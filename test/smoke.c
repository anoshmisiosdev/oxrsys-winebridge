/* Headless bridge smoke test: loads wineopenxr.dll as the OpenXR runtime,
 * negotiates, creates an instance, queries system + extensions.
 * Exercises PE -> __wine_unix_call -> native loader -> OXRSys without graphics. */
#include <windows.h>
#include <stdio.h>
#define XR_USE_PLATFORM_WIN32 1
#include "openxr/openxr.h"
#include "openxr/openxr_loader_negotiation.h"

int main(void)
{
    HMODULE dll = LoadLibraryA("wineopenxr.dll");
    if (!dll) { printf("FAIL: LoadLibrary wineopenxr.dll (err %lu)\n", GetLastError()); return 1; }
    printf("OK: wineopenxr.dll loaded\n");

    PFN_xrNegotiateLoaderRuntimeInterface negotiate =
        (PFN_xrNegotiateLoaderRuntimeInterface)GetProcAddress(dll, "xrNegotiateLoaderRuntimeInterface");
    if (!negotiate) { printf("FAIL: no xrNegotiateLoaderRuntimeInterface export\n"); return 1; }

    XrNegotiateLoaderInfo li = { XR_LOADER_INTERFACE_STRUCT_LOADER_INFO, XR_LOADER_INFO_STRUCT_VERSION, sizeof(li) };
    li.minInterfaceVersion = 1; li.maxInterfaceVersion = XR_CURRENT_LOADER_RUNTIME_VERSION;
    li.minApiVersion = XR_MAKE_VERSION(1, 0, 0); li.maxApiVersion = XR_MAKE_VERSION(1, 0, 999);
    XrNegotiateRuntimeRequest req = { XR_LOADER_INTERFACE_STRUCT_RUNTIME_REQUEST, XR_RUNTIME_INFO_STRUCT_VERSION, sizeof(req) };
    XrResult r = negotiate(&li, &req);
    printf("%s: negotiate -> %d (runtime api %llu.%llu)\n", r == XR_SUCCESS ? "OK" : "FAIL", r,
           (unsigned long long)XR_VERSION_MAJOR(req.runtimeApiVersion),
           (unsigned long long)XR_VERSION_MINOR(req.runtimeApiVersion));
    if (r != XR_SUCCESS) return 1;

    PFN_xrGetInstanceProcAddr gipa = req.getInstanceProcAddr;
    PFN_xrEnumerateInstanceExtensionProperties enumExt;
    gipa(XR_NULL_HANDLE, "xrEnumerateInstanceExtensionProperties", (PFN_xrVoidFunction *)&enumExt);
    uint32_t n = 0;
    enumExt(NULL, 0, &n, NULL);
    printf("OK: %u instance extensions:\n", n);
    XrExtensionProperties props[64];
    for (uint32_t i = 0; i < n && i < 64; i++) { props[i].type = XR_TYPE_EXTENSION_PROPERTIES; props[i].next = NULL; }
    if (n > 64) n = 64;
    enumExt(NULL, n, &n, props);
    for (uint32_t i = 0; i < n; i++) printf("    %s\n", props[i].extensionName);

    PFN_xrCreateInstance createInstance;
    gipa(XR_NULL_HANDLE, "xrCreateInstance", (PFN_xrVoidFunction *)&createInstance);
    XrInstanceCreateInfo ci = { XR_TYPE_INSTANCE_CREATE_INFO };
    ci.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 0, 34);
    snprintf(ci.applicationInfo.applicationName, XR_MAX_APPLICATION_NAME_SIZE, "smoke");
    ci.applicationInfo.applicationVersion = 1;
    XrInstance inst;
    r = createInstance(&ci, &inst);
    printf("%s: xrCreateInstance -> %d\n", r == XR_SUCCESS ? "OK" : "FAIL", r);
    if (r != XR_SUCCESS) return 1;

    PFN_xrGetInstanceProperties getProps; PFN_xrGetSystem getSystem; PFN_xrDestroyInstance destroy;
    gipa(inst, "xrGetInstanceProperties", (PFN_xrVoidFunction *)&getProps);
    gipa(inst, "xrGetSystem", (PFN_xrVoidFunction *)&getSystem);
    gipa(inst, "xrDestroyInstance", (PFN_xrVoidFunction *)&destroy);

    XrInstanceProperties ip = { XR_TYPE_INSTANCE_PROPERTIES };
    getProps(inst, &ip);
    printf("OK: runtime = '%s' %llu.%llu.%llu\n", ip.runtimeName,
           (unsigned long long)XR_VERSION_MAJOR(ip.runtimeVersion),
           (unsigned long long)XR_VERSION_MINOR(ip.runtimeVersion),
           (unsigned long long)XR_VERSION_PATCH(ip.runtimeVersion));

    XrSystemGetInfo sgi = { XR_TYPE_SYSTEM_GET_INFO };
    sgi.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    XrSystemId sys;
    r = getSystem(inst, &sgi, &sys);
    printf("%s: xrGetSystem -> %d (systemId %llu)\n", r == XR_SUCCESS ? "OK" : "note", r, (unsigned long long)sys);

    destroy(inst);
    printf("PASS: bridge chain PE -> unixlib -> OXRSys is alive\n");
    return 0;
}
