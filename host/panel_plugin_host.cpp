#include <widgets/panel_plugin_host.h>

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <nlohmann/json.hpp>
#include <signal.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <eda_base_frame.h>
#include <paths.h>
#include <pcb_draw_panel_gal.h>
#include <pcb_edit_frame.h>
#include <pgm_base.h>
#include <tool/action_menu.h>
#include <tool/tool_manager.h>
#include <view/view.h>
#include <view/view_controls.h>
#include <wx/dcbuffer.h>
#include <wx/dir.h>
#include <wx/filename.h>
#include <wx/rawbmp.h>

namespace
{
constexpr uint32_t KP_MAGIC_V = 0x4B504E4Cu;
constexpr size_t KP_FB_OFF_V = 16384;
constexpr size_t KP_SHM_BYTES_V = 4210688;
constexpr int KP_MAX_DIM_V = 1024;

std::string SanitizeKey( const std::string& aRaw )
{
    std::string out;
    out.reserve( aRaw.size() );
    for( char c : aRaw )
    {
        bool ok = ( c >= '0' && c <= '9' ) || ( c >= 'A' && c <= 'Z' ) || ( c >= 'a' && c <= 'z' ) || c == '_' || c == '-';
        out.push_back( ok ? c : '_' );
    }
    return out.empty() ? "default" : out;
}

std::vector<PLUGIN_PANEL_SPEC> DiscoverSpecs()
{
    std::vector<PLUGIN_PANEL_SPEC> specs;
    wxString rootPath = PATHS::GetUserPluginsPath();
    if( !wxDir::Exists( rootPath ) )
        return specs;
    wxDir dir( rootPath );
    wxString subName;
    bool cont = dir.GetFirst( &subName, wxEmptyString, wxDIR_DIRS );
    while( cont )
    {
        wxFileName pj( rootPath + "/" + subName, "plugin.json" );
        if( pj.FileExists() )
        {
            std::ifstream ifs( pj.GetFullPath().ToStdString() );
            nlohmann::json doc = nlohmann::json::parse( ifs, nullptr, false );
            if( doc.is_object() && doc.contains( "panels" ) && doc["panels"].is_array() )
            {
                std::string plugId = doc.value( "identifier", subName.ToStdString() );
                for( const auto& item : doc["panels"] )
                {
                    if( !item.is_object() )
                        continue;
                    PLUGIN_PANEL_SPEC sp;
                    sp.plugin_id = plugId;
                    sp.panel_id = item.value( "identifier", "" );
                    sp.title = item.value( "name", sp.panel_id );
                    sp.dock_side = item.value( "dock", "left" );
                    std::string ep = item.value( "entrypoint", "" );
                    sp.init_w = item.value( "width", 324 );
                    sp.init_h = item.value( "height", 720 );
                    if( item.contains( "args" ) && item["args"].is_array() )
                    {
                        for( const auto& a : item["args"] )
                            if( a.is_string() )
                                sp.args.push_back( a.get<std::string>() );
                    }
                    if( !sp.panel_id.empty() && !ep.empty() )
                    {
                        sp.exe_path = ( rootPath + "/" + subName + "/" + wxString::FromUTF8( ep ) ).ToStdString();
                        specs.push_back( sp );
                    }
                }
            }
        }
        cont = dir.GetNext( &subName );
    }
    return specs;
}
}

PANEL_PLUGIN_SURFACE::PANEL_PLUGIN_SURFACE( PCB_EDIT_FRAME* aFrame, const PLUGIN_PANEL_SPEC& aSpec ) :
        wxPanel( aFrame, wxID_ANY, wxDefaultPosition, wxSize( aSpec.init_w, aSpec.init_h ), wxWANTS_CHARS | wxBORDER_NONE ),
        m_frame( aFrame ),
        m_spec( aSpec )
{
    SetBackgroundStyle( wxBG_STYLE_PAINT );
    m_paneName = "PluginPanel_" + SanitizeKey( m_spec.plugin_id + "_" + m_spec.panel_id );
    m_shmPath = "/tmp/kicad_panel_" + SanitizeKey( m_spec.plugin_id + "." + m_spec.panel_id ) + ".shm";
    int fd = ::open( m_shmPath.c_str(), O_CREAT | O_RDWR, 0600 );
    if( fd >= 0 )
    {
        ::ftruncate( fd, static_cast<off_t>( KP_SHM_BYTES_V ) );
        void* m = ::mmap( nullptr, KP_SHM_BYTES_V, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0 );
        ::close( fd );
        if( m && m != MAP_FAILED )
        {
            m_shmAddr = m;
            m_shmBytes = KP_SHM_BYTES_V;
            auto* u32 = static_cast<uint32_t*>( m_shmAddr );
            auto* i32 = static_cast<int32_t*>( m_shmAddr );
            auto* f32 = static_cast<float*>( m_shmAddr );
            std::memset( m_shmAddr, 0, KP_FB_OFF_V );
            i32[4] = m_spec.init_w;
            i32[5] = m_spec.init_h;
            i32[6] = m_spec.init_w;
            i32[7] = 2;
            f32[10] = 0.1f;
            f32[13] = static_cast<float>( GetContentScaleFactor() );
            u32[1] = 1u;
            __atomic_store_n( &u32[0], KP_MAGIC_V, __ATOMIC_RELEASE );
        }
    }
    Bind( wxEVT_PAINT, &PANEL_PLUGIN_SURFACE::onPaint, this );
    Bind( wxEVT_SIZE, &PANEL_PLUGIN_SURFACE::onSize, this );
    Bind( wxEVT_MOTION, &PANEL_PLUGIN_SURFACE::onMouse, this );
    Bind( wxEVT_LEFT_DOWN, &PANEL_PLUGIN_SURFACE::onMouse, this );
    Bind( wxEVT_LEFT_UP, &PANEL_PLUGIN_SURFACE::onMouse, this );
    Bind( wxEVT_RIGHT_DOWN, &PANEL_PLUGIN_SURFACE::onMouse, this );
    Bind( wxEVT_RIGHT_UP, &PANEL_PLUGIN_SURFACE::onMouse, this );
    Bind( wxEVT_MOUSEWHEEL, &PANEL_PLUGIN_SURFACE::onMouse, this );
    Bind( wxEVT_CHAR, &PANEL_PLUGIN_SURFACE::onChar, this );
    Bind( wxEVT_KEY_DOWN, &PANEL_PLUGIN_SURFACE::onKeyDown, this );
    Bind( wxEVT_SET_FOCUS, &PANEL_PLUGIN_SURFACE::onFocus, this );
    Bind( wxEVT_KILL_FOCUS, &PANEL_PLUGIN_SURFACE::onFocus, this );
}

PANEL_PLUGIN_SURFACE::~PANEL_PLUGIN_SURFACE()
{
    StopPluginProcess();
    if( m_shmAddr && m_shmBytes > 0 )
    {
        ::munmap( m_shmAddr, m_shmBytes );
        m_shmAddr = nullptr;
    }
    if( !m_shmPath.empty() )
        ::unlink( m_shmPath.c_str() );
}

void PANEL_PLUGIN_SURFACE::EnsurePluginRunning()
{
    if( m_childPid > 0 )
    {
        int status = 0;
        pid_t r = ::waitpid( static_cast<pid_t>( m_childPid ), &status, WNOHANG );
        if( r == 0 )
            return;
        m_childPid = -1;
    }
    if( ::access( m_spec.exe_path.c_str(), X_OK ) != 0 )
        return;
    pid_t pid = ::fork();
    if( pid == 0 )
    {
        ::setenv( "KICAD_PANEL_SHM", m_shmPath.c_str(), 1 );
        std::vector<char*> argv;
        argv.push_back( const_cast<char*>( m_spec.exe_path.c_str() ) );
        for( const auto& s : m_spec.args )
            argv.push_back( const_cast<char*>( s.c_str() ) );
        argv.push_back( nullptr );
        ::execv( m_spec.exe_path.c_str(), argv.data() );
        ::_exit( 1 );
    }
    if( pid > 0 )
        m_childPid = static_cast<long>( pid );
}

void PANEL_PLUGIN_SURFACE::StopPluginProcess()
{
    if( m_shmAddr )
    {
        auto* i32 = static_cast<int32_t*>( m_shmAddr );
        i32[7] |= 16;
    }
    if( m_childPid > 0 )
    {
        ::kill( static_cast<pid_t>( m_childPid ), SIGTERM );
        ::waitpid( static_cast<pid_t>( m_childPid ), nullptr, WNOHANG );
        m_childPid = -1;
    }
}

void PANEL_PLUGIN_SURFACE::TickFromRegistry( double aWxMm, double aWyMm, double aZoomMpp, int aLayer, uint32_t aCanvasFlags )
{
    if( !m_shmAddr )
        return;
    bool shown = IsShownOnScreen();
    if( shown )
        EnsurePluginRunning();
    auto* u32 = static_cast<uint32_t*>( m_shmAddr );
    auto* i32 = static_cast<int32_t*>( m_shmAddr );
    auto* f32 = static_cast<float*>( m_shmAddr );
    f32[8] = static_cast<float>( aWxMm );
    f32[9] = static_cast<float>( aWyMm );
    f32[10] = static_cast<float>( aZoomMpp );
    i32[11] = static_cast<int32_t>( aLayer );
    u32[12] = aCanvasFlags;
    if( shown )
        i32[7] |= 3;
    else
        i32[7] = ( i32[7] | 2 ) & ~1;
    __atomic_add_fetch( &u32[2], 1u, __ATOMIC_RELEASE );
    uint32_t fseq = __atomic_load_n( &u32[3], __ATOMIC_ACQUIRE );
    if( shown && fseq != m_lastFrameSeq )
    {
        m_lastFrameSeq = fseq;
        Refresh( false );
    }
}

void PANEL_PLUGIN_SURFACE::pushEvent( int32_t aKind, int32_t aX, int32_t aY, int32_t aAux )
{
    if( !m_shmAddr )
        return;
    auto* u32 = static_cast<uint32_t*>( m_shmAddr );
    auto* i32 = static_cast<int32_t*>( m_shmAddr );
    uint32_t wIdx = __atomic_load_n( &u32[16], __ATOMIC_RELAXED );
    size_t base = 64 + static_cast<size_t>( wIdx & 255u ) * 4;
    i32[base + 0] = aKind;
    i32[base + 1] = aX;
    i32[base + 2] = aY;
    i32[base + 3] = aAux;
    __atomic_store_n( &u32[16], wIdx + 1u, __ATOMIC_RELEASE );
}

void PANEL_PLUGIN_SURFACE::onSize( wxSizeEvent& aEv )
{
    if( m_shmAddr )
    {
        wxSize sz = GetClientSize();
        auto* u32 = static_cast<uint32_t*>( m_shmAddr );
        auto* i32 = static_cast<int32_t*>( m_shmAddr );
        i32[4] = std::max( 64, std::min( KP_MAX_DIM_V, sz.x ) );
        i32[5] = std::max( 64, std::min( KP_MAX_DIM_V, sz.y ) );
        __atomic_add_fetch( &u32[2], 1u, __ATOMIC_RELEASE );
    }
    aEv.Skip();
}

void PANEL_PLUGIN_SURFACE::onMouse( wxMouseEvent& aEv )
{
    wxPoint pt = aEv.GetPosition();
    if( aEv.LeftDown() )
    {
        SetFocus();
        pushEvent( 2, pt.x, pt.y, 1 );
    }
    else if( aEv.LeftUp() )
        pushEvent( 3, pt.x, pt.y, 1 );
    else if( aEv.RightDown() )
        pushEvent( 2, pt.x, pt.y, 2 );
    else if( aEv.RightUp() )
        pushEvent( 3, pt.x, pt.y, 2 );
    else if( aEv.GetWheelRotation() != 0 )
        pushEvent( 4, pt.x, pt.y, aEv.GetWheelRotation() );
    else
        pushEvent( 1, pt.x, pt.y, 0 );
    aEv.Skip();
}

void PANEL_PLUGIN_SURFACE::onKeyDown( wxKeyEvent& aEv )
{
    int kc = aEv.GetKeyCode();
    if( kc == WXK_BACK )
        pushEvent( 5, 8, 0, 0 );
    else if( kc == WXK_TAB )
        pushEvent( 5, 9, 0, 0 );
    else if( kc == WXK_RETURN || kc == WXK_NUMPAD_ENTER )
        pushEvent( 5, 13, 0, 0 );
    else if( kc == WXK_ESCAPE )
        pushEvent( 5, 27, 0, 0 );
    else
        aEv.Skip();
}

void PANEL_PLUGIN_SURFACE::onChar( wxKeyEvent& aEv )
{
    wxChar uc = aEv.GetUnicodeKey();
    if( uc >= 32 && uc < 127 )
        pushEvent( 5, static_cast<int32_t>( uc ), 0, 0 );
    else
        aEv.Skip();
}

void PANEL_PLUGIN_SURFACE::onFocus( wxFocusEvent& aEv )
{
    pushEvent( 6, aEv.GetEventType() == wxEVT_SET_FOCUS ? 1 : 0, 0, 0 );
    aEv.Skip();
}

void PANEL_PLUGIN_SURFACE::onPaint( wxPaintEvent& )
{
    wxAutoBufferedPaintDC dc( this );
    wxSize sz = GetClientSize();
    dc.SetBrush( wxBrush( wxColour( 14, 20, 30 ) ) );
    dc.SetPen( *wxTRANSPARENT_PEN );
    dc.DrawRectangle( 0, 0, sz.x, sz.y );
    if( !m_shmAddr )
        return;
    auto* i32 = static_cast<const int32_t*>( m_shmAddr );
    int fw = std::max( 1, std::min( KP_MAX_DIM_V, static_cast<int>( i32[4] ) ) );
    int fh = std::max( 1, std::min( KP_MAX_DIM_V, static_cast<int>( i32[5] ) ) );
    int dw = std::min( sz.x, fw );
    int dh = std::min( sz.y, fh );
    if( dw <= 0 || dh <= 0 )
        return;
    wxBitmap bmp( dw, dh, 24 );
    wxNativePixelData data( bmp );
    if( !data )
        return;
    const auto* src = static_cast<const uint8_t*>( m_shmAddr ) + KP_FB_OFF_V;
    wxNativePixelData::Iterator rowIt( data );
    for( int y = 0; y < dh; ++y )
    {
        wxNativePixelData::Iterator p = rowIt;
        const uint8_t* srow = src + static_cast<size_t>( y * fw ) * 4;
        for( int x = 0; x < dw; ++x, ++p )
        {
            p.Red() = srow[x * 4 + 0];
            p.Green() = srow[x * 4 + 1];
            p.Blue() = srow[x * 4 + 2];
        }
        rowIt.OffsetY( data, 1 );
    }
    dc.DrawBitmap( bmp, 0, 0, false );
}

PANEL_PLUGIN_REGISTRY* PANEL_PLUGIN_REGISTRY::Attach( PCB_EDIT_FRAME* aFrame, wxAuiManager& aAuiMgr )
{
    return new PANEL_PLUGIN_REGISTRY( aFrame, aAuiMgr );
}

void PANEL_PLUGIN_REGISTRY::PopulateMenu( PCB_EDIT_FRAME* aFrame, PANEL_PLUGIN_REGISTRY* aReg, ACTION_MENU* aPanelsMenu )
{
    if( !aFrame || !aReg || !aPanelsMenu || aReg->m_surfaces.empty() )
        return;
    aPanelsMenu->AppendSeparator();
    for( size_t i = 0; i < aReg->m_surfaces.size(); ++i )
    {
        int mid = aReg->m_baseMenuId + static_cast<int>( i );
        aPanelsMenu->AppendCheckItem( mid, wxString::FromUTF8( aReg->m_surfaces[i]->MenuTitle() ) );
    }
}

PANEL_PLUGIN_REGISTRY::PANEL_PLUGIN_REGISTRY( PCB_EDIT_FRAME* aFrame, wxAuiManager& aAuiMgr ) :
        m_frame( aFrame ),
        m_auiMgr( &aAuiMgr ),
        m_timer( this )
{
    std::vector<PLUGIN_PANEL_SPEC> specs = DiscoverSpecs();
    if( !specs.empty() )
        m_baseMenuId = wxWindow::NewControlId( static_cast<int>( specs.size() ) );
    for( const auto& sp : specs )
    {
        auto* surf = new PANEL_PLUGIN_SURFACE( m_frame, sp );
        EDA_PANE pane;
        pane.Name( wxString::FromUTF8( surf->PaneName() ) )
                .Caption( wxString::FromUTF8( sp.title ) )
                .PaneBorder( false )
                .MinSize( m_frame->FromDIP( wxSize( 280, 240 ) ) )
                .BestSize( m_frame->FromDIP( wxSize( sp.init_w, sp.init_h ) ) )
                .FloatingSize( m_frame->FromDIP( wxSize( sp.init_w, sp.init_h ) ) )
                .DestroyOnClose( false )
                .CloseButton( true );
        if( sp.dock_side == "right" )
            pane.Right();
        else if( sp.dock_side == "bottom" )
            pane.Bottom();
        else
            pane.Left();
        pane.Hide();
        m_auiMgr->AddPane( surf, pane );
        m_surfaces.push_back( surf );
    }
    Bind( wxEVT_TIMER, &PANEL_PLUGIN_REGISTRY::onTimer, this );
    if( !m_surfaces.empty() )
    {
        int lastId = m_baseMenuId + static_cast<int>( m_surfaces.size() ) - 1;
        m_frame->Bind( wxEVT_MENU, &PANEL_PLUGIN_REGISTRY::onToggleMenu, this, m_baseMenuId, lastId );
        m_frame->Bind( wxEVT_UPDATE_UI, &PANEL_PLUGIN_REGISTRY::onUpdateMenuUi, this, m_baseMenuId, lastId );
        m_frame->ReCreateMenuBar();
        m_timer.Start( 16 );
    }
}

PANEL_PLUGIN_REGISTRY::~PANEL_PLUGIN_REGISTRY()
{
    m_timer.Stop();
    if( m_frame && !m_surfaces.empty() )
    {
        int lastId = m_baseMenuId + static_cast<int>( m_surfaces.size() ) - 1;
        m_frame->Unbind( wxEVT_MENU, &PANEL_PLUGIN_REGISTRY::onToggleMenu, this, m_baseMenuId, lastId );
        m_frame->Unbind( wxEVT_UPDATE_UI, &PANEL_PLUGIN_REGISTRY::onUpdateMenuUi, this, m_baseMenuId, lastId );
    }
}

void PANEL_PLUGIN_REGISTRY::onToggleMenu( wxCommandEvent& aEv )
{
    int idx = aEv.GetId() - m_baseMenuId;
    if( idx < 0 || idx >= static_cast<int>( m_surfaces.size() ) || !m_auiMgr )
        return;
    wxAuiPaneInfo& pane = m_auiMgr->GetPane( wxString::FromUTF8( m_surfaces[idx]->PaneName() ) );
    if( !pane.IsOk() )
        return;
    bool show = !pane.IsShown();
    pane.Show( show );
    if( show )
        m_surfaces[idx]->EnsurePluginRunning();
    m_auiMgr->Update();
}

void PANEL_PLUGIN_REGISTRY::onUpdateMenuUi( wxUpdateUIEvent& aEv )
{
    int idx = aEv.GetId() - m_baseMenuId;
    if( idx < 0 || idx >= static_cast<int>( m_surfaces.size() ) || !m_auiMgr )
        return;
    wxAuiPaneInfo& pane = m_auiMgr->GetPane( wxString::FromUTF8( m_surfaces[idx]->PaneName() ) );
    aEv.Check( pane.IsOk() && pane.IsShown() );
}

void PANEL_PLUGIN_REGISTRY::onTimer( wxTimerEvent& )
{
    if( !m_frame || m_surfaces.empty() )
        return;
    double wxMm = 0.0;
    double wyMm = 0.0;
    double zoomMpp = 0.1;
    uint32_t cFlags = 0u;
    if( PCB_DRAW_PANEL_GAL* canvas = m_frame->GetCanvas() )
    {
        if( KIGFX::VIEW_CONTROLS* vc = canvas->GetViewControls() )
        {
            VECTOR2D pos = vc->GetCursorPosition( false );
            wxMm = pos.x * 1e-6;
            wyMm = pos.y * 1e-6;
            cFlags |= 1u;
        }
        if( KIGFX::VIEW* v = canvas->GetView() )
        {
            double sc = v->GetScale();
            if( sc > 1e-9 )
                zoomMpp = 1e-6 / sc;
            if( v->IsMirroredX() )
                cFlags |= 8u;
        }
    }
    int actLayer = static_cast<int>( m_frame->GetActiveLayer() );
    for( PANEL_PLUGIN_SURFACE* surf : m_surfaces )
        surf->TickFromRegistry( wxMm, wyMm, zoomMpp, actLayer, cFlags );
}
