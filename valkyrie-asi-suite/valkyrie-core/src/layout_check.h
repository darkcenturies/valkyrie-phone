#pragma once
#include <windows.h>
#include <cstdint>
#include <cstring>
namespace valkyrie_layout {
inline bool Bytes(uintptr_t address,const unsigned char* expected,size_t size) {
    unsigned char actual[32]{};SIZE_T read=0;
    return size<=sizeof actual && ReadProcessMemory(GetCurrentProcess(),reinterpret_cast<void*>(address),actual,size,&read)
        && read==size && memcmp(actual,expected,size)==0;
}
// Entry fingerprints identify packers, not all compatible 1.0 layouts.
// Unknown fingerprints need independent code anchors. Never authorize a
// different layout merely because it has mapped memory or is named gta_pe.exe.
inline bool Compatible10() {
    if(reinterpret_cast<uintptr_t>(GetModuleHandleA(nullptr))!=0x400000)return false;
    const unsigned char find[]={0x8B,0x44,0x24,0x04,0x85,0xC0,0x7D,0x07,0x0F,0xB6,0x05,0x74};
    const unsigned char pitch[]={0x66,0x83,0xB9,0x0E,0x01,0,0,0,0x75,0x18,0x0F,0xB7};
    const unsigned char font[]={0x8A,0x4C,0x24,0x04,0x0F,0xB6,0xC1,0x83,0xE8,0x02,0x74,0x1E};
    return Bytes(0x56E0D0,find,sizeof find) && Bytes(0x56E210,find,sizeof find)
        && Bytes(0x53FBD0,pitch,sizeof pitch) && Bytes(0x719490,font,sizeof font);
}
}
