from std.ffi import external_call
from std.math import abs
from std.memory import Pointer
from std.sys import argv
from kicad_panels import (
    KP_EV_DIALOG_CLOSED, KP_EV_DOWN, KP_EV_FOCUS, KP_EV_KEY, KP_EV_MOVE, KP_EV_UP, KP_EV_WHEEL,
    KP_EV_WIDGET_CHECK, KP_EV_WIDGET_CLICK, KP_EV_WIDGET_SELECT,
    kp_begin_dialog, kp_begin_widgets, kp_board_path, kp_button, kp_canvas, kp_canvas_ptr,
    kp_checkbox, kp_choice, kp_clear_rgba, kp_close, kp_close_dialog, kp_discover_manifests,
    kp_draw_text, kp_end_dialog, kp_end_widgets, kp_fb_ptr, kp_header, kp_host_attached,
    kp_host_commit_text, kp_host_dialog_open, kp_host_frame_seq, kp_host_overlay_seq,
    kp_host_push_canvas_mouse, kp_host_push_event, kp_host_set_board_path, kp_host_set_canvas,
    kp_host_set_size, kp_host_widget_count, kp_host_widget_label, kp_host_widget_sel,
    kp_host_widget_value, kp_kv_row, kp_open, kp_parse_manifest_json, kp_poll, kp_present,
    kp_present_canvas, kp_request_width, kp_rgba, kp_section, kp_set_board_overlay, kp_set_key_focus,
    kp_textbox,
)
from patch_kicad import _p_find, _p_read, _p_write, apply_kicad_panels_patch

def _must(cond: Bool, tag: String) raises:
    if not cond:
        raise Error("KICAD_PANELS_TEST_FAIL: " + tag)

def _mkdir_p(path: String):
    var pc = path + String("\0")
    _ = external_call["mkdir", Int32](pc.unsafe_ptr(), UInt16(493))
    _ = pc

def main() raises:
    var shm_key = String("unit_test_panel_abi_v2")
    var stale_c = String("/tmp/kicad_panel_unit_test_panel_abi_v2.shm\0")
    _ = external_call["unlink", Int32](stale_c.unsafe_ptr())
    _ = stale_c
    var host_p = kp_open(shm_key, True, 324, 760)
    var guest_p = kp_open(shm_key, False, 324, 760)
    _must(host_p.shm_addr != 0 and guest_p.shm_addr != 0, "shm_open_both_ends")

    kp_host_set_board_path(host_p, String("/tmp/demo_board.kicad_pcb"))
    kp_host_set_canvas(host_p, Float32(42.75), Float32(-18.25), Float32(0.085), 55, True, True, False, True)
    kp_host_set_size(host_p, 480, 740, True)
    _must(kp_host_attached(guest_p) and kp_board_path(guest_p) == String("/tmp/demo_board.kicad_pcb"), "host_attached_and_board_path")

    kp_host_push_event(host_p, KP_EV_FOCUS, 1, 0, 0)
    kp_host_push_event(host_p, KP_EV_MOVE, 128, 256, 0)
    kp_host_push_event(host_p, KP_EV_DOWN, 130, 260, 1)
    kp_host_push_event(host_p, KP_EV_WHEEL, 130, 260, -120)
    kp_host_push_event(host_p, KP_EV_KEY, 48, 0, 0)
    kp_host_push_event(host_p, KP_EV_KEY, 46, 0, 0)
    kp_host_push_event(host_p, KP_EV_KEY, 53, 0, 0)
    kp_host_push_event(host_p, KP_EV_KEY, 13, 0, 0)
    kp_host_push_event(host_p, KP_EV_UP, 130, 260, 1)

    var inp = kp_poll(guest_p)
    _must(inp.resized and guest_p.w == 480 and guest_p.h == 740, "resize_sync")
    _must(inp.focused and inp.mx == 130 and inp.my == 260, "mouse_focus_sync")
    _must(inp.clicked and inp.released and not inp.mouse_down, "click_release_edge")
    _must(inp.wheel_dy == -120, "wheel_delta")
    _must(len(inp.typed_keys) == 4 and inp.typed_keys[0] == 48 and inp.typed_keys[3] == 13, "typed_keys_ring")
    _must(guest_p.canvas.cursor_valid and abs(guest_p.canvas.wx_mm - Float32(42.75)) < Float32(1e-3), "canvas_wx_mm")
    _must(abs(guest_p.canvas.wy_mm - Float32(-18.25)) < Float32(1e-3) and guest_p.canvas.active_layer_enum == 55, "canvas_wy_layer")
    _must(guest_p.canvas.is_routing and (not guest_p.canvas.is_moving) and guest_p.canvas.flipped_x, "canvas_flags")

    var opts = List[String]()
    opts.append(String("JLC7628"))
    opts.append(String("JLC3313"))
    opts.append(String("OSHPark"))
    var stk_sel = 0
    var diag_on = True
    var cong_txt = String("1.0")

    kp_begin_widgets(guest_p)
    _ = kp_header(guest_p, 10, String("USB_DP F.Cu"), String("LOCKED"), kp_rgba(48, 214, 168))
    _ = kp_button(guest_p, 11, String("Open Inspector"), kp_rgba(248, 202, 44))
    _ = kp_choice(guest_p, 12, String("Fab"), opts, stk_sel, row_group=1)
    _ = kp_checkbox(guest_p, 13, String("Diagrams"), diag_on, row_group=1)
    _ = kp_textbox(guest_p, 14, String("Futr Cong"), cong_txt)
    kp_kv_row(guest_p, 15, String("LOOP 41.2 nH"), String("MRG 12 %"), kp_rgba(64, 224, 180))
    _ = kp_section(guest_p, 16, String("1. CORRIDOR MAP"), String("Zoom+"))
    kp_canvas(guest_p, 17, 0, 196)
    kp_end_widgets(guest_p)

    _must(kp_host_widget_count(host_p, False) == 8, "dock_widget_count_8")
    _must(kp_host_widget_label(host_p, 0, False) == String("USB_DP F.Cu") and kp_host_widget_value(host_p, 0, False) == String("LOCKED"), "header_slot_content")
    _must(kp_host_widget_sel(host_p, 2, False) == 0 and kp_host_widget_sel(host_p, 3, False) == 1, "choice_and_check_initial")

    var fseq_before_idem = kp_host_frame_seq(host_p)
    kp_begin_widgets(guest_p)
    _ = kp_header(guest_p, 10, String("USB_DP F.Cu"), String("LOCKED"), kp_rgba(48, 214, 168))
    _ = kp_button(guest_p, 11, String("Open Inspector"), kp_rgba(248, 202, 44))
    _ = kp_choice(guest_p, 12, String("Fab"), opts, stk_sel, row_group=1)
    _ = kp_checkbox(guest_p, 13, String("Diagrams"), diag_on, row_group=1)
    _ = kp_textbox(guest_p, 14, String("Futr Cong"), cong_txt)
    kp_kv_row(guest_p, 15, String("LOOP 41.2 nH"), String("MRG 12 %"), kp_rgba(64, 224, 180))
    _ = kp_section(guest_p, 16, String("1. CORRIDOR MAP"), String("Zoom+"))
    kp_canvas(guest_p, 17, 0, 196)
    kp_end_widgets(guest_p)
    _must(kp_host_frame_seq(host_p) == fseq_before_idem, "zero_churn_when_widgets_unchanged")

    kp_host_push_event(host_p, KP_EV_WIDGET_CLICK, 11, 0, 0)
    kp_host_push_event(host_p, KP_EV_WIDGET_SELECT, 12, 2, 0)
    kp_host_push_event(host_p, KP_EV_WIDGET_CHECK, 13, 0, 0)
    kp_host_commit_text(host_p, 14, 4, String("2.75"))
    kp_host_push_canvas_mouse(host_p, 0, 2, 144, 88, 0)
    kp_host_push_canvas_mouse(host_p, 0, 4, 144, 88, 120)

    var inp2 = kp_poll(guest_p)
    _must(inp2.canvas0.inside and inp2.canvas0.clicked and inp2.canvas0.mx == 144 and inp2.canvas0.my == 88 and inp2.canvas0.wheel_dy == 120, "canvas0_mouse_and_wheel")

    kp_begin_widgets(guest_p)
    _ = kp_header(guest_p, 10, String("USB_DP F.Cu"), String("LOCKED"), kp_rgba(48, 214, 168))
    var btn_hit = kp_button(guest_p, 11, String("Open Inspector"), kp_rgba(248, 202, 44))
    var ch_hit = kp_choice(guest_p, 12, String("Fab"), opts, stk_sel, row_group=1)
    var cb_hit = kp_checkbox(guest_p, 13, String("Diagrams"), diag_on, row_group=1)
    var tx_hit = kp_textbox(guest_p, 14, String("Futr Cong"), cong_txt)
    kp_end_widgets(guest_p)
    _must(btn_hit and ch_hit and stk_sel == 2 and cb_hit and (not diag_on) and tx_hit and cong_txt == String("2.75"), "native_widget_event_roundtrip")

    kp_begin_dialog(guest_p, String("Blossom Profile Inspector"))
    var cfm_hit = kp_button(guest_p, 201, String("Confirm & Lock"), kp_rgba(48, 214, 168), in_dialog=True)
    kp_end_dialog(guest_p)
    _must((not cfm_hit) and kp_host_dialog_open(host_p) and kp_host_widget_count(host_p, True) == 1, "native_dialog_open")
    kp_host_push_event(host_p, KP_EV_DIALOG_CLOSED, 0, 0, 0)
    var inp3 = kp_poll(guest_p)
    _must(inp3.dialog_closed and (not kp_host_dialog_open(host_p)), "native_dialog_user_close")
    kp_close_dialog(guest_p)

    var cv0 = kp_canvas_ptr(guest_p, 0)
    kp_clear_rgba(cv0, 320, 196, kp_rgba(14, 20, 30, 255))
    kp_draw_text(cv0, 320, 196, 8, 8, String("CORRIDOR"), 2, kp_rgba(72, 238, 196, 255))
    kp_present_canvas(guest_p, 0, 320, 196)

    var ov_buf = List[UInt8](length=16 * 16 * 4, fill=UInt8(0))
    ov_buf[0] = UInt8(40)
    ov_buf[1] = UInt8(220)
    ov_buf[2] = UInt8(80)
    ov_buf[3] = UInt8(180)
    var oseq0 = kp_host_overlay_seq(host_p)
    kp_set_board_overlay(guest_p, ov_buf.unsafe_ptr(), 16, 16, Int64(25000000), Int64(18000000), Float64(1.25), is_bgra=True, pcb_layer=55)
    _must(kp_host_overlay_seq(host_p) == oseq0 + UInt32(1), "gal_overlay_seq_increment")

    kp_request_width(guest_p, 640)
    kp_set_key_focus(guest_p, True)
    var fb = kp_fb_ptr(guest_p)
    fb[unsafe_offset=0] = UInt8(18)
    fb[unsafe_offset=1] = UInt8(214)
    fb[unsafe_offset=2] = UInt8(255)
    fb[unsafe_offset=3] = UInt8(255)
    var seq0 = kp_host_frame_seq(host_p)
    kp_present(guest_p)
    var seq1 = kp_host_frame_seq(host_p)
    var hfb = kp_fb_ptr(host_p)
    _must(seq1 == seq0 + UInt32(1) and hfb[unsafe_offset=1] == UInt8(214), "rgba_frame_present")

    kp_close(guest_p, False)
    kp_close(host_p, True)

    var sample_json = String("{\"$schema\":\"https://go.kicad.org/api/schemas/v1\",\"identifier\":\"dev.bee.theoretical-aided-pcb\",\"runtime\":{\"type\":\"exec\"},\"actions\":[{\"identifier\":\"blossom-routing-field\",\"scopes\":[\"pcb\"],\"entrypoint\":\"blossom_pcb\"}],\"panels\":[{\"identifier\":\"blossom-physics\",\"name\":\"Blossom Physics\",\"dock\":\"left\",\"width\":324,\"height\":760,\"entrypoint\":\"blossom_pcb\",\"args\":[\"--panel\"]}]}")
    var parsed = kp_parse_manifest_json(sample_json, String("/tmp/mock_plug"))
    _must(len(parsed) == 1 and parsed[0].plugin_id == String("dev.bee.theoretical-aided-pcb") and parsed[0].panel_id == String("blossom-physics") and parsed[0].width == 324, "manifest_json_parse")

    var tmp_plug_root = String("/tmp/kp_test_plugins")
    var tmp_plug_sub = tmp_plug_root + String("/sample_plug")
    _mkdir_p(tmp_plug_root)
    _mkdir_p(tmp_plug_sub)
    _ = _p_write(tmp_plug_sub + String("/plugin.json"), sample_json)
    var disc = kp_discover_manifests(tmp_plug_root)
    _must(len(disc) >= 1 and disc[0].name == String("Blossom Physics"), "readdir_discover_manifests")

    var tmp_kc = String("/tmp/kp_mock_kicad")
    _mkdir_p(tmp_kc)
    _mkdir_p(tmp_kc + String("/pcbnew"))
    _mkdir_p(tmp_kc + String("/pcbnew/widgets"))
    var args = argv()
    var real_kc = String(args[1]) if len(args) >= 2 else String("/Users/bee/src/kicad")
    var repo_dir = String(args[2]) if len(args) >= 3 else String("/Users/bee/Hacking/kicad-panels")
    _ = _p_write(tmp_kc + String("/pcbnew/CMakeLists.txt"), _p_read(real_kc + String("/pcbnew/CMakeLists.txt")))
    _ = _p_write(tmp_kc + String("/pcbnew/pcb_edit_frame.h"), _p_read(real_kc + String("/pcbnew/pcb_edit_frame.h")))
    _ = _p_write(tmp_kc + String("/pcbnew/pcb_edit_frame.cpp"), _p_read(real_kc + String("/pcbnew/pcb_edit_frame.cpp")))
    _ = _p_write(tmp_kc + String("/pcbnew/menubar_pcb_editor.cpp"), _p_read(real_kc + String("/pcbnew/menubar_pcb_editor.cpp")))

    var real_plug_json = _p_read(String("/Users/bee/Hacking/theoretical-aided-pcb/kicad_plugin/plugin.json"))
    if real_plug_json.byte_length() > 0:
        var real_parsed = kp_parse_manifest_json(real_plug_json, String("/Users/bee/Hacking/theoretical-aided-pcb/kicad_plugin"))
        _must(len(real_parsed) == 1 and real_parsed[0].panel_id == String("blossom-physics") and real_parsed[0].height == 760 and real_parsed[0].entrypoint == String("blossom_pcb"), "real_plugin_json_manifest")

    var n1 = apply_kicad_panels_patch(tmp_kc, repo_dir, False)
    var n2 = apply_kicad_panels_patch(tmp_kc, repo_dir, False)
    var patched_menu = _p_read(tmp_kc + String("/pcbnew/menubar_pcb_editor.cpp"))
    var patched_frame = _p_read(tmp_kc + String("/pcbnew/pcb_edit_frame.cpp"))
    var copied_host = _p_read(tmp_kc + String("/pcbnew/widgets/panel_plugin_host.cpp"))
    _must(n1 == 6 and n2 == 6 and _p_find(patched_menu, String("PANEL_PLUGIN_REGISTRY::PopulateMenu")) >= 0 and _p_find(patched_frame, String("PANEL_PLUGIN_REGISTRY::Attach")) >= 0 and copied_host.byte_length() > 1000, "idempotent_anchor_patch")

    print("KICAD_PANELS_SUITE_PASS: native_wx_widgets=OK | dialog_and_canvas=OK | gal_overlay=OK | manifest_discovery=OK | anchor_patch_idempotent=OK")
