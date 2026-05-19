#!/usr/bin/env python3
"""Build vita_texcache.vtc for PS Vita from a Dolphin-compatible HD texture pack.

Decodes all DDS files (BC7/BC3/BC1/RGBA) -> RGBA -> DXT1/DXT5 -> VTC cache.
Opaque textures use DXT1 (4 bpp), alpha textures use DXT5 (8 bpp).

V3 (default): pre-swizzled GXM Morton layout. Vita memcpys directly to vram.
V2 (--v2): linear DXT. Vita swizzles on a worker thread.

Usage:
    python build_vita_texcache.py [--v2] <texture_pack_dir> [output_file]

Output should be copied to: ux0:data/AnimalCrossing/texture_packs/<name>.vtc
"""

import struct
import os
import sys
import glob
import zipfile
import time

try:
    import texture2ddecoder
except ImportError:
    print("ERROR: pip install texture2ddecoder"); sys.exit(1)
try:
    import etcpak
except ImportError:
    print("ERROR: pip install etcpak"); sys.exit(1)

# --- VTC format constants ---
VTC_MAGIC      = 0x56544331  # "VTC1"
VTC_VERSION_V2 = 2  # linear DXT, swizzled on Vita
VTC_VERSION_V3 = 3  # pre-swizzled DXT, direct memcpy on Vita
FMT_DXT1       = 1
FMT_DXT5       = 5


# --- Morton (Z-order) swizzle for V3 ---
# Mirrors SwizzleTexData1x1 in vitaGL fork (source/utils/texture_swizzler.cpp).
# Convention: morton_pos(x, y) = (Part1By1(x) << 1) + Part1By1(y), y at LSB.

def _part1by1(x):
    x &= 0xFFFFFFFF
    x &= 0x0000FFFF
    x = (x ^ (x << 8)) & 0x00FF00FF
    x = (x ^ (x << 4)) & 0x0F0F0F0F
    x = (x ^ (x << 2)) & 0x33333333
    x = (x ^ (x << 1)) & 0x55555555
    return x & 0xFFFFFFFF

def _nearest_po2(v):
    if v <= 1:
        return 1
    return 1 << (v - 1).bit_length()

def swizzle_dxt_blocks(linear_blocks, bw_lin, bh_lin, bw_pot, bh_pot, block_bytes):
    """Swizzle linear DXT blocks to GXM Morton order, sized to POT.

    POT-padding blocks stay zero; the GPU never samples them since the
    descriptor uses the logical dimensions.
    """
    tile_size = min(bw_pot, bh_pot)
    x_mask = (0xAAAAAAAA | (~((tile_size * tile_size) - 1) & 0xFFFFFFFF)) & 0xFFFFFFFF
    y_mask = (0x55555555 | (~((tile_size * tile_size) - 1) & 0xFFFFFFFF)) & 0xFFFFFFFF
    x_origin = (_part1by1(0) << 1) & 0xFFFFFFFF
    y_origin = _part1by1(0)

    out = bytearray(bw_pot * bh_pot * block_bytes)
    x_tw = x_origin
    y_tw = y_origin
    for y_pos in range(bh_lin):
        for x_pos in range(bw_lin):
            src = (y_pos * bw_lin + x_pos) * block_bytes
            dst = (x_tw + y_tw) * block_bytes
            out[dst:dst + block_bytes] = linear_blocks[src:src + block_bytes]
            x_tw = (x_tw - x_mask) & x_mask
        x_tw = x_origin
        y_tw = (y_tw - y_mask) & y_mask
    return bytes(out)


def pot_swizzled_size(orig_w, orig_h, block_bytes):
    """POT-rounded swizzled byte size for an orig_w x orig_h texture."""
    pot_w = _nearest_po2(orig_w)
    pot_h = _nearest_po2(orig_h)
    bw_pot = (pot_w + 3) // 4
    bh_pot = (pot_h + 3) // 4
    return bw_pot * bh_pot * block_bytes

# --- Cache key (must match loaded_cache_key in pc_texture_pack.c) ---
def loaded_cache_key(data_hash, tlut_hash, fmt, w, h):
    k = data_hash & 0xFFFFFFFFFFFFFFFF
    k ^= (tlut_hash * 0x517CC1B727220A95) & 0xFFFFFFFFFFFFFFFF
    k ^= (fmt * 0x6C62272E07BB0142) & 0xFFFFFFFFFFFFFFFF
    k ^= (w * 0x165667B19E3779F9) & 0xFFFFFFFFFFFFFFFF
    k ^= (h * 0x85EBCA77C2B2AE63) & 0xFFFFFFFFFFFFFFFF
    return k & 0xFFFFFFFFFFFFFFFF

# --- DDS filename parser ---
def parse_texpack_filename(name):
    """Parse tex1_{W}x{H}_{data_hash}[_{tlut_hash}]_{fmt}.dds"""
    if not name.startswith("tex1_") or not name.endswith(".dds"):
        return None
    body = name[5:-4]
    x_pos = body.find('x')
    if x_pos < 1: return None
    try: w = int(body[:x_pos])
    except ValueError: return None
    rest = body[x_pos + 1:]
    us = rest.find('_')
    if us < 1: return None
    try: h = int(rest[:us])
    except ValueError: return None
    rest = rest[us + 1:]
    if len(rest) < 16: return None
    try: data_hash = int(rest[:16], 16)
    except ValueError: return None
    rest = rest[16:]
    if not rest.startswith('_'): return None
    rest = rest[1:]
    parts = rest.split('_')
    if len(parts) == 1:
        try: return (w, h, data_hash, 0, int(parts[0]))
        except ValueError: return None
    elif len(parts) == 2:
        if parts[0] == '$':
            try: return (w, h, data_hash, 0, int(parts[1]))
            except ValueError: return None
        try: return (w, h, data_hash, int(parts[0], 16), int(parts[1]))
        except ValueError: return None
    return None

# --- DDS decoder ---
DXGI_BC1, DXGI_BC3, DXGI_BC7, DXGI_RGBA8, DXGI_BGRA8 = 71, 77, 98, 28, 87

def decode_dds_to_rgba(data):
    """Decode any DDS to RGBA bytes. Returns (width, height, rgba_bytes) or None."""
    if len(data) < 128: return None
    magic = struct.unpack_from('<I', data, 0)[0]
    if magic != 0x20534444: return None
    dds_h, dds_w = struct.unpack_from('<II', data, 12)
    pf_flags = struct.unpack_from('<I', data, 80)[0]
    pf_fourcc = struct.unpack_from('<I', data, 84)[0]

    hdr_size, dxgi, compressed, block_size = 128, 0, False, 0

    if (pf_flags & 4) and pf_fourcc == 0x30315844:
        if len(data) < 148: return None
        dxgi = struct.unpack_from('<I', data, 128)[0]
        hdr_size = 148
        if dxgi == DXGI_BC7: compressed, block_size = True, 16
        elif dxgi == DXGI_BC3: compressed, block_size = True, 16
        elif dxgi == DXGI_BC1: compressed, block_size = True, 8
        elif dxgi in (DXGI_RGBA8, DXGI_BGRA8): compressed = False
        else: return None
    elif (pf_flags & 4):
        if pf_fourcc == 0x31545844: compressed, block_size, dxgi = True, 8, DXGI_BC1
        elif pf_fourcc == 0x35545844: compressed, block_size, dxgi = True, 16, DXGI_BC3
        else: return None
    else:
        if struct.unpack_from('<I', data, 88)[0] == 32: dxgi = DXGI_RGBA8
        else: return None

    if compressed:
        comp_size = ((dds_w + 3) // 4) * ((dds_h + 3) // 4) * block_size
    else:
        comp_size = dds_w * dds_h * 4
    if len(data) < hdr_size + comp_size: return None
    pixel_data = data[hdr_size:hdr_size + comp_size]

    if compressed:
        if dxgi == DXGI_BC7: rgba = texture2ddecoder.decode_bc7(pixel_data, dds_w, dds_h)
        elif dxgi == DXGI_BC3: rgba = texture2ddecoder.decode_bc3(pixel_data, dds_w, dds_h)
        elif dxgi == DXGI_BC1: rgba = texture2ddecoder.decode_bc1(pixel_data, dds_w, dds_h)
        else: return None
        # texture2ddecoder returns BGRA
        rgba = bytearray(rgba)
        for i in range(0, len(rgba), 4):
            rgba[i], rgba[i+2] = rgba[i+2], rgba[i]
        rgba = bytes(rgba)
    else:
        rgba = pixel_data
        if dxgi == DXGI_BGRA8:
            rgba = bytearray(rgba)
            for i in range(0, len(rgba), 4):
                rgba[i], rgba[i+2] = rgba[i+2], rgba[i]
            rgba = bytes(rgba)

    return (dds_w, dds_h, rgba)

# --- Alpha detection ---
def has_meaningful_alpha(rgba_bytes):
    """Check if texture has real alpha (not just BC7 decode noise).
    BC7 decompression can produce alpha=254/253 on fully opaque textures.
    Use 240 as threshold — conservative enough to catch semi-transparent
    gradients while still filtering BC7 decode noise (typically 250-255)."""
    for i in range(3, len(rgba_bytes), 4):
        if rgba_bytes[i] < 240:
            return True
    return False

# --- DXT encoding ---
def encode_dxt(rgba_bytes, w, h, use_dxt5):
    """Encode RGBA to DXT1 or DXT5. Returns (dxt_bytes, fmt, pad_w, pad_h).

    pad_w/pad_h are the dims rounded up to a multiple of 4 (etcpak requirement).
    Caller needs them to size POT swizzle buffers when w/h aren't mult-of-4.
    """
    # etcpak requires dimensions to be multiples of 4
    pad_w = (w + 3) & ~3
    pad_h = (h + 3) & ~3
    if pad_w != w or pad_h != h:
        # Pad the image using edge-extension (repeat last col/row)
        # to avoid dark seams from LINEAR filtering into black padding
        padded = bytearray(pad_w * pad_h * 4)
        for row in range(pad_h):
            src_row = min(row, h - 1)
            src_off = src_row * w * 4
            dst_off = row * pad_w * 4
            # Copy original row data
            padded[dst_off:dst_off + w * 4] = rgba_bytes[src_off:src_off + w * 4]
            # Extend right edge pixel across padding columns
            if pad_w > w:
                edge_pixel = rgba_bytes[src_off + (w - 1) * 4:src_off + w * 4]
                for col in range(w, pad_w):
                    padded[dst_off + col * 4:dst_off + col * 4 + 4] = edge_pixel
        rgba_bytes = bytes(padded)

    if use_dxt5:
        return (etcpak.compress_to_dxt5(rgba_bytes, pad_w, pad_h), FMT_DXT5, pad_w, pad_h)
    else:
        return (etcpak.compress_to_dxt1(rgba_bytes, pad_w, pad_h), FMT_DXT1, pad_w, pad_h)


def swizzle_for_v3(linear_dxt, orig_w, orig_h, pad_w, pad_h, fmt_id):
    """Swizzle (pad_w x pad_h) linear DXT into a POT-sized Morton buffer."""
    block_bytes = 16 if fmt_id == FMT_DXT5 else 8
    bw_lin = pad_w // 4
    bh_lin = pad_h // 4
    pot_w = _nearest_po2(orig_w)
    pot_h = _nearest_po2(orig_h)
    bw_pot = (pot_w + 3) // 4
    bh_pot = (pot_h + 3) // 4
    return swizzle_dxt_blocks(linear_dxt, bw_lin, bh_lin, bw_pot, bh_pot, block_bytes)

# --- Main ---
def main():
    # Parse args: optional --v2 flag selects legacy linear layout.
    write_v3 = True
    positional = []
    for a in sys.argv[1:]:
        if a == "--v2":
            write_v3 = False
        elif a == "--v3":
            write_v3 = True
        else:
            positional.append(a)

    if not positional:
        print(f"Usage: {sys.argv[0]} [--v2|--v3] <texture_pack_dir> [output_file]")
        print("  --v3 (default): pre-swizzled blocks, zero-CPU-cost on Vita")
        print("  --v2:           legacy linear blocks (Vita swizzles at runtime)")
        sys.exit(1)

    pack_dir = positional[0]
    output_file = positional[1] if len(positional) > 1 else os.path.join(pack_dir, "vita_texcache.vtc")

    vtc_version = VTC_VERSION_V3 if write_v3 else VTC_VERSION_V2
    print(f"Building VTC v{vtc_version} ({'pre-swizzled' if write_v3 else 'linear'} layout)")

    if not os.path.isdir(pack_dir):
        print(f"ERROR: {pack_dir} is not a directory"); sys.exit(1)

    # Collect DDS files
    print(f"Scanning {pack_dir}...")
    dds_files = []  # (parsed, read_func)

    for fp in glob.glob(os.path.join(pack_dir, "**/*.dds"), recursive=True):
        p = parse_texpack_filename(os.path.basename(fp))
        if p: dds_files.append((p, lambda f=fp: open(f, 'rb').read()))

    for zp in glob.glob(os.path.join(pack_dir, "**/*.zip"), recursive=True):
        try:
            zf = zipfile.ZipFile(zp, 'r')
            for info in zf.infolist():
                if info.is_dir(): continue
                bn = os.path.basename(info.filename)
                if not bn.lower().endswith('.dds'): continue
                p = parse_texpack_filename(bn)
                if p: dds_files.append((p, lambda z=zf, n=info.filename: z.read(n)))
        except Exception as e:
            print(f"  Warning: {zp}: {e}")

    total = len(dds_files)
    print(f"Found {total} textures")
    if total == 0:
        print("No textures found."); sys.exit(1)

    # Process all textures
    print("Converting textures...")
    t_start = time.time()

    entries = []  # (cache_key, compressed_bytes, hd_w, hd_h, fmt_id)
    dxt1_count = dxt5_count = failed = 0
    total_compressed_size = 0

    for i, (parsed, read_func) in enumerate(dds_files):
        orig_w, orig_h, data_hash, tlut_hash, gc_fmt = parsed
        cache_key = loaded_cache_key(data_hash, tlut_hash, gc_fmt, orig_w, orig_h)

        try:
            dds_data = read_func()
            result = decode_dds_to_rgba(dds_data)
        except Exception:
            failed += 1; continue
        if result is None:
            failed += 1; continue

        hd_w, hd_h, rgba = result
        use_alpha = has_meaningful_alpha(rgba)
        comp_data, fmt_id, pad_w, pad_h = encode_dxt(rgba, hd_w, hd_h, use_alpha)

        if write_v3:
            comp_data = swizzle_for_v3(comp_data, hd_w, hd_h, pad_w, pad_h, fmt_id)

        if fmt_id == FMT_DXT1: dxt1_count += 1
        else: dxt5_count += 1
        total_compressed_size += len(comp_data)

        entries.append((cache_key, comp_data, hd_w, hd_h, fmt_id))

        if (i + 1) % 500 == 0 or i == total - 1:
            elapsed = time.time() - t_start
            pct = (i + 1) * 100 // total
            print(f"  {i+1}/{total} ({pct}%) — DXT1:{dxt1_count} DXT5:{dxt5_count} failed:{failed} — {elapsed:.1f}s")

    # Sort by cache_key for binary search on Vita
    entries.sort(key=lambda e: e[0])

    # Check for duplicate keys
    deduped = []
    seen = set()
    for e in entries:
        if e[0] not in seen:
            seen.add(e[0])
            deduped.append(e)
    if len(deduped) < len(entries):
        print(f"  Removed {len(entries) - len(deduped)} duplicate cache keys")
    entries = deduped

    # Write VTC file
    print(f"Writing {output_file}...")
    count = len(entries)
    index_size = 16 + count * 24  # header + index table
    data_offset = index_size  # compressed data starts right after index

    with open(output_file, 'wb') as f:
        # Header
        f.write(struct.pack('<IIII', VTC_MAGIC, vtc_version, count, 0))

        # Build index table (need to compute offsets first)
        offsets = []
        cur_offset = data_offset
        for cache_key, comp_data, hd_w, hd_h, fmt_id in entries:
            offsets.append(cur_offset)
            cur_offset += len(comp_data)

        # Write index
        for idx, (cache_key, comp_data, hd_w, hd_h, fmt_id) in enumerate(entries):
            f.write(struct.pack('<QII HH B 3x',
                                cache_key,
                                offsets[idx],
                                len(comp_data),
                                hd_w, hd_h,
                                fmt_id))

        # Write compressed data
        for cache_key, comp_data, hd_w, hd_h, fmt_id in entries:
            f.write(comp_data)

    elapsed = time.time() - t_start
    file_size = os.path.getsize(output_file)
    print(f"\nDone in {elapsed:.1f}s!")
    print(f"  Textures: {count} (DXT1:{dxt1_count} DXT5:{dxt5_count}, {failed} failed)")
    print(f"  File size: {file_size / (1024*1024):.1f} MB")
    print(f"  Index: {count * 24 / 1024:.0f} KB")
    print(f"  Compressed data: {total_compressed_size / (1024*1024):.1f} MB")
    print(f"\nCopy to Vita: ux0:data/AnimalCrossing/texture_pack/vita_texcache.vtc")

if __name__ == '__main__':
    main()
