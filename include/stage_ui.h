#pragma once
#include <windows.h>
#include <dwrite_2.h>
#include <wrl/client.h>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

// Stage UI toolkit: an immediate-mode widget layer drawn on the stage canvas.
// This module owns input, focus, animation and text layout; it draws nothing
// itself. Each frame produces a DrawList that the renderer paints with
// Direct2D (stage_ui_draw.h). All coordinates are client DIPs.
namespace stage::ui {
using Microsoft::WRL::ComPtr;

struct Color { float r=0,g=0,b=0,a=1; };
// Scales alpha by factor (which may exceed 1); the result stays in 0..1.
Color WithAlpha(Color color,float factor);
Color Mix(Color a,Color b,float t);

struct Point { float x=0,y=0; };
struct Rect {
    float left=0,top=0,right=0,bottom=0;
    static Rect Xywh(float x,float y,float w,float h) {return {x,y,x+w,y+h};}
    float Width() const {return right-left;}
    float Height() const {return bottom-top;}
    Point Center() const {return {(left+right)*.5f,(top+bottom)*.5f};}
    bool Empty() const {return !(right>left&&bottom>top);}
    bool Contains(Point p) const {return p.x>=left&&p.x<right&&p.y>=top&&p.y<bottom;}
    Rect Inset(float dx,float dy) const {return {left+dx,top+dy,right-dx,bottom-dy};}
    Rect Offset(float dx,float dy) const {return {left+dx,top+dy,right+dx,bottom+dy};}
    Rect Intersect(const Rect& other) const;
};

enum class Font : uint8_t { Body,Strong,Small,Caption,Overline,Title,Value,Icon,IconSmall,StageNav,Count };
enum class Align : uint8_t { Leading,Center,Trailing };

// One painted primitive. Text items carry the exact layout that was measured.
enum class Op : uint8_t { Glass,Fill,Stroke,Text,Line,Light,Cover,Clip,Unclip };
struct Item {
    Op op=Op::Fill;
    Rect rect{};
    float radius=0,width=1,opacity=1;
    Color color{},second{},third{}; // Glass: tint, rim, travelling light; others: color
    float sheen=-1;                 // Glass: travelling rim light position 0..1, negative for none
    bool frosted=true;              // Glass: samples the blurred stage behind it
    Point from{},to{};      // Line
    ComPtr<IDWriteTextLayout> layout;
};
class DrawList {
public:
    void Clear();
    void Glass(Rect rect,float radius,Color tint,Color rim,float opacity=1,float sheen=-1,Color sheenColor={1,1,1,.6f},bool frosted=true);
    void Fill(Rect rect,float radius,Color color);
    void Stroke(Rect rect,float radius,Color color,float width=1);
    void Text(const ComPtr<IDWriteTextLayout>& layout,Point origin,Color color);
    void Line(Point from,Point to,Color color,float width=1);
    void Light(Point center,float radius,Color color,float opacity=1);
    void Cover(Rect rect,float radius,float opacity=1);
    void PushClip(Rect rect);
    void PopClip();
    // Multiplies the alpha of everything added afterwards (fading groups).
    void SetOpacity(float opacity);
    float Opacity() const {return opacity_;}
    const std::vector<Item>& Items() const {return items_;}
    bool UsesGlass() const {return glass_;}
    bool Empty() const {return items_.empty();}
private:
    std::vector<Item> items_;
    int clips_=0;
    bool glass_=false;
    float opacity_=1.f;
};

// Visual tokens of one surface: the console over the stage follows the stage
// palette; the Backstage room is always dark.
struct Theme {
    Color ink{.96f,.95f,.93f,1},muted{.74f,.72f,.70f,1},faint{.55f,.54f,.52f,1};
    Color accent{.86f,.70f,.45f,1},onAccent{.08f,.07f,.06f,1};
    Color glassTint{.07f,.07f,.08f,.55f},glassRim{1,1,1,.13f};
    Color field{1,1,1,.06f},focus{.95f,.80f,.55f,1},danger{.95f,.45f,.38f,1};
    bool light=false;
};
struct FrameInfo {
    float width=0,height=0; // client DIPs
    int64_t nowMs=0;        // monotonic
    bool reducedMotion=false;
    Theme theme;
    HWND window=nullptr;    // clipboard owner; may be null (previews)
};

struct Interaction {
    bool hovered=false,pressed=false,held=false,clicked=false,doubleClicked=false,focused=false,focusVisible=false;
    float hover=0,press=0,focus=0; // animated 0..1
    Point pointer{};
};
enum Flags : uint32_t { FlagNone=0,FlagDisabled=1,FlagNoFocus=2,FlagNoPointer=4 };
enum class ButtonKind : uint8_t { Secondary,Primary,Quiet };
struct TextEvent { bool changed=false,submitted=false,committed=false; };
struct ListRow { std::wstring title,subtitle; bool flagged=false; };
struct ListEvent { bool selected=false,activated=false; };

class Context {
public:
    Context();~Context();
    Context(const Context&)=delete;
    Context& operator=(const Context&)=delete;

    // Input from the window procedure (client DIPs). Events queue until the
    // next frame; at most one press and one release are consumed per frame.
    void MouseMove(float x,float y);
    void MouseDown(float x,float y,bool doubleClick);
    void MouseUp(float x,float y);
    void MouseLeave();
    void Wheel(float x,float y,float notches);
    // True when the focused widget uses this key; the window then skips its
    // own shortcut. Only a focused text field takes plain characters.
    bool KeyDown(UINT vk,bool ctrl,bool shift);
    void Char(wchar_t ch);
    // Moves keyboard focus into the UI (first focusable widget) or out of it.
    void FocusFirst();
    void ClearFocus();
    // Pointer routing before the next frame, from the last built frame.
    bool HitTest(float x,float y) const;
    bool PointerCaptured() const;
    bool WantsKeyboard() const;
    bool EditingText() const;
    bool CaretRect(Rect& caret) const;
    bool Animating() const;

    void Begin(const FrameInfo& info);
    void End();
    const DrawList& Output() const;
    DrawList& Draw();
    const FrameInfo& Frame() const;
    // Switches the visual tokens for widgets declared afterwards.
    void SetTheme(const Theme& theme);
    float DeltaSeconds() const;

    // Text measured and drawn with the same layouts.
    ComPtr<IDWriteTextLayout> Layout(std::wstring_view text,Font font,float maxWidth,Align align=Align::Leading,bool wrap=false);
    float TextWidth(std::wstring_view text,Font font);
    float TextHeight(std::wstring_view text,Font font,float width);
    // Single line (ellipsis when it does not fit) or wrapped within rect.
    void Label(Rect rect,std::wstring_view text,Font font,Color color,Align align=Align::Leading,bool wrap=false);
    void Icon(Point center,wchar_t glyph,Font font,Color color);

    // Group opacity for everything drawn until the matching pop.
    void PushOpacity(float opacity);
    void PopOpacity();
    // Pointer-opaque area without interaction (panels, backgrounds).
    void Block(Rect rect);
    Interaction Interact(uint32_t id,Rect rect,uint32_t flags=FlagNone);
    bool Focused(uint32_t id) const;
    // Enter or Space on the focused widget id (consumed): keyboard activation
    // for custom widgets built on Interact.
    bool Activate(uint32_t id);
    // Per-widget animated values: exponential approach or a damped spring.
    float Animate(uint32_t id,int slot,float target,float rate);
    float Spring(uint32_t id,int slot,float target,float omega=14.f,float damping=.8f);

    bool Button(uint32_t id,Rect rect,std::wstring_view text,ButtonKind kind=ButtonKind::Secondary,uint32_t flags=FlagNone);
    bool IconButton(uint32_t id,Rect rect,wchar_t glyph,std::wstring_view tooltip,bool active=false,uint32_t flags=FlagNone);
    bool Switch(uint32_t id,Rect rect,std::wstring_view label,bool& value,uint32_t flags=FlagNone);
    bool Slider(uint32_t id,Rect rect,float& value,float minimum,float maximum,float step,uint32_t flags=FlagNone);
    bool Segmented(uint32_t id,Rect rect,const std::vector<std::wstring>& options,int& index,uint32_t flags=FlagNone);
    TextEvent TextBox(uint32_t id,Rect rect,std::wstring& text,std::wstring_view placeholder,size_t maxLength,uint32_t flags=FlagNone);
    ListEvent List(uint32_t id,Rect rect,const std::vector<ListRow>& rows,int& selected,uint32_t flags=FlagNone);
    // Scrollable read-only text; Ctrl+C copies all of it while focused.
    void TextBlock(uint32_t id,Rect rect,std::wstring_view text,Font font,Color color);
    // Attaches a delayed hint to the widget declared just before.
    void Tooltip(std::wstring_view text);
    void FocusRing(Rect rect,float radius,const Interaction& state);

    // Vertical scroll region; returns the y offset to add to content.
    float BeginScroll(uint32_t id,Rect viewport);
    void EndScroll(float contentHeight);

    static uint32_t Id(std::string_view name,uint32_t index=0);
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
