#pragma once
#include <CTxdStore.h>
#include "imgui.h"
#include <string>
#include "NavIcons.h"
#include "AssetBundle.h"
namespace WeaponIcons {
inline int hud=-1;
inline int dictionary=-1;inline bool attempted=false;
inline RwTexture* weaponTextures[47]{};
inline RwTexture* navTextures[NavIcons::Count]{};
inline const char* Asset(int type){static const char* names[]={"","brassknuckle","golfclub","nitestick","knifecur","bat","shovel","poolcue","katana","chnsaw","gun_dildo1","gun_dildo2","gun_vibe1","gun_vibe2","flowera","gun_cane","grenade","teargas","molotov","","","","colt45","silenced","desert_eagle","chromegun","sawnoff","shotgspa","micro_uzi","mp5lng","ak47","m4","tec9","cuntgun","sniper","rocketla","heatseek","flame","minigun","satchel","bomb","spraycan","fire_ex","camera","nvgoggles","irgoggles","gun_para"};return type>0&&type<47?names[type]:"";}
// Load on the game callback, not while drawing. The dedicated TXD reference
// owns native rasters until shutdown; UI borrows current D3D pointers per frame.
inline void Process(){if(attempted)return;attempted=true;char exe[MAX_PATH]{};GetModuleFileNameA(nullptr,exe,MAX_PATH);std::string path=exe;path.resize(path.find_last_of("\\/")+1);hud=CTxdStore::AddTxdSlot("valkyrie_trainer_hud");
 if(hud>=0){if(CTxdStore::LoadTxd(hud,(path+"models\\hud.txd").c_str()))CTxdStore::AddRef(hud);else{CTxdStore::RemoveTxdSlot(hud);hud=-1;}}
 path+="valkyrie-trainer-weapons.txd";
 dictionary=CTxdStore::AddTxdSlot("valkyrie_trainer_weapons");
 if(dictionary>=0){
   bool ok=false;
   if(auto bundled=AssetBundle::Get(102)){
     RwMemory memory{const_cast<unsigned char*>(bundled.data),bundled.size};
     if(auto* stream=RwStreamOpen(rwSTREAMMEMORY,rwSTREAMREAD,&memory)){
       ok=CTxdStore::LoadTxd(dictionary,stream);RwStreamClose(stream,nullptr);
     }
   }else if(GetFileAttributesA(path.c_str())!=INVALID_FILE_ATTRIBUTES)ok=CTxdStore::LoadTxd(dictionary,path.c_str());
   if(ok)CTxdStore::AddRef(dictionary);else{CTxdStore::RemoveTxdSlot(dictionary);dictionary=-1;}
 }
 auto* pool=CTxdStore::ms_pTxdPool;if(!pool)return;
 if(dictionary>=0){auto* entry=pool->GetAt(dictionary);if(entry&&entry->m_pRwDictionary)for(int i=1;i<47;++i){auto name=std::string(Asset(i))+"icon";weaponTextures[i]=RwTexDictionaryFindNamedTexture(entry->m_pRwDictionary,name.c_str());}}
 if(hud>=0){auto* entry=pool->GetAt(hud);if(entry&&entry->m_pRwDictionary)for(int i=0;i<NavIcons::Count;++i)navTextures[i]=RwTexDictionaryFindNamedTexture(entry->m_pRwDictionary,NavIcons::Names[i]);}
}
inline IDirect3DTexture9* Get(int type){auto* texture=type>0&&type<47?weaponTextures[type]:nullptr;
 if(!texture||!texture->raster)return nullptr;return *reinterpret_cast<IDirect3DTexture9**>(reinterpret_cast<unsigned char*>(texture->raster)+52);
}
inline ImTextureID Nav(int tab){
 if(tab<0||tab>=NavIcons::Count)return {};
 auto* texture=navTextures[tab];if(!texture||!texture->raster)return {};
 return reinterpret_cast<ImTextureID>(*reinterpret_cast<IDirect3DTexture9**>(reinterpret_cast<unsigned char*>(texture->raster)+52));
}

}
