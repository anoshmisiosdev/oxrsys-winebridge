/* Controller/hand pose action-state diagnostic.
 *
 * Creates a minimal D3D11 OpenXR session through wineopenxr.dll (real
 * loader-negotiation path, same as d3d11_full_test.cpp), then:
 *   - creates a pose action with subaction paths for /user/hand/left and
 *     /user/hand/right
 *   - suggests bindings for it under BOTH /interaction_profiles/khr/simple_controller
 *     (grip/pose) and /interaction_profiles/oculus/touch_controller (grip/pose),
 *     to see whether OXRSys treats the real connected Quest as a
 *     simple_controller or a touch_controller (or neither)
 *   - attaches the action set, creates action spaces for both hands
 *   - every frame calls xrSyncActions + xrGetActionStatePose +
 *     xrLocateSpace for both hands, and xrGetCurrentInteractionProfile,
 *     logging isActive, locationFlags, position, and the resolved
 *     interaction profile path string to stdout.
 *
 * This gives objective log-readable evidence of whether/when isActive
 * ever flips true and which interaction profile (if any) OXRSys reports
 * as currently bound to the real headset's controllers/hands.
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
#define SOFT(expr) do { XrResult _r = (expr); \
    printf("%s: %s -> %d (line %d)\n", _r == XR_SUCCESS ? "OK" : "WARN", #expr, _r, __LINE__); } while (0)

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

    const char *ext = XR_KHR_D3D11_ENABLE_EXTENSION_NAME;
    XrInstanceCreateInfo ici = { XR_TYPE_INSTANCE_CREATE_INFO };
    ici.enabledExtensionCount = 1; ici.enabledExtensionNames = &ext;
    ici.applicationInfo.apiVersion = XR_MAKE_VERSION(1,0,34);
    snprintf(ici.applicationInfo.applicationName, XR_MAX_APPLICATION_NAME_SIZE, "action_state_test");
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

    auto stringToPath = fn<PFN_xrStringToPath>(inst, "xrStringToPath");
    auto pathToString = fn<PFN_xrPathToString>(inst, "xrPathToString");
    XrPath leftHandPath, rightHandPath;
    CHECK(stringToPath(inst, "/user/hand/left", &leftHandPath));
    CHECK(stringToPath(inst, "/user/hand/right", &rightHandPath));

    /* action set + pose action */
    XrActionSetCreateInfo asci = { XR_TYPE_ACTION_SET_CREATE_INFO };
    snprintf(asci.actionSetName, XR_MAX_ACTION_SET_NAME_SIZE, "gameplay");
    snprintf(asci.localizedActionSetName, XR_MAX_LOCALIZED_ACTION_SET_NAME_SIZE, "Gameplay");
    XrActionSet actionSet;
    CHECK(fn<PFN_xrCreateActionSet>(inst, "xrCreateActionSet")(inst, &asci, &actionSet));

    XrPath subactionPaths[2] = { leftHandPath, rightHandPath };
    XrActionCreateInfo aci = { XR_TYPE_ACTION_CREATE_INFO };
    aci.actionType = XR_ACTION_TYPE_POSE_INPUT;
    snprintf(aci.actionName, XR_MAX_ACTION_NAME_SIZE, "handpose");
    snprintf(aci.localizedActionName, XR_MAX_LOCALIZED_ACTION_NAME_SIZE, "Hand Pose");
    aci.countSubactionPaths = 2; aci.subactionPaths = subactionPaths;
    XrAction poseAction;
    CHECK(fn<PFN_xrCreateAction>(inst, "xrCreateAction")(actionSet, &aci, &poseAction));

    auto suggest = fn<PFN_xrSuggestInteractionProfileBindings>(inst, "xrSuggestInteractionProfileBindings");

    /* simple_controller: grip/pose */
    {
        XrPath profile, leftGrip, rightGrip;
        CHECK(stringToPath(inst, "/interaction_profiles/khr/simple_controller", &profile));
        CHECK(stringToPath(inst, "/user/hand/left/input/grip/pose", &leftGrip));
        CHECK(stringToPath(inst, "/user/hand/right/input/grip/pose", &rightGrip));
        XrActionSuggestedBinding bindings[2] = { { poseAction, leftGrip }, { poseAction, rightGrip } };
        XrInteractionProfileSuggestedBinding sb = { XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING };
        sb.interactionProfile = profile; sb.countSuggestedBindings = 2; sb.suggestedBindings = bindings;
        SOFT(suggest(inst, &sb));
    }
    /* oculus/touch_controller: grip/pose */
    {
        XrPath profile, leftGrip, rightGrip;
        CHECK(stringToPath(inst, "/interaction_profiles/oculus/touch_controller", &profile));
        CHECK(stringToPath(inst, "/user/hand/left/input/grip/pose", &leftGrip));
        CHECK(stringToPath(inst, "/user/hand/right/input/grip/pose", &rightGrip));
        XrActionSuggestedBinding bindings[2] = { { poseAction, leftGrip }, { poseAction, rightGrip } };
        XrInteractionProfileSuggestedBinding sb = { XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING };
        sb.interactionProfile = profile; sb.countSuggestedBindings = 2; sb.suggestedBindings = bindings;
        SOFT(suggest(inst, &sb));
    }

    XrGraphicsBindingD3D11KHR bind = { XR_TYPE_GRAPHICS_BINDING_D3D11_KHR }; bind.device = dev;
    XrSessionCreateInfo sci = { XR_TYPE_SESSION_CREATE_INFO }; sci.next = &bind; sci.systemId = sys;
    XrSession session;
    CHECK(fn<PFN_xrCreateSession>(inst, "xrCreateSession")(inst, &sci, &session));

    XrReferenceSpaceCreateInfo rsci = { XR_TYPE_REFERENCE_SPACE_CREATE_INFO };
    rsci.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
    rsci.poseInReferenceSpace.orientation.w = 1.0f;
    XrSpace refSpace;
    CHECK(fn<PFN_xrCreateReferenceSpace>(inst, "xrCreateReferenceSpace")(session, &rsci, &refSpace));

    XrSessionActionSetsAttachInfo attachInfo = { XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO };
    attachInfo.countActionSets = 1; attachInfo.actionSets = &actionSet;
    CHECK(fn<PFN_xrAttachSessionActionSets>(inst, "xrAttachSessionActionSets")(session, &attachInfo));

    auto createActionSpace = fn<PFN_xrCreateActionSpace>(inst, "xrCreateActionSpace");
    XrSpace leftSpace, rightSpace;
    {
        XrActionSpaceCreateInfo ascInfo = { XR_TYPE_ACTION_SPACE_CREATE_INFO };
        ascInfo.action = poseAction; ascInfo.subactionPath = leftHandPath;
        ascInfo.poseInActionSpace.orientation.w = 1.0f;
        CHECK(createActionSpace(session, &ascInfo, &leftSpace));
        ascInfo.subactionPath = rightHandPath;
        CHECK(createActionSpace(session, &ascInfo, &rightSpace));
    }

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
        for (uint32_t i = 0; i < nimg[eye]; i++) {
            tex[eye][i] = imgs[i].texture;
            dev->CreateRenderTargetView((ID3D11Resource *)tex[eye][i], nullptr, &rtv[eye][i]);
        }
    }

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
    auto waitImg = fn<PFN_xrWaitSwapchainImage>(inst, "xrWaitSwapchainImage");
    auto release = fn<PFN_xrReleaseSwapchainImage>(inst, "xrReleaseSwapchainImage");
    auto syncActions = fn<PFN_xrSyncActions>(inst, "xrSyncActions");
    auto getPoseState = fn<PFN_xrGetActionStatePose>(inst, "xrGetActionStatePose");
    auto locateSpace = fn<PFN_xrLocateSpace>(inst, "xrLocateSpace");
    auto getCurProfile = fn<PFN_xrGetCurrentInteractionProfile>(inst, "xrGetCurrentInteractionProfile");

    for (int f = 0; f < 1800; f++) {   /* ~20-30s depending on refresh */
        XrFrameState fs = { XR_TYPE_FRAME_STATE };
        XrFrameWaitInfo fwi = { XR_TYPE_FRAME_WAIT_INFO };
        if (waitFrame(session, &fwi, &fs) != XR_SUCCESS) break;
        XrFrameBeginInfo fbi = { XR_TYPE_FRAME_BEGIN_INFO };
        beginFrame(session, &fbi);

        XrViewLocateInfo vli = { XR_TYPE_VIEW_LOCATE_INFO };
        vli.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
        vli.displayTime = fs.predictedDisplayTime; vli.space = refSpace;
        XrViewState vst = { XR_TYPE_VIEW_STATE };
        XrView views[2] = { { XR_TYPE_VIEW }, { XR_TYPE_VIEW } };
        uint32_t nv = 0;
        locateViews(session, &vli, &vst, 2, &nv, views);

        for (int eye = 0; eye < 2; eye++) {
            uint32_t idx = 0;
            XrSwapchainImageAcquireInfo ai = { XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO };
            acquire(sc[eye], &ai, &idx);
            XrSwapchainImageWaitInfo wi = { XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO }; wi.timeout = XR_INFINITE_DURATION;
            waitImg(sc[eye], &wi);
            float clear[4] = { 0, 0, 0, 1 };
            ctx->ClearRenderTargetView(rtv[eye][idx % nimg[eye]], clear);
            XrSwapchainImageReleaseInfo ri = { XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
            release(sc[eye], &ri);
        }

        /* --- action state diagnostics --- */
        XrActiveActionSet active = { actionSet, XR_NULL_PATH };
        XrActionsSyncInfo syncInfo = { XR_TYPE_ACTIONS_SYNC_INFO };
        syncInfo.countActiveActionSets = 1; syncInfo.activeActionSets = &active;
        XrResult syncRes = syncActions(session, &syncInfo);

        bool doPrint = (f < 30) || (f % 60 == 0);
        if (doPrint) printf("frame %d: xrSyncActions -> %d\n", f, syncRes);

        for (int h = 0; h < 2; h++) {
            XrPath hp = h == 0 ? leftHandPath : rightHandPath;
            const char *hname = h == 0 ? "left" : "right";
            XrSpace hspace = h == 0 ? leftSpace : rightSpace;

            XrActionStateGetInfo gi = { XR_TYPE_ACTION_STATE_GET_INFO };
            gi.action = poseAction; gi.subactionPath = hp;
            XrActionStatePose ps = { XR_TYPE_ACTION_STATE_POSE };
            XrResult gr = getPoseState(session, &gi, &ps);

            XrSpaceLocation loc = { XR_TYPE_SPACE_LOCATION };
            XrResult lr = locateSpace(hspace, refSpace, fs.predictedDisplayTime, &loc);

            XrInteractionProfileState ips = { XR_TYPE_INTERACTION_PROFILE_STATE };
            XrResult pr = getCurProfile(session, hp, &ips);
            char profStr[128] = "<none>";
            if (pr == XR_SUCCESS && ips.interactionProfile != XR_NULL_PATH) {
                uint32_t len = 0;
                pathToString(inst, ips.interactionProfile, sizeof(profStr), &len, profStr);
            }

            if (doPrint) {
                printf("  %s: getActionStatePose=%d isActive=%d | locateSpace=%d flags=0x%x pos=(%.3f,%.3f,%.3f) | profile(%d)=%s\n",
                       hname, gr, ps.isActive, lr, loc.locationFlags,
                       loc.pose.position.x, loc.pose.position.y, loc.pose.position.z,
                       pr, profStr);
            }
        }

        XrFrameEndInfo fei = { XR_TYPE_FRAME_END_INFO };
        fei.displayTime = fs.predictedDisplayTime;
        fei.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
        fei.layerCount = 0; fei.layers = nullptr;
        if (endFrame(session, &fei) != XR_SUCCESS) break;
    }
    printf("PASS: action-state diagnostic run complete\n");
    return 0;
}
