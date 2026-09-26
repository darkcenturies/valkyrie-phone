#pragma once
#include <cstdint>
#include <vector>
namespace PreviewMesh {
struct Face {uint16_t vertIndex[3];unsigned matIndex;};
inline void Append(const uint16_t* indices,unsigned count,bool strip,unsigned vertices,unsigned material,std::vector<Face>& faces){
 if(!indices || count>900000)return;
 auto emit=[&](uint16_t a,uint16_t b,uint16_t c){if(a!=b&&b!=c&&a!=c&&a<vertices&&b<vertices&&c<vertices)faces.push_back({{a,b,c},material});};
 if(strip){for(unsigned i=2;i<count;++i){if(i&1)emit(indices[i-1],indices[i-2],indices[i]);else emit(indices[i-2],indices[i-1],indices[i]);}}
 else for(unsigned i=0;i+2<count;i+=3)emit(indices[i],indices[i+1],indices[i+2]);
}
}
