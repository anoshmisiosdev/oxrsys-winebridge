/* D3D12 on CrossOver's D3DMetal rendering straight into the OXRSys runtime's
 * swapchain MTLTextures (bridge dmsubst path, XR_KHR_D3D12_enable).
 *
 *   probe  - with the bridge's Metal swizzles installed, arm this thread in
 *            PROBE mode around ID3D12Device::CreateCommittedResource (2D and
 *            2D-array render targets, typeless / sRGB / 16F, depth, MSAA,
 *            UAV), CreateHeap + CreatePlacedResource and CreateFence, and
 *            report which Metal selector D3DMetal used, on which thread.
 *   fence  - CreateFence under an EVENT arm, then ID3D12CommandQueue::Signal:
 *            does the captured MTLSharedEvent follow the D3D12 fence?
 *   xr     - full OpenXR D3D12 session on the D3DMetal device: swapchain
 *            images arrive as ID3D12Resources (substituted), each frame each
 *            eye is cleared and a scissored triangle drawn (optionally
 *            rendered elsewhere and copied in, like REFramework), submitted on
 *            the app queue, released (the bridge signals its fence on that
 *            queue), and at the end every image is read back on the Metal side
 *            from the runtime's own MTLTexture.
 *   both   - probe + fence + xr (default).
 *
 * xr env knobs: DMS_TEST_FRAMES, DMS_TEST_FORMAT (DXGI), DMS_TEST_ARRAY=2
 * (one 2-slice swapchain per eye; eye N renders slice N), DMS_TEST_COPY=1
 * (render into an intermediate texture, CopyTextureRegion into the image -
 * REFramework's pattern), DMS_TEST_MSAA=4 (render MSAA, ResolveSubresource
 * into the image), DMS_TEST_SYNCCHECK=1|2 as in the D3D11 test.
 * Bridge knobs: OXR_DMSUBST_TRACE=1|2, OXR_DMSUBST_SYNC=cpu,
 * OXR_BRIDGE_D3D12_BACKEND=d3dmetal|none, OXR_DMSUBST_DUMP_AT=N. D3DMetal's
 * Metal 4 backend: D3DM_MTL4=1 (pass via EXTRA_CX_ENV).
 *
 * Build: x86_64-w64-mingw32-g++ -O1 -static -I../bridge/extern/OpenXR-SDK/include \
 *          d3d12_d3dmetal_subst_test.cpp -ld3d12 -ldxgi -ld3dcompiler -o d3d12_d3dmetal_subst_test.exe
 * Run: BUILD=<bridge build> test/dmsubst/run-in-bottle.sh /abs/path/d3d12_d3dmetal_subst_test.exe [mode] */
#define WIDL_EXPLICIT_AGGREGATE_RETURNS 1
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <d3dcompiler.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#define XR_USE_PLATFORM_WIN32 1
#define XR_USE_GRAPHICS_API_D3D12 1
#include "openxr/openxr.h"
#include "openxr/openxr_platform.h"
#include "openxr/openxr_loader_negotiation.h"
#include "../bridge/src/include/dmsubst.h"

typedef int (WINAPI *PFN_proto_dmsubst)(struct dmsubst_params *);
typedef uint64_t (WINAPI *PFN_proto_sc_tex)(XrSwapchain, uint32_t);
typedef uint64_t (WINAPI *PFN_proto_sess_ev)(XrSession, uint64_t *);
static PFN_proto_dmsubst dms;
static PFN_proto_sc_tex sc_tex;
static PFN_proto_sess_ev sess_ev;

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
}

/* ---- a tiny GPU-idle helper on the app queue ---------------------------- */

struct gpu_sync {
    ID3D12Fence *fence = nullptr; HANDLE ev = nullptr; UINT64 value = 0;
    void init(ID3D12Device *dev) {
        dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, __uuidof(ID3D12Fence), (void **)&fence);
        ev = CreateEventA(nullptr, FALSE, FALSE, nullptr);
    }
    UINT64 signal(ID3D12CommandQueue *q) { q->Signal(fence, ++value); return value; }
    bool wait(UINT64 v, DWORD ms = 5000) {
        if (fence->GetCompletedValue() >= v) return true;
        fence->SetEventOnCompletion(v, ev);
        return WaitForSingleObject(ev, ms) == WAIT_OBJECT_0;
    }
    bool idle(ID3D12CommandQueue *q) { return wait(signal(q)); }
};

static void barrier(ID3D12GraphicsCommandList *cl, ID3D12Resource *r, D3D12_RESOURCE_STATES from,
                    D3D12_RESOURCE_STATES to, UINT sub = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES)
{
    D3D12_RESOURCE_BARRIER b = {};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = r; b.Transition.Subresource = sub;
    b.Transition.StateBefore = from; b.Transition.StateAfter = to;
    cl->ResourceBarrier(1, &b);
}

/* ---- probe --------------------------------------------------------------- */

struct probe_case { const char *name; DXGI_FORMAT fmt; D3D12_RESOURCE_FLAGS flags; UINT arr; UINT samples;
                    D3D12_RESOURCE_STATES state; D3D12_HEAP_FLAGS heap_flags; };

static void probe_report(const char *what, HRESULT hr, const struct dmsubst_params &p)
{
    printf("PROBE %-34s hr=0x%08lx same-thread creations=%u any-thread=%u via \"%s\" -> MTL fmt=%u type=%u %ux%u arr=%u mips=%u usage=0x%llx storage=%u\n",
           what, (unsigned long)hr, p.seen, p.global_seen, p.where, p.desc_pixel_format,
           p.desc_texture_type, p.desc_width, p.desc_height, p.desc_array_length, p.desc_mips,
           (unsigned long long)p.desc_usage, p.desc_storage_mode);
}

static int run_probe(ID3D12Device *dev)
{
    struct dmsubst_params t = {}; t.op = DMSUBST_OP_THREAD; dms(&t);
    printf("PROBE: this thread: win32 tid %lu, unix pthread id %llu\n",
           GetCurrentThreadId(), (unsigned long long)t.thread_id);
    const D3D12_RESOURCE_FLAGS RT = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET, DS = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL,
                               UAV = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    const D3D12_RESOURCE_STATES SRT = D3D12_RESOURCE_STATE_RENDER_TARGET, SDS = D3D12_RESOURCE_STATE_DEPTH_WRITE;
    static const probe_case cases[] = {
        { "committed RGBA8_UNORM RT",         DXGI_FORMAT_R8G8B8A8_UNORM,      RT, 1, 1, SRT, D3D12_HEAP_FLAG_NONE },
        { "committed RGBA8_TYPELESS RT",      DXGI_FORMAT_R8G8B8A8_TYPELESS,   RT, 1, 1, SRT, D3D12_HEAP_FLAG_NONE },
        { "committed RGBA8_UNORM_SRGB RT",    DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, RT, 1, 1, SRT, D3D12_HEAP_FLAG_NONE },
        { "committed BGRA8_TYPELESS RT",      DXGI_FORMAT_B8G8R8A8_TYPELESS,   RT, 1, 1, SRT, D3D12_HEAP_FLAG_NONE },
        { "committed RGBA16F RT",             DXGI_FORMAT_R16G16B16A16_FLOAT,  RT, 1, 1, SRT, D3D12_HEAP_FLAG_NONE },
        { "committed R10G10B10A2 RT",         DXGI_FORMAT_R10G10B10A2_UNORM,   RT, 1, 1, SRT, D3D12_HEAP_FLAG_NONE },
        { "committed RGBA8_TYPELESS RT arr2", DXGI_FORMAT_R8G8B8A8_TYPELESS,   RT, 2, 1, SRT, D3D12_HEAP_FLAG_NONE },
        { "committed RGBA8 RT NOT_ZEROED",    DXGI_FORMAT_R8G8B8A8_TYPELESS,   RT, 1, 1, SRT, D3D12_HEAP_FLAG_CREATE_NOT_ZEROED },
        { "committed RGBA8 RT|UAV",           DXGI_FORMAT_R8G8B8A8_UNORM,      (D3D12_RESOURCE_FLAGS)(RT | UAV), 1, 1, SRT, D3D12_HEAP_FLAG_NONE },
        { "committed RGBA8 RT MSAA4",         DXGI_FORMAT_R8G8B8A8_UNORM,      RT, 1, 4, SRT, D3D12_HEAP_FLAG_NONE },
        { "committed RGBA8 no flags",         DXGI_FORMAT_R8G8B8A8_UNORM,      D3D12_RESOURCE_FLAG_NONE, 1, 1, D3D12_RESOURCE_STATE_COMMON, D3D12_HEAP_FLAG_NONE },
        { "committed D32_FLOAT DS",           DXGI_FORMAT_D32_FLOAT,           DS, 1, 1, SDS, D3D12_HEAP_FLAG_NONE },
        { "committed R32_TYPELESS DS arr2",   DXGI_FORMAT_R32_TYPELESS,        DS, 2, 1, SDS, D3D12_HEAP_FLAG_NONE },
    };
    for (const probe_case &c : cases) {
        D3D12_HEAP_PROPERTIES hp = {}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC rd = {};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D; rd.Width = 64; rd.Height = 64;
        rd.DepthOrArraySize = (UINT16)c.arr; rd.MipLevels = 1; rd.Format = c.fmt;
        rd.SampleDesc.Count = c.samples; rd.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN; rd.Flags = c.flags;
        D3D12_CLEAR_VALUE cv = {}; cv.Format = c.fmt == DXGI_FORMAT_R32_TYPELESS ? DXGI_FORMAT_D32_FLOAT : c.fmt;
        cv.DepthStencil.Depth = 1.0f;
        bool clearable = (c.flags & (RT | DS)) && c.fmt != DXGI_FORMAT_R8G8B8A8_TYPELESS && c.fmt != DXGI_FORMAT_B8G8R8A8_TYPELESS;
        struct dmsubst_params p = {}; p.op = DMSUBST_OP_ARM; p.flags = DMSUBST_ARM_PROBE; dms(&p);
        ID3D12Resource *res = nullptr;
        HRESULT hr = dev->CreateCommittedResource(&hp, c.heap_flags, &rd, c.state, clearable ? &cv : nullptr,
                                                  __uuidof(ID3D12Resource), (void **)&res);
        p = {}; p.op = DMSUBST_OP_DISARM; dms(&p);
        probe_report(c.name, hr, p);
        if (res) res->Release();
    }

    /* placed: heap first (armed, to see what the heap is in Metal terms), then a placed RT */
    {
        D3D12_HEAP_DESC hd = {}; hd.SizeInBytes = 16 << 20; hd.Properties.Type = D3D12_HEAP_TYPE_DEFAULT;
        hd.Alignment = D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT; hd.Flags = D3D12_HEAP_FLAG_ALLOW_ONLY_RT_DS_TEXTURES;
        ID3D12Heap *heap = nullptr;
        struct dmsubst_params p = {}; p.op = DMSUBST_OP_ARM; p.flags = DMSUBST_ARM_PROBE; dms(&p);
        HRESULT hr = dev->CreateHeap(&hd, __uuidof(ID3D12Heap), (void **)&heap);
        p = {}; p.op = DMSUBST_OP_DISARM; dms(&p);
        probe_report("CreateHeap 16MB RT/DS", hr, p);
        if (heap) {
            D3D12_RESOURCE_DESC rd = {};
            rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D; rd.Width = 64; rd.Height = 64;
            rd.DepthOrArraySize = 1; rd.MipLevels = 1; rd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            rd.SampleDesc.Count = 1; rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
            ID3D12Resource *res = nullptr;
            p = {}; p.op = DMSUBST_OP_ARM; p.flags = DMSUBST_ARM_PROBE; dms(&p);
            hr = dev->CreatePlacedResource(heap, 0, &rd, D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr,
                                           __uuidof(ID3D12Resource), (void **)&res);
            p = {}; p.op = DMSUBST_OP_DISARM; dms(&p);
            probe_report("placed RGBA8 RT @0", hr, p);
            if (res) res->Release();
            heap->Release();
        }
    }
    stats("after probe");
    return 0;
}

/* ---- fence probe ------------------------------------------------------------ */

static int run_fence_probe(ID3D12Device *dev, ID3D12CommandQueue *q)
{
    struct dmsubst_params p = {}; p.op = DMSUBST_OP_ARM; p.flags = DMSUBST_ARM_PROBE | DMSUBST_ARM_EVENT_SHARED; dms(&p);
    ID3D12Fence *fence = nullptr;
    HRESULT hr = dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, __uuidof(ID3D12Fence), (void **)&fence);
    p = {}; p.op = DMSUBST_OP_DISARM; dms(&p);
    printf("FENCE: CreateFence hr=0x%08lx; events created on this thread while armed=%u via \"%s\" -> MTLSharedEvent 0x%llx\n",
           (unsigned long)hr, p.seen, p.where, (unsigned long long)p.event);
    if (FAILED(hr) || !p.event) return 0;
    uint64_t ev = p.event;
    for (uint64_t v = 1; v <= 3; v++) {
        uint64_t before;
        hr = q->Signal(fence, v);
        { struct dmsubst_params e = {}; e.op = DMSUBST_OP_EVENT_VALUE; e.mtl_texture = ev; dms(&e); before = e.event_value; }
        DWORD t0 = GetTickCount(); uint64_t val = 0;
        do { struct dmsubst_params e = {}; e.op = DMSUBST_OP_EVENT_VALUE; e.mtl_texture = ev; dms(&e); val = e.event_value; }
        while (val < v && GetTickCount() - t0 < 1000);
        printf("FENCE: queue Signal(%llu) hr=0x%08lx; MTLSharedEvent.signaledValue right after=%llu, later=%llu (%lu ms); GetCompletedValue=%llu\n",
               (unsigned long long)v, (unsigned long)hr, (unsigned long long)before, (unsigned long long)val,
               (unsigned long)(GetTickCount() - t0), (unsigned long long)fence->GetCompletedValue());
    }
    fence->Release();
    return 0;
}

/* ---- xr ------------------------------------------------------------------------ */

static const char *g_src =
    "float4 vs(uint id : SV_VertexID) : SV_POSITION {\n"
    "  float2 uv = float2((id << 1) & 2, id & 2);\n"
    "  return float4(uv * float2(2, -2) + float2(-1, 1), 0, 1); }\n"
    "float4 ps() : SV_TARGET { return float4(1, 1, 0, 50.0 / 255.0); }\n";

static int run_xr(HMODULE dll, ID3D12Device *dev, ID3D12CommandQueue *queue, int frames)
{
    auto negotiate = (PFN_xrNegotiateLoaderRuntimeInterface)GetProcAddress(dll, "xrNegotiateLoaderRuntimeInterface");
    XrNegotiateLoaderInfo li = { XR_LOADER_INTERFACE_STRUCT_LOADER_INFO, XR_LOADER_INFO_STRUCT_VERSION, sizeof(li) };
    li.minInterfaceVersion = 1; li.maxInterfaceVersion = XR_CURRENT_LOADER_RUNTIME_VERSION;
    li.minApiVersion = XR_MAKE_VERSION(1,0,0); li.maxApiVersion = XR_MAKE_VERSION(1,0,999);
    XrNegotiateRuntimeRequest req = { XR_LOADER_INTERFACE_STRUCT_RUNTIME_REQUEST, XR_RUNTIME_INFO_STRUCT_VERSION, sizeof(req) };
    if (negotiate(&li, &req) != XR_SUCCESS) { printf("FAIL: negotiate\n"); return 1; }
    gipa = req.getInstanceProcAddr;

    {
        uint32_t n = 0; XrExtensionProperties props[64];
        for (auto &e : props) e = { XR_TYPE_EXTENSION_PROPERTIES };
        fn<PFN_xrEnumerateInstanceExtensionProperties>(XR_NULL_HANDLE, "xrEnumerateInstanceExtensionProperties")
            (nullptr, 64, &n, props);
        bool have = false;
        for (uint32_t i = 0; i < n; i++) have |= !strcmp(props[i].extensionName, XR_KHR_D3D12_ENABLE_EXTENSION_NAME);
        printf("%s: %s advertised (%u extensions)\n", have ? "OK" : "FAIL", XR_KHR_D3D12_ENABLE_EXTENSION_NAME, n);
        if (!have) return 1;
    }
    const char *ext = XR_KHR_D3D12_ENABLE_EXTENSION_NAME;
    XrInstanceCreateInfo ici = { XR_TYPE_INSTANCE_CREATE_INFO };
    ici.enabledExtensionCount = 1; ici.enabledExtensionNames = &ext;
    ici.applicationInfo.apiVersion = XR_MAKE_VERSION(1,0,34);
    snprintf(ici.applicationInfo.applicationName, XR_MAX_APPLICATION_NAME_SIZE, "dmsubst-d3d12-test");
    XrInstance inst;
    CHECK(fn<PFN_xrCreateInstance>(XR_NULL_HANDLE, "xrCreateInstance")(&ici, &inst));
    XrSystemGetInfo sgi = { XR_TYPE_SYSTEM_GET_INFO }; sgi.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    XrSystemId sys;
    CHECK(fn<PFN_xrGetSystem>(inst, "xrGetSystem")(inst, &sgi, &sys));
    XrGraphicsRequirementsD3D12KHR reqs = { XR_TYPE_GRAPHICS_REQUIREMENTS_D3D12_KHR };
    CHECK(fn<PFN_xrGetD3D12GraphicsRequirementsKHR>(inst, "xrGetD3D12GraphicsRequirementsKHR")(inst, sys, &reqs));
    LUID dl; dev->GetAdapterLuid(&dl);
    printf("%s: requirements LUID %08lx:%08lx min FL 0x%x; device adapter LUID %08lx:%08lx\n",
           (reqs.adapterLuid.LowPart == dl.LowPart && reqs.adapterLuid.HighPart == dl.HighPart) ? "OK" : "WARN",
           (unsigned long)reqs.adapterLuid.HighPart, (unsigned long)reqs.adapterLuid.LowPart, reqs.minFeatureLevel,
           (unsigned long)dl.HighPart, (unsigned long)dl.LowPart);

    XrGraphicsBindingD3D12KHR bind = { XR_TYPE_GRAPHICS_BINDING_D3D12_KHR }; bind.device = dev; bind.queue = queue;
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
    const char *want = getenv("DMS_TEST_FORMAT");
    for (uint32_t i = 0; i < nfmt; i++) {
        printf("    format[%u] = %lld\n", i, (long long)fmts[i]);
        if (want && fmts[i] == atoll(want)) fmt = fmts[i];
    }
    const uint32_t W = vcv[0].recommendedImageRectWidth, H = vcv[0].recommendedImageRectHeight;
    const uint32_t arr = getenv("DMS_TEST_ARRAY") ? (uint32_t)atoi(getenv("DMS_TEST_ARRAY")) : 1;
    const int copy_in = getenv("DMS_TEST_COPY") && atoi(getenv("DMS_TEST_COPY"));
    const UINT msaa = getenv("DMS_TEST_MSAA") ? (UINT)atoi(getenv("DMS_TEST_MSAA")) : 1;
    printf("OK: using swapchain format %lld, %ux%u, arraySize %u%s%s\n", (long long)fmt, W, H, arr,
           copy_in ? ", render elsewhere + CopyTextureRegion" : "", msaa > 1 ? ", MSAA + ResolveSubresource" : "");

    /* RTV heap: [eye][image] swapchain RTVs, then [eye] intermediate RTVs */
    ID3D12DescriptorHeap *rtv_heap = nullptr;
    D3D12_DESCRIPTOR_HEAP_DESC dhd = {}; dhd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV; dhd.NumDescriptors = 32;
    if (FAILED(dev->CreateDescriptorHeap(&dhd, __uuidof(ID3D12DescriptorHeap), (void **)&rtv_heap))) { printf("FAIL: rtv heap\n"); return 1; }
    const UINT rtv_inc = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    D3D12_CPU_DESCRIPTOR_HANDLE rtv_base = rtv_heap->GetCPUDescriptorHandleForHeapStart();
    auto rtv_at = [&](UINT i) { D3D12_CPU_DESCRIPTOR_HANDLE h = rtv_base; h.ptr += (SIZE_T)i * rtv_inc; return h; };

    XrSwapchain sc[2]; ID3D12Resource *img[2][8] = {}; uint32_t nimg[2] = {};
    for (int eye = 0; eye < 2; eye++) {
        XrSwapchainCreateInfo scci = { XR_TYPE_SWAPCHAIN_CREATE_INFO };
        scci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT |
                          (copy_in ? XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT : 0);
        scci.format = fmt; scci.sampleCount = 1; scci.width = W; scci.height = H;
        scci.faceCount = 1; scci.arraySize = arr; scci.mipCount = 1;
        CHECK(fn<PFN_xrCreateSwapchain>(inst, "xrCreateSwapchain")(session, &scci, &sc[eye]));
        XrSwapchainImageD3D12KHR imgs[8];
        for (int i = 0; i < 8; i++) imgs[i] = { XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR };
        CHECK(fn<PFN_xrEnumerateSwapchainImages>(inst, "xrEnumerateSwapchainImages")
              (sc[eye], 8, &nimg[eye], (XrSwapchainImageBaseHeader *)imgs));
        for (uint32_t i = 0; i < nimg[eye]; i++) {
            img[eye][i] = imgs[i].texture;
            D3D12_RESOURCE_DESC rd = img[eye][i]->GetDesc();
            D3D12_RENDER_TARGET_VIEW_DESC rv = {};
            rv.Format = (DXGI_FORMAT)fmt; rv.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
            if (arr > 1) {
                rv.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DARRAY;
                rv.Texture2DArray.FirstArraySlice = eye; rv.Texture2DArray.ArraySize = 1;
            }
            dev->CreateRenderTargetView(img[eye][i], &rv, rtv_at(eye * 8 + i));
            printf("    eye %d image %u: ID3D12Resource %p (desc fmt %d %llux%u arr %u flags 0x%x) runtime MTLTexture 0x%llx\n",
                   eye, i, (void *)img[eye][i], (int)rd.Format, (unsigned long long)rd.Width, rd.Height,
                   rd.DepthOrArraySize, (unsigned)rd.Flags, (unsigned long long)sc_tex(sc[eye], i));
        }
    }
    stats("after swapchain images");

    /* Intermediate per-eye targets for the copy / MSAA-resolve variants */
    ID3D12Resource *mid[2] = {};
    if (copy_in || msaa > 1) {
        for (int eye = 0; eye < 2; eye++) {
            D3D12_HEAP_PROPERTIES hp = {}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
            D3D12_RESOURCE_DESC rd = {};
            rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D; rd.Width = W; rd.Height = H;
            rd.DepthOrArraySize = 1; rd.MipLevels = 1; rd.Format = (DXGI_FORMAT)fmt;
            rd.SampleDesc.Count = msaa; rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
            if (FAILED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_RENDER_TARGET,
                                                    nullptr, __uuidof(ID3D12Resource), (void **)&mid[eye]))) {
                printf("FAIL: intermediate target\n"); return 1;
            }
            D3D12_RENDER_TARGET_VIEW_DESC rv = {}; rv.Format = (DXGI_FORMAT)fmt;
            rv.ViewDimension = msaa > 1 ? D3D12_RTV_DIMENSION_TEXTURE2DMS : D3D12_RTV_DIMENSION_TEXTURE2D;
            dev->CreateRenderTargetView(mid[eye], &rv, rtv_at(16 + eye));
        }
    }

    /* PSO: fullscreen triangle, constant colour, no root parameters */
    ID3DBlob *vsb = nullptr, *psb = nullptr, *err = nullptr, *rsb = nullptr;
    if (FAILED(D3DCompile(g_src, strlen(g_src), nullptr, nullptr, nullptr, "vs", "vs_5_0", 0, 0, &vsb, &err)) ||
        FAILED(D3DCompile(g_src, strlen(g_src), nullptr, nullptr, nullptr, "ps", "ps_5_0", 0, 0, &psb, &err))) {
        printf("FAIL: shader compile %s\n", err ? (char *)err->GetBufferPointer() : "?"); return 1;
    }
    D3D12_ROOT_SIGNATURE_DESC rsd = {};
    ID3D12RootSignature *rootsig = nullptr;
    if (FAILED(D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &rsb, &err)) ||
        FAILED(dev->CreateRootSignature(0, rsb->GetBufferPointer(), rsb->GetBufferSize(), __uuidof(ID3D12RootSignature), (void **)&rootsig))) {
        printf("FAIL: root signature\n"); return 1;
    }
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pd = {};
    pd.pRootSignature = rootsig;
    pd.VS = { vsb->GetBufferPointer(), vsb->GetBufferSize() };
    pd.PS = { psb->GetBufferPointer(), psb->GetBufferSize() };
    pd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    pd.SampleMask = UINT_MAX;
    pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID; pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pd.RasterizerState.DepthClipEnable = TRUE;
    pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pd.NumRenderTargets = 1; pd.RTVFormats[0] = (DXGI_FORMAT)fmt; pd.SampleDesc.Count = msaa;
    ID3D12PipelineState *pso = nullptr;
    HRESULT hr = dev->CreateGraphicsPipelineState(&pd, __uuidof(ID3D12PipelineState), (void **)&pso);
    if (FAILED(hr)) { printf("FAIL: CreateGraphicsPipelineState 0x%08lx\n", (unsigned long)hr); return 1; }

    const int NA = 3;
    ID3D12CommandAllocator *alloc[NA] = {}; UINT64 alloc_done[NA] = {};
    ID3D12GraphicsCommandList *cl = nullptr;
    for (int i = 0; i < NA; i++)
        dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, __uuidof(ID3D12CommandAllocator), (void **)&alloc[i]);
    dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc[0], nullptr, __uuidof(ID3D12GraphicsCommandList), (void **)&cl);
    cl->Close();
    gpu_sync gs; gs.init(dev);

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

    const float drawn[4] = { 1, 1, 0, 50.0f / 255.0f };
    int synccheck = getenv("DMS_TEST_SYNCCHECK") ? atoi(getenv("DMS_TEST_SYNCCHECK")) : 0;
    int sync_ok = 0, sync_bad = 0;
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
            const int a = (f * 2 + eye) % NA;
            if (!gs.wait(alloc_done[a])) { printf("FAIL: allocator fence timeout\n"); return 1; }
            alloc[a]->Reset(); cl->Reset(alloc[a], pso);

            uint32_t idx = 0;
            XrSwapchainImageAcquireInfo ai = { XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO };
            acquire(sc[eye], &ai, &idx);
            XrSwapchainImageWaitInfo wi = { XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO }; wi.timeout = XR_INFINITE_DURATION;
            wait(sc[eye], &wi);
            float c[4] = { (float)((f + eye) & 1), (float)(((f + eye) >> 1) & 1), (float)(eye ^ 1),
                           (float)((f % 16) * 17) / 255.0f };
            const bool indirect = copy_in || msaa > 1;
            D3D12_CPU_DESCRIPTOR_HANDLE target = indirect ? rtv_at(16 + eye) : rtv_at(eye * 8 + idx);
            cl->ClearRenderTargetView(target, c, 0, nullptr);
            D3D12_VIEWPORT vp = { 0, 0, (FLOAT)W, (FLOAT)H, 0, 1 };
            D3D12_RECT sr = { 0, 0, (LONG)(W / 2), (LONG)(H / 2) };
            cl->OMSetRenderTargets(1, &target, FALSE, nullptr);
            cl->RSSetViewports(1, &vp); cl->RSSetScissorRects(1, &sr);
            cl->SetGraphicsRootSignature(rootsig);
            cl->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            cl->DrawInstanced(3, 1, 0, 0);
            /* OpenXR D3D12: colour images are in RENDER_TARGET at acquire and
             * must be back in RENDER_TARGET at release */
            if (indirect) {
                const UINT dst_sub = arr > 1 ? (UINT)eye : 0; /* mip 0 of slice eye */
                if (msaa > 1) {
                    barrier(cl, mid[eye], D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_RESOLVE_SOURCE);
                    barrier(cl, img[eye][idx], D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_RESOLVE_DEST, dst_sub);
                    cl->ResolveSubresource(img[eye][idx], dst_sub, mid[eye], 0, (DXGI_FORMAT)fmt);
                    barrier(cl, img[eye][idx], D3D12_RESOURCE_STATE_RESOLVE_DEST, D3D12_RESOURCE_STATE_RENDER_TARGET, dst_sub);
                    barrier(cl, mid[eye], D3D12_RESOURCE_STATE_RESOLVE_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
                } else {
                    barrier(cl, mid[eye], D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
                    barrier(cl, img[eye][idx], D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_DEST, dst_sub);
                    D3D12_TEXTURE_COPY_LOCATION dl2 = {}, sl = {};
                    dl2.pResource = img[eye][idx]; dl2.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; dl2.SubresourceIndex = dst_sub;
                    sl.pResource = mid[eye]; sl.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; sl.SubresourceIndex = 0;
                    cl->CopyTextureRegion(&dl2, 0, 0, 0, &sl, nullptr);
                    barrier(cl, img[eye][idx], D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_RENDER_TARGET, dst_sub);
                    barrier(cl, mid[eye], D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
                }
            }
            cl->Close();
            ID3D12CommandList *lists[] = { cl };
            queue->ExecuteCommandLists(1, lists);
            alloc_done[a] = gs.signal(queue);
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
            pv[eye].subImage.imageArrayIndex = arr > 1 ? eye : 0;
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
        printf("SYNCCHECK(%s): %d/%d immediate post-release readbacks correct\n",
               synccheck == 1 ? "readback queue GPU-waits on the release event" : "control: no wait",
               sync_ok, sync_ok + sync_bad);
    printf("OK: %d frames in %lu ms (%.2f ms/frame)\n", frames, (unsigned long)ms, frames ? (double)ms / frames : 0.0);
    if (!gs.idle(queue)) printf("WARN: final GPU idle wait timed out\n");

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
                /* MSAA resolve of a solid colour is exact; the triangle edge is far from the probe points */
                for (int k = 0; k < 4; k++) if (fabsf(p.rgba[k] - e[k]) > 1.5f / 255.0f) ok = 0;
                printf("READBACK %s eye %d image %u slice %u (MTL fmt %u): raw %02x %02x %02x %02x -> rgba(%.3f %.3f %.3f %.3f), "
                       "expected (%.3f %.3f %.3f %.3f) %s\n", pt ? "drawn  (W/4,H/4)" : "cleared(W/2,H/2)", eye, i, p.slice,
                       p.tex_pixel_format, p.raw[0], p.raw[1], p.raw[2], p.raw[3], p.rgba[0], p.rgba[1], p.rgba[2], p.rgba[3],
                       e[0], e[1], e[2], e[3], ok ? "MATCH" : "MISMATCH");
                checked++; if (!ok) bad++;
            }
        }
    stats("after frames");
    printf("%s: %d/%d texels match\n", bad ? "FAIL" : "PASS", checked - bad, checked);

    for (int i = 0; i < NA; i++) alloc[i]->Release();
    cl->Release(); pso->Release(); rootsig->Release(); rtv_heap->Release();
    for (int eye = 0; eye < 2; eye++) if (mid[eye]) mid[eye]->Release();
    CHECK(fn<PFN_xrDestroySwapchain>(inst, "xrDestroySwapchain")(sc[0]));
    CHECK(fn<PFN_xrDestroySwapchain>(inst, "xrDestroySwapchain")(sc[1]));
    CHECK(fn<PFN_xrDestroySession>(inst, "xrDestroySession")(session));
    CHECK(fn<PFN_xrDestroyInstance>(inst, "xrDestroyInstance")(inst));
    return bad ? 1 : 0;
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

    struct dmsubst_params p = {}; p.op = DMSUBST_OP_INSTALL; dms(&p);
    printf("INSTALL (before D3D12CreateDevice): detect 0x%x\n", p.detect_flags);

    IDXGIFactory1 *factory = nullptr;
    if (SUCCEEDED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void **)&factory))) {
        IDXGIAdapter1 *ad = nullptr;
        for (UINT i = 0; factory->EnumAdapters1(i, &ad) == S_OK; i++) {
            DXGI_ADAPTER_DESC1 d; ad->GetDesc1(&d);
            printf("DXGI adapter %u: %ls vendor 0x%04x device 0x%04x LUID %08lx:%08lx flags 0x%x\n", i, d.Description,
                   d.VendorId, d.DeviceId, (unsigned long)d.AdapterLuid.HighPart, (unsigned long)d.AdapterLuid.LowPart,
                   d.Flags);
            ad->Release();
        }
        factory->Release();
    }

    ID3D12Device *dev = nullptr;
    HRESULT hr = D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, __uuidof(ID3D12Device), (void **)&dev);
    if (FAILED(hr)) { printf("FAIL: D3D12CreateDevice hr=0x%08lx\n", (unsigned long)hr); return 1; }
    ID3D12CommandQueue *queue = nullptr;
    D3D12_COMMAND_QUEUE_DESC qd = {}; qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    hr = dev->CreateCommandQueue(&qd, __uuidof(ID3D12CommandQueue), (void **)&queue);
    if (FAILED(hr)) { printf("FAIL: CreateCommandQueue hr=0x%08lx\n", (unsigned long)hr); return 1; }
    p = {}; p.op = DMSUBST_OP_DETECT; dms(&p);
    printf("OK: D3D12 device %p queue %p; DETECT: 0x%x (D3DMetal=%d DXMT=%d MTL4 queue seen=%d)\n", (void *)dev,
           (void *)queue, p.detect_flags, !!(p.detect_flags & DMSUBST_DETECT_D3DMETAL),
           !!(p.detect_flags & DMSUBST_DETECT_DXMT), !!(p.detect_flags & DMSUBST_DETECT_MTL4));
    stats("after device+queue");

    int rc = 0;
    if (!strcmp(mode, "probe") || !strcmp(mode, "both")) rc |= run_probe(dev);
    if (!strcmp(mode, "fence") || !strcmp(mode, "both")) rc |= run_fence_probe(dev, queue);
    if (!rc && (!strcmp(mode, "xr") || !strcmp(mode, "both"))) rc |= run_xr(dll, dev, queue, frames);
    queue->Release();
    dev->Release();
    printf("EXIT rc=%d\n", rc);
    return rc;
}
