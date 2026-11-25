#ifndef PANEL_PLUGIN_HOST_H_
#define PANEL_PLUGIN_HOST_H_

#include <cstdint>
#include <string>
#include <vector>
#include <wx/aui/framemanager.h>
#include <wx/panel.h>
#include <wx/scrolwin.h>
#include <wx/timer.h>

class ACTION_MENU;
class BOARD;
class PCB_EDIT_FRAME;
class PANEL_PLUGIN_DIALOG;
class PANEL_PLUGIN_OVERLAY_ITEM;
class PANEL_CANVAS_WIDGET;
class PANEL_PILL_BADGE;
class wxButton;
class wxCheckBox;
class wxChoice;
class wxStaticText;
class wxTextCtrl;

namespace KIGFX
{
class VIEW;
}

struct PLUGIN_PANEL_SPEC
{
    std::string plugin_id;
    std::string panel_id;
    std::string title;
    std::string dock_side;
    std::string plugin_dir;
    std::string exe_path;
    std::vector<std::string> args;
    int init_w = 324;
    int init_h = 760;
};

struct KP_BUILT_WIDGET
{
    int slot_idx = 0;
    int id = 0;
    int kind = 0;
    int row_group = 0;
    int min_h = 0;
    bool has_label = false;
    bool has_action = false;
    wxStaticText* left_lbl = nullptr;
    wxStaticText* right_lbl = nullptr;
    PANEL_PILL_BADGE* badge = nullptr;
    wxButton* btn = nullptr;
    wxChoice* choice = nullptr;
    wxCheckBox* check = nullptr;
    wxTextCtrl* text = nullptr;
    PANEL_CANVAS_WIDGET* canvas = nullptr;
    std::string cached_label;
    std::string cached_value;
    int cached_sel = -99999;
    uint32_t cached_color = 0xFFFFFFFFu;
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
    void TickFromRegistry( double aWxMm, double aWyMm, double aZoomMpp, int aLayer, uint32_t aCanvasFlags, const std::string& aBoardPath );
    const std::string& PaneName() const { return m_paneName; }
    const std::string& MenuTitle() const { return m_spec.title; }

    const uint8_t* CanvasBuffer( int aCanvasIdx, int& aOutW, int& aOutH, uint32_t& aOutSeq ) const;
    void PushCanvasMouse( int aCanvasIdx, int aSubEv, int aLx, int aLy, int aWheel );
    void NotifyDialogClosedByUser();
    void PushWidgetClickById( int aWidgetId );
    void HandleWidgetButton( wxCommandEvent& aEv );
    void HandleWidgetChoice( wxCommandEvent& aEv );
    void HandleWidgetCheck( wxCommandEvent& aEv );
    void HandleWidgetTextEnter( wxCommandEvent& aEv );
    void HandleWidgetTextBlur( wxFocusEvent& aEv );

private:
    void pushEvent( int32_t aKind, int32_t aX, int32_t aY, int32_t aAux );
    int slotWidgetId( int aSlotIdx ) const;
    void commitSlotText( int aSlotIdx );
    void reconcileContainer( wxWindow* aParent, std::vector<KP_BUILT_WIDGET>& aBuilt, int aSlotOffset, int aCount );
    void syncDialogState();
    void syncOverlayState( bool aPanelShown );
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
    int m_ctrlBaseId = 0;
    uint32_t m_lastFrameSeq = 0;
    uint32_t m_lastWidgetGen = 0;
    uint32_t m_lastDialogGen = 0;
    uint32_t m_lastCanvasSeq[3] = { 0, 0, 0 };
    uint32_t m_lastOverlaySeq = 0;
    bool m_wasShown = false;
    int m_lastReqW = 0;
    long m_childPid = -1;

    wxScrolledWindow* m_scroller = nullptr;
    PANEL_PLUGIN_DIALOG* m_dialog = nullptr;
    PANEL_PLUGIN_OVERLAY_ITEM* m_overlayItem = nullptr;
    BOARD* m_preparedBoard = nullptr;
    std::vector<KP_BUILT_WIDGET> m_dockWidgets;
    std::vector<KP_BUILT_WIDGET> m_dlgWidgets;
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
