from std.atomic import Atomic
from std.ffi import external_call
from std.math import clamp
from std.memory import Pointer
from std.sys.info import CompilationTarget

comptime KP_MAGIC = UInt32(0x4B504E4C)
comptime KP_VERSION = UInt32(1)
comptime KP_FB_OFF = 16384
comptime KP_MAX_DIM = 1024
comptime KP_SHM_BYTES = 4210688
comptime KP_RING_MASK = UInt32(255)

comptime KP_EV_MOVE = 1
comptime KP_EV_DOWN = 2
comptime KP_EV_UP = 3
comptime KP_EV_WHEEL = 4
comptime KP_EV_KEY = 5
comptime KP_EV_FOCUS = 6

@fieldwise_init
struct KpPanelManifest(Copyable, Movable):
    var plugin_id: String
    var panel_id: String
    var name: String
    var dock: String
    var width: Int
    var height: Int
    var entrypoint: String
    var base_dir: String

@fieldwise_init
struct KpCanvasState(Copyable, Movable):
    var cursor_valid: Bool
    var wx_mm: Float32
    var wy_mm: Float32
    var zoom_mpp: Float32
    var active_layer_enum: Int
    var is_routing: Bool
    var is_moving: Bool
    var flipped_x: Bool

@fieldwise_init
struct KpInputState(Copyable, Movable):
    var mx: Int
    var my: Int
    var mouse_down: Bool
    var clicked: Bool
    var released: Bool
    var wheel_dy: Int
    var focused: Bool
    var resized: Bool
    var quit_requested: Bool
    var typed_keys: List[Int]

@fieldwise_init
struct KpPanel(Copyable, Movable):
    var shm_addr: Int
    var shm_bytes: Int
    var shm_path: String
    var owns_shm: Bool
    var w: Int
    var h: Int
    var mx: Int
    var my: Int
    var mouse_down: Bool
    var focused: Bool
    var last_host_seq: UInt32
    var canvas: KpCanvasState

def kp_sanitize_id(raw: String) -> String:
    var out = String("")
    var sb = raw.as_bytes()
    for i in range(len(sb)):
        var c = Int(sb[i])
        var ok = (c >= 48 and c <= 57) or (c >= 65 and c <= 90) or (c >= 97 and c <= 122) or c == 95 or c == 45
        out += chr(c) if ok else String("_")
    return out if out.byte_length() > 0 else String("default")

def kp_shm_file_path(panel_key: String) -> String:
    if panel_key.startswith("/tmp/") or panel_key.startswith("/var/"):
        return panel_key
    return String("/tmp/kicad_panel_") + kp_sanitize_id(panel_key) + String(".shm")

def _kp_read_env(name: String) -> String:
    var nc = name + String("\0")
    var p = external_call["getenv", Int](nc.unsafe_ptr())
    _ = nc
    if p == 0:
        return String("")
    var u8 = Pointer[mut=False, UInt8, MutAnyOrigin](unsafe_from_address=p)
    var buf = List[UInt8]()
    var k = 0
    while k < 1024 and u8[unsafe_offset=k] != UInt8(0):
        buf.append(u8[unsafe_offset=k])
        k += 1
    return String(from_utf8_lossy=buf[0:len(buf)])

def kp_open(shm_spec: String = String(""), create: Bool = True, init_w: Int = 324, init_h: Int = 760) -> KpPanel:
    var spec = shm_spec if shm_spec.byte_length() > 0 else _kp_read_env(String("KICAD_PANEL_SHM"))
    if spec.byte_length() == 0:
        spec = String("dev.bee.theoretical-aided-pcb.blossom-physics")
    var path = kp_shm_file_path(spec)
    var pc = path + String("\0")
    var creat_rw = Int32(0x0042) if CompilationTarget.is_linux() else Int32(0x0202)
    var flags = creat_rw if create else Int32(0x0002)
    var fd = external_call["open", Int32](pc.unsafe_ptr(), flags)
    _ = pc
    var empty_cv = KpCanvasState(False, Float32(0.0), Float32(0.0), Float32(0.1), 0, False, False, False)
    if fd < Int32(0):
        return KpPanel(0, 0, path, False, init_w, init_h, -1, -1, False, False, UInt32(0), empty_cv^)
    if create:
        _ = external_call["fchmod", Int32](fd, UInt16(384))
        _ = external_call["ftruncate", Int32](fd, Int64(KP_SHM_BYTES))
    var addr = external_call["mmap", Int](Int(0), KP_SHM_BYTES, Int32(3), Int32(1), fd, Int64(0))
    _ = external_call["close", Int32](fd)
    if addr == -1 or addr == 0:
        return KpPanel(0, 0, path, False, init_w, init_h, -1, -1, False, False, UInt32(0), empty_cv^)
    var u32 = Pointer[mut=True, UInt32, MutAnyOrigin](unsafe_from_address=addr)
    var i32 = Pointer[mut=True, Int32, MutAnyOrigin](unsafe_from_address=addr)
    var f32 = Pointer[mut=True, Float32, MutAnyOrigin](unsafe_from_address=addr)
    var w = clamp(init_w, 64, KP_MAX_DIM)
    var h = clamp(init_h, 64, KP_MAX_DIM)
    if Atomic.load(u32.unsafe_offset(0)) != KP_MAGIC:
        _ = external_call["memset", Int](addr, Int32(0), KP_FB_OFF)
        i32[unsafe_offset=4] = Int32(w)
        i32[unsafe_offset=5] = Int32(h)
        i32[unsafe_offset=6] = Int32(w)
        i32[unsafe_offset=7] = Int32(5)
        f32[unsafe_offset=10] = Float32(0.1)
        f32[unsafe_offset=13] = Float32(1.0)
        u32[unsafe_offset=1] = KP_VERSION
        Atomic.store(u32.unsafe_offset(0), KP_MAGIC)
    else:
        w = clamp(Int(i32[unsafe_offset=4]), 64, KP_MAX_DIM)
        h = clamp(Int(Int(i32[unsafe_offset=5])), 64, KP_MAX_DIM)
        i32[unsafe_offset=7] = i32[unsafe_offset=7] | Int32(4)
    return KpPanel(addr, KP_SHM_BYTES, path, create, w, h, -1, -1, False, False, Atomic.load(u32.unsafe_offset(2)), empty_cv^)

def kp_close(mut p: KpPanel, unlink_file: Bool = False):
    if p.shm_addr != 0 and p.shm_bytes > 0:
        var i32 = Pointer[mut=True, Int32, MutAnyOrigin](unsafe_from_address=p.shm_addr)
        i32[unsafe_offset=7] = i32[unsafe_offset=7] & (~Int32(4))
        _ = external_call["munmap", Int32](p.shm_addr, p.shm_bytes)
        p.shm_addr = 0
    if unlink_file and p.shm_path.byte_length() > 0:
        var pc = p.shm_path + String("\0")
        _ = external_call["unlink", Int32](pc.unsafe_ptr())
        _ = pc

def kp_fb_ptr(p: KpPanel) -> Pointer[mut=True, UInt8, MutAnyOrigin]:
    return Pointer[mut=True, UInt8, MutAnyOrigin](unsafe_from_address=p.shm_addr + KP_FB_OFF)

def kp_request_width(p: KpPanel, req_w: Int):
    if p.shm_addr == 0:
        return
    var i32 = Pointer[mut=True, Int32, MutAnyOrigin](unsafe_from_address=p.shm_addr)
    i32[unsafe_offset=6] = Int32(clamp(req_w, 64, KP_MAX_DIM))

def kp_set_key_focus(p: KpPanel, want_key: Bool):
    if p.shm_addr == 0:
        return
    var i32 = Pointer[mut=True, Int32, MutAnyOrigin](unsafe_from_address=p.shm_addr)
    if want_key:
        i32[unsafe_offset=7] = i32[unsafe_offset=7] | Int32(8)
    else:
        i32[unsafe_offset=7] = i32[unsafe_offset=7] & (~Int32(8))

def kp_present(mut p: KpPanel):
    if p.shm_addr == 0:
        return
    var u32 = Pointer[mut=True, UInt32, MutAnyOrigin](unsafe_from_address=p.shm_addr)
    Atomic.store(u32.unsafe_offset(3), Atomic.load(u32.unsafe_offset(3)) + UInt32(1))

def kp_poll(mut p: KpPanel) -> KpInputState:
    var keys = List[Int]()
    if p.shm_addr == 0:
        return KpInputState(p.mx, p.my, p.mouse_down, False, False, 0, p.focused, False, True, keys^)
    var u32 = Pointer[mut=True, UInt32, MutAnyOrigin](unsafe_from_address=p.shm_addr)
    var i32 = Pointer[mut=False, Int32, MutAnyOrigin](unsafe_from_address=p.shm_addr)
    var f32 = Pointer[mut=False, Float32, MutAnyOrigin](unsafe_from_address=p.shm_addr)
    var hseq = Atomic.load(u32.unsafe_offset(2))
    var nw = clamp(Int(i32[unsafe_offset=4]), 64, KP_MAX_DIM)
    var nh = clamp(Int(i32[unsafe_offset=5]), 64, KP_MAX_DIM)
    var resized = (nw != p.w) or (nh != p.h)
    p.w = nw
    p.h = nh
    p.last_host_seq = hseq
    var pflags = Int(i32[unsafe_offset=7])
    var quit_req = (pflags & 16) != 0
    var cflags = u32[unsafe_offset=12]
    p.canvas = KpCanvasState(
        (cflags & UInt32(1)) != UInt32(0),
        f32[unsafe_offset=8],
        f32[unsafe_offset=9],
        max(Float32(1e-4), f32[unsafe_offset=10]),
        Int(i32[unsafe_offset=11]),
        (cflags & UInt32(2)) != UInt32(0),
        (cflags & UInt32(4)) != UInt32(0),
        (cflags & UInt32(8)) != UInt32(0),
    )
    var clicked = False
    var released = False
    var wheel_dy = 0
    var w_idx = Atomic.load(u32.unsafe_offset(16))
    var r_idx = Atomic.load(u32.unsafe_offset(17))
    var guard = 0
    while r_idx != w_idx and guard < 256:
        var base = 64 + Int(r_idx & KP_RING_MASK) * 4
        var kind = Int(i32[unsafe_offset=base + 0])
        var vx = Int(i32[unsafe_offset=base + 1])
        var vy = Int(i32[unsafe_offset=base + 2])
        var aux = Int(i32[unsafe_offset=base + 3])
        if kind == KP_EV_MOVE:
            p.mx = vx
            p.my = vy
        elif kind == KP_EV_DOWN:
            p.mx = vx
            p.my = vy
            if aux <= 1:
                if not p.mouse_down:
                    clicked = True
                p.mouse_down = True
        elif kind == KP_EV_UP:
            p.mx = vx
            p.my = vy
            if aux <= 1:
                if p.mouse_down:
                    released = True
                p.mouse_down = False
        elif kind == KP_EV_WHEEL:
            wheel_dy += aux
        elif kind == KP_EV_KEY:
            if vx > 0:
                keys.append(vx)
        elif kind == KP_EV_FOCUS:
            p.focused = vx != 0
        r_idx += UInt32(1)
        guard += 1
    Atomic.store(u32.unsafe_offset(17), r_idx)
    return KpInputState(p.mx, p.my, p.mouse_down, clicked, released, wheel_dy, p.focused, resized, quit_req, keys^)

def kp_host_set_size(p: KpPanel, w: Int, h: Int, visible: Bool = True):
    if p.shm_addr == 0:
        return
    var u32 = Pointer[mut=True, UInt32, MutAnyOrigin](unsafe_from_address=p.shm_addr)
    var i32 = Pointer[mut=True, Int32, MutAnyOrigin](unsafe_from_address=p.shm_addr)
    i32[unsafe_offset=4] = Int32(clamp(w, 64, KP_MAX_DIM))
    i32[unsafe_offset=5] = Int32(clamp(h, 64, KP_MAX_DIM))
    var fl = (i32[unsafe_offset=7] | Int32(2))
    if visible:
        fl |= Int32(1)
    else:
        fl &= ~Int32(1)
    i32[unsafe_offset=7] = fl
    Atomic.store(u32.unsafe_offset(2), Atomic.load(u32.unsafe_offset(2)) + UInt32(1))

def kp_host_set_canvas(
    p: KpPanel, wx_mm: Float32, wy_mm: Float32, zoom_mpp: Float32,
    layer_enum: Int, in_canvas: Bool, is_routing: Bool, is_moving: Bool, flipped_x: Bool,
):
    if p.shm_addr == 0:
        return
    var u32 = Pointer[mut=True, UInt32, MutAnyOrigin](unsafe_from_address=p.shm_addr)
    var i32 = Pointer[mut=True, Int32, MutAnyOrigin](unsafe_from_address=p.shm_addr)
    var f32 = Pointer[mut=True, Float32, MutAnyOrigin](unsafe_from_address=p.shm_addr)
    f32[unsafe_offset=8] = wx_mm
    f32[unsafe_offset=9] = wy_mm
    f32[unsafe_offset=10] = zoom_mpp
    i32[unsafe_offset=11] = Int32(layer_enum)
    var cf = (UInt32(1) if in_canvas else UInt32(0)) | (UInt32(2) if is_routing else UInt32(0)) | (UInt32(4) if is_moving else UInt32(0)) | (UInt32(8) if flipped_x else UInt32(0))
    u32[unsafe_offset=12] = cf
    Atomic.store(u32.unsafe_offset(2), Atomic.load(u32.unsafe_offset(2)) + UInt32(1))

def kp_host_push_event(p: KpPanel, kind: Int, x: Int, y: Int, aux: Int = 0):
    if p.shm_addr == 0:
        return
    var u32 = Pointer[mut=True, UInt32, MutAnyOrigin](unsafe_from_address=p.shm_addr)
    var i32 = Pointer[mut=True, Int32, MutAnyOrigin](unsafe_from_address=p.shm_addr)
    var w_idx = Atomic.load(u32.unsafe_offset(16))
    var base = 64 + Int(w_idx & KP_RING_MASK) * 4
    i32[unsafe_offset=base + 0] = Int32(kind)
    i32[unsafe_offset=base + 1] = Int32(x)
    i32[unsafe_offset=base + 2] = Int32(y)
    i32[unsafe_offset=base + 3] = Int32(aux)
    Atomic.store(u32.unsafe_offset(16), w_idx + UInt32(1))

def kp_host_frame_seq(p: KpPanel) -> UInt32:
    if p.shm_addr == 0:
        return UInt32(0)
    var u32 = Pointer[mut=True, UInt32, MutAnyOrigin](unsafe_from_address=p.shm_addr)
    return Atomic.load(u32.unsafe_offset(3))

def _kp_read_file_str(path: String) -> String:
    var pc = path + String("\0")
    var fd = external_call["open", Int32](pc.unsafe_ptr(), Int32(0))
    _ = pc
    if fd < Int32(0):
        return String("")
    var out = List[UInt8]()
    var chunk = List[UInt8](length=65536, fill=UInt8(0))
    var cur_off = Int64(0)
    while True:
        var n = external_call["pread", Int](fd, chunk.unsafe_ptr(), 65536, cur_off)
        if n <= 0:
            break
        for i in range(n):
            out.append(chunk[i])
        cur_off += Int64(n)
    _ = external_call["close", Int32](fd)
    return String(from_utf8_lossy=out[0:len(out)])

def _kp_find_sub(s: String, pat: String, start: Int = 0) -> Int:
    var sb = s.as_bytes()
    var pb = pat.as_bytes()
    var sl = len(sb)
    var pl = len(pb)
    if pl == 0 or start < 0 or start + pl > sl:
        return -1
    for i in range(start, sl - pl + 1):
        var ok = True
        for j in range(pl):
            if sb[i + j] != pb[j]:
                ok = False
                break
        if ok:
            return i
    return -1

def _kp_json_str_after(txt: String, key: String, lo: Int, hi: Int) -> String:
    var kpat = String("\"") + key + String("\"")
    var kp = _kp_find_sub(txt, kpat, lo)
    if kp < 0 or kp >= hi:
        return String("")
    var sb = txt.as_bytes()
    var c = kp + kpat.byte_length()
    while c < hi and sb[c] != UInt8(58):
        c += 1
    c += 1
    while c < hi and (sb[c] == UInt8(32) or sb[c] == UInt8(9) or sb[c] == UInt8(10) or sb[c] == UInt8(13)):
        c += 1
    if c >= hi or sb[c] != UInt8(34):
        return String("")
    c += 1
    var st = c
    while c < hi and sb[c] != UInt8(34):
        if sb[c] == UInt8(92):
            c += 1
        c += 1
    return String(from_utf8_lossy=sb[st:min(c, hi)])

def _kp_json_int_after(txt: String, key: String, lo: Int, hi: Int, fallback: Int) -> Int:
    var kpat = String("\"") + key + String("\"")
    var kp = _kp_find_sub(txt, kpat, lo)
    if kp < 0 or kp >= hi:
        return fallback
    var sb = txt.as_bytes()
    var c = kp + kpat.byte_length()
    while c < hi and sb[c] != UInt8(58):
        c += 1
    c += 1
    while c < hi and (sb[c] == UInt8(32) or sb[c] == UInt8(9) or sb[c] == UInt8(10) or sb[c] == UInt8(13)):
        c += 1
    var v = 0
    var has_d = False
    while c < hi and sb[c] >= UInt8(48) and sb[c] <= UInt8(57):
        v = v * 10 + (Int(sb[c]) - 48)
        has_d = True
        c += 1
    return v if has_d else fallback

def kp_parse_manifest_json(json_txt: String, base_dir: String) -> List[KpPanelManifest]:
    var out = List[KpPanelManifest]()
    var n = json_txt.byte_length()
    if n == 0:
        return out^
    var plug_id = _kp_json_str_after(json_txt, String("identifier"), 0, n)
    var p_pos = _kp_find_sub(json_txt, String("\"panels\""), 0)
    if p_pos < 0:
        return out^
    var sb = json_txt.as_bytes()
    var i = p_pos + 8
    while i < n and sb[i] != UInt8(91):
        i += 1
    if i >= n:
        return out^
    var arr_depth = 1
    i += 1
    while i < n and arr_depth > 0:
        if sb[i] == UInt8(91):
            arr_depth += 1
            i += 1
        elif sb[i] == UInt8(93):
            arr_depth -= 1
            i += 1
        elif sb[i] == UInt8(123):
            var obj_st = i
            var odepth = 1
            i += 1
            while i < n and odepth > 0:
                if sb[i] == UInt8(34):
                    i += 1
                    while i < n and sb[i] != UInt8(34):
                        if sb[i] == UInt8(92):
                            i += 1
                        i += 1
                elif sb[i] == UInt8(123):
                    odepth += 1
                elif sb[i] == UInt8(125):
                    odepth -= 1
                i += 1
            var obj_end = i
            var pid = _kp_json_str_after(json_txt, String("identifier"), obj_st, obj_end)
            var pnm = _kp_json_str_after(json_txt, String("name"), obj_st, obj_end)
            var pdk = _kp_json_str_after(json_txt, String("dock"), obj_st, obj_end)
            var pep = _kp_json_str_after(json_txt, String("entrypoint"), obj_st, obj_end)
            var pw = _kp_json_int_after(json_txt, String("width"), obj_st, obj_end, 324)
            var ph = _kp_json_int_after(json_txt, String("height"), obj_st, obj_end, 720)
            if pid.byte_length() > 0 and pep.byte_length() > 0:
                out.append(KpPanelManifest(plug_id, pid, pnm if pnm.byte_length() > 0 else pid, pdk if pdk.byte_length() > 0 else String("left"), pw, ph, pep, base_dir))
        else:
            i += 1
    return out^

def kp_discover_manifests(plugins_root: String) -> List[KpPanelManifest]:
    var out = List[KpPanelManifest]()
    var rc = plugins_root + String("\0")
    var dirp = external_call["opendir", Int](rc.unsafe_ptr())
    _ = rc
    if dirp == 0:
        return out^
    while True:
        var ent = external_call["readdir", Int](dirp)
        if ent == 0:
            break
        var name_off = 19 if CompilationTarget.is_linux() else 21
        var np = Pointer[mut=False, UInt8, MutAnyOrigin](unsafe_from_address=ent + name_off)
        var nbuf = List[UInt8]()
        var k = 0
        while k < 255 and np[unsafe_offset=k] != UInt8(0):
            nbuf.append(np[unsafe_offset=k])
            k += 1
        var nm = String(from_utf8_lossy=nbuf[0:len(nbuf)])
        if nm.byte_length() == 0 or nm == String(".") or nm == String(".."):
            continue
        var sub = plugins_root + String("/") + nm
        var pj = _kp_read_file_str(sub + String("/plugin.json"))
        if pj.byte_length() > 0:
            var found = kp_parse_manifest_json(pj, sub)
            for m in range(len(found)):
                out.append(found[m].copy())
    _ = external_call["closedir", Int32](dirp)
    return out^
