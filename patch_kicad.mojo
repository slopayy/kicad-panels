from std.ffi import external_call
from std.sys import argv

def _p_read(path: String) -> String:
    var pc = path + String("\0")
    var mc = String("rb\0")
    var fp = external_call["fopen", Int](pc.unsafe_ptr(), mc.unsafe_ptr())
    _ = pc
    _ = mc
    if fp == 0:
        return String("")
    var out = List[UInt8]()
    var chunk = List[UInt8](length=65536, fill=UInt8(0))
    while True:
        var n = external_call["fread", Int](chunk.unsafe_ptr(), 1, 65536, fp)
        if n <= 0:
            break
        for i in range(n):
            out.append(chunk[i])
    _ = external_call["fclose", Int32](fp)
    return String(from_utf8_lossy=out[0:len(out)])

def _p_write(path: String, content: String) -> Bool:
    var pc = path + String("\0")
    var mc = String("wb\0")
    var fp = external_call["fopen", Int](pc.unsafe_ptr(), mc.unsafe_ptr())
    _ = pc
    _ = mc
    if fp == 0:
        return False
    var sb = content.as_bytes()
    var tot = len(sb)
    var off = 0
    while off < tot:
        var w = external_call["fwrite", Int](sb.unsafe_ptr().unsafe_offset(off), 1, tot - off, fp)
        if w <= 0:
            _ = external_call["fclose", Int32](fp)
            return False
        off += w
    _ = external_call["fclose", Int32](fp)
    _ = content
    return True

def _p_find(hay: String, needle: String) -> Int:
    var hb = hay.as_bytes()
    var nb = needle.as_bytes()
    var hl = len(hb)
    var nl = len(nb)
    if nl == 0 or nl > hl:
        return -1
    for i in range(hl - nl + 1):
        var ok = True
        for j in range(nl):
            if hb[i + j] != nb[j]:
                ok = False
                break
        if ok:
            return i
    return -1

def inject_after_anchor(src: String, anchor: String, guard_token: String, addition: String) raises -> String:
    if _p_find(src, guard_token) >= 0:
        return src
    var idx = _p_find(src, anchor)
    if idx < 0:
        raise Error("ANCHOR_MISSING: " + anchor)
    var split = idx + anchor.byte_length()
    var sb = src.as_bytes()
    var head = String(from_utf8_lossy=sb[0:split])
    var tail = String(from_utf8_lossy=sb[split:len(sb)])
    return head + addition + tail

def apply_kicad_panels_patch(kicad_root: String, panels_repo: String, dry_run: Bool) raises -> Int:
    var cmake_p = kicad_root + String("/pcbnew/CMakeLists.txt")
    var frame_h_p = kicad_root + String("/pcbnew/pcb_edit_frame.h")
    var frame_c_p = kicad_root + String("/pcbnew/pcb_edit_frame.cpp")
    var menu_c_p = kicad_root + String("/pcbnew/menubar_pcb_editor.cpp")

    var cmake_s = _p_read(cmake_p)
    var frame_h = _p_read(frame_h_p)
    var frame_c = _p_read(frame_c_p)
    var menu_c = _p_read(menu_c_p)
    if cmake_s.byte_length() == 0 or frame_h.byte_length() == 0 or frame_c.byte_length() == 0 or menu_c.byte_length() == 0:
        raise Error("KICAD_TREE_INCOMPLETE: " + kicad_root)

    var new_cmake = inject_after_anchor(
        cmake_s,
        String("    widgets/panel_constraints.cpp\n"),
        String("widgets/panel_plugin_host.cpp"),
        String("    widgets/panel_plugin_host.cpp\n"),
    )

    var fh1 = inject_after_anchor(
        frame_h,
        String("class PANEL_CONSTRAINTS;\n"),
        String("class PANEL_PLUGIN_REGISTRY;"),
        String("class PANEL_PLUGIN_REGISTRY;\n"),
    ) if _p_find(frame_h, String("class PANEL_CONSTRAINTS;\n")) >= 0 else inject_after_anchor(
        frame_h,
        String("#include <pcb_base_edit_frame.h>\n"),
        String("class PANEL_PLUGIN_REGISTRY;"),
        String("class PANEL_PLUGIN_REGISTRY;\n"),
    )
    var new_frame_h = inject_after_anchor(
        fh1,
        String("PANEL_CONSTRAINTS* m_constraintsPanel = nullptr;"),
        String("m_pluginPanels"),
        String("\n    PANEL_PLUGIN_REGISTRY* m_pluginPanels = nullptr;"),
    )

    var fc1 = inject_after_anchor(
        frame_c,
        String("#include <pcb_edit_frame.h>\n"),
        String("widgets/panel_plugin_host.h"),
        String("#include <widgets/panel_plugin_host.h>\n"),
    )
    var fc2 = inject_after_anchor(
        fc1,
        String("    RestoreAuiLayout();\n"),
        String("PANEL_PLUGIN_REGISTRY::Attach"),
        String("    m_pluginPanels = PANEL_PLUGIN_REGISTRY::Attach( this, m_auimgr );\n    ReCreateMenuBar();\n"),
    )
    var new_frame_c = inject_after_anchor(
        fc2,
        String("    delete m_exportNetlistAction;\n"),
        String("delete m_pluginPanels;"),
        String("    delete m_pluginPanels;\n"),
    )

    var mc1 = inject_after_anchor(
        menu_c,
        String("#include <pcb_edit_frame.h>\n"),
        String("widgets/panel_plugin_host.h"),
        String("#include <widgets/panel_plugin_host.h>\n"),
    )
    var new_menu_c = inject_after_anchor(
        mc1,
        String("showHidePanels->Add( PCB_ACTIONS::showConstraintsPanel,       ACTION_MENU::CHECK );\n"),
        String("PANEL_PLUGIN_REGISTRY::PopulateMenu"),
        String("    PANEL_PLUGIN_REGISTRY::PopulateMenu( this, m_pluginPanels, showHidePanels );\n"),
    )

    var host_h = _p_read(panels_repo + String("/host/panel_plugin_host.h"))
    var host_c = _p_read(panels_repo + String("/host/panel_plugin_host.cpp"))
    if host_h.byte_length() == 0 or host_c.byte_length() == 0:
        raise Error("HOST_SOURCES_MISSING: " + panels_repo)

    if not dry_run:
        if not _p_write(cmake_p, new_cmake):
            raise Error("WRITE_FAIL: " + cmake_p)
        if not _p_write(frame_h_p, new_frame_h):
            raise Error("WRITE_FAIL: " + frame_h_p)
        if not _p_write(frame_c_p, new_frame_c):
            raise Error("WRITE_FAIL: " + frame_c_p)
        if not _p_write(menu_c_p, new_menu_c):
            raise Error("WRITE_FAIL: " + menu_c_p)
        if not _p_write(kicad_root + String("/pcbnew/widgets/panel_plugin_host.h"), host_h):
            raise Error("WRITE_FAIL: panel_plugin_host.h")
        if not _p_write(kicad_root + String("/pcbnew/widgets/panel_plugin_host.cpp"), host_c):
            raise Error("WRITE_FAIL: panel_plugin_host.cpp")
    return 6

def main() raises:
    var args = argv()
    var mode = String(args[1]) if len(args) >= 2 else String("--check")
    var kicad_dir = String(args[2]) if len(args) >= 3 else String("/Users/bee/src/kicad")
    var repo_dir = String(args[3]) if len(args) >= 4 else String("/Users/bee/Hacking/kicad-panels")
    var dry = mode != String("--apply")
    var touched = apply_kicad_panels_patch(kicad_dir, repo_dir, dry)
    print("KICAD_PANELS_PATCH_OK:", "dry_run=" + String(dry), "files=" + String(touched), "target=" + kicad_dir)
