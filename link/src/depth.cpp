#include "depth.h"

#include "log.h"

#include <MinHook.h>
#include <d3d11.h>
#include <windows.h>

#include <cmath>
#include <cstdio>
#include <map>
#include <tuple>
#include <mutex>

namespace mcsr3::depth
{
	namespace
	{
		using OMSetRTFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, ID3D11RenderTargetView* const*, ID3D11DepthStencilView*);
		using ClearDSFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11DepthStencilView*, UINT, FLOAT, UINT8);
		using MapFn = HRESULT(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11Resource*, UINT, D3D11_MAP, UINT, D3D11_MAPPED_SUBRESOURCE*);
		using UnmapFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11Resource*, UINT);
		OMSetRTFn g_omOrig;
		ClearDSFn g_clearOrig;
		MapFn g_mapOrig;
		UnmapFn g_unmapOrig;

		// Dev: <game>\mcsr3\scanproj present -> for a few frames, every constant buffer written through
		// Map/Unmap is searched for a perspective projection matrix; finds go to the log.
		int g_scanFrames;
		std::map<ID3D11Resource*, std::pair<void*, UINT>> g_mapped;
		int g_found;
		// Candidates from the current scan: (xs, ys, a, b) -> how many constant buffers held it.
		struct Proj { float xs, ys, a, b; };
		std::map<std::tuple<int, int, int, int>, std::pair<Proj, int>> g_votes;
		Proj g_proj;
		bool g_haveProj;
		bool g_logFinds;

		void ScanProjection(const float* m, std::size_t n)
		{
			for (std::size_t i = 0; i + 16 <= n && g_found < 256; i += 4)
			{
				const float* p = m + i;
				auto z = [](float v) { return v > -1e-4f && v < 1e-4f; };
				if (!(p[0] > 0.3f && p[0] < 10 && p[5] > 0.3f && p[5] < 10 && z(p[1]) && z(p[2]) && z(p[3]) && z(p[4]) && z(p[6]) && z(p[7]) && z(p[15])))
					continue;
				bool rowMajor = (p[11] > 0.999f && p[11] < 1.001f) || (p[11] < -0.999f && p[11] > -1.001f);
				bool colMajor = (p[14] > 0.999f && p[14] < 1.001f) || (p[14] < -0.999f && p[14] > -1.001f);
				if (!rowMajor && !colMajor)
					continue;
				float a = p[10], b = rowMajor ? p[14] : p[11];
				// D3D z = a + b/t (t = view depth): near where z = 0, far where z = 1
				float nearZ = -b / a, farZ = b / (1 - a);
				if (g_logFinds)
					Log("depth: projection (%s) at +%zu: %.5f %.5f | %.6f %.6f -> near %.4f far %.1f, vfov %.2f deg, aspect %.4f",
						rowMajor ? "row" : "col", i * 4, p[0], p[5], a, b, nearZ, farZ, 2 * std::atan(1 / p[5]) * 57.29578f, p[5] / p[0]);
				if (nearZ > 0.001f && farZ > nearZ)
				{
					auto key = std::make_tuple(int(p[0] * 1000), int(p[5] * 1000), int(a * 1e6f), int(b * 1e5f));
					auto& v = g_votes[key];
					v.first = { p[0], p[5], a, b };
					v.second++;
				}
				g_found++;
			}
		}

		HRESULT STDMETHODCALLTYPE MapHook(ID3D11DeviceContext* ctx, ID3D11Resource* res, UINT sub, D3D11_MAP type, UINT flags, D3D11_MAPPED_SUBRESOURCE* out)
		{
			HRESULT hr = g_mapOrig(ctx, res, sub, type, flags, out);
			if (g_scanFrames > 0 && SUCCEEDED(hr) && out && (type == D3D11_MAP_WRITE_DISCARD || type == D3D11_MAP_WRITE_NO_OVERWRITE))
			{
				D3D11_RESOURCE_DIMENSION dim;
				res->GetType(&dim);
				if (dim == D3D11_RESOURCE_DIMENSION_BUFFER)
				{
					D3D11_BUFFER_DESC d;
					static_cast<ID3D11Buffer*>(res)->GetDesc(&d);
					if (d.BindFlags & D3D11_BIND_CONSTANT_BUFFER)
						g_mapped[res] = { out->pData, d.ByteWidth };
				}
			}
			return hr;
		}

		void STDMETHODCALLTYPE UnmapHook(ID3D11DeviceContext* ctx, ID3D11Resource* res, UINT sub)
		{
			if (g_scanFrames > 0)
			{
				auto it = g_mapped.find(res);
				if (it != g_mapped.end())
				{
					ScanProjection(static_cast<const float*>(it->second.first), it->second.second / 4);
					g_mapped.erase(it);
				}
			}
			g_unmapOrig(ctx, res, sub);
		}
		ID3D11Device* g_dev;
		ID3D11DeviceContext* g_ctx;

		struct Use
		{
			int binds = 0;
			int clears = 0;
			float clearDepth = 0;
		};
		std::map<ID3D11Resource*, Use> g_frame;  // this frame's back-buffer-sized depth targets (not AddRef'd)
		unsigned g_w, g_h;

		ID3D11Texture2D* g_pick;  // AddRef'd
		ID3D11ShaderResourceView* g_srv;
		float g_pickClear;

		bool BackBufferSized(ID3D11DepthStencilView* dsv, ID3D11Resource** res)
		{
			dsv->GetResource(res);
			D3D11_RESOURCE_DIMENSION dim;
			(*res)->GetType(&dim);
			bool ok = false;
			if (dim == D3D11_RESOURCE_DIMENSION_TEXTURE2D)
			{
				D3D11_TEXTURE2D_DESC d;
				static_cast<ID3D11Texture2D*>(*res)->GetDesc(&d);
				ok = d.Width == g_w && d.Height == g_h;
			}
			(*res)->Release();  // the DSV keeps it alive for the frame
			return ok;
		}

		void STDMETHODCALLTYPE OMSetRTHook(ID3D11DeviceContext* ctx, UINT n, ID3D11RenderTargetView* const* rtv, ID3D11DepthStencilView* dsv)
		{
			ID3D11Resource* res;
			if (ctx == g_ctx && dsv && g_w && BackBufferSized(dsv, &res))
				g_frame[res].binds++;
			g_omOrig(ctx, n, rtv, dsv);
		}

		void STDMETHODCALLTYPE ClearDSHook(ID3D11DeviceContext* ctx, ID3D11DepthStencilView* dsv, UINT flags, FLOAT d, UINT8 s)
		{
			ID3D11Resource* res;
			if (ctx == g_ctx && dsv && g_w && (flags & D3D11_CLEAR_DEPTH) && BackBufferSized(dsv, &res))
			{
				Use& u = g_frame[res];
				u.clears++;
				u.clearDepth = d;
			}
			g_clearOrig(ctx, dsv, flags, d, s);
		}

		DXGI_FORMAT SrvFormat(DXGI_FORMAT f)
		{
			switch (f)
			{
			case DXGI_FORMAT_R24G8_TYPELESS: return DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
			case DXGI_FORMAT_R32_TYPELESS: return DXGI_FORMAT_R32_FLOAT;
			case DXGI_FORMAT_R32G8X24_TYPELESS: return DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
			case DXGI_FORMAT_R16_TYPELESS: return DXGI_FORMAT_R16_UNORM;
			default: return DXGI_FORMAT_UNKNOWN;
			}
		}

		// Dev: <game>\mcsr3\dumpdepth present -> the picked depth texture as raw rows in depth.raw (+ depth.txt).
		void MaybeDump()
		{
			std::wstring flag = DataDir() + L"dumpdepth";
			if (!g_pick || GetFileAttributesW(flag.c_str()) == INVALID_FILE_ATTRIBUTES)
				return;
			DeleteFileW(flag.c_str());
			D3D11_TEXTURE2D_DESC d;
			g_pick->GetDesc(&d);
			d.Usage = D3D11_USAGE_STAGING;
			d.BindFlags = 0;
			d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
			d.MiscFlags = 0;
			ID3D11Texture2D* staging = nullptr;
			if (FAILED(g_dev->CreateTexture2D(&d, nullptr, &staging)))
			{
				Log("depth: dump: no staging texture");
				return;
			}
			g_ctx->CopyResource(staging, g_pick);
			D3D11_MAPPED_SUBRESOURCE m;
			if (SUCCEEDED(g_ctx->Map(staging, 0, D3D11_MAP_READ, 0, &m)))
			{
				if (FILE* f = _wfopen((DataDir() + L"depth.raw").c_str(), L"wb"))
				{
					for (UINT y = 0; y < d.Height; y++)
						std::fwrite(static_cast<const char*>(m.pData) + std::size_t(y) * m.RowPitch, 1, m.RowPitch, f);
					std::fclose(f);
				}
				if (FILE* f = _wfopen((DataDir() + L"depth.txt").c_str(), L"w"))
				{
					std::fprintf(f, "%u %u %d %u %g\n", d.Width, d.Height, int(d.Format), m.RowPitch, g_pickClear);
					std::fclose(f);
				}
				g_ctx->Unmap(staging, 0);
				Log("depth: dumped %ux%u format %d pitch %u (cleared to %g)", d.Width, d.Height, int(d.Format), m.RowPitch, g_pickClear);
			}
			staging->Release();
		}
	}

	bool Install(ID3D11Device* dev, ID3D11DeviceContext* ctx)
	{
		g_dev = dev;
		g_ctx = ctx;
		void** vt = *reinterpret_cast<void***>(ctx);
		// ID3D11DeviceContext: 14 Map, 15 Unmap, 33 OMSetRenderTargets, 53 ClearDepthStencilView
		if (MH_CreateHook(vt[33], (void*)OMSetRTHook, (void**)&g_omOrig) != MH_OK || MH_CreateHook(vt[53], (void*)ClearDSHook, (void**)&g_clearOrig) != MH_OK ||
			MH_CreateHook(vt[14], (void*)MapHook, (void**)&g_mapOrig) != MH_OK || MH_CreateHook(vt[15], (void*)UnmapHook, (void**)&g_unmapOrig) != MH_OK ||
			MH_EnableHook(vt[33]) != MH_OK || MH_EnableHook(vt[53]) != MH_OK || MH_EnableHook(vt[14]) != MH_OK || MH_EnableHook(vt[15]) != MH_OK)
		{
			Log("depth: hooking the context failed");
			return false;
		}
		return true;
	}

	bool Projection(float& a, float& b, float* xs, float* ys)
	{
		a = g_proj.a;
		b = g_proj.b;
		if (xs)
			*xs = g_proj.xs;
		if (ys)
			*ys = g_proj.ys;
		return g_haveProj;
	}

	ID3D11ShaderResourceView* EndFrame(unsigned width, unsigned height)
	{
		// Most binds this frame wins: the scene's depth target is bound for every geometry pass.
		ID3D11Resource* best = nullptr;
		Use bestUse;
		for (auto& [res, u] : g_frame)
			if (u.binds > bestUse.binds)
				best = res, bestUse = u;
		if (best && best != g_pick)
		{
			if (g_srv)
				g_srv->Release(), g_srv = nullptr;
			if (g_pick)
				g_pick->Release();
			g_pick = static_cast<ID3D11Texture2D*>(best);
			g_pick->AddRef();
			D3D11_TEXTURE2D_DESC d;
			g_pick->GetDesc(&d);
			D3D11_SHADER_RESOURCE_VIEW_DESC sd{};
			sd.Format = SrvFormat(d.Format);
			sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
			sd.Texture2D.MipLevels = 1;
			HRESULT hr = (d.BindFlags & D3D11_BIND_SHADER_RESOURCE) && sd.Format != DXGI_FORMAT_UNKNOWN ? g_dev->CreateShaderResourceView(g_pick, &sd, &g_srv) : E_FAIL;
			std::string all;
			for (auto& [res, u] : g_frame)
			{
				char b[96];
				std::snprintf(b, sizeof(b), " [%p binds %d clears %d to %g]", (void*)res, u.binds, u.clears, u.clearDepth);
				all += b;
			}
			Log("depth: picked %p format %d bind 0x%x, SRV %s; candidates:%s", (void*)g_pick, int(d.Format), d.BindFlags, SUCCEEDED(hr) ? "ok" : "none", all.c_str());
		}
		if (best)
			g_pickClear = bestUse.clearDepth;
		MaybeDump();
		// The scene projection: one frame every 2 s, every constant buffer the frame wrote is searched; the
		// perspective matrix with the screen's aspect that most buffers hold wins. (scanproj: also log finds.)
		if (g_scanFrames > 0 && --g_scanFrames == 0)
		{
			const std::pair<Proj, int>* winner = nullptr;
			for (auto& [k, v] : g_votes)
				if (std::fabs(v.first.ys / v.first.xs - float(width) / float(height)) < 0.01f && (!winner || v.second > winner->second))
					winner = &v;
			if (winner)
			{
				bool changed = !g_haveProj || std::fabs(winner->first.a - g_proj.a) > 1e-6f || std::fabs(winner->first.b - g_proj.b) > 1e-6f;
				g_proj = winner->first;
				g_haveProj = true;
				if (changed)
					Log("depth: scene projection xs %.5f ys %.5f, z = %.6f + %.6f/t (near %.4f) from %d buffers", g_proj.xs, g_proj.ys, g_proj.a, g_proj.b, -g_proj.b / g_proj.a, winner->second);
			}
			g_votes.clear();
			g_logFinds = false;
		}
		std::wstring scanFlag = DataDir() + L"scanproj";
		bool flag = GetFileAttributesW(scanFlag.c_str()) != INVALID_FILE_ATTRIBUTES;
		static ULONGLONG lastScan;
		if (g_scanFrames == 0 && (flag || GetTickCount64() - lastScan > 2000))
		{
			if (flag)
				DeleteFileW(scanFlag.c_str()), g_logFinds = true;
			lastScan = GetTickCount64();
			g_scanFrames = 2;  // this frame's writes are scanned; settled at the end of the next
			g_found = 0;
		}
		g_frame.clear();
		g_w = width;
		g_h = height;
		return best ? g_srv : nullptr;
	}
}
