// The vertex shader of the one instanced draw (render_gl.c, render_vk.c): every primitive is a 4-vertex strip, expanded here.
// tools/gen_shaders.py makes src/shaders.inc from this: the GL source with "#version 330 core", and SPIR-V for Vulkan ("#version 450": glslc defines VULKAN).
layout(location=0) in vec4 a_bounds;
layout(location=1) in vec4 a_clip;
layout(location=2) in vec4 a_color;
layout(location=3) in vec4 a_border;
layout(location=4) in vec4 a_radii;
layout(location=5) in vec4 a_params;  // border_w, kind, blur, -
layout(location=6) in vec4 a_uv;
layout(location=7) in vec4 a_s0;
layout(location=8) in vec4 a_s1;
layout(location=9) in vec4 a_s2;
layout(location=10) in vec4 a_bsty;
layout(location=11) in vec4 a_xf0;  // the transform: (a - 1, b, c, d - 1)
layout(location=12) in vec4 a_xf1;  // (tx, ty, g, h)
layout(location=13) in vec4 a_xf2;  // (w1, cuts, -, -): w = g x + h y + 1 + w1 (all 0: no perspective); the lines it is cut along (quad.frag)
#ifdef VULKAN
layout(push_constant) uniform Push { vec2 u_viewport; float u_scale; float u_gamma; };
#define gl_VertexID gl_VertexIndex
#else
uniform vec2 u_viewport;
uniform float u_scale;
#endif
flat out vec4 v_bounds, v_clip, v_color, v_border, v_radii, v_params, v_uvr, v_s0, v_s1, v_s2, v_bsty;
flat out float v_mag;
flat out vec4 v_xf0, v_xf1, v_xf2;
out vec2 v_local;
out vec2 v_pos;
out vec2 v_uv;
void main() {
  vec2 corner = vec2(float(gl_VertexID & 1), float(gl_VertexID >> 1));
  float kind = a_params.y;
  float pad = kind == 1.0 ? a_params.z * 1.5 + 1.0 : ((kind == 0.0 || kind == 5.0 || kind == 6.0 || kind == 7.0 || kind == 8.0 || kind == 18.0) ? 1.0 : 0.0);
// a glyph under a transform that is more than a move: a pixel of the window spans parts of several texels, so it is sampled between them (fs: glyph_texel), and its quad grows by a pixel so that the ink fades out at the bitmap's edge instead of being cut by the quad's (where a pixel's centre on that edge decides)
  bool turned_glyph = (kind == 2.0 || kind == 3.0 || kind == 4.0) && (a_xf0 != vec4(0.0) || a_xf1.z != 0.0 || a_xf1.w != 0.0 || a_xf2.x != 0.0);
  if (turned_glyph) pad = max(1.0, 1.0 / max(sqrt(abs((1.0 + a_xf0.x) * (1.0 + a_xf0.w) - a_xf0.y * a_xf0.z)), 0.01)) / u_scale;  // (a pixel where it is seen)
  vec2 size = a_bounds.zw + 2.0 * pad;
  vec2 pos0 = a_bounds.xy - pad + corner * size;  // where it is in the layout, before the transform
  vec2 pos = pos0 + vec2(a_xf0.x * pos0.x + a_xf0.z * pos0.y + a_xf1.x, a_xf0.y * pos0.x + a_xf0.w * pos0.y + a_xf1.y);
  float w = a_xf1.z * pos0.x + a_xf1.w * pos0.y + 1.0 + a_xf2.x;  // in perspective: the position is homogeneous, and what is interpolated across is too
  v_pos = pos / w;
  v_mag = sqrt(abs((1.0 + a_xf0.x) * (1.0 + a_xf0.w) - a_xf0.y * a_xf0.z));  // how much it magnifies (affine)
  v_xf0 = a_xf0; v_xf1 = a_xf1; v_xf2 = a_xf2;
  v_local = pos0 - (a_bounds.xy + a_bounds.zw * 0.5);
  v_uv = turned_glyph ? a_uv.xy + (pos0 - a_bounds.xy) / max(a_bounds.zw, vec2(1e-6)) * a_uv.zw : a_uv.xy + corner * a_uv.zw;
  v_bounds = a_bounds; v_clip = a_clip; v_color = a_color; v_border = a_border;
  v_radii = a_radii; v_params = a_params; v_uvr = a_uv;
  v_s0 = a_s0; v_s1 = a_s1; v_s2 = a_s2; v_bsty = a_bsty;
#ifdef VULKAN
  gl_Position = vec4(pos.x / u_viewport.x * 2.0 - w, pos.y / u_viewport.y * 2.0 - w, 0.0, w);  // (Vulkan's y goes down, as the layout's does)
#else
  gl_Position = vec4(pos.x / u_viewport.x * 2.0 - w, w - pos.y / u_viewport.y * 2.0, 0.0, w);
#endif
}
