#!/usr/bin/env python3
"""Generate C header with embedded GXP shader binaries."""
import os

SHADER_DIR = os.path.dirname(os.path.abspath(__file__))
OUT_DIR = os.path.join(os.path.dirname(SHADER_DIR), "include")
OUT_FILE = os.path.join(OUT_DIR, "vita_gxp_shaders.h")

def emit_array(f, name, filepath):
    data = open(filepath, "rb").read()
    f.write(f"static const unsigned char {name}[{len(data)}] = {{\n")
    for i in range(0, len(data), 16):
        chunk = data[i:i+16]
        hex_vals = ", ".join(f"0x{b:02x}" for b in chunk)
        f.write(f"    {hex_vals},\n")
    f.write("};\n")
    f.write(f"#define {name}_size {len(data)}\n\n")

os.makedirs(OUT_DIR, exist_ok=True)
total = 0

with open(OUT_FILE, "w") as f:
    f.write("/* Auto-generated GXP shader binaries - do not edit */\n")
    f.write("/* Compiled via psp2cgc from default_v.cg, default_f.cg, simple_f.cg */\n")
    f.write("#ifndef VITA_GXP_SHADERS_H\n")
    f.write("#define VITA_GXP_SHADERS_H\n\n")

    # Vertex shader (basic — no lighting)
    f.write("/* Vertex shader (basic) */\n")
    vpath = os.path.join(SHADER_DIR, "default_v.gxp")
    emit_array(f, "gxp_vertex", vpath)
    total += os.path.getsize(vpath)

    # Vertex shader (LIGHTING — per-vertex lighting)
    vlit_path = os.path.join(SHADER_DIR, "default_v_lit.gxp")
    if os.path.exists(vlit_path):
        f.write("/* Vertex shader (LIGHTING) */\n")
        emit_array(f, "gxp_vertex_lit", vlit_path)
        total += os.path.getsize(vlit_path)
        f.write("#define VITA_HAS_VERTEX_LIGHTING 1\n\n")

    # 16 complex fragment variants (full branchless TEV)
    flags_names = {0:"base", 1:"L", 2:"F", 3:"LF", 4:"A", 5:"LA", 6:"FA", 7:"LFA",
                   8:"T", 9:"LT", 10:"FT", 11:"LFT", 12:"AT", 13:"LAT", 14:"FAT", 15:"LFAT"}
    for i in range(16):
        f.write(f"/* Complex fragment variant {i}: {flags_names[i]} */\n")
        p = os.path.join(SHADER_DIR, f"frag_v{i}.gxp")
        emit_array(f, f"gxp_frag_v{i}", p)
        total += os.path.getsize(p)

    f.write("/* Complex fragment variant lookup table */\n")
    f.write("static const unsigned char* gxp_frag_variants[16] = {\n")
    for i in range(16):
        f.write(f"    gxp_frag_v{i},\n")
    f.write("};\n")
    f.write("static const unsigned int gxp_frag_variant_sizes[16] = {\n")
    for i in range(16):
        f.write(f"    gxp_frag_v{i}_size,\n")
    f.write("};\n\n")

    # 8 simple fragment variants (tex * ras MODULATE, half precision)
    simple_names = {0:"base", 1:"L", 2:"F", 3:"LF", 4:"A", 5:"LA", 6:"FA", 7:"LFA"}
    for i in range(8):
        f.write(f"/* Simple fragment variant {i}: {simple_names[i]} */\n")
        p = os.path.join(SHADER_DIR, f"simple_v{i}.gxp")
        emit_array(f, f"gxp_simple_v{i}", p)
        total += os.path.getsize(p)

    f.write("/* Simple fragment variant lookup table */\n")
    f.write("static const unsigned char* gxp_simple_variants[8] = {\n")
    for i in range(8):
        f.write(f"    gxp_simple_v{i},\n")
    f.write("};\n")
    f.write("static const unsigned int gxp_simple_variant_sizes[8] = {\n")
    for i in range(8):
        f.write(f"    gxp_simple_v{i}_size,\n")
    f.write("};\n\n")

    # Specialized CFG0 fragment variants (77% of draws: tex*ras*reg1, 8 L/F/A)
    lfa_names = {0:"base", 1:"L", 2:"F", 3:"LF", 4:"A", 5:"LA", 6:"FA", 7:"LFA"}
    for i in range(8):
        f.write(f"/* CFG0 fragment variant {i}: {lfa_names[i]} */\n")
        p = os.path.join(SHADER_DIR, f"cfg0_v{i}.gxp")
        emit_array(f, f"gxp_cfg0_v{i}", p)
        total += os.path.getsize(p)

    f.write("/* CFG0 specialized variant lookup table */\n")
    f.write("static const unsigned char* gxp_cfg0_variants[8] = {\n")
    for i in range(8):
        f.write(f"    gxp_cfg0_v{i},\n")
    f.write("};\n")
    f.write("static const unsigned int gxp_cfg0_variant_sizes[8] = {\n")
    for i in range(8):
        f.write(f"    gxp_cfg0_v{i}_size,\n")
    f.write("};\n\n")
    f.write("#define VITA_HAS_CFG0 1\n\n")

    # Specialized CFG2 fragment variants (7.7% of draws: lerp(ras,tex,A0) then lerp(C2,C1,prev))
    for i in range(8):
        f.write(f"/* CFG2 fragment variant {i}: {lfa_names[i]} */\n")
        p = os.path.join(SHADER_DIR, f"cfg2_v{i}.gxp")
        emit_array(f, f"gxp_cfg2_v{i}", p)
        total += os.path.getsize(p)

    f.write("/* CFG2 specialized variant lookup table */\n")
    f.write("static const unsigned char* gxp_cfg2_variants[8] = {\n")
    for i in range(8):
        f.write(f"    gxp_cfg2_v{i},\n")
    f.write("};\n")
    f.write("static const unsigned int gxp_cfg2_variant_sizes[8] = {\n")
    for i in range(8):
        f.write(f"    gxp_cfg2_v{i}_size,\n")
    f.write("};\n\n")
    f.write("#define VITA_HAS_CFG2 1\n\n")

    # Specialized CFG3 fragment variants (4.5% of draws: color=C1, alpha=tex.a*A0)
    for i in range(8):
        f.write(f"/* CFG3 fragment variant {i}: {lfa_names[i]} */\n")
        p = os.path.join(SHADER_DIR, f"cfg3_v{i}.gxp")
        emit_array(f, f"gxp_cfg3_v{i}", p)
        total += os.path.getsize(p)

    f.write("/* CFG3 specialized variant lookup table */\n")
    f.write("static const unsigned char* gxp_cfg3_variants[8] = {\n")
    for i in range(8):
        f.write(f"    gxp_cfg3_v{i},\n")
    f.write("};\n")
    f.write("static const unsigned int gxp_cfg3_variant_sizes[8] = {\n")
    for i in range(8):
        f.write(f"    gxp_cfg3_v{i}_size,\n")
    f.write("};\n\n")
    f.write("#define VITA_HAS_CFG3 1\n\n")

    # Specialized CFG4 fragment variants (register passthrough + flexible alpha)
    cfg4_exists = os.path.exists(os.path.join(SHADER_DIR, "cfg4_v0.gxp"))
    if cfg4_exists:
        for i in range(8):
            f.write(f"/* CFG4 fragment variant {i}: {lfa_names[i]} */\n")
            p = os.path.join(SHADER_DIR, f"cfg4_v{i}.gxp")
            emit_array(f, f"gxp_cfg4_v{i}", p)
            total += os.path.getsize(p)

        f.write("/* CFG4 specialized variant lookup table */\n")
        f.write("static const unsigned char* gxp_cfg4_variants[8] = {\n")
        for i in range(8):
            f.write(f"    gxp_cfg4_v{i},\n")
        f.write("};\n")
        f.write("static const unsigned int gxp_cfg4_variant_sizes[8] = {\n")
        for i in range(8):
            f.write(f"    gxp_cfg4_v{i}_size,\n")
        f.write("};\n\n")
    f.write(f"#define VITA_HAS_CFG4 {1 if cfg4_exists else 0}\n\n")

    # Specialized CFG5 fragment variants (lerp(reg,reg,tex) + tex*register alpha)
    cfg5_exists = os.path.exists(os.path.join(SHADER_DIR, "cfg5_v0.gxp"))
    if cfg5_exists:
        for i in range(8):
            f.write(f"/* CFG5 fragment variant {i}: {lfa_names[i]} */\n")
            p = os.path.join(SHADER_DIR, f"cfg5_v{i}.gxp")
            emit_array(f, f"gxp_cfg5_v{i}", p)
            total += os.path.getsize(p)

        f.write("/* CFG5 specialized variant lookup table */\n")
        f.write("static const unsigned char* gxp_cfg5_variants[8] = {\n")
        for i in range(8):
            f.write(f"    gxp_cfg5_v{i},\n")
        f.write("};\n")
        f.write("static const unsigned int gxp_cfg5_variant_sizes[8] = {\n")
        for i in range(8):
            f.write(f"    gxp_cfg5_v{i}_size,\n")
        f.write("};\n\n")
    f.write(f"#define VITA_HAS_CFG5 {1 if cfg5_exists else 0}\n\n")

    # Specialized CFG6 fragment variants (2-stage register + TEXA chain)
    cfg6_exists = os.path.exists(os.path.join(SHADER_DIR, "cfg6_v0.gxp"))
    if cfg6_exists:
        for i in range(8):
            f.write(f"/* CFG6 fragment variant {i}: {lfa_names[i]} */\n")
            p = os.path.join(SHADER_DIR, f"cfg6_v{i}.gxp")
            emit_array(f, f"gxp_cfg6_v{i}", p)
            total += os.path.getsize(p)

        f.write("/* CFG6 specialized variant lookup table */\n")
        f.write("static const unsigned char* gxp_cfg6_variants[8] = {\n")
        for i in range(8):
            f.write(f"    gxp_cfg6_v{i},\n")
        f.write("};\n")
        f.write("static const unsigned int gxp_cfg6_variant_sizes[8] = {\n")
        for i in range(8):
            f.write(f"    gxp_cfg6_v{i}_size,\n")
        f.write("};\n\n")
    f.write(f"#define VITA_HAS_CFG6 {1 if cfg6_exists else 0}\n\n")

    # Specialized CFG7 fragment variants (ocean 2-stage surface)
    cfg7_exists = os.path.exists(os.path.join(SHADER_DIR, "cfg7_v0.gxp"))
    if cfg7_exists:
        for i in range(8):
            f.write(f"/* CFG7 fragment variant {i}: {lfa_names[i]} */\n")
            p = os.path.join(SHADER_DIR, f"cfg7_v{i}.gxp")
            emit_array(f, f"gxp_cfg7_v{i}", p)
            total += os.path.getsize(p)

        f.write("/* CFG7 specialized variant lookup table */\n")
        f.write("static const unsigned char* gxp_cfg7_variants[8] = {\n")
        for i in range(8):
            f.write(f"    gxp_cfg7_v{i},\n")
        f.write("};\n")
        f.write("static const unsigned int gxp_cfg7_variant_sizes[8] = {\n")
        for i in range(8):
            f.write(f"    gxp_cfg7_v{i}_size,\n")
        f.write("};\n\n")
    f.write(f"#define VITA_HAS_CFG7 {1 if cfg7_exists else 0}\n\n")

    # Specialized CFG8 fragment variants (ocean 1-stage edge)
    cfg8_exists = os.path.exists(os.path.join(SHADER_DIR, "cfg8_v0.gxp"))
    if cfg8_exists:
        for i in range(8):
            f.write(f"/* CFG8 fragment variant {i}: {lfa_names[i]} */\n")
            p = os.path.join(SHADER_DIR, f"cfg8_v{i}.gxp")
            emit_array(f, f"gxp_cfg8_v{i}", p)
            total += os.path.getsize(p)

        f.write("/* CFG8 specialized variant lookup table */\n")
        f.write("static const unsigned char* gxp_cfg8_variants[8] = {\n")
        for i in range(8):
            f.write(f"    gxp_cfg8_v{i},\n")
        f.write("};\n")
        f.write("static const unsigned int gxp_cfg8_variant_sizes[8] = {\n")
        for i in range(8):
            f.write(f"    gxp_cfg8_v{i}_size,\n")
        f.write("};\n\n")
    f.write(f"#define VITA_HAS_CFG8 {1 if cfg8_exists else 0}\n\n")

    # Specialized CFG9 fragment variants (D+B*ras color, TEXA*register alpha)
    cfg9_exists = os.path.exists(os.path.join(SHADER_DIR, "cfg9_v0.gxp"))
    if cfg9_exists:
        for i in range(8):
            f.write(f"/* CFG9 fragment variant {i}: {lfa_names[i]} */\n")
            p = os.path.join(SHADER_DIR, f"cfg9_v{i}.gxp")
            emit_array(f, f"gxp_cfg9_v{i}", p)
            total += os.path.getsize(p)

        f.write("/* CFG9 specialized variant lookup table */\n")
        f.write("static const unsigned char* gxp_cfg9_variants[8] = {\n")
        for i in range(8):
            f.write(f"    gxp_cfg9_v{i},\n")
        f.write("};\n")
        f.write("static const unsigned int gxp_cfg9_variant_sizes[8] = {\n")
        for i in range(8):
            f.write(f"    gxp_cfg9_v{i}_size,\n")
        f.write("};\n\n")
    f.write(f"#define VITA_HAS_CFG9 {1 if cfg9_exists else 0}\n\n")

    # Specialized CFG10 fragment variants (tex+ras*lerp(C2,C1,tex), TEXA*A0 alpha)
    cfg10_exists = os.path.exists(os.path.join(SHADER_DIR, "cfg10_v0.gxp"))
    if cfg10_exists:
        for i in range(8):
            f.write(f"/* CFG10 fragment variant {i}: {lfa_names[i]} */\n")
            p = os.path.join(SHADER_DIR, f"cfg10_v{i}.gxp")
            emit_array(f, f"gxp_cfg10_v{i}", p)
            total += os.path.getsize(p)

        f.write("/* CFG10 specialized variant lookup table */\n")
        f.write("static const unsigned char* gxp_cfg10_variants[8] = {\n")
        for i in range(8):
            f.write(f"    gxp_cfg10_v{i},\n")
        f.write("};\n")
        f.write("static const unsigned int gxp_cfg10_variant_sizes[8] = {\n")
        for i in range(8):
            f.write(f"    gxp_cfg10_v{i}_size,\n")
        f.write("};\n\n")
    f.write(f"#define VITA_HAS_CFG10 {1 if cfg10_exists else 0}\n\n")

    # Specialized CFG11 fragment variants (tex*ras color, TEXA*A1 alpha)
    cfg11_exists = os.path.exists(os.path.join(SHADER_DIR, "cfg11_v0.gxp"))
    if cfg11_exists:
        for i in range(8):
            f.write(f"/* CFG11 fragment variant {i}: {lfa_names[i]} */\n")
            p = os.path.join(SHADER_DIR, f"cfg11_v{i}.gxp")
            emit_array(f, f"gxp_cfg11_v{i}", p)
            total += os.path.getsize(p)

        f.write("/* CFG11 specialized variant lookup table */\n")
        f.write("static const unsigned char* gxp_cfg11_variants[8] = {\n")
        for i in range(8):
            f.write(f"    gxp_cfg11_v{i},\n")
        f.write("};\n")
        f.write("static const unsigned int gxp_cfg11_variant_sizes[8] = {\n")
        for i in range(8):
            f.write(f"    gxp_cfg11_v{i}_size,\n")
        f.write("};\n\n")
    f.write(f"#define VITA_HAS_CFG11 {1 if cfg11_exists else 0}\n\n")

    # Specialized CFG12 fragment variants (3-stage ocean A: tex0*(1+tex1)+C1*ras)
    cfg12_exists = os.path.exists(os.path.join(SHADER_DIR, "cfg12_v0.gxp"))
    if cfg12_exists:
        for i in range(8):
            f.write(f"/* CFG12 fragment variant {i}: {lfa_names[i]} */\n")
            p = os.path.join(SHADER_DIR, f"cfg12_v{i}.gxp")
            emit_array(f, f"gxp_cfg12_v{i}", p)
            total += os.path.getsize(p)

        f.write("/* CFG12 specialized variant lookup table */\n")
        f.write("static const unsigned char* gxp_cfg12_variants[8] = {\n")
        for i in range(8):
            f.write(f"    gxp_cfg12_v{i},\n")
        f.write("};\n")
        f.write("static const unsigned int gxp_cfg12_variant_sizes[8] = {\n")
        for i in range(8):
            f.write(f"    gxp_cfg12_v{i}_size,\n")
        f.write("};\n\n")
    f.write(f"#define VITA_HAS_CFG12 {1 if cfg12_exists else 0}\n\n")

    # Specialized CFG13 fragment variants (3-stage ocean B: tex0*tex1.a+C1*ras)
    cfg13_exists = os.path.exists(os.path.join(SHADER_DIR, "cfg13_v0.gxp"))
    if cfg13_exists:
        for i in range(8):
            f.write(f"/* CFG13 fragment variant {i}: {lfa_names[i]} */\n")
            p = os.path.join(SHADER_DIR, f"cfg13_v{i}.gxp")
            emit_array(f, f"gxp_cfg13_v{i}", p)
            total += os.path.getsize(p)

        f.write("/* CFG13 specialized variant lookup table */\n")
        f.write("static const unsigned char* gxp_cfg13_variants[8] = {\n")
        for i in range(8):
            f.write(f"    gxp_cfg13_v{i},\n")
        f.write("};\n")
        f.write("static const unsigned int gxp_cfg13_variant_sizes[8] = {\n")
        for i in range(8):
            f.write(f"    gxp_cfg13_v{i}_size,\n")
        f.write("};\n\n")
    f.write(f"#define VITA_HAS_CFG13 {1 if cfg13_exists else 0}\n\n")

    # Specialized CFG14 fragment variants (tex*ras color + register alpha D passthrough)
    cfg14_exists = os.path.exists(os.path.join(SHADER_DIR, "cfg14_v0.gxp"))
    if cfg14_exists:
        for i in range(8):
            f.write(f"/* CFG14 fragment variant {i}: {lfa_names[i]} */\n")
            p = os.path.join(SHADER_DIR, f"cfg14_v{i}.gxp")
            emit_array(f, f"gxp_cfg14_v{i}", p)
            total += os.path.getsize(p)

        f.write("/* CFG14 specialized variant lookup table */\n")
        f.write("static const unsigned char* gxp_cfg14_variants[8] = {\n")
        for i in range(8):
            f.write(f"    gxp_cfg14_v{i},\n")
        f.write("};\n")
        f.write("static const unsigned int gxp_cfg14_variant_sizes[8] = {\n")
        for i in range(8):
            f.write(f"    gxp_cfg14_v{i}_size,\n")
        f.write("};\n\n")
    f.write(f"#define VITA_HAS_CFG14 {1 if cfg14_exists else 0}\n\n")

    # Specialized CFG15 fragment variants (lerp(reg,reg,tex) + TEXA chain alpha)
    cfg15_exists = os.path.exists(os.path.join(SHADER_DIR, "cfg15_v0.gxp"))
    if cfg15_exists:
        for i in range(8):
            f.write(f"/* CFG15 fragment variant {i}: {lfa_names[i]} */\n")
            p = os.path.join(SHADER_DIR, f"cfg15_v{i}.gxp")
            emit_array(f, f"gxp_cfg15_v{i}", p)
            total += os.path.getsize(p)

        f.write("/* CFG15 specialized variant lookup table */\n")
        f.write("static const unsigned char* gxp_cfg15_variants[8] = {\n")
        for i in range(8):
            f.write(f"    gxp_cfg15_v{i},\n")
        f.write("};\n")
        f.write("static const unsigned int gxp_cfg15_variant_sizes[8] = {\n")
        for i in range(8):
            f.write(f"    gxp_cfg15_v{i}_size,\n")
        f.write("};\n\n")
    f.write(f"#define VITA_HAS_CFG15 {1 if cfg15_exists else 0}\n\n")

    # Specialized CFG16 fragment variants (register color + reg*TEXA chain alpha)
    cfg16_exists = os.path.exists(os.path.join(SHADER_DIR, "cfg16_v0.gxp"))
    if cfg16_exists:
        for i in range(8):
            f.write(f"/* CFG16 fragment variant {i}: {lfa_names[i]} */\n")
            p = os.path.join(SHADER_DIR, f"cfg16_v{i}.gxp")
            emit_array(f, f"gxp_cfg16_v{i}", p)
            total += os.path.getsize(p)

        f.write("/* CFG16 specialized variant lookup table */\n")
        f.write("static const unsigned char* gxp_cfg16_variants[8] = {\n")
        for i in range(8):
            f.write(f"    gxp_cfg16_v{i},\n")
        f.write("};\n")
        f.write("static const unsigned int gxp_cfg16_variant_sizes[8] = {\n")
        for i in range(8):
            f.write(f"    gxp_cfg16_v{i}_size,\n")
        f.write("};\n\n")
    f.write(f"#define VITA_HAS_CFG16 {1 if cfg16_exists else 0}\n\n")

    # Specialized CFG17 fragment variants (lerp(reg,reg,tex) + TEXA passthrough)
    cfg17_exists = os.path.exists(os.path.join(SHADER_DIR, "cfg17_v0.gxp"))
    if cfg17_exists:
        for i in range(8):
            f.write(f"/* CFG17 fragment variant {i}: {lfa_names[i]} */\n")
            p = os.path.join(SHADER_DIR, f"cfg17_v{i}.gxp")
            emit_array(f, f"gxp_cfg17_v{i}", p)
            total += os.path.getsize(p)
        f.write("static const unsigned char* gxp_cfg17_variants[8] = {\n")
        for i in range(8):
            f.write(f"    gxp_cfg17_v{i},\n")
        f.write("};\nstatic const unsigned int gxp_cfg17_variant_sizes[8] = {\n")
        for i in range(8):
            f.write(f"    gxp_cfg17_v{i}_size,\n")
        f.write("};\n\n")
    f.write(f"#define VITA_HAS_CFG17 {1 if cfg17_exists else 0}\n\n")

    # Specialized CFG18 fragment variants (register*tex + TEXA passthrough)
    cfg18_exists = os.path.exists(os.path.join(SHADER_DIR, "cfg18_v0.gxp"))
    if cfg18_exists:
        for i in range(8):
            f.write(f"/* CFG18 fragment variant {i}: {lfa_names[i]} */\n")
            p = os.path.join(SHADER_DIR, f"cfg18_v{i}.gxp")
            emit_array(f, f"gxp_cfg18_v{i}", p)
            total += os.path.getsize(p)
        f.write("static const unsigned char* gxp_cfg18_variants[8] = {\n")
        for i in range(8):
            f.write(f"    gxp_cfg18_v{i},\n")
        f.write("};\nstatic const unsigned int gxp_cfg18_variant_sizes[8] = {\n")
        for i in range(8):
            f.write(f"    gxp_cfg18_v{i}_size,\n")
        f.write("};\n\n")
    f.write(f"#define VITA_HAS_CFG18 {1 if cfg18_exists else 0}\n\n")

    # Specialized CFG19 fragment variants (reg+reg*reg color + reg*TEXA alpha)
    cfg19_exists = os.path.exists(os.path.join(SHADER_DIR, "cfg19_v0.gxp"))
    if cfg19_exists:
        for i in range(8):
            f.write(f"/* CFG19 fragment variant {i}: {lfa_names[i]} */\n")
            p = os.path.join(SHADER_DIR, f"cfg19_v{i}.gxp")
            emit_array(f, f"gxp_cfg19_v{i}", p)
            total += os.path.getsize(p)
        f.write("static const unsigned char* gxp_cfg19_variants[8] = {\n")
        for i in range(8):
            f.write(f"    gxp_cfg19_v{i},\n")
        f.write("};\nstatic const unsigned int gxp_cfg19_variant_sizes[8] = {\n")
        for i in range(8):
            f.write(f"    gxp_cfg19_v{i}_size,\n")
        f.write("};\n\n")
    f.write(f"#define VITA_HAS_CFG19 {1 if cfg19_exists else 0}\n\n")

    # Specialized CFG20 fragment variants (reg*(1+tex) color + TEXA alpha)
    cfg20_exists = os.path.exists(os.path.join(SHADER_DIR, "cfg20_v0.gxp"))
    if cfg20_exists:
        for i in range(8):
            f.write(f"/* CFG20 fragment variant {i}: {lfa_names[i]} */\n")
            p = os.path.join(SHADER_DIR, f"cfg20_v{i}.gxp")
            emit_array(f, f"gxp_cfg20_v{i}", p)
            total += os.path.getsize(p)
        f.write("static const unsigned char* gxp_cfg20_variants[8] = {\n")
        for i in range(8):
            f.write(f"    gxp_cfg20_v{i},\n")
        f.write("};\nstatic const unsigned int gxp_cfg20_variant_sizes[8] = {\n")
        for i in range(8):
            f.write(f"    gxp_cfg20_v{i}_size,\n")
        f.write("};\n\n")
    f.write(f"#define VITA_HAS_CFG20 {1 if cfg20_exists else 0}\n\n")

    # Specialized CFG21 fragment variants (reg color A-slot + TEXA*reg*TEXA alpha)
    cfg21_exists = os.path.exists(os.path.join(SHADER_DIR, "cfg21_v0.gxp"))
    if cfg21_exists:
        for i in range(8):
            f.write(f"/* CFG21 fragment variant {i}: {lfa_names[i]} */\n")
            p = os.path.join(SHADER_DIR, f"cfg21_v{i}.gxp")
            emit_array(f, f"gxp_cfg21_v{i}", p)
            total += os.path.getsize(p)
        f.write("static const unsigned char* gxp_cfg21_variants[8] = {\n")
        for i in range(8):
            f.write(f"    gxp_cfg21_v{i},\n")
        f.write("};\nstatic const unsigned int gxp_cfg21_variant_sizes[8] = {\n")
        for i in range(8):
            f.write(f"    gxp_cfg21_v{i}_size,\n")
        f.write("};\n\n")
    f.write(f"#define VITA_HAS_CFG21 {1 if cfg21_exists else 0}\n\n")

    # Specialized CFG22 fragment variants (tex*register color + TEXA alpha)
    cfg22_exists = os.path.exists(os.path.join(SHADER_DIR, "cfg22_v0.gxp"))
    if cfg22_exists:
        for i in range(8):
            f.write(f"/* CFG22 fragment variant {i}: {lfa_names[i]} */\n")
            p = os.path.join(SHADER_DIR, f"cfg22_v{i}.gxp")
            emit_array(f, f"gxp_cfg22_v{i}", p)
            total += os.path.getsize(p)
        f.write("static const unsigned char* gxp_cfg22_variants[8] = {\n")
        for i in range(8):
            f.write(f"    gxp_cfg22_v{i},\n")
        f.write("};\nstatic const unsigned int gxp_cfg22_variant_sizes[8] = {\n")
        for i in range(8):
            f.write(f"    gxp_cfg22_v{i}_size,\n")
        f.write("};\n\n")
    f.write(f"#define VITA_HAS_CFG22 {1 if cfg22_exists else 0}\n\n")

    # Specialized CFG23 fragment variants (tex0 color + tex1 alpha composite)
    cfg23_exists = os.path.exists(os.path.join(SHADER_DIR, "cfg23_v0.gxp"))
    if cfg23_exists:
        for i in range(8):
            f.write(f"/* CFG23 fragment variant {i}: {lfa_names[i]} */\n")
            p = os.path.join(SHADER_DIR, f"cfg23_v{i}.gxp")
            emit_array(f, f"gxp_cfg23_v{i}", p)
            total += os.path.getsize(p)
        f.write("static const unsigned char* gxp_cfg23_variants[8] = {\n")
        for i in range(8):
            f.write(f"    gxp_cfg23_v{i},\n")
        f.write("};\nstatic const unsigned int gxp_cfg23_variant_sizes[8] = {\n")
        for i in range(8):
            f.write(f"    gxp_cfg23_v{i}_size,\n")
        f.write("};\n\n")
    f.write(f"#define VITA_HAS_CFG23 {1 if cfg23_exists else 0}\n\n")

    # Specialized CFG24 fragment variants (3-stage waterfall)
    cfg24_exists = os.path.exists(os.path.join(SHADER_DIR, "cfg24_v0.gxp"))
    if cfg24_exists:
        for i in range(8):
            f.write(f"/* CFG24 fragment variant {i}: {lfa_names[i]} */\n")
            p = os.path.join(SHADER_DIR, f"cfg24_v{i}.gxp")
            emit_array(f, f"gxp_cfg24_v{i}", p)
            total += os.path.getsize(p)
        f.write("static const unsigned char* gxp_cfg24_variants[8] = {\n")
        for i in range(8):
            f.write(f"    gxp_cfg24_v{i},\n")
        f.write("};\nstatic const unsigned int gxp_cfg24_variant_sizes[8] = {\n")
        for i in range(8):
            f.write(f"    gxp_cfg24_v{i}_size,\n")
        f.write("};\n\n")
    f.write(f"#define VITA_HAS_CFG24 {1 if cfg24_exists else 0}\n\n")

    # Specialized CFG25 fragment variants (3-stage tex*TEXA blend + register lerp)
    cfg25_exists = os.path.exists(os.path.join(SHADER_DIR, "cfg25_v0.gxp"))
    if cfg25_exists:
        for i in range(8):
            f.write(f"/* CFG25 fragment variant {i}: {lfa_names[i]} */\n")
            p = os.path.join(SHADER_DIR, f"cfg25_v{i}.gxp")
            emit_array(f, f"gxp_cfg25_v{i}", p)
            total += os.path.getsize(p)
        f.write("static const unsigned char* gxp_cfg25_variants[8] = {\n")
        for i in range(8):
            f.write(f"    gxp_cfg25_v{i},\n")
        f.write("};\nstatic const unsigned int gxp_cfg25_variant_sizes[8] = {\n")
        for i in range(8):
            f.write(f"    gxp_cfg25_v{i}_size,\n")
        f.write("};\n\n")
    f.write(f"#define VITA_HAS_CFG25 {1 if cfg25_exists else 0}\n\n")

    # Specialized CFG26 fragment variants (register color + RASA*A1 alpha)
    cfg26_exists = os.path.exists(os.path.join(SHADER_DIR, "cfg26_v0.gxp"))
    if cfg26_exists:
        for i in range(8):
            f.write(f"/* CFG26 fragment variant {i}: {lfa_names[i]} */\n")
            p = os.path.join(SHADER_DIR, f"cfg26_v{i}.gxp")
            emit_array(f, f"gxp_cfg26_v{i}", p)
            total += os.path.getsize(p)
        f.write("static const unsigned char* gxp_cfg26_variants[8] = {\n")
        for i in range(8):
            f.write(f"    gxp_cfg26_v{i},\n")
        f.write("};\nstatic const unsigned int gxp_cfg26_variant_sizes[8] = {\n")
        for i in range(8):
            f.write(f"    gxp_cfg26_v{i}_size,\n")
        f.write("};\n\n")
    f.write(f"#define VITA_HAS_CFG26 {1 if cfg26_exists else 0}\n\n")

    # Specialized CFG27 fragment variants (tex*ras*TEXA_clr color + A1+A0*TEXA alpha)
    cfg27_exists = os.path.exists(os.path.join(SHADER_DIR, "cfg27_v0.gxp"))
    if cfg27_exists:
        for i in range(8):
            f.write(f"/* CFG27 fragment variant {i}: {lfa_names[i]} */\n")
            p = os.path.join(SHADER_DIR, f"cfg27_v{i}.gxp")
            emit_array(f, f"gxp_cfg27_v{i}", p)
            total += os.path.getsize(p)
        f.write("static const unsigned char* gxp_cfg27_variants[8] = {\n")
        for i in range(8):
            f.write(f"    gxp_cfg27_v{i},\n")
        f.write("};\nstatic const unsigned int gxp_cfg27_variant_sizes[8] = {\n")
        for i in range(8):
            f.write(f"    gxp_cfg27_v{i}_size,\n")
        f.write("};\n\n")
    f.write(f"#define VITA_HAS_CFG27 {1 if cfg27_exists else 0}\n\n")

    # Specialized CFG28 fragment variants (3-stage waterfall foam: TEXA_clr color + A1*lerp alpha)
    cfg28_exists = os.path.exists(os.path.join(SHADER_DIR, "cfg28_v0.gxp"))
    if cfg28_exists:
        for i in range(8):
            f.write(f"/* CFG28 fragment variant {i}: {lfa_names[i]} */\n")
            p = os.path.join(SHADER_DIR, f"cfg28_v{i}.gxp")
            emit_array(f, f"gxp_cfg28_v{i}", p)
            total += os.path.getsize(p)
        f.write("static const unsigned char* gxp_cfg28_variants[8] = {\n")
        for i in range(8):
            f.write(f"    gxp_cfg28_v{i},\n")
        f.write("};\nstatic const unsigned int gxp_cfg28_variant_sizes[8] = {\n")
        for i in range(8):
            f.write(f"    gxp_cfg28_v{i}_size,\n")
        f.write("};\n\n")
    f.write(f"#define VITA_HAS_CFG28 {1 if cfg28_exists else 0}\n\n")

    # Specialized CFG29 fragment variants (white color saturated additive, tex1a*tex0a alpha)
    cfg29_exists = os.path.exists(os.path.join(SHADER_DIR, "cfg29_v0.gxp"))
    if cfg29_exists:
        for i in range(8):
            f.write(f"/* CFG29 fragment variant {i}: {lfa_names[i]} */\n")
            p = os.path.join(SHADER_DIR, f"cfg29_v{i}.gxp")
            emit_array(f, f"gxp_cfg29_v{i}", p)
            total += os.path.getsize(p)
        f.write("static const unsigned char* gxp_cfg29_variants[8] = {\n")
        for i in range(8):
            f.write(f"    gxp_cfg29_v{i},\n")
        f.write("};\nstatic const unsigned int gxp_cfg29_variant_sizes[8] = {\n")
        for i in range(8):
            f.write(f"    gxp_cfg29_v{i}_size,\n")
        f.write("};\n\n")
    f.write(f"#define VITA_HAS_CFG29 {1 if cfg29_exists else 0}\n\n")

    # Specialized CFG30 fragment variants (tex1+ras*PREV color, tex1a*A0*tex0a alpha)
    cfg30_exists = os.path.exists(os.path.join(SHADER_DIR, "cfg30_v0.gxp"))
    if cfg30_exists:
        for i in range(8):
            f.write(f"/* CFG30 fragment variant {i}: {lfa_names[i]} */\n")
            p = os.path.join(SHADER_DIR, f"cfg30_v{i}.gxp")
            emit_array(f, f"gxp_cfg30_v{i}", p)
            total += os.path.getsize(p)
        f.write("static const unsigned char* gxp_cfg30_variants[8] = {\n")
        for i in range(8):
            f.write(f"    gxp_cfg30_v{i},\n")
        f.write("};\nstatic const unsigned int gxp_cfg30_variant_sizes[8] = {\n")
        for i in range(8):
            f.write(f"    gxp_cfg30_v{i}_size,\n")
        f.write("};\n\n")
    f.write(f"#define VITA_HAS_CFG30 {1 if cfg30_exists else 0}\n\n")

    # Specialized CFG31 fragment variants (white color saturated, tex1a*(1+tex0a) alpha)
    cfg31_exists = os.path.exists(os.path.join(SHADER_DIR, "cfg31_v0.gxp"))
    if cfg31_exists:
        for i in range(8):
            f.write(f"/* CFG31 fragment variant {i}: {lfa_names[i]} */\n")
            p = os.path.join(SHADER_DIR, f"cfg31_v{i}.gxp")
            emit_array(f, f"gxp_cfg31_v{i}", p)
            total += os.path.getsize(p)
        f.write("static const unsigned char* gxp_cfg31_variants[8] = {\n")
        for i in range(8):
            f.write(f"    gxp_cfg31_v{i},\n")
        f.write("};\nstatic const unsigned int gxp_cfg31_variant_sizes[8] = {\n")
        for i in range(8):
            f.write(f"    gxp_cfg31_v{i}_size,\n")
        f.write("};\n\n")
    f.write(f"#define VITA_HAS_CFG31 {1 if cfg31_exists else 0}\n\n")

    # Specialized CFG32 fragment variants (TEXC*(1+RASC)+TEXA_clr*RASC color, tex1a*tex0a alpha)
    cfg32_exists = os.path.exists(os.path.join(SHADER_DIR, "cfg32_v0.gxp"))
    if cfg32_exists:
        for i in range(8):
            f.write(f"/* CFG32 fragment variant {i}: {lfa_names[i]} */\n")
            p = os.path.join(SHADER_DIR, f"cfg32_v{i}.gxp")
            emit_array(f, f"gxp_cfg32_v{i}", p)
            total += os.path.getsize(p)
        f.write("static const unsigned char* gxp_cfg32_variants[8] = {\n")
        for i in range(8):
            f.write(f"    gxp_cfg32_v{i},\n")
        f.write("};\nstatic const unsigned int gxp_cfg32_variant_sizes[8] = {\n")
        for i in range(8):
            f.write(f"    gxp_cfg32_v{i}_size,\n")
        f.write("};\n\n")
    f.write(f"#define VITA_HAS_CFG32 {1 if cfg32_exists else 0}\n\n")

    # Specialized CFG33 fragment variants (lerp(TEXA_clr,TEXC,A2) color + TEXA alpha, 1-stage)
    cfg33_exists = os.path.exists(os.path.join(SHADER_DIR, "cfg33_v0.gxp"))
    if cfg33_exists:
        for i in range(8):
            f.write(f"/* CFG33 fragment variant {i}: {lfa_names[i]} */\n")
            p = os.path.join(SHADER_DIR, f"cfg33_v{i}.gxp")
            emit_array(f, f"gxp_cfg33_v{i}", p)
            total += os.path.getsize(p)
        f.write("static const unsigned char* gxp_cfg33_variants[8] = {\n")
        for i in range(8):
            f.write(f"    gxp_cfg33_v{i},\n")
        f.write("};\nstatic const unsigned int gxp_cfg33_variant_sizes[8] = {\n")
        for i in range(8):
            f.write(f"    gxp_cfg33_v{i}_size,\n")
        f.write("};\n\n")
    f.write(f"#define VITA_HAS_CFG33 {1 if cfg33_exists else 0}\n\n")

    # Specialized CFG34 fragment variants (TEXA_clr color + A1 register alpha, was old CFG0, 650+ hits)
    cfg34_exists = os.path.exists(os.path.join(SHADER_DIR, "cfg34_v0.gxp"))
    if cfg34_exists:
        for i in range(8):
            f.write(f"/* CFG34 fragment variant {i}: {lfa_names[i]} */\n")
            p = os.path.join(SHADER_DIR, f"cfg34_v{i}.gxp")
            emit_array(f, f"gxp_cfg34_v{i}", p)
            total += os.path.getsize(p)
        f.write("static const unsigned char* gxp_cfg34_variants[8] = {\n")
        for i in range(8):
            f.write(f"    gxp_cfg34_v{i},\n")
        f.write("};\nstatic const unsigned int gxp_cfg34_variant_sizes[8] = {\n")
        for i in range(8):
            f.write(f"    gxp_cfg34_v{i}_size,\n")
        f.write("};\n\n")
    f.write(f"#define VITA_HAS_CFG34 {1 if cfg34_exists else 0}\n\n")

    # Specialized CFG35 fragment variants (TEXC*RASC*TEXA_clr color + A1*TEXA alpha, was old CFG1, 650+ hits)
    cfg35_exists = os.path.exists(os.path.join(SHADER_DIR, "cfg35_v0.gxp"))
    if cfg35_exists:
        for i in range(8):
            f.write(f"/* CFG35 fragment variant {i}: {lfa_names[i]} */\n")
            p = os.path.join(SHADER_DIR, f"cfg35_v{i}.gxp")
            emit_array(f, f"gxp_cfg35_v{i}", p)
            total += os.path.getsize(p)
        f.write("static const unsigned char* gxp_cfg35_variants[8] = {\n")
        for i in range(8):
            f.write(f"    gxp_cfg35_v{i},\n")
        f.write("};\nstatic const unsigned int gxp_cfg35_variant_sizes[8] = {\n")
        for i in range(8):
            f.write(f"    gxp_cfg35_v{i}_size,\n")
        f.write("};\n\n")
    f.write(f"#define VITA_HAS_CFG35 {1 if cfg35_exists else 0}\n\n")

    # Specialized CFG36 fragment variants ((KONST*TEX)*RAS island ocean, 2-stage)
    cfg36_exists = os.path.exists(os.path.join(SHADER_DIR, "cfg36_v0.gxp"))
    if cfg36_exists:
        for i in range(8):
            f.write(f"/* CFG36 fragment variant {i}: {lfa_names[i]} */\n")
            p = os.path.join(SHADER_DIR, f"cfg36_v{i}.gxp")
            emit_array(f, f"gxp_cfg36_v{i}", p)
            total += os.path.getsize(p)
        f.write("static const unsigned char* gxp_cfg36_variants[8] = {\n")
        for i in range(8):
            f.write(f"    gxp_cfg36_v{i},\n")
        f.write("};\nstatic const unsigned int gxp_cfg36_variant_sizes[8] = {\n")
        for i in range(8):
            f.write(f"    gxp_cfg36_v{i}_size,\n")
        f.write("};\n\n")
    f.write(f"#define VITA_HAS_CFG36 {1 if cfg36_exists else 0}\n\n")

    # Specialized CFG37 fragment variants (TEX*RAS + TEXA alpha, island terrain, 1-stage)
    cfg37_exists = os.path.exists(os.path.join(SHADER_DIR, "cfg37_v0.gxp"))
    if cfg37_exists:
        for i in range(8):
            f.write(f"/* CFG37 fragment variant {i}: {lfa_names[i]} */\n")
            p = os.path.join(SHADER_DIR, f"cfg37_v{i}.gxp")
            emit_array(f, f"gxp_cfg37_v{i}", p)
            total += os.path.getsize(p)
        f.write("static const unsigned char* gxp_cfg37_variants[8] = {\n")
        for i in range(8):
            f.write(f"    gxp_cfg37_v{i},\n")
        f.write("};\nstatic const unsigned int gxp_cfg37_variant_sizes[8] = {\n")
        for i in range(8):
            f.write(f"    gxp_cfg37_v{i}_size,\n")
        f.write("};\n\n")
    f.write(f"#define VITA_HAS_CFG37 {1 if cfg37_exists else 0}\n\n")

    # Specialized CFG38 fragment variants (TEX*(1+RAS)+RAS*KONST, island 2-stage)
    cfg38_exists = os.path.exists(os.path.join(SHADER_DIR, "cfg38_v0.gxp"))
    if cfg38_exists:
        for i in range(8):
            f.write(f"/* CFG38 fragment variant {i}: {lfa_names[i]} */\n")
            p = os.path.join(SHADER_DIR, f"cfg38_v{i}.gxp")
            emit_array(f, f"gxp_cfg38_v{i}", p)
            total += os.path.getsize(p)
        f.write("static const unsigned char* gxp_cfg38_variants[8] = {\n")
        for i in range(8):
            f.write(f"    gxp_cfg38_v{i},\n")
        f.write("};\nstatic const unsigned int gxp_cfg38_variant_sizes[8] = {\n")
        for i in range(8):
            f.write(f"    gxp_cfg38_v{i}_size,\n")
        f.write("};\n\n")
    f.write(f"#define VITA_HAS_CFG38 {1 if cfg38_exists else 0}\n\n")

    # Specialized CFG39 fragment variants (lerp(C2,C1,TEX) + TEXA chain, island 2-stage)
    cfg39_exists = os.path.exists(os.path.join(SHADER_DIR, "cfg39_v0.gxp"))
    if cfg39_exists:
        for i in range(8):
            f.write(f"/* CFG39 fragment variant {i}: {lfa_names[i]} */\n")
            p = os.path.join(SHADER_DIR, f"cfg39_v{i}.gxp")
            emit_array(f, f"gxp_cfg39_v{i}", p)
            total += os.path.getsize(p)
        f.write("static const unsigned char* gxp_cfg39_variants[8] = {\n")
        for i in range(8):
            f.write(f"    gxp_cfg39_v{i},\n")
        f.write("};\nstatic const unsigned int gxp_cfg39_variant_sizes[8] = {\n")
        for i in range(8):
            f.write(f"    gxp_cfg39_v{i}_size,\n")
        f.write("};\n\n")
    f.write(f"#define VITA_HAS_CFG39 {1 if cfg39_exists else 0}\n\n")

    # Composite shader for depth-aware water FBO compositing
    comp_v_exists = os.path.exists(os.path.join(SHADER_DIR, "composite_v.gxp"))
    comp_f_exists = os.path.exists(os.path.join(SHADER_DIR, "composite_f.gxp"))
    if comp_v_exists and comp_f_exists:
        f.write("/* Composite shader for depth-aware water FBO compositing */\n")
        p = os.path.join(SHADER_DIR, "composite_v.gxp")
        emit_array(f, "gxp_composite_vert", p)
        total += os.path.getsize(p)
        p = os.path.join(SHADER_DIR, "composite_f.gxp")
        emit_array(f, "gxp_composite_frag", p)
        total += os.path.getsize(p)
    f.write(f"#define VITA_HAS_COMPOSITE {1 if (comp_v_exists and comp_f_exists) else 0}\n\n")

    f.write("#endif /* VITA_GXP_SHADERS_H */\n")

print(f"Generated {OUT_FILE}")
print(f"Total GXP data: {total} bytes ({total/1024:.1f} KB)")
