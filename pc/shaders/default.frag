/* GLSL 120 fragment shader — CPU-resolved branchless TEV combiner.
 * ALL TEV input selection done on CPU. Shader uses mix/step only.
 * Compile variants: LIGHTING, FOG, ALPHA_TEST, TEV2 */

varying vec4 v_color;
varying vec2 v_texcoord0;
varying vec2 v_texcoord1;
varying vec3 v_normal;
varying float v_fog_z;

uniform sampler2D u_texture0;
uniform float u_use_texture0;
uniform float u_tev0_tc_src;

/* TEV stage 0 — CPU pre-resolved inputs */
uniform vec4 u_tev0_csrc;
uniform vec3 u_tev0_ca;
uniform vec3 u_tev0_cb;
uniform vec3 u_tev0_cc;
uniform vec3 u_tev0_cd;
uniform vec4 u_tev0_asrc;
uniform vec4 u_tev0_aval;
uniform vec4 u_tev0_param;
uniform vec2 u_tev0_aparam; /* [alpha_scale, alpha_op] */

#ifdef TEV2
uniform sampler2D u_texture1;
uniform float u_use_texture1;
uniform float u_tev1_tc_src;
uniform vec4 u_tev1_csrc;
uniform vec3 u_tev1_ca;
uniform vec3 u_tev1_cb;
uniform vec3 u_tev1_cc;
uniform vec3 u_tev1_cd;
uniform vec4 u_tev1_asrc;
uniform vec4 u_tev1_aval;
uniform vec4 u_tev1_param;
uniform vec2 u_tev1_aparam; /* [alpha_scale, alpha_op] */
#endif

uniform float u_num_chans;
uniform vec4 u_mat_color;
uniform float u_chan_mat_src;
uniform float u_alpha_mat_src;

#ifdef LIGHTING
uniform vec4 u_amb_color;
uniform float u_chan_amb_src;
uniform float u_alpha_lighting_enabled;
uniform vec3 u_light_pos[8];
uniform vec4 u_light_color[8];
#endif

#ifdef FOG
uniform float u_fog_start;
uniform float u_fog_end;
uniform vec4 u_fog_color;
#endif

#ifdef ALPHA_TEST
uniform float u_alpha_ref0;
uniform float u_alpha_comp0; /* 0=GEQUAL(default), 1=LESS(inverted) — CPU pre-resolves other modes */
#endif

void main() {
    vec2 tc0 = mix(v_texcoord0, v_texcoord1, step(0.5, u_tev0_tc_src));
    vec4 tex0 = mix(vec4(1.0), texture2D(u_texture0, tc0), step(0.5, u_use_texture0));

    vec3 matC = mix(u_mat_color.rgb, v_color.rgb, step(0.5, u_chan_mat_src));
    float matA = mix(u_mat_color.a, v_color.a, step(0.5, u_alpha_mat_src));

#ifdef LIGHTING
    vec3 n_dir = normalize(v_normal);
    vec3 ambC = (u_chan_amb_src != 0) ? v_color.rgb : u_amb_color.rgb;
    vec3 lightAccum = ambC;
    lightAccum += max(dot(n_dir, normalize(u_light_pos[0])), 0.0) * u_light_color[0].rgb;
    lightAccum += max(dot(n_dir, normalize(u_light_pos[1])), 0.0) * u_light_color[1].rgb;
    lightAccum += max(dot(n_dir, normalize(u_light_pos[2])), 0.0) * u_light_color[2].rgb;
    lightAccum += max(dot(n_dir, normalize(u_light_pos[3])), 0.0) * u_light_color[3].rgb;
    lightAccum += max(dot(n_dir, normalize(u_light_pos[4])), 0.0) * u_light_color[4].rgb;
    lightAccum += max(dot(n_dir, normalize(u_light_pos[5])), 0.0) * u_light_color[5].rgb;
    lightAccum += max(dot(n_dir, normalize(u_light_pos[6])), 0.0) * u_light_color[6].rgb;
    lightAccum += max(dot(n_dir, normalize(u_light_pos[7])), 0.0) * u_light_color[7].rgb;
    matC = matC * clamp(lightAccum, 0.0, 1.0);
    matA = mix(matA, matA * u_amb_color.a, step(0.5, u_alpha_lighting_enabled));
#endif

    vec3 ras_c = mix(vec3(1.0), matC, step(0.5, u_num_chans));
    float ras_a = mix(1.0, matA, step(0.5, u_num_chans));

    /* Stage 0 color: src 0=fixed 1=tex.rgb 2=tex.aaa 3=ras.rgb 4=ras.aaa */
    vec3 s0ca = mix(mix(mix(u_tev0_ca, tex0.rgb, step(0.5,u_tev0_csrc.x)),
                    vec3(tex0.a), step(1.5,u_tev0_csrc.x)),
                    mix(ras_c, vec3(ras_a), step(3.5,u_tev0_csrc.x)),
                    step(2.5,u_tev0_csrc.x));
    vec3 s0cb = mix(mix(mix(u_tev0_cb, tex0.rgb, step(0.5,u_tev0_csrc.y)),
                    vec3(tex0.a), step(1.5,u_tev0_csrc.y)),
                    mix(ras_c, vec3(ras_a), step(3.5,u_tev0_csrc.y)),
                    step(2.5,u_tev0_csrc.y));
    vec3 s0cc = mix(mix(mix(u_tev0_cc, tex0.rgb, step(0.5,u_tev0_csrc.z)),
                    vec3(tex0.a), step(1.5,u_tev0_csrc.z)),
                    mix(ras_c, vec3(ras_a), step(3.5,u_tev0_csrc.z)),
                    step(2.5,u_tev0_csrc.z));
    vec3 s0cd = mix(mix(mix(u_tev0_cd, tex0.rgb, step(0.5,u_tev0_csrc.w)),
                    vec3(tex0.a), step(1.5,u_tev0_csrc.w)),
                    mix(ras_c, vec3(ras_a), step(3.5,u_tev0_csrc.w)),
                    step(2.5,u_tev0_csrc.w));

    /* Stage 0 alpha: src 0=fixed 1=tex.a 2=ras.a */
    float s0aa = mix(mix(u_tev0_aval.x, tex0.a, step(0.5,u_tev0_asrc.x)), ras_a, step(1.5,u_tev0_asrc.x));
    float s0ab = mix(mix(u_tev0_aval.y, tex0.a, step(0.5,u_tev0_asrc.y)), ras_a, step(1.5,u_tev0_asrc.y));
    float s0ac = mix(mix(u_tev0_aval.z, tex0.a, step(0.5,u_tev0_asrc.z)), ras_a, step(1.5,u_tev0_asrc.z));
    float s0ad = mix(mix(u_tev0_aval.w, tex0.a, step(0.5,u_tev0_asrc.w)), ras_a, step(1.5,u_tev0_asrc.w));

    /* TEV: result = clamp((D + op*mix(A,B,C) + bias) * scale) */
    vec3 prev_c = clamp((s0cd + u_tev0_param.w * mix(s0ca, s0cb, s0cc) + vec3(u_tev0_param.x)) * u_tev0_param.z, 0.0, 1.0);
    float prev_a = clamp((s0ad + u_tev0_aparam.y * mix(s0aa, s0ab, s0ac) + u_tev0_param.y) * u_tev0_aparam.x, 0.0, 1.0);

#ifdef TEV2
    vec2 tc1 = mix(v_texcoord0, v_texcoord1, step(0.5, u_tev1_tc_src));
    vec4 tex1 = mix(vec4(1.0), texture2D(u_texture1, tc1), step(0.5, u_use_texture1));

    /* Stage 1 color: add src 5=prev.rgb 6=prev.aaa */
    vec3 s1ca = mix(mix(mix(mix(u_tev1_ca, tex1.rgb, step(0.5,u_tev1_csrc.x)),
                    vec3(tex1.a), step(1.5,u_tev1_csrc.x)),
                    mix(ras_c, vec3(ras_a), step(3.5,u_tev1_csrc.x)),
                    step(2.5,u_tev1_csrc.x)),
                    mix(prev_c, vec3(prev_a), step(5.5,u_tev1_csrc.x)),
                    step(4.5,u_tev1_csrc.x));
    vec3 s1cb = mix(mix(mix(mix(u_tev1_cb, tex1.rgb, step(0.5,u_tev1_csrc.y)),
                    vec3(tex1.a), step(1.5,u_tev1_csrc.y)),
                    mix(ras_c, vec3(ras_a), step(3.5,u_tev1_csrc.y)),
                    step(2.5,u_tev1_csrc.y)),
                    mix(prev_c, vec3(prev_a), step(5.5,u_tev1_csrc.y)),
                    step(4.5,u_tev1_csrc.y));
    vec3 s1cc = mix(mix(mix(mix(u_tev1_cc, tex1.rgb, step(0.5,u_tev1_csrc.z)),
                    vec3(tex1.a), step(1.5,u_tev1_csrc.z)),
                    mix(ras_c, vec3(ras_a), step(3.5,u_tev1_csrc.z)),
                    step(2.5,u_tev1_csrc.z)),
                    mix(prev_c, vec3(prev_a), step(5.5,u_tev1_csrc.z)),
                    step(4.5,u_tev1_csrc.z));
    vec3 s1cd = mix(mix(mix(mix(u_tev1_cd, tex1.rgb, step(0.5,u_tev1_csrc.w)),
                    vec3(tex1.a), step(1.5,u_tev1_csrc.w)),
                    mix(ras_c, vec3(ras_a), step(3.5,u_tev1_csrc.w)),
                    step(2.5,u_tev1_csrc.w)),
                    mix(prev_c, vec3(prev_a), step(5.5,u_tev1_csrc.w)),
                    step(4.5,u_tev1_csrc.w));

    /* Stage 1 alpha: src 0=fixed 1=tex.a 2=ras.a 3=prev.a */
    float s1aa = mix(mix(u_tev1_aval.x, tex1.a, step(0.5,u_tev1_asrc.x)), mix(ras_a, prev_a, step(2.5,u_tev1_asrc.x)), step(1.5,u_tev1_asrc.x));
    float s1ab = mix(mix(u_tev1_aval.y, tex1.a, step(0.5,u_tev1_asrc.y)), mix(ras_a, prev_a, step(2.5,u_tev1_asrc.y)), step(1.5,u_tev1_asrc.y));
    float s1ac = mix(mix(u_tev1_aval.z, tex1.a, step(0.5,u_tev1_asrc.z)), mix(ras_a, prev_a, step(2.5,u_tev1_asrc.z)), step(1.5,u_tev1_asrc.z));
    float s1ad = mix(mix(u_tev1_aval.w, tex1.a, step(0.5,u_tev1_asrc.w)), mix(ras_a, prev_a, step(2.5,u_tev1_asrc.w)), step(1.5,u_tev1_asrc.w));

    prev_c = clamp((s1cd + u_tev1_param.w * mix(s1ca, s1cb, s1cc) + vec3(u_tev1_param.x)) * u_tev1_param.z, 0.0, 1.0);
    prev_a = clamp((s1ad + u_tev1_aparam.y * mix(s1aa, s1ab, s1ac) + u_tev1_param.y) * u_tev1_aparam.x, 0.0, 1.0);
#endif

#ifdef ALPHA_TEST
    /* GEQUAL: pass when alpha >= ref. LESS: pass when alpha < ref.
     * CPU pre-resolves GREATER→GEQUAL(ref+eps), LEQUAL→LESS(ref+eps),
     * NEVER→ref=2.0, ALWAYS→ref=-1.0, EQUAL/NEQUAL→approx GEQUAL. */
    float at_gequal = step(u_alpha_ref0, prev_a);
    float at_pass = mix(at_gequal, 1.0 - at_gequal, step(0.5, u_alpha_comp0));
    if (at_pass < 0.5) discard;
#endif

#ifdef FOG
    float fd = max(u_fog_end - u_fog_start, 0.001);
    float ff = clamp((v_fog_z - u_fog_start) / fd, 0.0, 1.0);
    prev_c = mix(prev_c, u_fog_color.rgb, ff);
#endif

    gl_FragColor = vec4(prev_c, prev_a);
}
