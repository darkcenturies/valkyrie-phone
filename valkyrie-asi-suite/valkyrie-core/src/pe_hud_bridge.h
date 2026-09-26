#pragma once
#include <windows.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include "log.h"
#include "pe_hud_embedded.h"

// Verified SanViveLuaHudSA build only. Redirect its single HUD load call, NOT
// the embedded Lua API globally. The user's hud.lua remains byte-for-byte intact.
namespace pe_hud_bridge {
using LoadFile=int(__cdecl*)(void*,const char*,const char*);
inline LoadFile original=nullptr;
using Reader=const char*(__cdecl*)(void*,void*,size_t*);
using LoadChunk=int(__cdecl*)(void*,Reader,void*,const char*,const char*);
inline LoadChunk loadChunk=nullptr;
inline const char* __cdecl ReadEmbedded(void*,void* context,size_t* size){
    auto& consumed=*static_cast<bool*>(context);
    if(consumed){*size=0;return nullptr;}
    consumed=true;*size=sizeof(embeddedLoader)-1;return embeddedLoader;
}
inline bool checked=false;
inline int __cdecl Load(void* state,const char* path,const char* mode){
    if(path && !_stricmp(path,"hud.lua")){
        bool consumed=false;
        return loadChunk(state,&ReadEmbedded,&consumed,"@valkyrie-embedded-pe-hud",mode);
    }
    return original(state,path,mode);
}
inline void Poll(){
    if(checked)return;
    auto module=GetModuleHandleA("SanViveLuaHudSA.asi");
    if(!module)return;
    checked=true;
    char filename[MAX_PATH]{};
    if(!GetModuleFileNameA(module,filename,MAX_PATH))return;
    FILE* input=nullptr;
    if(fopen_s(&input,filename,"rb") || !input)return;
    uint64_t hash=14695981039346656037ULL;
    unsigned char buffer[8192];size_t count=0;
    while((count=fread(buffer,1,sizeof buffer,input))!=0)
        for(size_t i=0;i<count;++i)hash=(hash^buffer[i])*1099511628211ULL;
    const bool readOk=!ferror(input);fclose(input);
    if(!readOk || hash!=0x2795428a0b03c63fULL){
        logfile::Line("PE HUD bridge: unrecognized plugin build; unchanged (fingerprint=%016llX)",hash);return;
    }
    auto base=reinterpret_cast<uintptr_t>(module);
    auto site=reinterpret_cast<unsigned char*>(base+0x893E0);
    const unsigned char expected[]={0xE8,0x2B,0xF3,0xFF,0xFF};
    if(memcmp(site,expected,sizeof expected)){
        logfile::Line("PE HUD bridge: load call already owned/modified; left untouched");return;
    }
    original=reinterpret_cast<LoadFile>(base+0x88710);
    // Same five-argument lua_loadx used by this binary's verified loadfilex.
    loadChunk=reinterpret_cast<LoadChunk>(base+0x88EB0);
    DWORD old=0;
    if(!VirtualProtect(site,5,PAGE_EXECUTE_READWRITE,&old))return;
    *reinterpret_cast<int32_t*>(site+1)=static_cast<int32_t>(reinterpret_cast<uintptr_t>(&Load)-(base+0x893E5));
    DWORD ignored=0;VirtualProtect(site,5,old,&ignored);FlushInstructionCache(GetCurrentProcess(),site,5);
    // On the game/render thread, invalidate only the known HUD file timestamp.
    // The plugin reloads normally and recaches its callbacks next frame.
    *reinterpret_cast<uint64_t*>(base+0x1F2120)=0;
    logfile::Line("PE HUD bridge: verified embedded load-call bridge installed; no companion files; hud.lua not modified");
}
}
