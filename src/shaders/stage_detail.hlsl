#include "stage_detail.hlsli"
cbuffer DetailConstants:register(b0){float4 dimensions;float4 response;float4 accent;float4 base;};
struct Pixel {float4 position:SV_POSITION;float2 uv:TEXCOORD0;};
float4 DetailPS(Pixel p):SV_TARGET{return ThemeDetail(p.uv,dimensions.x/dimensions.y,dimensions.z,response.x,response.y,response.z,accent.rgb,base.rgb,(int)dimensions.w,response.w);}
