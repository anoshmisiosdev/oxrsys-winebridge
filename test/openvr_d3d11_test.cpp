/* Combined test: real D3D11 rendering through OpenComposite's IVRCompositor
 * (the actual DXMT D3D11->Metal interop path a real game exercises),
 * running CONCURRENTLY with two threads hammering GetHiddenAreaMesh /
 * GetEyeToHeadTransform - the same call surface openvr_test_mt.cpp already
 * proved race-free in isolation (37M+ calls, 5s, zero corruption).
 *
 * openvr_test_mt.cpp's clean result narrows the corruption seen in real
 * games (BasaultVR) away from "OpenComposite's OpenVR call handling isn't
 * thread-safe" and toward "something in the real D3D11/DXMT render path,
 * running concurrently with HMD queries, corrupts nearby heap memory" -
 * this test is the direct, controlled way to check that theory.
 */
#include <windows.h>
#include <d3d11.h>
#include <cstdio>
#include <atomic>

#include "generated/interfaces/IVRSystem_020.h"
#include "generated/interfaces/IVRChaperone_003.h"
#include "generated/interfaces/IVRCompositor_022.h"

using namespace vr;

typedef uint32_t(VR_CALLTYPE *PFN_VR_InitInternal2)(EVRInitError *, EVRApplicationType, const char *);
typedef void(VR_CALLTYPE *PFN_VR_ShutdownInternal)();
typedef void *(VR_CALLTYPE *PFN_VR_GetGenericInterface)(const char *, EVRInitError *);

#define CHECK(expr)                                                     \
	do {                                                                 \
		if (!(expr)) {                                                   \
			printf("FAIL: %s (line %d)\n", #expr, __LINE__);            \
			fflush(stdout);                                              \
			return 1;                                                    \
		}                                                                \
		printf("OK: %s\n", #expr);                                      \
		fflush(stdout);                                                 \
	} while (0)

static std::atomic<bool> g_stop{ false };
static std::atomic<uint64_t> g_iters_hidden{ 0 }, g_iters_eye{ 0 }, g_frames{ 0 };
static std::atomic<int> g_anomalies{ 0 };

static DWORD WINAPI hiddenMeshWorker(LPVOID param)
{
	auto *sys = (IVRSystem_020::IVRSystem *)param;
	while (!g_stop.load(std::memory_order_relaxed)) {
		HiddenAreaMesh_t mL = sys->GetHiddenAreaMesh(Eye_Left, k_eHiddenAreaMesh_Standard);
		HiddenAreaMesh_t mR = sys->GetHiddenAreaMesh(Eye_Right, k_eHiddenAreaMesh_Standard);
		if ((mL.pVertexData == nullptr && mL.unTriangleCount != 0) ||
		    (mR.pVertexData == nullptr && mR.unTriangleCount != 0)) {
			g_anomalies.fetch_add(1);
			printf("!!! ANOMALY in hiddenMeshWorker: L(ptr=%p,n=%u) R(ptr=%p,n=%u)\n",
			       (void *)mL.pVertexData, mL.unTriangleCount, (void *)mR.pVertexData, mR.unTriangleCount);
			fflush(stdout);
		}
		g_iters_hidden.fetch_add(1, std::memory_order_relaxed);
	}
	return 0;
}

static DWORD WINAPI eyeTransformWorker(LPVOID param)
{
	auto *sys = (IVRSystem_020::IVRSystem *)param;
	while (!g_stop.load(std::memory_order_relaxed)) {
		float l, r, t, b;
		sys->GetProjectionRaw(Eye_Left, &l, &r, &t, &b);
		sys->GetProjectionRaw(Eye_Right, &l, &r, &t, &b);
		HmdMatrix34_t eL = sys->GetEyeToHeadTransform(Eye_Left);
		HmdMatrix34_t eR = sys->GetEyeToHeadTransform(Eye_Right);
		(void)eL; (void)eR;
		g_iters_eye.fetch_add(1, std::memory_order_relaxed);
	}
	return 0;
}

int main(void)
{
	HMODULE dll = LoadLibraryA("openvr_api.dll");
	if (!dll) { printf("FAIL: LoadLibrary openvr_api.dll (%lu)\n", GetLastError()); return 1; }
	auto pInit = (PFN_VR_InitInternal2)GetProcAddress(dll, "VR_InitInternal2");
	auto pShutdown = (PFN_VR_ShutdownInternal)GetProcAddress(dll, "VR_ShutdownInternal");
	auto pGetIface = (PFN_VR_GetGenericInterface)GetProcAddress(dll, "VR_GetGenericInterface");
	CHECK(pInit && pShutdown && pGetIface);

	EVRInitError err = VRInitError_None;
	pInit(&err, VRApplication_Scene, nullptr);
	CHECK(err == VRInitError_None);

	err = VRInitError_None;
	auto *system = (IVRSystem_020::IVRSystem *)pGetIface(IVRSystem_020::IVRSystem_Version, &err);
	CHECK(system != nullptr && err == VRInitError_None);

	err = VRInitError_None;
	auto *compositor = (IVRCompositor_022::IVRCompositor *)pGetIface(IVRCompositor_022::IVRCompositor_Version, &err);
	CHECK(compositor != nullptr && err == VRInitError_None);

	uint32_t w = 0, h = 0;
	system->GetRecommendedRenderTargetSize(&w, &h);
	printf("OK: recommended size %ux%u\n", w, h);
	fflush(stdout);

	/* real D3D11 device - the actual DXMT-facing path */
	ID3D11Device *dev = nullptr;
	ID3D11DeviceContext *ctx = nullptr;
	D3D_FEATURE_LEVEL flReq[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0 }, flGot;
	HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, flReq, 2, D3D11_SDK_VERSION, &dev, &flGot, &ctx);
	if (FAILED(hr)) { printf("FAIL: D3D11CreateDevice hr=%#lx\n", (unsigned long)hr); return 1; }
	printf("OK: D3D11 device (feature level %#x)\n", flGot);
	fflush(stdout);

	ID3D11Texture2D *tex[2] = {};
	ID3D11RenderTargetView *rtv[2] = {};
	D3D11_TEXTURE2D_DESC td{};
	td.Width = w; td.Height = h; td.MipLevels = 1; td.ArraySize = 1;
	td.Format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
	td.SampleDesc.Count = 1;
	td.Usage = D3D11_USAGE_DEFAULT;
	td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
	for (int eye = 0; eye < 2; eye++) {
		hr = dev->CreateTexture2D(&td, nullptr, &tex[eye]);
		if (FAILED(hr)) { printf("FAIL: CreateTexture2D eye=%d hr=%#lx\n", eye, (unsigned long)hr); return 1; }
		hr = dev->CreateRenderTargetView((ID3D11Resource *)tex[eye], nullptr, &rtv[eye]);
		if (FAILED(hr)) { printf("FAIL: CreateRenderTargetView eye=%d hr=%#lx\n", eye, (unsigned long)hr); return 1; }
	}
	printf("OK: created 2 render-target textures %ux%u\n", w, h);
	fflush(stdout);

	printf("Starting 2 racing threads + real D3D11/Submit render loop for 10s...\n");
	fflush(stdout);
	HANDLE hHidden = CreateThread(nullptr, 0, hiddenMeshWorker, system, 0, nullptr);
	HANDLE hEye = CreateThread(nullptr, 0, eyeTransformWorker, system, 0, nullptr);

	Texture_t vrtex[2];
	for (int eye = 0; eye < 2; eye++) {
		vrtex[eye].handle = (void *)tex[eye];
		vrtex[eye].eType = TextureType_DirectX;
		vrtex[eye].eColorSpace = ColorSpace_Auto;
	}

	DWORD startTick = GetTickCount();
	int frame = 0;
	while (GetTickCount() - startTick < 10000) {
		float t = frame / 90.0f;
		FLOAT colL[4] = { 0.5f + 0.5f * (float)__builtin_sinf(t), 0.5f + 0.5f * (float)__builtin_sinf(t + 2.1f), 0.2f, 1.0f };
		FLOAT colR[4] = { 0.5f + 0.5f * (float)__builtin_sinf(t), 0.5f + 0.5f * (float)__builtin_sinf(t + 2.1f), 0.9f, 1.0f };
		ctx->ClearRenderTargetView(rtv[0], colL);
		ctx->ClearRenderTargetView(rtv[1], colR);

		IVRCompositor_022::EVRCompositorError e0 = compositor->Submit(Eye_Left, &vrtex[0], nullptr, vr::Submit_Default);
		IVRCompositor_022::EVRCompositorError e1 = compositor->Submit(Eye_Right, &vrtex[1], nullptr, vr::Submit_Default);
		if (e0 != IVRCompositor_022::VRCompositorError_None || e1 != IVRCompositor_022::VRCompositorError_None) {
			printf("note: Submit -> left=%d right=%d (frame %d)\n", e0, e1, frame);
			fflush(stdout);
		}
		g_frames.fetch_add(1, std::memory_order_relaxed);

		if (frame % 90 == 0) {
			printf("    [frame %d] iters_hidden=%llu iters_eye=%llu anomalies=%d\n",
			       frame, (unsigned long long)g_iters_hidden.load(), (unsigned long long)g_iters_eye.load(), g_anomalies.load());
			fflush(stdout);
		}
		frame++;
		Sleep(11); /* ~90Hz */
	}

	g_stop.store(true);
	WaitForSingleObject(hHidden, INFINITE);
	WaitForSingleObject(hEye, INFINITE);
	CloseHandle(hHidden);
	CloseHandle(hEye);

	printf("Done. frames=%llu iters_hidden=%llu iters_eye=%llu anomalies=%d\n",
	       (unsigned long long)g_frames.load(), (unsigned long long)g_iters_hidden.load(),
	       (unsigned long long)g_iters_eye.load(), g_anomalies.load());
	fflush(stdout);

	pShutdown();
	printf("PASS: openvr_d3d11_test completed without crashing\n");
	return 0;
}
