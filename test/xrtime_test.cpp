/* Exercises XR_KHR_win32_convert_performance_counter_time end to end:
 *   - requests it at xrCreateInstance (must be ACCEPTED - this used to fail
 *     with XR_ERROR_EXTENSION_NOT_PRESENT because apply_substitutions()
 *     never advertised it, since OXRSys never reports the native
 *     XR_KHR_convert_timespec_time it used to be gated on)
 *   - confirms it shows up in xrEnumerateInstanceExtensionProperties
 *   - runs a couple of real frames to get a genuine host-produced XrTime
 *     (XrFrameState.predictedDisplayTime)
 *   - converts that XrTime -> QPC -> XrTime and checks the roundtrip lands
 *     within 1 QPC tick (100ns) of the original, and that the QPC value is
 *     in the same ballpark as a real QueryPerformanceCounter() reading
 *     taken at the same moment (same clock domain sanity check)
 */
#include <windows.h>
#include <d3d11.h>
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
    setvbuf(stdout, nullptr, _IONBF, 0);
    HMODULE dll = LoadLibraryA("wineopenxr.dll");
    if (!dll) { printf("FAIL: LoadLibrary (%lu)\n", GetLastError()); return 1; }
    auto negotiate = (PFN_xrNegotiateLoaderRuntimeInterface)GetProcAddress(dll, "xrNegotiateLoaderRuntimeInterface");
    XrNegotiateLoaderInfo li = { XR_LOADER_INTERFACE_STRUCT_LOADER_INFO, XR_LOADER_INFO_STRUCT_VERSION, sizeof(li) };
    li.minInterfaceVersion = 1; li.maxInterfaceVersion = XR_CURRENT_LOADER_RUNTIME_VERSION;
    li.minApiVersion = XR_MAKE_VERSION(1,0,0); li.maxApiVersion = XR_MAKE_VERSION(1,0,999);
    XrNegotiateRuntimeRequest req = { XR_LOADER_INTERFACE_STRUCT_RUNTIME_REQUEST, XR_RUNTIME_INFO_STRUCT_VERSION, sizeof(req) };
    if (negotiate(&li, &req) != XR_SUCCESS) { printf("FAIL: negotiate\n"); return 1; }
    gipa = req.getInstanceProcAddr;

    /* Confirm the extension is now unconditionally enumerated */
    {
        auto enumExt = fn<PFN_xrEnumerateInstanceExtensionProperties>(XR_NULL_HANDLE, "xrEnumerateInstanceExtensionProperties");
        if (!enumExt) { printf("FAIL: could not resolve xrEnumerateInstanceExtensionProperties\n"); return 1; }
        uint32_t count = 0;
        enumExt(nullptr, 0, &count, nullptr);
        XrExtensionProperties *props = new XrExtensionProperties[count];
        for (uint32_t i = 0; i < count; i++) props[i] = { XR_TYPE_EXTENSION_PROPERTIES };
        enumExt(nullptr, count, &count, props);
        bool found = false;
        for (uint32_t i = 0; i < count; i++) {
            printf("  enumerated: %s\n", props[i].extensionName);
            if (!strcmp(props[i].extensionName, "XR_KHR_win32_convert_performance_counter_time"))
                found = true;
        }
        delete[] props;
        if (!found) { printf("FAIL: XR_KHR_win32_convert_performance_counter_time not enumerated\n"); return 1; }
        printf("OK: XR_KHR_win32_convert_performance_counter_time is enumerated\n");
    }

    const char *exts[2] = { XR_KHR_D3D11_ENABLE_EXTENSION_NAME, "XR_KHR_win32_convert_performance_counter_time" };
    XrInstanceCreateInfo ici = { XR_TYPE_INSTANCE_CREATE_INFO };
    ici.enabledExtensionCount = 2; ici.enabledExtensionNames = exts;
    ici.applicationInfo.apiVersion = XR_MAKE_VERSION(1,0,34);
    snprintf(ici.applicationInfo.applicationName, XR_MAX_APPLICATION_NAME_SIZE, "xrtime_test");
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

    XrGraphicsBindingD3D11KHR bind = { XR_TYPE_GRAPHICS_BINDING_D3D11_KHR }; bind.device = dev;
    XrSessionCreateInfo sci = { XR_TYPE_SESSION_CREATE_INFO }; sci.next = &bind; sci.systemId = sys;
    XrSession session;
    CHECK(fn<PFN_xrCreateSession>(inst, "xrCreateSession")(inst, &sci, &session));

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
    auto toQpc = (PFN_xrConvertTimeToWin32PerformanceCounterKHR)0;
    auto toTime = (PFN_xrConvertWin32PerformanceCounterToTimeKHR)0;
    gipa(inst, "xrConvertTimeToWin32PerformanceCounterKHR", (PFN_xrVoidFunction*)&toQpc);
    gipa(inst, "xrConvertWin32PerformanceCounterToTimeKHR", (PFN_xrVoidFunction*)&toTime);
    if (!toQpc || !toTime) { printf("FAIL: could not resolve conversion functions\n"); return 1; }

    int pass = 0, fail = 0;
    for (int f = 0; f < 5; f++) {
        XrFrameState fs = { XR_TYPE_FRAME_STATE };
        XrFrameWaitInfo fwi = { XR_TYPE_FRAME_WAIT_INFO };
        if (waitFrame(session, &fwi, &fs) != XR_SUCCESS) break;
        XrFrameBeginInfo fbi = { XR_TYPE_FRAME_BEGIN_INFO };
        beginFrame(session, &fbi);

        LARGE_INTEGER qpc = {};
        XrResult r1 = toQpc(inst, fs.predictedDisplayTime, &qpc);
        LARGE_INTEGER realQpc; QueryPerformanceCounter(&realQpc);
        XrTime roundtrip = 0;
        XrResult r2 = toTime(inst, &qpc, &roundtrip);

        long long delta = roundtrip - fs.predictedDisplayTime;
        long long realDeltaTicks = qpc.QuadPart - realQpc.QuadPart;
        bool ok = (r1 == XR_SUCCESS) && (r2 == XR_SUCCESS) &&
                  (delta >= -100 && delta <= 100) &&                 /* 1 QPC tick = 100ns */
                  (realDeltaTicks > -100000000LL && realDeltaTicks < 100000000LL); /* within ~10s of real QPC, same clock domain */
        printf("frame %d: predictedDisplayTime=%lld -> QPC=%lld (real QPC=%lld, delta_ticks=%lld) -> roundtrip=%lld (delta_ns=%lld) %s\n",
               f, (long long)fs.predictedDisplayTime, (long long)qpc.QuadPart, (long long)realQpc.QuadPart,
               realDeltaTicks, (long long)roundtrip, delta, ok ? "OK" : "FAIL");
        if (ok) pass++; else fail++;

        XrFrameEndInfo fei = { XR_TYPE_FRAME_END_INFO };
        fei.displayTime = fs.predictedDisplayTime;
        fei.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
        fei.layerCount = 0; fei.layers = nullptr;
        if (endFrame(session, &fei) != XR_SUCCESS) break;
    }

    /* Negative-input validation, per the Monado-compatibility comment */
    {
        LARGE_INTEGER bad; bad.QuadPart = 0;
        XrTime t;
        XrResult r = toTime(inst, &bad, &t);
        printf("%s: toTime(QuadPart=0) -> %d (expect XR_ERROR_TIME_INVALID=%d)\n",
               r == XR_ERROR_TIME_INVALID ? "OK" : "FAIL", r, XR_ERROR_TIME_INVALID);
        if (r != XR_ERROR_TIME_INVALID) fail++; else pass++;
    }

    printf("%s: xrtime_test (%d ok, %d fail)\n", fail == 0 ? "PASS" : "FAIL", pass, fail);
    return fail == 0 ? 0 : 1;
}
