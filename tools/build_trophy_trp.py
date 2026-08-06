#!/usr/bin/env python3
# build_trophy_trp.py
# Builds sce_sys/trophy/<TITLEID>_00/TROPHY.TRP for the Vita port: generates
# TROPCONF.SFM + 38 trophy icons sourced from the HD texture pack, then packs
# them into an unencrypted TRP for use with Rinnegatamante's NoTrpDrm plugin.
#
# TRP container is the PS3/Vita format (big-endian, 0x40 header + 0x40 entries).
# The distributable SFM is raw XML prefixed with a dummy Sce-Np-Trophy-Signature
# comment; NoTrpDrm disables the signature check so the hex need not be valid.

import os
import sys
import struct
import glob
import argparse
import xml.sax.saxutils as sax

# Reuse the tested DDS decoder from the VTC pipeline (no side effects: it has
# a __main__ guard).
_HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, _HERE)
from build_vita_texcache import decode_dds_to_rgba, parse_texpack_filename

try:
    from PIL import Image, ImageDraw, ImageFilter
except ImportError:
    sys.exit("error: Pillow (PIL) is required. pip install Pillow")

NP_COMM_ID = "ACGC00001_01"
TITLE_NAME = "Animal Crossing"
TITLE_DETAIL = "Trophies for the Animal Crossing PS Vita port."
SET_VERSION = "01.00"
ICON_SIZE = 240
BANNER_W, BANNER_H = 320, 176

# id -> (ttype, name, detail, hd_subpath, variant)
# ttype: P=platinum G=gold S=silver B=bronze. hd_subpath is relative to GAF/.
# variant picks the Nth-largest texture in that folder so reused folders differ.
TROPHIES = [
    ("P", "Mayor's Commendation", "Earn every other trophy.", "Furniture/Mouth of truth", 0),
    ("B", "Welcome to Town", "Take out your first home loan from Tom Nook.", "OI:50", 0),
    ("S", "Paid in Full", "Pay off your final home loan.", "OI:54", 0),
    ("B", "For the Collection", "Donate your first item to the museum.", "IDX:Other Items:5", 0),
    ("S", "Master Angler", "Donate every species of fish to the museum.", "FISH:34", 0),
    ("S", "Entomologist", "Donate every species of bug to the museum.", "OI:43", 0),
    ("S", "Fossil Hunter", "Complete the museum's fossil collection.", "OI:69", 0),
    ("S", "Art Connoisseur", "Donate every painting to the museum.", "Furniture/Paintings", 0),
    ("G", "The Curator", "Complete all four museum exhibits.", "OI:31", 0),
    ("B", "Reel Beginner", "Catch your first fish.", "FISH:24", 0),
    ("B", "Net Gain", "Catch your first bug.", "BUG:17", 0),
    ("B", "The Big One", "Catch a coelacanth.", "FISH:81", 0),
    ("S", "Tourney Champ", "Win a fishing tourney.", "Furniture/Fishing trophies@180", 4),
    ("B", "Retro Gamer", "Collect your first NES game.", "IDX:Furniture/NES:17#edgekey", 0),
    ("S", "Cartridge Collector", "Collect every common NES game.", "Furniture/Autumn medal", 0),
    ("B", "Penpal", "Send a letter to a neighbor.", "OI:7", 0),
    ("S", "Best Friends", "Reach maximum friendship with a neighbor.", "OI:42", 0),
    ("B", "Howdy, Neighbor", "Welcome a new villager to town.", "Structures/Signposts", 0),
    ("B", "Saturday Night", "Attend a K.K. Slider concert.", "OI:3", 0),
    ("S", "Bootleg Collector", "Collect every K.K. Slider aircheck.", "OI:40", 0),
    ("B", "Black Market", "Buy an item from Crazy Redd.", "OI:60", 0),
    ("B", "Resetti's Wrath", "Get lectured by Mr. Resetti.", "OI:23", 0),
    ("B", "Festival Spirit", "Take part in a seasonal town event.", "OI:22", 0),
    ("S", "Going for Gold", "Obtain a golden tool.", "OI:48", 0),
    ("G", "All That Glitters", "Collect every golden tool.", "OI:52", 0),
    ("S", "Perfect Town", "Achieve a perfect town rating.", "OI:8", 0),
    ("B", "Able Apprentice", "Create a custom design at the Able Sisters.", "OI:81", 0),
    ("B", "Sound of Home", "Set a town tune and town flag.", "OI:82", 0),
    ("B", "Six Figures", "Bank 100,000 Bells.", "OI:64", 0),
    ("S", "Bell Baron", "Bank 999,999 Bells.", "OI:28", 0),
    ("B", "Stalk Market", "Turn a profit selling turnips.", "Other Items/Turnips", 0),
    ("B", "Island Getaway", "Visit the island with Kapp'n.", "OI:15", 0),
    ("B", "Going Coconuts", "Plant a coconut palm.", "OI:33", 0),
    ("S", "Island Hospitality", "Befriend an islander.", "Environment/Shells#strip", 2),
    ("S", "Interior Designer", "Earn a Happy Room Academy reward.", "OI:55", 0),
    ("B", "Lloid & Found", "Dig up your first gyroid.", "OI:4", 0),
    ("B", "Let It Snow", "Build a perfect snowman.", "Environment/Trees/Tree#br", 0),
    ("S", "Pack Rat", "Collect 50 pieces of furniture.", "Furniture/Apple box@180", 0),
    ("B", "Resetti's Nemesis", "Reset the game five times.", "IDX:UI Elements/Resetti input:5", 0),
    ("B", "Beach Bum", "Get a sunburn at the beach.", "IDX:Environment/Shells:12", 0),
]

# trophy ids hidden until unlocked (in TROPCONF.SFM)
HIDDEN_IDS = {38, 39}

# Set icon (ICON0.PNG) source.
ICON0_SUBPATH = "Environment/Trees/Tree"

# grade -> Environment/Grass tile (hash substring); a distinct AC ground shape +
# colour per grade so the trophy tier is readable from the background alone.
GRADE_GROUND = {
    "B": "903adc6355e36f59_20430b0042498b04",
    "S": "626709d910129694_963dc45f18e13cf7",
    "G": "e96fecd242518c9f_4f54e78515f0a185",
    "P": "cfa57a6e6f966d11_6d5ff5628ae85b42",
    "SET": "cfa57a6e6f966d11_6d5ff5628ae85b42",
}


def _largest_dds(folder):
    # Return DDS paths in the folder sorted by pixel area (largest first).
    cands = []
    for p in glob.glob(os.path.join(folder, "*.dds")):
        dims = parse_texpack_filename(os.path.basename(p))
        area = (dims[0] * dims[1]) if dims else 0
        cands.append((area, p))
    cands.sort(key=lambda t: t[0], reverse=True)
    return [p for _, p in cands]


def load_texture(hd_root, subpath, variant):
    # Decode the variant-th largest texture under GAF/<subpath> to an RGBA image.
    folder = os.path.join(hd_root, "GAF", subpath)
    if not os.path.isdir(folder):
        return None
    paths = _largest_dds(folder)
    if not paths:
        return None
    order = paths[variant:] + paths[:variant] if variant < len(paths) else paths
    for p in order:
        try:
            with open(p, "rb") as f:
                dec = decode_dds_to_rgba(f.read())
        except OSError:
            dec = None
        if dec:
            w, h, rgba = dec
            return Image.frombytes("RGBA", (w, h), bytes(rgba))
    return None


# Other Items round icons sit on a flat blue disc; this colour keys it out.
OI_DISC_BLUE = (99, 99, 255)


def strip_blue_disc(im, key=OI_DISC_BLUE, tol=52):
    # remove the flat blue fill AND the darker periwinkle outline/AA ring
    im = im.convert("RGBA")
    px = im.load()
    w, h = im.size
    k2 = tol * tol
    for y in range(h):
        for x in range(w):
            r, g, b, a = px[x, y]
            if not a:
                continue
            flat = (r - key[0]) ** 2 + (g - key[1]) ** 2 + (b - key[2]) ** 2 < k2
            peri = abs(r - g) <= 46 and (b - max(r, g)) >= 40 and b >= 95
            if flat or peri:
                px[x, y] = (r, g, b, 0)
    return im


def load_other_item(hd_root, index):
    # Load the index-th (sorted) round icon from Other Items, disc removed.
    paths = sorted(glob.glob(os.path.join(hd_root, "GAF", "Other Items", "*.dds")))
    if index < 0 or index >= len(paths):
        return None
    try:
        with open(paths[index], "rb") as f:
            dec = decode_dds_to_rgba(f.read())
    except OSError:
        dec = None
    if not dec:
        return None
    w, h, rgba = dec
    return strip_blue_disc(Image.frombytes("RGBA", (w, h), bytes(rgba)))


def _decode_idx(hd_root, folder, index):
    paths = sorted(glob.glob(os.path.join(hd_root, "GAF", folder, "*.dds")))
    if index < 0 or index >= len(paths):
        return None
    try:
        with open(paths[index], "rb") as f:
            dec = decode_dds_to_rgba(f.read())
    except OSError:
        dec = None
    if not dec:
        return None
    w, h, rgba = dec
    return Image.frombytes("RGBA", (w, h), bytes(rgba))


def _key_black_bg(im, thr=46):
    # NES art is light-on-black; clear all near-black pixels to transparent
    im = im.convert("RGBA")
    px = im.load()
    w, h = im.size
    for y in range(h):
        for x in range(w):
            r, g, b, a = px[x, y]
            if a and r < thr and g < thr and b < thr:
                px[x, y] = (r, g, b, 0)
    return im


def _key_black_edge(im, thr=64):
    # remove only the border-connected near-black bg (flood fill from the edges),
    # preserving interior black enclosed by lighter pixels (e.g. a controller body)
    from collections import deque
    im = im.convert("RGBA")
    px = im.load()
    w, h = im.size

    def blk(x, y):
        r, g, b, a = px[x, y]
        return a > 0 and r < thr and g < thr and b < thr

    seen = bytearray(w * h)
    dq = deque()
    for x in range(w):
        for y in (0, h - 1):
            if blk(x, y) and not seen[y * w + x]:
                seen[y * w + x] = 1
                dq.append((x, y))
    for y in range(h):
        for x in (0, w - 1):
            if blk(x, y) and not seen[y * w + x]:
                seen[y * w + x] = 1
                dq.append((x, y))
    while dq:
        x, y = dq.popleft()
        r, g, b, _a = px[x, y]
        px[x, y] = (r, g, b, 0)
        for nx, ny in ((x + 1, y), (x - 1, y), (x, y + 1), (x, y - 1)):
            if 0 <= nx < w and 0 <= ny < h and not seen[ny * w + nx] and blk(nx, ny):
                seen[ny * w + nx] = 1
                dq.append((nx, ny))
    return im


def _mirror_full(im):
    # bug textures store one symmetric half; mirror on X and join at the seam
    half = _trim_alpha(im)
    mir = half.transpose(Image.FLIP_LEFT_RIGHT)
    full = Image.new("RGBA", (half.width * 2, half.height), (0, 0, 0, 0))
    full.paste(half, (0, 0))
    full.paste(mir, (half.width, 0))
    return _trim_alpha(full)


def load_subject(hd_root, subpath, variant):
    # prefixes pick indexed HD icons; "@<deg>" rotates; "#strip" keys the blue disc.
    rot = 0
    strip = False
    crop_br = False
    edgekey = False
    if subpath.endswith("#edgekey"):
        subpath = subpath[:-8]
        edgekey = True
    if subpath.endswith("#strip"):
        subpath = subpath[:-6]
        strip = True
    if subpath.endswith("#br"):
        subpath = subpath[:-3]
        crop_br = True
    if "@" in subpath:
        subpath, deg = subpath.rsplit("@", 1)
        rot = int(deg)
    if subpath.startswith("OI:"):
        im = load_other_item(hd_root, int(subpath[3:]))
    elif subpath.startswith("FISH:"):
        im = _decode_idx(hd_root, "Fish", int(subpath[5:]))
        if im is not None:
            im = strip_blue_disc(im)
    elif subpath.startswith("BUG:"):
        im = _decode_idx(hd_root, "Bugs", int(subpath[4:]))
        if im is not None:
            im = _mirror_full(im)
    elif subpath.startswith("NES:"):
        im = _decode_idx(hd_root, "Furniture/NES", int(subpath[4:]))
        if im is not None:
            im = _key_black_bg(im)
    elif subpath.startswith("IDX:"):
        # IDX:<folder>:<n> picks the name-sorted Nth .dds in that folder
        folder, n = subpath[4:].rsplit(":", 1)
        im = _decode_idx(hd_root, folder, int(n))
    else:
        im = load_texture(hd_root, subpath, variant)
    if im is not None and crop_br:
        w, h = im.size
        im = im.crop((w // 2, int(h * 0.38), w, h))
    if im is not None and edgekey:
        im = _key_black_edge(im)
    if im is not None and strip:
        im = strip_blue_disc(im)
    if im is not None and rot:
        im = im.rotate(rot, expand=True)
    return im


def load_grade_ground(hd_root, grade):
    # Decode the grade's chosen Environment/Grass tile to an RGB image.
    folder = os.path.join(hd_root, "GAF", "Environment", "Grass")
    needle = GRADE_GROUND[grade]
    for p in sorted(glob.glob(os.path.join(folder, "*.dds"))):
        if needle in os.path.basename(p):
            try:
                with open(p, "rb") as f:
                    dec = decode_dds_to_rgba(f.read())
            except OSError:
                dec = None
            if dec:
                w, h, rgba = dec
                return Image.frombytes("RGBA", (w, h), bytes(rgba)).convert("RGB")
    return None


def _avg(img):
    return img.resize((1, 1)).getpixel((0, 0))


def _trim_alpha(img):
    # Crop transparent margins so the subject fills the icon.
    if img.mode != "RGBA":
        return img
    bbox = img.getchannel("A").getbbox()
    return img.crop(bbox) if bbox else img


def compose_icon(tex, ground, size=ICON_SIZE):
    bg = ground.convert("RGB").resize((size, size), Image.LANCZOS).convert("RGBA")
    # dark, semi-transparent backdrop tinted to the ground so the subject pops
    dark = tuple(int(c * 0.40) for c in _avg(ground)[:3])
    glow = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    ImageDraw.Draw(glow).ellipse([10, 10, size - 10, size - 10], fill=dark + (185,))
    glow = glow.filter(ImageFilter.GaussianBlur(14))
    bg = Image.alpha_composite(bg, glow)
    if tex is not None:
        sub = _trim_alpha(tex)
        if sub.width and sub.height:
            inner = int(size * 0.50)
            scale = min(inner / sub.width, inner / sub.height)
            nw = max(1, int(sub.width * scale))
            nh = max(1, int(sub.height * scale))
            sub = sub.resize((nw, nh), Image.LANCZOS)
            shadow = Image.new("RGBA", bg.size, (0, 0, 0, 0))
            sx, sy = (size - nw) // 2 + 2, (size - nh) // 2 + 3
            shadow.paste((0, 0, 0, 110), (sx, sy), sub)
            bg = Image.alpha_composite(bg, shadow)
            bg.paste(sub, ((size - nw) // 2, (size - nh) // 2), sub)
    return bg.convert("RGBA")


def _tiled_ground(ground, w, h, tile=240):
    # repeat the square grass tile to fill w x h (then center-crop) instead of
    # stretching it - keeps the polka dots circular. Tile is seamless in-game.
    import math
    t = ground.convert("RGB").resize((tile, tile), Image.LANCZOS)
    cols, rows = math.ceil(w / tile) + 1, math.ceil(h / tile) + 1
    canvas = Image.new("RGB", (cols * tile, rows * tile))
    for r in range(rows):
        for c in range(cols):
            canvas.paste(t, (c * tile, r * tile))
    left, top = (canvas.width - w) // 2, (canvas.height - h) // 2
    return canvas.crop((left, top, left + w, top + h))


def make_banner(hd_root):
    # 320x176 trophy-set banner: gold-tier green-dot grass + the AC title logo.
    w, h = BANNER_W, BANNER_H
    ground = load_grade_ground(hd_root, "G")
    bg = _tiled_ground(ground, w, h).convert("RGBA")
    dark = tuple(int(c * 0.40) for c in _avg(ground)[:3])
    glow = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    ImageDraw.Draw(glow).ellipse([w * 0.10, 4, w * 0.90, h - 4], fill=dark + (165,))
    glow = glow.filter(ImageFilter.GaussianBlur(22))
    bg = Image.alpha_composite(bg, glow)
    # centered subject: the "Welcome to Animal Crossing" LiveArea logo
    logo_path = os.path.join(_HERE, "..", "vita", "sce_sys", "livearea",
                             "contents", "startup.png")
    sub = None
    if os.path.isfile(logo_path):
        sub = _trim_alpha(Image.open(logo_path).convert("RGBA"))
    if sub is None:
        sub = _trim_alpha(load_subject(hd_root, "OI:50", 0))
    if sub is not None:
        scale = min((w * 0.96) / sub.width, (h * 0.92) / sub.height)
        nw, nh = max(1, int(sub.width * scale)), max(1, int(sub.height * scale))
        sub = sub.resize((nw, nh), Image.LANCZOS)
        shadow = Image.new("RGBA", (w, h), (0, 0, 0, 0))
        shadow.paste((0, 0, 0, 110), ((w - nw) // 2 + 2, (h - nh) // 2 + 3), sub)
        bg = Image.alpha_composite(bg, shadow)
        bg.paste(sub, ((w - nw) // 2, (h - nh) // 2), sub)
    return bg.convert("RGBA")


def gen_tropconf_sfm():
    # 160-byte signature (320 hex chars): magic 4c39b98c + version 0100 + zeros.
    sig = "4c39b98c0100" + "0" * 308
    out = []
    out.append('<?xml version="1.0" encoding="UTF-8" standalone="no" ?>')
    out.append("<!--Sce-Np-Trophy-Signature: " + sig + "-->")
    out.append('<trophyconf version="1">')
    out.append("<npcommid>%s</npcommid>" % NP_COMM_ID)
    out.append("<trophyset-version>%s</trophyset-version>" % SET_VERSION)
    out.append("<parental-level>0</parental-level>")
    out.append("<title-name>%s</title-name>" % sax.escape(TITLE_NAME))
    out.append("<title-detail>%s</title-detail>" % sax.escape(TITLE_DETAIL))
    for i, (ttype, name, detail, _sp, _v) in enumerate(TROPHIES):
        hid = "yes" if i in HIDDEN_IDS else "no"
        out.append('<trophy id="%d" hidden="%s" ttype="%s" pid="0">' % (i, hid, ttype))
        out.append("<name>%s</name>" % sax.escape(name))
        out.append("<detail>%s</detail>" % sax.escape(detail))
        out.append("</trophy>")
    out.append("</trophyconf>")
    return ("\n".join(out) + "\n").encode("utf-8")


def pack_trp(files):
    # files: list of (name, bytes). Returns the complete TRP image.
    HDR, ENT = 0x40, 0x40
    n = len(files)
    data_off = HDR + n * ENT
    entries = bytearray()
    blob = bytearray()
    off = data_off
    for name, content in files:
        nb = name.encode("ascii")
        if len(nb) >= 32:
            raise ValueError("filename too long: %s" % name)
        ent = nb.ljust(32, b"\0")
        ent += struct.pack(">Q", off)
        ent += struct.pack(">Q", len(content))
        ent += struct.pack(">I", 0)
        ent += b"\0" * 12
        assert len(ent) == ENT
        entries += ent
        blob += content
        off += len(content)
    total = data_off + len(blob)
    hdr = struct.pack(">I", 0xDCA24D00)
    hdr += struct.pack(">I", 0x00000003)
    hdr += struct.pack(">Q", total)
    hdr += struct.pack(">I", n)
    hdr += struct.pack(">I", ENT)
    hdr += struct.pack(">I", 0)
    hdr += b"\0" * 20
    hdr += b"\0" * 16
    assert len(hdr) == HDR
    return bytes(hdr) + bytes(entries) + bytes(blob)


def build_icons(hd_root, dump_dir=None):
    # Returns dict name -> PNG bytes for ICON0.PNG + TROP000..TROPNNN.PNG.
    import io
    icons = {}

    def to_png(img):
        buf = io.BytesIO()
        img.save(buf, "PNG")
        return buf.getvalue()

    grounds = {g: load_grade_ground(hd_root, g) for g in GRADE_GROUND}
    missing_ground = [g for g, im in grounds.items() if im is None]
    if missing_ground:
        sys.exit("error: grade ground tile not found for: %s" % ", ".join(missing_ground))

    icons["ICON0.PNG"] = to_png(make_banner(hd_root))
    misses = []
    for i, (ttype, name, _d, sp, var) in enumerate(TROPHIES):
        tex = load_subject(hd_root, sp, var)
        if tex is None:
            misses.append("%d %s (%s)" % (i, name, sp))
        icons["TROP%03d.PNG" % i] = to_png(compose_icon(tex, grounds[ttype]))
    if dump_dir:
        os.makedirs(dump_dir, exist_ok=True)
        for k, v in icons.items():
            with open(os.path.join(dump_dir, k), "wb") as f:
                f.write(v)
    if misses:
        print("warning: no HD texture found for:")
        for m in misses:
            print("  " + m)
    return icons


def main():
    ap = argparse.ArgumentParser(description="Build TROPHY.TRP for the Vita port.")
    ap.add_argument("--hd-pack", required=True,
                    help="path to the ACHD texture pack root (contains GAF/)")
    ap.add_argument("--out", required=True, help="output TROPHY.TRP path")
    ap.add_argument("--icons-dir",
                    help="use hand-curated PNGs from here instead of the HD pack")
    ap.add_argument("--dump-icons", help="also write generated PNGs to this dir")
    args = ap.parse_args()

    if args.icons_dir:
        icons = {}
        for n in ["ICON0.PNG"] + ["TROP%03d.PNG" % i for i in range(len(TROPHIES))]:
            p = os.path.join(args.icons_dir, n)
            if not os.path.isfile(p):
                sys.exit("error: missing icon %s in %s" % (n, args.icons_dir))
            with open(p, "rb") as f:
                icons[n] = f.read()
    else:
        icons = build_icons(args.hd_pack, args.dump_icons)

    files = [("TROPCONF.SFM", gen_tropconf_sfm()), ("ICON0.PNG", icons["ICON0.PNG"])]
    for i in range(len(TROPHIES)):
        files.append(("TROP%03d.PNG" % i, icons["TROP%03d.PNG" % i]))

    trp = pack_trp(files)
    os.makedirs(os.path.dirname(os.path.abspath(args.out)), exist_ok=True)
    with open(args.out, "wb") as f:
        f.write(trp)
    print("wrote %s (%d files, %d bytes)" % (args.out, len(files), len(trp)))


if __name__ == "__main__":
    main()
