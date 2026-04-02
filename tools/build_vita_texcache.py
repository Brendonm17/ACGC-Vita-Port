#!/usr/bin/env python3
"""Build vita_texcache.vtc for PS Vita from a Dolphin-compatible HD texture pack.

Decodes all DDS files (BC7/BC3/BC1/RGBA) -> RGBA -> PVRTC or DXT -> VTC cache.
Square power-of-2 textures use PVRTC 4bpp (native PowerVR, fastest upload).
Non-square/non-POT textures fall back to DXT1/DXT5.

Usage:
    python build_vita_texcache.py <texture_pack_dir> [output_file]

Output should be copied to: ux0:data/AnimalCrossing/texture_pack/vita_texcache.vtc
"""

import struct
import os
import sys
import glob
import zipfile
import time
import subprocess

try:
    import texture2ddecoder
except ImportError:
    print("ERROR: pip install texture2ddecoder"); sys.exit(1)
try:
    import etcpak
except ImportError:
    print("ERROR: pip install etcpak"); sys.exit(1)

# --- VTC format constants ---
VTC_MAGIC    = 0x56544331  # "VTC1"
VTC_VERSION  = 2
FMT_DXT1     = 1
FMT_PVRTC4   = 4   # PVRTC V1 4bpp (native PowerVR, square POT)
FMT_DXT5     = 5
FMT_PVRTCII  = 6   # PVRTCII V2 4bpp (native PowerVR, non-square POT)

# --- Tool paths ---
PVRTC_ENCODE = os.path.join(os.path.dirname(os.path.abspath(__file__)), "pvrtc_encode")
if sys.platform == "win32":
    PVRTC_ENCODE += ".exe"

PVRTEXTOOL_CLI = os.environ.get("PVRTEXTOOL_CLI",
    r"C:\Program Files\Imgtec\PowerVR_Tools\PVRTexTool\CLI\Windows_x86_64\PVRTexToolCLI.exe")

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

# --- Power-of-2 check ---
def is_pot(n):
    return n >= 8 and (n & (n - 1)) == 0

# --- DXT encoding ---
def encode_dxt(rgba_bytes, w, h, use_dxt5):
    """Encode RGBA to DXT1 or DXT5 using etcpak. Returns (dxt_bytes, format_id)."""
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
        w, h = pad_w, pad_h

    if use_dxt5:
        return (etcpak.compress_to_dxt5(rgba_bytes, w, h), FMT_DXT5)
    else:
        return (etcpak.compress_to_dxt1(rgba_bytes, w, h), FMT_DXT1)

# --- PVR file header parsing ---
PVR_HEADER_SIZE = 52  # PVR v3 header

def parse_pvr_data(pvr_bytes, expected_w=0, expected_h=0):
    """Extract raw compressed data from a PVR v3 file.
    If expected_w/h are given, reject if PVRTexTool resized the texture."""
    if len(pvr_bytes) < PVR_HEADER_SIZE:
        return None
    # PVR v3 header: version(4) flags(4) pixfmt(8) colourspace(4) chantype(4)
    #                height(4) width(4) depth(4) numsurfaces(4) numfaces(4)
    #                mipcount(4) metasize(4)
    version = struct.unpack_from('<I', pvr_bytes, 0)[0]
    if version != 0x03525650:  # "PVR\x03"
        return None
    if expected_w and expected_h:
        out_h = struct.unpack_from('<I', pvr_bytes, 24)[0]
        out_w = struct.unpack_from('<I', pvr_bytes, 28)[0]
        if out_w != expected_w or out_h != expected_h:
            print(f"  WARNING: PVRTexTool resized {expected_w}x{expected_h} -> {out_w}x{out_h}, falling back to DXT")
            return None
    meta_size = struct.unpack_from('<I', pvr_bytes, 48)[0]
    data_offset = PVR_HEADER_SIZE + meta_size
    return pvr_bytes[data_offset:]

# --- PVRTC encoding via PVRTexTool CLI ---
def encode_pvrtc_pvrtextool(rgba_bytes, w, h, is_square):
    """Encode RGBA to PVRTC via PVRTexToolCLI. Returns (data, fmt_id) or None."""
    import tempfile
    # Write raw RGBA as a temporary TGA file (simplest uncompressed format)
    # Actually, write as raw RGBA and use PVRTexTool's raw input
    # Simpler: write a temporary PNG, let PVRTexTool read it
    try:
        with tempfile.NamedTemporaryFile(suffix='.pvr', delete=False) as tmp_in:
            tmp_in_path = tmp_in.name
            # Write PVR v3 header with RGBA8 format
            # pixfmt for r8g8b8a8 = bytes [8,8,8,8,r,g,b,a] = specific encoding
            # Easier: write raw RGBA file and tell PVRTexTool the format
            # Actually simplest: create a raw file and use specific flags
            pass

        with tempfile.NamedTemporaryFile(suffix='.pvr', delete=False) as tmp_out:
            tmp_out_path = tmp_out.name

        # Write a minimal PVR v3 file with RGBA8 uncompressed data
        with open(tmp_in_path, 'wb') as f:
            # PVR v3 header
            f.write(struct.pack('<I', 0x03525650))   # version
            f.write(struct.pack('<I', 0))              # flags
            f.write(struct.pack('<Q', 0x0808080861626772))  # pixfmt: r8g8b8a8
            f.write(struct.pack('<I', 0))              # colourspace (lRGB)
            f.write(struct.pack('<I', 0))              # channel type (UBN)
            f.write(struct.pack('<I', h))              # height
            f.write(struct.pack('<I', w))              # width
            f.write(struct.pack('<I', 1))              # depth
            f.write(struct.pack('<I', 1))              # num surfaces
            f.write(struct.pack('<I', 1))              # num faces
            f.write(struct.pack('<I', 1))              # mip count
            f.write(struct.pack('<I', 0))              # meta size
            f.write(rgba_bytes)                        # pixel data

        # Choose format based on square vs non-square
        if is_square:
            pvr_fmt = "PVRTCI_4BPP_RGBA"
            fmt_id = FMT_PVRTC4
        else:
            pvr_fmt = "PVRTCII_4BPP"
            fmt_id = FMT_PVRTCII

        result = subprocess.run(
            [PVRTEXTOOL_CLI, "-i", tmp_in_path, "-f", pvr_fmt, "-o", tmp_out_path, "-shh", "-q", "pvrtcfast"],
            capture_output=True, timeout=30)

        if result.returncode != 0:
            return None

        with open(tmp_out_path, 'rb') as f:
            pvr_data = f.read()

        compressed = parse_pvr_data(pvr_data, w, h)
        if compressed is None:
            return None

        return (compressed, fmt_id)
    except Exception:
        return None
    finally:
        try: os.unlink(tmp_in_path)
        except: pass
        try: os.unlink(tmp_out_path)
        except: pass

# --- PVRTC encoding via built-in encoder (square POT only, fallback) ---
def encode_pvrtc_builtin(rgba_bytes, w, h):
    """Encode RGBA to PVRTC V1 4bpp via built-in tool. Returns (data, FMT_PVRTC4) or None."""
    result = subprocess.run(
        [PVRTC_ENCODE, str(w), str(h)],
        input=rgba_bytes, capture_output=True)
    if result.returncode != 0:
        return None
    expected_size = w * h // 2
    if len(result.stdout) != expected_size:
        return None
    return (result.stdout, FMT_PVRTC4)

# --- Texture encoder (chooses PVRTC or DXT) ---
def encode_texture(rgba_bytes, w, h, use_alpha, pvrtextool_available, pvrtc_builtin_available):
    """Encode texture to best available format.
    POT textures -> PVRTC (native PowerVR, via PVRTexTool or built-in).
    Non-POT -> DXT1/DXT5 fallback."""
    if is_pot(w) and is_pot(h):
        is_square = (w == h)

        # Try PVRTexTool first (handles both square and non-square)
        if pvrtextool_available:
            result = encode_pvrtc_pvrtextool(rgba_bytes, w, h, is_square)
            if result:
                return result

        # Fallback: built-in encoder (square only)
        if is_square and pvrtc_builtin_available:
            result = encode_pvrtc_builtin(rgba_bytes, w, h)
            if result:
                return result

    # DXT fallback for non-POT or encoder failures
    return encode_dxt(rgba_bytes, w, h, use_alpha)

# --- Main ---
def main():
    if len(sys.argv) < 2:
        print(f"Usage: {sys.argv[0]} <texture_pack_dir> [output_file]")
        sys.exit(1)

    pack_dir = sys.argv[1]
    output_file = sys.argv[2] if len(sys.argv) > 2 else os.path.join(pack_dir, "vita_texcache.vtc")

    if not os.path.isdir(pack_dir):
        print(f"ERROR: {pack_dir} is not a directory"); sys.exit(1)

    # Check for PVRTC encoders
    pvrtextool_available = os.path.isfile(PVRTEXTOOL_CLI)
    pvrtc_builtin_available = os.path.isfile(PVRTC_ENCODE)

    if pvrtextool_available:
        print(f"PVRTexTool CLI found: {PVRTEXTOOL_CLI}")
        print("  All POT textures will use native PVRTC (V1 square, V2 non-square)")
    elif pvrtc_builtin_available:
        print(f"Built-in PVRTC encoder found: {PVRTC_ENCODE}")
        print("  Only square POT textures will use PVRTC. Install PVRTexTool for full coverage.")
    else:
        print("WARNING: No PVRTC encoder found. All textures will use DXT.")
        print(f"  Install PVRTexTool or build: gcc -O2 -o pvrtc_encode pvrtc_encode.c")

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
    pvrtc_count = pvrtcii_count = dxt1_count = dxt5_count = failed = 0
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
        comp_data, fmt_id = encode_texture(rgba, hd_w, hd_h, use_alpha, pvrtextool_available, pvrtc_builtin_available)

        if fmt_id == FMT_PVRTC4: pvrtc_count += 1
        elif fmt_id == FMT_PVRTCII: pvrtcii_count += 1
        elif fmt_id == FMT_DXT1: dxt1_count += 1
        else: dxt5_count += 1
        total_compressed_size += len(comp_data)

        entries.append((cache_key, comp_data, hd_w, hd_h, fmt_id))

        if (i + 1) % 500 == 0 or i == total - 1:
            elapsed = time.time() - t_start
            pct = (i + 1) * 100 // total
            print(f"  {i+1}/{total} ({pct}%) — PVRTCv1:{pvrtc_count} PVRTCv2:{pvrtcii_count} DXT1:{dxt1_count} DXT5:{dxt5_count} failed:{failed} — {elapsed:.1f}s")

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
        f.write(struct.pack('<IIII', VTC_MAGIC, VTC_VERSION, count, 0))

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
    pvrtc_total = pvrtc_count + pvrtcii_count
    print(f"  Textures: {count} (PVRTC:{pvrtc_total} [V1:{pvrtc_count} V2:{pvrtcii_count}] DXT1:{dxt1_count} DXT5:{dxt5_count}, {failed} failed)")
    print(f"  File size: {file_size / (1024*1024):.1f} MB")
    print(f"  Index: {count * 24 / 1024:.0f} KB")
    print(f"  Compressed data: {total_compressed_size / (1024*1024):.1f} MB")
    print(f"\nCopy to Vita: ux0:data/AnimalCrossing/texture_pack/vita_texcache.vtc")

if __name__ == '__main__':
    main()
