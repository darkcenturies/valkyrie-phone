#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <d3d9.h>
#include <string>
namespace Branding {
void SetDevice(IDirect3DDevice9* device);
void SetDirectory(const std::string& directory);
void Info();
}
