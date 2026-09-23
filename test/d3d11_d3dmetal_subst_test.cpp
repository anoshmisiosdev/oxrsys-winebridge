/* PROTOTYPE feasibility test: D3D11 on CrossOver's D3DMetal rendering straight
 * into the OXRSys runtime's swapchain MTLTextures (bridge dmsubst path).
 *
 *   probe  - step 1: with the bridge's Metal swizzles installed, arm a thread
 *            in PROBE mode around a few ID3D11Device::CreateTexture2D calls and
 *            report which Metal selector D3DMetal used, on which thread.
 *   xr     - step 2: full OpenXR session on the D3DMetal device. The bridge
 *            substitutes the runtime's swapchain textures into D3DMetal's
 *            CreateTexture2D; we clear each image to a known colour through
 *            D3D11, release (bridge CPU-syncs), then read the texel back on the
 *            Metal side from the runtime's own MTLTexture.
 *
 *   fence  - GPU-sync probe: capture/substitute the Metal event behind an
 *            ID3D11Fence and watch ID3D11DeviceContext4::Signal reach it.
 *   both   - probe + fence + xr (default).
 *   vprt   - layered rendering into a 2-slice Texture2DArray (VS/GS-written
 *            SV_RenderTargetArrayIndex, per-slice RTVs, Unity-like MSAA+depth
 *            variants) on (a) a plain D3DMetal texture and (b) a substituted
 *            arraySize=2 swapchain image; logs the D3D11 OPTIONS* caps.
 *            DMS_TEST_NO_XR=1 skips (b). caps - log the caps only.
 *
 * xr env knobs: DMS_TEST_FRAMES, DMS_TEST_FORMAT (DXGI), DMS_TEST_ARRAY=2,
 * DMS_TEST_EXTRA=1 (UpdateSubresource/CopySubresourceRegion into an image),
 * DMS_TEST_SYNCCHECK=1|2 (+DMS_TEST_HEAVY) immediate post-release readback with
 * (1) / without (2, control) a GPU wait on the bridge's release event.
 * Bridge knobs: OXR_DMSUBST_TRACE=1|2, OXR_DMSUBST_SYNC=cpu,
 * OXR_BRIDGE_D3D11_BACKEND=dxmt|d3dmetal, OXR_DMSUBST_DUMP_AT=N,
 * OXR_DMSUBST_CONTROL=1. Run with test/dmsubst/run-in-bottle.sh.
 *
 * Build: x86_64-w64-mingw32-g++ -O1 -static -I../bridge/extern/OpenXR-SDK/include \
 *          d3d11_d3dmetal_subst_test.cpp -ld3d11 -ldxgi -ld3dcompiler -o d3d11_d3dmetal_subst_test.exe
 * Run (throwaway D3DMetal bottle):  wine d3d11_d3dmetal_subst_test.exe [probe|xr|both] */
#include <windows.h>
#include <d3d11_4.h>
#include <d3dcompiler.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#define XR_USE_PLATFORM_WIN32 1
#define XR_USE_GRAPHICS_API_D3D11 1
#include "openxr/openxr.h"
#include "openxr/openxr_platform.h"
#include "openxr/openxr_loader_negotiation.h"
#include "../bridge/src/include/dmsubst.h"

typedef int (WINAPI *PFN_proto_dmsubst)(struct dmsubst_params *);
typedef uint64_t (WINAPI *PFN_proto_sc_tex)(XrSwapchain, uint32_t);
static PFN_proto_dmsubst dms;
static PFN_proto_sc_tex sc_tex;
typedef uint64_t (WINAPI *PFN_proto_sess_ev)(XrSession, uint64_t *);
static PFN_proto_sess_ev sess_ev;

static void gpu_idle(ID3D11Device *dev, ID3D11DeviceContext *ctx)
{
    ID3D11Query *q = nullptr; D3D11_QUERY_DESC qd = { D3D11_QUERY_EVENT, 0 };
    dev->CreateQuery(&qd, &q); ctx->End(q); ctx->Flush();
    BOOL done = FALSE; DWORD t1 = GetTickCount();
    while (ctx->GetData(q, &done, sizeof(done), 0) != S_OK && GetTickCount() - t1 < 2000) Sleep(1);
    q->Release();
}

static PFN_xrGetInstanceProcAddr gipa;
template <typename T> static T fn(XrInstance inst, const char *name)
{ PFN_xrVoidFunction f = nullptr; gipa(inst, name, &f); return (T)f; }

#define CHECK(expr) do { XrResult _r = (expr); if (_r != XR_SUCCESS) { \
    printf("FAIL: %s -> %d (line %d)\n", #expr, _r, __LINE__); fflush(stdout); return 1; } \
    printf("OK: %s\n", #expr); fflush(stdout); } while (0)

static void stats(const char *when)
{
    struct dmsubst_params p = {}; p.op = DMSUBST_OP_STATS; dms(&p);
    printf("STATS[%s]: detect=0x%x textures=%llu heaps=%llu mtl4_queues=%llu classic_queues=%llu "
           "mtl4_commits=%llu classic_cmdbufs=%llu resset_adds=%llu resset_adds_of_substituted=%llu heap_queries_of_substituted=%llu\n",
           when, p.detect_flags, (unsigned long long)p.n_textures, (unsigned long long)p.n_heaps,
           (unsigned long long)p.n_mtl4_queues, (unsigned long long)p.n_classic_queues,
           (unsigned long long)p.n_mtl4_commits, (unsigned long long)p.n_classic_cbs, (unsigned long long)p.n_resset_add,
           (unsigned long long)p.n_resset_add_subst, (unsigned long long)p.n_heap_queries_subst);
    fflush(stdout);
}

struct probe_case { const char *name; DXGI_FORMAT fmt; UINT bind; UINT arr; UINT mips; bool init; UINT misc; };

static int run_probe(ID3D11Device *dev)
{
    struct dmsubst_params t = {}; t.op = DMSUBST_OP_THREAD; dms(&t);
    printf("PROBE: this thread: win32 tid %lu, unix pthread id %llu\n",
           GetCurrentThreadId(), (unsigned long long)t.thread_id);
    static const probe_case cases[] = {
        { "RGBA8_UNORM RT|SRV",         DXGI_FORMAT_R8G8B8A8_UNORM,      D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE, 1, 1, false, 0 },
        { "RGBA8_TYPELESS RT|SRV",      DXGI_FORMAT_R8G8B8A8_TYPELESS,   D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE, 1, 1, false, 0 },
        { "RGBA8_UNORM_SRGB RT|SRV",    DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE, 1, 1, false, 0 },
        { "BGRA8_TYPELESS RT|SRV",      DXGI_FORMAT_B8G8R8A8_TYPELESS,   D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE, 1, 1, false, 0 },
        { "RGBA16F RT|SRV",             DXGI_FORMAT_R16G16B16A16_FLOAT,  D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE, 1, 1, false, 0 },
        { "RGBA8 RT|SRV array2",        DXGI_FORMAT_R8G8B8A8_TYPELESS,   D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE, 2, 1, false, 0 },
        { "RGBA8 SRV w/ init data",     DXGI_FORMAT_R8G8B8A8_UNORM,      D3D11_BIND_SHADER_RESOURCE, 1, 1, true, 0 },
        { "RGBA8 RT|UAV",               DXGI_FORMAT_R8G8B8A8_UNORM,      D3D11_BIND_RENDER_TARGET | D3D11_BIND_UNORDERED_ACCESS, 1, 1, false, 0 },
        { "D32_FLOAT DS",               DXGI_FORMAT_D32_FLOAT,           D3D11_BIND_DEPTH_STENCIL, 1, 1, false, 0 },
        { "R32_TYPELESS DS|SRV",        DXGI_FORMAT_R32_TYPELESS,        D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE, 1, 1, false, 0 },
        { "D24S8 DS",                   DXGI_FORMAT_D24_UNORM_S8_UINT,   D3D11_BIND_DEPTH_STENCIL, 1, 1, false, 0 },
        { "RGBA8 RT mips4",             DXGI_FORMAT_R8G8B8A8_UNORM,      D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE, 1, 4, false, D3D11_RESOURCE_MISC_GENERATE_MIPS },
    };
    static uint32_t pixels[64 * 64];
    for (const probe_case &c : cases) {
        D3D11_TEXTURE2D_DESC d = {};
        d.Width = 64; d.Height = 64; d.MipLevels = c.mips; d.ArraySize = c.arr; d.Format = c.fmt;
        d.SampleDesc.Count = 1; d.Usage = D3D11_USAGE_DEFAULT; d.BindFlags = c.bind; d.MiscFlags = c.misc;
        D3D11_SUBRESOURCE_DATA init[2] = {};
        for (int i = 0; i < 2; i++) { init[i].pSysMem = pixels; init[i].SysMemPitch = 64 * 4; }
        struct dmsubst_params p = {}; p.op = DMSUBST_OP_ARM; p.flags = DMSUBST_ARM_PROBE; dms(&p);
        ID3D11Texture2D *tex = nullptr;
        HRESULT hr = dev->CreateTexture2D(&d, c.init ? init : nullptr, &tex);
        p = {}; p.op = DMSUBST_OP_DISARM; dms(&p);
        printf("PROBE %-26s hr=0x%08lx same-thread creations=%u any-thread=%u via \"%s\" -> MTL fmt=%u type=%u %ux%u arr=%u mips=%u usage=0x%llx storage=%u\n",
               c.name, (unsigned long)hr, p.seen, p.global_seen, p.where, p.desc_pixel_format,
               p.desc_texture_type, p.desc_width, p.desc_height, p.desc_array_length, p.desc_mips,
               (unsigned long long)p.desc_usage, p.desc_storage_mode);
        fflush(stdout);
        if (tex) tex->Release();
    }
    stats("after probe");
    return 0;
}

/* GPU-sync investigation: does D3DMetal back an ID3D11Fence with an
 * MTLSharedEvent created synchronously in CreateFence, and does
 * ID3D11DeviceContext4::Signal signal that event on its queue? If so the bridge
 * could hand that event to the runtime's queue (encodeWaitForEvent) instead of
 * CPU-waiting at release */
static int run_fence_probe(ID3D11Device *dev, ID3D11DeviceContext *ctx)
{
    ID3D11Device5 *dev5 = nullptr; ID3D11DeviceContext4 *ctx4 = nullptr; ID3D11Fence *fence = nullptr;
    if (FAILED(dev->QueryInterface(__uuidof(ID3D11Device5), (void **)&dev5)) ||
        FAILED(ctx->QueryInterface(__uuidof(ID3D11DeviceContext4), (void **)&ctx4))) {
        printf("FENCE: no ID3D11Device5/ID3D11DeviceContext4\n"); return 0;
    }
    struct dmsubst_params p = {}; p.op = DMSUBST_OP_ARM; p.flags = DMSUBST_ARM_PROBE | DMSUBST_ARM_EVENT_SHARED; dms(&p);
    HRESULT hr = dev5->CreateFence(0, D3D11_FENCE_FLAG_NONE, __uuidof(ID3D11Fence), (void **)&fence);
    p = {}; p.op = DMSUBST_OP_DISARM; dms(&p);
    printf("FENCE: CreateFence hr=0x%08lx; events created on this thread while armed=%u via \"%s\" -> MTLSharedEvent 0x%llx\n",
           (unsigned long)hr, p.seen, p.where, (unsigned long long)p.event);
    if (FAILED(hr) || !p.event) return 0;
    uint64_t ev = p.event;
    for (uint64_t v = 1; v <= 3; v++) {
        float c[4] = { 0, 0, 0, 1 };
        ID3D11Texture2D *t = nullptr; ID3D11RenderTargetView *r = nullptr;
        D3D11_TEXTURE2D_DESC d = {}; d.Width = d.Height = 256; d.MipLevels = d.ArraySize = 1;
        d.Format = DXGI_FORMAT_R8G8B8A8_UNORM; d.SampleDesc.Count = 1; d.BindFlags = D3D11_BIND_RENDER_TARGET;
        dev->CreateTexture2D(&d, nullptr, &t); dev->CreateRenderTargetView(t, nullptr, &r);
        ctx->ClearRenderTargetView(r, c);
        hr = ctx4->Signal(fence, v);
        uint64_t before; { struct dmsubst_params q = {}; q.op = DMSUBST_OP_EVENT_VALUE; q.mtl_texture = ev; dms(&q); before = q.event_value; }
        ctx->Flush();
        DWORD t0 = GetTickCount(); uint64_t val = 0;
        do { struct dmsubst_params q = {}; q.op = DMSUBST_OP_EVENT_VALUE; q.mtl_texture = ev; dms(&q); val = q.event_value; }
        while (val < v && GetTickCount() - t0 < 1000);
        printf("FENCE: Signal(%llu) hr=0x%08lx; MTLSharedEvent.signaledValue before Flush=%llu, after=%llu (%lu ms); GetCompletedValue=%llu\n",
               (unsigned long long)v, (unsigned long)hr, (unsigned long long)before, (unsigned long long)val,
               (unsigned long)(GetTickCount() - t0), (unsigned long long)fence->GetCompletedValue());
        r->Release(); t->Release();
    }
    fence->Release(); ctx4->Release(); dev5->Release();
    return 0;
}

static int run_xr(HMODULE dll, ID3D11Device *dev, ID3D11DeviceContext *ctx, int frames)
{
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
    snprintf(ici.applicationInfo.applicationName, XR_MAX_APPLICATION_NAME_SIZE, "dmsubst-test");
    XrInstance inst;
    CHECK(fn<PFN_xrCreateInstance>(XR_NULL_HANDLE, "xrCreateInstance")(&ici, &inst));
    XrSystemGetInfo sgi = { XR_TYPE_SYSTEM_GET_INFO }; sgi.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    XrSystemId sys;
    CHECK(fn<PFN_xrGetSystem>(inst, "xrGetSystem")(inst, &sgi, &sys));
    XrGraphicsRequirementsD3D11KHR reqs = { XR_TYPE_GRAPHICS_REQUIREMENTS_D3D11_KHR };
    CHECK(fn<PFN_xrGetD3D11GraphicsRequirementsKHR>(inst, "xrGetD3D11GraphicsRequirementsKHR")(inst, sys, &reqs));

    XrGraphicsBindingD3D11KHR bind = { XR_TYPE_GRAPHICS_BINDING_D3D11_KHR }; bind.device = dev;
    XrSessionCreateInfo sci = { XR_TYPE_SESSION_CREATE_INFO }; sci.next = &bind; sci.systemId = sys;
    XrSession session;
    CHECK(fn<PFN_xrCreateSession>(inst, "xrCreateSession")(inst, &sci, &session));
    XrReferenceSpaceCreateInfo rsci = { XR_TYPE_REFERENCE_SPACE_CREATE_INFO };
    rsci.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL; rsci.poseInReferenceSpace.orientation.w = 1.0f;
    XrSpace space;
    CHECK(fn<PFN_xrCreateReferenceSpace>(inst, "xrCreateReferenceSpace")(session, &rsci, &space));

    XrViewConfigurationView vcv[2] = { { XR_TYPE_VIEW_CONFIGURATION_VIEW }, { XR_TYPE_VIEW_CONFIGURATION_VIEW } };
    uint32_t nviews = 0;
    fn<PFN_xrEnumerateViewConfigurationViews>(inst, "xrEnumerateViewConfigurationViews")
        (inst, sys, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 2, &nviews, vcv);
    int64_t fmts[32]; uint32_t nfmt = 0;
    fn<PFN_xrEnumerateSwapchainFormats>(inst, "xrEnumerateSwapchainFormats")(session, 32, &nfmt, fmts);
    int64_t fmt = fmts[0];
    const char *want = getenv("DMS_TEST_FORMAT"); /* e.g. 28 / 29 / 87 / 91 / 10 */
    for (uint32_t i = 0; i < nfmt; i++) {
        printf("    format[%u] = %lld\n", i, (long long)fmts[i]);
        if (want && fmts[i] == atoll(want)) fmt = fmts[i];
    }
    printf("OK: using swapchain format %lld, %ux%u\n", (long long)fmt,
           vcv[0].recommendedImageRectWidth, vcv[0].recommendedImageRectHeight);

    const uint32_t W = vcv[0].recommendedImageRectWidth, H = vcv[0].recommendedImageRectHeight;
    /* DMS_TEST_ARRAY=2: array swapchains (single-pass stereo style); each eye
     * renders to slice <eye> of its own swapchain */
    const uint32_t arr = getenv("DMS_TEST_ARRAY") ? (uint32_t)atoi(getenv("DMS_TEST_ARRAY")) : 1;
    XrSwapchain sc[2]; ID3D11RenderTargetView *rtv[2][8] = {}; uint32_t nimg[2] = {};
    for (int eye = 0; eye < 2; eye++) {
        XrSwapchainCreateInfo scci = { XR_TYPE_SWAPCHAIN_CREATE_INFO };
        scci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
        scci.format = fmt; scci.sampleCount = 1; scci.width = W; scci.height = H;
        scci.faceCount = 1; scci.arraySize = arr; scci.mipCount = 1;
        CHECK(fn<PFN_xrCreateSwapchain>(inst, "xrCreateSwapchain")(session, &scci, &sc[eye]));
        XrSwapchainImageD3D11KHR imgs[8];
        for (int i = 0; i < 8; i++) imgs[i] = { XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR };
        CHECK(fn<PFN_xrEnumerateSwapchainImages>(inst, "xrEnumerateSwapchainImages")
              (sc[eye], 8, &nimg[eye], (XrSwapchainImageBaseHeader *)imgs));
        for (uint32_t i = 0; i < nimg[eye]; i++) {
            D3D11_TEXTURE2D_DESC td; imgs[i].texture->GetDesc(&td);
            D3D11_RENDER_TARGET_VIEW_DESC rd = {};
            rd.Format = (DXGI_FORMAT)fmt; rd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
            if (arr > 1) {
                rd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2DARRAY;
                rd.Texture2DArray.FirstArraySlice = eye; rd.Texture2DArray.ArraySize = 1;
            }
            HRESULT hr = dev->CreateRenderTargetView(imgs[i].texture, &rd, &rtv[eye][i]);
            printf("    eye %d image %u: ID3D11Texture2D %p (desc fmt %d %ux%u) runtime MTLTexture 0x%llx, RTV hr=0x%08lx\n",
                   eye, i, (void *)imgs[i].texture, (int)td.Format, td.Width, td.Height,
                   (unsigned long long)sc_tex(sc[eye], i), (unsigned long)hr);
            if (FAILED(hr)) { printf("FAIL: CreateRenderTargetView\n"); return 1; }
        }
    }
    stats("after swapchain images");

    /* A scissored full-screen triangle drawn into the top-left quadrant after a
     * Flush, so it lands in a second render pass that must LOAD the clear */
    static const char *src =
        "float4 vs(uint id : SV_VertexID) : SV_POSITION {\n"
        "  float2 uv = float2((id << 1) & 2, id & 2);\n"
        "  return float4(uv * float2(2, -2) + float2(-1, 1), 0, 1); }\n"
        "float4 ps() : SV_TARGET { return float4(1, 1, 0, 50.0 / 255.0); }\n";
    ID3DBlob *vsb = nullptr, *psb = nullptr, *err = nullptr;
    ID3D11VertexShader *vsh = nullptr; ID3D11PixelShader *psh = nullptr; ID3D11RasterizerState *rs = nullptr;
    if (FAILED(D3DCompile(src, strlen(src), nullptr, nullptr, nullptr, "vs", "vs_5_0", 0, 0, &vsb, &err)) ||
        FAILED(D3DCompile(src, strlen(src), nullptr, nullptr, nullptr, "ps", "ps_5_0", 0, 0, &psb, &err))) {
        printf("FAIL: shader compile %s\n", err ? (char *)err->GetBufferPointer() : "?"); return 1;
    }
    dev->CreateVertexShader(vsb->GetBufferPointer(), vsb->GetBufferSize(), nullptr, &vsh);
    dev->CreatePixelShader(psb->GetBufferPointer(), psb->GetBufferSize(), nullptr, &psh);
    D3D11_RASTERIZER_DESC rsd = {}; rsd.FillMode = D3D11_FILL_SOLID; rsd.CullMode = D3D11_CULL_NONE;
    rsd.ScissorEnable = TRUE; rsd.DepthClipEnable = TRUE;
    dev->CreateRasterizerState(&rsd, &rs);
    const float drawn[4] = { 1, 1, 0, 50.0f / 255.0f };
    /* DMS_TEST_SYNCCHECK=1: before each clear, keep the GPU busy with many
     * full-screen triangles, and right after each eye-0 release read the image
     * back on ANOTHER Metal queue that GPU-waits on the bridge's release event
     * (as the runtime's queue does). DMS_TEST_SYNCCHECK=2 reads WITHOUT the wait
     * (control: shows the check can catch a missing sync) */
    int synccheck = getenv("DMS_TEST_SYNCCHECK") ? atoi(getenv("DMS_TEST_SYNCCHECK")) : 0;
    int heavy = getenv("DMS_TEST_HEAVY") ? atoi(getenv("DMS_TEST_HEAVY")) : 400;
    int sync_ok = 0, sync_bad = 0;
    D3D11_RASTERIZER_DESC rsd2 = rsd; rsd2.ScissorEnable = FALSE;
    ID3D11RasterizerState *rs_full = nullptr; dev->CreateRasterizerState(&rsd2, &rs_full);

    auto poll = fn<PFN_xrPollEvent>(inst, "xrPollEvent");
    XrSessionState state = XR_SESSION_STATE_UNKNOWN;
    for (int i = 0; i < 100 && state < XR_SESSION_STATE_READY; i++) {
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
    CHECK(fn<PFN_xrBeginSession>(inst, "xrBeginSession")(session, &sbi));

    auto waitFrame = fn<PFN_xrWaitFrame>(inst, "xrWaitFrame");
    auto beginFrame = fn<PFN_xrBeginFrame>(inst, "xrBeginFrame");
    auto endFrame = fn<PFN_xrEndFrame>(inst, "xrEndFrame");
    auto locateViews = fn<PFN_xrLocateViews>(inst, "xrLocateViews");
    auto acquire = fn<PFN_xrAcquireSwapchainImage>(inst, "xrAcquireSwapchainImage");
    auto wait = fn<PFN_xrWaitSwapchainImage>(inst, "xrWaitSwapchainImage");
    auto release = fn<PFN_xrReleaseSwapchainImage>(inst, "xrReleaseSwapchainImage");

    /* Per (eye, image) the colour it was last cleared to. Components are 0/1
     * except alpha, so sRGB encoding cannot change them; alpha encodes the
     * frame so a stale image is caught */
    float last[2][8][4] = {};
    int cleared[2][8] = {};
    DWORD t0 = GetTickCount();
    for (int f = 0; f < frames; f++) {
        XrFrameState fs = { XR_TYPE_FRAME_STATE };
        XrFrameWaitInfo fwi = { XR_TYPE_FRAME_WAIT_INFO };
        XrResult r = waitFrame(session, &fwi, &fs);
        if (r != XR_SUCCESS) { printf("FAIL: xrWaitFrame %d\n", r); return 1; }
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
            float c[4] = { (float)((f + eye) & 1), (float)(((f + eye) >> 1) & 1), (float)(eye ^ 1),
                           (float)((f % 16) * 17) / 255.0f };
            if (synccheck) {
                D3D11_VIEWPORT vpf = { 0, 0, (FLOAT)W, (FLOAT)H, 0, 1 };
                ctx->OMSetRenderTargets(1, &rtv[eye][idx], nullptr);
                ctx->RSSetViewports(1, &vpf); ctx->RSSetState(rs_full);
                ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
                ctx->IASetInputLayout(nullptr);
                ctx->VSSetShader(vsh, nullptr, 0); ctx->PSSetShader(psh, nullptr, 0);
                ctx->DrawInstanced(3, heavy, 0, 0);
            }
            ctx->ClearRenderTargetView(rtv[eye][idx], c);
            ctx->Flush();
            D3D11_VIEWPORT vp = { 0, 0, (FLOAT)W, (FLOAT)H, 0, 1 };
            D3D11_RECT sr = { 0, 0, (LONG)(W / 2), (LONG)(H / 2) };
            ctx->OMSetRenderTargets(1, &rtv[eye][idx], nullptr);
            ctx->RSSetViewports(1, &vp); ctx->RSSetScissorRects(1, &sr); ctx->RSSetState(rs);
            ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            ctx->IASetInputLayout(nullptr);
            ctx->VSSetShader(vsh, nullptr, 0); ctx->PSSetShader(psh, nullptr, 0);
            ctx->Draw(3, 0);
            memcpy(last[eye][idx], c, sizeof(c)); cleared[eye][idx] = 1;
            XrSwapchainImageReleaseInfo ri = { XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
            release(sc[eye], &ri);
            if (synccheck && eye == 0) {
                struct dmsubst_params p = {}; p.op = DMSUBST_OP_READBACK;
                p.mtl_texture = sc_tex(sc[0], idx); p.x = W / 2; p.y = H / 2; p.slice = 0;
                if (synccheck == 1) p.event = sess_ev(session, &p.event_value);
                dms(&p);
                int ok = 1;
                for (int k = 0; k < 4; k++) if (fabsf(p.rgba[k] - c[k]) > 1.5f / 255.0f) ok = 0;
                if (ok) sync_ok++; else sync_bad++;
                if (!ok && sync_bad <= 3)
                    printf("SYNC frame %d: read rgba(%.3f %.3f %.3f %.3f), wanted (%.3f %.3f %.3f %.3f)%s\n", f,
                           p.rgba[0], p.rgba[1], p.rgba[2], p.rgba[3], c[0], c[1], c[2], c[3],
                           p.event ? "" : " [no GPU wait]");
            }
            pv[eye] = { XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW };
            pv[eye].pose = views[eye].pose; pv[eye].fov = views[eye].fov;
            pv[eye].subImage.swapchain = sc[eye];
            pv[eye].subImage.imageRect.extent.width = (int32_t)W;
            pv[eye].subImage.imageRect.extent.height = (int32_t)H;
        }
        XrCompositionLayerProjection layer = { XR_TYPE_COMPOSITION_LAYER_PROJECTION };
        layer.space = space; layer.viewCount = 2; layer.views = pv;
        const XrCompositionLayerBaseHeader *layers[] = { (XrCompositionLayerBaseHeader *)&layer };
        XrFrameEndInfo fei = { XR_TYPE_FRAME_END_INFO };
        fei.displayTime = fs.predictedDisplayTime;
        fei.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
        fei.layerCount = 1; fei.layers = layers;
        r = endFrame(session, &fei);
        if (r != XR_SUCCESS) { printf("FAIL: xrEndFrame %d\n", r); return 1; }
    }
    DWORD ms = GetTickCount() - t0;
    if (synccheck)
        printf("SYNCCHECK(%s, %d heavy tris/frame): %d/%d immediate post-release readbacks correct\n",
               synccheck == 1 ? "readback queue GPU-waits on the release event" : "control: no wait",
               heavy, sync_ok, sync_ok + sync_bad);
    printf("OK: %d frames in %lu ms (%.2f ms/frame incl. CPU release sync)\n", frames,
           (unsigned long)ms, frames ? (double)ms / frames : 0.0);

    /* With the bridge's GPU fence the release no longer waits for the GPU;
     * make sure the last frame finished before reading it from another queue */
    gpu_idle(dev, ctx);

    int bad = 0, checked = 0;
    for (int eye = 0; eye < 2; eye++)
        for (uint32_t i = 0; i < nimg[eye]; i++) {
            if (!cleared[eye][i]) continue;
            for (int pt = 0; pt < 2; pt++) {
            struct dmsubst_params p = {}; p.op = DMSUBST_OP_READBACK;
            p.mtl_texture = sc_tex(sc[eye], i);
            p.x = pt ? W / 4 : W / 2; p.y = pt ? H / 4 : H / 2; p.slice = arr > 1 ? eye : 0;
            if (dms(&p)) { printf("FAIL: readback eye %d image %u status %d\n", eye, i, p.status); bad++; continue; }
            const float *e = pt ? drawn : last[eye][i];
            int ok = 1;
            for (int k = 0; k < 4; k++) if (fabsf(p.rgba[k] - e[k]) > 1.5f / 255.0f) ok = 0;
            printf("READBACK %s eye %d image %u (MTL fmt %u): raw %02x %02x %02x %02x -> rgba(%.3f %.3f %.3f %.3f), "
                   "expected (%.3f %.3f %.3f %.3f) %s\n", pt ? "drawn  (W/4,H/4)" : "cleared(W/2,H/2)", eye, i, p.tex_pixel_format,
                   p.raw[0], p.raw[1], p.raw[2], p.raw[3], p.rgba[0], p.rgba[1], p.rgba[2], p.rgba[3],
                   e[0], e[1], e[2], e[3], ok ? "MATCH" : "MISMATCH");
            checked++; if (!ok) bad++;
            }
        }
    stats("after frames");

    /* Extra probes, reported but not part of PASS: a CPU upload
     * (UpdateSubresource) and a GPU copy (CopySubresourceRegion) into a
     * swapchain image. D3DMetal asked for Shared storage; the runtime's texture
     * is Private, so a CPU-side replaceRegion would not land */
    if (getenv("DMS_TEST_EXTRA") && arr == 1) {
        XrSwapchainImageAcquireInfo ai = { XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO };
        XrSwapchainImageWaitInfo wi = { XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO }; wi.timeout = XR_INFINITE_DURATION;
        XrSwapchainImageReleaseInfo ri = { XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
        uint32_t idx = 0;
        acquire(sc[0], &ai, &idx); wait(sc[0], &wi);
        ID3D11Resource *res = nullptr; rtv[0][idx]->GetResource(&res);
        static uint32_t px[8 * 8];
        for (int k = 0; k < 64; k++) px[k] = 0x80402010u; /* B8G8R8A8: b=10 g=20 r=40 a=80 */
        D3D11_BOX box = { W / 2 + 16, H / 2, 0, W / 2 + 24, H / 2 + 8, 1 };
        ctx->UpdateSubresource(res, 0, &box, px, 8 * 4, 0);
        D3D11_TEXTURE2D_DESC sd = {}; sd.Width = 8; sd.Height = 8; sd.MipLevels = 1; sd.ArraySize = 1;
        /* CopySubresourceRegion needs the same format family as the image */
        sd.Format = (fmt == DXGI_FORMAT_R8G8B8A8_UNORM || fmt == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB)
                        ? DXGI_FORMAT_R8G8B8A8_UNORM : DXGI_FORMAT_B8G8R8A8_UNORM;
        sd.SampleDesc.Count = 1; sd.Usage = D3D11_USAGE_DEFAULT;
        sd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        for (int k = 0; k < 64; k++) px[k] = 0x40302010u;
        D3D11_SUBRESOURCE_DATA sdata = { px, 8 * 4, 0 };
        ID3D11Texture2D *src = nullptr; dev->CreateTexture2D(&sd, &sdata, &src);
        D3D11_BOX sbox = { 0, 0, 0, 8, 8, 1 };
        ctx->CopySubresourceRegion(res, 0, W / 2 + 32, H / 2, 0, src, 0, &sbox);
        release(sc[0], &ri);
        gpu_idle(dev, ctx);
        const char *names[2] = { "UpdateSubresource", "CopySubresourceRegion" };
        const uint8_t want[2][4] = { { 0x10, 0x20, 0x40, 0x80 }, { 0x10, 0x20, 0x30, 0x40 } };
        for (int t = 0; t < 2; t++) {
            struct dmsubst_params p = {}; p.op = DMSUBST_OP_READBACK; p.mtl_texture = sc_tex(sc[0], idx);
            p.x = W / 2 + (t ? 36 : 20); p.y = H / 2 + 4; dms(&p);
            printf("EXTRA %-22s raw %02x %02x %02x %02x (want %02x %02x %02x %02x) %s\n", names[t],
                   p.raw[0], p.raw[1], p.raw[2], p.raw[3], want[t][0], want[t][1], want[t][2], want[t][3],
                   memcmp(p.raw, want[t], 4) ? "MISMATCH" : "MATCH");
        }
        res->Release(); src->Release();
    }

    /* Teardown: exercise the lifecycle (D3DMetal releasing substituted
     * textures, residency-set removal) before the runtime frees its images */
    for (int eye = 0; eye < 2; eye++)
        for (uint32_t i = 0; i < nimg[eye]; i++) rtv[eye][i]->Release();
    ctx->ClearState(); ctx->Flush();
    for (int eye = 0; eye < 2; eye++)
        CHECK(fn<PFN_xrDestroySwapchain>(inst, "xrDestroySwapchain")(sc[eye]));
    fn<PFN_xrRequestExitSession>(inst, "xrRequestExitSession")(session);
    fn<PFN_xrEndSession>(inst, "xrEndSession")(session);
    CHECK(fn<PFN_xrDestroySession>(inst, "xrDestroySession")(session));
    CHECK(fn<PFN_xrDestroyInstance>(inst, "xrDestroyInstance")(inst));
    stats("after teardown");
    fflush(stdout);
    if (!checked || bad) { printf("FAIL: %d/%d texels did not read back their clear colour\n", bad, checked); return 1; }
    printf("PASS: rendered through the bridge (%d texels read back from the runtime's MTLTextures)\n", checked);
    return 0;
}

/* ---- VPRT: layered rendering into a 2-slice Texture2DArray -----------------
 * What Unity's single-pass instanced stereo does on D3D11: one DrawInstanced
 * with 2x the instances into an RTV spanning both slices, and the VERTEX
 * shader writes SV_RenderTargetArrayIndex = instanceID & 1. Three routes:
 *   vs    - VS writes SV_RenderTargetArrayIndex (needs D3D11.3 OPTIONS3 VPRT)
 *   gs    - classic geometry shader writes SV_RenderTargetArrayIndex
 *   slice - one RTV per slice, one draw each (what multi-pass stereo does)
 * Clear blue, instance/slice 0 draws red, instance/slice 1 green. Expected:
 * slice 0 red, slice 1 green. "Layer lost" shows as slice 0 green (instance 1
 * drawn over instance 0) and slice 1 blue (never touched) */

static void log_caps(ID3D11Device *dev)
{
    D3D11_FEATURE_DATA_D3D11_OPTIONS o = {};
    D3D11_FEATURE_DATA_D3D11_OPTIONS1 o1 = {};
    D3D11_FEATURE_DATA_D3D11_OPTIONS2 o2 = {};
    D3D11_FEATURE_DATA_D3D11_OPTIONS3 o3 = {};
    D3D11_FEATURE_DATA_D3D10_X_HARDWARE_OPTIONS x = {};
    HRESULT h, h1, h2, h3, hx;
    h = dev->CheckFeatureSupport(D3D11_FEATURE_D3D11_OPTIONS, &o, sizeof(o));
    h1 = dev->CheckFeatureSupport(D3D11_FEATURE_D3D11_OPTIONS1, &o1, sizeof(o1));
    h2 = dev->CheckFeatureSupport(D3D11_FEATURE_D3D11_OPTIONS2, &o2, sizeof(o2));
    h3 = dev->CheckFeatureSupport(D3D11_FEATURE_D3D11_OPTIONS3, &o3, sizeof(o3));
    hx = dev->CheckFeatureSupport(D3D11_FEATURE_D3D10_X_HARDWARE_OPTIONS, &x, sizeof(x));
    printf("CAPS feature level 0x%x\n", dev->GetFeatureLevel());
    printf("CAPS D3D11_OPTIONS hr=0x%08lx: OutputMergerLogicOp=%d UAVOnlyRenderingForcedSampleCount=%d "
           "DiscardAPIsSeenByDriver=%d FlagsForUpdateAndCopySeenByDriver=%d ClearView=%d "
           "CopyWithOverlap=%d ConstantBufferPartialUpdate=%d ConstantBufferOffsetting=%d "
           "MapNoOverwriteOnDynamicConstantBuffer=%d MapNoOverwriteOnDynamicBufferSRV=%d "
           "MultisampleRTVWithForcedSampleCountOne=%d SAD4ShaderInstructions=%d ExtendedDoublesShaderInstructions=%d "
           "ExtendedResourceSharing=%d\n", (unsigned long)h, o.OutputMergerLogicOp,
           o.UAVOnlyRenderingForcedSampleCount, o.DiscardAPIsSeenByDriver, o.FlagsForUpdateAndCopySeenByDriver,
           o.ClearView, o.CopyWithOverlap, o.ConstantBufferPartialUpdate, o.ConstantBufferOffsetting,
           o.MapNoOverwriteOnDynamicConstantBuffer, o.MapNoOverwriteOnDynamicBufferSRV,
           o.MultisampleRTVWithForcedSampleCountOne, o.SAD4ShaderInstructions,
           o.ExtendedDoublesShaderInstructions, o.ExtendedResourceSharing);
    printf("CAPS D3D11_OPTIONS1 hr=0x%08lx: TiledResourcesTier=%d MinMaxFiltering=%d "
           "ClearViewAlsoSupportsDepthOnlyFormats=%d MapOnDefaultBuffers=%d\n", (unsigned long)h1,
           o1.TiledResourcesTier, o1.MinMaxFiltering, o1.ClearViewAlsoSupportsDepthOnlyFormats, o1.MapOnDefaultBuffers);
    printf("CAPS D3D11_OPTIONS2 hr=0x%08lx: PSSpecifiedStencilRef=%d TypedUAVLoadAdditionalFormats=%d ROVs=%d "
           "ConservativeRasterizationTier=%d TiledResourcesTier=%d MapOnDefaultTextures=%d StandardSwizzle=%d "
           "UnifiedMemoryArchitecture=%d\n", (unsigned long)h2, o2.PSSpecifiedStencilRefSupported,
           o2.TypedUAVLoadAdditionalFormats, o2.ROVsSupported, o2.ConservativeRasterizationTier,
           o2.TiledResourcesTier, o2.MapOnDefaultTextures, o2.StandardSwizzle, o2.UnifiedMemoryArchitecture);
    printf("CAPS D3D11_OPTIONS3 hr=0x%08lx: VPAndRTArrayIndexFromAnyShaderFeedingRasterizer=%d\n",
           (unsigned long)h3, o3.VPAndRTArrayIndexFromAnyShaderFeedingRasterizer);
    printf("CAPS D3D10_X_HARDWARE_OPTIONS hr=0x%08lx: ComputeShaders_Plus_RawAndStructuredBuffers_Via_Shader_4_x=%d\n",
           (unsigned long)hx, x.ComputeShaders_Plus_RawAndStructuredBuffers_Via_Shader_4_x);
}

static const char *vprt_src =
    "struct VO { float4 pos : SV_POSITION; float4 col : COLOR0; uint rt : SV_RenderTargetArrayIndex; };\n"
    "struct VU { float4 pos : SV_POSITION; float4 col : COLOR0; uint rt : SV_RenderTargetArrayIndex; uint eye : BLENDINDICES0; };\n"
    "struct PO { float4 pos : SV_POSITION; float4 col : COLOR0; };\n"
    "struct VG { float4 pos : SV_POSITION; float4 col : COLOR0; uint inst : TEXCOORD0; };\n"
    "float4 tri(uint id) { float2 uv = float2((id << 1) & 2, id & 2);\n"
    "  return float4(uv * float2(2, -2) + float2(-1, 1), 0.5, 1); }\n"
    "float4 col(uint i) { return i == 0 ? float4(1, 0, 0, 1) : float4(0, 1, 0, 1); }\n"
    "VO vs_vprt(uint id : SV_VertexID, uint inst : SV_InstanceID) {\n"
    "  VO o; o.pos = tri(id); o.col = col(inst & 1); o.rt = inst & 1; return o; }\n"
    "VU vs_unity(float3 p : POSITION, uint inst : SV_InstanceID) {\n"
    "  VU o; o.pos = float4(p, 1); o.col = col(inst % 2); o.rt = inst % 2; o.eye = inst % 2; return o; }\n"
    "VG vs_gs(uint id : SV_VertexID, uint inst : SV_InstanceID) {\n"
    "  VG o; o.pos = tri(id); o.col = col(inst & 1); o.inst = inst & 1; return o; }\n"
    "[maxvertexcount(3)] void gs(triangle VG i[3], inout TriangleStream<VO> s) {\n"
    "  for (int k = 0; k < 3; k++) { VO o; o.pos = i[k].pos; o.col = i[k].col; o.rt = i[k].inst; s.Append(o); } }\n"
    "float4 vs_plain(uint id : SV_VertexID) : SV_POSITION { return tri(id); }\n"
    "float4 ps(VO i) : SV_TARGET { return i.col; }\n"
    "float4 ps_nort(PO i) : SV_TARGET { return i.col; }\n"
    "float4 ps_unity(VU i) : SV_TARGET { return i.col; }\n"
    "float4 ps_red() : SV_TARGET { return float4(1, 0, 0, 1); }\n"
    "float4 ps_green() : SV_TARGET { return float4(0, 1, 0, 1); }\n"
    "Texture2DMSArray<float4> ms : register(t0);\n"
    "float4 ps_load0(float4 p : SV_POSITION) : SV_TARGET { return ms.Load(int3(p.xy, 0), 0); }\n"
    "float4 ps_load1(float4 p : SV_POSITION) : SV_TARGET { return ms.Load(int3(p.xy, 1), 0); }\n";

struct vprt_shaders
{
    ID3D11VertexShader *vs_vprt, *vs_gs, *vs_plain, *vs_unity;
    ID3D11GeometryShader *gs;
    ID3D11PixelShader *ps, *ps_red, *ps_green, *ps_nort, *ps_unity, *ps_load0, *ps_load1;
    ID3D11RasterizerState *rs;
    ID3D11InputLayout *il;
    ID3D11Buffer *vb, *ib;
    ID3D11DepthStencilState *dss;
};

static bool vprt_compile(ID3D11Device *dev, vprt_shaders *s)
{
    struct { const char *entry, *target; void **out; int kind; } list[] = {
        { "vs_vprt", "vs_5_0", (void **)&s->vs_vprt, 0 }, { "vs_gs", "vs_5_0", (void **)&s->vs_gs, 0 },
        { "vs_plain", "vs_5_0", (void **)&s->vs_plain, 0 }, { "vs_unity", "vs_5_0", (void **)&s->vs_unity, 0 },
        { "gs", "gs_5_0", (void **)&s->gs, 1 },
        { "ps", "ps_5_0", (void **)&s->ps, 2 }, { "ps_red", "ps_5_0", (void **)&s->ps_red, 2 },
        { "ps_green", "ps_5_0", (void **)&s->ps_green, 2 }, { "ps_nort", "ps_5_0", (void **)&s->ps_nort, 2 },
        { "ps_unity", "ps_5_0", (void **)&s->ps_unity, 2 },
        { "ps_load0", "ps_5_0", (void **)&s->ps_load0, 2 }, { "ps_load1", "ps_5_0", (void **)&s->ps_load1, 2 },
    };
    memset(s, 0, sizeof(*s));
    for (auto &e : list) {
        ID3DBlob *b = nullptr, *err = nullptr;
        HRESULT hr = D3DCompile(vprt_src, strlen(vprt_src), nullptr, nullptr, nullptr, e.entry, e.target, 0, 0, &b, &err);
        if (FAILED(hr)) { printf("VPRT: compile %s failed: %s\n", e.entry, err ? (char *)err->GetBufferPointer() : "?"); return false; }
        if (e.kind == 0) hr = dev->CreateVertexShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, (ID3D11VertexShader **)e.out);
        else if (e.kind == 1) hr = dev->CreateGeometryShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, (ID3D11GeometryShader **)e.out);
        else hr = dev->CreatePixelShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, (ID3D11PixelShader **)e.out);
        if (FAILED(hr)) printf("VPRT: Create shader %s hr=0x%08lx\n", e.entry, (unsigned long)hr);
        if (e.out == (void **)&s->vs_unity) {
            D3D11_INPUT_ELEMENT_DESC ie = { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 };
            dev->CreateInputLayout(&ie, 1, b->GetBufferPointer(), b->GetBufferSize(), &s->il);
        }
        b->Release();
    }
    D3D11_RASTERIZER_DESC rsd = {}; rsd.FillMode = D3D11_FILL_SOLID; rsd.CullMode = D3D11_CULL_NONE; rsd.DepthClipEnable = TRUE;
    dev->CreateRasterizerState(&rsd, &s->rs);
    /* full-screen quad, indexed */
    static const float v[] = { -1, -1, 0.5f, -1, 1, 0.5f, 1, 1, 0.5f, 1, -1, 0.5f };
    static const uint16_t idx[] = { 0, 1, 2, 0, 2, 3 };
    D3D11_BUFFER_DESC bd = { sizeof(v), D3D11_USAGE_DEFAULT, D3D11_BIND_VERTEX_BUFFER, 0, 0, 0 };
    D3D11_SUBRESOURCE_DATA sd = { v, 0, 0 };
    dev->CreateBuffer(&bd, &sd, &s->vb);
    bd.ByteWidth = sizeof(idx); bd.BindFlags = D3D11_BIND_INDEX_BUFFER; sd.pSysMem = idx;
    dev->CreateBuffer(&bd, &sd, &s->ib);
    D3D11_DEPTH_STENCIL_DESC dsd = {}; dsd.DepthEnable = TRUE; dsd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
    dsd.DepthFunc = D3D11_COMPARISON_LESS_EQUAL;
    dev->CreateDepthStencilState(&dsd, &s->dss);
    return true;
}

enum { VPRT_VS, VPRT_GS, VPRT_SLICE, VPRT_VS_NORT, VPRT_UNITY, VPRT_UNITY_DEPTH, VPRT_UNITY_MSAA,
       VPRT_MSAA_NODEPTH, VPRT_MSAA_SLICE, VPRT_MSAA_LOAD, VPRT_MSAA_CLEARONLY, VPRT_MSAA_SLICE_DEPTH, VPRT_NROUTES };
static const char *vprt_route_name[] = { "VS SV_RenderTargetArrayIndex", "GS SV_RenderTargetArrayIndex", "per-slice RTVs",
    "VS rtai, PS input w/o rtai", "Unity-like indexed+IA+BLENDIDX", "Unity-like + 2-slice depth", "Unity-like MSAA4x+resolve",
    "MSAA4x layered, no depth, resolve", "MSAA4x per-slice RTVs, resolve", "MSAA4x layered, shader Load", "MSAA4x clear per slice, resolve",
    "MSAA4x per-slice RTV+DSV (multi-pass)" };

/* Render one route into tex (a 2-slice array); DXGI view format fmt */
static bool vprt_render(ID3D11Device *dev, ID3D11DeviceContext *ctx, const vprt_shaders *s,
                        ID3D11Texture2D *tex, DXGI_FORMAT fmt, int route)
{
    D3D11_TEXTURE2D_DESC td; tex->GetDesc(&td);
    ID3D11Texture2D *target = tex, *msaa = nullptr, *depth = nullptr;
    ID3D11DepthStencilView *dsv = nullptr;
    bool is_msaa = route >= VPRT_UNITY_MSAA;
    if (is_msaa) {
        D3D11_TEXTURE2D_DESC md = td; md.Format = fmt; md.SampleDesc.Count = 4; md.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE; md.MiscFlags = 0;
        struct dmsubst_params p = {}; p.op = DMSUBST_OP_ARM; p.flags = DMSUBST_ARM_PROBE; dms(&p);
        HRESULT hr = dev->CreateTexture2D(&md, nullptr, &msaa);
        p = {}; p.op = DMSUBST_OP_DISARM; dms(&p);
        static int once;
        if (!once++)
            printf("VPRT MSAA color D3D11 %ux%u arr=%u samples=%u -> Metal type=%u arr=%u fmt=%u usage=0x%llx (via %s, %u creations)\n",
                   md.Width, md.Height, md.ArraySize, md.SampleDesc.Count, p.desc_texture_type, p.desc_array_length,
                   p.desc_pixel_format, (unsigned long long)p.desc_usage, p.where, p.seen);
        if (FAILED(hr)) { printf("VPRT: MSAA array texture hr=0x%08lx\n", (unsigned long)hr); return false; }
        target = msaa;
    }
    ID3D11DepthStencilView *dsone[2] = {};
    if (route == VPRT_UNITY_DEPTH || route == VPRT_UNITY_MSAA || route == VPRT_MSAA_LOAD || route == VPRT_MSAA_SLICE_DEPTH) {
        D3D11_TEXTURE2D_DESC dd = td; dd.Format = DXGI_FORMAT_D24_UNORM_S8_UINT; dd.BindFlags = D3D11_BIND_DEPTH_STENCIL;
        dd.MiscFlags = 0; dd.SampleDesc.Count = is_msaa ? 4 : 1;
        if (getenv("DMS_TEST_DEPTH_FMT")) dd.Format = (DXGI_FORMAT)atoi(getenv("DMS_TEST_DEPTH_FMT"));
        {
            struct dmsubst_params p = {}; p.op = DMSUBST_OP_ARM; p.flags = DMSUBST_ARM_PROBE; dms(&p);
            HRESULT hr = dev->CreateTexture2D(&dd, nullptr, &depth);
            p = {}; p.op = DMSUBST_OP_DISARM; dms(&p);
            static int once[2];
            if (!once[dd.SampleDesc.Count > 1]++)
                printf("VPRT depth D3D11 fmt=%d %ux%u arr=%u samples=%u hr=0x%08lx -> Metal type=%u arr=%u fmt=%u usage=0x%llx (via %s, %u creations)\n",
                       (int)dd.Format, dd.Width, dd.Height, dd.ArraySize, dd.SampleDesc.Count, (unsigned long)hr, p.desc_texture_type,
                       p.desc_array_length, p.desc_pixel_format, (unsigned long long)p.desc_usage, p.where, p.seen);
        }
        D3D11_DEPTH_STENCIL_VIEW_DESC dvd = {}; dvd.Format = dd.Format;
        if (dd.SampleDesc.Count > 1) { dvd.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2DMSARRAY; dvd.Texture2DMSArray.ArraySize = 2; }
        else { dvd.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2DARRAY; dvd.Texture2DArray.ArraySize = 2; }
        if (!depth || FAILED(dev->CreateDepthStencilView(depth, &dvd, &dsv))) printf("VPRT: depth array DSV failed\n");
        else ctx->ClearDepthStencilView(dsv, D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 1.0f, 0);
        for (int i = 0; i < 2 && depth && route == VPRT_MSAA_SLICE_DEPTH; i++) {
            D3D11_DEPTH_STENCIL_VIEW_DESC d1 = dvd;
            d1.Texture2DMSArray.FirstArraySlice = i; d1.Texture2DMSArray.ArraySize = 1;
            dev->CreateDepthStencilView(depth, &d1, &dsone[i]);
        }
    }
    D3D11_RENDER_TARGET_VIEW_DESC rd = {};
    rd.Format = fmt; rd.ViewDimension = msaa ? D3D11_RTV_DIMENSION_TEXTURE2DMSARRAY : D3D11_RTV_DIMENSION_TEXTURE2DARRAY;
    rd.Texture2DArray.FirstArraySlice = 0; rd.Texture2DArray.ArraySize = 2;
    if (msaa) { rd.Texture2DMSArray.FirstArraySlice = 0; rd.Texture2DMSArray.ArraySize = 2; }
    ID3D11RenderTargetView *all = nullptr, *one[2] = {};
    if (FAILED(dev->CreateRenderTargetView(target, &rd, &all))) { printf("VPRT: 2-slice RTV failed\n"); return false; }
    ID3D11RenderTargetView *msone[2] = {};
    for (int i = 0; i < 2; i++) {
        D3D11_RENDER_TARGET_VIEW_DESC r1 = {}; r1.Format = fmt; r1.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2DARRAY;
        r1.Texture2DArray.FirstArraySlice = i; r1.Texture2DArray.ArraySize = 1;
        dev->CreateRenderTargetView(tex, &r1, &one[i]);
        if (msaa) {
            r1.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2DMSARRAY; r1.Texture2DMSArray.FirstArraySlice = i; r1.Texture2DMSArray.ArraySize = 1;
            dev->CreateRenderTargetView(msaa, &r1, &msone[i]);
        }
    }
    const float blue[4] = { 0, 0, 1, 1 };
    ctx->ClearRenderTargetView(all, blue);
    if (msaa) { const float black[4] = { 0, 0, 0, 1 }; ctx->ClearRenderTargetView(one[0], black); ctx->ClearRenderTargetView(one[1], black); }
    D3D11_VIEWPORT vp = { 0, 0, (FLOAT)td.Width, (FLOAT)td.Height, 0, 1 };
    ctx->RSSetViewports(1, &vp); ctx->RSSetState(s->rs);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx->IASetInputLayout(nullptr);
    ctx->OMSetDepthStencilState(dsv ? s->dss : nullptr, 0);
    if (route == VPRT_SLICE) {
        ctx->VSSetShader(s->vs_plain, nullptr, 0); ctx->GSSetShader(nullptr, nullptr, 0);
        for (int i = 0; i < 2; i++) {
            ctx->OMSetRenderTargets(1, &one[i], nullptr);
            ctx->PSSetShader(i ? s->ps_green : s->ps_red, nullptr, 0);
            ctx->Draw(3, 0);
        }
    } else if (route == VPRT_MSAA_SLICE_DEPTH) {
        /* per-eye passes as multi-pass stereo does, each with its own DSV slice.
         * Slice 0's depth is cleared to 0 after slice 1's to 1, so a DSV that
         * maps to the wrong slice shows: expected red fails the depth test
         * (slice 0 stays black), green passes */
        ctx->ClearDepthStencilView(dsone[1], D3D11_CLEAR_DEPTH, 1.0f, 0);
        ctx->ClearDepthStencilView(dsone[0], D3D11_CLEAR_DEPTH, 0.0f, 0);
        ctx->OMSetDepthStencilState(s->dss, 0);
        ctx->VSSetShader(s->vs_plain, nullptr, 0); ctx->GSSetShader(nullptr, nullptr, 0);
        const float black[4] = { 0, 0, 0, 1 };
        for (int i = 0; i < 2; i++) {
            ctx->ClearRenderTargetView(msone[i], black);
            ctx->OMSetRenderTargets(1, &msone[i], dsone[i]);
            ctx->PSSetShader(i ? s->ps_green : s->ps_red, nullptr, 0);
            ctx->Draw(3, 0);
        }
        ctx->OMSetRenderTargets(0, nullptr, nullptr);
        for (int i = 0; i < 2; i++)
            ctx->ResolveSubresource(tex, D3D11CalcSubresource(0, i, 1), msaa, D3D11CalcSubresource(0, i, 1), fmt);
    } else if (route == VPRT_MSAA_SLICE || route == VPRT_MSAA_CLEARONLY) {
        const float c[2][4] = { { 1, 0, 0, 1 }, { 0, 1, 0, 1 } };
        ctx->VSSetShader(s->vs_plain, nullptr, 0); ctx->GSSetShader(nullptr, nullptr, 0);
        for (int i = 0; i < 2; i++) {
            if (route == VPRT_MSAA_CLEARONLY) { ctx->ClearRenderTargetView(msone[i], c[i]); continue; }
            ctx->OMSetRenderTargets(1, &msone[i], nullptr);
            ctx->PSSetShader(i ? s->ps_green : s->ps_red, nullptr, 0);
            ctx->Draw(3, 0);
        }
        ctx->OMSetRenderTargets(0, nullptr, nullptr);
        for (int i = 0; i < 2; i++)
            ctx->ResolveSubresource(tex, D3D11CalcSubresource(0, i, 1), msaa, D3D11CalcSubresource(0, i, 1), fmt);
    } else if (route >= VPRT_UNITY) {
        UINT stride = 12, off = 0;
        ctx->OMSetRenderTargets(1, &all, dsv);
        ctx->IASetInputLayout(s->il);
        ctx->IASetVertexBuffers(0, 1, &s->vb, &stride, &off);
        ctx->IASetIndexBuffer(s->ib, DXGI_FORMAT_R16_UINT, 0);
        ctx->VSSetShader(s->vs_unity, nullptr, 0); ctx->GSSetShader(nullptr, nullptr, 0);
        ctx->PSSetShader(s->ps_unity, nullptr, 0);
        ctx->DrawIndexedInstanced(6, 2, 0, 0, 0);
        if (msaa && route == VPRT_MSAA_LOAD) {
            /* read the MSAA array back with Texture2DMSArray.Load (sample 0) instead of ResolveSubresource */
            ctx->OMSetRenderTargets(0, nullptr, nullptr);
            ID3D11ShaderResourceView *srv = nullptr;
            D3D11_SHADER_RESOURCE_VIEW_DESC sv = {}; sv.Format = fmt; sv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DMSARRAY;
            sv.Texture2DMSArray.FirstArraySlice = 0; sv.Texture2DMSArray.ArraySize = 2;
            dev->CreateShaderResourceView(msaa, &sv, &srv);
            ctx->IASetInputLayout(nullptr);
            ctx->VSSetShader(s->vs_plain, nullptr, 0);
            ctx->OMSetDepthStencilState(nullptr, 0);
            for (int i = 0; i < 2; i++) {
                ctx->OMSetRenderTargets(1, &one[i], nullptr);
                ctx->PSSetShader(i ? s->ps_load1 : s->ps_load0, nullptr, 0);
                ctx->PSSetShaderResources(0, 1, &srv);
                ctx->Draw(3, 0);
            }
            ID3D11ShaderResourceView *nul = nullptr; ctx->PSSetShaderResources(0, 1, &nul);
            srv->Release();
        } else if (msaa) {
            ctx->OMSetRenderTargets(0, nullptr, nullptr);
            for (int i = 0; i < 2; i++)
                ctx->ResolveSubresource(tex, D3D11CalcSubresource(0, i, 1), msaa, D3D11CalcSubresource(0, i, 1), fmt);
        }
    } else {
        ctx->OMSetRenderTargets(1, &all, nullptr);
        ctx->VSSetShader(route == VPRT_GS ? s->vs_gs : s->vs_vprt, nullptr, 0);
        ctx->GSSetShader(route == VPRT_GS ? s->gs : nullptr, nullptr, 0);
        ctx->PSSetShader(route == VPRT_VS_NORT ? s->ps_nort : s->ps, nullptr, 0);
        ctx->DrawInstanced(3, 2, 0, 0);
    }
    ctx->GSSetShader(nullptr, nullptr, 0);
    ctx->OMSetRenderTargets(0, nullptr, nullptr);
    ctx->OMSetDepthStencilState(nullptr, 0);
    all->Release();
    for (int i = 0; i < 2; i++) { if (one[i]) one[i]->Release(); if (msone[i]) msone[i]->Release(); }
    if (dsv) dsv->Release();
    for (int i = 0; i < 2; i++) if (dsone[i]) dsone[i]->Release();
    if (depth) depth->Release();
    if (msaa) msaa->Release();
    return true;
}

/* D3D11-side readback of the centre texel of both slices through a staging copy */
static void vprt_read_d3d(ID3D11Device *dev, ID3D11DeviceContext *ctx, ID3D11Texture2D *tex, uint8_t out[2][4])
{
    D3D11_TEXTURE2D_DESC td; tex->GetDesc(&td);
    D3D11_TEXTURE2D_DESC sd = td; sd.Usage = D3D11_USAGE_STAGING; sd.BindFlags = 0;
    sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ; sd.MiscFlags = 0;
    ID3D11Texture2D *st = nullptr;
    memset(out, 0xee, 8);
    if (FAILED(dev->CreateTexture2D(&sd, nullptr, &st))) { printf("VPRT: staging failed\n"); return; }
    ctx->CopyResource(st, tex);
    for (int i = 0; i < 2; i++) {
        D3D11_MAPPED_SUBRESOURCE m;
        UINT sub = D3D11CalcSubresource(0, i, td.MipLevels);
        if (SUCCEEDED(ctx->Map(st, sub, D3D11_MAP_READ, 0, &m))) {
            memcpy(out[i], (uint8_t *)m.pData + (td.Height / 2) * m.RowPitch + (td.Width / 2) * 4, 4);
            ctx->Unmap(st, sub);
        }
    }
    st->Release();
}

static int vprt_judge(const char *target, int route, const uint8_t px[2][4], bool bgra, const char *how)
{
    /* red / green / blue by channel (RGBA or BGRA byte order) */
    auto name = [bgra](const uint8_t *p) -> const char * {
        int r = bgra ? p[2] : p[0], g = p[1], b = bgra ? p[0] : p[2];
        if (r > 200 && g < 50 && b < 50) return "red";
        if (g > 200 && r < 50 && b < 50) return "green";
        if (b > 200 && r < 50 && g < 50) return "blue";
        if (!r && !g && !b) return "zero";
        return "other";
    };
    const char *s0 = name(px[0]), *s1 = name(px[1]);
    const char *want0 = route == VPRT_MSAA_SLICE_DEPTH ? "zero" : "red";
    int ok = !strcmp(s0, want0) && !strcmp(s1, "green");
    printf("VPRT %-9s %-30s (%s): slice0 %02x%02x%02x%02x=%s slice1 %02x%02x%02x%02x=%s -> %s\n", target,
           vprt_route_name[route], how, px[0][0], px[0][1], px[0][2], px[0][3], s0,
           px[1][0], px[1][1], px[1][2], px[1][3], s1,
           ok ? "PASS" : (!strcmp(s0, "green") && !strcmp(s1, "blue")) ? "FAIL (layer index lost: both instances in slice 0)" : "FAIL");
    return ok;
}

static void vprt_read_mtl(uint64_t mtl, uint8_t out[2][4])
{
    for (int i = 0; i < 2; i++) {
        struct dmsubst_params p = {}; p.op = DMSUBST_OP_READBACK; p.mtl_texture = mtl; p.slice = i;
        memset(out[i], 0xee, 4);
        if (!mtl) continue;
        /* centre texel; the Metal texture's size is what we created */
        p.x = 32; p.y = 32;
        if (!dms(&p)) memcpy(out[i], p.raw, 4);
    }
}

/* (a) a plain D3DMetal-owned 64x64 2-slice texture, no substitution */
static int run_vprt_plain(ID3D11Device *dev, ID3D11DeviceContext *ctx, DXGI_FORMAT res_fmt, DXGI_FORMAT view_fmt)
{
    vprt_shaders s; if (!vprt_compile(dev, &s)) return 1;
    int pass = 0;
    for (int route = 0; route < VPRT_NROUTES; route++) {
        D3D11_TEXTURE2D_DESC d = {}; d.Width = d.Height = 64; d.MipLevels = 1; d.ArraySize = 2; d.Format = res_fmt;
        d.SampleDesc.Count = 1; d.Usage = D3D11_USAGE_DEFAULT; d.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        struct dmsubst_params p = {}; p.op = DMSUBST_OP_ARM; p.flags = DMSUBST_ARM_PROBE | DMSUBST_ARM_CAPTURE; dms(&p);
        ID3D11Texture2D *tex = nullptr;
        HRESULT hr = dev->CreateTexture2D(&d, nullptr, &tex);
        p = {}; p.op = DMSUBST_OP_DISARM; dms(&p);
        if (FAILED(hr)) { printf("VPRT: CreateTexture2D hr=0x%08lx\n", (unsigned long)hr); return 1; }
        if (route == 0)
            printf("VPRT plain texture: D3DMetal's own MTLTexture 0x%llx type=%u arr=%u usage=0x%llx fmt=%u via %s\n",
                   (unsigned long long)p.created_texture, p.desc_texture_type, p.desc_array_length,
                   (unsigned long long)p.desc_usage, p.desc_pixel_format, p.where);
        vprt_render(dev, ctx, &s, tex, view_fmt, route);
        gpu_idle(dev, ctx);
        uint8_t a[2][4], b[2][4];
        vprt_read_d3d(dev, ctx, tex, a);
        vprt_read_mtl(p.created_texture, b);
        bool bgra = res_fmt == DXGI_FORMAT_B8G8R8A8_TYPELESS || res_fmt == DXGI_FORMAT_B8G8R8A8_UNORM;
        pass += vprt_judge("plain", route, a, bgra, "D3D11 staging");
        if (p.created_texture) vprt_judge("plain", route, b, bgra, "Metal blit");
        tex->Release();
    }
    printf("VPRT plain: %d/%d routes correct\n", pass, VPRT_NROUTES);
    return 0;
}

/* (b) the substituted runtime texture: one arraySize=2 swapchain, as Unity creates */
static int run_vprt_xr(HMODULE dll, ID3D11Device *dev, ID3D11DeviceContext *ctx)
{
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
    snprintf(ici.applicationInfo.applicationName, XR_MAX_APPLICATION_NAME_SIZE, "dmsubst-vprt");
    XrInstance inst;
    CHECK(fn<PFN_xrCreateInstance>(XR_NULL_HANDLE, "xrCreateInstance")(&ici, &inst));
    XrSystemGetInfo sgi = { XR_TYPE_SYSTEM_GET_INFO }; sgi.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    XrSystemId sys;
    CHECK(fn<PFN_xrGetSystem>(inst, "xrGetSystem")(inst, &sgi, &sys));
    XrGraphicsRequirementsD3D11KHR reqs = { XR_TYPE_GRAPHICS_REQUIREMENTS_D3D11_KHR };
    CHECK(fn<PFN_xrGetD3D11GraphicsRequirementsKHR>(inst, "xrGetD3D11GraphicsRequirementsKHR")(inst, sys, &reqs));
    XrGraphicsBindingD3D11KHR bind = { XR_TYPE_GRAPHICS_BINDING_D3D11_KHR }; bind.device = dev;
    XrSessionCreateInfo sci = { XR_TYPE_SESSION_CREATE_INFO }; sci.next = &bind; sci.systemId = sys;
    XrSession session;
    CHECK(fn<PFN_xrCreateSession>(inst, "xrCreateSession")(inst, &sci, &session));

    int64_t fmt = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    if (getenv("DMS_TEST_FORMAT")) fmt = atoll(getenv("DMS_TEST_FORMAT"));
    XrSwapchainCreateInfo scci = { XR_TYPE_SWAPCHAIN_CREATE_INFO };
    scci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
    scci.format = fmt; scci.sampleCount = 1; scci.width = 64; scci.height = 64;
    scci.faceCount = 1; scci.arraySize = 2; scci.mipCount = 1;
    XrSwapchain sc;
    CHECK(fn<PFN_xrCreateSwapchain>(inst, "xrCreateSwapchain")(session, &scci, &sc));
    XrSwapchainImageD3D11KHR imgs[8]; uint32_t n = 0;
    for (int i = 0; i < 8; i++) imgs[i] = { XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR };
    CHECK(fn<PFN_xrEnumerateSwapchainImages>(inst, "xrEnumerateSwapchainImages")(sc, 8, &n, (XrSwapchainImageBaseHeader *)imgs));
    auto acquire = fn<PFN_xrAcquireSwapchainImage>(inst, "xrAcquireSwapchainImage");
    auto wait = fn<PFN_xrWaitSwapchainImage>(inst, "xrWaitSwapchainImage");
    auto release = fn<PFN_xrReleaseSwapchainImage>(inst, "xrReleaseSwapchainImage");
    vprt_shaders s; if (!vprt_compile(dev, &s)) return 1;
    int pass = 0;
    bool bgra = fmt == DXGI_FORMAT_B8G8R8A8_UNORM || fmt == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
    for (int route = 0; route < VPRT_NROUTES; route++) {
        uint32_t idx = 0;
        XrSwapchainImageAcquireInfo ai = { XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO };
        XrSwapchainImageWaitInfo wi = { XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO }; wi.timeout = XR_INFINITE_DURATION;
        XrSwapchainImageReleaseInfo ri = { XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
        acquire(sc, &ai, &idx); wait(sc, &wi);
        D3D11_TEXTURE2D_DESC td; imgs[idx].texture->GetDesc(&td);
        if (route == 0) {
            struct dmsubst_params q = {}; q.op = DMSUBST_OP_READBACK; q.mtl_texture = sc_tex(sc, idx); dms(&q);
            printf("VPRT subst: image %u desc %ux%u arr=%u fmt=%d; runtime MTLTexture 0x%llx fmt=%u\n", idx, td.Width,
                   td.Height, td.ArraySize, (int)td.Format, (unsigned long long)sc_tex(sc, idx), q.tex_pixel_format);
        }
        vprt_render(dev, ctx, &s, imgs[idx].texture, (DXGI_FORMAT)fmt, route);
        release(sc, &ri);
        gpu_idle(dev, ctx);
        uint8_t a[2][4], b[2][4];
        vprt_read_d3d(dev, ctx, imgs[idx].texture, a);
        vprt_read_mtl(sc_tex(sc, idx), b);
        vprt_judge("subst", route, a, bgra, "D3D11 staging");
        pass += vprt_judge("subst", route, b, bgra, "runtime MTLTexture");
    }
    printf("VPRT subst: %d/%d routes correct (runtime texture)\n", pass, VPRT_NROUTES);
    ctx->ClearState(); ctx->Flush();
    CHECK(fn<PFN_xrDestroySwapchain>(inst, "xrDestroySwapchain")(sc));
    CHECK(fn<PFN_xrDestroySession>(inst, "xrDestroySession")(session));
    CHECK(fn<PFN_xrDestroyInstance>(inst, "xrDestroyInstance")(inst));
    return 0;
}

int main(int argc, char **argv)
{
    const char *mode = argc > 1 ? argv[1] : "both";
    const char *path = getenv("DMS_TEST_DLL") ? getenv("DMS_TEST_DLL") : "wineopenxr.dll";
    int frames = getenv("DMS_TEST_FRAMES") ? atoi(getenv("DMS_TEST_FRAMES")) : 90;
    setvbuf(stdout, nullptr, _IONBF, 0);

    HMODULE dll = LoadLibraryA(path);
    if (!dll) { printf("FAIL: LoadLibrary(%s) (%lu)\n", path, GetLastError()); return 1; }
    char got[MAX_PATH]; GetModuleFileNameA(dll, got, sizeof(got));
    dms = (PFN_proto_dmsubst)GetProcAddress(dll, "wineopenxr_proto_dmsubst");
    sc_tex = (PFN_proto_sc_tex)GetProcAddress(dll, "wineopenxr_proto_swapchain_mtl_texture");
    sess_ev = (PFN_proto_sess_ev)GetProcAddress(dll, "wineopenxr_proto_session_event");
    if (!dms || !sc_tex) { printf("FAIL: %s is not the prototype bridge (no proto exports)\n", got); return 1; }
    printf("OK: loaded %s\n", got);

    struct dmsubst_params p = {}; p.op = DMSUBST_OP_DETECT; dms(&p);
    printf("DETECT before device: 0x%x\n", p.detect_flags);
    if (!getenv("DMS_TEST_LATE_INSTALL")) {
        p = {}; p.op = DMSUBST_OP_INSTALL; dms(&p);
        printf("INSTALL (before D3D11CreateDevice): detect 0x%x\n", p.detect_flags);
    }

    ID3D11Device *dev = nullptr; ID3D11DeviceContext *ctx = nullptr;
    D3D_FEATURE_LEVEL fl_req[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0 }, fl_got;
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, fl_req, 2,
                                   D3D11_SDK_VERSION, &dev, &fl_got, &ctx);
    if (FAILED(hr)) { printf("FAIL: D3D11CreateDevice hr=0x%08lx\n", (unsigned long)hr); return 1; }
    p = {}; p.op = DMSUBST_OP_DETECT; dms(&p);
    printf("OK: D3D11 device %p, feature level 0x%x; DETECT after device: 0x%x (D3DMetal=%d DXMT=%d MTL4=%d)\n",
           (void *)dev, fl_got, p.detect_flags, !!(p.detect_flags & DMSUBST_DETECT_D3DMETAL),
           !!(p.detect_flags & DMSUBST_DETECT_DXMT), !!(p.detect_flags & DMSUBST_DETECT_MTL4));
    if (getenv("DMS_TEST_LATE_INSTALL")) {
        p = {}; p.op = DMSUBST_OP_INSTALL; dms(&p);
        printf("INSTALL (after D3D11CreateDevice): detect 0x%x\n", p.detect_flags);
    }
    stats("after device");

    int rc = 0;
    if (!strcmp(mode, "probe") || !strcmp(mode, "both")) rc |= run_probe(dev);
    if (!strcmp(mode, "probe") || !strcmp(mode, "both") || !strcmp(mode, "fence")) rc |= run_fence_probe(dev, ctx);
    if (!rc && (!strcmp(mode, "xr") || !strcmp(mode, "both"))) rc |= run_xr(dll, dev, ctx, frames);
    if (!strcmp(mode, "vprt") || !strcmp(mode, "caps")) log_caps(dev);
    if (!strcmp(mode, "vprt")) {
        rc |= run_vprt_plain(dev, ctx, DXGI_FORMAT_R8G8B8A8_TYPELESS, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB);
        if (!getenv("DMS_TEST_NO_XR")) rc |= run_vprt_xr(dll, dev, ctx);
    }
    return rc;
}
