#pragma once
// CJ rig setup from Valkyrie Inventory/Atmosphere player_portrait.h.
// Dressed clothing meshes share the new private hierarchy, never the live pose.
namespace TrainerPedRig {
struct Vec {float x,y,z;};
struct Matrix {Vec right;unsigned flags;Vec up;unsigned pad1;Vec at;unsigned pad2;Vec pos;unsigned pad3;};
template<class T>T& Field(uintptr_t address){return *reinterpret_cast<T*>(address);}
template<uintptr_t Address,class Result,class... Args> Result Call(Args... args){return reinterpret_cast<Result(__cdecl*)(Args...)>(Address)(args...);}
inline uintptr_t previewClump=0,previewHierarchy=0;inline bool ownsAnimation=false;inline bool correctShoulders=false;
inline void Clear(){if(previewClump){if(ownsAnimation)Call<0x749B70,uintptr_t>(previewClump,reinterpret_cast<uintptr_t(__cdecl*)(uintptr_t,void*)>(0x734B90),static_cast<void*>(nullptr));Call<0x74A310,int>(previewClump);}previewClump=previewHierarchy=0;ownsAnimation=false;correctShoulders=false;}
uintptr_t __cdecl BindPreviewSkin(uintptr_t atomic,void* hierarchy) {
    Call<0x7C7520,uintptr_t>(atomic,hierarchy);
    return atomic; // Visit every clothing mesh, not just the first atomic.
}
bool CreatePreview(uintptr_t source) {
    if(!source)return false;
    // Clone the dressed player, not the generic model-info template. Rebind
    // every clothing mesh to the cloned hierarchy before applying animation.
    previewClump=Call<0x749F70,uintptr_t>(source);
    if(!previewClump)return false;
    previewHierarchy=Call<0x734B10,uintptr_t>(previewClump);
    const auto sourceHierarchy=Call<0x734A40,uintptr_t>(source);
    if(!previewHierarchy || previewHierarchy==sourceHierarchy ||
       (sourceHierarchy && Field<uintptr_t>(previewHierarchy+8)==Field<uintptr_t>(sourceHierarchy+8)))return false;
    const auto animation=Call<0x4D60E0,uintptr_t>(previewHierarchy);
    if(!animation)return false;
    if(!Call<0x7CD5A0,int>(Field<uintptr_t>(previewHierarchy+32),animation)) {
        Call<0x7CCF10,int>(animation);return false;
    }
    Call<0x749B70,uintptr_t>(previewClump,&BindPreviewSkin,reinterpret_cast<void*>(previewHierarchy));
    ownsAnimation=true;
    Field<int>(previewHierarchy)=0x3000;
    Matrix root{};root.right={1,0,0};root.up={0,1,0};root.at={0,0,1};root.flags=3;
    Call<0x7F0F70,uintptr_t>(Field<uintptr_t>(previewClump+4),&root,0);
    Call<0x4D6720,void>(previewClump);
    // CJ's dressed rig can have adjusted rest offsets. Keep those offsets,
    // never the live pose, associations, or pointers into the player's rig.
    const auto pluginOffset=Field<unsigned>(0xB5F878);
    const auto sourceData=Field<uintptr_t>(source+pluginOffset);
    const auto targetData=Field<uintptr_t>(previewClump+pluginOffset);
    if(!sourceData || !targetData || sourceData==targetData)return false;
    const int count=Field<int>(targetData+8),sourceCount=Field<int>(sourceData+8);
    const auto frames=Field<uintptr_t>(targetData+16),sourceFrames=Field<uintptr_t>(sourceData+16);
    if(count<=0 || count>256 || sourceCount!=count || !frames || !sourceFrames || frames==sourceFrames)return false;
    int changed=0;
    for(int i=0;i<count;++i) {
        const auto target=frames+i*24;
        int j=0;
        for(;j<sourceCount;++j)if(Field<int>(sourceFrames+j*24+20)==Field<int>(target+20))break;
        if(j==sourceCount)return false;
        const auto offset=Field<Vec>(sourceFrames+j*24+4);
        if(memcmp(reinterpret_cast<void*>(target+4),&offset,sizeof(offset)))++changed;
        Field<Vec>(target+4)=offset;
    }
    correctShoulders=true;
    for(int tag:{0x12e,0x20,0x1f,0x12d,0x16,0x15}){
        const int index=Call<0x7C51A0,int>(previewHierarchy,tag);
        if(index<0||index>=count)correctShoulders=false;
    }
    logfile::Line("portrait rig: preserved %d rest offsets, %d corrected",count,changed);
    if(!Call<0x4D4610,uintptr_t>(previewClump,0,3,1000.0f))return false; // default idle
    Call<0x4D34F0,void>(previewClump,0.1f,true);
    Call<0x7C51D0,int>(previewHierarchy);
    return true;
}
inline void Update(float seconds){
 Call<0x4D34F0,void>(previewClump,seconds,true);Call<0x7C51D0,int>(previewHierarchy);
 // Static cdecl routine: a thiscall cast leaves its argument on the x86 stack.
 if(correctShoulders)Call<0x5DF560,void>(previewClump);
}
}
