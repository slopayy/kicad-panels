#ifndef PANEL_PLUGIN_HOST_H_
#define PANEL_PLUGIN_HOST_H_

#include <cstdint>
#include <string>
#include <vector>
#include <wx/aui/framemanager.h>
#include <wx/panel.h>
#include <wx/timer.h>

class ACTION_MENU;
class PCB_EDIT_FRAME;
class PANEL_PLUGIN_SURFACE;

struct PLUGIN_PANEL_SPEC
{
    std::string plugin_id;
    std::string panel_id;
    std::string title;
    std::string dock_side;
    std::string exe_path;
    std::vector<std::string> args;
    int init_w = 324;
    int init_h = 720;
};

class PANEL_PLUGIN_SURFACE : public wxPanel
{
public:
    PANEL_PLUGIN_SURFACE( PCB_EDIT_FRAME* aFrame, const PLUGIN_PANEL_SPEC& aSpec );
    ~PANEL_PLUGIN_SURFACE() override;

    void EnsurePluginRunning();
    void StopPluginProcess();
    bool ConsumeToggleRequest();
    int PollRequestedWidth();
    void TickFromRegistry( double aWxMm, double aWyMm, double aZoomMpp, int aLayer, uint32_t aCanvasFlags );
    const std::string& PaneName() const { return m_paneName; }
    const std::string& MenuTitle() const { return m_spec.title; }

private:
    void pushEvent( int32_t aKind, int32_t aX, int32_t aY, int32_t aAux );
    void onPaint( wxPaintEvent& aEv );
    void onSize( wxSizeEvent& aEv );
    void onMouse( wxMouseEvent& aEv );
    void onChar( wxKeyEvent& aEv );
    void onKeyDown( wxKeyEvent& aEv );
    void onFocus( wxFocusEvent& aEv );

    PCB_EDIT_FRAME* m_frame = nullptr;
    PLUGIN_PANEL_SPEC m_spec;
    std::string m_paneName;
    std::string m_shmPath;
    void* m_shmAddr = nullptr;
    size_t m_shmBytes = 0;
    uint32_t m_lastFrameSeq = 0;
    int m_lastReqW = 0;
    long m_childPid = -1;
};

class PANEL_PLUGIN_REGISTRY : public wxEvtHandler
{
public:
    static PANEL_PLUGIN_REGISTRY* Attach( PCB_EDIT_FRAME* aFrame, wxAuiManager& aAuiMgr );
    static void PopulateMenu( PCB_EDIT_FRAME* aFrame, PANEL_PLUGIN_REGISTRY* aReg, ACTION_MENU* aPanelsMenu );

    PANEL_PLUGIN_REGISTRY( PCB_EDIT_FRAME* aFrame, wxAuiManager& aAuiMgr );
    ~PANEL_PLUGIN_REGISTRY() override;

private:
    void onTimer( wxTimerEvent& aEv );
    void onToggleMenu( wxCommandEvent& aEv );
    void onUpdateMenuUi( wxUpdateUIEvent& aEv );

    PCB_EDIT_FRAME* m_frame = nullptr;
    wxAuiManager* m_auiMgr = nullptr;
    wxTimer m_timer;
    int m_baseMenuId = 0;
    std::vector<PANEL_PLUGIN_SURFACE*> m_surfaces;
};

#endif
