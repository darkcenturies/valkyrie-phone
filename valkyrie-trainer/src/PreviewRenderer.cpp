#include "PreviewRenderer.h"
#include <algorithm>
#include <cmath>
#include <d3dcompiler.h>
#include <cstdio>
#include "PreviewInk.h"
namespace PreviewRenderer {
namespace {
constexpr unsigned kSize=512;
IDirect3DTexture9* target=nullptr;
IDirect3DSurface9* depth=nullptr;
IDirect3DTexture9* outlined=nullptr;
IDirect3DTexture9* tapTexture=nullptr;
IDirect3DPixelShader9* inkShader=nullptr;
template<class T> void Release(T*& p) { if(p){p->Release();p=nullptr;} }
// D3D state blocks do not hold render targets/depth surfaces. Preserve those
// separately, including MRTs, and restore viewport after restoring targets.
struct DeviceState {
    IDirect3DDevice9* d; IDirect3DStateBlock9* block=nullptr;
    IDirect3DSurface9* rt[4]{}; IDirect3DSurface9* ds=nullptr;
    D3DVIEWPORT9 viewport{}; D3DMATRIX world{},view{},projection{}; unsigned count=1;
    explicit DeviceState(IDirect3DDevice9* device):d(device) {
        D3DCAPS9 caps{}; if(SUCCEEDED(d->GetDeviceCaps(&caps))) count=std::min(4u,(unsigned)caps.NumSimultaneousRTs);
        if(FAILED(d->CreateStateBlock(D3DSBT_ALL,&block))) return;
        block->Capture();
        for(unsigned i=0;i<count;++i) d->GetRenderTarget(i,&rt[i]);
        d->GetDepthStencilSurface(&ds); d->GetViewport(&viewport);
        d->GetTransform(D3DTS_WORLD,&world);d->GetTransform(D3DTS_VIEW,&view);d->GetTransform(D3DTS_PROJECTION,&projection);
    }
    ~DeviceState() {
        if(!block) return;
        d->SetDepthStencilSurface(nullptr);
        for(unsigned i=1;i<count;++i) d->SetRenderTarget(i,nullptr);
        d->SetRenderTarget(0,rt[0]);
        for(unsigned i=1;i<count;++i) d->SetRenderTarget(i,rt[i]);
        d->SetDepthStencilSurface(ds);block->Apply();d->SetViewport(&viewport);
        d->SetTransform(D3DTS_WORLD,&world);d->SetTransform(D3DTS_VIEW,&view);d->SetTransform(D3DTS_PROJECTION,&projection);
        for(auto*& p:rt) Release(p);Release(ds);Release(block);
    }
};
D3DMATRIX Identity() { D3DMATRIX m{};m._11=m._22=m._33=m._44=1;return m; }
}
IDirect3DTexture9* Texture(){return outlined;}
void Invalidate(){Release(target);Release(depth);Release(outlined);Release(inkShader);Release(tapTexture);}
bool Draw(IDirect3DDevice9* d,const std::vector<Batch>& batches,const View& camera){
    DeviceState saved(d);if(!saved.block)return false;
    const auto [cx,cy0,cz,radius,yaw,pitch,zoom,inkTime]=camera;
    if(!target && FAILED(d->CreateTexture(kSize,kSize,1,D3DUSAGE_RENDERTARGET,D3DFMT_A8R8G8B8,D3DPOOL_DEFAULT,&target,nullptr))) return false;
    if(!depth && FAILED(d->CreateDepthStencilSurface(kSize,kSize,D3DFMT_D16,D3DMULTISAMPLE_NONE,0,TRUE,&depth,nullptr))) return false;
    IDirect3DSurface9* surface=nullptr;
    if(FAILED(target->GetSurfaceLevel(0,&surface))) return false;
    d->SetDepthStencilSurface(nullptr);
    for(unsigned i=1;i<saved.count;++i) d->SetRenderTarget(i,nullptr);
    HRESULT hr=d->SetRenderTarget(0,surface);Release(surface);if(FAILED(hr)) return false;
    if(FAILED(d->SetDepthStencilSurface(depth))) return false;
    D3DVIEWPORT9 vp{0,0,kSize,kSize,0,1};d->SetViewport(&vp);
    d->Clear(0,nullptr,D3DCLEAR_TARGET|D3DCLEAR_ZBUFFER,0,1,0);
    d->SetVertexShader(nullptr);d->SetPixelShader(nullptr);
    d->SetFVF(D3DFVF_XYZ|D3DFVF_NORMAL|D3DFVF_DIFFUSE|D3DFVF_TEX1);
    d->SetRenderState(D3DRS_ZENABLE,TRUE);d->SetRenderState(D3DRS_ZWRITEENABLE,TRUE);
    d->SetRenderState(D3DRS_ZFUNC,D3DCMP_LESSEQUAL);d->SetRenderState(D3DRS_CULLMODE,D3DCULL_NONE);
    d->SetRenderState(D3DRS_FILLMODE,D3DFILL_SOLID);d->SetRenderState(D3DRS_FOGENABLE,FALSE);
    d->SetRenderState(D3DRS_SHADEMODE,D3DSHADE_GOURAUD);
    d->SetRenderState(D3DRS_STENCILENABLE,FALSE);d->SetRenderState(D3DRS_SCISSORTESTENABLE,FALSE);
    d->SetRenderState(D3DRS_CLIPPLANEENABLE,0);d->SetRenderState(D3DRS_ALPHABLENDENABLE,TRUE);
    d->SetRenderState(D3DRS_SRCBLEND,D3DBLEND_SRCALPHA);d->SetRenderState(D3DRS_DESTBLEND,D3DBLEND_INVSRCALPHA);
    d->SetRenderState(D3DRS_BLENDOP,D3DBLENDOP_ADD);d->SetRenderState(D3DRS_SEPARATEALPHABLENDENABLE,TRUE);
    d->SetRenderState(D3DRS_SRCBLENDALPHA,D3DBLEND_ONE);d->SetRenderState(D3DRS_DESTBLENDALPHA,D3DBLEND_INVSRCALPHA);
    d->SetRenderState(D3DRS_BLENDOPALPHA,D3DBLENDOP_ADD);
    d->SetRenderState(D3DRS_COLORWRITEENABLE,15);
    d->SetRenderState(D3DRS_ALPHATESTENABLE,TRUE);d->SetRenderState(D3DRS_ALPHAFUNC,D3DCMP_GREATER);d->SetRenderState(D3DRS_ALPHAREF,8);
    d->SetRenderState(D3DRS_LIGHTING,TRUE);d->SetRenderState(D3DRS_NORMALIZENORMALS,TRUE);
    d->SetRenderState(D3DRS_AMBIENT,0x00909090);d->SetRenderState(D3DRS_COLORVERTEX,TRUE);
    d->SetRenderState(D3DRS_DIFFUSEMATERIALSOURCE,D3DMCS_COLOR1);d->SetRenderState(D3DRS_AMBIENTMATERIALSOURCE,D3DMCS_COLOR1);
    d->SetRenderState(D3DRS_EMISSIVEMATERIALSOURCE,D3DMCS_MATERIAL);
    d->SetRenderState(D3DRS_SPECULARMATERIALSOURCE,D3DMCS_MATERIAL);
    d->SetRenderState(D3DRS_SPECULARENABLE,FALSE);d->SetRenderState(D3DRS_VERTEXBLEND,D3DVBF_DISABLE);d->SetRenderState(D3DRS_INDEXEDVERTEXBLENDENABLE,FALSE);
    d->SetRenderState(D3DRS_SRGBWRITEENABLE,FALSE);d->SetRenderState(D3DRS_DEPTHBIAS,0);d->SetRenderState(D3DRS_SLOPESCALEDEPTHBIAS,0);
    D3DLIGHT9 light{};light.Type=D3DLIGHT_DIRECTIONAL;light.Diffuse={.7f,.7f,.7f,1};light.Direction={-.4f,.6f,-.7f};
    d->SetLight(0,&light);d->LightEnable(0,TRUE);for(unsigned i=1;i<8;++i)d->LightEnable(i,FALSE);
    D3DMATERIAL9 material{};material.Diffuse=material.Ambient={1,1,1,1};d->SetMaterial(&material);
    d->SetTextureStageState(1,D3DTSS_COLOROP,D3DTOP_DISABLE);d->SetTextureStageState(1,D3DTSS_ALPHAOP,D3DTOP_DISABLE);
    d->SetTextureStageState(0,D3DTSS_TEXCOORDINDEX,0);d->SetTextureStageState(0,D3DTSS_TEXTURETRANSFORMFLAGS,D3DTTFF_DISABLE);
    d->SetTextureStageState(0,D3DTSS_RESULTARG,D3DTA_CURRENT);
    d->SetSamplerState(0,D3DSAMP_MINFILTER,D3DTEXF_LINEAR);d->SetSamplerState(0,D3DSAMP_MAGFILTER,D3DTEXF_LINEAR);
    d->SetSamplerState(0,D3DSAMP_MIPFILTER,D3DTEXF_NONE);d->SetSamplerState(0,D3DSAMP_ADDRESSU,D3DTADDRESS_WRAP);d->SetSamplerState(0,D3DSAMP_ADDRESSV,D3DTADDRESS_WRAP);d->SetSamplerState(0,D3DSAMP_SRGBTEXTURE,FALSE);
    d->SetSamplerState(0,D3DSAMP_MAXMIPLEVEL,0);d->SetSamplerState(0,D3DSAMP_MIPMAPLODBIAS,0);
    D3DMATRIX w=Identity(); w._41=-cx;w._42=-cy0;w._43=-cz;d->SetTransform(D3DTS_WORLD,&w);
    // Right/up/forward basis: GTA is Z-up, D3D's view is Y-up / +Z-forward.
    // A positive preview pitch puts the eye above GTA's Z-up models.
    float cy=cosf(yaw),sy=sinf(yaw),cp=cosf(pitch),sp=-sinf(pitch);
    D3DMATRIX v{};v._11=cy;v._21=sy;v._31=0;v._12=-sy*sp;v._22=cy*sp;v._32=cp;
    v._13=sy*cp;v._23=-cy*cp;v._33=sp;v._43=radius*3.1f*zoom;v._44=1;d->SetTransform(D3DTS_VIEW,&v);
    float nearZ=std::max(.001f,radius*.01f),farZ=radius*30;
    D3DMATRIX p{};p._11=p._22=2.41421356f;p._33=farZ/(farZ-nearZ);p._34=1;p._43=-nearZ*farZ/(farZ-nearZ);d->SetTransform(D3DTS_PROJECTION,&p);
    bool ok=true;
    for(int pass=0;pass<2;++pass) for(auto& b:batches) {
        if(b.transparent!=(pass==1))continue;
        d->SetRenderState(D3DRS_ZWRITEENABLE,pass==0);
        d->SetTexture(0,b.texture);
        d->SetTextureStageState(0,D3DTSS_COLOROP,b.texture?D3DTOP_MODULATE:D3DTOP_SELECTARG2);
        d->SetTextureStageState(0,D3DTSS_COLORARG1,D3DTA_TEXTURE);d->SetTextureStageState(0,D3DTSS_COLORARG2,D3DTA_DIFFUSE);
        d->SetTextureStageState(0,D3DTSS_ALPHAOP,b.texture?D3DTOP_MODULATE:D3DTOP_SELECTARG2);
        d->SetTextureStageState(0,D3DTSS_ALPHAARG1,D3DTA_TEXTURE);d->SetTextureStageState(0,D3DTSS_ALPHAARG2,D3DTA_DIFFUSE);
        for(size_t start=0;start<b.vertices.size();start+=30000) {
            UINT n=(UINT)std::min<size_t>(30000,b.vertices.size()-start);
            if(FAILED(d->DrawPrimitiveUP(D3DPT_TRIANGLELIST,n/3,b.vertices.data()+start,sizeof(Vertex))))ok=false;
        }
    }
    if(!ok)return false;
    if(!inkShader) {
        ID3DBlob *code=nullptr,*errors=nullptr;
        HRESULT compiled=D3DCompile(kPreviewInk,strlen(kPreviewInk),"Atmosphere silhouette",nullptr,nullptr,"main","ps_3_0",D3DCOMPILE_OPTIMIZATION_LEVEL3,0,&code,&errors);
        if(errors){OutputDebugStringA(static_cast<const char*>(errors->GetBufferPointer()));fprintf(stderr,"%s",static_cast<const char*>(errors->GetBufferPointer()));errors->Release();}
        if(FAILED(compiled))return false;
        HRESULT created=d->CreatePixelShader(static_cast<const DWORD*>(code->GetBufferPointer()),&inkShader);code->Release();
        if(FAILED(created))return false;
    }
    if(!outlined && FAILED(d->CreateTexture(kSize,kSize,1,D3DUSAGE_RENDERTARGET,D3DFMT_A8R8G8B8,D3DPOOL_DEFAULT,&outlined,nullptr)))return false;
    IDirect3DSurface9* output=nullptr;if(FAILED(outlined->GetSurfaceLevel(0,&output)))return false;
    d->SetDepthStencilSurface(nullptr);hr=d->SetRenderTarget(0,output);output->Release();if(FAILED(hr))return false;
    d->SetRenderState(D3DRS_ZENABLE,FALSE);d->SetRenderState(D3DRS_ZWRITEENABLE,FALSE);
    d->SetRenderState(D3DRS_ALPHATESTENABLE,FALSE);d->SetRenderState(D3DRS_ALPHABLENDENABLE,FALSE);
    d->SetRenderState(D3DRS_LIGHTING,FALSE);d->SetPixelShader(inkShader);d->SetTexture(0,target);
    d->SetSamplerState(0,D3DSAMP_ADDRESSU,D3DTADDRESS_CLAMP);d->SetSamplerState(0,D3DSAMP_ADDRESSV,D3DTADDRESS_CLAMP);
    d->SetSamplerState(0,D3DSAMP_MINFILTER,D3DTEXF_POINT);d->SetSamplerState(0,D3DSAMP_MAGFILTER,D3DTEXF_POINT);
    float taps[96][4]{};int at=0;
    for(int i=0;i<16;++i){float a=i*.3926991f,dx=cosf(a),dy=sinf(a),weight=std::max(.25f,1+.30f*(dx*-.55f+dy*-.83f));
        for(int r=1;r<=6;++r){float t=r/6.f,px=1+(5.4f*2.15f-1)*t*t;taps[at][0]=floorf(.5f+dx*px)/kSize;taps[at][1]=floorf(.5f-dy*px)/kSize;taps[at++][2]=px/weight;}}
    if(!tapTexture){
        if(FAILED(d->CreateTexture(96,1,1,0,D3DFMT_A32B32G32R32F,D3DPOOL_MANAGED,&tapTexture,nullptr)))return false;
        D3DLOCKED_RECT lock{};if(FAILED(tapTexture->LockRect(0,&lock,nullptr,0)))return false;memcpy(lock.pBits,taps,sizeof(taps));tapTexture->UnlockRect(0);
    }
    d->SetTexture(1,tapTexture);d->SetSamplerState(1,D3DSAMP_MINFILTER,D3DTEXF_POINT);d->SetSamplerState(1,D3DSAMP_MAGFILTER,D3DTEXF_POINT);d->SetSamplerState(1,D3DSAMP_MIPFILTER,D3DTEXF_NONE);d->SetSamplerState(1,D3DSAMP_SRGBTEXTURE,FALSE);
    float params[4]={(float)kSize,(float)kSize,inkTime,0};d->SetPixelShaderConstantF(0,params,1);
    struct Quad {float x,y,z,w,u,v;};const float edge=kSize-.5f;
    Quad quad[]={{-.5f,-.5f,0,1,0,0},{edge,-.5f,0,1,1,0},{-.5f,edge,0,1,0,1},{edge,edge,0,1,1,1}};
    d->SetFVF(D3DFVF_XYZRHW|D3DFVF_TEX1);
    return SUCCEEDED(d->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP,2,quad,sizeof(Quad)));
}
}
