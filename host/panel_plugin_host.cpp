#include <widgets/panel_plugin_host.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <fstream>
#include <memory>
#include <nlohmann/json.hpp>
#include <signal.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <api/api_server.h>
#include <base_units.h>
#include <board.h>
#include <eda_base_frame.h>
#include <gal/graphics_abstraction_layer.h>
#include <layer_ids.h>
#include <lset.h>
#include <paths.h>
#include <pcb_draw_panel_gal.h>
#include <pcb_edit_frame.h>
#include <pgm_base.h>
#include <reference_image.h>
#include <tool/action_menu.h>
#include <view/view.h>
#include <view/view_controls.h>
#include <view/view_item.h>

#include <wx/button.h>
#include <wx/checkbox.h>
#include <wx/choice.h>
#include <wx/dcbuffer.h>
#include <wx/dialog.h>
#include <wx/image.h>
#include <wx/rawbmp.h>
#include <wx/scrolwin.h>
#include <wx/settings.h>
#include <wx/sizer.h>
#include <wx/statline.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>

namespace
{
constexpr uint32_t KP_MAGIC_V = 0x4B504E4Cu;
constexpr uint32_t KP_VERSION_V = 2u;
constexpr size_t KP_RING_I32_BASE_V = 512;
constexpr size_t KP_TEXT_COMMIT_OFF_V = 6144;
constexpr size_t KP_WIDGET_OFF_V = 16384;
constexpr size_t KP_WIDGET_STRIDE_V = 2048;
constexpr int KP_DOCK_SLOTS_V = 48;
constexpr int KP_DIALOG_SLOTS_V = 48;
constexpr size_t KP_CANVAS0_OFF_V = 262144;
constexpr size_t KP_CANVAS1_OFF_V = 1490944;
constexpr size_t KP_CANVAS2_OFF_V = 2514944;
constexpr size_t KP_OVERLAY_OFF_V = 4153344;
constexpr size_t KP_FB_OFF_V = 4153344;
constexpr size_t KP_SHM_BYTES_V = 8347648;
constexpr int KP_MAX_DIM_V = 1024;

constexpr int KP_EV_WIDGET_CLICK_V = 10;
constexpr int KP_EV_WIDGET_SELECT_V = 11;
constexpr int KP_EV_WIDGET_CHECK_V = 12;
constexpr int KP_EV_WIDGET_TEXT_V = 13;
constexpr int KP_EV_CANVAS_MOUSE_V = 14;
constexpr int KP_EV_DIALOG_CLOSED_V = 15;

constexpr int KP_W_HEADER_V = 1;
constexpr int KP_W_BUTTON_V = 2;
constexpr int KP_W_CHOICE_V = 3;
constexpr int KP_W_CHECKBOX_V = 4;
constexpr int KP_W_TEXTBOX_V = 5;
constexpr int KP_W_KV_ROW_V = 6;
constexpr int KP_W_SECTION_V = 7;
constexpr int KP_W_CANVAS_V = 8;

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

std::string ReadShmStr( const uint8_t* aPtr, size_t aMaxLen )
{
    size_t n = 0;
    while( n < aMaxLen && aPtr[n] != 0 )
        ++n;
    return std::string( reinterpret_cast<const char*>( aPtr ), n );
}

void WriteShmStr( uint8_t* aPtr, size_t aCap, const std::string& aStr )
{
    if( aCap == 0 )
        return;
    size_t n = std::min( aStr.size(), aCap - 1 );
    if( n > 0 )
        std::memcpy( aPtr, aStr.data(), n );
    aPtr[n] = 0;
}

wxColour UnpackRgba( uint32_t aCol, const wxColour& aFallback )
{
    uint8_t a = static_cast<uint8_t>( ( aCol >> 24 ) & 0xFFu );
    if( a == 0 )
        return aFallback;
    return wxColour(
            static_cast<unsigned char>( aCol & 0xFFu ),
            static_cast<unsigned char>( ( aCol >> 8 ) & 0xFFu ),
            static_cast<unsigned char>( ( aCol >> 16 ) & 0xFFu ) );
}

wxArrayString SplitLinesWx( const std::string& aJoined )
{
    wxArrayString arr;
    size_t pos = 0;
    while( pos <= aJoined.size() )
    {
        size_t nxt = aJoined.find( '\n', pos );
        if( nxt == std::string::npos )
        {
            arr.Add( wxString::FromUTF8( aJoined.substr( pos ) ) );
            break;
        }
        arr.Add( wxString::FromUTF8( aJoined.substr( pos, nxt - pos ) ) );
        pos = nxt + 1;
    }
    return arr;
}

void ScanPluginsDir( const std::string& aRoot, std::vector<PLUGIN_PANEL_SPEC>& aOut )
{
    if( aRoot.empty() )
        return;
    DIR* dp = ::opendir( aRoot.c_str() );
    if( !dp )
        return;
    while( struct dirent* ent = ::readdir( dp ) )
    {
        std::string subName( ent->d_name );
        if( subName.empty() || subName == "." || subName == ".." )
            continue;
        std::string subDir = aRoot + "/" + subName;
        struct stat stDir{};
        if( ::stat( subDir.c_str(), &stDir ) != 0 || !S_ISDIR( stDir.st_mode ) )
            continue;
        std::string pjPath = subDir + "/plugin.json";
        struct stat stPj{};
        if( ::stat( pjPath.c_str(), &stPj ) != 0 || !S_ISREG( stPj.st_mode ) )
            continue;
        std::ifstream ifs( pjPath );
        nlohmann::json doc = nlohmann::json::parse( ifs, nullptr, false );
        if( !doc.is_object() || !doc.contains( "panels" ) || !doc["panels"].is_array() )
            continue;
        std::string plugId = doc.value( "identifier", subName );
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
            sp.init_h = item.value( "height", 760 );
            if( item.contains( "args" ) && item["args"].is_array() )
            {
                for( const auto& a : item["args"] )
                    if( a.is_string() )
                        sp.args.push_back( a.get<std::string>() );
            }
            if( sp.panel_id.empty() || ep.empty() )
                continue;
            bool dup = false;
            for( const auto& ex : aOut )
            {
                if( ex.plugin_id == sp.plugin_id && ex.panel_id == sp.panel_id )
                {
                    dup = true;
                    break;
                }
            }
            if( !dup )
            {
                sp.plugin_dir = subDir;
                sp.exe_path = subDir + "/" + ep;
                aOut.push_back( sp );
            }
        }
    }
    ::closedir( dp );
}

std::vector<PLUGIN_PANEL_SPEC> DiscoverSpecs()
{
    std::vector<PLUGIN_PANEL_SPEC> specs;
    ScanPluginsDir( PATHS::GetUserPluginsPath().ToStdString(), specs );
    std::string pcmRoot = PATHS::GetDefault3rdPartyPath().ToStdString();
    ScanPluginsDir( pcmRoot + "/plugins", specs );
    ScanPluginsDir( pcmRoot, specs );
    return specs;
}
}

class PANEL_PILL_BADGE : public wxPanel
{
public:
    PANEL_PILL_BADGE( wxWindow* aParent, PANEL_PLUGIN_SURFACE* aSurf, int aWidgetId ) :
            wxPanel( aParent, wxID_ANY, wxDefaultPosition, aParent->FromDIP( wxSize( 76, 22 ) ), wxBORDER_NONE ),
            m_surf( aSurf ),
            m_widgetId( aWidgetId ),
            m_fill( 46, 160, 128 )
    {
        SetBackgroundStyle( wxBG_STYLE_PAINT );
        Bind( wxEVT_PAINT, &PANEL_PILL_BADGE::onPaint, this );
        Bind( wxEVT_LEFT_DOWN, &PANEL_PILL_BADGE::onClick, this );
    }

    void SetBadge( const std::string& aText, uint32_t aRgba, int aWidgetId )
    {
        m_widgetId = aWidgetId;
        wxColour fill = UnpackRgba( aRgba, wxColour( 46, 160, 128 ) );
        if( m_text == aText && m_fill == fill )
            return;
        m_text = aText;
        m_fill = fill;
        wxClientDC dc( this );
        wxFont f = GetFont();
        f.MakeBold();
        dc.SetFont( f );
        wxSize ts = dc.GetTextExtent( wxString::FromUTF8( m_text ) );
        wxSize minSz( std::max( FromDIP( 64 ), ts.x + FromDIP( 16 ) ), std::max( FromDIP( 20 ), ts.y + FromDIP( 6 ) ) );
        SetMinSize( minSz );
        Refresh( false );
    }

private:
    void onPaint( wxPaintEvent& )
    {
        wxAutoBufferedPaintDC dc( this );
        wxSize sz = GetClientSize();
        wxColour bg = GetParent() ? GetParent()->GetBackgroundColour() : wxSystemSettings::GetColour( wxSYS_COLOUR_WINDOW );
        dc.SetBrush( wxBrush( bg ) );
        dc.SetPen( *wxTRANSPARENT_PEN );
        dc.DrawRectangle( 0, 0, sz.x, sz.y );

        dc.SetBrush( wxBrush( m_fill ) );
        dc.SetPen( wxPen( m_fill.ChangeLightness( 80 ), 1 ) );
        dc.DrawRoundedRectangle( 0, 1, std::max( 2, sz.x ), std::max( 2, sz.y - 2 ), FromDIP( 4 ) );

        wxFont f = GetFont();
        f.MakeBold();
        dc.SetFont( f );
        int lum = static_cast<int>( m_fill.Red() ) * 299 + static_cast<int>( m_fill.Green() ) * 587 + static_cast<int>( m_fill.Blue() ) * 114;
        dc.SetTextForeground( lum > 145000 ? wxColour( 16, 20, 26 ) : wxColour( 248, 250, 252 ) );
        wxString ws = wxString::FromUTF8( m_text );
        wxSize ts = dc.GetTextExtent( ws );
        dc.DrawText( ws, std::max( 2, ( sz.x - ts.x ) / 2 ), std::max( 0, ( sz.y - ts.y ) / 2 ) );
    }

    void onClick( wxMouseEvent& )
    {
        if( m_surf )
            m_surf->PushWidgetClickById( m_widgetId );
    }

    PANEL_PLUGIN_SURFACE* m_surf = nullptr;
    int m_widgetId = 0;
    std::string m_text;
    wxColour m_fill;
};

class PANEL_CANVAS_WIDGET : public wxPanel
{
public:
    PANEL_CANVAS_WIDGET( wxWindow* aParent, PANEL_PLUGIN_SURFACE* aSurf, int aCanvasIdx, int aDipH ) :
            wxPanel( aParent, wxID_ANY, wxDefaultPosition, aParent->FromDIP( wxSize( 280, aDipH ) ), wxBORDER_NONE ),
            m_surf( aSurf ),
            m_canvasIdx( aCanvasIdx )
    {
        SetBackgroundStyle( wxBG_STYLE_PAINT );
        SetMinSize( aParent->FromDIP( wxSize( 240, aDipH ) ) );
        Bind( wxEVT_PAINT, &PANEL_CANVAS_WIDGET::onPaint, this );
        Bind( wxEVT_SIZE, &PANEL_CANVAS_WIDGET::onSize, this );
        Bind( wxEVT_MOTION, &PANEL_CANVAS_WIDGET::onMouse, this );
        Bind( wxEVT_LEFT_DOWN, &PANEL_CANVAS_WIDGET::onMouse, this );
        Bind( wxEVT_LEFT_UP, &PANEL_CANVAS_WIDGET::onMouse, this );
        Bind( wxEVT_MOUSEWHEEL, &PANEL_CANVAS_WIDGET::onMouse, this );
        Bind( wxEVT_LEAVE_WINDOW, &PANEL_CANVAS_WIDGET::onLeave, this );
    }

    int CanvasIndex() const { return m_canvasIdx; }

    void NotifyCanvasDirty()
    {
        Refresh( false );
    }

private:
    void forwardPos( wxMouseEvent& aEv, int aSubEv, int aWheel )
    {
        if( !m_surf )
            return;
        int cw = 0;
        int ch = 0;
        uint32_t seq = 0;
        m_surf->CanvasBuffer( m_canvasIdx, cw, ch, seq );
        wxSize sz = GetClientSize();
        if( cw <= 0 || ch <= 0 || sz.x <= 0 || sz.y <= 0 )
            return;
        wxPoint pt = aEv.GetPosition();
        int lx = std::clamp( ( pt.x * cw ) / sz.x, 0, cw - 1 );
        int ly = std::clamp( ( pt.y * ch ) / sz.y, 0, ch - 1 );
        m_surf->PushCanvasMouse( m_canvasIdx, aSubEv, lx, ly, aWheel );
    }

    void onMouse( wxMouseEvent& aEv )
    {
        if( aEv.LeftDown() )
            forwardPos( aEv, 2, 0 );
        else if( aEv.LeftUp() )
            forwardPos( aEv, 3, 0 );
        else if( aEv.GetWheelRotation() != 0 )
            forwardPos( aEv, 4, aEv.GetWheelRotation() );
        else
            forwardPos( aEv, 1, 0 );
        aEv.Skip();
    }

    void onLeave( wxMouseEvent& aEv )
    {
        if( m_surf )
            m_surf->PushCanvasMouse( m_canvasIdx, 5, -1, -1, 0 );
        aEv.Skip();
    }

    void onSize( wxSizeEvent& aEv )
    {
        Refresh( false );
        aEv.Skip();
    }

    void onPaint( wxPaintEvent& )
    {
        wxAutoBufferedPaintDC dc( this );
        wxSize sz = GetClientSize();
        if( sz.x <= 0 || sz.y <= 0 )
            return;
        int cw = 0;
        int ch = 0;
        uint32_t seq = 0;
        const uint8_t* src = m_surf ? m_surf->CanvasBuffer( m_canvasIdx, cw, ch, seq ) : nullptr;
        if( !src || cw <= 0 || ch <= 0 )
        {
            dc.SetBrush( wxBrush( wxColour( 14, 20, 30 ) ) );
            dc.SetPen( *wxTRANSPARENT_PEN );
            dc.DrawRectangle( 0, 0, sz.x, sz.y );
            return;
        }
        double dpi = GetDPIScaleFactor();
        int physW = std::max( 1, static_cast<int>( std::lround( sz.x * dpi ) ) );
        int physH = std::max( 1, static_cast<int>( std::lround( sz.y * dpi ) ) );
        if( !m_bmp.IsOk() || m_renderedSeq != seq || m_renderedPhys != wxSize( physW, physH ) )
        {
            wxImage img( physW, physH, false );
            unsigned char* dst = img.GetData();
            for( int py = 0; py < physH; ++py )
            {
                int sy = std::min( ch - 1, ( py * ch ) / physH );
                const uint8_t* srow = src + static_cast<size_t>( sy * cw ) * 4;
                unsigned char* drow = dst + static_cast<size_t>( py * physW ) * 3;
                for( int px = 0; px < physW; ++px )
                {
                    int sx = std::min( cw - 1, ( px * cw ) / physW );
                    const uint8_t* sp = srow + sx * 4;
                    uint32_t a = sp[3];
                    if( a == 255u )
                    {
                        drow[px * 3 + 0] = sp[0];
                        drow[px * 3 + 1] = sp[1];
                        drow[px * 3 + 2] = sp[2];
                    }
                    else
                    {
                        uint32_t inv = 255u - a;
                        drow[px * 3 + 0] = static_cast<unsigned char>( ( sp[0] * a + 14u * inv ) / 255u );
                        drow[px * 3 + 1] = static_cast<unsigned char>( ( sp[1] * a + 20u * inv ) / 255u );
                        drow[px * 3 + 2] = static_cast<unsigned char>( ( sp[2] * a + 30u * inv ) / 255u );
                    }
                }
            }
            m_bmp = wxBitmap( img );
            m_bmp.SetScaleFactor( dpi );
            m_renderedSeq = seq;
            m_renderedPhys = wxSize( physW, physH );
        }
        dc.DrawBitmap( m_bmp, 0, 0, false );
    }

    PANEL_PLUGIN_SURFACE* m_surf = nullptr;
    int m_canvasIdx = 0;
    uint32_t m_renderedSeq = 0;
    wxSize m_renderedPhys;
    wxBitmap m_bmp;
};

class PANEL_PLUGIN_DIALOG : public wxDialog
{
public:
    PANEL_PLUGIN_DIALOG( wxWindow* aParent, PANEL_PLUGIN_SURFACE* aSurf ) :
            wxDialog( aParent, wxID_ANY, wxS( "Plugin Inspector" ), wxDefaultPosition, aParent->FromDIP( wxSize( 620, 680 ) ), wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER ),
            m_surf( aSurf )
    {
        auto* outer = new wxBoxSizer( wxVERTICAL );
        m_scroller = new wxScrolledWindow( this, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxVSCROLL );
        m_scroller->SetScrollRate( 0, FromDIP( 8 ) );
        m_scroller->SetSizer( new wxBoxSizer( wxVERTICAL ) );
        outer->Add( m_scroller, 1, wxEXPAND | wxALL, FromDIP( 8 ) );
        SetSizer( outer );
        Bind( wxEVT_CLOSE_WINDOW, &PANEL_PLUGIN_DIALOG::onClose, this );
        Bind( wxEVT_CHAR_HOOK, &PANEL_PLUGIN_DIALOG::onCharHook, this );
    }

    wxScrolledWindow* Scroller() const { return m_scroller; }

private:
    void onClose( wxCloseEvent& )
    {
        Hide();
        if( m_surf )
            m_surf->NotifyDialogClosedByUser();
    }

    void onCharHook( wxKeyEvent& aEv )
    {
        if( aEv.GetKeyCode() == WXK_ESCAPE )
        {
            Hide();
            if( m_surf )
                m_surf->NotifyDialogClosedByUser();
            return;
        }
        aEv.Skip();
    }

    PANEL_PLUGIN_SURFACE* m_surf = nullptr;
    wxScrolledWindow* m_scroller = nullptr;
};

class PANEL_PLUGIN_OVERLAY_ITEM : public KIGFX::VIEW_ITEM
{
public:
    PANEL_PLUGIN_OVERLAY_ITEM() :
            KIGFX::VIEW_ITEM( false, false )
    {
    }

    wxString GetClass() const override
    {
        return wxS( "PANEL_PLUGIN_OVERLAY_ITEM" );
    }

    const BOX2I ViewBBox() const override
    {
        return m_bbox;
    }

    std::vector<int> ViewGetLayers() const override
    {
        return { LAYER_GP_OVERLAY };
    }

    void SetPanelVisible( bool aVis )
    {
        m_panelVisible = aVis;
    }

    void ClearImage()
    {
        m_refImg.reset();
        m_hasImage = false;
        m_bbox = BOX2I();
    }

    void SetOverlayFromShm( const uint8_t* aSrc, int aW, int aH, bool aIsBgra, int64_t aCxIu, int64_t aCyIu, double aScaleRatio, PCB_LAYER_ID aLayer )
    {
        if( !aSrc || aW <= 0 || aH <= 0 )
        {
            ClearImage();
            return;
        }
        size_t npx = static_cast<size_t>( aW ) * static_cast<size_t>( aH );
        auto* rgb = static_cast<unsigned char*>( std::malloc( npx * 3 ) );
        auto* alpha = static_cast<unsigned char*>( std::malloc( npx ) );
        if( !rgb || !alpha )
        {
            std::free( rgb );
            std::free( alpha );
            return;
        }
        if( aIsBgra )
        {
            for( size_t i = 0; i < npx; ++i )
            {
                rgb[i * 3 + 0] = aSrc[i * 4 + 2];
                rgb[i * 3 + 1] = aSrc[i * 4 + 1];
                rgb[i * 3 + 2] = aSrc[i * 4 + 0];
                alpha[i] = aSrc[i * 4 + 3];
            }
        }
        else
        {
            for( size_t i = 0; i < npx; ++i )
            {
                rgb[i * 3 + 0] = aSrc[i * 4 + 0];
                rgb[i * 3 + 1] = aSrc[i * 4 + 1];
                rgb[i * 3 + 2] = aSrc[i * 4 + 2];
                alpha[i] = aSrc[i * 4 + 3];
            }
        }
        wxImage img( aW, aH, rgb, alpha, false );
        auto ref = std::make_unique<REFERENCE_IMAGE>( pcbIUScale );
        ref->SetImage( img );
        ref->SetPosition( VECTOR2I( static_cast<int>( aCxIu ), static_cast<int>( aCyIu ) ) );
        ref->SetImageScale( aScaleRatio > 1e-6 ? aScaleRatio : 1.0 );
        m_pcbLayer = aLayer;
        m_bbox = ref->GetBoundingBox();
        m_refImg = std::move( ref );
        m_hasImage = true;
    }

    void ViewDraw( int, KIGFX::VIEW* aView ) const override
    {
        if( !m_panelVisible || !m_hasImage || !m_refImg || !aView || !aView->IsLayerVisible( m_pcbLayer ) )
            return;
        KIGFX::GAL* gal = aView->GetGAL();
        if( !gal )
            return;
        gal->Save();
        gal->Translate( VECTOR2D( m_refImg->GetPosition() ) );
        double sc = m_refImg->GetImageScale();
        if( sc != 1.0 )
            gal->Scale( VECTOR2D( sc, sc ) );
        gal->DrawBitmap( m_refImg->GetImage(), 0.92 );
        gal->Restore();
    }

private:
    std::unique_ptr<REFERENCE_IMAGE> m_refImg;
    BOX2I m_bbox;
    PCB_LAYER_ID m_pcbLayer = User_9;
    bool m_hasImage = false;
    bool m_panelVisible = false;
};

PANEL_PLUGIN_SURFACE::PANEL_PLUGIN_SURFACE( PCB_EDIT_FRAME* aFrame, const PLUGIN_PANEL_SPEC& aSpec ) :
        wxPanel( aFrame, wxID_ANY, wxDefaultPosition, wxSize( aSpec.init_w, aSpec.init_h ), wxWANTS_CHARS | wxBORDER_NONE ),
        m_frame( aFrame ),
        m_spec( aSpec )
{
    SetBackgroundStyle( wxBG_STYLE_PAINT );
    m_ctrlBaseId = wxWindow::NewControlId( KP_DOCK_SLOTS_V + KP_DIALOG_SLOTS_V );

    auto* topSizer = new wxBoxSizer( wxVERTICAL );
    m_scroller = new wxScrolledWindow( this, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxVSCROLL );
    m_scroller->SetScrollRate( 0, FromDIP( 8 ) );
    m_scroller->SetSizer( new wxBoxSizer( wxVERTICAL ) );
    m_scroller->Hide();
    topSizer->Add( m_scroller, 1, wxEXPAND );
    SetSizer( topSizer );

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
            std::memset( m_shmAddr, 0, KP_CANVAS0_OFF_V );
            i32[4] = m_spec.init_w;
            i32[5] = m_spec.init_h;
            i32[6] = m_spec.init_w;
            i32[7] = 2;
            f32[10] = 0.1f;
            f32[13] = static_cast<float>( GetDPIScaleFactor() );
            u32[1] = KP_VERSION_V;
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
    if( m_dialog )
    {
        m_dialog->Destroy();
        m_dialog = nullptr;
    }
    delete m_overlayItem;
    m_overlayItem = nullptr;
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

    std::string apiSock;
    std::string apiTok;
    if( KICAD_API_SERVER* srv = Pgm().ApiServerOrNull() )
    {
        if( !srv->Running() )
        {
            srv->Start();
            srv->SetReadyToReply( true );
        }
        apiSock = srv->SocketPath();
        apiTok = srv->Token();
    }

    pid_t pid = ::fork();
    if( pid == 0 )
    {
        if( !m_spec.plugin_dir.empty() )
        {
            int cr = ::chdir( m_spec.plugin_dir.c_str() );
            (void) cr;
        }
        ::setenv( "KICAD_PANEL_SHM", m_shmPath.c_str(), 1 );
        if( !apiSock.empty() )
            ::setenv( "KICAD_API_SOCKET", apiSock.c_str(), 1 );
        if( !apiTok.empty() )
            ::setenv( "KICAD_API_TOKEN", apiTok.c_str(), 1 );
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

bool PANEL_PLUGIN_SURFACE::ConsumeToggleRequest()
{
    if( !m_shmAddr )
        return false;
    auto* i32 = static_cast<int32_t*>( m_shmAddr );
    if( ( i32[7] & 32 ) == 0 )
        return false;
    i32[7] &= ~32;
    return true;
}

int PANEL_PLUGIN_SURFACE::PollRequestedWidth()
{
    if( !m_shmAddr )
        return 0;
    auto* i32 = static_cast<const int32_t*>( m_shmAddr );
    if( i32[18] > 0 )
        return 0;
    int rw = std::max( 64, std::min( KP_MAX_DIM_V, static_cast<int>( i32[6] ) ) );
    if( rw == m_lastReqW )
        return 0;
    m_lastReqW = rw;
    return rw;
}

const uint8_t* PANEL_PLUGIN_SURFACE::CanvasBuffer( int aCanvasIdx, int& aOutW, int& aOutH, uint32_t& aOutSeq ) const
{
    if( !m_shmAddr || aCanvasIdx < 0 || aCanvasIdx > 2 )
    {
        aOutW = 0;
        aOutH = 0;
        aOutSeq = 0;
        return nullptr;
    }
    const auto* u32 = static_cast<const uint32_t*>( m_shmAddr );
    const auto* i32 = static_cast<const int32_t*>( m_shmAddr );
    int base = 20 + aCanvasIdx * 3;
    int maxH = aCanvasIdx == 0 ? 480 : ( aCanvasIdx == 1 ? 400 : 640 );
    aOutSeq = __atomic_load_n( &u32[base], __ATOMIC_ACQUIRE );
    aOutW = std::clamp( static_cast<int>( i32[base + 1] ), 0, 640 );
    aOutH = std::clamp( static_cast<int>( i32[base + 2] ), 0, maxH );
    size_t off = aCanvasIdx == 0 ? KP_CANVAS0_OFF_V : ( aCanvasIdx == 1 ? KP_CANVAS1_OFF_V : KP_CANVAS2_OFF_V );
    return static_cast<const uint8_t*>( m_shmAddr ) + off;
}

void PANEL_PLUGIN_SURFACE::PushCanvasMouse( int aCanvasIdx, int aSubEv, int aLx, int aLy, int aWheel )
{
    int32_t packedXy = ( aLx & 0xFFFF ) | ( ( aLy & 0xFFFF ) << 16 );
    int32_t packedAux = ( aSubEv & 0xFF ) | ( aWheel << 8 );
    pushEvent( KP_EV_CANVAS_MOUSE_V, aCanvasIdx, packedXy, packedAux );
}

void PANEL_PLUGIN_SURFACE::NotifyDialogClosedByUser()
{
    if( m_shmAddr )
    {
        auto* i32 = static_cast<int32_t*>( m_shmAddr );
        i32[7] &= ~64;
    }
    pushEvent( KP_EV_DIALOG_CLOSED_V, 0, 0, 0 );
}

void PANEL_PLUGIN_SURFACE::PushWidgetClickById( int aWidgetId )
{
    pushEvent( KP_EV_WIDGET_CLICK_V, aWidgetId, 0, 0 );
}

int PANEL_PLUGIN_SURFACE::slotWidgetId( int aSlotIdx ) const
{
    if( !m_shmAddr || aSlotIdx < 0 || aSlotIdx >= KP_DOCK_SLOTS_V + KP_DIALOG_SLOTS_V )
        return 0;
    const auto* base = static_cast<const uint8_t*>( m_shmAddr ) + KP_WIDGET_OFF_V + static_cast<size_t>( aSlotIdx ) * KP_WIDGET_STRIDE_V;
    return *reinterpret_cast<const int32_t*>( base );
}

void PANEL_PLUGIN_SURFACE::commitSlotText( int aSlotIdx )
{
    if( !m_shmAddr || aSlotIdx < 0 || aSlotIdx >= KP_DOCK_SLOTS_V + KP_DIALOG_SLOTS_V )
        return;
    wxTextCtrl* tc = nullptr;
    const auto& vec = aSlotIdx < KP_DOCK_SLOTS_V ? m_dockWidgets : m_dlgWidgets;
    for( const auto& w : vec )
    {
        if( w.slot_idx == aSlotIdx )
        {
            tc = w.text;
            break;
        }
    }
    if( !tc )
        return;
    std::string val( tc->GetValue().ToUTF8() );
    auto* dst = static_cast<uint8_t*>( m_shmAddr ) + KP_TEXT_COMMIT_OFF_V + static_cast<size_t>( aSlotIdx ) * 64;
    WriteShmStr( dst, 64, val );
    pushEvent( KP_EV_WIDGET_TEXT_V, slotWidgetId( aSlotIdx ), aSlotIdx, 1 );
}

void PANEL_PLUGIN_SURFACE::HandleWidgetButton( wxCommandEvent& aEv )
{
    int slotIdx = aEv.GetId() - m_ctrlBaseId;
    if( slotIdx >= 0 && slotIdx < KP_DOCK_SLOTS_V + KP_DIALOG_SLOTS_V )
        pushEvent( KP_EV_WIDGET_CLICK_V, slotWidgetId( slotIdx ), 0, 0 );
}

void PANEL_PLUGIN_SURFACE::HandleWidgetChoice( wxCommandEvent& aEv )
{
    int slotIdx = aEv.GetId() - m_ctrlBaseId;
    if( slotIdx >= 0 && slotIdx < KP_DOCK_SLOTS_V + KP_DIALOG_SLOTS_V )
        pushEvent( KP_EV_WIDGET_SELECT_V, slotWidgetId( slotIdx ), aEv.GetSelection(), 0 );
}

void PANEL_PLUGIN_SURFACE::HandleWidgetCheck( wxCommandEvent& aEv )
{
    int slotIdx = aEv.GetId() - m_ctrlBaseId;
    if( slotIdx >= 0 && slotIdx < KP_DOCK_SLOTS_V + KP_DIALOG_SLOTS_V )
        pushEvent( KP_EV_WIDGET_CHECK_V, slotWidgetId( slotIdx ), aEv.IsChecked() ? 1 : 0, 0 );
}

void PANEL_PLUGIN_SURFACE::HandleWidgetTextEnter( wxCommandEvent& aEv )
{
    int slotIdx = aEv.GetId() - m_ctrlBaseId;
    commitSlotText( slotIdx );
}

void PANEL_PLUGIN_SURFACE::HandleWidgetTextBlur( wxFocusEvent& aEv )
{
    if( wxWindow* w = dynamic_cast<wxWindow*>( aEv.GetEventObject() ) )
        commitSlotText( w->GetId() - m_ctrlBaseId );
    aEv.Skip();
}

void PANEL_PLUGIN_SURFACE::reconcileContainer( wxWindow* aParent, std::vector<KP_BUILT_WIDGET>& aBuilt, int aSlotOffset, int aCount )
{
    if( !m_shmAddr || !aParent )
        return;
    const auto* shmBytes = static_cast<const uint8_t*>( m_shmAddr );
    bool needRebuild = ( static_cast<int>( aBuilt.size() ) != aCount );
    for( int i = 0; !needRebuild && i < aCount; ++i )
    {
        const uint8_t* sb = shmBytes + KP_WIDGET_OFF_V + static_cast<size_t>( aSlotOffset + i ) * KP_WIDGET_STRIDE_V;
        const auto* wi = reinterpret_cast<const int32_t*>( sb );
        bool hasLbl = sb[32] != 0;
        bool hasAct = ( wi[1] == KP_W_SECTION_V ) && ( sb[128] != 0 );
        const KP_BUILT_WIDGET& bw = aBuilt[static_cast<size_t>( i )];
        if( bw.id != wi[0] || bw.kind != wi[1] || bw.row_group != wi[2] || bw.min_h != wi[5] || bw.has_label != hasLbl || bw.has_action != hasAct )
            needRebuild = true;
    }

    if( needRebuild )
    {
        aParent->Freeze();
        wxSizer* sizer = aParent->GetSizer();
        sizer->Clear( true );
        aBuilt.clear();
        aBuilt.reserve( static_cast<size_t>( aCount ) );

        wxBoxSizer* curRowSizer = nullptr;
        int curGroup = 0;

        for( int i = 0; i < aCount; ++i )
        {
            int slotIdx = aSlotOffset + i;
            int ctrlId = m_ctrlBaseId + slotIdx;
            const uint8_t* sb = shmBytes + KP_WIDGET_OFF_V + static_cast<size_t>( slotIdx ) * KP_WIDGET_STRIDE_V;
            const auto* wi = reinterpret_cast<const int32_t*>( sb );

            KP_BUILT_WIDGET bw;
            bw.slot_idx = slotIdx;
            bw.id = wi[0];
            bw.kind = wi[1];
            bw.row_group = wi[2];
            bw.min_h = wi[5];
            bw.has_label = sb[32] != 0;
            bw.has_action = ( bw.kind == KP_W_SECTION_V ) && ( sb[128] != 0 );

            wxSizer* targetSizer = sizer;
            int padFlag = wxEXPAND | wxLEFT | wxRIGHT | wxTOP;
            int padPx = aParent->FromDIP( 4 );
            if( bw.row_group > 0 )
            {
                if( curGroup != bw.row_group || !curRowSizer )
                {
                    curRowSizer = new wxBoxSizer( wxHORIZONTAL );
                    sizer->Add( curRowSizer, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, aParent->FromDIP( 4 ) );
                    curGroup = bw.row_group;
                }
                targetSizer = curRowSizer;
                padFlag = wxEXPAND | wxRIGHT;
                padPx = aParent->FromDIP( 4 );
            }
            else
            {
                curRowSizer = nullptr;
                curGroup = 0;
            }

            if( bw.kind == KP_W_HEADER_V )
            {
                auto* row = new wxBoxSizer( wxHORIZONTAL );
                bw.left_lbl = new wxStaticText( aParent, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize, wxST_ELLIPSIZE_END );
                wxFont f = bw.left_lbl->GetFont();
                f.MakeBold();
                bw.left_lbl->SetFont( f );
                bw.badge = new PANEL_PILL_BADGE( aParent, this, bw.id );
                row->Add( bw.left_lbl, 1, wxALIGN_CENTER_VERTICAL | wxRIGHT, aParent->FromDIP( 6 ) );
                row->Add( bw.badge, 0, wxALIGN_CENTER_VERTICAL );
                sizer->Add( row, 0, wxEXPAND | wxALL, aParent->FromDIP( 6 ) );
                sizer->Add( new wxStaticLine( aParent ), 0, wxEXPAND | wxLEFT | wxRIGHT, aParent->FromDIP( 4 ) );
            }
            else if( bw.kind == KP_W_SECTION_V )
            {
                sizer->Add( new wxStaticLine( aParent ), 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, aParent->FromDIP( 6 ) );
                auto* row = new wxBoxSizer( wxHORIZONTAL );
                bw.left_lbl = new wxStaticText( aParent, wxID_ANY, wxEmptyString );
                wxFont f = bw.left_lbl->GetFont();
                f.MakeBold();
                bw.left_lbl->SetFont( f );
                row->Add( bw.left_lbl, 1, wxALIGN_CENTER_VERTICAL );
                if( bw.has_action )
                {
                    bw.btn = new wxButton( aParent, ctrlId, wxEmptyString, wxDefaultPosition, wxDefaultSize, wxBU_EXACTFIT );
                    bw.btn->Bind( wxEVT_BUTTON, &PANEL_PLUGIN_SURFACE::HandleWidgetButton, this );
                    row->Add( bw.btn, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, aParent->FromDIP( 4 ) );
                }
                sizer->Add( row, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, aParent->FromDIP( 4 ) );
            }
            else if( bw.kind == KP_W_BUTTON_V )
            {
                bw.btn = new wxButton( aParent, ctrlId, wxEmptyString );
                bw.btn->Bind( wxEVT_BUTTON, &PANEL_PLUGIN_SURFACE::HandleWidgetButton, this );
                targetSizer->Add( bw.btn, bw.row_group > 0 ? 1 : 0, padFlag, padPx );
            }
            else if( bw.kind == KP_W_CHOICE_V )
            {
                auto* cell = new wxBoxSizer( wxHORIZONTAL );
                if( bw.has_label )
                {
                    bw.left_lbl = new wxStaticText( aParent, wxID_ANY, wxEmptyString );
                    cell->Add( bw.left_lbl, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, aParent->FromDIP( 4 ) );
                }
                bw.choice = new wxChoice( aParent, ctrlId );
                bw.choice->Bind( wxEVT_CHOICE, &PANEL_PLUGIN_SURFACE::HandleWidgetChoice, this );
                cell->Add( bw.choice, 1, wxALIGN_CENTER_VERTICAL );
                targetSizer->Add( cell, bw.row_group > 0 ? 1 : 0, padFlag, padPx );
            }
            else if( bw.kind == KP_W_CHECKBOX_V )
            {
                bw.check = new wxCheckBox( aParent, ctrlId, wxEmptyString );
                bw.check->Bind( wxEVT_CHECKBOX, &PANEL_PLUGIN_SURFACE::HandleWidgetCheck, this );
                targetSizer->Add( bw.check, bw.row_group > 0 ? 1 : 0, padFlag, padPx );
            }
            else if( bw.kind == KP_W_TEXTBOX_V )
            {
                auto* cell = new wxBoxSizer( wxHORIZONTAL );
                if( bw.has_label )
                {
                    bw.left_lbl = new wxStaticText( aParent, wxID_ANY, wxEmptyString );
                    cell->Add( bw.left_lbl, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, aParent->FromDIP( 4 ) );
                }
                bw.text = new wxTextCtrl( aParent, ctrlId, wxEmptyString, wxDefaultPosition, wxDefaultSize, wxTE_PROCESS_ENTER );
                bw.text->Bind( wxEVT_TEXT_ENTER, &PANEL_PLUGIN_SURFACE::HandleWidgetTextEnter, this );
                bw.text->Bind( wxEVT_KILL_FOCUS, &PANEL_PLUGIN_SURFACE::HandleWidgetTextBlur, this );
                cell->Add( bw.text, 1, wxALIGN_CENTER_VERTICAL );
                targetSizer->Add( cell, bw.row_group > 0 ? 1 : 0, padFlag, padPx );
            }
            else if( bw.kind == KP_W_KV_ROW_V )
            {
                auto* cell = new wxBoxSizer( wxHORIZONTAL );
                bw.left_lbl = new wxStaticText( aParent, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize, wxST_ELLIPSIZE_END );
                bw.right_lbl = new wxStaticText( aParent, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize, wxALIGN_RIGHT );
                cell->Add( bw.left_lbl, 1, wxALIGN_CENTER_VERTICAL | wxRIGHT, aParent->FromDIP( 4 ) );
                cell->Add( bw.right_lbl, 0, wxALIGN_CENTER_VERTICAL );
                targetSizer->Add( cell, bw.row_group > 0 ? 1 : 0, padFlag, padPx );
            }
            else if( bw.kind == KP_W_CANVAS_V )
            {
                int cIdx = std::clamp( static_cast<int>( wi[4] ), 0, 2 );
                int dipH = std::max( 64, bw.min_h );
                bw.canvas = new PANEL_CANVAS_WIDGET( aParent, this, cIdx, dipH );
                sizer->Add( bw.canvas, 0, wxEXPAND | wxALL, aParent->FromDIP( 4 ) );
            }

            aBuilt.push_back( bw );
        }

        sizer->AddSpacer( aParent->FromDIP( 6 ) );
        aParent->Layout();
        if( auto* sw = dynamic_cast<wxScrolledWindow*>( aParent ) )
            sw->FitInside();
        aParent->Thaw();
    }

    wxColour defFg = wxSystemSettings::GetColour( wxSYS_COLOUR_WINDOWTEXT );
    bool anyLabelResized = false;
    for( int i = 0; i < aCount; ++i )
    {
        int slotIdx = aSlotOffset + i;
        const uint8_t* sb = shmBytes + KP_WIDGET_OFF_V + static_cast<size_t>( slotIdx ) * KP_WIDGET_STRIDE_V;
        const auto* wi = reinterpret_cast<const int32_t*>( sb );
        const auto* wu = reinterpret_cast<const uint32_t*>( sb );
        std::string lbl = ReadShmStr( sb + 32, 95 );
        std::string val = ReadShmStr( sb + 128, 1919 );
        int sel = wi[4];
        uint32_t col = wu[6];

        KP_BUILT_WIDGET& bw = aBuilt[static_cast<size_t>( i )];
        if( bw.kind == KP_W_HEADER_V )
        {
            if( bw.left_lbl && bw.cached_label != lbl )
            {
                bw.left_lbl->SetLabel( wxString::FromUTF8( lbl ) );
                bw.cached_label = lbl;
            }
            if( bw.badge && ( bw.cached_value != val || bw.cached_color != col ) )
            {
                bw.badge->SetBadge( val, col, bw.id );
                bw.cached_value = val;
                bw.cached_color = col;
                anyLabelResized = true;
            }
        }
        else if( bw.kind == KP_W_SECTION_V )
        {
            if( bw.left_lbl && bw.cached_label != lbl )
            {
                bw.left_lbl->SetLabel( wxString::FromUTF8( lbl ) );
                bw.cached_label = lbl;
            }
            if( bw.btn && bw.cached_value != val )
            {
                bw.btn->SetLabel( wxString::FromUTF8( val ) );
                bw.cached_value = val;
                anyLabelResized = true;
            }
        }
        else if( bw.kind == KP_W_BUTTON_V && bw.btn )
        {
            if( bw.cached_label != lbl )
            {
                bw.btn->SetLabel( wxString::FromUTF8( lbl ) );
                bw.cached_label = lbl;
            }
        }
        else if( bw.kind == KP_W_CHOICE_V && bw.choice )
        {
            if( bw.left_lbl && bw.cached_label != lbl )
            {
                bw.left_lbl->SetLabel( wxString::FromUTF8( lbl ) );
                bw.cached_label = lbl;
            }
            if( bw.cached_value != val )
            {
                bw.choice->Set( SplitLinesWx( val ) );
                bw.cached_value = val;
                bw.cached_sel = -99999;
            }
            int cnt = static_cast<int>( bw.choice->GetCount() );
            int clampedSel = cnt > 0 ? std::clamp( sel, 0, cnt - 1 ) : wxNOT_FOUND;
            if( bw.cached_sel != clampedSel && clampedSel != wxNOT_FOUND )
            {
                bw.choice->SetSelection( clampedSel );
                bw.cached_sel = clampedSel;
            }
        }
        else if( bw.kind == KP_W_CHECKBOX_V && bw.check )
        {
            if( bw.cached_label != lbl )
            {
                bw.check->SetLabel( wxString::FromUTF8( lbl ) );
                bw.cached_label = lbl;
            }
            bool chk = sel != 0;
            if( bw.cached_sel != ( chk ? 1 : 0 ) )
            {
                bw.check->SetValue( chk );
                bw.cached_sel = chk ? 1 : 0;
            }
        }
        else if( bw.kind == KP_W_TEXTBOX_V && bw.text )
        {
            if( bw.left_lbl && bw.cached_label != lbl )
            {
                bw.left_lbl->SetLabel( wxString::FromUTF8( lbl ) );
                bw.cached_label = lbl;
            }
            if( wxWindow::FindFocus() != bw.text && bw.cached_value != val )
            {
                bw.text->ChangeValue( wxString::FromUTF8( val ) );
                bw.cached_value = val;
            }
        }
        else if( bw.kind == KP_W_KV_ROW_V )
        {
            if( bw.left_lbl && bw.cached_label != lbl )
            {
                bw.left_lbl->SetLabel( wxString::FromUTF8( lbl ) );
                bw.cached_label = lbl;
                anyLabelResized = true;
            }
            if( bw.right_lbl && ( bw.cached_value != val || bw.cached_color != col ) )
            {
                bw.right_lbl->SetLabel( wxString::FromUTF8( val ) );
                bw.right_lbl->SetForegroundColour( UnpackRgba( col, defFg ) );
                bw.cached_value = val;
                bw.cached_color = col;
                anyLabelResized = true;
            }
        }
    }
    if( anyLabelResized )
        aParent->Layout();
}

void PANEL_PLUGIN_SURFACE::syncDialogState()
{
    if( !m_shmAddr )
        return;
    const auto* u32 = static_cast<const uint32_t*>( m_shmAddr );
    const auto* i32 = static_cast<const int32_t*>( m_shmAddr );
    bool wantOpen = IsShownOnScreen() && ( ( i32[7] & 64 ) != 0 ) && ( i32[19] > 0 );
    uint32_t dgen = __atomic_load_n( &u32[15], __ATOMIC_ACQUIRE );
    if( !wantOpen )
    {
        if( m_dialog && m_dialog->IsShown() )
            m_dialog->Hide();
        return;
    }
    if( !m_dialog )
        m_dialog = new PANEL_PLUGIN_DIALOG( m_frame, this );
    if( dgen != m_lastDialogGen || !m_dialog->IsShown() )
    {
        m_lastDialogGen = dgen;
        std::string title = ReadShmStr( static_cast<const uint8_t*>( m_shmAddr ) + 256, 127 );
        if( !title.empty() )
            m_dialog->SetTitle( wxString::FromUTF8( title ) );
        int cnt = std::clamp( static_cast<int>( i32[19] ), 0, KP_DIALOG_SLOTS_V );
        reconcileContainer( m_dialog->Scroller(), m_dlgWidgets, KP_DOCK_SLOTS_V, cnt );
    }
    if( !m_dialog->IsShown() )
        m_dialog->Show( true );
}

void PANEL_PLUGIN_SURFACE::syncOverlayState( bool aPanelShown )
{
    if( !m_shmAddr || !m_frame )
        return;
    PCB_DRAW_PANEL_GAL* canvas = m_frame->GetCanvas();
    if( !canvas )
        return;
    KIGFX::PCB_VIEW* view = canvas->GetView();
    if( !view )
        return;

    const auto* u32 = static_cast<const uint32_t*>( m_shmAddr );
    const auto* i32 = static_cast<const int32_t*>( m_shmAddr );
    const auto* i64 = static_cast<const int64_t*>( m_shmAddr );
    const auto* f64 = static_cast<const double*>( m_shmAddr );
    uint32_t oseq = __atomic_load_n( &u32[29], __ATOMIC_ACQUIRE );

    if( oseq == 0 && !m_overlayItem )
        return;

    if( !m_overlayItem )
        m_overlayItem = new PANEL_PLUGIN_OVERLAY_ITEM();
    if( !m_overlayItem->viewPrivData() )
        view->Add( m_overlayItem );

    bool visChanged = ( aPanelShown != m_wasShown );
    m_overlayItem->SetPanelVisible( aPanelShown );

    if( aPanelShown && oseq != m_lastOverlaySeq )
    {
        int ow = std::clamp( static_cast<int>( i32[30] ), 0, KP_MAX_DIM_V );
        int oh = std::clamp( static_cast<int>( i32[31] ), 0, KP_MAX_DIM_V );
        int rawLay = i32[32];
        PCB_LAYER_ID pcbLay = ( rawLay > 0 && rawLay < PCB_LAYER_ID_COUNT ) ? static_cast<PCB_LAYER_ID>( rawLay ) : User_9;
        bool isBgra = ( i32[33] == 0 );
        int64_t cxIu = i64[18];
        int64_t cyIu = i64[19];
        double scRatio = f64[20];

        if( BOARD* brd = m_frame->GetBoard() )
        {
            if( brd != m_preparedBoard )
            {
                m_preparedBoard = brd;
                LSET en = brd->GetEnabledLayers();
                if( !en[pcbLay] )
                {
                    en.set( pcbLay );
                    brd->SetEnabledLayers( en );
                }
                LSET vis = brd->GetVisibleLayers();
                if( !vis[pcbLay] )
                {
                    vis.set( pcbLay );
                    brd->SetVisibleLayers( vis );
                }
                view->SetLayerVisible( pcbLay, true );
            }
        }

        const uint8_t* src = static_cast<const uint8_t*>( m_shmAddr ) + KP_OVERLAY_OFF_V;
        if( ow > 0 && oh > 0 )
            m_overlayItem->SetOverlayFromShm( src, ow, oh, isBgra, cxIu, cyIu, scRatio, pcbLay );
        else
            m_overlayItem->ClearImage();

        m_lastOverlaySeq = oseq;
        view->Update( m_overlayItem, KIGFX::REPAINT | KIGFX::GEOMETRY );
        canvas->Refresh();
    }
    else if( visChanged )
    {
        view->Update( m_overlayItem, KIGFX::REPAINT );
        canvas->Refresh();
    }
}

void PANEL_PLUGIN_SURFACE::TickFromRegistry( double aWxMm, double aWyMm, double aZoomMpp, int aLayer, uint32_t aCanvasFlags, const std::string& aBoardPath )
{
    if( !m_shmAddr )
        return;
    bool shown = IsShownOnScreen();
    if( shown )
        EnsurePluginRunning();
    auto* u32 = static_cast<uint32_t*>( m_shmAddr );
    auto* i32 = static_cast<int32_t*>( m_shmAddr );
    auto* f32 = static_cast<float*>( m_shmAddr );
    auto* u8 = static_cast<uint8_t*>( m_shmAddr );
    f32[8] = static_cast<float>( aWxMm );
    f32[9] = static_cast<float>( aWyMm );
    f32[10] = static_cast<float>( aZoomMpp );
    i32[11] = static_cast<int32_t>( aLayer );
    u32[12] = aCanvasFlags;
    f32[13] = static_cast<float>( GetDPIScaleFactor() );
    WriteShmStr( u8 + 512, 1024, aBoardPath );
    if( shown )
        i32[7] |= 3;
    else
        i32[7] = ( i32[7] | 2 ) & ~1;
    __atomic_add_fetch( &u32[2], 1u, __ATOMIC_RELEASE );

    if( shown )
    {
        int dockCnt = std::clamp( static_cast<int>( i32[18] ), 0, KP_DOCK_SLOTS_V );
        uint32_t wgen = __atomic_load_n( &u32[14], __ATOMIC_ACQUIRE );
        if( dockCnt > 0 )
        {
            if( !m_scroller->IsShown() )
            {
                m_scroller->Show( true );
                Layout();
            }
            if( wgen != m_lastWidgetGen || static_cast<int>( m_dockWidgets.size() ) != dockCnt )
            {
                m_lastWidgetGen = wgen;
                reconcileContainer( m_scroller, m_dockWidgets, 0, dockCnt );
            }
        }
        else
        {
            if( m_scroller->IsShown() )
            {
                m_scroller->Show( false );
                Layout();
            }
            uint32_t fseq = __atomic_load_n( &u32[3], __ATOMIC_ACQUIRE );
            if( fseq != m_lastFrameSeq )
            {
                m_lastFrameSeq = fseq;
                Refresh( false );
            }
        }

        for( int ci = 0; ci < 3; ++ci )
        {
            uint32_t cseq = __atomic_load_n( &u32[20 + ci * 3], __ATOMIC_ACQUIRE );
            if( cseq != m_lastCanvasSeq[ci] )
            {
                m_lastCanvasSeq[ci] = cseq;
                for( auto& w : m_dockWidgets )
                    if( w.canvas && w.canvas->CanvasIndex() == ci )
                        w.canvas->NotifyCanvasDirty();
                for( auto& w : m_dlgWidgets )
                    if( w.canvas && w.canvas->CanvasIndex() == ci )
                        w.canvas->NotifyCanvasDirty();
            }
        }
    }

    syncDialogState();
    syncOverlayState( shown );
    m_wasShown = shown;
}

void PANEL_PLUGIN_SURFACE::pushEvent( int32_t aKind, int32_t aX, int32_t aY, int32_t aAux )
{
    if( !m_shmAddr )
        return;
    auto* u32 = static_cast<uint32_t*>( m_shmAddr );
    auto* i32 = static_cast<int32_t*>( m_shmAddr );
    uint32_t wIdx = __atomic_load_n( &u32[16], __ATOMIC_RELAXED );
    size_t base = KP_RING_I32_BASE_V + static_cast<size_t>( wIdx & 255u ) * 4;
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
    if( m_scroller && m_scroller->IsShown() )
    {
        aEv.Skip();
        return;
    }
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
    if( m_scroller && m_scroller->IsShown() )
    {
        aEv.Skip();
        return;
    }
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
    if( m_scroller && m_scroller->IsShown() )
    {
        aEv.Skip();
        return;
    }
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
    wxColour bg = ( m_scroller && m_scroller->IsShown() ) ? wxSystemSettings::GetColour( wxSYS_COLOUR_WINDOW ) : wxColour( 14, 20, 30 );
    dc.SetBrush( wxBrush( bg ) );
    dc.SetPen( *wxTRANSPARENT_PEN );
    dc.DrawRectangle( 0, 0, sz.x, sz.y );
    if( !m_shmAddr || ( m_scroller && m_scroller->IsShown() ) )
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
    if( !m_frame->CanAcceptApiCommands() )
        cFlags |= 2u;
    int actLayer = static_cast<int>( m_frame->GetActiveLayer() );
    std::string boardPath;
    if( BOARD* brd = m_frame->GetBoard() )
        boardPath = std::string( brd->GetFileName().ToUTF8() );

    bool auiDirty = false;
    for( PANEL_PLUGIN_SURFACE* surf : m_surfaces )
    {
        if( m_auiMgr && surf->ConsumeToggleRequest() )
        {
            wxAuiPaneInfo& pane = m_auiMgr->GetPane( wxString::FromUTF8( surf->PaneName() ) );
            if( pane.IsOk() )
            {
                bool show = !pane.IsShown();
                pane.Show( show );
                if( show )
                    surf->EnsurePluginRunning();
                auiDirty = true;
            }
        }
        if( m_auiMgr )
        {
            int rw = surf->PollRequestedWidth();
            if( rw > 0 )
            {
                wxAuiPaneInfo& pane = m_auiMgr->GetPane( wxString::FromUTF8( surf->PaneName() ) );
                if( pane.IsOk() && pane.IsShown() )
                {
                    pane.BestSize( m_frame->FromDIP( wxSize( rw, 720 ) ) );
                    pane.MinSize( m_frame->FromDIP( wxSize( std::min( rw, 324 ), 240 ) ) );
                    auiDirty = true;
                }
            }
        }
        surf->TickFromRegistry( wxMm, wyMm, zoomMpp, actLayer, cFlags, boardPath );
    }
    if( auiDirty && m_auiMgr )
        m_auiMgr->Update();
}
