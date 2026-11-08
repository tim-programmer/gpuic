/*
 * OpenGL 3.3 renderer. The whole scene is one instanced draw: every primitive
 * (rounded quad with border, blurred shadow, glyph) is a 4-vertex strip
 * expanded in the vertex shader and shaded with signed-distance functions.
 * Draw order == paint order, so there is no sorting and one draw call.
 */
#include "internal.h"

/* SDL ships its own copy of the GL headers, so neither libGL nor Mesa headers
 * are needed to build; every entry point is loaded at runtime. */
#define GL_GLEXT_PROTOTYPES 1 /* declarations only: they give the table its types */
#include <SDL3/SDL_opengl.h>

#define GL_FUNCS(X)                                                                       \
    X(glActiveTexture)                                                                    \
    X(glBindTexture)                                                                      \
    X(glBlendFunc)                                                                        \
    X(glClear)                                                                            \
    X(glClearColor)                                                                       \
    X(glDeleteTextures)                                                                   \
    X(glEnable)                                                                           \
    X(glGenTextures)                                                                      \
    X(glPixelStorei)                                                                      \
    X(glReadPixels)                                                                       \
    X(glTexImage2D)                                                                       \
    X(glTexParameteri)                                                                    \
    X(glTexSubImage2D)                                                                    \
    X(glViewport)                                                                         \
    X(glGenVertexArrays)                                        \
    X(glBindVertexArray)                                        \
    X(glDeleteVertexArrays)                                  \
    X(glGenBuffers)                                                  \
    X(glDeleteBuffers)                                            \
    X(glBindBuffer)                                                  \
    X(glBufferData)                                                  \
    X(glEnableVertexAttribArray)                        \
    X(glVertexAttribPointer)                                \
    X(glVertexAttribDivisor)                                \
    X(glCreateShader)                                              \
    X(glShaderSource)                                              \
    X(glCompileShader)                                            \
    X(glGetShaderiv)                                                \
    X(glGetShaderInfoLog)                                      \
    X(glDeleteShader)                                              \
    X(glCreateProgram)                                            \
    X(glAttachShader)                                              \
    X(glLinkProgram)                                                \
    X(glGetProgramiv)                                              \
    X(glGetProgramInfoLog)                                    \
    X(glDeleteProgram)                                            \
    X(glUseProgram)                                                  \
    X(glGetUniformLocation)                                  \
    X(glUniform1f)                                                    \
    X(glUniform1i)                                                    \
    X(glUniform2f)                                                    \
    X(glGenerateMipmap)                                                 \
    X(glDrawArraysInstanced)

/* The headers declare these as prototypes; calls go through `gl.name` instead. */
static struct {
#define X(name) __typeof__(&name) name;
    GL_FUNCS(X)
#undef X
} gl;

static const char *vs_src =
    "#version 330 core\n"
    "layout(location=0) in vec4 a_bounds;\n"
    "layout(location=1) in vec4 a_clip;\n"
    "layout(location=2) in vec4 a_color;\n"
    "layout(location=3) in vec4 a_border;\n"
    "layout(location=4) in vec4 a_radii;\n"
    "layout(location=5) in vec4 a_params;\n" /* border_w, kind, blur, - */
    "layout(location=6) in vec4 a_uv;\n"
    "layout(location=7) in vec4 a_s0;\n"
    "layout(location=8) in vec4 a_s1;\n"
    "layout(location=9) in vec4 a_s2;\n"
    "layout(location=10) in vec4 a_bsty;\n"
    "layout(location=11) in vec4 a_xf0;\n"   /* the transform: (a - 1, b, c, d - 1) */
    "layout(location=12) in vec4 a_xf1;\n"   /* (tx, ty, scale - 1, -) */
    "uniform vec2 u_viewport;\n"
    "flat out vec4 v_bounds, v_clip, v_color, v_border, v_radii, v_params, v_uvr, v_s0, v_s1, v_s2, v_bsty;\n"
    "flat out float v_mag;\n"
    "out vec2 v_local;\n"
    "out vec2 v_pos;\n"
    "out vec2 v_uv;\n"
    "void main() {\n"
    "  vec2 corner = vec2(float(gl_VertexID & 1), float(gl_VertexID >> 1));\n"
    "  float kind = a_params.y;\n"
    "  float pad = kind == 1.0 ? a_params.z * 1.5 + 1.0 : ((kind == 0.0 || kind == 5.0 || kind == 6.0 || kind == 7.0 || kind == 8.0) ? 1.0 : 0.0);\n"
    "  vec2 size = a_bounds.zw + 2.0 * pad;\n"
    "  vec2 pos0 = a_bounds.xy - pad + corner * size;\n"   /* where it is in the layout, before the transform */
    "  vec2 pos = pos0 + vec2(a_xf0.x * pos0.x + a_xf0.z * pos0.y + a_xf1.x, a_xf0.y * pos0.x + a_xf0.w * pos0.y + a_xf1.y);\n"
    "  v_pos = pos;\n"                                      /* the clip is in screen space */
    "  v_mag = 1.0 + a_xf1.z;\n"
    "  v_local = pos0 - (a_bounds.xy + a_bounds.zw * 0.5);\n"
    "  v_uv = a_uv.xy + corner * a_uv.zw;\n"
    "  v_bounds = a_bounds; v_clip = a_clip; v_color = a_color; v_border = a_border;\n"
    "  v_radii = a_radii; v_params = a_params; v_uvr = a_uv;\n"
    "  v_s0 = a_s0; v_s1 = a_s1; v_s2 = a_s2; v_bsty = a_bsty;\n"
    "  gl_Position = vec4(pos.x / u_viewport.x * 2.0 - 1.0, 1.0 - pos.y / u_viewport.y * 2.0, 0.0, 1.0);\n"
    "}\n";

static const char *fs_src =
    "#version 330 core\n"
    "flat in vec4 v_bounds, v_clip, v_color, v_border, v_radii, v_params, v_uvr, v_s0, v_s1, v_s2, v_bsty;\n"
    "flat in float v_mag;\n"
    "in vec2 v_local;\n"
    "in vec2 v_pos;\n"
    "in vec2 v_uv;\n"
    "uniform float u_scale;\n"
    "uniform float u_gamma;\n"
    "uniform sampler2D u_atlas;\n"
    "uniform sampler2D u_color_atlas;\n"
    "uniform sampler2D u_lcd_atlas;\n"
    "uniform sampler2D u_ramps;\n"
    "uniform sampler2D u_image;\n"
    "layout(location = 0, index = 0) out vec4 frag;\n"
    "layout(location = 0, index = 1) out vec4 frag_cov;\n" /* what each channel of the destination keeps: 1 - this */
    "float sd_rounded(vec2 p, vec2 half_size, vec4 r4) {\n"
    "  float r = p.x < 0.0 ? (p.y < 0.0 ? r4.x : r4.w) : (p.y < 0.0 ? r4.y : r4.z);\n"
    "  vec2 q = abs(p) - half_size + r;\n"
    "  return min(max(q.x, q.y), 0.0) + length(max(q, 0.0)) - r;\n"
    "}\n"
    "float erf_approx(float x) {\n"
    "  float x2 = x * x;\n"
    "  float t = (1.2732395 + 0.147 * x2) / (1.0 + 0.147 * x2);\n"
    "  return sign(x) * sqrt(1.0 - exp(-x2 * t));\n"
    "}\n"
    "vec4 premul(vec4 c) { return vec4(c.rgb * c.a, c.a); }\n"
    /* Text coverage adjusted so that blending in the encoded (gamma) space gives what blending in linear light would, for text of luma `lt` on a
     * background of the opposite luma (a background that is not that is the technique's limit). u_gamma 1 is no adjustment. */
    "float tone(float c, float lt) {\n"
    "  if (u_gamma == 1.0) return c;\n"
    "  float lb = 1.0 - lt, d = lt - lb;\n"
    "  if (abs(d) < 0.02) return c;\n"
    "  float g = pow((1.0 - c) * pow(lb, u_gamma) + c * pow(lt, u_gamma), 1.0 / u_gamma);\n"
    "  return clamp((g - lb) / d, 0.0, 1.0);\n"
    "}\n"
    "void main() {\n"
    "  float us = u_scale * v_mag;\n"
    "  if (v_pos.x < v_clip.x || v_pos.y < v_clip.y ||\n"
    "      v_pos.x > v_clip.x + v_clip.z || v_pos.y > v_clip.y + v_clip.w) discard;\n"
    "  float kind = v_params.y;\n"
    "  vec2 half_size = v_bounds.zw * 0.5;\n"
    "  vec4 lcd_cov = vec4(0.0);\n"
    "  if (kind == 4.0) {\n" /* LCD text: a coverage per channel, blended per channel */
    "    vec3 cov = texture(u_lcd_atlas, v_uv).rgb;\n"
    "    float lt = dot(v_color.rgb, vec3(0.2126, 0.7152, 0.0722));\n"
    "    cov = vec3(tone(cov.r, lt), tone(cov.g, lt), tone(cov.b, lt));\n"
    "    frag = vec4(v_color.rgb * v_color.a * cov, v_color.a * (cov.r + cov.g + cov.b) / 3.0);\n"
    "    lcd_cov = vec4(cov * v_color.a, frag.a);\n"
    "  } else if (kind == 7.0) {\n" /* an image: the texture rectangle is in v_uvr; the corners are cut like a rounded box's */
    "    vec2 q = clamp(v_local / v_bounds.zw + 0.5, 0.0, 1.0);\n"
    "    float mask = clamp(0.5 - sd_rounded(v_local, half_size, v_radii) * us, 0.0, 1.0);\n"
    "    vec2 uv = v_uvr.xy + q * v_uvr.zw;\n"
    "    float blur = v_params.z >= 500.0 ? v_params.z - 1000.0 : v_params.z;\n" /* params.z: the blur radius, plus 1000 for pixelated */
    "    vec4 t;\n"
    "    if (blur > 0.0) {\n" /* a 3x3 tap blur of a coarser mip level, wide enough to cover the radius */
    "      vec2 ruv = blur * v_uvr.zw / v_bounds.zw;\n"
    "      float lod = max(log2(max(ruv.x * float(textureSize(u_image, 0).x), 1.0)) - 1.0, 0.0);\n"
    "      t = vec4(0.0);\n"
    "      for (int i = -1; i <= 1; i++)\n"
    "        for (int j = -1; j <= 1; j++)\n"
    "          t += textureLod(u_image, clamp(uv + vec2(float(i), float(j)) * ruv * 0.8, v_uvr.xy, v_uvr.xy + v_uvr.zw), lod) * (float(2 - abs(i)) * float(2 - abs(j)) / 16.0);\n"
    "    } else {\n"
    "      t = texture(u_image, uv);\n"
    "    }\n"
    "    vec3 c = t.a > 0.0 ? t.rgb / t.a : vec3(0.0);\n" /* the filters work on the color, not on its premultiplied form */
    "    float brightness = 1.0 + v_border.z, saturate = 1.0 + v_border.w, contrast = 1.0 + v_params.x;\n"
    "    c *= brightness;\n"
    "    c = (c - 0.5) * contrast + 0.5;\n"
    "    float luma = dot(c, vec3(0.2126, 0.7152, 0.0722));\n"
    "    c = mix(vec3(luma), c, saturate);\n"
    "    c = mix(c, vec3(dot(c, vec3(0.2126, 0.7152, 0.0722))), v_border.y);\n"
    "    c = clamp(c * v_color.rgb, 0.0, 1.0);\n"
    "    frag = vec4(c * t.a, t.a) * (v_color.a * mask);\n"
    "  } else if (kind == 3.0) {\n"
    "    frag = texture(u_color_atlas, v_uv) * v_color.a;\n" /* premultiplied RGBA; alpha carries opacity */
    "  } else if (kind == 2.0) {\n"
    "    float a = tone(texture(u_atlas, v_uv).r, dot(v_color.rgb, vec3(0.2126, 0.7152, 0.0722)));\n"
    "    frag = premul(v_color) * a;\n"
    "  } else if (kind == 1.0) {\n"
    "    float sigma = max(v_params.z * 0.5, 0.001);\n"
    "    float d = sd_rounded(v_local, half_size, v_radii);\n"
    "    float a = 0.5 - 0.5 * erf_approx(d / (sigma * 1.41421356));\n"
    "    frag = premul(v_color) * a;\n"
    "  } else {\n"
    "    float d = sd_rounded(v_local, half_size, v_radii);\n"
    "    float outer = clamp(0.5 - d * us, 0.0, 1.0);\n"
    "    vec4 col = premul(v_color);\n"
    "    if (kind == 5.0 || kind == 6.0 || kind == 8.0) {\n" /* a gradient: v_color holds its geometry and the opacity, v_uvr.x the ramp's row */
    "      float t;\n"
    "      if (kind == 5.0) t = dot(v_local, v_color.xy) / v_color.z + 0.5;\n"
    "      else if (kind == 6.0) t = length(v_local - v_color.xy) / v_color.z;\n"
    "      else { vec2 d = v_local - v_color.xy; t = fract((atan(d.x, -d.y) - v_color.z) / 6.28318530718); }\n" /* conic: clockwise from up */
    "      if (v_params.z > 0.0) t = fract((t - v_params.w) * v_params.z);\n" /* repeating: params.z = 1 / period, params.w = where it starts */
    "      col = texture(u_ramps, vec2(clamp(t, 0.0, 1.0), v_uvr.x)) * v_color.w;\n"
    "      float dither = fract(52.9829189 * fract(dot(gl_FragCoord.xy, vec2(0.06711056, 0.00583715)))) - 0.5;\n" /* hides 8-bit banding */
    "      col.rgb = clamp(col.rgb + dither / 255.0 * col.a, 0.0, col.a);\n"
    "    }\n"
    "    float bl = v_params.x, bt = v_uvr.y, br = v_uvr.z, bb = v_uvr.w;\n" /* the border's width on each side */
    "    if (bl + bt + br + bb > 0.0) {\n"
    "      vec2 ihalf = max(half_size - vec2(bl + br, bt + bb) * 0.5, vec2(0.0));\n" /* the padding box: the box less its borders */
    "      vec2 ictr = vec2(bl - br, bt - bb) * 0.5;\n"
    "      vec4 ir = max(v_radii - vec4(bl + bt, br + bt, br + bb, bl + bb) * 0.5, 0.0);\n"
    "      float di = sd_rounded(v_local - ictr, ihalf, ir);\n"
    "      float inner = clamp(0.5 - di * us, 0.0, 1.0);\n"
    "      vec4 ring = premul(v_border);\n"
    "      if (v_bsty.x > 0.5 || v_bsty.y > 0.5 || v_bsty.z > 0.0) {\n" /* sides of their own colors, a gradient, dashes or dots */
    "        vec2 p = v_local;\n"
    "        float dl = bl > 0.0 ? (p.x + half_size.x) / bl : 1e9, dt = bt > 0.0 ? (p.y + half_size.y) / bt : 1e9;\n"
    "        float dr = br > 0.0 ? (half_size.x - p.x) / br : 1e9, db = bb > 0.0 ? (half_size.y - p.y) / bb : 1e9;\n"
    "        float m = min(min(dl, dt), min(dr, db));\n" /* the side: the one this pixel is nearest to, in units of its own width: the mitre */
    "        if (v_bsty.y > 0.5) ring = m == dl ? premul(v_border) : m == dt ? premul(v_s0) : m == dr ? premul(v_s1) : premul(v_s2);\n"
    "        if (v_bsty.z > 0.0) {\n" /* a gradient over the box: the same mapping as a background's */
    "          float gt;\n"
    "          if (v_bsty.w == 5.0) gt = dot(v_local, v_s0.xy) / v_s0.z + 0.5;\n"
    "          else if (v_bsty.w == 6.0) gt = length(v_local - v_s0.xy) / v_s0.z;\n"
    "          else { vec2 gd = v_local - v_s0.xy; gt = fract((atan(gd.x, -gd.y) - v_s0.z) / 6.28318530718); }\n"
    "          ring = texture(u_ramps, vec2(clamp(gt, 0.0, 1.0), v_bsty.z)) * v_s0.w;\n"
    "        }\n"
    "        if (v_bsty.x > 0.5) {\n" /* dashes: 2 widths and a gap of 1; dots: circles of the width, 2 apart: along the side from its start, and across it */
    "          float wd = m == dl ? bl : m == dt ? bt : m == dr ? br : bb;\n"
    "          float along = (m == dl || m == dr) ? p.y + half_size.y : p.x + half_size.x;\n"
    "          float across = m == dl ? p.x + half_size.x : m == dt ? p.y + half_size.y : m == dr ? half_size.x - p.x : half_size.y - p.y;\n"
    "          float mask;\n"
    "          if (v_bsty.x < 1.5) {\n"
    "            float per = 3.0 * wd, ph = mod(along, per);\n"
    "            mask = clamp(min(ph, 2.0 * wd - ph) * us + 0.5, 0.0, 1.0);\n" /* the dash's edges, each a pixel wide */
    "            mask = ph < 2.0 * wd ? mask : clamp(0.5 - min(ph - 2.0 * wd, per - ph) * us, 0.0, 1.0);\n"
    "          } else {\n"
    "            float per = 2.0 * wd, cx0 = (floor(along / per) + 0.5) * per;\n"
    "            float dd = length(vec2(along - cx0, across - wd * 0.5)) - wd * 0.5;\n"
    "            mask = clamp(0.5 - dd * us, 0.0, 1.0);\n"
    "          }\n"
    "          ring = mix(col, ring, mask);\n" /* between the dashes the fill shows */
    "        }\n"
    "      }\n"
    "      col = mix(ring, col, inner);\n"
    "    }\n"
    "    frag = col * outer;\n"
    "  }\n"
    "  frag_cov = kind == 4.0 ? lcd_cov : vec4(frag.a);\n"
    "}\n";

struct gp_renderer {
    GLuint prog, vao, vbo, atlas, color_atlas, lcd_atlas, ramps;
    GLint u_viewport, u_scale, u_gamma, u_atlas, u_color_atlas, u_lcd_atlas, u_ramps, u_image;
    size_t vbo_cap;
};

static GLuint compile(GLenum type, const char *src) {
    GLuint s = gl.glCreateShader(type);
    gl.glShaderSource(s, 1, &src, NULL);
    gl.glCompileShader(s);
    GLint ok = 0;
    gl.glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[2048];
        gl.glGetShaderInfoLog(s, sizeof log, NULL, log);
        fprintf(stderr, "gpuic: shader compile failed:\n%s\n", log);
        return 0;
    }
    return s;
}

gp_renderer *gp_renderer_new(void *(*get_proc)(const char *)) {
#define X(name)                                                                           \
    gl.name = (__typeof__(gl.name))get_proc(#name);                                       \
    if (!gl.name) { fprintf(stderr, "gpuic: missing GL function %s\n", #name); return NULL; }
    GL_FUNCS(X)
#undef X

    gp_renderer *r = calloc(1, sizeof *r);
    GLuint vs = compile(GL_VERTEX_SHADER, vs_src), fs = compile(GL_FRAGMENT_SHADER, fs_src);
    if (!vs || !fs) { free(r); return NULL; }
    r->prog = gl.glCreateProgram();
    gl.glAttachShader(r->prog, vs);
    gl.glAttachShader(r->prog, fs);
    gl.glLinkProgram(r->prog);
    GLint ok = 0;
    gl.glGetProgramiv(r->prog, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[2048];
        gl.glGetProgramInfoLog(r->prog, sizeof log, NULL, log);
        fprintf(stderr, "gpuic: program link failed:\n%s\n", log);
        free(r);
        return NULL;
    }
    gl.glDeleteShader(vs);
    gl.glDeleteShader(fs);
    r->u_viewport = gl.glGetUniformLocation(r->prog, "u_viewport");
    r->u_scale = gl.glGetUniformLocation(r->prog, "u_scale");
    r->u_gamma = gl.glGetUniformLocation(r->prog, "u_gamma");
    r->u_atlas = gl.glGetUniformLocation(r->prog, "u_atlas");
    r->u_color_atlas = gl.glGetUniformLocation(r->prog, "u_color_atlas");
    r->u_lcd_atlas = gl.glGetUniformLocation(r->prog, "u_lcd_atlas");
    r->u_ramps = gl.glGetUniformLocation(r->prog, "u_ramps");
    r->u_image = gl.glGetUniformLocation(r->prog, "u_image");

    gl.glGenVertexArrays(1, &r->vao);
    gl.glBindVertexArray(r->vao);
    gl.glGenBuffers(1, &r->vbo);
    gl.glBindBuffer(GL_ARRAY_BUFFER, r->vbo);
    for (GLuint i = 0; i < 13; i++) {
        /* gp_instance is thirteen consecutive vec4s */
        gl.glEnableVertexAttribArray(i);
        gl.glVertexAttribPointer(i, 4, GL_FLOAT, GL_FALSE, sizeof(gp_instance),
                              (const void *)(uintptr_t)(i * 4 * sizeof(float)));
        gl.glVertexAttribDivisor(i, 1);
    }

    gl.glGenTextures(1, &r->atlas);
    gl.glBindTexture(GL_TEXTURE_2D, r->atlas);
    gl.glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    gl.glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, GP_ATLAS_SIZE, GP_ATLAS_SIZE, 0, GL_RED,
                 GL_UNSIGNED_BYTE, NULL);
    gl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    gl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    gl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    gl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    gl.glGenTextures(1, &r->color_atlas);
    gl.glActiveTexture(GL_TEXTURE1);
    gl.glBindTexture(GL_TEXTURE_2D, r->color_atlas);
    gl.glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, GP_COLOR_ATLAS_SIZE, GP_COLOR_ATLAS_SIZE, 0, GL_RGBA,
                 GL_UNSIGNED_BYTE, NULL);
    gl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    gl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    gl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    gl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    gl.glGenTextures(1, &r->ramps);
    gl.glActiveTexture(GL_TEXTURE3);
    gl.glBindTexture(GL_TEXTURE_2D, r->ramps);
    gl.glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, GP_GRAD_W, GP_GRAD_ROWS, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    gl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    gl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    gl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    gl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    gl.glGenTextures(1, &r->lcd_atlas);
    gl.glActiveTexture(GL_TEXTURE2);
    gl.glBindTexture(GL_TEXTURE_2D, r->lcd_atlas);
    gl.glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, GP_LCD_ATLAS_SIZE, GP_LCD_ATLAS_SIZE, 0, GL_RGBA,
                 GL_UNSIGNED_BYTE, NULL);
    gl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    gl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    gl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    gl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    gl.glActiveTexture(GL_TEXTURE0);
    return r;
}

void gp_renderer_free(gp_renderer *r, gp_gfx *gfx) {
    if (!r) return;
    {   /* the images' textures go with the context; they are made again if a new renderer draws them */
        uint32_t tex[64];
        size_t n;
        while (gfx && (n = gp_gfx_take_all_textures(gfx, tex, 64)) > 0) gl.glDeleteTextures((GLsizei)n, tex);
        uint32_t dead[64];
        while (gfx && (n = gp_gfx_take_dead_textures(gfx, dead, 64)) > 0) gl.glDeleteTextures((GLsizei)n, dead);
    }
    gl.glDeleteProgram(r->prog);
    gl.glDeleteBuffers(1, &r->vbo);
    gl.glDeleteVertexArrays(1, &r->vao);
    gl.glDeleteTextures(1, &r->atlas);
    gl.glDeleteTextures(1, &r->color_atlas);
    gl.glDeleteTextures(1, &r->lcd_atlas);
    gl.glDeleteTextures(1, &r->ramps);
    free(r);
}

/* instances [first, first + count) in one instanced draw */
static void draw_run(gp_renderer *r, const gp_scene *scene, size_t first, size_t count) {
    (void)scene;
    for (GLuint a = 0; a < 13; a++)
        gl.glVertexAttribPointer(a, 4, GL_FLOAT, GL_FALSE, sizeof(gp_instance),
                                 (const void *)(uintptr_t)(first * sizeof(gp_instance) + a * 4 * sizeof(float)));
    gl.glDrawArraysInstanced(GL_TRIANGLE_STRIP, 0, 4, (GLsizei)count);
    (void)r;
}

/* The texture of an image, made when it is first drawn: its pixels, with mipmaps for when it is shown smaller. */
static void upload_image(gp_renderer *r, gp_gfx_image *im) {
    (void)r;
    if (im->gl_tex) return;
    GLuint tex;
    gl.glGenTextures(1, &tex);
    gl.glActiveTexture(GL_TEXTURE4);
    gl.glBindTexture(GL_TEXTURE_2D, tex);
    gl.glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    gl.glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    gl.glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, im->w, im->h, 0, GL_RGBA, GL_UNSIGNED_BYTE, im->pixels);
    gl.glGenerateMipmap(GL_TEXTURE_2D);
    gl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    gl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    gl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    gl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    gl.glActiveTexture(GL_TEXTURE0);
    im->gl_tex = tex;
}

void gp_renderer_draw(gp_renderer *r, gp_text_system *ts, const gp_scene *scene, int px_w,
                      int px_h, float scale, gp_color clear) {
    gl.glViewport(0, 0, px_w, px_h);
    gl.glClearColor(clear.r, clear.g, clear.b, clear.a);
    gl.glClear(GL_COLOR_BUFFER_BIT);

    {   /* textures of images that were released */
        gp_gfx *g = gp_text_gfx(ts);
        uint32_t dead[32];
        size_t n;
        while (g && (n = gp_gfx_take_dead_textures(g, dead, 32)) > 0) gl.glDeleteTextures((GLsizei)n, dead);
    }

    /* upload any glyphs rasterized since the last frame */
    gl.glActiveTexture(GL_TEXTURE0);
    gl.glBindTexture(GL_TEXTURE_2D, r->atlas);
    int dx, dy, dw, dh;
    if (gp_text_atlas_take_dirty(ts, &dx, &dy, &dw, &dh)) {
        gl.glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        gl.glPixelStorei(GL_UNPACK_ROW_LENGTH, GP_ATLAS_SIZE);
        gl.glTexSubImage2D(GL_TEXTURE_2D, 0, dx, dy, dw, dh, GL_RED, GL_UNSIGNED_BYTE,
                        gp_text_atlas(ts) + (size_t)dy * GP_ATLAS_SIZE + (size_t)dx);
        gl.glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    }
    gl.glActiveTexture(GL_TEXTURE1);
    gl.glBindTexture(GL_TEXTURE_2D, r->color_atlas);
    if (gp_text_color_atlas_take_dirty(ts, &dx, &dy, &dw, &dh)) {
        gl.glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        gl.glPixelStorei(GL_UNPACK_ROW_LENGTH, GP_COLOR_ATLAS_SIZE);
        gl.glTexSubImage2D(GL_TEXTURE_2D, 0, dx, dy, dw, dh, GL_RGBA, GL_UNSIGNED_BYTE,
                        gp_text_color_atlas(ts) + ((size_t)dy * GP_COLOR_ATLAS_SIZE + (size_t)dx) * 4);
        gl.glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    }
    gl.glActiveTexture(GL_TEXTURE2);
    gl.glBindTexture(GL_TEXTURE_2D, r->lcd_atlas);
    if (gp_text_lcd_atlas_take_dirty(ts, &dx, &dy, &dw, &dh)) {
        gl.glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        gl.glPixelStorei(GL_UNPACK_ROW_LENGTH, GP_LCD_ATLAS_SIZE);
        gl.glTexSubImage2D(GL_TEXTURE_2D, 0, dx, dy, dw, dh, GL_RGBA, GL_UNSIGNED_BYTE,
                        gp_text_lcd_atlas(ts) + ((size_t)dy * GP_LCD_ATLAS_SIZE + (size_t)dx) * 4);
        gl.glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    }
    gl.glActiveTexture(GL_TEXTURE3);
    gl.glBindTexture(GL_TEXTURE_2D, r->ramps);
    {
        gp_gfx *gfx = gp_text_gfx(ts);
        int row0, row1;
        if (gfx && gp_gfx_gradient_take_dirty(gfx, &row0, &row1)) {
            gl.glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
            gl.glTexSubImage2D(GL_TEXTURE_2D, 0, 0, row0, GP_GRAD_W, row1 - row0, GL_RGBA, GL_UNSIGNED_BYTE,
                               gp_gfx_gradient_data(gfx) + (size_t)row0 * GP_GRAD_W * 4);
        }
    }
    gl.glActiveTexture(GL_TEXTURE0);
    if (scene->len == 0) return;

    gl.glEnable(GL_BLEND);
    gl.glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC1_COLOR);   /* dual-source: LCD text keeps a different share of each channel */
    gl.glUseProgram(r->prog);
    gl.glUniform2f(r->u_viewport, (float)px_w / scale, (float)px_h / scale);
    gl.glUniform1f(r->u_scale, scale);
    gl.glUniform1f(r->u_gamma, gp_text_gamma(ts));
    gl.glUniform1i(r->u_atlas, 0);
    gl.glUniform1i(r->u_color_atlas, 1);
    gl.glUniform1i(r->u_lcd_atlas, 2);
    gl.glUniform1i(r->u_ramps, 3);
    gl.glUniform1i(r->u_image, 4);

    gl.glBindVertexArray(r->vao);
    gl.glBindBuffer(GL_ARRAY_BUFFER, r->vbo);
    size_t bytes = scene->len * sizeof(gp_instance);
    gl.glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)bytes, NULL, GL_STREAM_DRAW); /* orphan */
    gl.glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)bytes, scene->items, GL_STREAM_DRAW);
    /* Everything but images is one instanced draw. An image has a texture of its own, so it splits the scene: the run
     * before it, then the image alone, then on. Order is kept, which is what painting needs. */
    gp_gfx *gfx = gp_text_gfx(ts);
    size_t run = 0;
    for (size_t i = 0; i < scene->len; i++) {
        const gp_instance *in = &scene->items[i];
        if ((int)in->kind != GP_PRIM_IMAGE) continue;
        gp_gfx_image *im = gp_gfx_image_get(gfx, (uint32_t)in->_pad, (uint32_t)in->border_color[0]);
        if (!im) continue;   /* released since it was painted: nothing to draw (the next frame has no instance) */
        if (i > run) draw_run(r, scene, run, i - run);
        upload_image(r, im);
        gl.glActiveTexture(GL_TEXTURE4);
        gl.glBindTexture(GL_TEXTURE_2D, im->gl_tex);
        gl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, in->blur >= 500.f ? GL_NEAREST : GL_LINEAR);
        gl.glActiveTexture(GL_TEXTURE0);
        draw_run(r, scene, i, 1);
        run = i + 1;
    }
    if (scene->len > run) draw_run(r, scene, run, scene->len - run);
}

uint8_t *gp_renderer_read_pixels(int px_w, int px_h) {
    uint8_t *raw = malloc((size_t)px_w * px_h * 3);
    uint8_t *out = malloc((size_t)px_w * px_h * 3);
    gl.glPixelStorei(GL_PACK_ALIGNMENT, 1);
    gl.glReadPixels(0, 0, px_w, px_h, GL_RGB, GL_UNSIGNED_BYTE, raw);
    for (int y = 0; y < px_h; y++) /* GL origin is bottom-left */
        memcpy(out + (size_t)y * px_w * 3, raw + (size_t)(px_h - 1 - y) * px_w * 3, (size_t)px_w * 3);
    free(raw);
    return out;
}
