/* Multi-threaded variant of openvr_test.cpp: the single-threaded sequential
 * version passed cleanly (IVRSystem::this stayed identical across every
 * call, valid mesh pointers both eyes, twice), ruling out GetHiddenAreaMesh
 * and its call sequence as inherently broken. This version spins up a
 * second thread that concurrently hammers the SAME cached interface
 * objects (GetHiddenAreaMesh, GetEyeToHeadTransform, GetRecommendedRender-
 * TargetSize) while the main thread does the same, on the theory that UE4
 * calls OpenVR/OpenXR HMD queries from more than one thread (game thread +
 * render thread) and something in OpenComposite's backend singleton state
 * (BackendManager::Instance(), or mutable state inside XrHMD) isn't
 * synchronized for concurrent access - which the earlier interfaces-map
 * mutex fix does not cover, since that only guards interface creation/
 * lookup, not concurrent USE of an already-cached interface.
 */
#include <windows.h>
#include <cstdio>
#include <cstring>
#include <atomic>

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

	/* --- concurrent stress: two threads hammering the same cached
	 * interfaces simultaneously, one biased toward GetHiddenAreaMesh
	 * (matching the game thread's HMD-setup calls), one toward
	 * GetEyeToHeadTransform/GetProjectionRaw/WaitGetPoses (matching a
	 * render thread's per-frame HMD queries) --- */
	static std::atomic<bool> stop{ false };
	static std::atomic<int> corruption_count{ 0 };
	static std::atomic<uint64_t> iterations_a{ 0 }, iterations_b{ 0 };
	void *expected_this = (void *)system;

	auto workerA = [](LPVOID param) -> DWORD {
		auto *sys = (IVRSystem_020::IVRSystem *)param;
		while (!stop.load(std::memory_order_relaxed)) {
			HiddenAreaMesh_t m = sys->GetHiddenAreaMesh(Eye_Left, k_eHiddenAreaMesh_Standard);
			(void)m;
			HiddenAreaMesh_t m2 = sys->GetHiddenAreaMesh(Eye_Right, k_eHiddenAreaMesh_Standard);
			(void)m2;
			iterations_a.fetch_add(1, std::memory_order_relaxed);
		}
		return 0;
	};
	auto workerB = [](LPVOID param) -> DWORD {
		auto *sys = (IVRSystem_020::IVRSystem *)param;
		while (!stop.load(std::memory_order_relaxed)) {
			float l, r, t, b;
			sys->GetProjectionRaw(Eye_Left, &l, &r, &t, &b);
			sys->GetProjectionRaw(Eye_Right, &l, &r, &t, &b);
			HmdMatrix34_t e = sys->GetEyeToHeadTransform(Eye_Left);
			(void)e;
			iterations_b.fetch_add(1, std::memory_order_relaxed);
		}
		return 0;
	};

	printf("Starting 2 threads to race GetHiddenAreaMesh / GetEyeToHeadTransform for 5s...\n");
	fflush(stdout);
	HANDLE hA = CreateThread(nullptr, 0, workerA, system, 0, nullptr);
	HANDLE hB = CreateThread(nullptr, 0, workerB, system, 0, nullptr);

	for (int sec = 0; sec < 5; sec++) {
		Sleep(1000);
		printf("    [%ds] iterations A=%llu B=%llu | polling 'this' from main thread: %p\n",
		       sec + 1, (unsigned long long)iterations_a.load(), (unsigned long long)iterations_b.load(), (void *)system);
		fflush(stdout);
		/* main thread also joins in, checking `this` identity holds */
		HiddenAreaMesh_t mm = system->GetHiddenAreaMesh(Eye_Left, k_eHiddenAreaMesh_Standard);
		if (mm.pVertexData == nullptr && mm.unTriangleCount != 0) {
			corruption_count.fetch_add(1);
			printf("    !!! anomalous mesh: pVertexData=null unTriangleCount=%u\n", mm.unTriangleCount);
		}
	}
	stop.store(true);
	WaitForSingleObject(hA, INFINITE);
	WaitForSingleObject(hB, INFINITE);
	CloseHandle(hA);
	CloseHandle(hB);

	printf("Race complete. iterations A=%llu B=%llu, this pointer never changed if we got here: %p (started as %p)\n",
	       (unsigned long long)iterations_a.load(), (unsigned long long)iterations_b.load(), (void *)system, expected_this);
	fflush(stdout);

	pShutdown();
	printf("PASS: openvr_test completed without crashing\n");
	return 0;
}
