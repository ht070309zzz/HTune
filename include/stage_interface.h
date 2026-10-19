#pragma once
#include "settings_view.h"
#include "stage_ui.h"
#include <string>
#include <vector>

namespace stage {
// Presentation-only state of the stage UI, owned by the app; never persisted.
struct InterfaceState {
    bool backstage=false;
    int page=0;              // 0 picture, 1 theme, 2 lyrics, 3 status
    std::wstring query;      // manual lyric lookup text
    std::wstring offsetText; // offset field while it is being edited
    bool offsetEditing=false;
};
// Facts about the frame the interface is drawn over.
struct InterfaceFrame {
    int style=2;
    float width=0,height=0;  // client DIPs
    int64_t nowMs=0;
    bool reducedMotion=false;
    bool lightStage=false;   // the stage palette is light (paper)
    ui::Color ink{},muted{},accent{};
    bool console=false;      // pointer/keyboard activity asks for the console
    bool desktop=false;      // desktop host: no interface at all
    bool windowChrome=false,maximized=false,windowActive=true;
    float pointerX=-1,pointerY=-1;
    bool pointerActive=false;
};
// Builds the console, stage pills and Backstage pages for one frame. Every
// effect is returned as a UiAction; nothing here touches app state.
class StageInterface {
public:
    void Build(ui::Context& ui,const SettingsView& view,InterfaceState& state,const InterfaceFrame& frame,std::vector<UiAction>& actions);
    // True while Backstage or the console is on screen (input routing).
    bool Visible() const {return originalVisible_||consolePresence_>.01f||backstagePresence_>.01f;}
    bool BackstageVisible() const {return backstagePresence_>.01f;}
    bool ConsoleShown() const {return consoleTarget_;}
    float BackstageAmount() const {return backstagePresence_;}
    static ui::Theme ConsoleTheme(const InterfaceFrame& frame);
    static ui::Theme BackstageTheme(const InterfaceFrame& frame);
private:
    void WindowChrome(ui::Context& ui,const InterfaceFrame& frame,std::vector<UiAction>& actions);
    void OriginalControls(ui::Context& ui,const SettingsView& view,InterfaceState& state,const InterfaceFrame& frame,std::vector<UiAction>& actions);
    void Console(ui::Context& ui,const SettingsView& view,InterfaceState& state,const InterfaceFrame& frame,std::vector<UiAction>& actions);
    void Pills(ui::Context& ui,const SettingsView& view,const InterfaceFrame& frame,std::vector<UiAction>& actions);
    void Toast(ui::Context& ui,const SettingsView& view,const InterfaceFrame& frame);
    void Backstage(ui::Context& ui,const SettingsView& view,InterfaceState& state,const InterfaceFrame& frame,std::vector<UiAction>& actions);
    float PicturePage(ui::Context& ui,const SettingsView& view,ui::Rect column,std::vector<UiAction>& actions);
    float ThemePage(ui::Context& ui,const SettingsView& view,ui::Rect column,std::vector<UiAction>& actions);
    float LyricsPage(ui::Context& ui,const SettingsView& view,InterfaceState& state,ui::Rect column,std::vector<UiAction>& actions);
    float StatusPage(ui::Context& ui,const SettingsView& view,ui::Rect column,std::vector<UiAction>& actions);
    float consolePresence_=0,backstagePresence_=0;
    int64_t pageChangedAt_=0;int shownPage_=-1;
    std::wstring toastText_;
    bool consoleTarget_=false;
    bool originalVisible_=false;
};
}
