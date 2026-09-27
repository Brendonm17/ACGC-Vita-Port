"""Build pc/include/pc_mp_text_data.h from pc/tools/mp_msgs.txt.

Multiplayer dialogue lives outside the ROM archives: messages get ids from
0x7000 and choice labels from 0x0300, clear of both ROM tables. Text is encoded
with the game's charset and control codes via tools/msg_tool.py.

Run by hand after editing mp_msgs.txt; the generated header is committed.
  python pc/tools/gen_mp_msgs.py
"""
import os
import re
import sys
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, "..", ".."))
sys.path.insert(0, os.path.join(ROOT, "tools"))
import msg_tool  # noqa: E402

SRC = os.path.join(HERE, "mp_msgs.txt")
OUT = os.path.join(ROOT, "pc", "include", "pc_mp_text_data.h")

MSG_BASE = 0x7000
CH_BASE = 0x0300
MSG_BUF_MAX = 1536
STR_CODE_RESERVE = 16  # a free/name string can expand a 2-byte code to 16 chars
CHOICE_LEN = 16
MAX_LINES = 4
LINE_MAX_PX = 212      # p99 of the ROM's own lines
STR_CODE_PX = 80       # names/free strings: assume 8 chars at 10 px
FONT_CELL_PX = 12      # mFont_TEX_CHAR_WIDTH; glyph width = cell - offset
FONT_OFFSETS = os.path.join(ROOT, "src", "game", "m_font_offset.c_inc")

HEADER = re.compile(r"^\[\[(VMSG|VCHOICE)\s+([A-Z0-9_]+)((?:\s+[a-z]+)*)\]\]\s*$")
REF = re.compile(r"\{(m|c):([A-Za-z0-9_]+)\}")
CODE = re.compile(r"<<([A-Z0-9_]+)(?:\s*\[([0-9A-Fa-f ]*)\])?>>")
TERMINATORS = ("MSGEND", "MSGCONTINUE", "MSGTIMEEND")
STR_CODES = {c for c in msg_tool.COMMANDS if c.startswith("STR_")}
PAGE_BREAK = "MSGCLEAR"


def fail(msg):
    sys.stderr.write("gen_mp_msgs: " + msg + "\n")
    sys.exit(1)


def load_font_offsets():
    with open(FONT_OFFSETS, "r", encoding="utf-8") as f:
        body = f.read().split("{", 2)[2].split("}", 1)[0]
    vals = [int(v) for v in re.findall(r"\d+", body)]
    if len(vals) != 256:
        fail(f"expected 256 font offsets in {FONT_OFFSETS}, got {len(vals)}")
    return vals


FONT_OFS = load_font_offsets()


def line_px(line):
    n_str = sum(1 for c in CODE.finditer(line) if c.group(1) in STR_CODES)
    px = n_str * STR_CODE_PX
    for ch in CODE.sub("", line):
        if ch in msg_tool.CHAR_MAP:
            px += FONT_CELL_PX - FONT_OFS[msg_tool.CHAR_MAP.index(ch)]
    return px


def parse(path):
    msgs, choices = [], []
    cur = None
    with open(path, "r", encoding="utf-8") as f:
        for lineno, raw in enumerate(f, 1):
            line = raw.rstrip("\n")
            if line.lstrip().startswith("//"):
                continue
            m = HEADER.match(line)
            if m:
                kind, name, attrs = m.group(1), m.group(2), m.group(3).split()
                cur = {"kind": kind, "name": name, "attrs": attrs, "lines": [], "line": lineno}
                (msgs if kind == "VMSG" else choices).append(cur)
                continue
            if cur is None:
                if line.strip():
                    fail(f"{path}:{lineno}: text outside an entry")
                continue
            cur["lines"].append(line)
    for e in msgs + choices:
        while e["lines"] and not e["lines"][-1].strip():
            e["lines"].pop()
    return msgs, choices


def resolve_refs(text, msg_ids, ch_ids, where):
    def sub(m):
        kind, key = m.group(1), m.group(2)
        table = msg_ids if kind == "m" else ch_ids
        if key.lower().startswith("0x"):
            val = int(key, 16)
        elif key in table:
            val = table[key]
        else:
            fail(f"{where}: unknown {'message' if kind == 'm' else 'choice'} '{key}'")
        return f"{val >> 8:02X} {val & 0xFF:02X}"
    return REF.sub(sub, text)


def check_message(name, text, where):
    codes = [c.group(1) for c in CODE.finditer(text)]
    for c in codes:
        if c not in msg_tool.COMMANDS:
            fail(f"{where}: unknown control code {c}")
    if not codes or codes[-1] not in TERMINATORS:
        fail(f"{where}: must end with MSGEND, MSGCONTINUE or MSGTIMEEND")

    # choices: every SETNEXTMSGn must point at an offered slot
    sel = [c for c in codes if c.startswith("SETSELSTR")]
    if sel:
        n = int(sel[-1][len("SETSELSTR"):])
        for c in codes:
            if c.startswith("SETNEXTMSG") and c[len("SETNEXTMSG"):].isdigit():
                if int(c[len("SETNEXTMSG"):]) >= n:
                    fail(f"{where}: {c} has no matching choice (only {n})")

    # page layout: at most 4 visible lines between page clears, each within the window
    for page in re.split(r"<<MSGCLEAR>>", text):
        lines = page.split("\n")
        while lines and not CODE.sub("", lines[-1]).strip() and not any(
                c.group(1) in STR_CODES for c in CODE.finditer(lines[-1])):
            lines.pop()
        if len(lines) > MAX_LINES:
            fail(f"{where}: page has {len(lines)} lines (max {MAX_LINES})")
        for ln in lines:
            px = line_px(ln)
            if px > LINE_MAX_PX:
                fail(f"{where}: line is {px} px (max {LINE_MAX_PX}): '{CODE.sub('', ln)}'")


def encode(text, where):
    for ch in CODE.sub("", text):
        if ch not in msg_tool.CHAR_MAP:
            fail(f"{where}: character {ch!r} is not in the AC charset")
    try:
        return bytes(msg_tool.encode_entry(text))
    except ValueError as e:
        fail(f"{where}: {e}")


def main():
    msgs, choices = parse(SRC)
    msg_ids = {e["name"]: MSG_BASE + i for i, e in enumerate(msgs)}
    ch_ids = {e["name"]: CH_BASE + i for i, e in enumerate(choices)}
    if len(msg_ids) != len(msgs) or len(ch_ids) != len(choices):
        fail("duplicate entry names")

    blob = bytearray()
    ofs, flags = [], []
    for e in msgs:
        where = f"{os.path.basename(SRC)}:{e['line']} {e['name']}"
        text = resolve_refs("\n".join(e["lines"]), msg_ids, ch_ids, where)
        check_message(e["name"], text, where)
        data = encode(text, where)
        n_str = sum(1 for c in CODE.finditer(text) if c.group(1) in STR_CODES)
        if len(data) + n_str * STR_CODE_RESERVE > MSG_BUF_MAX:
            fail(f"{where}: too long for the message buffer")
        ofs.append(len(blob))
        blob += data
        flags.append(1 if "pa" in e["attrs"] else 0)
    ofs.append(len(blob))

    labels = []
    for e in choices:
        where = f"{os.path.basename(SRC)}:{e['line']} {e['name']}"
        if len(e["lines"]) != 1:
            fail(f"{where}: a choice label is one line")
        data = encode(e["lines"][0], where)
        if len(data) > CHOICE_LEN:
            fail(f"{where}: label is {len(data)} bytes (max {CHOICE_LEN})")
        labels.append(data + b"\x20" * (CHOICE_LEN - len(data)))

    text_hash = zlib.crc32(bytes(blob) + b"".join(labels)) & 0xFFFFFFFF

    out = []
    out.append("// generated by pc/tools/gen_mp_msgs.py from pc/tools/mp_msgs.txt; do not edit")
    out.append("#ifndef PC_MP_TEXT_DATA_H")
    out.append("#define PC_MP_TEXT_DATA_H")
    out.append("")
    out.append(f"#define MP_VMSG_BASE  0x{MSG_BASE:04X}")
    out.append(f"#define MP_VMSG_COUNT {len(msgs)}")
    out.append(f"#define MP_VCH_BASE   0x{CH_BASE:04X}")
    out.append(f"#define MP_VCH_COUNT  {len(choices)}")
    out.append(f"#define MP_TEXT_HASH  0x{text_hash:08X}u")
    out.append("")
    out.append("enum {")
    for e in msgs:
        out.append(f"    MP_MSG_{e['name']} = 0x{msg_ids[e['name']]:04X},")
    out.append("};")
    out.append("")
    out.append("enum {")
    for e in choices:
        out.append(f"    MP_CH_{e['name']} = 0x{ch_ids[e['name']]:04X},")
    out.append("};")
    out.append("")
    out.append("#ifdef PC_MP_TEXT_DATA_IMPL")
    out.append(f"static const unsigned char mp_vmsg_bytes[{max(len(blob), 1)}] = {{")
    for i in range(0, len(blob), 16):
        out.append("    " + ", ".join(f"0x{b:02X}" for b in blob[i:i + 16]) + ",")
    out.append("};")
    out.append("")
    out.append(f"static const unsigned int mp_vmsg_ofs[{len(ofs)}] = {{")
    for i in range(0, len(ofs), 8):
        out.append("    " + ", ".join(str(v) for v in ofs[i:i + 8]) + ",")
    out.append("};")
    out.append("")
    out.append(f"static const unsigned char mp_vmsg_flags[{max(len(flags), 1)}] = {{")
    for i in range(0, len(flags), 16):
        out.append("    " + ", ".join(str(v) for v in flags[i:i + 16]) + ",")
    out.append("};")
    out.append("")
    out.append(f"static const unsigned char mp_vch_labels[{max(len(labels), 1)}][{CHOICE_LEN}] = {{")
    for e, lab in zip(choices, labels):
        out.append("    { " + ", ".join(f"0x{b:02X}" for b in lab) + " }, // " + e["name"])
    out.append("};")
    out.append("#endif")
    out.append("")
    out.append("#endif")

    with open(OUT, "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(out) + "\n")
    print(f"wrote {os.path.relpath(OUT, ROOT)}: {len(msgs)} messages ({len(blob)} bytes), "
          f"{len(choices)} choices, hash {text_hash:08X}")


if __name__ == "__main__":
    main()
