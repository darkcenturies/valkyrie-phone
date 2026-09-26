#include <plugin.h>
#include <common.h>
#include <CBaseModelInfo.h>
#include <CStreaming.h>
#include <CAnimManager.h>
#include <CPedModelInfo.h>
#include <CPlayerPed.h>
#include <CTimer.h>
#include <CTxdStore.h>
#include <CVehicleModelInfo.h>
#include <enums/eAnimations.h>
#include <enums/ePedBones.h>
#include <NodeName.h>
#include <d3d9.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#include <fstream>
#include <string>
#include <vector>
#include "imgui.h"
#include "Render.h"
#include "ModelPreview.h"
#include "TrainerUI.h"
#include "PreviewRenderer.h"
#include "PreviewPaint.h"
#include "PreviewMesh.h"
#include "VehicleList.h"
#include "model_access.h"
#include "log.h"
#include "instrument.h"
#include "TrainerPedRig.h"

namespace {
static_assert(sizeof(RwRaster)==52, "PC RenderWare raster extension offset changed");
using PreviewRenderer::Vertex;
using PreviewRenderer::Batch;
std::vector<Batch> batches;
std::map<RwTexture*, IDirect3DTexture9*> textures;
CBaseModelInfo* held = nullptr;
RpClump* clump = nullptr;
int requested = -1, loaded = -1;
bool vehicle = false, touched = false, copied = false, failed = false;
bool pedAnimationReady = false;
bool privateCJ=false;
bool meshSkinned = false;
bool previewLogged = false;
ULONGLONG requestedAt = 0;
float yaw = 0.6f, pitch = 0.35f, zoom = 1.0f;
bool spin = true;
bool refreshPending=false;
RwV3d low{}, high{}, center{};
float radius = 1.0f;
std::string status = "Select a model";
PreviewPaint::Colors paintColors{};
int paintModel = -1;
std::map<int, int> lastPaintVariation;
std::map<RpMaterial*, int> paintMaterialSlots;
struct PedBasis { RwV3d right{}, forward{}, up{}; bool valid=false; } pedBasis;
const PreviewPaint::Data& PaintData() {
    static const auto data=[] {
        char exe[MAX_PATH]{};GetModuleFileNameA(nullptr,exe,MAX_PATH);
        std::string path=exe;path.resize(path.find_last_of("\\/")+1);
        std::ifstream file(path+"data\\carcols.dat");
        return PreviewPaint::Read(file);
    }();
    return data;
}
bool ReadPaint(unsigned index,RwRGBA& color) {
    // Read the palette operand used by the running game's material callback.
    // Limit adjusters relocate this table; the stock 0xB4E480 is not valid in PE.
    unsigned char instruction[7]{};SIZE_T bytes=0;uintptr_t address=0;
    if(ReadProcessMemory(GetCurrentProcess(),reinterpret_cast<void*>(0x4C838D),instruction,7,&bytes)
       && bytes==7 && instruction[0]==0x8A && instruction[1]==0x0C && instruction[2]==0xB5) {
        memcpy(&address,instruction+3,4);
        if(index<65536 && ReadProcessMemory(GetCurrentProcess(),reinterpret_cast<void*>(address+index*4),&color,4,&bytes) && bytes==4) {
            color.alpha=255;return true;
        }
    }
    const auto& palette=PaintData().palette;
    if(index>=palette.size())return false;
    color={palette[index][0],palette[index][1],palette[index][2],255};return true;
}

template<class T> void Release(T*& p) { if (p) { p->Release(); p = nullptr; } }
void ClearMesh() {
    batches.clear();
    for (auto& p : textures) Release(p.second);
    textures.clear();
    paintMaterialSlots.clear();
    pedBasis.valid=false;
    copied = false;
    previewLogged = false;
}
void ClearBatches() { batches.clear(); copied=false; }
void ReleaseModel() {
    if(privateCJ){TrainerPedRig::Clear();privateCJ=false;clump=nullptr;}
    if (clump) { RpClumpDestroy(clump); clump = nullptr; }
    if (held) { held->RemoveRef(); held = nullptr; }
    loaded = -1;
    pedAnimationReady = false;
}
RwV3d Transform(const RwV3d& p, const RwMatrix& m, bool point);
RwMatrix Multiply(const RwMatrix& a, const RwMatrix& b) {
    RwMatrix out{};
    out.right=Transform(a.right,b,false); out.up=Transform(a.up,b,false);
    out.at=Transform(a.at,b,false); out.pos=Transform(a.pos,b,true);
    return out;
}
int PaintSlot(const RwRGBA& c) {
    if(c.red==60 && c.green==255 && c.blue==0) return 0;
    if(c.red==255 && c.green==0 && c.blue==175) return 1;
    if(c.red==0 && c.green==255 && c.blue==255) return 2;
    if(c.red==255 && c.green==0 && c.blue==255) return 3;
    return -1;
}
RwV3d Transform(const RwV3d& p, const RwMatrix& m, bool point) {
    return {p.x*m.right.x+p.y*m.up.x+p.z*m.at.x+(point?m.pos.x:0),
            p.x*m.right.y+p.y*m.up.y+p.z*m.at.y+(point?m.pos.y:0),
            p.x*m.right.z+p.y*m.up.z+p.z*m.at.z+(point?m.pos.z:0)};
}
float Dot(const RwV3d& a,const RwV3d& b){return a.x*b.x+a.y*b.y+a.z*b.z;}
RwV3d Cross(const RwV3d& a,const RwV3d& b){return {a.y*b.z-a.z*b.y,a.z*b.x-a.x*b.z,a.x*b.y-a.y*b.x};}
bool Normalize(RwV3d& v){float len=sqrtf(Dot(v,v));if(len<0.0001f)return false;v.x/=len;v.y/=len;v.z/=len;return true;}
RpAtomic* CapturePaintSlots(RpAtomic* atomic,void*) {
    auto* g=RpAtomicGetGeometry(atomic);
    if(g) for(int i=0;i<g->matList.numMaterials;++i) {
        auto* mat=g->matList.materials[i];
        if(mat) {int slot=PaintSlot(mat->color);if(slot>=0)paintMaterialSlots[mat]=slot;}
    }
    return atomic;
}
RpAtomic* CapturePedBasis(RpAtomic* atomic,void*) {
    if(pedBasis.valid)return atomic;
    auto* h=RpSkinAtomicGetHAnimHierarchy(atomic);
    auto* frame=RpAtomicGetFrame(atomic);
    if(!h || !frame || !h->pNodeInfo || h->numNodes<=0)return atomic;
    // Read the matrices prepared on the game callback. Rebuilding here erased
    // CJ's post-hierarchy shoulder correction from the Atmosphere portrait.
    auto* pose=RpHAnimHierarchyGetMatrixArray(h);
    if(!pose)return atomic;
    RwV3d pelvis{},head{},left{},right{};bool found[4]{};
    for(int i=0;i<h->numNodes;++i) {
        int idx=h->pNodeInfo[i].nodeIndex;
        if(idx<0 || idx>=h->numNodes)continue;
        auto p=pose[idx].pos;
        if(h->flags & rpHANIMHIERARCHYLOCALSPACEMATRICES)p=Transform(p,*RwFrameGetLTM(frame),true);
        switch(h->pNodeInfo[i].nodeID) {
            case BONE_PELVIS: pelvis=p;found[0]=true;break;
            case BONE_HEAD: head=p;found[1]=true;break;
            case BONE_LEFTSHOULDER: left=p;found[2]=true;break;
            case BONE_RIGHTSHOULDER: right=p;found[3]=true;break;
        }
    }
    if(!(found[0]&&found[1]&&found[2]&&found[3]))return atomic;
    RwV3d up{head.x-pelvis.x,head.y-pelvis.y,head.z-pelvis.z};
    RwV3d side{right.x-left.x,right.y-left.y,right.z-left.z};
    if(!Normalize(up) || !Normalize(side))return atomic;
    // Correct the model's axis convention, not its animated posture. Using
    // the leaning head-to-pelvis vector as vertical tilts the entire body.
    const float ax=fabsf(up.x),ay=fabsf(up.y),az=fabsf(up.z);
    up=ax>ay && ax>az?RwV3d{up.x<0?-1.f:1.f,0,0}:
       ay>az?RwV3d{0,up.y<0?-1.f:1.f,0}:RwV3d{0,0,up.z<0?-1.f:1.f};
    float projection=Dot(side,up);
    side={side.x-projection*up.x,side.y-projection*up.y,side.z-projection*up.z};
    if(!Normalize(side))return atomic;
    auto forward=Cross(up,side);
    if(!Normalize(forward))return atomic;
    pedBasis={side,forward,up,true};
    return atomic;
}
IDirect3DTexture9* CopyTexture(IDirect3DDevice9* device, RwTexture* source) {
    if (!source || !source->raster) return nullptr;
    auto found = textures.find(source);
    if (found != textures.end()) return found->second;
    auto* raster = source->raster;
    if (raster->width <= 0 || raster->height <= 0 || raster->width > 4096 || raster->height > 4096) return nullptr;
    // PC RenderWare keeps the original Direct3D texture directly after
    // RwRaster. Reuse that native texture, including 16-bit and DXT formats.
    auto* nativeRaster=raster->parent ? raster->parent : raster;
    auto* native=*reinterpret_cast<IDirect3DTexture9**>(nativeRaster+1);
    if(native) {
        D3DSURFACE_DESC desc{};
        if(SUCCEEDED(native->GetLevelDesc(0,&desc))) {
            native->AddRef();
            textures[source]=native;
            return native;
        }
    }
    auto* img = RwImageCreate(raster->width, raster->height, 32);
    if (!img) return nullptr;
    IDirect3DTexture9* result = nullptr;
    if (RwImageAllocatePixels(img) && RwImageSetFromRaster(img, raster) && img->depth == 32 && img->cpPixels) {
        if (SUCCEEDED(device->CreateTexture(img->width,img->height,1,0,D3DFMT_A8R8G8B8,D3DPOOL_MANAGED,&result,nullptr))) {
            D3DLOCKED_RECT lock{};
            if (SUCCEEDED(result->LockRect(0,&lock,nullptr,0))) {
                for (int y=0; y<img->height; ++y) {
                    auto* src=img->cpPixels+y*img->stride;
                    auto* dst=static_cast<unsigned char*>(lock.pBits)+y*lock.Pitch;
                    for (int x=0; x<img->width; ++x) {
                        dst[x*4]=src[x*4+2]; dst[x*4+1]=src[x*4+1];
                        dst[x*4+2]=src[x*4]; dst[x*4+3]=src[x*4+3];
                    }
                }
                result->UnlockRect(0);
            } else Release(result);
        }
    }
    if(!result) logfile::Line("Preview texture copy failed: %s, raster %dx%d depth %d format 0x%X",source->name,raster->width,raster->height,raster->depth,raster->cFormat);
    RwImageDestroy(img);
    textures[source]=result;
    return result;
}
RpAtomic* CopyAtomic(RpAtomic* atomic, void* data) {
    if (!(RpAtomicGetFlags(atomic) & rpATOMICRENDER)) return atomic;
    auto* frame=RpAtomicGetFrame(atomic);
    const char* name=frame?GetFrameNodeName(frame):nullptr;
    if (vehicle && name && (strstr(name,"_dam") || strstr(name,"_vlo") || strstr(name,"_lod") || strstr(name,"_col"))) return atomic;
    auto* g=RpAtomicGetGeometry(atomic);
    if (!g || !frame || !g->morphTarget || !g->morphTarget[0].verts || g->numVertices<=0 || g->numTriangles<=0) return atomic;
    if (g->numVertices>200000 || g->numTriangles>300000 || batches.size()>512) { failed=true; return nullptr; }
    // The rendering BinMesh is authoritative. Some PE exports retain an all-zero
    // RpTriangle material table while their nine (or more) draw meshes are correct.
    std::vector<PreviewMesh::Face> triangles;
    if(g->mesh && g->mesh->numMeshes<=512 && (g->mesh->flags & ~1u)==0){
        auto* meshes=reinterpret_cast<RpMesh*>(reinterpret_cast<unsigned char*>(g->mesh+1)+g->mesh->firstMeshOffset);
        for(unsigned i=0;i<g->mesh->numMeshes;++i){
            const auto& mesh=meshes[i];unsigned material=0;
            for(;material<static_cast<unsigned>(g->matList.numMaterials);++material)if(g->matList.materials[material]==mesh.material)break;
            if(material<static_cast<unsigned>(g->matList.numMaterials))PreviewMesh::Append(mesh.indices,mesh.numIndices,(g->mesh->flags&1)!=0,g->numVertices,material,triangles);
        }
    }
    if(triangles.empty() && g->triangles)for(int i=0;i<g->numTriangles;++i){const auto& t=g->triangles[i];triangles.push_back({{t.vertIndex[0],t.vertIndex[1],t.vertIndex[2]},t.matIndex});}
    auto* device=static_cast<IDirect3DDevice9*>(data);
    auto* matrix=RwFrameGetLTM(frame);
    const RwV3d* sourceNormals=g->morphTarget[0].normals;
    std::vector<RwV3d> generatedNormals;
    if(!vehicle && !sourceNormals) {
        // Some imported peds omit normals. Generate shared vertex normals,
        // rather than shading every triangle as a separate flat polygon.
        generatedNormals.resize(g->numVertices);
        for(const auto& t:triangles) {
            if(t.vertIndex[0]>=g->numVertices || t.vertIndex[1]>=g->numVertices || t.vertIndex[2]>=g->numVertices)continue;
            const auto a=g->morphTarget[0].verts[t.vertIndex[0]],b=g->morphTarget[0].verts[t.vertIndex[1]],c=g->morphTarget[0].verts[t.vertIndex[2]];
            const auto n=Cross({b.x-a.x,b.y-a.y,b.z-a.z},{c.x-a.x,c.y-a.y,c.z-a.z});
            for(int j=0;j<3;++j) {auto& v=generatedNormals[t.vertIndex[j]];v.x+=n.x;v.y+=n.y;v.z+=n.z;}
        }
        for(auto& n:generatedNormals)Normalize(n);
        sourceNormals=generatedNormals.data();
    }
    std::vector<RwV3d> posed(g->numVertices), posedNormals;
    if(sourceNormals) posedNormals.resize(g->numVertices);
    auto* skin=vehicle?nullptr:RpSkinGeometryGetSkin(g);
    auto* hierarchy=skin?RpSkinAtomicGetHAnimHierarchy(atomic):nullptr;
    const auto* weights=skin?RpSkinGetVertexBoneWeights(skin):nullptr;
    const auto* indices=skin?RpSkinGetVertexBoneIndices(skin):nullptr;
    const auto* bind=skin?RpSkinGetSkinToBoneMatrices(skin):nullptr;
    auto* pose=hierarchy?RpHAnimHierarchyGetMatrixArray(hierarchy):nullptr;
    std::vector<RwMatrix> boneMatrices;
    if(weights && indices && bind && pose && hierarchy->numNodes>0) {
        unsigned count=std::min<unsigned>(RpSkinGetNumBones(skin),hierarchy->numNodes);
        boneMatrices.reserve(count);
        for(unsigned b=0;b<count;++b) boneMatrices.push_back(Multiply(bind[b],pose[b]));
        meshSkinned=meshSkinned || count>0;
    }
    for(int i=0;i<g->numVertices;++i) {
        const auto src=g->morphTarget[0].verts[i];
        const auto srcNormal=sourceNormals?sourceNormals[i]:RwV3d{};
        RwV3d p{},n{}; float total=0;
        if(!boneMatrices.empty()) {
            const float ws[4]={weights[i].w0,weights[i].w1,weights[i].w2,weights[i].w3};
            for(unsigned k=0;k<4;++k) {
                if(ws[k]<=0.f) continue;
                // Four 8-bit indices are packed into one word per vertex.
                unsigned bone=(indices[i] >> (k*8)) & 0xffu;
                if(bone>=boneMatrices.size()) continue;
                auto q=Transform(src,boneMatrices[bone],true);
                p.x+=q.x*ws[k];p.y+=q.y*ws[k];p.z+=q.z*ws[k];
                if(!posedNormals.empty()) {
                    q=Transform(srcNormal,boneMatrices[bone],false);
                    n.x+=q.x*ws[k];n.y+=q.y*ws[k];n.z+=q.z*ws[k];
                }
                total+=ws[k];
            }
        }
        if(total>0.0001f) {
            p.x/=total;p.y/=total;p.z/=total;
            n.x/=total;n.y/=total;n.z/=total;
        } else {p=src;n=srcNormal;}
        const bool worldSkin=total>0.0001f && hierarchy && !(hierarchy->flags & rpHANIMHIERARCHYLOCALSPACEMATRICES);
        posed[i]=worldSkin?p:Transform(p,*matrix,true);
        if(!posedNormals.empty()) posedNormals[i]=worldSkin?n:Transform(n,*matrix,false);
    }
    std::map<unsigned, size_t> materials;
    for (const auto& t:triangles) {
        if (t.matIndex>=g->matList.numMaterials || t.vertIndex[0]>=g->numVertices || t.vertIndex[1]>=g->numVertices || t.vertIndex[2]>=g->numVertices) continue;
            auto* mat=g->matList.materials[t.matIndex];
        if (!mat) continue;
        if (!materials.count(t.matIndex)) {
            materials[t.matIndex]=batches.size();
            Batch b; b.texture=CopyTexture(device,mat->texture); b.transparent=mat->color.alpha<255;
            if(!previewLogged)logfile::Line("Preview material: model %d texture %s native %d RGBA %u,%u,%u,%u geometry 0x%X normals %s",loaded,mat->texture?mat->texture->name:"(solid)",b.texture!=nullptr,mat->color.red,mat->color.green,mat->color.blue,mat->color.alpha,g->flags,generatedNormals.empty()?"model":"generated");
            batches.push_back(std::move(b));
        }
        auto& b=batches[materials[t.matIndex]];
        RwV3d p[3];
        for(int j=0;j<3;++j) p[j]=posed[t.vertIndex[j]];
        RwV3d a{p[1].x-p[0].x,p[1].y-p[0].y,p[1].z-p[0].z}, c{p[2].x-p[0].x,p[2].y-p[0].y,p[2].z-p[0].z};
        RwV3d face{a.y*c.z-a.z*c.y,a.z*c.x-a.x*c.z,a.x*c.y-a.y*c.x};
        for(int j=0;j<3;++j) {
            unsigned ix=t.vertIndex[j];
            RwV3d n=!posedNormals.empty()?posedNormals[ix]:face;
            float len=sqrtf(n.x*n.x+n.y*n.y+n.z*n.z); if(len>0.00001f) {n.x/=len;n.y/=len;n.z/=len;}
            RwTexCoords uv=g->texCoords[0]?g->texCoords[0][ix]:RwTexCoords{0,0};
            auto col=mat->color;
            int slot=-1;
            if(vehicle) {
                auto it=paintMaterialSlots.find(mat);
                if(it!=paintMaterialSlots.end())slot=it->second;
                if(slot>=0 && paintModel==loaded) ReadPaint(paintColors[slot],col);
            }
            // Vehicle diffuse colors include intentional black tires/trim and
            // glass tint. Preserve them; only paint markers use the palette.
            if(!vehicle && !(g->flags & rpGEOMETRYMODULATEMATERIALCOLOR)) col.red=col.green=col.blue=255;
            b.vertices.push_back({p[j].x,p[j].y,p[j].z,n.x,n.y,n.z,D3DCOLOR_ARGB(col.alpha,col.red,col.green,col.blue),uv.u,uv.v});
            low.x=std::min(low.x,p[j].x); low.y=std::min(low.y,p[j].y); low.z=std::min(low.z,p[j].z);
            high.x=std::max(high.x,p[j].x); high.y=std::max(high.y,p[j].y); high.z=std::max(high.z,p[j].z);
        }
    }
    return atomic;
}
bool RenderMesh(IDirect3DDevice9* d) {
    if(!copied) {
        ClearBatches(); meshSkinned=false; low={1e20f,1e20f,1e20f};high={-1e20f,-1e20f,-1e20f};
        if(vehicle) {
            paintMaterialSlots.clear();
            RpClumpForAllAtomics(clump,CapturePaintSlots,nullptr);
            RpClumpForAllAtomics(clump,CopyAtomic,d);
        } else {
            RpClumpForAllAtomics(clump,CapturePedBasis,nullptr);
            RpClumpForAllAtomics(clump,CopyAtomic,d);
            if(pedBasis.valid) {
                low={1e20f,1e20f,1e20f};high={-1e20f,-1e20f,-1e20f};
                for(auto& batch:batches)for(auto& vertex:batch.vertices) {
                    RwV3d p{vertex.x,vertex.y,vertex.z},n{vertex.nx,vertex.ny,vertex.nz};
                    vertex.x=Dot(p,pedBasis.right);vertex.y=Dot(p,pedBasis.forward);vertex.z=Dot(p,pedBasis.up);
                    vertex.nx=Dot(n,pedBasis.right);vertex.ny=Dot(n,pedBasis.forward);vertex.nz=Dot(n,pedBasis.up);
                    low.x=std::min(low.x,vertex.x);low.y=std::min(low.y,vertex.y);low.z=std::min(low.z,vertex.z);
                    high.x=std::max(high.x,vertex.x);high.y=std::max(high.y,vertex.y);high.z=std::max(high.z,vertex.z);
                }
            }
        }
        copied=true;
        if(failed || batches.empty()) { failed=true; status="This model has no supported preview mesh"; return false; }
        center={(low.x+high.x)*.5f,(low.y+high.y)*.5f,(low.z+high.z)*.5f};
        float x=high.x-low.x,y=high.y-low.y,z=high.z-low.z;
        radius=std::max(.01f,.5f*sqrtf(x*x+y*y+z*z));
        if(!std::isfinite(radius)) {failed=true;status="Invalid model bounds";return false;}
        status=vehicle?"Original game paint":"Idle stance";
        if(!previewLogged) {
            logfile::Line("Preview ready: %s %d, %zu material batches%s%s", vehicle?"vehicle":"ped", loaded, batches.size(),meshSkinned?", CPU skinning":"",pedBasis.valid?", bone orientation":"");
            instrument::NoteNumber("preview model",loaded);
            previewLogged=true;
        }
    }
    return PreviewRenderer::Draw(d,batches,{center.x,center.y,center.z,radius,yaw,pitch,zoom,GetTickCount64()*.001f});
}
}
namespace ModelPreview {
void BeforeSkinChange(){ReleaseModel();ClearMesh();requestedAt=GetTickCount64();failed=false;status="Loading game model...";}
void BeginFrame(){touched=false;}
void Refresh(const char* kind,int id){if(requested==id && vehicle==(strcmp(kind,"vehicle")==0)){refreshPending=true;status="Loading game model...";failed=false;requestedAt=GetTickCount64();}}
void EndFrame(){if(!touched){requested=-1;ClearMesh();InvalidateDeviceObjects();}}
void InvalidateDeviceObjects(){PreviewRenderer::Invalidate();}
void UpdatePedPose(float seconds){
    if(privateCJ){TrainerPedRig::Update(seconds);}
    else {
        RpAnimBlendClumpUpdateAnimations(clump,seconds,true);
        RpClumpForAllAtomics(clump,[](RpAtomic* a,void*)->RpAtomic* {
            auto* hierarchy=RpSkinAtomicGetHAnimHierarchy(a);
            if(hierarchy)RpHAnimHierarchyUpdateMatrices(hierarchy);
            return a;
        },nullptr);
    }
    copied=false;
}
void Process(){
    if(!Render::IsMenuOpen()) requested=-1;
    if(loaded!=requested || refreshPending){ReleaseModel();ClearMesh();refreshPending=false;}
    if(clump && !vehicle && pedAnimationReady) {
        // GTA's timestep is in 1/50-second units; the animation API takes seconds.
        const float seconds=std::clamp(*reinterpret_cast<float*>(0xB7CB5C)*0.02f,0.0f,0.06f);
        UpdatePedPose(seconds);
    }
    if(requested<0 || held || failed || GetTickCount64()-requestedAt<180)return;
    auto* info=static_cast<CBaseModelInfo*>(valkyrie_models::Find(requested));
    if(!info || info->GetModelType()!=(vehicle?MODEL_INFO_VEHICLE:MODEL_INFO_PED)) {status="Choose a valid model from the list";failed=true;return;}
    if(!info->m_pRwObject){
        if(GetTickCount64()-requestedAt>10000){status="Model did not stream. Select it again to retry.";failed=true;return;}
        CStreaming::RequestModel(requested,0);status="Loading game model...";return;
    }
    if(info->m_nTxdIndex>=0) {
        CStreaming::RequestTxdModel(info->m_nTxdIndex,0);
        auto* pool=CTxdStore::ms_pTxdPool;
        auto* txd=pool && info->m_nTxdIndex<pool->m_nSize?pool->GetAt(info->m_nTxdIndex):nullptr;
        if(!txd || !txd->m_pRwDictionary) {
            if(GetTickCount64()-requestedAt>10000){status="Model textures did not stream. Select it again to retry.";failed=true;return;}
            status="Loading model textures...";return;
        }
    }
    info->AddRef();held=info;loaded=requested;
    // Isolated RW instance only: never CPed/CVehicle or CWorld::Add.
    if(info->m_nTxdIndex>=0) { CTxdStore::PushCurrentTxd(); CTxdStore::SetCurrentTxd(info->m_nTxdIndex); }
    RwObject* object=nullptr;
    if(!vehicle && requested==0){
        auto* player=FindPlayerPed();
        // The dressed player is the authoritative CJ rig. Use the model template
        // only when CJ is not the live player; never change the player's skin.
        auto* source=player && player->m_nModelIndex==0?player->m_pRwClump:reinterpret_cast<RpClump*>(info->m_pRwObject);
        privateCJ=true;
        if(TrainerPedRig::CreatePreview(reinterpret_cast<uintptr_t>(source)))object=reinterpret_cast<RwObject*>(TrainerPedRig::previewClump);
    } else object=info->CreateInstance();
    if(info->m_nTxdIndex>=0) CTxdStore::PopCurrentTxd();
    if(!object || RwObjectGetType(object)!=rpCLUMP){status="Model cannot create a preview clump";failed=true;ReleaseModel();return;}
    clump=reinterpret_cast<RpClump*>(object);
    if(vehicle) {
        const std::vector<PreviewPaint::Colors>* choices=nullptr;
        for(const auto& entry:VehicleList::Get())if(entry.id==requested) {
            auto found=PaintData().vehicles.find(entry.name);
            if(found!=PaintData().vehicles.end())choices=&found->second;
            break;
        }
        unsigned count=choices?static_cast<unsigned>(choices->size()):0;
        paintModel=requested;
        if(count) {
            auto [it, inserted]=lastPaintVariation.try_emplace(requested,-1);
            int& previous=it->second;
            int variation=(previous+1)%static_cast<int>(count);
            previous=variation;
            paintColors=(*choices)[variation];
            logfile::Line("Preview paint: model %d variation %d/%u palette %u,%u,%u,%u",requested,variation+1,count,paintColors[0],paintColors[1],paintColors[2],paintColors[3]);
        } else paintColors={}; // GTA's default paint indices when no carcols entry exists.
    } else if(privateCJ){
        TrainerPedRig::Update(0);pedAnimationReady=true;
    } else {
        auto* pi=static_cast<CPedModelInfo*>(info);
        if(!RpAnimBlendClumpIsInitialized(clump)) RpAnimBlendClumpInit(clump);
        RpAnimBlendClumpRemoveAllAssociations(clump);
        pedAnimationReady=reinterpret_cast<CAnimBlendAssociation*(__cdecl*)(RpClump*,int,int,float)>(0x4D4610)(clump,ANIM_GROUP_DEFAULT,ANIM_DEFAULT_IDLE_STANCE,1000.0f)!=nullptr;
        // CapturePedBasis runs on the first draw and caches the axis correction.
        // Publish idle skeleton matrices now, not on the following game tick:
        // imported DFF rest matrices can use a different vertical axis.
        // Match the isolated Atmosphere/CJ portrait warm-up: zero delta leaves
        // the new association at zero blend weight and still exposes rest pose.
        if(pedAnimationReady)UpdatePedPose(0.1f);
        logfile::Line("Preview ped idle setup: model %d default group ready %d (model group %d)",requested,pedAnimationReady,pi->m_nAnimType);
    }
}
void Draw(const char* kind,int id,float width,float height){
    touched=true;
    bool isVehicle=strcmp(kind,"vehicle")==0;
    if(requested!=id || vehicle!=isVehicle){requested=id;vehicle=isVehicle;requestedAt=GetTickCount64();failed=false;status="Loading game model...";yaw=isVehicle?.6f:0.f;pitch=isVehicle?.35f:0.f;zoom=1;}

    float size=std::max(80.f,std::min(width,height));
    if(spin)yaw+=std::min(ImGui::GetIO().DeltaTime,.05f)*.45f;
    bool ready=loaded==requested && clump && !failed;
    if(ready){auto* d=reinterpret_cast<IDirect3DDevice9*>(GetD3DDevice());ready=d && RenderMesh(d);}
    const auto imagePos=ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("##model-orbit",ImVec2(size,size));
    auto* draw=ImGui::GetWindowDrawList();
    if(ready) draw->AddImage(reinterpret_cast<ImTextureID>(PreviewRenderer::Texture()),imagePos,ImVec2(imagePos.x+size,imagePos.y+size));

    bool hovered=ImGui::IsItemHovered();
    if(ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left)){spin=false;yaw+=ImGui::GetIO().MouseDelta.x*.012f;pitch=std::clamp(pitch+ImGui::GetIO().MouseDelta.y*.009f,-1.3f,1.3f);}
    if(hovered) ImGui::SetItemKeyOwner(ImGuiKey_MouseWheelY);
    if(hovered)zoom=std::clamp(zoom-ImGui::GetIO().MouseWheel*.12f,.65f,2.5f);
    if(!ready){
        draw->PushClipRect(imagePos,{imagePos.x+size,imagePos.y+size},true);
        draw->AddText(ImGui::GetFont(),ImGui::GetFontSize(),{imagePos.x+10,imagePos.y+size*.4f},IM_COL32(217,209,191,255),status.c_str(),nullptr,std::max(20.f,size-20));
        draw->PopClipRect();
    }
    TrainerUI::Toggle("Rotate",&spin);TrainerUI::Inline(100);
    if(TrainerUI::SmallButton("Reset view")){yaw=vehicle?.6f:0.f;pitch=vehicle?.35f:0.f;zoom=1;spin=true;}
    TrainerUI::Inline(70);ImGui::BeginDisabled(!failed);
    if(TrainerUI::SmallButton("Retry")){refreshPending=true;failed=false;requestedAt=GetTickCount64();status="Loading game model...";}
    ImGui::EndDisabled();
    if(hovered)ImGui::SetTooltip("Drag to turn / scroll to zoom");
}
}
