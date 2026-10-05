#pragma once
struct ID3D11DeviceContext;
struct ID3D11Device;
struct ID3D11ShaderResourceView;

// SR3's scene depth buffer: the back-buffer-sized depth target its frame binds most, found by
// watching OMSetRenderTargets / ClearDepthStencilView on the immediate context.
namespace mcsr3::depth
{
	bool Install(ID3D11Device* dev, ID3D11DeviceContext* ctx);
	// At Present: settles this frame's pick. Returns an SRV of it (owned here), or nullptr.
	ID3D11ShaderResourceView* EndFrame(unsigned width, unsigned height);
	// SR3's scene projection: depth z = a + b / viewDepth, screen scale xs, ys; false until found.
	bool Projection(float& a, float& b, float* xs = nullptr, float* ys = nullptr);
}
