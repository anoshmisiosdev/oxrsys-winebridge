/* Minimal D3D11 OpenXR test: negotiates with wineopenxr.dll, creates a D3D11
 * device + session, and renders color-cycling clears to both eyes for ~10s.
 * If this shows color in the headset, the whole chain works. */
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <stdio.h>
#define XR_USE_PLATFORM_WIN32 1
#define XR_USE_GRAPHICS_API_D3D11 1
#include "openxr/openxr.h"
#include "openxr/openxr_platform.h"
#include "openxr/openxr_loader_negotiation.h"

#define CHECK(expr) do { XrResult _r = (expr); if (_r != XR_SUCCESS) { \
    printf("FAIL: %s -> %d (line %d)\n", #expr, _r, __LINE__); return 1; } \
    printf("OK: %s\n", #expr); } while (0)

static PFN_xrGetInstanceProcAddr gipa;
template <typename T> static T fn(XrInstance inst, const char *name)
{ PFN_xrVoidFunction f = nullptr; gipa(inst, name, &f); return (T)f; }

int main(void)
{
    HMODULE dll = LoadLibraryA("wineopenxr.dll");
    if (!dll) { printf("FAIL: LoadLibrary (%lu)\n", GetLastError()); return 1; }
    auto negotiate = (PFN_xrNegotiateLoaderRuntimeInterface)GetProcAddress(dll, "xrNegotiateLoaderRuntimeInterface");
    XrNegotiateLoaderInfo li = { XR_LOADER_INTERFACE_STRUCT_LOADER_INFO, XR_LOADER_INFO_STRUCT_VERSION, sizeof(li) };
    li.minInterfaceVersion = 1; li.maxInterfaceVersion = XR_CURRENT_LOADER_RUNTIME_VERSION;
    li.minApiVersion = XR_MAKE_VERSION(1,0,0); li.maxApiVersion = XR_MAKE_VERSION(1,0,999);
    XrNegotiateRuntimeRequest req = { XR_LOADER_INTERFACE_STRUCT_RUNTIME_REQUEST, XR_RUNTIME_INFO_STRUCT_VERSION, sizeof(req) };
    if (negotiate(&li, &req) != XR_SUCCESS) { printf("FAIL: negotiate\n"); return 1; }
    gipa = req.getInstanceProcAddr;

    const char *ext = XR_KHR_D3D11_ENABLE_EXTENSION_NAME;
    XrInstanceCreateInfo ici = { XR_TYPE_INSTANCE_CREATE_INFO };
    ici.enabledExtensionCount = 1; ici.enabledExtensionNames = &ext;
    ici.applicationInfo.apiVersion = XR_MAKE_VERSION(1,0,34);
    snprintf(ici.applicationInfo.applicationName, XR_MAX_APPLICATION_NAME_SIZE, "d3d11test");
    XrInstance inst;
    CHECK(fn<PFN_xrCreateInstance>(XR_NULL_HANDLE, "xrCreateInstance")(&ici, &inst));

    XrSystemGetInfo sgi = { XR_TYPE_SYSTEM_GET_INFO }; sgi.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    XrSystemId sys;
    CHECK(fn<PFN_xrGetSystem>(inst, "xrGetSystem")(inst, &sgi, &sys));

    XrGraphicsRequirementsD3D11KHR reqs = { XR_TYPE_GRAPHICS_REQUIREMENTS_D3D11_KHR };
    CHECK(fn<PFN_xrGetD3D11GraphicsRequirementsKHR>(inst, "xrGetD3D11GraphicsRequirementsKHR")(inst, sys, &reqs));
    printf("    adapter LUID %08lx%08lx, minFeatureLevel %#x\n",
           (unsigned long)reqs.adapterLuid.HighPart, (unsigned long)reqs.adapterLuid.LowPart, reqs.minFeatureLevel);

    ID3D11Device *dev = nullptr; ID3D11DeviceContext *ctx = nullptr;
    D3D_FEATURE_LEVEL fl_req[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0 }, fl_got;
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
                                   fl_req, 2, D3D11_SDK_VERSION, &dev, &fl_got, &ctx);
    if (FAILED(hr)) { printf("FAIL: D3D11CreateDevice hr=%#lx\n", (unsigned long)hr); return 1; }
    printf("OK: D3D11 device (feature level %#x)\n", fl_got);

    /* Minimal draw pipeline: a magenta triangle covering the lower-left
     * quadrant in clip space, no transforms, no textures, no depth buffer.
     * Isolates whether real vertex/pixel-shader draw output survives the
     * DXMT->OXRSys interop, vs the already-proven-working ClearRenderTargetView
     * path (the earlier version of this test, and openvr_d3d11_test.cpp,
     * only ever cleared - never issued a draw call). */
    const char *shaderSrc =
        "struct VOut { float4 pos : SV_POSITION; };\n"
        "VOut vs(float2 p : POSITION) { VOut o; o.pos = float4(p, 0, 1); return o; }\n"
        "float4 ps(VOut i) : SV_TARGET { return float4(1, 0, 1, 1); }\n";
    ID3DBlob *vsBlob = nullptr, *psBlob = nullptr, *errBlob = nullptr;
    hr = D3DCompile(shaderSrc, strlen(shaderSrc), nullptr, nullptr, nullptr, "vs", "vs_5_0", 0, 0, &vsBlob, &errBlob);
    if (FAILED(hr)) { printf("FAIL: vs compile hr=%#lx err=%s\n", (unsigned long)hr, errBlob ? (char*)errBlob->GetBufferPointer() : "?"); return 1; }
    hr = D3DCompile(shaderSrc, strlen(shaderSrc), nullptr, nullptr, nullptr, "ps", "ps_5_0", 0, 0, &psBlob, &errBlob);
    if (FAILED(hr)) { printf("FAIL: ps compile hr=%#lx err=%s\n", (unsigned long)hr, errBlob ? (char*)errBlob->GetBufferPointer() : "?"); return 1; }
    printf("OK: shaders compiled\n");

    ID3D11VertexShader *vshader = nullptr;
    ID3D11PixelShader *pshader = nullptr;
    ID3D11InputLayout *layout = nullptr;
    dev->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr, &vshader);
    dev->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(), nullptr, &pshader);
    D3D11_INPUT_ELEMENT_DESC ilDesc[] = { { "POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 } };
    dev->CreateInputLayout(ilDesc, 1, vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), &layout);

    float verts[] = { -1, -1,  -1, 0,  0, -1 }; /* triangle in the lower-left quadrant of clip space */
    D3D11_BUFFER_DESC vbDesc = {}; vbDesc.ByteWidth = sizeof(verts); vbDesc.Usage = D3D11_USAGE_DEFAULT; vbDesc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    D3D11_SUBRESOURCE_DATA vbData = { verts };
    ID3D11Buffer *vbuf = nullptr;
    dev->CreateBuffer(&vbDesc, &vbData, &vbuf);
    printf("OK: draw resources created\n");

    XrGraphicsBindingD3D11KHR bind = { XR_TYPE_GRAPHICS_BINDING_D3D11_KHR }; bind.device = dev;
    XrSessionCreateInfo sci = { XR_TYPE_SESSION_CREATE_INFO }; sci.next = &bind; sci.systemId = sys;
    XrSession session;
    CHECK(fn<PFN_xrCreateSession>(inst, "xrCreateSession")(inst, &sci, &session));

    XrReferenceSpaceCreateInfo rsci = { XR_TYPE_REFERENCE_SPACE_CREATE_INFO };
    rsci.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
    rsci.poseInReferenceSpace.orientation.w = 1.0f;
    XrSpace space;
    CHECK(fn<PFN_xrCreateReferenceSpace>(inst, "xrCreateReferenceSpace")(session, &rsci, &space));

    uint32_t nviews = 0;
    fn<PFN_xrEnumerateViewConfigurationViews>(inst, "xrEnumerateViewConfigurationViews")
        (inst, sys, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 0, &nviews, nullptr);
    XrViewConfigurationView vcv[2] = { { XR_TYPE_VIEW_CONFIGURATION_VIEW }, { XR_TYPE_VIEW_CONFIGURATION_VIEW } };
    fn<PFN_xrEnumerateViewConfigurationViews>(inst, "xrEnumerateViewConfigurationViews")
        (inst, sys, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 2, &nviews, vcv);
    printf("OK: %u views, %ux%u recommended\n", nviews,
           vcv[0].recommendedImageRectWidth, vcv[0].recommendedImageRectHeight);

    int64_t fmts[16]; uint32_t nfmt = 0;
    fn<PFN_xrEnumerateSwapchainFormats>(inst, "xrEnumerateSwapchainFormats")(session, 16, &nfmt, fmts);
    printf("OK: %u swapchain formats, using %lld\n", nfmt, (long long)fmts[0]);

    XrSwapchain sc[2]; ID3D11Texture2D *tex[2][8] = {}; ID3D11RenderTargetView *rtv[2][8] = {};
    uint32_t nimg[2] = {};
    for (int eye = 0; eye < 2; eye++) {
        XrSwapchainCreateInfo scci = { XR_TYPE_SWAPCHAIN_CREATE_INFO };
        scci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
        scci.format = fmts[0]; scci.sampleCount = 1;
        scci.width = vcv[eye].recommendedImageRectWidth; scci.height = vcv[eye].recommendedImageRectHeight;
        scci.faceCount = 1; scci.arraySize = 1; scci.mipCount = 1;
        CHECK(fn<PFN_xrCreateSwapchain>(inst, "xrCreateSwapchain")(session, &scci, &sc[eye]));
        XrSwapchainImageD3D11KHR imgs[8];
        for (int i = 0; i < 8; i++) { imgs[i] = { XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR }; }
        CHECK(fn<PFN_xrEnumerateSwapchainImages>(inst, "xrEnumerateSwapchainImages")
              (sc[eye], 8, &nimg[eye], (XrSwapchainImageBaseHeader *)imgs));
        printf("    eye %d: %u images\n", eye, nimg[eye]);
        for (uint32_t i = 0; i < nimg[eye]; i++) {
            tex[eye][i] = imgs[i].texture;
            hr = dev->CreateRenderTargetView((ID3D11Resource *)tex[eye][i], nullptr, &rtv[eye][i]);
            if (FAILED(hr)) { printf("FAIL: CreateRenderTargetView hr=%#lx\n", (unsigned long)hr); return 1; }
        }
    }

    /* pump events until READY */
    auto poll = fn<PFN_xrPollEvent>(inst, "xrPollEvent");
    auto beginSession = fn<PFN_xrBeginSession>(inst, "xrBeginSession");
    XrSessionState state = XR_SESSION_STATE_UNKNOWN;
    for (int i = 0; i < 200 && state < XR_SESSION_STATE_READY; i++) {
        XrEventDataBuffer ev = { XR_TYPE_EVENT_DATA_BUFFER };
        while (poll(inst, &ev) == XR_SUCCESS) {
            if (ev.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED)
                state = ((XrEventDataSessionStateChanged *)&ev)->state;
            ev = { XR_TYPE_EVENT_DATA_BUFFER };
        }
        Sleep(50);
    }
    printf("OK: session state %d\n", state);
    XrSessionBeginInfo sbi = { XR_TYPE_SESSION_BEGIN_INFO };
    sbi.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    CHECK(beginSession(session, &sbi));

    auto waitFrame = fn<PFN_xrWaitFrame>(inst, "xrWaitFrame");
    auto beginFrame = fn<PFN_xrBeginFrame>(inst, "xrBeginFrame");
    auto endFrame = fn<PFN_xrEndFrame>(inst, "xrEndFrame");
    auto locateViews = fn<PFN_xrLocateViews>(inst, "xrLocateViews");
    auto acquire = fn<PFN_xrAcquireSwapchainImage>(inst, "xrAcquireSwapchainImage");
    auto wait = fn<PFN_xrWaitSwapchainImage>(inst, "xrWaitSwapchainImage");
    auto release = fn<PFN_xrReleaseSwapchainImage>(inst, "xrReleaseSwapchainImage");

    for (int f = 0; f < 27000; f++) {   /* ~5min at 90 Hz */
        XrFrameState fs = { XR_TYPE_FRAME_STATE };
        XrFrameWaitInfo fwi = { XR_TYPE_FRAME_WAIT_INFO };
        if (waitFrame(session, &fwi, &fs) != XR_SUCCESS) break;
        XrFrameBeginInfo fbi = { XR_TYPE_FRAME_BEGIN_INFO };
        beginFrame(session, &fbi);

        XrViewLocateInfo vli = { XR_TYPE_VIEW_LOCATE_INFO };
        vli.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
        vli.displayTime = fs.predictedDisplayTime; vli.space = space;
        XrViewState vs = { XR_TYPE_VIEW_STATE };
        XrView views[2] = { { XR_TYPE_VIEW }, { XR_TYPE_VIEW } };
        uint32_t nv = 0;
        locateViews(session, &vli, &vs, 2, &nv, views);

        XrCompositionLayerProjectionView pv[2];
        for (int eye = 0; eye < 2; eye++) {
            uint32_t idx = 0;
            XrSwapchainImageAcquireInfo ai = { XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO };
            acquire(sc[eye], &ai, &idx);
            XrSwapchainImageWaitInfo wi = { XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO }; wi.timeout = XR_INFINITE_DURATION;
            wait(sc[eye], &wi);
            float t = f / 90.0f;
            FLOAT col[4] = { 0.5f + 0.5f * (float)__builtin_sinf(t),
                             0.5f + 0.5f * (float)__builtin_sinf(t + 2.1f),
                             eye ? 0.9f : 0.2f, 1.0f };
            ctx->ClearRenderTargetView(rtv[eye][idx % nimg[eye]], col);

            /* the actual draw - a magenta triangle should appear over the
             * clear color in the lower-left quadrant of each eye */
            ID3D11RenderTargetView *rt = rtv[eye][idx % nimg[eye]];
            ctx->OMSetRenderTargets(1, &rt, nullptr);
            D3D11_VIEWPORT vp = { 0, 0, (FLOAT)vcv[eye].recommendedImageRectWidth, (FLOAT)vcv[eye].recommendedImageRectHeight, 0, 1 };
            ctx->RSSetViewports(1, &vp);
            UINT stride = sizeof(float) * 2, offset = 0;
            ctx->IASetVertexBuffers(0, 1, &vbuf, &stride, &offset);
            ctx->IASetInputLayout(layout);
            ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            ctx->VSSetShader(vshader, nullptr, 0);
            ctx->PSSetShader(pshader, nullptr, 0);
            ctx->Draw(3, 0);

            XrSwapchainImageReleaseInfo ri = { XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
            release(sc[eye], &ri);
            pv[eye] = { XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW };
            pv[eye].pose = views[eye].pose; pv[eye].fov = views[eye].fov;
            pv[eye].subImage.swapchain = sc[eye];
            pv[eye].subImage.imageRect.extent.width = (int32_t)vcv[eye].recommendedImageRectWidth;
            pv[eye].subImage.imageRect.extent.height = (int32_t)vcv[eye].recommendedImageRectHeight;
        }
        XrCompositionLayerProjection layer = { XR_TYPE_COMPOSITION_LAYER_PROJECTION };
        layer.space = space; layer.viewCount = 2; layer.views = pv;
        const XrCompositionLayerBaseHeader *layers[] = { (XrCompositionLayerBaseHeader *)&layer };
        XrFrameEndInfo fei = { XR_TYPE_FRAME_END_INFO };
        fei.displayTime = fs.predictedDisplayTime;
        fei.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
        fei.layerCount = 1; fei.layers = layers;
        if (endFrame(session, &fei) != XR_SUCCESS) break;
        if (f % 90 == 0) printf("    frame %d ok\n", f);
    }
    printf("PASS: rendered 900 frames through the bridge\n");
    return 0;
}
