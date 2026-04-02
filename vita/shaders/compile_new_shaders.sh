#!/bin/bash
# Compile ALL CG shaders to GXP binaries.
# Run from vita/shaders/ directory with psp2cgc in PATH.
# psp2cgc is part of the Vita SDK ($VITASDK/bin/psp2cgc)
set -e

FP_FLAGS="-profile sce_fp_psp2"
VP_FLAGS="-profile sce_vp_psp2"

# Vertex shader (single variant — lighting controlled by u_vs_lighting uniform)
echo "Compiling default_v.gxp"
psp2cgc $VP_FLAGS -o default_v.gxp default_v.cg

# Complex fragment shader: 16 variants (LIGHTING|FOG|ALPHA_TEST|TEV2)
for i in $(seq 0 15); do
    DEFS=""
    [ $((i & 1)) -ne 0 ] && DEFS="$DEFS -DLIGHTING"
    [ $((i & 2)) -ne 0 ] && DEFS="$DEFS -DFOG"
    [ $((i & 4)) -ne 0 ] && DEFS="$DEFS -DALPHA_TEST"
    [ $((i & 8)) -ne 0 ] && DEFS="$DEFS -DTEV2"
    echo "Compiling frag_v${i}.gxp (complex, flags=$i $DEFS)"
    psp2cgc $FP_FLAGS $DEFS -o frag_v${i}.gxp default_f.cg
done

# Helper: compile 8 L/F/A variants for a given source file and output prefix
compile_lfa() {
    local src=$1 prefix=$2 label=$3
    for i in 0 1 2 3 4 5 6 7; do
        DEFS=""
        [ $((i & 1)) -ne 0 ] && DEFS="$DEFS -DLIGHTING"
        [ $((i & 2)) -ne 0 ] && DEFS="$DEFS -DFOG"
        [ $((i & 4)) -ne 0 ] && DEFS="$DEFS -DALPHA_TEST"
        echo "Compiling ${prefix}_v${i}.gxp (${label}, flags=$i $DEFS)"
        psp2cgc $FP_FLAGS $DEFS -o ${prefix}_v${i}.gxp ${src}
    done
}

compile_lfa simple_f.cg simple "simple tex*ras"
compile_lfa cfg0_f.cg cfg0 "CFG0 77%: tex*ras*reg1"
compile_lfa cfg2_f.cg cfg2 "CFG2 7.7%: lerp(ras,tex,A0)+lerp(C2,C1,prev)"
compile_lfa cfg3_f.cg cfg3 "CFG3 4.5%: color=C1, alpha=tex.a*A0"
compile_lfa cfg4_f.cg cfg4 "CFG4: register+flexible alpha"
compile_lfa cfg5_f.cg cfg5 "CFG5: lerp(reg,reg,tex)+tex*reg alpha"
compile_lfa cfg6_f.cg cfg6 "CFG6: 2-stage register+TEXA chain"
compile_lfa cfg7_f.cg cfg7 "CFG7: ocean 2-stage surface"
compile_lfa cfg8_f.cg cfg8 "CFG8: ocean 1-stage edge"
compile_lfa cfg9_f.cg cfg9 "CFG9: D+B*ras color, TEXA*reg alpha"
compile_lfa cfg10_f.cg cfg10 "CFG10: tex+ras*lerp(C2,C1,tex), TEXA*A0 alpha"
compile_lfa cfg11_f.cg cfg11 "CFG11: tex*ras color, TEXA*A1 alpha"
compile_lfa cfg12_f.cg cfg12 "CFG12: 3-stage ocean A: tex0*(1+tex1)+C1*ras"
compile_lfa cfg13_f.cg cfg13 "CFG13: 3-stage ocean B: tex0*tex1.a+C1*ras"
compile_lfa cfg14_f.cg cfg14 "CFG14: tex*ras color + register alpha D"
compile_lfa cfg15_f.cg cfg15 "CFG15: lerp(reg,reg,tex) + TEXA chain alpha"
compile_lfa cfg16_f.cg cfg16 "CFG16: register color + reg*TEXA chain alpha"
compile_lfa cfg17_f.cg cfg17 "CFG17: lerp(reg,reg,tex) + TEXA passthrough"
compile_lfa cfg18_f.cg cfg18 "CFG18: register*tex + TEXA passthrough"
compile_lfa cfg19_f.cg cfg19 "CFG19: reg+reg*reg color + reg*TEXA alpha"
compile_lfa cfg20_f.cg cfg20 "CFG20: reg*(1+tex) color + TEXA alpha"
compile_lfa cfg21_f.cg cfg21 "CFG21: reg color(A-slot) + TEXA*reg*TEXA alpha"
compile_lfa cfg22_f.cg cfg22 "CFG22: tex*register color + TEXA alpha"
compile_lfa cfg23_f.cg cfg23 "CFG23: tex0 color + tex1 alpha composite"
compile_lfa cfg24_f.cg cfg24 "CFG24: 3-stage waterfall C1*ras + tex blend alpha"
compile_lfa cfg25_f.cg cfg25 "CFG25: 3-stage tex*TEXA blend + register lerp"
compile_lfa cfg26_f.cg cfg26 "CFG26: register color + RASA*A1 alpha"
compile_lfa cfg27_f.cg cfg27 "CFG27: tex*ras*TEXA_clr color + A1+A0*TEXA alpha"
compile_lfa cfg28_f.cg cfg28 "CFG28: 3-stage waterfall foam: TEXA_clr color + A1*lerp(tex0a,tex1a,A0) alpha"
compile_lfa cfg29_f.cg cfg29 "CFG29: white color (saturated additive), tex1a*tex0a alpha"
compile_lfa cfg30_f.cg cfg30 "CFG30: tex1+ras*(TEXA_clr+TEXC) color + tex1a*A0*tex0a alpha"
compile_lfa cfg31_f.cg cfg31 "CFG31: white color (saturated), tex1a*(1+tex0a) alpha"
compile_lfa cfg32_f.cg cfg32 "CFG32: TEXC*(1+RASC)+TEXA*RASC color + tex1a*tex0a alpha"
compile_lfa cfg33_f.cg cfg33 "CFG33: lerp(TEXA_clr,TEXC,A2) color + TEXA alpha (1-stage)"
compile_lfa cfg34_f.cg cfg34 "CFG34: TEXA_clr color + A1 register alpha (was old CFG0, 650+ hits)"
compile_lfa cfg35_f.cg cfg35 "CFG35: TEXC*RASC*TEXA_clr color + A1*TEXA alpha (was old CFG1, 650+ hits)"
compile_lfa cfg36_f.cg cfg36 "CFG36: (KONST*TEX)*RAS island ocean (2-stage)"
compile_lfa cfg37_f.cg cfg37 "CFG37: TEX*RAS + TEXA alpha (island terrain, 1-stage)"
compile_lfa cfg38_f.cg cfg38 "CFG38: TEX*(1+RAS)+RAS*KONST color + TEXA*A1 alpha (island, 2-stage)"
compile_lfa cfg39_f.cg cfg39 "CFG39: lerp(C2,C1,TEX) + TEXA*APREV alpha chain (island, 2-stage)"

# Composite shader for depth-aware water FBO compositing (single variant each)
echo "Compiling composite_v.gxp"
psp2cgc $VP_FLAGS -o composite_v.gxp composite_v.cg
echo "Compiling composite_f.gxp"
psp2cgc $FP_FLAGS -o composite_f.gxp composite_f.cg

echo ""
echo "Done. Run: python3 gen_header.py"
