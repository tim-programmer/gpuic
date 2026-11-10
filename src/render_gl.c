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
    X(glGenFramebuffers)                                                \
    X(glDeleteFramebuffers)                                             \
    X(glBindFramebuffer)                                                \
    X(glFramebufferTexture2D)                                           \
    X(glCheckFramebufferStatus)                                         \
    X(glScissor)                                                        \
    X(glDisable)                                                        \
    X(glCopyTexSubImage2D)                                              \
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
    "layout(location=12) in vec4 a_xf1;\n"   /* (tx, ty, g, h) */
    "layout(location=13) in vec4 a_xf2;\n"   /* (w1, -, -, -): w = g x + h y + 1 + w1 (all 0: no perspective) */
    "uniform vec2 u_viewport;\n"
    "flat out vec4 v_bounds, v_clip, v_color, v_border, v_radii, v_params, v_uvr, v_s0, v_s1, v_s2, v_bsty;\n"
    "flat out float v_mag;\n"
    "flat out vec4 v_xf0, v_xf1, v_xf2;\n"
    "out vec2 v_local;\n"
    "out vec2 v_pos;\n"
    "out vec2 v_uv;\n"
    "void main() {\n"
    "  vec2 corner = vec2(float(gl_VertexID & 1), float(gl_VertexID >> 1));\n"
    "  float kind = a_params.y;\n"
    "  float pad = kind == 1.0 ? a_params.z * 1.5 + 1.0 : ((kind == 0.0 || kind == 5.0 || kind == 6.0 || kind == 7.0 || kind == 8.0 || kind == 18.0) ? 1.0 : 0.0);\n"
    "  vec2 size = a_bounds.zw + 2.0 * pad;\n"
    "  vec2 pos0 = a_bounds.xy - pad + corner * size;\n"   /* where it is in the layout, before the transform */
    "  vec2 pos = pos0 + vec2(a_xf0.x * pos0.x + a_xf0.z * pos0.y + a_xf1.x, a_xf0.y * pos0.x + a_xf0.w * pos0.y + a_xf1.y);\n"
    "  float w = a_xf1.z * pos0.x + a_xf1.w * pos0.y + 1.0 + a_xf2.x;\n"   /* in perspective: the position is homogeneous, and what is interpolated across is too */
    "  v_pos = pos / w;\n"
    "  v_mag = sqrt(abs((1.0 + a_xf0.x) * (1.0 + a_xf0.w) - a_xf0.y * a_xf0.z));\n"   /* how much it magnifies (affine) */
    "  v_xf0 = a_xf0; v_xf1 = a_xf1; v_xf2 = a_xf2;\n"
    "  v_local = pos0 - (a_bounds.xy + a_bounds.zw * 0.5);\n"
    "  v_uv = a_uv.xy + corner * a_uv.zw;\n"
    "  v_bounds = a_bounds; v_clip = a_clip; v_color = a_color; v_border = a_border;\n"
    "  v_radii = a_radii; v_params = a_params; v_uvr = a_uv;\n"
    "  v_s0 = a_s0; v_s1 = a_s1; v_s2 = a_s2; v_bsty = a_bsty;\n"
    "  gl_Position = vec4(pos.x / u_viewport.x * 2.0 - w, w - pos.y / u_viewport.y * 2.0, 0.0, w);\n"
    "}\n";

static const char *fs_src =
    "#version 330 core\n"
    "flat in vec4 v_bounds, v_clip, v_color, v_border, v_radii, v_params, v_uvr, v_s0, v_s1, v_s2, v_bsty;\n"
    "flat in float v_mag;\n"
    "flat in vec4 v_xf0, v_xf1, v_xf2;\n"
    "uniform vec2 u_viewport;\n"
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
    "uniform sampler2D u_layer;\n"   /* a layer (or a filter pass's input) */
    "uniform sampler2D u_aux;\n"     /* a drop shadow's blurred copy; at a layer's end, its mask */
    "uniform sampler2D u_backdrop;\n" /* at the end of a layer with a blend mode: what was under it */
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
    /* filter passes and layers: pixels of the window-sized textures; a pixel is read only inside the layer's region (v_clip), outside it is transparent */
    "bool in_region(ivec2 q) {\n"
    "  vec2 c = (vec2(q) + 0.5) / u_scale;\n"
    "  c.y = u_viewport.y - c.y;\n"   /* (GL's rows go up, the layout's down) */
    "  return c.x >= v_clip.x && c.y >= v_clip.y && c.x < v_clip.x + v_clip.z && c.y < v_clip.y + v_clip.w;\n"
    "}\n"
    /* inside the region, past the window's edge, the edge's pixel (nothing was drawn there: it goes on as it was) */
    "vec4 fetch(sampler2D t, ivec2 q) { return in_region(q) ? texelFetch(t, clamp(q, ivec2(0), textureSize(t, 0) - 1), 0) : vec4(0.0); }\n"
    /* a Gaussian along `dir` (whole pixels), sigma in device px; wide ones are sampled every few pixels */
    "vec4 gauss(sampler2D t, ivec2 q, ivec2 dir, float sigma) {\n"
    "  if (sigma < 0.3) return fetch(t, q);\n"
    "  int n = int(ceil(3.0 * sigma)), step = max(1, (n + 47) / 48);\n"
    "  vec4 acc = vec4(0.0); float wsum = 0.0;\n"
    "  for (int k = -n; k <= n; k += step) {\n"
    "    float w = exp(-float(k * k) / (2.0 * sigma * sigma));\n"
    "    acc += fetch(t, q + dir * k) * w; wsum += w;\n"
    "  }\n"
    "  return acc / wsum;\n"
    "}\n"
    /* blend modes, as the W3C compositing spec defines them, on straight colors: cb under, cs over */
    "float blend1(int m, float b, float s) {\n"
    "  if (m == 1) return b * s;\n"
    "  if (m == 2) return b + s - b * s;\n"
    "  if (m == 3) return b <= 0.5 ? s * 2.0 * b : s + (2.0 * b - 1.0) - s * (2.0 * b - 1.0);\n"   /* overlay: hard light with the two swapped */
    "  if (m == 4) return min(b, s);\n"
    "  if (m == 5) return max(b, s);\n"
    "  if (m == 6) return b <= 0.0 ? 0.0 : (s >= 1.0 ? 1.0 : min(1.0, b / (1.0 - s)));\n"
    "  if (m == 7) return b >= 1.0 ? 1.0 : (s <= 0.0 ? 0.0 : 1.0 - min(1.0, (1.0 - b) / s));\n"
    "  if (m == 8) return s <= 0.5 ? b * 2.0 * s : b + (2.0 * s - 1.0) - b * (2.0 * s - 1.0);\n"
    "  if (m == 9) {\n"
    "    if (s <= 0.5) return b - (1.0 - 2.0 * s) * b * (1.0 - b);\n"
    "    float d = b <= 0.25 ? ((16.0 * b - 12.0) * b + 4.0) * b : sqrt(b);\n"
    "    return b + (2.0 * s - 1.0) * (d - b);\n"
    "  }\n"
    "  if (m == 10) return abs(b - s);\n"
    "  if (m == 11) return b + s - 2.0 * b * s;\n"
    "  return s;\n"
    "}\n"
    "float lum(vec3 c) { return dot(c, vec3(0.3, 0.59, 0.11)); }\n"
    "vec3 clip_color(vec3 c) {\n"
    "  float l = lum(c), n = min(min(c.r, c.g), c.b), x = max(max(c.r, c.g), c.b);\n"
    "  if (n < 0.0) c = l + (c - l) * l / (l - n);\n"
    "  if (x > 1.0) c = l + (c - l) * (1.0 - l) / (x - l);\n"
    "  return c;\n"
    "}\n"
    "vec3 set_lum(vec3 c, float l) { return clip_color(c + (l - lum(c))); }\n"
    "float sat(vec3 c) { return max(max(c.r, c.g), c.b) - min(min(c.r, c.g), c.b); }\n"
    "vec3 set_sat(vec3 c, float s) {\n"
    "  float x = max(max(c.r, c.g), c.b), n = min(min(c.r, c.g), c.b);\n"
    "  return x > n ? (c - n) * s / (x - n) : vec3(0.0);\n"
    "}\n"
    "vec3 blend3(int m, vec3 b, vec3 s) {\n"
    "  if (m == 12) return set_lum(set_sat(s, sat(b)), lum(b));\n"
    "  if (m == 13) return set_lum(set_sat(b, sat(s)), lum(b));\n"
    "  if (m == 14) return set_lum(s, lum(b));\n"
    "  if (m == 15) return set_lum(b, lum(s));\n"
    "  return vec3(blend1(m, b.r, s.r), blend1(m, b.g, s.g), blend1(m, b.b, s.b));\n"
    "}\n"
    "void main() {\n"
    "  float us = u_scale * v_mag;\n"
    "  if (v_xf1.z != 0.0 || v_xf1.w != 0.0 || v_xf2.x != 0.0) {\n"   /* in perspective the magnification is not one number: what one pixel spans of the element, here */
    "    float lpp = 0.5 * (length(dFdx(v_local)) + length(dFdy(v_local)));\n"
    "    us = lpp > 1e-6 ? 1.0 / lpp : 1e6;\n"
    "  }\n"
    "  vec2 sp = vec2(gl_FragCoord.x, u_viewport.y * u_scale - gl_FragCoord.y) / u_scale;\n"   /* the pixel's center in the window: the clip is there */
    "  if (sp.x < v_clip.x || sp.y < v_clip.y ||\n"
    "      sp.x > v_clip.x + v_clip.z || sp.y > v_clip.y + v_clip.w) discard;\n"
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
    "  } else if (kind == 9.0) {\n" /* a piece of a stroke: the capsule of segment a-b (v_uvr), half width v_radii.x, the style of its two ends in v_radii.yz (0 round, 1 miter, 2 flat),
                                      the planes that divide it from its neighbours in v_s0 (a normal pointing out of this piece's side; zero: none) */
    "    vec2 a = v_uvr.xy, b = v_uvr.zw, p = v_local, na = v_s0.xy, nb = v_s0.zw;\n"
    "    float hw = v_radii.x, cov = 0.0, u = 0.0;\n"
    "    bool mine = !((dot(na, na) > 0.0 && dot(p - a, na) >= -0.001) || (dot(nb, nb) > 0.0 && dot(p - b, nb) > 0.001));\n"   /* (a point on the plane is the A piece's: a hair of slack, so the two pieces' rounding cannot make it both's or neither's) */
    "    if (mine) {\n"
    "      vec2 ba = b - a, pa = p - a;\n"
    "      float L = length(ba), d;\n"
    "      if (L < 1e-5) {\n"
    "        d = length(pa) - hw;\n"
    "      } else {\n"
    "        vec2 dir = ba / L;\n"
    "        float t = dot(pa, dir), perp = abs(dot(pa, vec2(-dir.y, dir.x)));\n"
    "        u = clamp(t / L, 0.0, 1.0);\n"
    "        if (t < 0.0) d = v_radii.y < 0.5 ? length(pa) - hw : (v_radii.y < 1.5 ? perp - hw : max(perp - hw, -t));\n"
    "        else if (t > L) d = v_radii.z < 0.5 ? length(p - b) - hw : (v_radii.z < 1.5 ? perp - hw : max(perp - hw, t - L));\n"
    "        else d = perp - hw;\n"
    "      }\n"
    "      cov = clamp(0.5 - d * us, 0.0, 1.0);\n"
    "    }\n"
    "    vec4 sc = v_bsty.z > 0.0 ? texture(u_ramps, vec2(mix(v_border.x, v_border.y, u), v_bsty.z)) * v_color.a : premul(v_color);\n" /* along a gradient: v_border.xy the piece's ends on it */
    "    frag = sc * cov;\n"
    "  } else if (kind == 10.0) {\n" /* a triangle (corners v_uvr.xy, v_uvr.zw, v_s0.xy); each edge (v_radii.xyz) soft (0), hard on a line shared with a neighbour (1: a pixel on
                                      the line is this one's, -1: the neighbour's), or an arrowhead's base (2 / -2: hard where the stroke meets it, within v_radii.w of its middle) */
    /* the pixel's center in the window, then in the layout: the same numbers for every instance, so two triangles sharing an edge see one line */
    "    vec3 ph = inverse(mat3(1.0 + v_xf0.x, v_xf0.y, v_xf1.z, v_xf0.z, 1.0 + v_xf0.w, v_xf1.w, v_xf1.x, v_xf1.y, 1.0 + v_xf2.x)) * vec3(sp, 1.0);\n"
    "    vec2 p = ph.xy / ph.z;\n"
    "    vec2 tv[3] = vec2[3](v_uvr.xy, v_uvr.zw, v_s0.xy);\n"
    "    float cov = 1.0;\n"
    "    for (int i = 0; i < 3; i++) {\n"
    "      vec2 a = tv[i], b = tv[(i + 1) % 3], c = tv[(i + 2) % 3];\n"
    "      if (a.x > b.x || (a.x == b.x && a.y > b.y)) { vec2 t = a; a = b; b = t; }\n" /* the edge's ends in one order, whichever triangle asks */
    "      vec2 e = b - a;\n"
    "      float le = length(e);\n"
    "      if (le < 1e-6) continue;\n"
    "      vec2 nn = vec2(e.y, -e.x) / le;\n"
    "      float d = dot(p - a, nn), mode = v_radii[i];\n"
    "      if (dot(c - a, nn) > 0.0) d = -d;\n" /* positive outside this triangle */
    "      if (abs(mode) > 1.5) {\n" /* an arrowhead's base: shared with a stroke piece, which divides by its own plane, with a hair of slack either way */
    "        if (abs(dot(p - (a + b) * 0.5, e / le)) < v_radii.w + 0.5 / us) { if (d >= (mode > 0.0 ? 0.001 : -0.001)) cov = 0.0; continue; }\n"
    "        mode = 0.0;\n"
    "      }\n"
    "      if (mode > 0.0) { if (d > 0.0) cov = 0.0; }\n"   /* a line on the line is the owner's: exactly one of the two takes it */
    "      else if (mode < 0.0) { if (d >= 0.0) cov = 0.0; }\n"
    "      else if (d > 0.0 || abs(dot(p - (a + b) * 0.5, e / le)) <= le * 0.5) cov = min(cov, clamp(0.5 - d * us, 0.0, 1.0));\n" /* (inside, the edge's line past its ends is not the outline: another piece's edge is) */
    "    }\n"
    "    frag = premul(v_color) * cov;\n"
    "  } else if (kind == 12.0) {\n" /* the end of a layer: its texture, which is the window's size, at this very pixel, cut by its mask, at the layer's opacity, blended */
    "    ivec2 q = ivec2(gl_FragCoord.xy);\n"
    "    vec4 sc = texelFetch(u_layer, q, 0);\n"
    "    if (v_uvr.y > 0.5) {\n"   /* masked: by the mask layer's alpha, or (2) its luminance times its alpha */
    "      vec4 m = texelFetch(u_aux, q, 0);\n"
    "      sc *= v_uvr.y < 1.5 ? m.a : (m.a > 0.0 ? dot(m.rgb / m.a, vec3(0.2126, 0.7152, 0.0722)) * m.a : 0.0);\n"
    "    }\n"
    "    sc *= v_color.a;\n"
    "    if (v_uvr.x > 0.5) {\n"   /* a blend mode: the result in full (blending is off for this quad), from what is under it */
    "      vec4 db = texelFetch(u_backdrop, q, 0);\n"
    "      vec3 cs = sc.a > 0.0 ? sc.rgb / sc.a : vec3(0.0), cb = db.a > 0.0 ? db.rgb / db.a : vec3(0.0);\n"
    "      vec3 bl = clamp(blend3(int(v_uvr.x + 0.5), cb, cs), 0.0, 1.0);\n"
    "      sc = vec4(sc.rgb * (1.0 - db.a) + db.rgb * (1.0 - sc.a) + sc.a * db.a * bl, sc.a + db.a * (1.0 - sc.a));\n"
    "    }\n"
    "    frag = sc;\n"
    "  } else if (kind == 18.0) {\n" /* a backdrop filter's result, in the element's rounded box */
    "    float cov = clamp(0.5 - sd_rounded(v_local, half_size, v_radii) * us, 0.0, 1.0);\n"
    "    frag = texelFetch(u_layer, ivec2(gl_FragCoord.xy), 0) * (cov * v_color.a);\n"
    "  } else if (kind == 13.0) {\n" /* a color filter: a matrix over straight RGBA (rows in color, border, radii, uvr; offsets in s0) */
    "    vec4 t = texelFetch(u_layer, ivec2(gl_FragCoord.xy), 0);\n"
    "    vec4 c = t.a > 0.0 ? vec4(t.rgb / t.a, t.a) : vec4(0.0);\n"
    "    vec4 o = clamp(vec4(dot(v_color, c), dot(v_border, c), dot(v_radii, c), dot(v_uvr, c)) + v_s0, 0.0, 1.0);\n"
    "    frag = vec4(o.rgb * o.a, o.a);\n"
    "  } else if (kind == 14.0) {\n" /* a blur along one axis (s0.x: 0 across, 1 down) */
    "    frag = gauss(u_layer, ivec2(gl_FragCoord.xy), v_s0.x > 0.5 ? ivec2(0, 1) : ivec2(1, 0), v_params.z * u_scale);\n"
    "  } else if (kind == 15.0) {\n" /* a drop shadow: the layer over its shape (the aside copy, blurred across already) blurred down, moved by s0.xy and colored */
    "    ivec2 q = ivec2(gl_FragCoord.xy);\n"
    "    ivec2 off = ivec2(int(floor(v_s0.x * u_scale + 0.5)), -int(floor(v_s0.y * u_scale + 0.5)));\n"
    "    float a = gauss(u_aux, q - off, ivec2(0, 1), v_params.z * u_scale).a;\n"
    "    vec4 sh = premul(v_color) * a;\n"
    "    vec4 t = texelFetch(u_layer, q, 0);\n"
    "    frag = t + sh * (1.0 - t.a);\n"
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

/* the window-sized targets: one layer per depth, then two for filter passes to go back and forth between, one aside (a drop shadow's blurred shape),
 * and one for what is under a layer with a blend mode */
enum { T_FX0 = GP_MAX_LAYERS, T_FX1, T_AUX, T_BACKDROP, T_COUNT };

struct gp_renderer {
    GLuint prog, vao, vbo, atlas, color_atlas, lcd_atlas, ramps;
    GLint u_viewport, u_scale, u_gamma, u_atlas, u_color_atlas, u_lcd_atlas, u_ramps, u_image, u_layer, u_aux, u_backdrop;
    size_t vbo_cap;
    /* offscreen targets (made when first needed, again when the window's size changes) */
    struct { GLuint fbo, tex; } layers[T_COUNT];
    int layer_w, layer_h;
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
    r->u_layer = gl.glGetUniformLocation(r->prog, "u_layer");
    r->u_aux = gl.glGetUniformLocation(r->prog, "u_aux");
    r->u_backdrop = gl.glGetUniformLocation(r->prog, "u_backdrop");

    gl.glGenVertexArrays(1, &r->vao);
    gl.glBindVertexArray(r->vao);
    gl.glGenBuffers(1, &r->vbo);
    gl.glBindBuffer(GL_ARRAY_BUFFER, r->vbo);
    for (GLuint i = 0; i < 14; i++) {
        /* gp_instance is fourteen consecutive vec4s */
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
    for (int i = 0; i < T_COUNT; i++) {
        if (r->layers[i].fbo) gl.glDeleteFramebuffers(1, &r->layers[i].fbo);
        if (r->layers[i].tex) gl.glDeleteTextures(1, &r->layers[i].tex);
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
    for (GLuint a = 0; a < 14; a++)
        gl.glVertexAttribPointer(a, 4, GL_FLOAT, GL_FALSE, sizeof(gp_instance),
                                 (const void *)(uintptr_t)(first * sizeof(gp_instance) + a * 4 * sizeof(float)));
    gl.glDrawArraysInstanced(GL_TRIANGLE_STRIP, 0, 4, (GLsizei)count);
    (void)r;
}

/* Target d (a layer for depth d, or T_*), the size of the window: made when first needed; all of them made again when the window's size changed. */
static bool layer_target(gp_renderer *r, int d, int px_w, int px_h) {
    if (r->layer_w != px_w || r->layer_h != px_h) {
        for (int i = 0; i < T_COUNT; i++) {
            if (r->layers[i].fbo) gl.glDeleteFramebuffers(1, &r->layers[i].fbo);
            if (r->layers[i].tex) gl.glDeleteTextures(1, &r->layers[i].tex);
            r->layers[i].fbo = r->layers[i].tex = 0;
        }
        r->layer_w = px_w;
        r->layer_h = px_h;
    }
    if (r->layers[d].fbo) return true;
    GLuint tex, fbo;
    gl.glGenTextures(1, &tex);
    gl.glActiveTexture(GL_TEXTURE5);
    gl.glBindTexture(GL_TEXTURE_2D, tex);
    gl.glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, px_w, px_h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    gl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    gl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    gl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    gl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    gl.glBindTexture(GL_TEXTURE_2D, 0);
    gl.glActiveTexture(GL_TEXTURE0);
    gl.glGenFramebuffers(1, &fbo);
    gl.glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    gl.glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
    bool ok = gl.glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
    if (!ok) {
        static bool warned;
        if (!warned) { warned = true; fprintf(stderr, "gpuic: an offscreen layer could not be made: layers are drawn straight onto the window\n"); }
        gl.glDeleteFramebuffers(1, &fbo);
        gl.glDeleteTextures(1, &tex);
        return false;
    }
    r->layers[d].fbo = fbo;
    r->layers[d].tex = tex;
    return true;
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
    gl.glUniform1i(r->u_layer, 5);
    gl.glUniform1i(r->u_aux, 6);
    gl.glUniform1i(r->u_backdrop, 7);

    gl.glBindVertexArray(r->vao);
    gl.glBindBuffer(GL_ARRAY_BUFFER, r->vbo);
    size_t bytes = scene->len * sizeof(gp_instance);
    gl.glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)bytes, NULL, GL_STREAM_DRAW); /* orphan */
    gl.glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)bytes, scene->items, GL_STREAM_DRAW);
    /* Everything but images and layers is one instanced draw. An image has a texture of its own, so it splits the scene: the run
     * before it, then the image alone, then on. A layer's BEGIN sends what follows into the layer's texture (only its region cleared), and its END
     * goes back to where it was and composites the region (the END is that quad). Order is kept, which is what painting needs. */
    gp_gfx *gfx = gp_text_gfx(ts);
    size_t run = 0;
    int depth = 0;       /* layers open */
    int skipped = 0;     /* ... of which these could not be made, inside the open ones: drawn straight on what is under them */
    int cur = -1;        /* where the layer about to end is now, after the filter passes so far (-1: its own target) */
    GLuint mask_tex = 0; /* the mask of the layer about to end */
    bool backdrop = false;   /* between a BACKDROP and its END: the passes are over the copy of what is under it */
    for (size_t i = 0; i < scene->len; i++) {
        const gp_instance *in = &scene->items[i];
        int kind = (int)in->kind;
        if (kind == GP_PRIM_BACKDROP) {   /* what is under the region, out of the target being drawn on (bound now), into the backdrop texture */
            if (i > run) draw_run(r, scene, run, i - run);
            run = i + 1;
            if (!layer_target(r, T_BACKDROP, px_w, px_h)) continue;
            int x0 = (int)floorf(in->bounds[0] * scale + 0.5f), x1 = (int)floorf((in->bounds[0] + in->bounds[2]) * scale + 0.5f);
            int y0 = (int)floorf(in->bounds[1] * scale + 0.5f), y1 = (int)floorf((in->bounds[1] + in->bounds[3]) * scale + 0.5f);
            if (x0 < 0) x0 = 0;
            if (y0 < 0) y0 = 0;
            if (x1 > px_w) x1 = px_w;
            if (y1 > px_h) y1 = px_h;
            gl.glActiveTexture(GL_TEXTURE7);
            gl.glBindTexture(GL_TEXTURE_2D, r->layers[T_BACKDROP].tex);
            if (x1 > x0 && y1 > y0) gl.glCopyTexSubImage2D(GL_TEXTURE_2D, 0, x0, px_h - y1, x0, px_h - y1, x1 - x0, y1 - y0);
            gl.glBindTexture(GL_TEXTURE_2D, 0);
            gl.glActiveTexture(GL_TEXTURE0);
            cur = T_BACKDROP;
            backdrop = true;
            continue;
        }
        if (kind == GP_PRIM_BACKDROP_END) {   /* back on the target, the result drawn in the box */
            if (i > run) draw_run(r, scene, run, i - run);
            run = i + 1;
            if (!backdrop) continue;
            backdrop = false;
            gl.glBindFramebuffer(GL_FRAMEBUFFER, depth ? r->layers[depth - 1].fbo : 0);
            gl.glActiveTexture(GL_TEXTURE5);
            gl.glBindTexture(GL_TEXTURE_2D, r->layers[cur].tex);
            gl.glActiveTexture(GL_TEXTURE0);
            draw_run(r, scene, i, 1);
            gl.glActiveTexture(GL_TEXTURE5);
            gl.glBindTexture(GL_TEXTURE_2D, 0);
            gl.glActiveTexture(GL_TEXTURE0);
            cur = -1;
            continue;
        }
        if (kind == GP_PRIM_FX_MATRIX || kind == GP_PRIM_FX_BLUR || kind == GP_PRIM_FX_SHADOW) {
            /* a pass over the open layer: from where it is now into the other of the two pass targets (a shadow's first pass: into the aside one) */
            if (i > run) draw_run(r, scene, run, i - run);
            run = i + 1;
            if (!backdrop && (skipped || !depth)) continue;
            int src = cur >= 0 ? cur : depth - 1;
            int dst = kind == GP_PRIM_FX_BLUR && in->side[0][1] > 0.5f ? T_AUX : (cur == T_FX0 ? T_FX1 : T_FX0);
            if (!layer_target(r, dst, px_w, px_h)) continue;
            gl.glBindFramebuffer(GL_FRAMEBUFFER, r->layers[dst].fbo);
            gl.glActiveTexture(GL_TEXTURE5);
            gl.glBindTexture(GL_TEXTURE_2D, r->layers[src].tex);
            gl.glActiveTexture(GL_TEXTURE6);
            gl.glBindTexture(GL_TEXTURE_2D, kind == GP_PRIM_FX_SHADOW ? r->layers[T_AUX].tex : 0);
            gl.glActiveTexture(GL_TEXTURE0);
            gl.glDisable(GL_BLEND);   /* (a pass writes its result whole) */
            draw_run(r, scene, i, 1);
            gl.glEnable(GL_BLEND);
            gl.glActiveTexture(GL_TEXTURE5);
            gl.glBindTexture(GL_TEXTURE_2D, 0);
            gl.glActiveTexture(GL_TEXTURE6);
            gl.glBindTexture(GL_TEXTURE_2D, 0);
            gl.glActiveTexture(GL_TEXTURE0);
            if (dst != T_AUX) cur = dst;
            continue;
        }
        if (kind == GP_PRIM_MASK_END) {   /* the mask's layer is done: it is kept for the END that follows (its own layer goes on being the open one) */
            if (i > run) draw_run(r, scene, run, i - run);
            run = i + 1;
            if (skipped) { skipped--; continue; }
            if (!depth) continue;
            depth--;
            mask_tex = r->layers[depth].tex;
            gl.glBindFramebuffer(GL_FRAMEBUFFER, depth ? r->layers[depth - 1].fbo : 0);
            continue;
        }
        if (kind == GP_PRIM_LAYER_BEGIN) {
            if (i > run) draw_run(r, scene, run, i - run);
            run = i + 1;
            if (skipped || depth >= GP_MAX_LAYERS || !layer_target(r, depth, px_w, px_h)) { skipped++; continue; }
            gl.glBindFramebuffer(GL_FRAMEBUFFER, r->layers[depth].fbo);
            int x0 = (int)floorf(in->bounds[0] * scale + 0.5f), x1 = (int)floorf((in->bounds[0] + in->bounds[2]) * scale + 0.5f);
            int y0 = (int)floorf(in->bounds[1] * scale + 0.5f), y1 = (int)floorf((in->bounds[1] + in->bounds[3]) * scale + 0.5f);
            if (x0 < 0) x0 = 0;
            if (y0 < 0) y0 = 0;
            if (x1 > px_w) x1 = px_w;
            if (y1 > px_h) y1 = px_h;
            if (x1 > x0 && y1 > y0) {
                gl.glEnable(GL_SCISSOR_TEST);
                gl.glScissor(x0, px_h - y1, x1 - x0, y1 - y0);   /* (GL counts rows from the bottom) */
                gl.glClearColor(0, 0, 0, 0);
                gl.glClear(GL_COLOR_BUFFER_BIT);
                gl.glDisable(GL_SCISSOR_TEST);
            }
            depth++;
            continue;
        }
        if (kind == GP_PRIM_LAYER_END) {
            if (i > run) draw_run(r, scene, run, i - run);
            run = i + 1;
            if (skipped) { skipped--; continue; }
            if (!depth) continue;
            depth--;
            gl.glBindFramebuffer(GL_FRAMEBUFFER, depth ? r->layers[depth - 1].fbo : 0);
            gl.glActiveTexture(GL_TEXTURE5);
            gl.glBindTexture(GL_TEXTURE_2D, r->layers[cur >= 0 ? cur : depth].tex);
            gl.glActiveTexture(GL_TEXTURE6);
            gl.glBindTexture(GL_TEXTURE_2D, in->uv[1] > 0.5f ? mask_tex : 0);
            bool blended = in->uv[0] > 0.5f && layer_target(r, T_BACKDROP, px_w, px_h);
            if (blended) {   /* what is under it, over the region, into the backdrop texture: the quad computes the blend and writes it whole */
                int x0 = (int)floorf(in->bounds[0] * scale + 0.5f), x1 = (int)floorf((in->bounds[0] + in->bounds[2]) * scale + 0.5f);
                int y0 = (int)floorf(in->bounds[1] * scale + 0.5f), y1 = (int)floorf((in->bounds[1] + in->bounds[3]) * scale + 0.5f);
                if (x0 < 0) x0 = 0;
                if (y0 < 0) y0 = 0;
                if (x1 > px_w) x1 = px_w;
                if (y1 > px_h) y1 = px_h;
                gl.glActiveTexture(GL_TEXTURE7);
                gl.glBindTexture(GL_TEXTURE_2D, r->layers[T_BACKDROP].tex);
                if (x1 > x0 && y1 > y0) gl.glCopyTexSubImage2D(GL_TEXTURE_2D, 0, x0, px_h - y1, x0, px_h - y1, x1 - x0, y1 - y0);
                gl.glDisable(GL_BLEND);
            }
            gl.glActiveTexture(GL_TEXTURE0);
            draw_run(r, scene, i, 1);
            if (blended) gl.glEnable(GL_BLEND);
            for (GLenum u = GL_TEXTURE5; u <= GL_TEXTURE7; u++) {   /* (none bound while it is drawn into again) */
                gl.glActiveTexture(u);
                gl.glBindTexture(GL_TEXTURE_2D, 0);
            }
            gl.glActiveTexture(GL_TEXTURE0);
            cur = -1;
            mask_tex = 0;
            continue;
        }
        if (kind != GP_PRIM_IMAGE) continue;
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
    if (depth) gl.glBindFramebuffer(GL_FRAMEBUFFER, 0);   /* (a scene whose layers do not close: what was in them is lost, the window is what is shown) */
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
