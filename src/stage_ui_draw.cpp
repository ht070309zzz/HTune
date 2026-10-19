#include "stage_ui_draw.h"
#include <d2d1_1helper.h>
#include <wrl/client.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <stdexcept>
#include <string>

namespace stage::ui {
namespace {
using Microsoft::WRL::ComPtr;
void PaintCheck(HRESULT hr) {
    if(FAILED(hr))throw std::runtime_error("Stage UI paint resource failure: "+std::to_string((unsigned long)hr));
}
D2D1_COLOR_F ToD2D(Color c,float opacity=1.f) {
    return D2D1::ColorF(c.r,c.g,c.b,std::clamp(c.a*opacity,0.f,1.f));
}
D2D1_RECT_F ToD2D(const Rect& r) {return D2D1::RectF(r.left,r.top,r.right,r.bottom);}
bool SameRgb(Color a,Color b) {return std::abs(a.r-b.r)<.004f&&std::abs(a.g-b.g)<.004f&&std::abs(a.b-b.b)<.004f;}
}

struct Painter::Impl {
    ID2D1DeviceContext* owner=nullptr;
    ComPtr<ID2D1SolidColorBrush> solid;
    ComPtr<ID2D1LinearGradientBrush> highlight;
    ComPtr<ID2D1BitmapBrush1> blurBrush,coverBrush;
    ID2D1Bitmap1* blurSource=nullptr;ID2D1Bitmap1* coverSource=nullptr;
    struct Tinted {Color color{};ComPtr<ID2D1RadialGradientBrush> light;ComPtr<ID2D1LinearGradientBrush> sheen;};
    std::array<Tinted,10> tinted{};size_t nextTinted=0;

    void Ensure(ID2D1DeviceContext* target) {
        if(owner==target&&solid)return;
        *this=Impl{};owner=target;
        PaintCheck(target->CreateSolidColorBrush(D2D1::ColorF(1,1,1,1),&solid));
        const D2D1_GRADIENT_STOP stops[]={{0.f,D2D1::ColorF(1,1,1,.085f)},{1.f,D2D1::ColorF(1,1,1,0.f)}};
        ComPtr<ID2D1GradientStopCollection> collection;
        PaintCheck(target->CreateGradientStopCollection(stops,2,&collection));
        PaintCheck(target->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties({0,0},{0,1}),collection.Get(),&highlight));
    }
    Tinted& Tint(ID2D1DeviceContext* target,Color color) {
        for(auto& entry:tinted)if(entry.light&&SameRgb(entry.color,color))return entry;
        auto& entry=tinted[nextTinted++%tinted.size()];
        entry=Tinted{};entry.color=color;
        const D2D1_GRADIENT_STOP light[]={{0.f,D2D1::ColorF(color.r,color.g,color.b,1.f)},{.42f,D2D1::ColorF(color.r,color.g,color.b,.36f)},
            {1.f,D2D1::ColorF(color.r,color.g,color.b,0.f)}};
        ComPtr<ID2D1GradientStopCollection> collection;
        PaintCheck(target->CreateGradientStopCollection(light,3,&collection));
        PaintCheck(target->CreateRadialGradientBrush(D2D1::RadialGradientBrushProperties({0,0},{0,0},1,1),collection.Get(),&entry.light));
        const D2D1_GRADIENT_STOP band[]={{0.f,D2D1::ColorF(color.r,color.g,color.b,0.f)},{.5f,D2D1::ColorF(color.r,color.g,color.b,1.f)},
            {1.f,D2D1::ColorF(color.r,color.g,color.b,0.f)}};
        ComPtr<ID2D1GradientStopCollection> bandStops;
        PaintCheck(target->CreateGradientStopCollection(band,3,&bandStops));
        PaintCheck(target->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties({0,0},{1,0}),bandStops.Get(),&entry.sheen));
        return entry;
    }
    ID2D1BitmapBrush1* BitmapBrush(ID2D1DeviceContext* target,ID2D1Bitmap1* bitmap,ComPtr<ID2D1BitmapBrush1>& brush,ID2D1Bitmap1*& source) {
        // A brush holds its bitmap: never keep one for a source that is gone.
        if(!bitmap){brush.Reset();source=nullptr;return nullptr;}
        if(!brush||source!=bitmap){
            brush.Reset();source=nullptr;
            PaintCheck(target->CreateBitmapBrush(bitmap,D2D1::BitmapBrushProperties1(D2D1_EXTEND_MODE_CLAMP,D2D1_EXTEND_MODE_CLAMP,
                D2D1_INTERPOLATION_MODE_LINEAR),D2D1::BrushProperties(),&brush));
            source=bitmap;
        }
        return brush.Get();
    }
    void Glass(ID2D1DeviceContext* target,const Item& item,const PaintResources& r) {
        const auto rect=ToD2D(item.rect);
        const auto rounded=D2D1::RoundedRect(rect,item.radius,item.radius);
        const float opacity=item.opacity;
        bool frosted=false;
        if(auto* blur=item.frosted?BitmapBrush(target,r.blur,blurBrush,blurSource):nullptr;blur&&r.clientWidth>0.f&&r.clientHeight>0.f){
            // The blurred stage is aligned with what lies behind the panel.
            const auto size=r.blur->GetSize();
            if(size.width>0.f&&size.height>0.f){
                blur->SetTransform(D2D1::Matrix3x2F::Scale(r.clientWidth/size.width,r.clientHeight/size.height));
                blur->SetOpacity(opacity);
                target->FillRoundedRectangle(rounded,blur);
                frosted=true;
            }
        }
        // Without the blurred stage the tint carries the panel alone; an
        // unfrosted panel keeps its tint exactly (it sits over the dark room).
        auto tint=item.color;if(item.frosted&&!frosted)tint.a=std::min(1.f,tint.a*1.5f+.18f);
        solid->SetColor(ToD2D(tint,opacity));target->FillRoundedRectangle(rounded,solid.Get());
        highlight->SetStartPoint({0,rect.top});highlight->SetEndPoint({0,rect.top+(rect.bottom-rect.top)*.5f});
        highlight->SetOpacity(opacity);target->FillRoundedRectangle(rounded,highlight.Get());
        const auto rim=D2D1::RoundedRect(D2D1::RectF(rect.left+.5f,rect.top+.5f,rect.right-.5f,rect.bottom-.5f),
            std::max(0.f,item.radius-.5f),std::max(0.f,item.radius-.5f));
        solid->SetColor(ToD2D(item.second,opacity));target->DrawRoundedRectangle(rim,solid.Get(),1.f);
        if(item.sheen>=0.f&&item.third.a>0.f){
            // A soft band of light travelling diagonally across the rim.
            auto& band=Tint(target,item.third);
            const float w=rect.right-rect.left,h=rect.bottom-rect.top,span=std::max(80.f,(w+h)*.18f);
            const float travel=std::fmod(item.sheen,1.f)*(w+span*2.f)-span;
            const D2D1_POINT_2F center{rect.left+travel,rect.top+h*.5f};
            band.sheen->SetStartPoint({center.x-span*.5f,center.y-span*.18f});
            band.sheen->SetEndPoint({center.x+span*.5f,center.y+span*.18f});
            band.sheen->SetOpacity(std::clamp(item.third.a*opacity,0.f,1.f));
            target->DrawRoundedRectangle(rim,band.sheen.Get(),1.4f);
        }
    }
};

Painter::Painter():impl_(std::make_unique<Impl>()){}
Painter::~Painter()=default;
void Painter::ReleaseBitmaps() {
    impl_->blurBrush.Reset();impl_->blurSource=nullptr;
    impl_->coverBrush.Reset();impl_->coverSource=nullptr;
}
void Painter::Paint(const DrawList& list,const PaintResources& r) {
    if(!r.target||list.Empty())return;
    auto& s=*impl_;s.Ensure(r.target);
    auto* target=r.target;
    D2D1_MATRIX_3X2_F saved{};target->GetTransform(&saved);target->SetTransform(D2D1::Matrix3x2F::Identity());
    int clips=0;
    for(const auto& item:list.Items()){
        switch(item.op){
            case Op::Glass:s.Glass(target,item,r);break;
            case Op::Fill:
                s.solid->SetColor(ToD2D(item.color));
                if(item.radius>0.f)target->FillRoundedRectangle(D2D1::RoundedRect(ToD2D(item.rect),item.radius,item.radius),s.solid.Get());
                else target->FillRectangle(ToD2D(item.rect),s.solid.Get());
                break;
            case Op::Stroke:
                s.solid->SetColor(ToD2D(item.color));
                target->DrawRoundedRectangle(D2D1::RoundedRect(ToD2D(item.rect),item.radius,item.radius),s.solid.Get(),item.width);
                break;
            case Op::Text:
                s.solid->SetColor(ToD2D(item.color));
                target->DrawTextLayout({item.rect.left,item.rect.top},item.layout.Get(),s.solid.Get(),D2D1_DRAW_TEXT_OPTIONS_ENABLE_COLOR_FONT);
                break;
            case Op::Line:
                s.solid->SetColor(ToD2D(item.color));
                target->DrawLine({item.from.x,item.from.y},{item.to.x,item.to.y},s.solid.Get(),item.width);
                break;
            case Op::Light:{
                auto& tinted=s.Tint(target,item.color);
                const auto center=item.rect.Center();
                tinted.light->SetCenter({center.x,center.y});tinted.light->SetRadiusX(item.radius);tinted.light->SetRadiusY(item.radius);
                tinted.light->SetOpacity(std::clamp(item.opacity*item.color.a,0.f,1.f));
                target->FillEllipse(D2D1::Ellipse({center.x,center.y},item.radius,item.radius),tinted.light.Get());
                break;
            }
            case Op::Cover:{
                const auto rounded=D2D1::RoundedRect(ToD2D(item.rect),item.radius,item.radius);
                if(auto* brush=s.BitmapBrush(target,r.cover,s.coverBrush,s.coverSource)){
                    const auto size=r.cover->GetSize();
                    if(size.width>0.f&&size.height>0.f){
                        brush->SetTransform(D2D1::Matrix3x2F::Scale(item.rect.Width()/size.width,item.rect.Height()/size.height)*
                            D2D1::Matrix3x2F::Translation(item.rect.left,item.rect.top));
                        brush->SetOpacity(item.opacity);
                        target->FillRoundedRectangle(rounded,brush);
                        break;
                    }
                }
                // No cover yet: a quiet placeholder tile.
                s.solid->SetColor(D2D1::ColorF(1,1,1,.08f*item.opacity));target->FillRoundedRectangle(rounded,s.solid.Get());
                break;
            }
            case Op::Clip:target->PushAxisAlignedClip(ToD2D(item.rect),D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);++clips;break;
            case Op::Unclip:if(clips>0){target->PopAxisAlignedClip();--clips;}break;
        }
    }
    while(clips-->0)target->PopAxisAlignedClip();
    target->SetTransform(saved);
}
}
