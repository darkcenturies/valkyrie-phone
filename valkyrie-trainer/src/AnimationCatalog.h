#pragma once
#include "AnimationList.h"
#include <cstdint>
#include <cstring>
namespace AnimationCatalog {
inline bool Parse(const std::vector<uint8_t>& data,const std::string& library,std::vector<AnimationList::Entry>& out){
    if(data.size()<36||memcmp(data.data(),"ANP3",4)||library.empty()||library.size()>=16)return false;
    size_t pos=32;auto read=[&](uint32_t& n){if(pos+4>data.size())return false;memcpy(&n,data.data()+pos,4);pos+=4;return true;};
    uint32_t count=0;if(!read(count)||count>10000)return false;
    std::vector<AnimationList::Entry> parsed;
    for(uint32_t i=0;i<count;++i){
        if(pos+36>data.size())return false;
        size_t length=0;while(length<24&&data[pos+length])++length;
        std::string name((const char*)data.data()+pos,length);pos+=24;
        uint32_t bones=0,bytes=0,unused=0;if(!read(bones)||!read(bytes)||!read(unused)||bones>256)return false;
        for(uint32_t j=0;j<bones;++j){
            if(pos+36>data.size())return false;pos+=24;uint32_t kind=0,frames=0,id=0;
            if(!read(kind)||!read(frames)||!read(id)||(kind!=3&&kind!=4)||frames>100000)return false;
            size_t skip=(size_t)frames*(kind==4?16:10);if(skip>data.size()-pos)return false;pos+=skip;
        }
        if(!name.empty()&&name.size()<24)parsed.push_back({library,name});
    }
    out.insert(out.end(),parsed.begin(),parsed.end());return true;
}
}
