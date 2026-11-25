// The fragment shader of the one instanced draw: each kind of primitive (internal.h's GP_PRIM_*) shaded by signed distances.
// tools/gen_shaders.py makes src/shaders.inc from this (see quad.vert).
flat in vec4 v_bounds, v_clip, v_color, v_border, v_radii, v_params, v_uvr, v_s0, v_s1, v_s2, v_bsty;
flat in float v_mag;
flat in vec4 v_xf0, v_xf1, v_xf2;
in vec2 v_local;
in vec2 v_pos;
in vec2 v_uv;
#ifdef VULKAN
layout(push_constant) uniform Push { vec2 u_viewport; float u_scale; float u_gamma; };
#define BINDING(n) layout(set = 0, binding = n)
#define WINDOW_ROWS_DOWN 1  // gl_FragCoord counts rows from the top, as the layout does
#else
uniform vec2 u_viewport;
uniform float u_scale;
uniform float u_gamma;
#define BINDING(n)
#define WINDOW_ROWS_DOWN 0  // (GL's rows go up, the layout's down)
#endif
BINDING(0) uniform sampler2D u_atlas;
BINDING(1) uniform sampler2D u_color_atlas;
BINDING(2) uniform sampler2D u_lcd_atlas;
BINDING(3) uniform sampler2D u_ramps;
BINDING(4) uniform sampler2D u_image;
BINDING(5) uniform sampler2D u_layer;  // a layer (or a filter pass's input)
BINDING(6) uniform sampler2D u_aux;  // a drop shadow's blurred copy; at a layer's end, its mask
BINDING(7) uniform sampler2D u_backdrop;  // at the end of a layer with a blend mode: what was under it
BINDING(8) uniform sampler2D u_cuts;  // the lines pieces of planes in a 3D context are cut along (GP_CUTS in internal.h)
layout(location = 0, index = 0) out vec4 frag;
layout(location = 0, index = 1) out vec4 frag_cov;  // what each channel of the destination keeps: 1 - this
float sd_rounded(vec2 p, vec2 half_size, vec4 r4) {
  float r = p.x < 0.0 ? (p.y < 0.0 ? r4.x : r4.w) : (p.y < 0.0 ? r4.y : r4.z);
  vec2 q = abs(p) - half_size + r;
  return min(max(q.x, q.y), 0.0) + length(max(q, 0.0)) - r;
}
float erf_approx(float x) {
  float x2 = x * x;
  float t = (1.2732395 + 0.147 * x2) / (1.0 + 0.147 * x2);
  return sign(x) * sqrt(1.0 - exp(-x2 * t));
}
vec4 premul(vec4 c) { return vec4(c.rgb * c.a, c.a); }
// a glyph's bitmap at uv between its texels (bilinear, by hand): texels outside its own rectangle r (uv origin and size) count as empty, so nothing of the glyphs next to it in the atlas comes in, however far the quad grew
vec4 glyph_texel(sampler2D t, vec2 uv, vec4 r) {
  vec2 sz = vec2(textureSize(t, 0));
  vec2 lo = r.xy * sz, hi = (r.xy + r.zw) * sz;
  vec2 p = uv * sz - 0.5, f = fract(p);
  ivec2 i0 = ivec2(floor(p));
  vec4 acc = vec4(0.0);
  for (int k = 0; k < 4; k++) {
    ivec2 q = i0 + ivec2(k & 1, k >> 1);
    vec2 c = vec2(q) + 0.5;
    float wgt = ((k & 1) == 1 ? f.x : 1.0 - f.x) * ((k >> 1) == 1 ? f.y : 1.0 - f.y);
    if (all(greaterThanEqual(c, lo)) && all(lessThan(c, hi))) acc += texelFetch(t, q, 0) * wgt;
  }
  return acc;
}
// a border side (0 left, 1 top, 2 right, 3 bottom): how its distance (in units of its width) grows, and its paint: its color or the gradient, then dashes or dots
vec2 side_grad(int s, vec4 w) {
  float ws = s == 0 ? w.x : s == 1 ? w.y : s == 2 ? w.z : w.w;
  if (ws <= 0.0) return vec2(0.0);
  return s == 0 ? vec2(1.0 / ws, 0.0) : s == 1 ? vec2(0.0, 1.0 / ws) : s == 2 ? vec2(-1.0 / ws, 0.0) : vec2(0.0, -1.0 / ws);
}
vec4 side_ring(int s, vec2 p, vec2 half_size, vec4 w, vec4 col, float us) {
  vec4 ring = premul(v_border);
  if (v_bsty.y > 0.5) ring = s == 0 ? premul(v_border) : s == 1 ? premul(v_s0) : s == 2 ? premul(v_s1) : premul(v_s2);
  if (v_bsty.z > 0.0) {  // a gradient over the box: the same mapping as a background's
    float gt;
    if (v_bsty.w == 5.0) gt = dot(v_local, v_s0.xy) / v_s0.z + 0.5;
    else if (v_bsty.w == 6.0) gt = length(v_local - v_s0.xy) / v_s0.z;
    else { vec2 gd = v_local - v_s0.xy; gt = fract((atan(gd.x, -gd.y) - v_s0.z) / 6.28318530718); }
    ring = texture(u_ramps, vec2(clamp(gt, 0.0, 1.0), v_bsty.z)) * v_s0.w;
  }
  if (v_bsty.x > 0.5) {  // dashes: 2 widths and a gap of 1; dots: circles of the width, 2 apart: along the side from its start, and across it
    float wd = s == 0 ? w.x : s == 1 ? w.y : s == 2 ? w.z : w.w;
    float along = (s == 0 || s == 2) ? p.y + half_size.y : p.x + half_size.x;
    float across = s == 0 ? p.x + half_size.x : s == 1 ? p.y + half_size.y : s == 2 ? half_size.x - p.x : half_size.y - p.y;
    float mask;
    if (v_bsty.x < 1.5) {
      float per = 3.0 * wd, ph = mod(along, per);
      mask = clamp(min(ph, 2.0 * wd - ph) * us + 0.5, 0.0, 1.0);  // the dash's edges, each a pixel wide
      mask = ph < 2.0 * wd ? mask : clamp(0.5 - min(ph - 2.0 * wd, per - ph) * us, 0.0, 1.0);
    } else {
      float per = 2.0 * wd, cx0 = (floor(along / per) + 0.5) * per;
      float dd = length(vec2(along - cx0, across - wd * 0.5)) - wd * 0.5;
      mask = clamp(0.5 - dd * us, 0.0, 1.0);
    }
    ring = mix(col, ring, mask);  // between the dashes the fill shows
  }
  return ring;
}
// Text coverage adjusted so that blending in the encoded (gamma) space gives what blending in linear light would, for text of luma `lt` on a background of the opposite luma (a background that is not that is the technique's limit). u_gamma 1 is no adjustment.
float tone(float c, float lt) {
  if (u_gamma == 1.0) return c;
  float lb = 1.0 - lt, d = lt - lb;
  if (abs(d) < 0.02) return c;
  float g = pow((1.0 - c) * pow(lb, u_gamma) + c * pow(lt, u_gamma), 1.0 / u_gamma);
  return clamp((g - lb) / d, 0.0, 1.0);
}
// filter passes and layers: pixels of the window-sized textures; a pixel is read only inside the layer's region (v_clip), outside it is transparent
bool in_region(ivec2 q) {
  vec2 c = (vec2(q) + 0.5) / u_scale;
  if (WINDOW_ROWS_DOWN == 0) c.y = u_viewport.y - c.y;
  return c.x >= v_clip.x && c.y >= v_clip.y && c.x < v_clip.x + v_clip.z && c.y < v_clip.y + v_clip.w;
}
// inside the region, past the window's edge, the edge's pixel (nothing was drawn there: it goes on as it was)
vec4 fetch(sampler2D t, ivec2 q) { return in_region(q) ? texelFetch(t, clamp(q, ivec2(0), textureSize(t, 0) - 1), 0) : vec4(0.0); }
// a Gaussian along `dir` (whole pixels), sigma in device px; wide ones are sampled every few pixels
vec4 gauss(sampler2D t, ivec2 q, ivec2 dir, float sigma) {
  if (sigma < 0.3) return fetch(t, q);
  int n = int(ceil(3.0 * sigma)), step = max(1, (n + 47) / 48);
  vec4 acc = vec4(0.0); float wsum = 0.0;
  for (int k = -n; k <= n; k += step) {
    float w = exp(-float(k * k) / (2.0 * sigma * sigma));
    acc += fetch(t, q + dir * k) * w; wsum += w;
  }
  return acc / wsum;
}
// blend modes, as the W3C compositing spec defines them, on straight colors: cb under, cs over
float blend1(int m, float b, float s) {
  if (m == 1) return b * s;
  if (m == 2) return b + s - b * s;
  if (m == 3) return b <= 0.5 ? s * 2.0 * b : s + (2.0 * b - 1.0) - s * (2.0 * b - 1.0);  // overlay: hard light with the two swapped
  if (m == 4) return min(b, s);
  if (m == 5) return max(b, s);
  if (m == 6) return b <= 0.0 ? 0.0 : (s >= 1.0 ? 1.0 : min(1.0, b / (1.0 - s)));
  if (m == 7) return b >= 1.0 ? 1.0 : (s <= 0.0 ? 0.0 : 1.0 - min(1.0, (1.0 - b) / s));
  if (m == 8) return s <= 0.5 ? b * 2.0 * s : b + (2.0 * s - 1.0) - b * (2.0 * s - 1.0);
  if (m == 9) {
    if (s <= 0.5) return b - (1.0 - 2.0 * s) * b * (1.0 - b);
    float d = b <= 0.25 ? ((16.0 * b - 12.0) * b + 4.0) * b : sqrt(b);
    return b + (2.0 * s - 1.0) * (d - b);
  }
  if (m == 10) return abs(b - s);
  if (m == 11) return b + s - 2.0 * b * s;
  return s;
}
float lum(vec3 c) { return dot(c, vec3(0.3, 0.59, 0.11)); }
vec3 clip_color(vec3 c) {
  float l = lum(c), n = min(min(c.r, c.g), c.b), x = max(max(c.r, c.g), c.b);
  if (n < 0.0) c = l + (c - l) * l / (l - n);
  if (x > 1.0) c = l + (c - l) * (1.0 - l) / (x - l);
  return c;
}
vec3 set_lum(vec3 c, float l) { return clip_color(c + (l - lum(c))); }
float sat(vec3 c) { return max(max(c.r, c.g), c.b) - min(min(c.r, c.g), c.b); }
vec3 set_sat(vec3 c, float s) {
  float x = max(max(c.r, c.g), c.b), n = min(min(c.r, c.g), c.b);
  return x > n ? (c - n) * s / (x - n) : vec3(0.0);
}
vec3 blend3(int m, vec3 b, vec3 s) {
  if (m == 12) return set_lum(set_sat(s, sat(b)), lum(b));
  if (m == 13) return set_lum(set_sat(b, sat(s)), lum(b));
  if (m == 14) return set_lum(s, lum(b));
  if (m == 15) return set_lum(b, lum(s));
  return vec3(blend1(m, b.r, s.r), blend1(m, b.g, s.g), blend1(m, b.b, s.b));
}
void main() {
  float us = u_scale * v_mag;
  if (v_xf1.z != 0.0 || v_xf1.w != 0.0 || v_xf2.x != 0.0) {  // in perspective the magnification is not one number: what one pixel spans of the element, here
    float lpp = 0.5 * (length(dFdx(v_local)) + length(dFdy(v_local)));
    us = lpp > 1e-6 ? 1.0 / lpp : 1e6;
  }
  vec2 fc = WINDOW_ROWS_DOWN == 1 ? gl_FragCoord.xy : vec2(gl_FragCoord.x, u_viewport.y * u_scale - gl_FragCoord.y);  // the pixel's center in the window, rows down
  vec2 sp = fc / u_scale;  // ... in logical px: the clip is there
  if (sp.x < v_clip.x || sp.y < v_clip.y ||
      sp.x > v_clip.x + v_clip.z || sp.y > v_clip.y + v_clip.w) discard;
  if (v_xf2.y > 0.0) {  // a piece of a plane: only on its side of each line (a x + b y + c >= 0, or > 0)
    int cuts = int(v_xf2.y + 0.5), from = cuts / 16;
    for (int i = 0; i < cuts - from * 16; i++) {
      int k = from + i;
      vec4 l = texelFetch(u_cuts, ivec2(k % 256, k / 256), 0);
      float v = dot(l.xyz, vec3(sp, 1.0));
      if (l.w > 0.5 ? v <= 0.0 : v < 0.0) {
        discard;
      }
    }
  }
  float kind = v_params.y;
  vec2 half_size = v_bounds.zw * 0.5;
  vec4 lcd_cov = vec4(0.0);
  bool turned = v_xf0 != vec4(0.0) || v_xf1.z != 0.0 || v_xf1.w != 0.0 || v_xf2.x != 0.0;  // (a glyph so: sampled between texels, see glyph_texel)
  if (kind == 4.0) {  // LCD text: a coverage per channel, blended per channel
    vec3 cov = (turned ? glyph_texel(u_lcd_atlas, v_uv, v_uvr) : texture(u_lcd_atlas, v_uv)).rgb;
    float lt = dot(v_color.rgb, vec3(0.2126, 0.7152, 0.0722));
    cov = vec3(tone(cov.r, lt), tone(cov.g, lt), tone(cov.b, lt));
    frag = vec4(v_color.rgb * v_color.a * cov, v_color.a * (cov.r + cov.g + cov.b) / 3.0);
    lcd_cov = vec4(cov * v_color.a, frag.a);
  } else if (kind == 7.0) {  // an image: the texture rectangle is in v_uvr; the corners are cut like a rounded box's
    vec2 q = clamp(v_local / v_bounds.zw + 0.5, 0.0, 1.0);
    float mask = clamp(0.5 - sd_rounded(v_local, half_size, v_radii) * us, 0.0, 1.0);
    vec2 uv = v_uvr.xy + q * v_uvr.zw;
    float blur = v_params.z >= 500.0 ? v_params.z - 1000.0 : v_params.z;  // params.z: the blur radius, plus 1000 for pixelated
    vec4 t;
    if (blur > 0.0) {  // a 3x3 tap blur of a coarser mip level, wide enough to cover the radius
      vec2 ruv = blur * v_uvr.zw / v_bounds.zw;
      float lod = max(log2(max(ruv.x * float(textureSize(u_image, 0).x), 1.0)) - 1.0, 0.0);
      t = vec4(0.0);
      for (int i = -1; i <= 1; i++)
        for (int j = -1; j <= 1; j++)
          t += textureLod(u_image, clamp(uv + vec2(float(i), float(j)) * ruv * 0.8, v_uvr.xy, v_uvr.xy + v_uvr.zw), lod) * (float(2 - abs(i)) * float(2 - abs(j)) / 16.0);
    } else {
      t = texture(u_image, uv);
    }
    vec3 c = t.a > 0.0 ? t.rgb / t.a : vec3(0.0);  // the filters work on the color, not on its premultiplied form
    float brightness = 1.0 + v_border.z, saturate = 1.0 + v_border.w, contrast = 1.0 + v_params.x;
    c *= brightness;
    c = (c - 0.5) * contrast + 0.5;
    float luma = dot(c, vec3(0.2126, 0.7152, 0.0722));
    c = mix(vec3(luma), c, saturate);
    c = mix(c, vec3(dot(c, vec3(0.2126, 0.7152, 0.0722))), v_border.y);
    c = clamp(c * v_color.rgb, 0.0, 1.0);
    frag = vec4(c * t.a, t.a) * (v_color.a * mask);
  } else if (kind == 3.0) {
    frag = (turned ? glyph_texel(u_color_atlas, v_uv, v_uvr) : texture(u_color_atlas, v_uv)) * v_color.a;  // premultiplied RGBA; alpha carries opacity
  } else if (kind == 2.0) {
    float a = tone((turned ? glyph_texel(u_atlas, v_uv, v_uvr) : texture(u_atlas, v_uv)).r, dot(v_color.rgb, vec3(0.2126, 0.7152, 0.0722)));
    frag = premul(v_color) * a;
  } else if (kind == 9.0) {
// a piece of a stroke: the capsule of segment a-b (v_uvr), half width v_radii.x, the style of its two ends in v_radii.yz (0 round, 1 miter, 2 flat), the planes that divide it from its neighbours in v_s0 (a normal pointing out of this piece's side; zero: none)
    vec2 a = v_uvr.xy, b = v_uvr.zw, p = v_local, na = v_s0.xy, nb = v_s0.zw;
    float hw = v_radii.x, cov = 0.0, u = 0.0;
    bool mine = !((dot(na, na) > 0.0 && dot(p - a, na) >= -0.001) || (dot(nb, nb) > 0.0 && dot(p - b, nb) > 0.001));  // (a point on the plane is the A piece's: a hair of slack, so the two pieces' rounding cannot make it both's or neither's)
    if (mine) {
      vec2 ba = b - a, pa = p - a;
      float L = length(ba), d;
      if (L < 1e-5) {
        d = length(pa) - hw;
      } else {
        vec2 dir = ba / L;
        float t = dot(pa, dir), perp = abs(dot(pa, vec2(-dir.y, dir.x)));
        u = clamp(t / L, 0.0, 1.0);
        if (t < 0.0) d = v_radii.y < 0.5 ? length(pa) - hw : (v_radii.y < 1.5 ? perp - hw : max(perp - hw, -t));
        else if (t > L) d = v_radii.z < 0.5 ? length(p - b) - hw : (v_radii.z < 1.5 ? perp - hw : max(perp - hw, t - L));
        else d = perp - hw;
      }
      cov = clamp(0.5 - d * us, 0.0, 1.0);
    }
    vec4 sc = v_bsty.z > 0.0 ? texture(u_ramps, vec2(mix(v_border.x, v_border.y, u), v_bsty.z)) * v_color.a : premul(v_color);  // along a gradient: v_border.xy the piece's ends on it
    frag = sc * cov;
  } else if (kind == 10.0) {
// a triangle (corners v_uvr.xy, v_uvr.zw, v_s0.xy); each edge (v_radii.xyz) soft (0), hard on a line shared with a neighbour (1: a pixel on the line is this one's, -1: the neighbour's), or an arrowhead's base (2 / -2: hard where the stroke meets it, within v_radii.w of its middle)
// the pixel's center in the window, then in the layout: the same numbers for every instance, so two triangles sharing an edge see one line
    vec3 ph = inverse(mat3(1.0 + v_xf0.x, v_xf0.y, v_xf1.z, v_xf0.z, 1.0 + v_xf0.w, v_xf1.w, v_xf1.x, v_xf1.y, 1.0 + v_xf2.x)) * vec3(sp, 1.0);
    vec2 p = ph.xy / ph.z;
    vec2 tv[3] = vec2[3](v_uvr.xy, v_uvr.zw, v_s0.xy);
    float cov = 1.0;
    for (int i = 0; i < 3; i++) {
      vec2 a = tv[i], b = tv[(i + 1) % 3], c = tv[(i + 2) % 3];
      if (a.x > b.x || (a.x == b.x && a.y > b.y)) { vec2 t = a; a = b; b = t; }  // the edge's ends in one order, whichever triangle asks
      vec2 e = b - a;
      float le = length(e);
      if (le < 1e-6) continue;
      vec2 nn = vec2(e.y, -e.x) / le;
      float d = dot(p - a, nn), mode = v_radii[i];
      if (dot(c - a, nn) > 0.0) d = -d;  // positive outside this triangle
      if (abs(mode) > 1.5) {  // an arrowhead's base: shared with a stroke piece, which divides by its own plane, with a hair of slack either way
        if (abs(dot(p - (a + b) * 0.5, e / le)) < v_radii.w + 0.5 / us) { if (d >= (mode > 0.0 ? 0.001 : -0.001)) cov = 0.0; continue; }
        mode = 0.0;
      }
      if (mode > 0.0) { if (d > 0.0) cov = 0.0; }  // a line on the line is the owner's: exactly one of the two takes it
      else if (mode < 0.0) { if (d >= 0.0) cov = 0.0; }
      else if (d > 0.0 || abs(dot(p - (a + b) * 0.5, e / le)) <= le * 0.5) cov = min(cov, clamp(0.5 - d * us, 0.0, 1.0));  // (inside, the edge's line past its ends is not the outline: another piece's edge is)
    }
    frag = premul(v_color) * cov;
  } else if (kind == 12.0) {  // the end of a layer: its texture, which is the window's size, at this very pixel, cut by its mask, at the layer's opacity, blended
    ivec2 q = ivec2(gl_FragCoord.xy);
    vec4 sc = texelFetch(u_layer, q, 0);
    if (v_uvr.y > 0.5) {  // masked: by the mask layer's alpha, or (2) its luminance times its alpha
      vec4 m = texelFetch(u_aux, q, 0);
      sc *= v_uvr.y < 1.5 ? m.a : (m.a > 0.0 ? dot(m.rgb / m.a, vec3(0.2126, 0.7152, 0.0722)) * m.a : 0.0);
    }
    sc *= v_color.a;
    if (v_uvr.x > 0.5) {  // a blend mode: the result in full (blending is off for this quad), from what is under it
      vec4 db = texelFetch(u_backdrop, q, 0);
      vec3 cs = sc.a > 0.0 ? sc.rgb / sc.a : vec3(0.0), cb = db.a > 0.0 ? db.rgb / db.a : vec3(0.0);
      vec3 bl = clamp(blend3(int(v_uvr.x + 0.5), cb, cs), 0.0, 1.0);
      sc = vec4(sc.rgb * (1.0 - db.a) + db.rgb * (1.0 - sc.a) + sc.a * db.a * bl, sc.a + db.a * (1.0 - sc.a));
    }
    frag = sc;
  } else if (kind == 18.0) {  // a backdrop filter's result, in the element's rounded box
    float cov = clamp(0.5 - sd_rounded(v_local, half_size, v_radii) * us, 0.0, 1.0);
    frag = texelFetch(u_layer, ivec2(gl_FragCoord.xy), 0) * (cov * v_color.a);
  } else if (kind == 13.0) {  // a color filter: a matrix over straight RGBA (rows in color, border, radii, uvr; offsets in s0)
    vec4 t = texelFetch(u_layer, ivec2(gl_FragCoord.xy), 0);
    vec4 c = t.a > 0.0 ? vec4(t.rgb / t.a, t.a) : vec4(0.0);
    vec4 o = clamp(vec4(dot(v_color, c), dot(v_border, c), dot(v_radii, c), dot(v_uvr, c)) + v_s0, 0.0, 1.0);
    frag = vec4(o.rgb * o.a, o.a);
  } else if (kind == 14.0) {  // a blur along one axis (s0.x: 0 across, 1 down)
    frag = gauss(u_layer, ivec2(gl_FragCoord.xy), v_s0.x > 0.5 ? ivec2(0, 1) : ivec2(1, 0), v_params.z * u_scale);
  } else if (kind == 15.0) {  // a drop shadow: the layer over its shape (the aside copy, blurred across already) blurred down, moved by s0.xy and colored
    ivec2 q = ivec2(gl_FragCoord.xy);
    ivec2 off = ivec2(int(floor(v_s0.x * u_scale + 0.5)), (WINDOW_ROWS_DOWN == 1 ? 1 : -1) * int(floor(v_s0.y * u_scale + 0.5)));
    float a = gauss(u_aux, q - off, ivec2(0, 1), v_params.z * u_scale).a;
    vec4 sh = premul(v_color) * a;
    vec4 t = texelFetch(u_layer, q, 0);
    frag = t + sh * (1.0 - t.a);
  } else if (kind == 1.0) {
    float sigma = max(v_params.z * 0.5, 0.001);
    float d = sd_rounded(v_local, half_size, v_radii);
    float a = 0.5 - 0.5 * erf_approx(d / (sigma * 1.41421356));
    frag = premul(v_color) * a;
  } else {
    float d = sd_rounded(v_local, half_size, v_radii);
    float outer = clamp(0.5 - d * us, 0.0, 1.0);
    vec4 col = premul(v_color);
    if (kind == 5.0 || kind == 6.0 || kind == 8.0) {  // a gradient: v_color holds its geometry and the opacity, v_uvr.x the ramp's row
      float t;
      if (kind == 5.0) t = dot(v_local, v_color.xy) / v_color.z + 0.5;
      else if (kind == 6.0) t = length(v_local - v_color.xy) / v_color.z;
      else { vec2 d = v_local - v_color.xy; t = fract((atan(d.x, -d.y) - v_color.z) / 6.28318530718); }  // conic: clockwise from up
      if (v_params.z > 0.0) t = fract((t - v_params.w) * v_params.z);  // repeating: params.z = 1 / period, params.w = where it starts
      col = texture(u_ramps, vec2(clamp(t, 0.0, 1.0), v_uvr.x)) * v_color.w;
      float dither = fract(52.9829189 * fract(dot(vec2(fc.x, u_viewport.y * u_scale - fc.y), vec2(0.06711056, 0.00583715)))) - 0.5;  // hides 8-bit banding
      col.rgb = clamp(col.rgb + dither / 255.0 * col.a, 0.0, col.a);
    }
    float bl = v_params.x, bt = v_uvr.y, br = v_uvr.z, bb = v_uvr.w;  // the border's width on each side
    if (bl + bt + br + bb > 0.0) {
      vec2 ihalf = max(half_size - vec2(bl + br, bt + bb) * 0.5, vec2(0.0));  // the padding box: the box less its borders
      vec2 ictr = vec2(bl - br, bt - bb) * 0.5;
      vec4 ir = max(v_radii - vec4(bl + bt, br + bt, br + bb, bl + bb) * 0.5, 0.0);
      float di = sd_rounded(v_local - ictr, ihalf, ir);
      float inner = clamp(0.5 - di * us, 0.0, 1.0);
      vec4 ring = premul(v_border);
      if (v_bsty.x > 0.5 || v_bsty.y > 0.5 || v_bsty.z > 0.0) {  // sides of their own colors, a gradient, dashes or dots
        vec2 p = v_local;
        float d[4] = float[4](bl > 0.0 ? (p.x + half_size.x) / bl : 1e9, bt > 0.0 ? (p.y + half_size.y) / bt : 1e9,
                              br > 0.0 ? (half_size.x - p.x) / br : 1e9, bb > 0.0 ? (half_size.y - p.y) / bb : 1e9);  // how far in from each side (left, top, right, bottom), in units of its width
        int sa = 0, sb = 1;  // the side this pixel is nearest to (the mitre divides the sides), and the next nearest
        if (d[sb] < d[sa]) { sa = 1; sb = 0; }
        for (int i = 2; i < 4; i++) { if (d[i] < d[sa]) { sb = sa; sa = i; } else if (d[i] < d[sb]) sb = i; }
        vec4 wv = vec4(bl, bt, br, bb);
        vec2 ga = side_grad(sa, wv), gb = side_grad(sb, wv);
        float e = (d[sb] - d[sa]) / max(length(gb - ga), 1e-6);  // how far from the mitre, in units of length (the difference of the two grows at |grad|)
        float k = clamp(0.5 + e * us, 0.0, 1.0);  // the join antialiased over a pixel: on the mitre itself half and half, whichever way rounding put the pixel
        ring = k < 1.0 ? mix(side_ring(sb, p, half_size, wv, col, us), side_ring(sa, p, half_size, wv, col, us), k) : side_ring(sa, p, half_size, wv, col, us);
      }
      col = mix(ring, col, inner);
    }
    frag = col * outer;
  }
  frag_cov = kind == 4.0 ? lcd_cov : vec4(frag.a);
}
