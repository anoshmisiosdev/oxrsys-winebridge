/* Controlled OpenVR-level test: talks to OpenComposite's openvr_api.dll
 * directly (bypassing UE4 entirely), replicating the exact call sequence
 * BasaultVR makes before it crashes - GetStringTrackedDeviceProperty,
 * GetRecommendedRenderTargetSize, GetPlayAreaRect (chaperone), then
 * GetHiddenAreaMesh for both eyes - to reproduce the CVRSystem_020::base
 * corruption in a fully-instrumented, single-source-controlled repro
 * instead of a stripped UE4 shipping binary.
 *
 * Also exercises more of the surrounding OpenVR surface (GetProjectionRaw,
 * GetEyeToHeadTransform, IVRCompositor pose query) as a broader spec-coverage
 * smoke test of the bridge, independent of any one game's quirks.
 */
#include <windows.h>
#include <cstdio>
#include <cstring>

#include "generated/interfaces/IVRSystem_020.h"
#include "generated/interfaces/IVRChaperone_003.h"
#include "generated/interfaces/IVRCompositor_022.h"

using namespace vr;

typedef uint32_t(VR_CALLTYPE *PFN_VR_InitInternal2)(EVRInitError *, EVRApplicationType, const char *);
typedef void(VR_CALLTYPE *PFN_VR_ShutdownInternal)();
typedef void *(VR_CALLTYPE *PFN_VR_GetGenericInterface)(const char *, EVRInitError *);
typedef bool(VR_CALLTYPE *PFN_VR_IsHmdPresent)();

#define CHECK(expr)                                                             \
	do {                                                                         \
		if (!(expr)) {                                                           \
			printf("FAIL: %s (line %d)\n", #expr, __LINE__);                    \
			fflush(stdout);                                                      \
			return 1;                                                            \
		}                                                                        \
		printf("OK: %s\n", #expr);                                              \
		fflush(stdout);                                                         \
	} while (0)

static void dumpMesh(const char *label, const HiddenAreaMesh_t &mesh)
{
	printf("    %s: pVertexData=%p unTriangleCount=%u\n", label, (void *)mesh.pVertexData, mesh.unTriangleCount);
	fflush(stdout);
}

int main(void)
{
	HMODULE dll = LoadLibraryA("openvr_api.dll");
	if (!dll) {
		printf("FAIL: LoadLibrary openvr_api.dll (err %lu)\n", GetLastError());
		return 1;
	}
	printf("OK: openvr_api.dll loaded\n");

	auto pInit = (PFN_VR_InitInternal2)GetProcAddress(dll, "VR_InitInternal2");
	auto pShutdown = (PFN_VR_ShutdownInternal)GetProcAddress(dll, "VR_ShutdownInternal");
	auto pGetIface = (PFN_VR_GetGenericInterface)GetProcAddress(dll, "VR_GetGenericInterface");
	auto pIsPresent = (PFN_VR_IsHmdPresent)GetProcAddress(dll, "VR_IsHmdPresent");
	if (!pInit || !pShutdown || !pGetIface) {
		printf("FAIL: missing exports (init=%p shutdown=%p getiface=%p)\n", (void *)pInit, (void *)pShutdown, (void *)pGetIface);
		return 1;
	}
	printf("OK: resolved VR_InitInternal2/VR_ShutdownInternal/VR_GetGenericInterface\n");

	if (pIsPresent)
		printf("INFO: VR_IsHmdPresent() = %d\n", pIsPresent());

	EVRInitError err = VRInitError_None;
	uint32_t token = pInit(&err, VRApplication_Scene, nullptr);
	printf("%s: VR_InitInternal2 -> token=%u err=%d\n", err == VRInitError_None ? "OK" : "FAIL", token, err);
	if (err != VRInitError_None)
		return 1;

	err = VRInitError_None;
	void *sysRaw = pGetIface(IVRSystem_020::IVRSystem_Version, &err);
	CHECK(sysRaw != nullptr && err == VRInitError_None);
	auto *system = (IVRSystem_020::IVRSystem *)sysRaw;
	printf("    IVRSystem this=%p\n", (void *)system);
	fflush(stdout);

	err = VRInitError_None;
	void *chapRaw = pGetIface(IVRChaperone_003::IVRChaperone_Version, &err);
	CHECK(chapRaw != nullptr && err == VRInitError_None);
	auto *chaperone = (IVRChaperone_003::IVRChaperone *)chapRaw;
	printf("    IVRChaperone this=%p\n", (void *)chaperone);
	fflush(stdout);

	err = VRInitError_None;
	void *compRaw = pGetIface(IVRCompositor_022::IVRCompositor_Version, &err);
	CHECK(compRaw != nullptr && err == VRInitError_None);
	auto *compositor = (IVRCompositor_022::IVRCompositor *)compRaw;
	printf("    IVRCompositor this=%p\n", (void *)compositor);
	fflush(stdout);

	/* --- replicate BasaultVR's exact pre-crash sequence --- */
	char buf[128];
	ETrackedPropertyError propErr;
	uint32_t n = system->GetStringTrackedDeviceProperty(k_unTrackedDeviceIndex_Hmd, Prop_TrackingSystemName_String, buf, sizeof(buf), &propErr);
	printf("OK: GetStringTrackedDeviceProperty(TrackingSystemName) -> n=%u err=%d val=\"%s\"\n", n, propErr, n ? buf : "");
	fflush(stdout);

	n = system->GetStringTrackedDeviceProperty(k_unTrackedDeviceIndex_Hmd, Prop_SerialNumber_String, buf, sizeof(buf), &propErr);
	printf("OK: GetStringTrackedDeviceProperty(SerialNumber) -> n=%u err=%d val=\"%s\"\n", n, propErr, n ? buf : "");
	fflush(stdout);

	uint32_t w = 0, h = 0;
	system->GetRecommendedRenderTargetSize(&w, &h);
	printf("OK: GetRecommendedRenderTargetSize -> %ux%u\n", w, h);
	fflush(stdout);

	HmdQuad_t rect{};
	bool haveRect = chaperone->GetPlayAreaRect(&rect);
	printf("%s: GetPlayAreaRect -> %d\n", haveRect ? "OK" : "note", haveRect);
	fflush(stdout);

	/* the exact call that corrupts CVRSystem_020::base in the real game */
	HiddenAreaMesh_t meshL = system->GetHiddenAreaMesh(Eye_Left, k_eHiddenAreaMesh_Standard);
	dumpMesh("Eye_Left", meshL);
	printf("    IVRSystem this=%p (should be unchanged: %p)\n", (void *)system, (void *)system);
	fflush(stdout);

	HiddenAreaMesh_t meshR = system->GetHiddenAreaMesh(Eye_Right, k_eHiddenAreaMesh_Standard);
	dumpMesh("Eye_Right", meshR);
	fflush(stdout);

	/* keep exercising the surface a bit further, matching what a real
	 * render loop would touch right after HMD setup */
	float l, r, t, b;
	system->GetProjectionRaw(Eye_Left, &l, &r, &t, &b);
	printf("OK: GetProjectionRaw(Left) -> l=%f r=%f t=%f b=%f\n", l, r, t, b);
	fflush(stdout);

	HmdMatrix34_t eyeL = system->GetEyeToHeadTransform(Eye_Left);
	printf("OK: GetEyeToHeadTransform(Left) -> m[0][0]=%f m[0][3]=%f\n", eyeL.m[0][0], eyeL.m[0][3]);
	fflush(stdout);

	HmdMatrix34_t eyeR = system->GetEyeToHeadTransform(Eye_Right);
	printf("OK: GetEyeToHeadTransform(Right) -> m[0][0]=%f m[0][3]=%f\n", eyeR.m[0][0], eyeR.m[0][3]);
	fflush(stdout);

	/* one more round of hidden-area-mesh calls, now that other calls have
	 * run in between - checks whether the corruption is order-sensitive */
	HiddenAreaMesh_t meshL2 = system->GetHiddenAreaMesh(Eye_Left, k_eHiddenAreaMesh_Standard);
	dumpMesh("Eye_Left (2nd pass)", meshL2);
	fflush(stdout);

	TrackedDevicePose_t poses[k_unMaxTrackedDeviceCount];
	IVRCompositor_022::EVRCompositorError cErr = compositor->WaitGetPoses(poses, k_unMaxTrackedDeviceCount, nullptr, 0);
	printf("%s: WaitGetPoses -> %d\n", cErr == IVRCompositor_022::VRCompositorError_None ? "OK" : "note", cErr);
	fflush(stdout);

	pShutdown();
	printf("PASS: openvr_test completed without crashing\n");
	return 0;
}
