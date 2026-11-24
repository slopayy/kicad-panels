from std.ffi import external_call
from std.math import abs
from std.sys import argv
from kicad_panels import (
    KP_EV_DOWN, KP_EV_FOCUS, KP_EV_KEY, KP_EV_MOVE, KP_EV_UP, KP_EV_WHEEL,
    kp_close, kp_discover_manifests, kp_fb_ptr, kp_host_frame_seq,
    kp_host_push_event, kp_host_set_canvas, kp_host_set_size, kp_open,
    kp_parse_manifest_json, kp_poll, kp_present, kp_request_width, kp_set_key_focus,
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
    var shm_key = String("unit_test_panel_abi")
    var host_p = kp_open(shm_key, True, 324, 720)
    var guest_p = kp_open(shm_key, False, 324, 720)
    _must(host_p.shm_addr != 0 and guest_p.shm_addr != 0, "shm_open_both_ends")

    kp_host_set_canvas(host_p, Float32(42.75), Float32(-18.25), Float32(0.085), 34, True, True, False, True)
    kp_host_set_size(host_p, 480, 740, True)
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
    _must(abs(guest_p.canvas.wy_mm - Float32(-18.25)) < Float32(1e-3) and guest_p.canvas.active_layer_enum == 34, "canvas_wy_layer")
    _must(guest_p.canvas.is_routing and (not guest_p.canvas.is_moving) and guest_p.canvas.flipped_x, "canvas_flags")

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

    print("KICAD_PANELS_SUITE_PASS: shm_abi=OK | manifest_discovery=OK | real_plugin_json=OK | anchor_patch_idempotent=OK")
