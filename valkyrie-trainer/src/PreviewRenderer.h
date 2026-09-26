#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <d3d9.h>
#include <vector>
namespace PreviewRenderer {
struct Vertex { float x,y,z,nx,ny,nz; DWORD color; float u,v; };
struct Batch { std::vector<Vertex> vertices; IDirect3DTexture9* texture = nullptr; bool transparent = false; };
struct View { float x,y,z,radius,yaw,pitch,zoom; float inkTime=0; };
bool Draw(IDirect3DDevice9* device, const std::vector<Batch>& batches, const View& camera);
IDirect3DTexture9* Texture();
void Invalidate();
}
