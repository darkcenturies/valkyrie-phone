#pragma once
// Same exterior-only WebsiteInk kernel/noise as Inventory/Atmosphere's portrait_pixels.h.
// Run on GPU to avoid a readback and 96 CPU probes per background pixel each frame.
static const char* kPreviewInk=R"hlsl(
sampler2D source : register(s0);
sampler2D tapSource : register(s1);
float4 params : register(c0);
float hash(float2 p){return frac(sin(dot(p,float2(127.1,311.7)))*43758.5453123);}
float noise(float2 p){float2 i=floor(p),f=frac(p);f=f*f*(3-2*f);return lerp(lerp(hash(i),hash(i+float2(1,0)),f.x),lerp(hash(i+float2(0,1)),hash(i+1),f.x),f.y);}
float4 main(float2 uv:TEXCOORD0):COLOR0 {
 float4 pixel=tex2D(source,uv);
 if(pixel.a>0)return float4(pixel.rgb/max(pixel.a,0.0001),pixel.a);
 float distance=1e6;
 [loop] for(int i=0;i<96;++i){float4 tap=tex2Dlod(tapSource,float4((i+.5)/96.0,.5,0,0));float2 at=uv+tap.xy;
  if(all(at>=0)&&all(at<=1)&&tex2Dlod(source,float4(at,0,0)).a>0.5)distance=min(distance,tap.z);}
 if(distance>99999)return 0;
 float2 p=uv*params.xy;
 float breathe=noise(float2(p.x,params.y-p.y)/55+float2(params.z*.62,-params.z*.37));
 float edge=5.4*(.68+1.35*breathe);float t=saturate((distance-(edge-.9))/1.8);float ink=1-t*t*(3-2*t);
 return float4(0,0,0,ink>=.02?ink:0);
}
)hlsl";
