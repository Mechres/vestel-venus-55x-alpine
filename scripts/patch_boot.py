#!/usr/bin/env python3
"""
patch_boot.py — enable headless ADB on the Vestel Venus 5.5x stock boot.img.

Adapted from an earlier boot-image patching script, with one fix: that
script's cpio "newc" reader computes the post-filename padding from
namesize alone, ignoring the 110-byte fixed header — since 110 % 4 == 2,
every entry after the first was parsed at the wrong offset and the loop
bailed out (only ever recovering a single file). This version pads from
the header+filename length together, per the actual newc spec. Verified
against the real Vestel ramdisk (cpio -tv on the decompressed archive) to
recover the identical 47-entry file set before trusting it to repack.

Usage:
    patch_boot.py unpack <boot.img> <out_dir>
    patch_boot.py repack <in_dir>   <out_boot.img>
    patch_boot.py patch  <boot.img> <out_boot.img>
    patch_boot.py replace-ramdisk <boot.img> <new_root_dir> <out_boot.img>
        Keeps the stock kernel/second/dt/header, replaces the ramdisk
        wholesale with a fresh tree built from <new_root_dir> (mode/type
        taken from disk, not a round-tripped manifest) — for a from-scratch
        ramdisk-pivot init rather than patching the stock Android ramdisk.
"""
import os
import sys
import stat
import struct
import gzip
import tempfile
import shutil
import json

def detect_and_decompress(data: bytes):
    if data[:2] == b"\x1f\x8b":
        return "gzip", gzip.decompress(data)
    return "none", data

def compress_back(fmt: str, data: bytes) -> bytes:
    if fmt == "none":
        return data
    return gzip.compress(data)

def parse_boot_header(data: bytes):
    magic = data[:8]
    if magic != b"ANDROID!":
        raise ValueError("not an Android boot image (bad magic)")
    (kernel_size, kernel_addr, ramdisk_size, ramdisk_addr,
     second_size, second_addr, tags_addr, pagesize,
     dt_size) = struct.unpack_from("<IIIIIIIII", data, 8)
    return {
        "kernel_size": kernel_size, "ramdisk_size": ramdisk_size,
        "second_size": second_size, "dt_size": dt_size,
        "pagesize": pagesize,
    }

def round_up(x, p):
    return (x + p - 1) // p * p

def extract_sections(data: bytes, hdr):
    p = hdr["pagesize"]
    off = p
    kernel = data[off:off + hdr["kernel_size"]]; off += round_up(hdr["kernel_size"], p)
    ramdisk = data[off:off + hdr["ramdisk_size"]]; off += round_up(hdr["ramdisk_size"], p)
    second = data[off:off + hdr["second_size"]]; off += round_up(hdr["second_size"], p)
    dt = data[off:off + hdr["dt_size"]]
    return kernel, ramdisk, second, dt

def repack_sections(header_bytes: bytes, hdr, kernel: bytes, ramdisk: bytes, second: bytes, dt: bytes):
    p = hdr["pagesize"]
    header = bytearray(header_bytes)
    struct.pack_into("<I", header, 8, len(kernel))
    struct.pack_into("<I", header, 16, len(ramdisk))
    struct.pack_into("<I", header, 24, len(second))
    struct.pack_into("<I", header, 40, len(dt))
    out = bytearray(header)
    out += kernel
    out += b"\x00" * (round_up(len(kernel), p) - len(kernel))
    out += ramdisk
    out += b"\x00" * (round_up(len(ramdisk), p) - len(ramdisk))
    out += second
    out += b"\x00" * (round_up(len(second), p) - len(second))
    out += dt
    out += b"\x00" * (round_up(len(dt), p) - len(dt))
    return bytes(out)

# ---- cpio "newc" reader/writer (fixed alignment) --------------------------
HDRLEN = 110  # 6-byte magic + 13 * 8 hex-digit fields

def cpio_read(data: bytes):
    entries = []
    off = 0
    n = len(data)
    if data[:6] != b"070701":
        raise ValueError("unsupported cpio magic %r" % data[:6])
    while off + HDRLEN <= n:
        hdr = data[off:off + HDRLEN]
        if hdr[:6] != b"070701":
            break
        vals = [int(hdr[6 + i * 8:6 + (i + 1) * 8], 16) for i in range(13)]
        (ino, mode, uid, gid, nlink, mtime, filesize,
         devmajor, devminor, rdevmajor, rdevminor, namesize, _check) = vals
        name_off = off + HDRLEN
        name = data[name_off:name_off + namesize - 1].decode("utf-8", "replace")
        # padding after header+name is relative to the WHOLE entry so far
        pre_pad_len = HDRLEN + namesize
        pad = (4 - (pre_pad_len % 4)) % 4
        data_off = name_off + namesize + pad
        content = data[data_off:data_off + filesize]
        post = data_off + filesize
        pad2 = (4 - (filesize % 4)) % 4
        off = post + pad2
        if name == "TRAILER!!!":
            break
        entries.append({"name": name, "mode": mode, "uid": uid, "gid": gid,
                         "mtime": mtime, "rdevmajor": rdevmajor, "rdevminor": rdevminor,
                         "data": content})
    return entries

def cpio_write_newc(entries) -> bytes:
    out = bytearray()
    ino = 1
    for e in entries:
        name_b = e["name"].encode("utf-8") + b"\x00"
        body = e["data"]
        mode = e.get("mode", 0o100644)
        uid = e.get("uid", 0)
        gid = e.get("gid", 0)
        mtime = e.get("mtime", 0)
        rdevmajor = e.get("rdevmajor", 0)
        rdevminor = e.get("rdevminor", 0)
        hdr = b"070701" + b"".join(
            b"%08X" % v for v in (ino, mode, uid, gid, 1, mtime, len(body),
                                 0, 0, rdevmajor, rdevminor, len(name_b), 0))
        out += hdr
        out += name_b
        pad = (4 - ((HDRLEN + len(name_b)) % 4)) % 4
        out += b"\x00" * pad
        out += body
        out += b"\x00" * ((4 - (len(body) % 4)) % 4)
        ino += 1
    tname = b"TRAILER!!!\x00"
    hdr = b"070701" + b"".join(
        b"%08X" % v for v in (ino, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, len(tname), 0))
    out += hdr
    out += tname
    pad = (4 - ((HDRLEN + len(tname)) % 4)) % 4
    out += b"\x00" * pad
    # cpio archives are traditionally padded to a 512-byte boundary
    out += b"\x00" * ((512 - (len(out) % 512)) % 512)
    return bytes(out)

def cpio_extract(ramdisk_raw: bytes, out_dir: str):
    entries = cpio_read(ramdisk_raw)
    manifest = []
    for e in entries:
        name = e["name"].lstrip("./")
        if not name:
            continue
        is_dir = (e["mode"] & 0o170000) == 0o040000
        is_symlink = (e["mode"] & 0o170000) == 0o120000
        dest = os.path.join(out_dir, "root", name)
        if is_dir:
            os.makedirs(dest, exist_ok=True)
        elif is_symlink:
            os.makedirs(os.path.dirname(dest), exist_ok=True)
            target = e["data"].decode("utf-8", "replace")
            if os.path.lexists(dest):
                os.remove(dest)
            os.symlink(target, dest)
        else:
            os.makedirs(os.path.dirname(dest) or out_dir, exist_ok=True)
            with open(dest, "wb") as f:
                f.write(e["data"])
            try:
                os.chmod(dest, e["mode"] & 0o7777)
            except OSError:
                pass
        manifest.append({"name": name, "mode": e["mode"], "uid": e["uid"],
                          "gid": e["gid"], "mtime": e["mtime"],
                          "rdevmajor": e["rdevmajor"], "rdevminor": e["rdevminor"]})
    with open(os.path.join(out_dir, ".manifest.json"), "w") as f:
        json.dump(manifest, f, indent=1)
    return len(entries)

def cpio_collect(dir_path: str) -> bytes:
    with open(os.path.join(dir_path, ".manifest.json")) as f:
        manifest = json.load(f)
    root = os.path.join(dir_path, "root")
    entries = []
    for m in manifest:
        full = os.path.join(root, m["name"])
        is_dir = (m["mode"] & 0o170000) == 0o040000
        is_symlink = (m["mode"] & 0o170000) == 0o120000
        if is_dir:
            data = b""
        elif is_symlink:
            data = os.readlink(full).encode("utf-8")
        else:
            with open(full, "rb") as f:
                data = f.read()
        entries.append({"name": m["name"], "mode": m["mode"], "uid": m["uid"],
                         "gid": m["gid"], "mtime": m["mtime"],
                         "rdevmajor": m.get("rdevmajor", 0), "rdevminor": m.get("rdevminor", 0),
                         "data": data})
    return cpio_write_newc(entries)

# ---- default.prop patching ----------------------------------------------
TARGET_PROPS = {
    "ro.adb.secure": "0",
    "ro.debuggable": "1",
    "persist.sys.usb.config": "mtp,adb",
    "persist.service.adb.enable": "1",
}

def patch_default_prop(root_dir: str):
    dp = os.path.join(root_dir, "default.prop")
    lines = []
    existing = {}
    if os.path.exists(dp):
        with open(dp, "r") as f:
            for line in f:
                line = line.rstrip("\n")
                if "=" in line:
                    k, v = line.split("=", 1)
                    existing[k.strip()] = v.strip()
    existing.update(TARGET_PROPS)
    with open(dp, "w") as f:
        for k, v in existing.items():
            f.write(f"{k}={v}\n")
    return existing

# Stock persist.sys.usb.config / persist.service.adb.enable on /data survive
# from the factory boot and override our default.prop at property-load time
# (persist.* wins over default.prop for keys already in /data/property).
# Force the gadget into adb mode and start adbd unconditionally on "boot",
# which runs after that property-trigger chain, so this write wins last.
FORCE_ADB_RC = """
on boot
    write /sys/class/android_usb/android0/enable 0
    write /sys/class/android_usb/android0/idVendor 18d1
    write /sys/class/android_usb/android0/idProduct D002
    write /sys/class/android_usb/android0/functions adb
    write /sys/class/android_usb/android0/enable 1
    start adbd
"""

def force_adb_gadget(root_dir: str):
    rc = os.path.join(root_dir, "init.rc")
    with open(rc, "a") as f:
        f.write(FORCE_ADB_RC)

# ---- commands --------------------------------------------------------------
def cmd_unpack(boot_img, out_dir):
    with open(boot_img, "rb") as f:
        data = f.read()
    hdr = parse_boot_header(data)
    kernel, ramdisk, second, dt = extract_sections(data, hdr)
    comp_fmt, ramdisk_raw = detect_and_decompress(ramdisk)
    os.makedirs(out_dir, exist_ok=True)
    n = cpio_extract(ramdisk_raw, out_dir)
    meta = {"pagesize": hdr["pagesize"], "ramdisk_compression": comp_fmt}
    with open(os.path.join(out_dir, ".bootmeta.json"), "w") as f:
        json.dump(meta, f)
    with open(os.path.join(out_dir, ".kernel"), "wb") as f:
        f.write(kernel)
    with open(os.path.join(out_dir, ".second"), "wb") as f:
        f.write(second)
    with open(os.path.join(out_dir, ".dt"), "wb") as f:
        f.write(dt)
    with open(os.path.join(out_dir, ".header"), "wb") as f:
        f.write(data[:hdr["pagesize"]])
    print(f"unpacked {n} cpio entries, compression={comp_fmt}, pagesize={hdr['pagesize']}")

def cmd_repack(in_dir, out_img):
    with open(os.path.join(in_dir, ".bootmeta.json")) as f:
        meta = json.load(f)
    with open(os.path.join(in_dir, ".kernel"), "rb") as f:
        kernel = f.read()
    with open(os.path.join(in_dir, ".second"), "rb") as f:
        second = f.read()
    with open(os.path.join(in_dir, ".dt"), "rb") as f:
        dt = f.read()
    with open(os.path.join(in_dir, ".header"), "rb") as f:
        header_bytes = f.read()
    hdr = {"pagesize": meta["pagesize"]}
    ramdisk_raw = cpio_collect(in_dir)
    ramdisk = compress_back(meta["ramdisk_compression"], ramdisk_raw)
    out = repack_sections(header_bytes, hdr, kernel, ramdisk, second, dt)
    with open(out_img, "wb") as f:
        f.write(out)
    print(f"repacked -> {out_img} ({len(out)} bytes, ramdisk raw={len(ramdisk_raw)} compressed={len(ramdisk)})")

def build_cpio_from_tree(root_dir: str) -> bytes:
    """Build a fresh newc cpio from an arbitrary directory tree (not a
    round-tripped stock ramdisk) — used for Phase 1's from-scratch
    ramdisk-pivot init, so mode/type is taken straight from disk rather
    than a saved manifest."""
    entries = []
    for dirpath, dirnames, filenames in os.walk(root_dir):
        dirnames.sort()
        filenames.sort()
        rel_dir = os.path.relpath(dirpath, root_dir)
        if rel_dir != ".":
            st = os.lstat(dirpath)
            entries.append({"name": rel_dir, "mode": stat.S_IFDIR | (st.st_mode & 0o7777),
                             "uid": 0, "gid": 0, "mtime": 0,
                             "rdevmajor": 0, "rdevminor": 0, "data": b""})
        for fn in filenames:
            full = os.path.join(dirpath, fn)
            rel = os.path.relpath(full, root_dir)
            st = os.lstat(full)
            if stat.S_ISLNK(st.st_mode):
                target = os.readlink(full)
                entries.append({"name": rel, "mode": stat.S_IFLNK | 0o777,
                                 "uid": 0, "gid": 0, "mtime": 0,
                                 "rdevmajor": 0, "rdevminor": 0,
                                 "data": target.encode("utf-8")})
            else:
                with open(full, "rb") as f:
                    data = f.read()
                entries.append({"name": rel, "mode": stat.S_IFREG | (st.st_mode & 0o7777),
                                 "uid": 0, "gid": 0, "mtime": 0,
                                 "rdevmajor": 0, "rdevminor": 0, "data": data})
    return cpio_write_newc(entries)

def cmd_replace_ramdisk(boot_img, new_root_dir, out_img):
    """Replace boot.img's entire ramdisk with a fresh tree (Phase 1
    ramdisk-pivot init), keeping the stock kernel/second/dt/header — the
    part already proven to boot on this board."""
    tmp = tempfile.mkdtemp(prefix="vestel_replace_")
    try:
        cmd_unpack(boot_img, tmp)
        with open(os.path.join(tmp, ".bootmeta.json")) as f:
            meta = json.load(f)
        with open(os.path.join(tmp, ".kernel"), "rb") as f:
            kernel = f.read()
        with open(os.path.join(tmp, ".second"), "rb") as f:
            second = f.read()
        with open(os.path.join(tmp, ".dt"), "rb") as f:
            dt = f.read()
        with open(os.path.join(tmp, ".header"), "rb") as f:
            header_bytes = f.read()
        hdr = {"pagesize": meta["pagesize"]}
        ramdisk_raw = build_cpio_from_tree(new_root_dir)
        ramdisk = compress_back(meta["ramdisk_compression"], ramdisk_raw)
        out = repack_sections(header_bytes, hdr, kernel, ramdisk, second, dt)
        with open(out_img, "wb") as f:
            f.write(out)
        print(f"replaced ramdisk -> {out_img} ({len(out)} bytes, "
              f"new ramdisk raw={len(ramdisk_raw)} compressed={len(ramdisk)})")
    finally:
        shutil.rmtree(tmp, ignore_errors=True)

def cmd_patch(boot_img, out_img):
    tmp = tempfile.mkdtemp(prefix="vestel_patchboot_")
    try:
        cmd_unpack(boot_img, tmp)
        props = patch_default_prop(os.path.join(tmp, "root"))
        force_adb_gadget(os.path.join(tmp, "root"))
        cmd_repack(tmp, out_img)
        print("patched default.prop with:", props)
        print("appended forced-adb 'on boot' block to init.rc")
    finally:
        shutil.rmtree(tmp, ignore_errors=True)

def main():
    if len(sys.argv) < 2:
        print(__doc__)
        sys.exit(1)
    cmd = sys.argv[1]
    if cmd == "unpack" and len(sys.argv) == 4:
        cmd_unpack(sys.argv[2], sys.argv[3])
    elif cmd == "repack" and len(sys.argv) == 4:
        cmd_repack(sys.argv[2], sys.argv[3])
    elif cmd == "patch" and len(sys.argv) == 4:
        cmd_patch(sys.argv[2], sys.argv[3])
    elif cmd == "replace-ramdisk" and len(sys.argv) == 5:
        cmd_replace_ramdisk(sys.argv[2], sys.argv[3], sys.argv[4])
    else:
        print(__doc__)
        sys.exit(1)

if __name__ == "__main__":
    main()
