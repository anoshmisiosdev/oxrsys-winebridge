/* OpenVR D3D12 app through OpenComposite (openvr_api.dll) -> wineopenxr ->
 * OXRSys, the way HITMAN 3 drives it: VR_InitInternal2(Scene) (OpenComposite
 * binds a temporary D3D11 session, looking the adapter up by the LUID the
 * bridge reports), IVRSystem_022 / IVRCompositor_028, then per frame
 * WaitGetPoses, GetLastPosePredictionIDs + GetPosesForFrame, render each eye on a D3DMetal D3D12 queue, and Submit
 * TextureType_DirectX12 with a D3D12TextureData_t {resource, queue}.
 *
 * Eye textures: left clears red with a moving white bar, right clears green.
 * Env knobs: OVR_TEST_FRAMES (default 300), OVR_TEST_FORMAT (DXGI, default
 * 29 = R8G8B8A8_UNORM_SRGB), OVR_TEST_SBS=1 (one double-wide texture, both
 * eyes submitted with bounds), OVR_TEST_DLL (default openvr_api.dll),
 * OVR_TEST_STATE (source state at Submit: 0 = PIXEL_SHADER_RESOURCE (default),
 * 1 = RENDER_TARGET, 2 = COPY_SOURCE).
 *
 * Build: x86_64-w64-mingw32-g++ -O1 -static -I<oc>/build/generated/interfaces \
 *          -I<oc>/OpenVRHeaders openvr_d3d12_test.cpp -ld3d12 -ldxgi -o openvr_d3d12_test.exe
 *   (<oc> = the opencomposite checkout; needs a configured build dir for
 *    the generated interface headers)
 * Run: test/dmsubst/run-in-bottle.sh /abs/path/openvr_d3d12_test.exe, with the
 *   OpenComposite openvr_api.dll next to the exe (or OVR_TEST_DLL=<abs path>) */
#define WIDL_EXPLICIT_AGGREGATE_RETURNS 1
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <stdio.h>
#include <stdlib.h>

#include "IVRSystem_022.h"
#include "IVRCompositor_028.h"

using namespace vr;

typedef uint32_t(VR_CALLTYPE *PFN_VR_InitInternal2)(EVRInitError *, EVRApplicationType, const char *);
typedef void(VR_CALLTYPE *PFN_VR_ShutdownInternal)();
typedef void *(VR_CALLTYPE *PFN_VR_GetGenericInterface)(const char *, EVRInitError *);

#define CHECK(expr)                                                  \
	do {                                                             \
		if (!(expr)) {                                               \
			printf("FAIL: %s (line %d)\n", #expr, __LINE__);         \
			fflush(stdout);                                          \
			return 1;                                                \
		}                                                            \
	} while (0)
#define CHECKHR(expr)                                                                 \
	do {                                                                              \
		HRESULT hr_ = (expr);                                                         \
		if (FAILED(hr_)) {                                                            \
			printf("FAIL: %s hr=%#lx (line %d)\n", #expr, (unsigned long)hr_, __LINE__); \
			fflush(stdout);                                                           \
			return 1;                                                                 \
		}                                                                             \
	} while (0)

static int envi(const char *name, int def)
{
	const char *v = getenv(name);
	return v && *v ? atoi(v) : def;
}

static void barrier(ID3D12GraphicsCommandList *cl, ID3D12Resource *res, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to)
{
	if (from == to)
		return;
	D3D12_RESOURCE_BARRIER b = {};
	b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
	b.Transition.pResource = res;
	b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
	b.Transition.StateBefore = from;
	b.Transition.StateAfter = to;
	cl->ResourceBarrier(1, &b);
}

int main(void)
{
	const char *dllName = getenv("OVR_TEST_DLL");
	int frames = envi("OVR_TEST_FRAMES", 300);
	DXGI_FORMAT fmt = (DXGI_FORMAT)envi("OVR_TEST_FORMAT", DXGI_FORMAT_R8G8B8A8_UNORM_SRGB);
	int sbs = envi("OVR_TEST_SBS", 0);
	int stateSel = envi("OVR_TEST_STATE", 0);
	D3D12_RESOURCE_STATES submitState = stateSel == 1 ? D3D12_RESOURCE_STATE_RENDER_TARGET
	    : stateSel == 2                               ? D3D12_RESOURCE_STATE_COPY_SOURCE
	                                                  : D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;

	HMODULE dll = LoadLibraryA(dllName && *dllName ? dllName : "openvr_api.dll");
	if (!dll) {
		printf("FAIL: LoadLibrary openvr_api.dll (%lu)\n", GetLastError());
		return 1;
	}
	auto pInit = (PFN_VR_InitInternal2)GetProcAddress(dll, "VR_InitInternal2");
	auto pShutdown = (PFN_VR_ShutdownInternal)GetProcAddress(dll, "VR_ShutdownInternal");
	auto pGetIface = (PFN_VR_GetGenericInterface)GetProcAddress(dll, "VR_GetGenericInterface");
	CHECK(pInit && pShutdown && pGetIface);

	printf("VR_InitInternal2(Scene)...\n");
	fflush(stdout);
	EVRInitError err = VRInitError_None;
	pInit(&err, VRApplication_Scene, nullptr);
	printf("VR_InitInternal2 -> %d\n", err);
	fflush(stdout);
	CHECK(err == VRInitError_None);

	auto *system = (IVRSystem_022::IVRSystem *)pGetIface(IVRSystem_022::IVRSystem_Version, &err);
	CHECK(system && err == VRInitError_None);
	auto *compositor = (IVRCompositor_028::IVRCompositor *)pGetIface(IVRCompositor_028::IVRCompositor_Version, &err);
	CHECK(compositor && err == VRInitError_None);

	uint32_t w = 0, h = 0;
	system->GetRecommendedRenderTargetSize(&w, &h);
	/* Like HITMAN 3: read the projection once, at startup */
	for (int eye = 0; eye < 2; eye++) {
		float l, r, t, b;
		system->GetProjectionRaw((EVREye)eye, &l, &r, &t, &b);
		/* (GetEyeToHeadTransform returns a struct by value with MSVC's ABI, which a GCC-built caller can't take) */
		printf("eye %d projection raw L=%.4f R=%.4f T=%.4f B=%.4f\n", eye, l, r, t, b);
	}
	printf("recommended size %ux%u, format %d, sbs %d, submit state %#x\n", w, h, fmt, sbs, submitState);
	fflush(stdout);

	/* What HITMAN 3 asks before creating its device: the adapter, by LUID and index */
	uint64_t outDev = 0xdeadbeef;
	system->GetOutputDevice(&outDev, TextureType_DirectX12, nullptr);
	int32_t dxgiIndex = -1;
	system->GetDXGIOutputInfo(&dxgiIndex);
	printf("GetOutputDevice(DX12) -> %#llx, GetDXGIOutputInfo -> %d\n", (unsigned long long)outDev, dxgiIndex);
	fflush(stdout);

	/* The app's D3D12 device on the default adapter (D3DMetal) */
	ID3D12Device *dev = nullptr;
	CHECKHR(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&dev)));
	ID3D12CommandQueue *queue = nullptr;
	D3D12_COMMAND_QUEUE_DESC qd = {};
	qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
	CHECKHR(dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue)));
	ID3D12CommandAllocator *alloc = nullptr;
	CHECKHR(dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc)));
	ID3D12GraphicsCommandList *cl = nullptr;
	CHECKHR(dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc, nullptr, IID_PPV_ARGS(&cl)));
	cl->Close();
	ID3D12Fence *fence = nullptr;
	CHECKHR(dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)));
	HANDLE fenceEvent = CreateEventA(nullptr, FALSE, FALSE, nullptr);
	UINT64 fenceValue = 0;

	int texCount = sbs ? 1 : 2;
	ID3D12Resource *tex[2] = {};
	D3D12_RESOURCE_DESC rd = {};
	rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
	rd.Width = sbs ? 2 * w : w;
	rd.Height = h;
	rd.DepthOrArraySize = 1;
	rd.MipLevels = 1;
	rd.Format = fmt;
	rd.SampleDesc.Count = 1;
	rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
	D3D12_HEAP_PROPERTIES hp = {};
	hp.Type = D3D12_HEAP_TYPE_DEFAULT;
	D3D12_DESCRIPTOR_HEAP_DESC dhd = {};
	dhd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
	dhd.NumDescriptors = 2;
	ID3D12DescriptorHeap *rtvHeap = nullptr;
	CHECKHR(dev->CreateDescriptorHeap(&dhd, IID_PPV_ARGS(&rtvHeap)));
	UINT rtvStride = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
	D3D12_CPU_DESCRIPTOR_HANDLE rtv[2];
	for (int i = 0; i < texCount; i++) {
		CHECKHR(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, submitState, nullptr, IID_PPV_ARGS(&tex[i])));
		rtv[i] = rtvHeap->GetCPUDescriptorHandleForHeapStart();
		rtv[i].ptr += i * rtvStride;
		dev->CreateRenderTargetView(tex[i], nullptr, rtv[i]);
	}
	printf("created %d eye texture(s) %llux%u\n", texCount, (unsigned long long)rd.Width, h);
	fflush(stdout);

	D3D12TextureData_t td[2] = {};
	Texture_t vrtex[2];
	for (int eye = 0; eye < 2; eye++) {
		td[eye].m_pResource = tex[sbs ? 0 : eye];
		td[eye].m_pCommandQueue = queue;
		td[eye].m_nNodeMask = 0;
		vrtex[eye].handle = &td[eye];
		vrtex[eye].eType = TextureType_DirectX12;
		vrtex[eye].eColorSpace = ColorSpace_Auto;
	}
	VRTextureBounds_t bounds[2] = { { 0.0f, 0.0f, 0.5f, 1.0f }, { 0.5f, 0.0f, 1.0f, 1.0f } };

	TrackedDevicePose_t poses[k_unMaxTrackedDeviceCount];
	int submitErrors = 0;
	for (int frame = 0; frame < frames; frame++) {
		IVRCompositor_028::EVRCompositorError we = compositor->WaitGetPoses(poses, k_unMaxTrackedDeviceCount, nullptr, 0);
		if (we != IVRCompositor_028::VRCompositorError_None && frame < 5)
			printf("note: WaitGetPoses -> %d\n", we);

		uint32_t renderId = 0, gameId = 0;
		compositor->GetLastPosePredictionIDs(&renderId, &gameId);
		TrackedDevicePose_t framePoses[k_unMaxTrackedDeviceCount];
		IVRCompositor_028::EVRCompositorError pe = compositor->GetPosesForFrame(renderId, framePoses, k_unMaxTrackedDeviceCount);
		if (frame % 90 == 0)
			printf("prediction id %u/%u, GetPosesForFrame -> %d, hmd valid %d\n", renderId, gameId, pe, framePoses[0].bPoseIsValid);

		CHECKHR(alloc->Reset());
		CHECKHR(cl->Reset(alloc, nullptr));
		for (int i = 0; i < texCount; i++) {
			barrier(cl, tex[i], submitState, D3D12_RESOURCE_STATE_RENDER_TARGET);
			for (int eye = 0; eye < 2; eye++) {
				if (!sbs && eye != i)
					continue;
				float red[4] = { 0.8f, 0.1f, 0.1f, 1.0f }, green[4] = { 0.1f, 0.8f, 0.1f, 1.0f }, white[4] = { 1, 1, 1, 1 };
				D3D12_RECT r = { (LONG)(sbs ? eye * w : 0), 0, (LONG)(sbs ? (eye + 1) * w : w), (LONG)h };
				cl->ClearRenderTargetView(rtv[i], eye == 0 ? red : green, 1, &r);
				LONG x = r.left + (LONG)((frame * 8) % (w - 64));
				D3D12_RECT bar = { x, (LONG)(h / 4), x + 64, (LONG)(3 * h / 4) };
				cl->ClearRenderTargetView(rtv[i], white, 1, &bar);
			}
			barrier(cl, tex[i], D3D12_RESOURCE_STATE_RENDER_TARGET, submitState);
		}
		CHECKHR(cl->Close());
		ID3D12CommandList *lists[] = { cl };
		queue->ExecuteCommandLists(1, lists);

		for (int eye = 0; eye < 2; eye++) {
			IVRCompositor_028::EVRCompositorError e = compositor->Submit((EVREye)eye, &vrtex[eye], sbs ? &bounds[eye] : nullptr, Submit_Default);
			if (e != IVRCompositor_028::VRCompositorError_None && submitErrors++ < 10)
				printf("note: Submit eye %d -> %d (frame %d)\n", eye, e, frame);
		}

		queue->Signal(fence, ++fenceValue);
		if (fence->GetCompletedValue() < fenceValue) {
			fence->SetEventOnCompletion(fenceValue, fenceEvent);
			WaitForSingleObject(fenceEvent, 5000);
		}
		if (frame % 90 == 0) {
			printf("frame %d, hmd pose valid %d\n", frame, poses[0].bPoseIsValid);
			fflush(stdout);
		}
	}

	printf("frames=%d submit errors=%d\n", frames, submitErrors);
	fflush(stdout);
	pShutdown();
	printf("PASS: openvr_d3d12_test\n");
	fflush(stdout);
	return 0;
}
