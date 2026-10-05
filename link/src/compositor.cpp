#include "compositor.h"

#include "depth.h"
#include "link.h"
#include "log.h"
#include "sr3.h"

#include <MinHook.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <dxgi.h>
#include <windows.h>

#include <atomic>
#include <cmath>
#include <cstring>

namespace mcsr3::compositor
{
	namespace
	{
		// Shared memory layout: mc/src/client/java/dev/mcsr3/client/FrameExporter.java
		constexpr wchar_t kShmName[] = L"Local\\MCPassthroughFrame";
		constexpr std::uint32_t kMagic = 0x5450434D;
		constexpr std::size_t kHeader = 4096, kSlotDesc = 256, kSlotDescBytes = 128;

		using PresentFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
		using ResizeFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);
		PresentFn g_presentOrig;
		ResizeFn g_resizeOrig;
		std::atomic<bool> g_enabled{ true };

		struct Slot
		{
			std::int64_t seq, mcFrame, hostFrame;
			std::int32_t width, height;
			float nearZ, farZ, fov;
			std::int32_t flags;
		};

		struct State
		{
			ID3D11Device* dev = nullptr;
			ID3D11DeviceContext* ctx = nullptr;
			ID3D11VertexShader* vs = nullptr;
			ID3D11PixelShader* ps = nullptr;
			ID3D11BlendState* blend = nullptr;
			ID3D11RasterizerState* raster = nullptr;
			ID3D11DepthStencilState* noDepth = nullptr;
			ID3D11SamplerState* sampler = nullptr;
			ID3D11Buffer* cb = nullptr;
			ID3D11Texture2D* tex[3] = {};  // world colour, overlay (RGBA8, premultiplied), world depth (R32F)
			ID3D11ShaderResourceView* srv[3] = {};
			float mcNear = 0.05f, mcFar = 1024.0f;
			float mcYaw = 0, mcPitch = 0, mcFov = 70;  // the pose Minecraft rendered this frame with
			int texW = 0, texH = 0;
			DXGI_FORMAT texFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
			ID3D11RenderTargetView* rtv = nullptr;
			const std::uint8_t* shm = nullptr;
			std::int64_t lastPublish = -1;
			int flags = 0;
			bool haveFrame = false;
			bool failed = false;
		} g;

		struct Constants
		{
			float flipY, depthTest, mcNear, mcFar;
			float mcFlags, srA, srB, reproject;
			float srR[3], srXs, srU[3], srYs, srF[3], pad0;  // SR3's camera now (SR3 world axes)
			float mcR[3], mcXs, mcU[3], mcYs, mcF[3], pad1;  // the camera Minecraft's frame was rendered with
			float srPos[3], body, feet[3], pad2;              // SR3's camera position; the player's feet (body = 1: known)
			float cursor[2], cursorOn, pad3;                  // a Minecraft screen's cursor (back buffer pixels)
		};

		// Re-projection: Minecraft's frame is a frame or two behind SR3's camera. Each SR3 pixel's view ray
		// (SR3's camera now) is looked up in the camera Minecraft rendered with, so turning doesn't make
		// blocks swim (rotation only; the HUD layer isn't moved).
		// Depth test: Minecraft's view depth at that spot, put on SR3's view axis, against SR3's scene depth
		// (D24, z = srA + srB / t from depth::Projection).
		const char kShader[] = R"(
cbuffer C : register(b0)
{
	float flipY, depthTest, mcNear, mcFar, mcFlags, srA, srB, reproject;
	float3 srR; float srXs; float3 srU; float srYs; float3 srF; float pad0;
	float3 mcR; float mcXs; float3 mcU; float mcYs; float3 mcF; float pad1;
	float3 srPos; float body; float3 feet; float pad2;
	float2 cursor; float cursorOn; float pad3;
};
Texture2D world : register(t0);
Texture2D overlay : register(t1);
Texture2D<float> mcDepth : register(t2);
Texture2D<float> srDepth : register(t3);
SamplerState s : register(s0);
struct V { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
V vs(uint id : SV_VertexID)
{
	V o;
	float2 t = float2((id << 1) & 2, id & 2);
	o.pos = float4(t * float2(2, -2) + float2(-1, 1), 0, 1);
	o.uv = t;
	return o;
}
float McViewDepth(float d)
{
	uint f = (uint)mcFlags;
	float n = mcNear, fa = mcFar;
	if (f & 4)  // reversed: 1 near, 0 far
		return (f & 1) ? n * fa / (n + d * (fa - n)) : 2 * n * fa / ((fa + n) + (2 * d - 1) * (fa - n));
	return (f & 1) ? n * fa / (fa - d * (fa - n)) : 2 * n * fa / ((fa + n) - (2 * d - 1) * (fa - n));
}
float4 Scene(V i)
{
	float2 flip = float2(i.uv.x, flipY > 0.5 ? 1 - i.uv.y : i.uv.y);
	float4 o = overlay.Sample(s, flip);
	float2 uv = flip;   // where in Minecraft's frame this pixel looks
	float along = 1;    // Minecraft view depth -> SR3 view depth, for that ray
	if (reproject > 0.5)
	{
		float2 n = float2(i.uv.x * 2 - 1, 1 - i.uv.y * 2);
		float3 d = srF + srR * (n.x / srXs) + srU * (n.y / srYs);  // dot(d, srF) = 1
		float zc = dot(d, mcF);
		float2 m = float2(dot(d, mcR), dot(d, mcU)) / max(zc, 1e-4) * float2(mcXs, mcYs);
		uv = float2(m.x * 0.5 + 0.5, 0.5 - m.y * 0.5);
		uv.y = flipY > 0.5 ? 1 - uv.y : uv.y;
		along = 1 / max(zc, 1e-4);
		if (zc <= 0 || any(uv < 0) || any(uv > 1))
			return o;
	}
	float4 w = world.Sample(s, uv);
	if (depthTest > 0.5 && w.a > 0)
	{
		uint mw, mh, sw, sh;
		mcDepth.GetDimensions(mw, mh);
		srDepth.GetDimensions(sw, sh);
		float tm = McViewDepth(mcDepth.Load(int3(uv * float2(mw, mh), 0))) * along;
		// Steve stands where the Boss stands: inside the player's body, Minecraft's pixel always wins, so
		// Steve covers the Boss instead of the Boss's limbs poking through him.
		if (body > 0.5 && reproject > 0.5)
		{
			float2 n = float2(i.uv.x * 2 - 1, 1 - i.uv.y * 2);
			float3 p = srPos + (srF + srR * (n.x / srXs) + srU * (n.y / srYs)) * tm;
			float2 off = p.xz - feet.xz;
			if (dot(off, off) < 0.55 * 0.55 && p.y > feet.y - 0.1 && p.y < feet.y + 2.1)
				return o + w * (1 - o.a);
		}
		float zs = srDepth.Load(int3(i.uv * float2(sw, sh), 0));
		float ts = zs >= 1 ? 1e30 : srB / (zs - srA);
		// a few cm of slack: SR3's surfaces and blocks placed on them meet exactly
		if (tm > ts + 0.03 + ts * 0.004)
			w = 0;
	}
	// premultiplied: overlay over world; the blend state puts the result over SR3's picture
	return o + w * (1 - o.a);
}
// The cursor while a Minecraft screen is open: a white arrow with a black edge
float4 ps(V i) : SV_Target
{
	float4 c = Scene(i);
	if (cursorOn > 0.5)
	{
		float2 r = i.pos.xy - cursor;
		r /= 1.5;  // 1.5x size
		bool outer = r.x >= 0 && r.y >= 0 && r.y <= 19 && r.x <= r.y * 0.62 + 1;
		float2 q = r - float2(1.2, 3);
		bool inner = q.x >= 0 && q.y >= 0 && q.y <= 13.5 && q.x <= q.y * 0.62;
		if (outer)
			return inner ? float4(1, 1, 1, 1) : float4(0, 0, 0, 1);
	}
	return c;
}
)";

		template <class T>
		void Release(T*& p)
		{
			if (p)
				p->Release();
			p = nullptr;
		}

		bool Compile(const char* entry, const char* target, ID3DBlob** out)
		{
			ID3DBlob* err = nullptr;
			HRESULT hr = D3DCompile(kShader, sizeof(kShader) - 1, "mcsr3", nullptr, nullptr, entry, target, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, out, &err);
			if (FAILED(hr))
			{
				Log("compositor: shader %s: %s", entry, err ? static_cast<const char*>(err->GetBufferPointer()) : "?");
				Release(err);
				return false;
			}
			Release(err);
			return true;
		}

		bool Setup(IDXGISwapChain* sc)
		{
			if (FAILED(sc->GetDevice(__uuidof(ID3D11Device), reinterpret_cast<void**>(&g.dev))))
				return false;
			g.dev->GetImmediateContext(&g.ctx);
			ID3DBlob *vsb = nullptr, *psb = nullptr;
			if (!Compile("vs", "vs_5_0", &vsb) || !Compile("ps", "ps_5_0", &psb))
				return false;
			g.dev->CreateVertexShader(vsb->GetBufferPointer(), vsb->GetBufferSize(), nullptr, &g.vs);
			g.dev->CreatePixelShader(psb->GetBufferPointer(), psb->GetBufferSize(), nullptr, &g.ps);
			Release(vsb);
			Release(psb);

			D3D11_BLEND_DESC bd{};
			bd.RenderTarget[0].BlendEnable = TRUE;
			bd.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE;
			bd.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
			bd.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
			bd.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ZERO;
			bd.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ONE;
			bd.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
			bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
			g.dev->CreateBlendState(&bd, &g.blend);
			D3D11_RASTERIZER_DESC rd{};
			rd.FillMode = D3D11_FILL_SOLID;
			rd.CullMode = D3D11_CULL_NONE;
			g.dev->CreateRasterizerState(&rd, &g.raster);
			D3D11_DEPTH_STENCIL_DESC dd{};
			g.dev->CreateDepthStencilState(&dd, &g.noDepth);
			D3D11_SAMPLER_DESC sd{};
			sd.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
			sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
			g.dev->CreateSamplerState(&sd, &g.sampler);
			D3D11_BUFFER_DESC cbd{ sizeof(Constants), D3D11_USAGE_DYNAMIC, D3D11_BIND_CONSTANT_BUFFER, D3D11_CPU_ACCESS_WRITE };
			g.dev->CreateBuffer(&cbd, nullptr, &g.cb);
			depth::Install(g.dev, g.ctx);
			Log("compositor: ready");
			return true;
		}

		bool MapShm()
		{
			if (g.shm)
				return true;
			static ULONGLONG lastTry;
			if (GetTickCount64() - lastTry < 2000)
				return false;
			lastTry = GetTickCount64();
			HANDLE h = OpenFileMappingW(FILE_MAP_READ, FALSE, kShmName);
			if (!h)
				return false;
			// The handle stays open: the view must outlive Minecraft closing the mapping.
			g.shm = static_cast<const std::uint8_t*>(MapViewOfFile(h, FILE_MAP_READ, 0, 0, 0));
			if (g.shm && *reinterpret_cast<const std::uint32_t*>(g.shm) != kMagic)
			{
				UnmapViewOfFile(g.shm);
				g.shm = nullptr;
			}
			if (g.shm)
				Log("compositor: Minecraft frames mapped");
			return g.shm != nullptr;
		}

		void EnsureTextures(int w, int h)
		{
			if (g.texW == w && g.texH == h && g.tex[0])
				return;
			for (int i = 0; i < 3; i++)
			{
				Release(g.srv[i]);
				Release(g.tex[i]);
				D3D11_TEXTURE2D_DESC td{};
				td.Width = w;
				td.Height = h;
				td.MipLevels = td.ArraySize = 1;
				td.Format = i == 2 ? DXGI_FORMAT_R32_FLOAT : g.texFormat;
				td.SampleDesc.Count = 1;
				td.Usage = D3D11_USAGE_DYNAMIC;
				td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
				td.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
				g.dev->CreateTexture2D(&td, nullptr, &g.tex[i]);
				g.dev->CreateShaderResourceView(g.tex[i], nullptr, &g.srv[i]);
			}
			g.texW = w;
			g.texH = h;
		}

		void Upload(ID3D11Texture2D* tex, const std::uint8_t* src, int w, int h)
		{
			D3D11_MAPPED_SUBRESOURCE m;
			if (FAILED(g.ctx->Map(tex, 0, D3D11_MAP_WRITE_DISCARD, 0, &m)))
				return;
			for (int y = 0; y < h; y++)
				std::memcpy(static_cast<std::uint8_t*>(m.pData) + std::size_t(y) * m.RowPitch, src + std::size_t(y) * w * 4, std::size_t(w) * 4);
			g.ctx->Unmap(tex, 0);
		}

		// The newest published slot, uploaded if it's new. Seqlock: retried if Minecraft rewrote it meanwhile.
		void PullFrame()
		{
			if (!MapShm())
				return;
			auto i32 = [](const std::uint8_t* p) { std::int32_t v; std::memcpy(&v, p, 4); return v; };
			auto i64 = [](const std::uint8_t* p) { std::int64_t v; std::memcpy(&v, p, 8); return v; };
			std::int64_t publish = i64(g.shm + 32);
			if (publish == g.lastPublish)
				return;
			std::int64_t stride = i64(g.shm + 16);
			for (int attempt = 0; attempt < 3; attempt++)
			{
				int slot = i32(g.shm + 40);
				if (slot < 0)
					return;
				const std::uint8_t* d = g.shm + kSlotDesc + kSlotDescBytes * slot;
				std::int64_t seq = i64(d);
				if (seq & 1)
					continue;
				int w = i32(d + 24), h = i32(d + 28);
				if (w <= 0 || h <= 0 || w > 3840 || h > 2160)
					return;
				EnsureTextures(w, h);
				const std::uint8_t* base = g.shm + kHeader + stride * slot;
				std::size_t n = std::size_t(w) * h * 4;
				Upload(g.tex[0], base, w, h);
				Upload(g.tex[1], base + 2 * n, w, h);
				Upload(g.tex[2], base + n, w, h);  // float32: also 4 bytes a pixel
				if (i64(d) != seq)
					continue;
				g.flags = i32(d + 44);
				std::memcpy(&g.mcNear, d + 32, 4);
				std::memcpy(&g.mcFar, d + 36, 4);
				std::memcpy(&g.mcFov, d + 40, 4);
				std::memcpy(&g.mcYaw, d + 72, 4);
				std::memcpy(&g.mcPitch, d + 76, 4);
				g.lastPublish = publish;
				g.haveFrame = true;
				return;
			}
		}

		void Draw(IDXGISwapChain* sc, ID3D11ShaderResourceView* srDepth)
		{
			if (!g.rtv)
			{
				ID3D11Texture2D* bb = nullptr;
				if (FAILED(sc->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&bb))))
					return;
				D3D11_TEXTURE2D_DESC bd;
				bb->GetDesc(&bd);
				HRESULT hr = g.dev->CreateRenderTargetView(bb, nullptr, &g.rtv);
				DXGI_FORMAT want = bd.Format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB || bd.Format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB ? DXGI_FORMAT_R8G8B8A8_UNORM_SRGB : DXGI_FORMAT_R8G8B8A8_UNORM;
				if (want != g.texFormat)
				{
					g.texFormat = want;
					g.texW = 0;  // recreated in the new format
					g.lastPublish = -1;
				}
				Log("compositor: back buffer %ux%u format %d (%s)", bd.Width, bd.Height, int(bd.Format), SUCCEEDED(hr) ? "ok" : "no RTV");
				bb->Release();
				if (!g.rtv)
					return;
			}
			ID3D11Texture2D* bb = nullptr;
			sc->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&bb));
			D3D11_TEXTURE2D_DESC bd;
			bb->GetDesc(&bd);
			bb->Release();

			// Save what we touch of SR3's pipeline state, and put it back afterwards.
			ID3D11RenderTargetView* oldRtv[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT] = {};
			ID3D11DepthStencilView* oldDsv = nullptr;
			g.ctx->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, oldRtv, &oldDsv);
			ID3D11BlendState* oldBlend = nullptr;
			float oldFactor[4];
			UINT oldMask;
			g.ctx->OMGetBlendState(&oldBlend, oldFactor, &oldMask);
			ID3D11DepthStencilState* oldDs = nullptr;
			UINT oldRef;
			g.ctx->OMGetDepthStencilState(&oldDs, &oldRef);
			ID3D11RasterizerState* oldRs = nullptr;
			g.ctx->RSGetState(&oldRs);
			UINT nvp = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
			D3D11_VIEWPORT oldVp[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE];
			g.ctx->RSGetViewports(&nvp, oldVp);
			ID3D11VertexShader* oldVs = nullptr;
			ID3D11PixelShader* oldPs = nullptr;
			ID3D11GeometryShader* oldGs = nullptr;
			g.ctx->VSGetShader(&oldVs, nullptr, nullptr);
			g.ctx->PSGetShader(&oldPs, nullptr, nullptr);
			g.ctx->GSGetShader(&oldGs, nullptr, nullptr);
			ID3D11ShaderResourceView* oldSrv[4] = {};
			g.ctx->PSGetShaderResources(0, 4, oldSrv);
			ID3D11SamplerState* oldSampler = nullptr;
			g.ctx->PSGetSamplers(0, 1, &oldSampler);
			ID3D11Buffer* oldCb = nullptr;
			g.ctx->PSGetConstantBuffers(0, 1, &oldCb);
			D3D11_PRIMITIVE_TOPOLOGY oldTopo;
			g.ctx->IAGetPrimitiveTopology(&oldTopo);
			ID3D11InputLayout* oldLayout = nullptr;
			g.ctx->IAGetInputLayout(&oldLayout);

			D3D11_MAPPED_SUBRESOURCE m;
			if (SUCCEEDED(g.ctx->Map(g.cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m)))
			{
				float a = 0, b = 0, xs = 0, ys = 0;
				bool proj = depth::Projection(a, b, &xs, &ys);
				bool test = srDepth && proj;
				Constants c{ (g.flags & 2) ? 1.0f : 0.0f, test ? 1.0f : 0.0f, g.mcNear, g.mcFar, float(g.flags), test ? a : 0, test ? b : 0, 0 };
				sr3::Camera cam;
				if (proj && sr3::GetCamera(cam))
				{
					c.reproject = 1;
					auto put = [](float* dst, const sr3::Vec3& v) { dst[0] = v.x, dst[1] = v.y, dst[2] = v.z; };
					put(c.srR, cam.right), put(c.srU, cam.up), put(c.srF, cam.forward);
					c.srXs = xs, c.srYs = ys;
					// Minecraft's camera basis from its yaw/pitch (Minecraft axes), then z flipped into SR3's
					const double yaw = g.mcYaw * 0.017453292519943295, pitch = g.mcPitch * 0.017453292519943295;
					const double f[3] = { -std::sin(yaw) * std::cos(pitch), -std::sin(pitch), std::cos(yaw) * std::cos(pitch) };
					const double r[3] = { -std::cos(yaw), 0, -std::sin(yaw) };
					const double u[3] = { r[1] * f[2] - r[2] * f[1], r[2] * f[0] - r[0] * f[2], r[0] * f[1] - r[1] * f[0] };
					for (int k = 0; k < 3; k++)
					{
						float flipZ = k == 2 ? -1.0f : 1.0f;
						c.mcF[k] = float(f[k]) * flipZ, c.mcR[k] = float(r[k]) * flipZ, c.mcU[k] = float(u[k]) * flipZ;
					}
					c.mcYs = float(1.0 / std::tan(g.mcFov * 0.5 * 0.017453292519943295));
					c.mcXs = c.mcYs * float(g.texH) / float(g.texW);
					put(c.srPos, cam.pos);
					sr3::Snapshot snap = sr3::GetSnapshot();  // render thread: no calls into the game
					if (snap.valid)
						c.body = 1, put(c.feet, snap.feet);
				}
				float cx, cy;
				if (link::Cursor(cx, cy))
				{
					// cursor pixels are Minecraft's picture's; the back buffer may be another size
					c.cursorOn = 1;
					c.cursor[0] = cx * float(bd.Width) / float(g.texW ? g.texW : bd.Width);
					c.cursor[1] = cy * float(bd.Height) / float(g.texH ? g.texH : bd.Height);
				}
				std::memcpy(m.pData, &c, sizeof(c));
				g.ctx->Unmap(g.cb, 0);
			}
			D3D11_VIEWPORT vp{ 0, 0, float(bd.Width), float(bd.Height), 0, 1 };
			g.ctx->OMSetRenderTargets(1, &g.rtv, nullptr);
			g.ctx->OMSetBlendState(g.blend, nullptr, 0xFFFFFFFF);
			g.ctx->OMSetDepthStencilState(g.noDepth, 0);
			g.ctx->RSSetState(g.raster);
			g.ctx->RSSetViewports(1, &vp);
			g.ctx->IASetInputLayout(nullptr);
			g.ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
			g.ctx->VSSetShader(g.vs, nullptr, 0);
			g.ctx->GSSetShader(nullptr, nullptr, 0);
			g.ctx->PSSetShader(g.ps, nullptr, 0);
			ID3D11ShaderResourceView* srvs[4] = { g.srv[0], g.srv[1], g.srv[2], srDepth };
			g.ctx->PSSetShaderResources(0, 4, srvs);
			g.ctx->PSSetSamplers(0, 1, &g.sampler);
			g.ctx->PSSetConstantBuffers(0, 1, &g.cb);
			g.ctx->Draw(3, 0);


			g.ctx->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, oldRtv, oldDsv);
			g.ctx->OMSetBlendState(oldBlend, oldFactor, oldMask);
			g.ctx->OMSetDepthStencilState(oldDs, oldRef);
			g.ctx->RSSetState(oldRs);
			g.ctx->RSSetViewports(nvp, oldVp);
			g.ctx->VSSetShader(oldVs, nullptr, 0);
			g.ctx->PSSetShader(oldPs, nullptr, 0);
			g.ctx->GSSetShader(oldGs, nullptr, 0);
			g.ctx->PSSetShaderResources(0, 4, oldSrv);
			g.ctx->PSSetSamplers(0, 1, &oldSampler);
			g.ctx->PSSetConstantBuffers(0, 1, &oldCb);
			g.ctx->IASetPrimitiveTopology(oldTopo);
			g.ctx->IASetInputLayout(oldLayout);
			for (auto* p : oldRtv)
				if (p)
					p->Release();
			for (auto* p : oldSrv)
				if (p)
					p->Release();
			for (IUnknown* p : std::initializer_list<IUnknown*>{ oldDsv, oldBlend, oldDs, oldRs, oldVs, oldPs, oldGs, oldSampler, oldCb, oldLayout })
				if (p)
					p->Release();
		}

		HRESULT STDMETHODCALLTYPE PresentHook(IDXGISwapChain* sc, UINT sync, UINT flags)
		{
			if (g_enabled && !g.failed)
			{
				if (!g.dev && !Setup(sc))
				{
					g.failed = true;
					Log("compositor: setup failed; off");
				}
				if (g.dev)
				{
					DXGI_SWAP_CHAIN_DESC sd;
					sc->GetDesc(&sd);
					ID3D11ShaderResourceView* srDepth = depth::EndFrame(sd.BufferDesc.Width, sd.BufferDesc.Height);
					PullFrame();
					// Only over SR3's 3D scene: no scene depth this frame means a menu, the pause screen or
					// a loading screen, and Minecraft stays out of those.
					if (g.haveFrame && srDepth && sr3::GetSnapshot().valid && !sr3::Paused())
						Draw(sc, srDepth);
				}
			}
			return g_presentOrig(sc, sync, flags);
		}

		HRESULT STDMETHODCALLTYPE ResizeHook(IDXGISwapChain* sc, UINT count, UINT w, UINT h, DXGI_FORMAT fmt, UINT flags)
		{
			Release(g.rtv);  // the back buffer goes away
			return g_resizeOrig(sc, count, w, h, fmt, flags);
		}

		// The swap chain vtable, from a throwaway device on a hidden window.
		void** SwapChainVtable()
		{
			WNDCLASSEXW wc{ sizeof(wc), 0, DefWindowProcW, 0, 0, GetModuleHandleW(nullptr), nullptr, nullptr, nullptr, nullptr, L"mcsr3_dummy" };
			RegisterClassExW(&wc);
			HWND wnd = CreateWindowExW(0, wc.lpszClassName, L"", WS_OVERLAPPEDWINDOW, 0, 0, 64, 64, nullptr, nullptr, wc.hInstance, nullptr);
			DXGI_SWAP_CHAIN_DESC d{};
			d.BufferCount = 1;
			d.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
			d.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
			d.OutputWindow = wnd;
			d.SampleDesc.Count = 1;
			d.Windowed = TRUE;
			IDXGISwapChain* sc = nullptr;
			ID3D11Device* dev = nullptr;
			ID3D11DeviceContext* ctx = nullptr;
			void** vtable = nullptr;
			if (SUCCEEDED(D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &d, &sc, &dev, nullptr, &ctx)))
			{
				vtable = *reinterpret_cast<void***>(sc);
				sc->Release();
				dev->Release();
				ctx->Release();
			}
			DestroyWindow(wnd);
			UnregisterClassW(wc.lpszClassName, wc.hInstance);
			return vtable;
		}
	}

	void SetEnabled(bool on) { g_enabled = on; }
	bool Enabled() { return g_enabled; }

	bool Install()
	{
		void** vt = SwapChainVtable();
		if (!vt)
		{
			Log("compositor: no D3D11 swap chain vtable");
			return false;
		}
		// IDXGISwapChain: 8 Present, 13 ResizeBuffers
		if (MH_CreateHook(vt[8], (void*)PresentHook, (void**)&g_presentOrig) != MH_OK || MH_CreateHook(vt[13], (void*)ResizeHook, (void**)&g_resizeOrig) != MH_OK ||
			MH_EnableHook(vt[8]) != MH_OK || MH_EnableHook(vt[13]) != MH_OK)
		{
			Log("compositor: hooking Present failed");
			return false;
		}
		Log("compositor: Present hooked");
		return true;
	}
}
