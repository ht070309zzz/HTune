// Exercise the compiled product shaders directly, isolating the ink from
// legitimate moving scenery/particles. A ring mask exposes unwanted dark rims.
{
    constexpr UINT side=64;
    std::vector<uint8_t> mask(side*side*4);
    for(UINT y=16;y<48;++y)for(UINT x=16;x<48;++x)
        if(!(x>=24&&x<40&&y>=24&&y<40))for(int c=0;c<4;++c)mask[(y*side+x)*4+c]=255;
    D3D11_TEXTURE2D_DESC desc{};desc.Width=desc.Height=side;desc.ArraySize=desc.MipLevels=1;
    desc.Format=DXGI_FORMAT_B8G8R8A8_UNORM;desc.SampleDesc.Count=1;desc.BindFlags=D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA data{mask.data(),side*4,0};
    ComPtr<ID3D11Texture2D> atlas,output;ComPtr<ID3D11ShaderResourceView> source;ComPtr<ID3D11RenderTargetView> targetView;
    Hr(device->CreateTexture2D(&desc,&data,&atlas));Hr(device->CreateShaderResourceView(atlas.Get(),nullptr,&source));
    desc.BindFlags=D3D11_BIND_RENDER_TARGET|D3D11_BIND_SHADER_RESOURCE;Hr(device->CreateTexture2D(&desc,nullptr,&output));Hr(device->CreateRenderTargetView(output.Get(),nullptr,&targetView));
    ComPtr<IDXGISurface> surface;Hr(output.As(&surface));ComPtr<ID2D1Bitmap1> bitmap;
    const auto props=D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_NONE,D2D1::PixelFormat(desc.Format,D2D1_ALPHA_MODE_PREMULTIPLIED));
    Hr(dc->CreateBitmapFromDxgiSurface(surface.Get(),&props,&bitmap));
    ComPtr<ID3D11VertexShader> vs;ComPtr<ID3D11PixelShader> ps;ComPtr<ID3D11InputLayout> layout;
    Hr(device->CreateVertexShader(g_RiftVS,sizeof(g_RiftVS),nullptr,&vs));Hr(device->CreatePixelShader(g_RiftPS,sizeof(g_RiftPS),nullptr,&ps));
    struct InkVertex{float x,y,u,v,r,g,b,a,kind,edge,reveal,feather,fill,start,advance;};
    const D3D11_INPUT_ELEMENT_DESC elements[]={
        {"POSITION",0,DXGI_FORMAT_R32G32_FLOAT,0,0,D3D11_INPUT_PER_VERTEX_DATA,0},
        {"TEXCOORD",0,DXGI_FORMAT_R32G32_FLOAT,0,8,D3D11_INPUT_PER_VERTEX_DATA,0},
        {"COLOR",0,DXGI_FORMAT_R32G32B32A32_FLOAT,0,16,D3D11_INPUT_PER_VERTEX_DATA,0},
        {"TEXCOORD",1,DXGI_FORMAT_R32G32B32A32_FLOAT,0,32,D3D11_INPUT_PER_VERTEX_DATA,0},
        {"TEXCOORD",2,DXGI_FORMAT_R32G32B32_FLOAT,0,48,D3D11_INPUT_PER_VERTEX_DATA,0}};
    Hr(device->CreateInputLayout(elements,5,g_RiftVS,sizeof(g_RiftVS),&layout));
    D3D11_BUFFER_DESC bd{};bd.ByteWidth=sizeof(InkVertex)*6;bd.BindFlags=D3D11_BIND_VERTEX_BUFFER;
    ComPtr<ID3D11Buffer> vertices,constants;Hr(device->CreateBuffer(&bd,nullptr,&vertices));
    bd.ByteWidth=48;bd.BindFlags=D3D11_BIND_CONSTANT_BUFFER;Hr(device->CreateBuffer(&bd,nullptr,&constants));
    D3D11_SAMPLER_DESC sd{};sd.Filter=D3D11_FILTER_MIN_MAG_MIP_LINEAR;sd.AddressU=sd.AddressV=sd.AddressW=D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.MaxLOD=D3D11_FLOAT32_MAX;ComPtr<ID3D11SamplerState> sampler;Hr(device->CreateSamplerState(&sd,&sampler));
    D3D11_RASTERIZER_DESC rd{};rd.FillMode=D3D11_FILL_SOLID;rd.CullMode=D3D11_CULL_NONE;rd.DepthClipEnable=TRUE;
    ComPtr<ID3D11RasterizerState> raster;Hr(device->CreateRasterizerState(&rd,&raster));
    auto draw=[&](int kind,float time,float fill){
        const float uv[6][2]={{0,0},{1,0},{1,1},{0,0},{1,1},{0,1}};InkVertex v[6];
        for(int i=0;i<6;++i)v[i]={uv[i][0]*side,uv[i][1]*side,uv[i][0],uv[i][1],.92f,.92f,.92f,1,float(kind),.4f,2,.01f,fill,0,1};
        const float cb[12]={float(side),float(side),1.f/side,1.f/side,time,0,1,173,.55f,.8f,.7f,1};
        ctx->UpdateSubresource(vertices.Get(),0,nullptr,v,0,0);ctx->UpdateSubresource(constants.Get(),0,nullptr,cb,0,0);
        auto* vb=vertices.Get();const UINT stride=sizeof(InkVertex),offset=0;ctx->IASetVertexBuffers(0,1,&vb,&stride,&offset);
        ctx->IASetInputLayout(layout.Get());ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ctx->VSSetShader(vs.Get(),nullptr,0);ctx->PSSetShader(ps.Get(),nullptr,0);auto* buffer=constants.Get();
        ctx->VSSetConstantBuffers(0,1,&buffer);ctx->PSSetConstantBuffers(0,1,&buffer);auto* srv=source.Get();auto* sm=sampler.Get();
        ctx->PSSetShaderResources(0,1,&srv);ctx->PSSetSamplers(0,1,&sm);
        const float clear[4]{};ctx->ClearRenderTargetView(targetView.Get(),clear);auto* rt=targetView.Get();
        ctx->OMSetRenderTargets(1,&rt,nullptr);ctx->OMSetBlendState(nullptr,nullptr,0xffffffff);ctx->OMSetDepthStencilState(nullptr,0);
        const D3D11_VIEWPORT viewport{0,0,float(side),float(side),0,1};ctx->RSSetViewports(1,&viewport);ctx->RSSetState(raster.Get());
        ctx->Draw(6,0);ctx->OMSetRenderTargets(0,nullptr,nullptr);ID3D11ShaderResourceView* none=nullptr;ctx->PSSetShaderResources(0,1,&none);
        return BitmapPixels(dc.Get(),bitmap.Get());
    };
    const auto outlined=draw(3,.1f,1);size_t darkRim=0;
    for(size_t i=0;i<mask.size();i+=4)if(!mask[i+3]&&outlined[i+3]>4&&
        std::max({outlined[i],outlined[i+1],outlined[i+2]})<outlined[i+3]*.15f)++darkRim;
    std::cout<<"Ink dark rim pixels="<<darkRim<<"\n";
    size_t movingTotal=0;
    // Glitch surface (9) intentionally has time-based geometry/chroma; its
    // unrelated motion is preserved. The five other surfaces must have no sheen.
    for(int kind:{0,3,4,5,6,7,8}){
        for(float fill:{.35f,1.f}){const auto a=draw(kind,.1f,fill),b=draw(kind,.8f,fill);const auto changed=ChangedPixels(a,b);
            movingTotal+=changed;std::cout<<"Ink time variation kind="<<kind<<" fill="<<fill<<" pixels="<<changed<<"\n";}
        const auto unsung=draw(kind,.1f,0),sung=draw(kind,.1f,1);
        expect(ChangedPixels(unsung,sung)>300,"Removing decorative sheen removed karaoke colour filling");
    }
    expect(darkRim==0,"Glyph still has an added black outline");
    expect(movingTotal==0,"Decorative scan light still moves inside the ink");
}
std::cout<<"Clean ink checks passed: "<<passed<<"; compiled native shader pixels only, no exports\n";
