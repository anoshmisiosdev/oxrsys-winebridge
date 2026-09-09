/* Minimal D3D11 OpenXR test: negotiates with wineopenxr.dll, creates a D3D11
 * device + session, and renders to both eyes over the bridge.
 *
 * This variant adds everything d3d11_depth_test.cpp still didn't exercise,
 * closely mirroring OpenXRSamples' SingleFileExample/main.cpp app_draw():
 *   - a real indexed cube mesh (position+normal, DrawIndexed not Draw)
 *   - a constant buffer updated via UpdateSubresource every draw
 *   - real view/projection matrices built from the XR-reported pose/fov,
 *     via DirectXMath (XMMatrixPerspectiveOffCenterRH, XMMatrixInverse,
 *     XMMatrixAffineTransformation, XMMatrixTranspose - same calls main.cpp
 *     uses)
 *   - clear color {0,0,0,1} (black, matching the sample) instead of the
 *     color-cycling clear the earlier tests used, so a correctly-working
 *     run should show a black background with a lit rotating-ish cube,
 *     not a colored square.
 * If the cube renders correctly here, the grey-screen bug is not in the
 * rendering content itself and must be in the sample's setup/event-loop
 * structure instead. */
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <stdio.h>
#include <DirectXMath.h>
using namespace DirectX;
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

struct XMFLOAT4X4_buf { XMFLOAT4X4 world; XMFLOAT4X4 viewproj; };

/* 8-vertex cube (position + normal), matching the shape of app_verts in the
 * real sample closely enough (position/normal input layout, R32G32B32_FLOAT
 * x2, 6 floats/vertex stride) - exact geometry doesn't matter, only that the
 * pipeline shape matches. */
static float cubeVerts[] = {
    /* pos.xyz          normal.xyz */
    -1,-1,-1,  0, 0,-1,   1,-1,-1,  0, 0,-1,   1, 1,-1,  0, 0,-1,  -1, 1,-1,  0, 0,-1,
    -1,-1, 1,  0, 0, 1,   1,-1, 1,  0, 0, 1,   1, 1, 1,  0, 0, 1,  -1, 1, 1,  0, 0, 1,
};
static uint16_t cubeInds[] = {
    0,1,2, 0,2,3,       /* back */
    4,6,5, 4,7,6,       /* front */
    0,4,5, 0,5,1,       /* bottom */
    3,2,6, 3,6,7,       /* top */
    0,3,7, 0,7,4,       /* left */
    1,5,6, 1,6,2,       /* right */
};

static const char *shaderSrc =
    "cbuffer TransformBuffer : register(b0) {\n"
    "    float4x4 world;\n"
    "    float4x4 viewproj;\n"
    "};\n"
    "struct VOut { float4 pos : SV_POSITION; float3 normal : NORMAL; };\n"
    "VOut vs(float3 p : POSITION, float3 n : NORMAL) {\n"
    "    VOut o;\n"
    "    float4 wp = mul(float4(p, 1), world);\n"
    "    o.pos = mul(wp, viewproj);\n"
    "    o.normal = mul(float4(n, 0), world).xyz;\n"
    "    return o;\n"
    "}\n"
    "float4 ps(VOut i) : SV_TARGET {\n"
    "    float light = saturate(dot(normalize(i.normal), normalize(float3(0.3, 1, 0.5)))) * 0.7 + 0.3;\n"
    "    return float4(1, 0, 1, 1) * light;\n"
    "}\n";

static XMMATRIX xr_projection(XrFovf fov, float clip_near, float clip_far)
{
    const float left = clip_near * tanf(fov.angleLeft);
    const float right = clip_near * tanf(fov.angleRight);
    const float down = clip_near * tanf(fov.angleDown);
    const float up = clip_near * tanf(fov.angleUp);
    return XMMatrixPerspectiveOffCenterRH(left, right, down, up, clip_near, clip_far);
}

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

    ID3D11Device *dev = nullptr; ID3D11DeviceContext *ctx = nullptr;
    D3D_FEATURE_LEVEL fl_req[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0 }, fl_got;
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
                                   fl_req, 2, D3D11_SDK_VERSION, &dev, &fl_got, &ctx);
    if (FAILED(hr)) { printf("FAIL: D3D11CreateDevice hr=%#lx\n", (unsigned long)hr); return 1; }
    printf("OK: D3D11 device (feature level %#x)\n", fl_got);

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
    D3D11_INPUT_ELEMENT_DESC ilDesc[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0 },
    };
    hr = dev->CreateInputLayout(ilDesc, 2, vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), &layout);
    if (FAILED(hr)) { printf("FAIL: CreateInputLayout hr=%#lx\n", (unsigned long)hr); return 1; }

    D3D11_BUFFER_DESC vbDesc = {}; vbDesc.ByteWidth = sizeof(cubeVerts); vbDesc.Usage = D3D11_USAGE_DEFAULT; vbDesc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    D3D11_SUBRESOURCE_DATA vbData = { cubeVerts };
    ID3D11Buffer *vbuf = nullptr;
    hr = dev->CreateBuffer(&vbDesc, &vbData, &vbuf);
    if (FAILED(hr)) { printf("FAIL: CreateBuffer(vb) hr=%#lx\n", (unsigned long)hr); return 1; }

    D3D11_BUFFER_DESC ibDesc = {}; ibDesc.ByteWidth = sizeof(cubeInds); ibDesc.Usage = D3D11_USAGE_DEFAULT; ibDesc.BindFlags = D3D11_BIND_INDEX_BUFFER;
    D3D11_SUBRESOURCE_DATA ibData = { cubeInds };
    ID3D11Buffer *ibuf = nullptr;
    hr = dev->CreateBuffer(&ibDesc, &ibData, &ibuf);
    if (FAILED(hr)) { printf("FAIL: CreateBuffer(ib) hr=%#lx\n", (unsigned long)hr); return 1; }

    D3D11_BUFFER_DESC cbDesc = {}; cbDesc.ByteWidth = sizeof(XMFLOAT4X4_buf); cbDesc.Usage = D3D11_USAGE_DEFAULT; cbDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    ID3D11Buffer *cbuf = nullptr;
    hr = dev->CreateBuffer(&cbDesc, nullptr, &cbuf);
    if (FAILED(hr)) { printf("FAIL: CreateBuffer(cb) hr=%#lx\n", (unsigned long)hr); return 1; }

    ID3D11DepthStencilState *dss = nullptr;
    D3D11_DEPTH_STENCIL_DESC dssDesc = {};
    dssDesc.DepthEnable = TRUE;
    dssDesc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
    dssDesc.DepthFunc = D3D11_COMPARISON_LESS_EQUAL;
    dev->CreateDepthStencilState(&dssDesc, &dss);
    printf("OK: draw resources created (cube + index buffer + constant buffer)\n");

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
    ID3D11Texture2D *depthTex[2] = {}; ID3D11DepthStencilView *dsv[2] = {};
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

        D3D11_TEXTURE2D_DESC dtDesc = {};
        dtDesc.Width = vcv[eye].recommendedImageRectWidth;
        dtDesc.Height = vcv[eye].recommendedImageRectHeight;
        dtDesc.MipLevels = 1; dtDesc.ArraySize = 1;
        dtDesc.Format = DXGI_FORMAT_D32_FLOAT; /* matches the real sample exactly */
        dtDesc.SampleDesc.Count = 1;
        dtDesc.Usage = D3D11_USAGE_DEFAULT;
        dtDesc.BindFlags = D3D11_BIND_DEPTH_STENCIL;
        hr = dev->CreateTexture2D(&dtDesc, nullptr, &depthTex[eye]);
        if (FAILED(hr)) { printf("FAIL: CreateTexture2D(depth) hr=%#lx\n", (unsigned long)hr); return 1; }
        D3D11_DEPTH_STENCIL_VIEW_DESC stencilDesc = {};
        stencilDesc.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
        stencilDesc.Format = DXGI_FORMAT_D32_FLOAT;
        hr = dev->CreateDepthStencilView(depthTex[eye], &stencilDesc, &dsv[eye]);
        if (FAILED(hr)) { printf("FAIL: CreateDepthStencilView hr=%#lx\n", (unsigned long)hr); return 1; }
        printf("    eye %d: depth buffer created (D32_FLOAT)\n", eye);
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

            float clear[4] = { 0, 0, 0, 1 }; /* black, matching the real sample exactly */
            ID3D11RenderTargetView *rt = rtv[eye][idx % nimg[eye]];
            ctx->ClearRenderTargetView(rt, clear);
            ctx->ClearDepthStencilView(dsv[eye], D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 1.0f, 0);
            ctx->OMSetRenderTargets(1, &rt, dsv[eye]);
            ctx->OMSetDepthStencilState(dss, 0);

            XrRect2Di rect = {}; rect.extent.width = (int32_t)vcv[eye].recommendedImageRectWidth;
            rect.extent.height = (int32_t)vcv[eye].recommendedImageRectHeight;
            D3D11_VIEWPORT vp = { (FLOAT)rect.offset.x, (FLOAT)rect.offset.y,
                                   (FLOAT)rect.extent.width, (FLOAT)rect.extent.height, 0, 1 };
            ctx->RSSetViewports(1, &vp);

            /* real view/projection matrices from the XR-reported pose+fov,
             * same construction as d3d_xr_projection()/app_draw() in main.cpp */
            XMMATRIX mat_projection = xr_projection(views[eye].fov, 0.05f, 100.0f);
            XMMATRIX mat_view = XMMatrixInverse(nullptr, XMMatrixAffineTransformation(
                g_XMOne, g_XMZero,
                XMLoadFloat4((XMFLOAT4*)&views[eye].pose.orientation),
                XMLoadFloat3((XMFLOAT3*)&views[eye].pose.position)));

            /* cube 1.5m in front of the origin, at 1/3 scale - should be
             * squarely in view for a forward-looking headset */
            XMVECTOR cubePos = XMVectorSet(0, 0, -1.5f, 0);
            XMMATRIX mat_model = XMMatrixAffineTransformation(
                XMVectorReplicate(0.3f), g_XMZero, XMQuaternionIdentity(), cubePos);

            XMFLOAT4X4_buf cb;
            XMStoreFloat4x4(&cb.world, XMMatrixTranspose(mat_model));
            XMStoreFloat4x4(&cb.viewproj, XMMatrixTranspose(mat_view * mat_projection));
            ctx->UpdateSubresource(cbuf, 0, nullptr, &cb, 0, 0);
            ctx->VSSetConstantBuffers(0, 1, &cbuf);

            UINT stride = sizeof(float) * 6, offset = 0;
            ctx->IASetVertexBuffers(0, 1, &vbuf, &stride, &offset);
            ctx->IASetIndexBuffer(ibuf, DXGI_FORMAT_R16_UINT, 0);
            ctx->IASetInputLayout(layout);
            ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            ctx->VSSetShader(vshader, nullptr, 0);
            ctx->PSSetShader(pshader, nullptr, 0);
            ctx->DrawIndexed((UINT)(sizeof(cubeInds) / sizeof(cubeInds[0])), 0, 0);

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
    printf("PASS: rendered through the bridge\n");
    return 0;
}
