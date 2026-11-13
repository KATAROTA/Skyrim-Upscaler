#pragma once

// Native DXMT 9839e55 ABI: src/d3d11/d3d11_interfaces.hpp.
// No NVIDIA runtime, PDPerf feature or shared texture is used by this backend.
#include <d3d11.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <RE/R/Renderer.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <chrono>
#include <cstring>
#include <utility>

namespace b20 {
using Microsoft::WRL::ComPtr;
struct TemporalDesc {
    UINT InputContentWidth, InputContentHeight;
    BOOL AutoExposure, InReset, DepthReversed, MotionVectorInDisplayRes;
    ID3D11Resource *Color, *Depth, *MotionVector, *Output;
    FLOAT MotionVectorScaleX, MotionVectorScaleY, PreExposure;
    ID3D11Resource* ExposureTexture;
    FLOAT JitterOffsetX, JitterOffsetY;
};
static_assert(sizeof(TemporalDesc) == 88);
struct __declspec(uuid("43ace3ce-1956-448b-a4eb-aee68bdeb283")) ContextExt : IUnknown {
    virtual void STDMETHODCALLTYPE TemporalUpscale(const TemporalDesc*) = 0;
    virtual void STDMETHODCALLTYPE BeginUAVOverlap() = 0;
    virtual void STDMETHODCALLTYPE EndUAVOverlap() = 0;
};
struct __declspec(uuid("19a8e35a-38be-418f-94e3-9f7323936870")) ContextExt1 : ContextExt {
    virtual HRESULT STDMETHODCALLTYPE CheckFeatureSupport(UINT, void*, UINT) = 0;
};

struct Source {
    ID3D11Texture2D *color{}, *depth{}, *motion{};
    ID3D11ShaderResourceView *colorSRV{}, *depthSRV{}, *motionSRV{};
    ID3D11RenderTargetView* colorRTV{};
    ID3D11DepthStencilView* depthDSV{};
    D3D11_TEXTURE2D_DESC colorDesc{}, depthDesc{}, motionDesc{};
    bool directCapable{};
};
inline bool CanResolveDirect(const Source& s);
struct SourceCache {
    Source value{};
    std::array<ComPtr<ID3D11Texture2D>,3> textures;
    std::array<ComPtr<ID3D11View>,5> views;
    bool valid=false;
    bool Matches(const Source& s, UINT w, UINT h) const {
        return valid && value.colorDesc.Width==w && value.colorDesc.Height==h &&
            value.color==s.color && value.depth==s.depth && value.motion==s.motion &&
            value.colorSRV==s.colorSRV && value.depthSRV==s.depthSRV && value.motionSRV==s.motionSRV &&
            value.colorRTV==s.colorRTV && value.depthDSV==s.depthDSV;
    }
    void Store(const Source& s) {
        textures[0]=s.color; textures[1]=s.depth; textures[2]=s.motion;
        views[0]=s.colorSRV; views[1]=s.depthSRV; views[2]=s.motionSRV;
        views[3]=s.colorRTV; views[4]=s.depthDSV;
        value=s; valid=true;
    }
};
inline bool SameResource(ID3D11View* view, ID3D11Texture2D* texture) {
    if (!view || !texture) return false;
    ComPtr<ID3D11Resource> r;
    view->GetResource(r.GetAddressOf());
    return r.Get() == static_cast<ID3D11Resource*>(texture);
}
inline bool GetSource(Source& s, UINT w, UINT h) {
    auto* renderer = RE::BSGraphics::Renderer::GetSingleton();
    if (!renderer) return false;
    auto& targets = renderer->GetRuntimeData().renderTargets;
    auto& color = targets[RE::RENDER_TARGETS::kMAIN];
    auto& motion = targets[RE::RENDER_TARGETS::kMOTION_VECTOR];
    auto& depth = renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kMAIN];
    s.color = reinterpret_cast<ID3D11Texture2D*>(color.texture);
    s.depth = reinterpret_cast<ID3D11Texture2D*>(depth.texture);
    s.motion = reinterpret_cast<ID3D11Texture2D*>(motion.texture);
    s.colorSRV = reinterpret_cast<ID3D11ShaderResourceView*>(color.SRV);
    s.depthSRV = reinterpret_cast<ID3D11ShaderResourceView*>(depth.depthSRV);
    s.motionSRV = reinterpret_cast<ID3D11ShaderResourceView*>(motion.SRV);
    s.colorRTV = reinterpret_cast<ID3D11RenderTargetView*>(color.RTV);
    s.depthDSV = reinterpret_cast<ID3D11DepthStencilView*>(depth.views[0]);
    static SourceCache cache;
    if (cache.Matches(s,w,h)) { s=cache.value; return true; }
    cache=SourceCache{};
    if (!s.color || !s.depth || !s.motion || !s.colorSRV || !s.depthSRV || !s.motionSRV) return false;
    if (!SameResource(s.colorSRV, s.color) || !SameResource(s.depthSRV, s.depth) || !SameResource(s.motionSRV, s.motion)) return false;
    s.color->GetDesc(&s.colorDesc); s.depth->GetDesc(&s.depthDesc); s.motion->GetDesc(&s.motionDesc);
    auto valid = [&](const D3D11_TEXTURE2D_DESC& d) {
        return d.Width == w && d.Height == h && d.SampleDesc.Count == 1 && d.ArraySize == 1 && d.MipLevels == 1;
    };
    const bool supported=valid(s.colorDesc) && valid(s.depthDesc) && valid(s.motionDesc) &&
        s.colorDesc.Format == DXGI_FORMAT_R11G11B10_FLOAT &&
        s.depthDesc.Format == DXGI_FORMAT_R24G8_TYPELESS &&
        s.motionDesc.Format == DXGI_FORMAT_R16G16_FLOAT;
    if(supported) { s.directCapable=CanResolveDirect(s); cache.Store(s); }
    return supported;
}

// A direct resolve may displace SRVs which alias kMAIN or its depth.
// Preserve only those slots, across all shader stages. PS t0-t2 are already
// handled by GraphicsState. Explicitly unbind conflicts before the direct draw.
struct ResolveAliases {
    using Getter=decltype(&ID3D11DeviceContext::PSGetShaderResources);
    using Setter=decltype(&ID3D11DeviceContext::PSSetShaderResources);
    struct Slot { Setter set{}; UINT index{}; ID3D11ShaderResourceView* view{}; };
    ID3D11DeviceContext* c;
    std::array<Slot,6*D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT> saved{};
    UINT used=0;
    bool unbound=false;
    ResolveAliases(ID3D11DeviceContext* context,const Source& s,bool direct):c(context) {
        if(!direct)return;
        Capture(&ID3D11DeviceContext::PSGetShaderResources,&ID3D11DeviceContext::PSSetShaderResources,s,3);
        Capture(&ID3D11DeviceContext::VSGetShaderResources,&ID3D11DeviceContext::VSSetShaderResources,s,0);
        Capture(&ID3D11DeviceContext::GSGetShaderResources,&ID3D11DeviceContext::GSSetShaderResources,s,0);
        Capture(&ID3D11DeviceContext::HSGetShaderResources,&ID3D11DeviceContext::HSSetShaderResources,s,0);
        Capture(&ID3D11DeviceContext::DSGetShaderResources,&ID3D11DeviceContext::DSSetShaderResources,s,0);
        Capture(&ID3D11DeviceContext::CSGetShaderResources,&ID3D11DeviceContext::CSSetShaderResources,s,0);
    }
    void Capture(Getter get,Setter set,const Source& s,UINT first) {
        constexpr UINT slots=D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT;
        ID3D11ShaderResourceView* views[slots]{};
        (c->*get)(first,slots-first,views);
        for(UINT i=0;i<slots-first;++i) {
            auto* view=views[i];
            if(!view)continue;
            ComPtr<ID3D11Resource> resource;
            view->GetResource(resource.GetAddressOf());
            if(resource.Get()==static_cast<ID3D11Resource*>(s.color)||resource.Get()==static_cast<ID3D11Resource*>(s.depth))
                saved[used++]={set,i+first,view};
            else view->Release();
        }
    }
    void Unbind() {
        ID3D11ShaderResourceView* empty=nullptr;
        for(UINT i=0;i<used;++i)(c->*saved[i].set)(saved[i].index,1,&empty);
        unbound=true;
    }
    ~ResolveAliases() {
        // Constructed before GraphicsState: engine OM bindings are restored first.
        for(UINT i=0;i<used;++i) {
            auto& slot=saved[i];
            if(unbound)(c->*slot.set)(slot.index,1,&slot.view);
            slot.view->Release();
        }
    }
    ResolveAliases(const ResolveAliases&)=delete;
    ResolveAliases& operator=(const ResolveAliases&)=delete;
};

inline bool CanResolveDirect(const Source& s) {
    if(!SameResource(s.colorRTV,s.color)||!SameResource(s.depthDSV,s.depth))return false;
    D3D11_RENDER_TARGET_VIEW_DESC r{};s.colorRTV->GetDesc(&r);
    D3D11_DEPTH_STENCIL_VIEW_DESC d{};s.depthDSV->GetDesc(&d);
    return r.Format==DXGI_FORMAT_R11G11B10_FLOAT && r.ViewDimension==D3D11_RTV_DIMENSION_TEXTURE2D && r.Texture2D.MipSlice==0 &&
        d.Format==DXGI_FORMAT_D24_UNORM_S8_UINT && d.ViewDimension==D3D11_DSV_DIMENSION_TEXTURE2D && d.Texture2D.MipSlice==0 &&
        !(d.Flags&D3D11_DSV_READ_ONLY_DEPTH);
}

// Isolate the two fullscreen passes. No writes to engine resources occur
// until all resources/shaders have been created and both passes dispatched.
struct GraphicsState {
    ID3D11DeviceContext* c;
    ID3D11RenderTargetView* rt[8]{};
    ID3D11DepthStencilView* ds{};
    ID3D11VertexShader* vs{}; ID3D11PixelShader* ps{};
    ID3D11GeometryShader* gs{}; ID3D11HullShader* hs{}; ID3D11DomainShader* dom{};
    ID3D11InputLayout* layout{};
    ID3D11BlendState* blend{}; ID3D11DepthStencilState* depth{}; ID3D11RasterizerState* raster{};
    ID3D11ShaderResourceView* srvs[3]{};
    ID3D11Buffer* cb{}; ID3D11Predicate* predicate{};
    BOOL predValue{}; FLOAT blendFactor[4]{}; UINT sampleMask{}, stencilRef{};
    D3D11_PRIMITIVE_TOPOLOGY topology{};
    D3D11_VIEWPORT vp[16]{}; D3D11_RECT scissors[16]{};
    UINT nvp=16, nsc=16, nrt=0;
    template<class T> static void Release(T* p) { if (p) p->Release(); }
    explicit GraphicsState(ID3D11DeviceContext* ctx) : c(ctx) {
        c->OMGetRenderTargets(8,rt,&ds);
        for (UINT i=0;i<8;++i) if (rt[i]) nrt=i+1;
        c->VSGetShader(&vs,nullptr,nullptr); c->PSGetShader(&ps,nullptr,nullptr);
        c->GSGetShader(&gs,nullptr,nullptr); c->HSGetShader(&hs,nullptr,nullptr); c->DSGetShader(&dom,nullptr,nullptr);
        c->IAGetInputLayout(&layout); c->IAGetPrimitiveTopology(&topology);
        c->OMGetBlendState(&blend,blendFactor,&sampleMask); c->OMGetDepthStencilState(&depth,&stencilRef);
        c->RSGetState(&raster); c->RSGetViewports(&nvp,vp); c->RSGetScissorRects(&nsc,scissors);
        c->PSGetShaderResources(0,3,srvs); c->PSGetConstantBuffers(0,1,&cb);
        c->GetPredication(&predicate,&predValue);
    }
    ~GraphicsState() {
        ID3D11ShaderResourceView* empty[3]{};
        c->PSSetShaderResources(0,3,empty);
        c->OMSetRenderTargets(nrt,rt,ds);
        c->VSSetShader(vs,nullptr,0); c->PSSetShader(ps,nullptr,0);
        c->GSSetShader(gs,nullptr,0); c->HSSetShader(hs,nullptr,0); c->DSSetShader(dom,nullptr,0);
        c->IASetInputLayout(layout); c->IASetPrimitiveTopology(topology);
        c->OMSetBlendState(blend,blendFactor,sampleMask); c->OMSetDepthStencilState(depth,stencilRef);
        c->RSSetState(raster); c->RSSetViewports(nvp,vp); c->RSSetScissorRects(nsc,scissors);
        c->PSSetShaderResources(0,3,srvs); c->PSSetConstantBuffers(0,1,&cb);
        c->SetPredication(predicate,predValue);
        for(auto* p:rt) Release(p); for(auto* p:srvs) Release(p);
        Release(ds); Release(vs); Release(ps); Release(gs); Release(hs); Release(dom);
        Release(layout); Release(blend); Release(depth); Release(raster); Release(cb); Release(predicate);
    }
    GraphicsState(const GraphicsState&)=delete;
    GraphicsState& operator=(const GraphicsState&)=delete;
};

struct Buffers {
    UINT w{}, h{};
    ComPtr<ID3D11Texture2D> input, motion, depth, output, resolved, fullDepth;
    ComPtr<ID3D11RenderTargetView> inputRTV, motionRTV, resolvedRTV;
    ComPtr<ID3D11DepthStencilView> depthDSV, fullDepthDSV;
    ComPtr<ID3D11ShaderResourceView> depthSRV, outputSRV, inputSRV;
    ComPtr<ID3D11UnorderedAccessView> outputUAV;
};
struct alignas(16) Params { UINT iw,ih,ow,oh; FLOAT sharp, padding[3]; };
static_assert(sizeof(Params)==32);

struct CpuProfile {
    using Clock=std::chrono::steady_clock;
    struct Totals {
        UINT w{},h{};
        float sharp{};
        bool direct{};
        unsigned count{};
        std::array<double,5> us{};
    };
    UINT w,h;
    float sharp;
    bool direct;
    Clock::time_point last=Clock::now();
    std::array<double,5> elapsed{};
    unsigned stage=0;
    explicit CpuProfile(UINT width,UINT height,float amount,bool fast):w(width),h(height),sharp(amount),direct(fast) {}
    void Mark() {
        const auto now=Clock::now();
        if(stage<elapsed.size()) elapsed[stage++]=std::chrono::duration<double,std::micro>(now-last).count();
        last=now;
    }
    ~CpuProfile() {
        if(stage!=4)return;
        Mark(); // state/alias restoration has completed before this destructor
        static Totals totals;
        if(totals.w!=w||totals.h!=h||totals.sharp!=sharp||totals.direct!=direct) {
            totals=Totals{};totals.w=w;totals.h=h;totals.sharp=sharp;totals.direct=direct;
        }
        for(unsigned i=0;i<5;++i)totals.us[i]+=elapsed[i];
        if(++totals.count==240) {
            const double n=totals.count;
            const double total=totals.us[0]+totals.us[1]+totals.us[2]+totals.us[3]+totals.us[4];
            logger::info("Build22 CPU submit {}x{} sharp={} direct={}: capture={:.1f}us prepare={:.1f}us dispatch={:.1f}us resolve={:.1f}us restore={:.1f}us total={:.1f}us ({} frames)",
                w,h,sharp,direct,totals.us[0]/n,totals.us[1]/n,totals.us[2]/n,totals.us[3]/n,totals.us[4]/n,total/n,totals.count);
            totals.count=0;totals.us.fill(0.0);
        }
    }
};

class NativeMetalFX {
    ComPtr<ContextExt1> ext;
    ComPtr<ID3D11Device> dev;
    ComPtr<ID3D11DeviceContext> ctx;
    ComPtr<ID3D11VertexShader> vs;
    ComPtr<ID3D11PixelShader> preparePS, resolvePS;
    ComPtr<ID3D11RasterizerState> raster;
    ComPtr<ID3D11DepthStencilState> depthWrite;
    ComPtr<ID3D11BlendState> blend;
    ComPtr<ID3D11Buffer> constants;
    Params lastParams{};
    bool paramsValid=false;
    Buffers b;
    UINT outW{},outH{};
    bool resetHistory=true;
    unsigned frames=0;
    bool Check(HRESULT hr,const char* operation) {
        if (SUCCEEDED(hr)) return true;
        logger::error("Build20 {} failed HRESULT=0x{:08X}",operation,static_cast<unsigned>(hr));
        return false;
    }
    bool Texture(UINT w,UINT h,DXGI_FORMAT format,UINT bind,ComPtr<ID3D11Texture2D>& out) {
        D3D11_TEXTURE2D_DESC d{};
        d.Width=w;d.Height=h;d.MipLevels=1;d.ArraySize=1;d.Format=format;
        d.SampleDesc.Count=1;d.Usage=D3D11_USAGE_DEFAULT;d.BindFlags=bind;
        return Check(dev->CreateTexture2D(&d,nullptr,out.ReleaseAndGetAddressOf()),"CreateTexture2D");
    }
    bool DepthView(ID3D11Texture2D* tex,DXGI_FORMAT format,ComPtr<ID3D11DepthStencilView>& out) {
        D3D11_DEPTH_STENCIL_VIEW_DESC d{};d.Format=format;d.ViewDimension=D3D11_DSV_DIMENSION_TEXTURE2D;
        return Check(dev->CreateDepthStencilView(tex,&d,out.ReleaseAndGetAddressOf()),"CreateDepthStencilView");
    }
    bool DepthRead(ID3D11Texture2D* tex,ComPtr<ID3D11ShaderResourceView>& out) {
        D3D11_SHADER_RESOURCE_VIEW_DESC d{};d.Format=DXGI_FORMAT_R32_FLOAT;d.ViewDimension=D3D11_SRV_DIMENSION_TEXTURE2D;d.Texture2D.MipLevels=1;
        return Check(dev->CreateShaderResourceView(tex,&d,out.ReleaseAndGetAddressOf()),"CreateDepthSRV");
    }
    bool Shaders() {
        const HMODULE compiler=LoadLibraryW(L"d3dcompiler_47.dll");
        if(!compiler) { logger::error("Build20 D3DCompiler unavailable"); return false; }
        const auto compile=reinterpret_cast<decltype(&D3DCompile)>(GetProcAddress(compiler,"D3DCompile"));
        if(!compile) {FreeLibrary(compiler);return false;}
        static constexpr char vertex[]=R"HLSL(
float4 main(uint id:SV_VertexID):SV_Position {
    float2 p=float2((id<<1)&2,id&2);
    return float4(p*float2(2,-2)+float2(-1,1),0,1);
})HLSL";
        static constexpr char prep[]=R"HLSL(
Texture2D<float4> Color:register(t0);
Texture2D<float2> Motion:register(t1);
Texture2D<float> Depth:register(t2);
struct Result {float4 color:SV_Target0; float2 motion:SV_Target1; float depth:SV_Depth;};
Result main(float4 p:SV_Position) {
    int2 xy=int2(p.xy); Result r;
    r.color=float4(max(Color.Load(int3(xy,0)).rgb,0.0),1.0);
    float2 m=Motion.Load(int3(xy,0));
    r.motion=all(isfinite(m)) && all(abs(m)<4.0) ? m : float2(0,0);
    r.depth=saturate(Depth.Load(int3(xy,0)));
    return r;
})HLSL";
        static constexpr char resolve[]=R"HLSL(
Texture2D<float4> Color:register(t0);
Texture2D<float> Depth:register(t1);
Texture2D<float4> Input:register(t2);
cbuffer Params:register(b0){uint2 inputSize;uint2 outputSize;float sharpness;float3 pad;};
float3 Read(int2 p){return Color.Load(int3(clamp(p,int2(0,0),int2(outputSize)-1),0)).rgb;}
struct Result {float4 color:SV_Target0;float depth:SV_Depth;};
Result main(float4 p:SV_Position) {
    int2 xy=int2(p.xy); float3 c=Read(xy);
    int2 z=min(int2(p.xy*float2(inputSize)/float2(outputSize)),int2(inputSize)-1);
    // Guard the first frame against invalid/empty output without presenting a black frame.
    if(!all(isfinite(c)) || all(abs(c)<0.00001)) {
        float3 fallback=Input.Load(int3(z,0)).rgb;
        if(!all(isfinite(c)) || any(fallback>0.001)) c=fallback;
    }
    if(sharpness>0.001){
        float3 a=Read(xy+int2(1,0)),b=Read(xy-int2(1,0));
        float3 d=Read(xy+int2(0,1)),e=Read(xy-int2(0,1));
        float3 lo=min(c,min(min(a,b),min(d,e))),hi=max(c,max(max(a,b),max(d,e)));
        float3 sharpened=clamp(c+sharpness*(c-(a+b+d+e)*0.25),lo,hi);
        if(all(isfinite(sharpened)))c=sharpened;
    }
    Result r;r.color=float4(max(c,0.0),1.0);
    r.depth=Depth.Load(int3(z,0));return r;
})HLSL";
        auto build=[&](const char* source,const char* target,ComPtr<ID3DBlob>& code) {
            ComPtr<ID3DBlob> err;
            auto hr=compile(source,std::strlen(source),"Build20",nullptr,nullptr,"main",target,D3DCOMPILE_OPTIMIZATION_LEVEL3,0,code.ReleaseAndGetAddressOf(),err.GetAddressOf());
            if(FAILED(hr)&&err) logger::error("Build20 HLSL {}: {}",target,std::string(static_cast<const char*>(err->GetBufferPointer()),err->GetBufferSize()));
            return Check(hr,"D3DCompile");
        };
        ComPtr<ID3DBlob> v,p,r;
        bool ok=build(vertex,"vs_5_0",v)&&build(prep,"ps_5_0",p)&&build(resolve,"ps_5_0",r);
        FreeLibrary(compiler);
        if(!ok)return false;
        return Check(dev->CreateVertexShader(v->GetBufferPointer(),v->GetBufferSize(),nullptr,vs.GetAddressOf()),"CreateVertexShader") &&
            Check(dev->CreatePixelShader(p->GetBufferPointer(),p->GetBufferSize(),nullptr,preparePS.GetAddressOf()),"Create preparePS") &&
            Check(dev->CreatePixelShader(r->GetBufferPointer(),r->GetBufferSize(),nullptr,resolvePS.GetAddressOf()),"Create resolvePS");
    }
    void View(UINT w,UINT h) {
        D3D11_VIEWPORT v{0,0,static_cast<float>(w),static_cast<float>(h),0,1};
        ctx->RSSetViewports(1,&v);
    }
    void ClearBindings() {
        ID3D11ShaderResourceView* empty[3]{};ctx->PSSetShaderResources(0,3,empty);ctx->OMSetRenderTargets(0,nullptr,nullptr);
    }
public:
    bool Init(ID3D11Device* device,ID3D11DeviceContext* context,UINT w,UINT h) {
        if(!device||!context||!w||!h)return false;
        dev=device;ctx=context;outW=w;outH=h;paramsValid=false;
        if(!Check(ctx->QueryInterface(__uuidof(ContextExt1),reinterpret_cast<void**>(ext.GetAddressOf())),"DXMT context interface"))return false;
        BOOL supported=FALSE;
        if(!Check(ext->CheckFeatureSupport(0,&supported,sizeof(supported)),"MetalFX feature support")||!supported)return false;
        if(!Shaders())return false;
        D3D11_RASTERIZER_DESC rd{};rd.FillMode=D3D11_FILL_SOLID;rd.CullMode=D3D11_CULL_NONE;rd.DepthClipEnable=TRUE;
        D3D11_DEPTH_STENCIL_DESC dd{};dd.DepthEnable=TRUE;dd.DepthWriteMask=D3D11_DEPTH_WRITE_MASK_ALL;dd.DepthFunc=D3D11_COMPARISON_ALWAYS;
        dd.StencilReadMask=dd.StencilWriteMask=0xff;
        dd.FrontFace.StencilFailOp=dd.FrontFace.StencilDepthFailOp=dd.FrontFace.StencilPassOp=D3D11_STENCIL_OP_KEEP;dd.FrontFace.StencilFunc=D3D11_COMPARISON_ALWAYS;dd.BackFace=dd.FrontFace;
        D3D11_BLEND_DESC bd{};for(auto& t:bd.RenderTarget){t.RenderTargetWriteMask=D3D11_COLOR_WRITE_ENABLE_ALL;t.SrcBlend=D3D11_BLEND_ONE;t.DestBlend=D3D11_BLEND_ZERO;t.BlendOp=D3D11_BLEND_OP_ADD;t.SrcBlendAlpha=D3D11_BLEND_ONE;t.DestBlendAlpha=D3D11_BLEND_ZERO;t.BlendOpAlpha=D3D11_BLEND_OP_ADD;}
        D3D11_BUFFER_DESC cb{};cb.ByteWidth=sizeof(Params);cb.Usage=D3D11_USAGE_DEFAULT;cb.BindFlags=D3D11_BIND_CONSTANT_BUFFER;
        return Check(dev->CreateRasterizerState(&rd,raster.GetAddressOf()),"CreateRasterizerState")&&
            Check(dev->CreateDepthStencilState(&dd,depthWrite.GetAddressOf()),"CreateDepthStencilState")&&
            Check(dev->CreateBlendState(&bd,blend.GetAddressOf()),"CreateBlendState")&&
            Check(dev->CreateBuffer(&cb,nullptr,constants.GetAddressOf()),"Create constant buffer");
    }
    bool Configure(UINT w,UINT h) {
        if(!ext||!dev||!vs||!w||!h||w>outW||h>outH)return false;
        if(b.input && b.w==w && b.h==h) { resetHistory=true;return true; }
        Buffers n;n.w=w;n.h=h;
        if(!Texture(w,h,DXGI_FORMAT_R16G16B16A16_FLOAT,D3D11_BIND_RENDER_TARGET|D3D11_BIND_SHADER_RESOURCE,n.input)||
           !Texture(w,h,DXGI_FORMAT_R16G16_FLOAT,D3D11_BIND_RENDER_TARGET|D3D11_BIND_SHADER_RESOURCE,n.motion)||
           !Texture(w,h,DXGI_FORMAT_R32_TYPELESS,D3D11_BIND_DEPTH_STENCIL|D3D11_BIND_SHADER_RESOURCE,n.depth)||
           !Texture(outW,outH,DXGI_FORMAT_R16G16B16A16_FLOAT,D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_UNORDERED_ACCESS,n.output)||
           !Texture(outW,outH,DXGI_FORMAT_R11G11B10_FLOAT,D3D11_BIND_RENDER_TARGET,n.resolved)||
           !Texture(outW,outH,DXGI_FORMAT_R24G8_TYPELESS,D3D11_BIND_DEPTH_STENCIL,n.fullDepth))return false;
        if(!Check(dev->CreateRenderTargetView(n.input.Get(),nullptr,n.inputRTV.GetAddressOf()),"input RTV")||
           !Check(dev->CreateRenderTargetView(n.motion.Get(),nullptr,n.motionRTV.GetAddressOf()),"motion RTV")||
           !Check(dev->CreateRenderTargetView(n.resolved.Get(),nullptr,n.resolvedRTV.GetAddressOf()),"resolve RTV")||
           !DepthView(n.depth.Get(),DXGI_FORMAT_D32_FLOAT,n.depthDSV)||!DepthRead(n.depth.Get(),n.depthSRV)||
           !DepthView(n.fullDepth.Get(),DXGI_FORMAT_D24_UNORM_S8_UINT,n.fullDepthDSV)||
           !Check(dev->CreateShaderResourceView(n.output.Get(),nullptr,n.outputSRV.GetAddressOf()),"output SRV"))return false;
        // Require a real UAV on the MetalFX destination; the engine's kMAIN does not need one.
        if(!Check(dev->CreateUnorderedAccessView(n.output.Get(),nullptr,n.outputUAV.GetAddressOf()),"output UAV")||
           !Check(dev->CreateShaderResourceView(n.input.Get(),nullptr,n.inputSRV.GetAddressOf()),"input SRV"))return false;
        b=std::move(n);resetHistory=true;frames=0;
        logger::info("Build20 native buffers READY: color FP16, motion RG16F, depth D32, {}x{} -> {}x{}",w,h,outW,outH);
        return true;
    }
    void Reset(){resetHistory=true;}
    bool Evaluate(const Source& s,float jx,float jy,float sharp) {
        if(!b.input||!b.output||!s.color||!s.depth||!s.motion)return false;
        // An active output-merger UAV cannot be safely displaced by this pass.
        ID3D11UnorderedAccessView* uavs[8]{};
        ctx->OMGetRenderTargetsAndUnorderedAccessViews(0,nullptr,nullptr,0,8,uavs);
        bool occupied=false;for(auto* v:uavs)if(v){occupied=true;v->Release();}
        if(occupied){logger::error("Build20 prepost has active OM UAV; aborting native pass");return false;}
        const bool direct=(b.w!=outW||b.h!=outH)&&s.directCapable;
        CpuProfile profile(b.w,b.h,sharp,direct);
        ResolveAliases aliases(ctx.Get(),s,direct);
        GraphicsState guard(ctx.Get());
        profile.Mark();
        ClearBindings();
        // DXMT implements UpdateSubresource on a DEFAULT buffer as a staging
        // blit. Upload only when values change, before (not after) MetalFX.
        const Params params{b.w,b.h,outW,outH,std::clamp(sharp,0.0f,1.0f),{0,0,0}};
        if(!paramsValid || std::memcmp(&params,&lastParams,sizeof(Params))!=0) {
            ctx->UpdateSubresource(constants.Get(),0,nullptr,&params,0,0);
            lastParams=params;paramsValid=true;
        }
        ctx->SetPredication(nullptr,FALSE);
        ctx->IASetInputLayout(nullptr);ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ctx->VSSetShader(vs.Get(),nullptr,0);ctx->GSSetShader(nullptr,nullptr,0);ctx->HSSetShader(nullptr,nullptr,0);ctx->DSSetShader(nullptr,nullptr,0);
        ctx->RSSetState(raster.Get());ctx->OMSetBlendState(blend.Get(),nullptr,0xffffffffu);ctx->OMSetDepthStencilState(depthWrite.Get(),0);
        // First pass copies only the genuinely rendered DRS rectangle; no bilinear downsampling.
        View(b.w,b.h);
        ID3D11RenderTargetView* rt[]{b.inputRTV.Get(),b.motionRTV.Get()};
        ctx->OMSetRenderTargets(2,rt,b.depthDSV.Get());
        ID3D11ShaderResourceView* src[]{s.colorSRV,s.motionSRV,s.depthSRV};
        ctx->PSSetShaderResources(0,3,src);ctx->PSSetShader(preparePS.Get(),nullptr,0);ctx->Draw(3,0);
        ClearBindings();
        profile.Mark();
        TemporalDesc d{};d.InputContentWidth=b.w;d.InputContentHeight=b.h;
        d.AutoExposure=TRUE;d.InReset=resetHistory?TRUE:FALSE;d.DepthReversed=FALSE;d.MotionVectorInDisplayRes=FALSE;
        d.Color=b.input.Get();d.Depth=b.depth.Get();d.MotionVector=b.motion.Get();d.Output=b.output.Get();
        d.MotionVectorScaleX=static_cast<float>(b.w);d.MotionVectorScaleY=static_cast<float>(b.h);d.PreExposure=1.0f;
        d.JitterOffsetX=jx;d.JitterOffsetY=jy;
        if(resetHistory){const FLOAT zero[4]{};ctx->ClearUnorderedAccessViewFloat(b.outputUAV.Get(),zero);}
        ext->TemporalUpscale(&d);
        profile.Mark();
        // DXMT's method returns void; its GPU execution cannot be verified by HRESULT.
        // Input color/depth have already been captured into independent textures.
        // Resolve directly to the engine's validated views; depth writes leave stencil intact.
        // Keep the Build20 scratch path when direct resolve is not safe.
        if(direct)aliases.Unbind();
        else ctx->CopyResource(b.fullDepth.Get(),s.depth);
        View(outW,outH);
        auto* out=direct?s.colorRTV:b.resolvedRTV.Get();
        ctx->OMSetRenderTargets(1,&out,direct?s.depthDSV:b.fullDepthDSV.Get());
        ID3D11ShaderResourceView* resolveSrc[]{b.outputSRV.Get(),b.depthSRV.Get(),b.inputSRV.Get()};
        ctx->PSSetShaderResources(0,3,resolveSrc);
        auto* cb=constants.Get();ctx->PSSetConstantBuffers(0,1,&cb);
        ctx->PSSetShader(resolvePS.Get(),nullptr,0);ctx->Draw(3,0);ClearBindings();
        if(!direct) {
            ctx->CopyResource(s.color,b.resolved.Get());ctx->CopyResource(s.depth,b.fullDepth.Get());
        }
        profile.Mark();
        if(frames++<3||resetHistory)logger::info("Build22 native MetalFX SUBMITTED: {}x{} -> {}x{}, reset={}, sharpness={}, directResolve={}",b.w,b.h,outW,outH,resetHistory,sharp,direct);
        resetHistory=false;return true;
    }
};
inline NativeMetalFX native;
} // namespace b20
