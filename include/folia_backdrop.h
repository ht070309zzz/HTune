// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
#include <d3d11.h>
#include <memory>
namespace stage {struct RiftFrame;
class FoliaBackdrop {
public:FoliaBackdrop();~FoliaBackdrop();void Create(ID3D11Device*);void Draw(const RiftFrame&,ID3D11RenderTargetView*,UINT width,UINT height);
private:struct Impl;std::unique_ptr<Impl> impl_;
};}
