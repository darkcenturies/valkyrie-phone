#pragma once
#include <windows.h>
#include <cstdint>
#include <cstring>
#include <cmath>

namespace valkyrie_waypoint {
template<class T> inline bool Read(uintptr_t address, T& value) {
    SIZE_T count=0;
    return ReadProcessMemory(GetCurrentProcess(),reinterpret_cast<void*>(address),&value,sizeof value,&count) && count==sizeof value;
}
// Resolve the radar array from the engine's counter/in-use checks. Limit
// adjusters relocate this array; the stock address and map cursor are unsafe
// substitutes for the position of the player's placed marker.
inline bool Position(float& x,float& y) {
    std::uint8_t code[27]{};
    if(!Read(0x582880,code))return false;
    const std::uint8_t stride[]={0x8d,0x14,0x80,0xc1,0xe2,0x03,0xc1,0xe9,0x10,0x66,0x3b,0x8a};
    if(std::memcmp(code,stride,sizeof stride)!=0 || code[18]!=0xf6 || code[19]!=0x82 || code[24]!=2)return false;
    std::uint32_t counterAddress=0,flagAddress=0,handle=0;
    std::memcpy(&counterAddress,code+12,4);std::memcpy(&flagAddress,code+20,4);
    if(counterAddress<0x10000 || flagAddress!=counterAddress+0x11 || !Read(0xBA6774,handle) || handle==0xffffffff || !handle)return false;
    std::uint8_t trace[40]{};
    if(!Read(counterAddress-0x14+uintptr_t(handle&0xffff)*40,trace))return false;
    std::uint16_t counter=0;std::memcpy(&counter,trace+0x14,2);
    if(counter!=(handle>>16) || !(trace[0x25]&2) || trace[0x24]!=41)return false;
    std::memcpy(&x,trace+8,4);std::memcpy(&y,trace+12,4);
    return std::isfinite(x)&&std::isfinite(y)&&std::fabs(x)<200000&&std::fabs(y)<200000;
}
}
