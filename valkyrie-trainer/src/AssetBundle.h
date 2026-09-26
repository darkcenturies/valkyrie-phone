#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
namespace AssetBundle {
struct View { const unsigned char* data=nullptr; DWORD size=0; explicit operator bool() const { return data&&size; } };
inline View Get(unsigned id){
 HMODULE module=nullptr; static const char marker=0;
 if(!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,&marker,&module))return {};
 auto resource=FindResourceA(module,MAKEINTRESOURCEA(id),RT_RCDATA);
 if(!resource)return {};
 auto loaded=LoadResource(module,resource);
 return {loaded?static_cast<const unsigned char*>(LockResource(loaded)):nullptr,SizeofResource(module,resource)};
}
}
