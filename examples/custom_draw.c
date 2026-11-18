/*
 * Custom GPU drawing: 100,000 particles in a spinning galaxy, drawn by the app's own OpenGL shaders (instanced quads, depth tested) into
 * a gp_custom_el next to ordinary widgets. Drag to orbit, wheel to zoom. Each frame only gp_custom_invalidate is called: no view renders,
 * nothing is laid out. The particles move in the vertex shader, so the CPU does nothing per particle.
 *
 * This example draws with OpenGL only (the renderer gpuic uses by default); under GPUIC_RENDERER=vulkan the element stays empty.
 * gp_custom itself works with both: a Vulkan app records into the command buffer it is handed (see gpuic.h, and tests/custom_test.c).
 */
#include <gpuic/gpuic.h>

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#define BASE    gp_rgb(0x1e1e2e)
#define SURFACE gp_rgb(0x313244)
#define TEXT    gp_rgb(0xcdd6f4)
#define SUBTEXT gp_rgb(0x9399b2)
#define BLUE    gp_rgb(0x89b4fa)
#define BLUE_HI gp_rgb(0xa6c4fb)

#define COUNT   100000

/* ---------------------------------------------------------------- OpenGL, loaded through gp_gpu.gl_get_proc_address (no GL headers needed) */

typedef unsigned GLuint;
typedef int GLint;
#define GL_FUNCS(X)                                                                           \
    X(GLuint, glCreateShader, unsigned)                                                       \
    X(void, glShaderSource, GLuint, int, const char* const*, const int*)                      \
    X(void, glCompileShader, GLuint)                                                          \
    X(GLuint, glCreateProgram, void)                                                          \
    X(void, glAttachShader, GLuint, GLuint)                                                   \
    X(void, glLinkProgram, GLuint)                                                            \
    X(void, glDeleteShader, GLuint)                                                           \
    X(void, glDeleteProgram, GLuint)                                                          \
    X(void, glUseProgram, GLuint)                                                             \
    X(GLint, glGetUniformLocation, GLuint, const char*)                                       \
    X(void, glUniformMatrix4fv, GLint, int, unsigned char, const float*)                      \
    X(void, glUniform1f, GLint, float)                                                        \
    X(void, glGenVertexArrays, int, GLuint*)                                                  \
    X(void, glBindVertexArray, GLuint)                                                        \
    X(void, glDeleteVertexArrays, int, const GLuint*)                                         \
    X(void, glGenBuffers, int, GLuint*)                                                       \
    X(void, glBindBuffer, unsigned, GLuint)                                                   \
    X(void, glBufferData, unsigned, intptr_t, const void*, unsigned)                          \
    X(void, glDeleteBuffers, int, const GLuint*)                                              \
    X(void, glEnableVertexAttribArray, GLuint)                                                \
    X(void, glVertexAttribPointer, GLuint, int, unsigned, unsigned char, int, const void*)    \
    X(void, glVertexAttribDivisor, GLuint, GLuint)                                            \
    X(void, glDrawArraysInstanced, unsigned, int, int, int)                                   \
    X(void, glGenTextures, int, GLuint*)                                                      \
    X(void, glBindTexture, unsigned, GLuint)                                                  \
    X(void, glTexImage2D, unsigned, int, int, int, int, int, unsigned, unsigned, const void*) \
    X(void, glDeleteTextures, int, const GLuint*)                                             \
    X(void, glGenFramebuffers, int, GLuint*)                                                  \
    X(void, glBindFramebuffer, unsigned, GLuint)                                              \
    X(void, glFramebufferTexture2D, unsigned, unsigned, unsigned, GLuint, int)                \
    X(void, glDeleteFramebuffers, int, const GLuint*)                                         \
    X(void, glGenRenderbuffers, int, GLuint*)                                                 \
    X(void, glBindRenderbuffer, unsigned, GLuint)                                             \
    X(void, glRenderbufferStorage, unsigned, unsigned, int, int)                              \
    X(void, glFramebufferRenderbuffer, unsigned, unsigned, unsigned, GLuint)                  \
    X(void, glDeleteRenderbuffers, int, const GLuint*)                                        \
    X(void, glViewport, int, int, int, int)                                                   \
    X(void, glClearColor, float, float, float, float)                                         \
    X(void, glClear, unsigned)                                                                \
    X(void, glEnable, unsigned)                                                               \
    X(void, glDisable, unsigned)

static struct
{
#define X(ret, name, ...) ret (*name)(__VA_ARGS__);
    GL_FUNCS(X)
#undef X
} gl;

enum { TEXTURE_2D = 0x0DE1,
    RGBA8 = 0x8058,
    RGBA = 0x1908,
    UNSIGNED_BYTE = 0x1401,
    FLOAT = 0x1406,
    FRAMEBUFFER = 0x8D40,
    RENDERBUFFER = 0x8D41,
    COLOR_ATTACHMENT0 = 0x8CE0,
    DEPTH_ATTACHMENT = 0x8D00,
    DEPTH_COMPONENT24 = 0x81A6,
    COLOR_BUFFER_BIT = 0x4000,
    DEPTH_BUFFER_BIT = 0x0100,
    DEPTH_TEST = 0x0B71,
    BLEND = 0x0BE2,
    ARRAY_BUFFER = 0x8892,
    STATIC_DRAW = 0x88E4,
    TRIANGLE_STRIP = 0x0005,
    VERTEX_SHADER = 0x8B31,
    FRAGMENT_SHADER = 0x8B30,
};

/* Each particle: (radius, angle, height, speed). It turns around the axis at its own speed; its color comes from its radius. */
static const char* vs_src = "#version 330 core\n"
                            "layout(location = 0) in vec4 a_p;\n"
                            "uniform mat4 u_mvp; uniform float u_time, u_aspect;\n"
                            "out vec2 v_q; out vec3 v_color;\n"
                            "void main() {\n"
                            "  float a = a_p.y + u_time * a_p.w;\n"
                            "  vec4 c = u_mvp * vec4(a_p.x * cos(a), a_p.z, a_p.x * sin(a), 1.0);\n"
                            "  v_q = vec2(gl_VertexID & 1, gl_VertexID >> 1) * 2.0 - 1.0;\n"
                            "  c.xy += v_q * vec2(1.0 / u_aspect, 1.0) * 0.012;\n" /* a quad of a fixed size in the world */
                            "  gl_Position = c;\n"
                            "  v_color = mix(vec3(1.0, 0.85, 0.6), vec3(0.45, 0.6, 1.0), clamp(a_p.x, 0.0, 1.0));\n"
                            "}\n";
static const char* fs_src = "#version 330 core\n"
                            "in vec2 v_q; in vec3 v_color; out vec4 o;\n"
                            "void main() {\n"
                            "  float r = dot(v_q, v_q);\n"
                            "  if (r > 1.0) discard;\n"
                            "  o = vec4(v_color * (1.0 - 0.5 * r), 1.0);\n" /* opaque, so it is already premultiplied */
                            "}\n";

/* ---------------------------------------------------------------- the view */

typedef struct
{
    gp_custom custom;
    bool paused;
    double time, last_frame;
    float yaw, pitch, dist;
    gp_point last;
    /* GL: made in init, freed in destroy */
    bool gl_ok;
    GLuint prog, vao, vbo, tex, depth, fbo;
    GLint u_mvp, u_time, u_aspect;
    int tw, th;
} Galaxy;

static GLuint shader(unsigned type, const char* src)
{
    GLuint s = gl.glCreateShader(type);
    gl.glShaderSource(s, 1, &src, NULL);
    gl.glCompileShader(s);
    return s;
}

static void galaxy_init(void* data, const gp_gpu* gpu)
{
    Galaxy* g = data;
    if(gpu->backend != GP_GPU_GL) {
        fprintf(stderr, "custom_draw: this example draws with OpenGL only\n");
        return;
    }
#define X(ret, name, ...) gl.name = (ret (*)(__VA_ARGS__))gpu->gl_get_proc_address(#name);
    GL_FUNCS(X)
#undef X
    GLuint vs = shader(VERTEX_SHADER, vs_src), fs = shader(FRAGMENT_SHADER, fs_src);
    g->prog = gl.glCreateProgram();
    gl.glAttachShader(g->prog, vs);
    gl.glAttachShader(g->prog, fs);
    gl.glLinkProgram(g->prog);
    gl.glDeleteShader(vs);
    gl.glDeleteShader(fs);
    g->u_mvp = gl.glGetUniformLocation(g->prog, "u_mvp");
    g->u_time = gl.glGetUniformLocation(g->prog, "u_time");
    g->u_aspect = gl.glGetUniformLocation(g->prog, "u_aspect");

    float* p = malloc(COUNT * 4 * sizeof *p); /* two spiral arms with some scatter, made once */
    srand(7);
    for(int i = 0; i < COUNT; i++) {
        float u = (float)rand() / (float)RAND_MAX, v = (float)rand() / (float)RAND_MAX - 0.5f, w = (float)rand() / (float)RAND_MAX - 0.5f;
        float r = 0.05f + u * u * 0.95f;
        p[i * 4 + 0] = r;
        p[i * 4 + 1] = (float)(i & 1) * 3.14159f + r * 6.f + v * 1.2f;
        p[i * 4 + 2] = w * 0.12f * (1.2f - r);
        p[i * 4 + 3] = 0.35f / (0.2f + r);
    }
    gl.glGenVertexArrays(1, &g->vao);
    gl.glBindVertexArray(g->vao);
    gl.glGenBuffers(1, &g->vbo);
    gl.glBindBuffer(ARRAY_BUFFER, g->vbo);
    gl.glBufferData(ARRAY_BUFFER, (intptr_t)(COUNT * 4 * sizeof *p), p, STATIC_DRAW);
    gl.glEnableVertexAttribArray(0);
    gl.glVertexAttribPointer(0, 4, FLOAT, 0, 0, NULL);
    gl.glVertexAttribDivisor(0, 1);
    free(p);
    gl.glGenTextures(1, &g->tex);
    gl.glGenRenderbuffers(1, &g->depth);
    gl.glGenFramebuffers(1, &g->fbo);
    g->tw = g->th = 0;
    g->gl_ok = true;
}

static void galaxy_destroy(void* data, const gp_gpu* gpu)
{
    Galaxy* g = data;
    if(!g->gl_ok) {
        return;
    }
    gl.glDeleteFramebuffers(1, &g->fbo);
    gl.glDeleteRenderbuffers(1, &g->depth);
    gl.glDeleteTextures(1, &g->tex);
    gl.glDeleteBuffers(1, &g->vbo);
    gl.glDeleteVertexArrays(1, &g->vao);
    gl.glDeleteProgram(g->prog);
    g->gl_ok = false;
}

/* out = a b (column-major 4 x 4) */
static void mul(float out[16], const float a[16], const float b[16])
{
    float r[16];
    for(int c = 0; c < 4; c++) {
        for(int i = 0; i < 4; i++) {
            r[c * 4 + i] = a[i] * b[c * 4] + a[4 + i] * b[c * 4 + 1] + a[8 + i] * b[c * 4 + 2] + a[12 + i] * b[c * 4 + 3];
        }
    }
    for(int i = 0; i < 16; i++) {
        out[i] = r[i];
    }
}

static uint64_t galaxy_draw(void* data, const gp_custom_frame* f)
{
    Galaxy* g = data;
    if(!g->gl_ok) {
        return 0;
    }
    if(g->tw != f->px_width || g->th != f->px_height) { /* the element changed size: the targets follow */
        g->tw = f->px_width;
        g->th = f->px_height;
        gl.glBindTexture(TEXTURE_2D, g->tex);
        gl.glTexImage2D(TEXTURE_2D, 0, RGBA8, g->tw, g->th, 0, RGBA, UNSIGNED_BYTE, NULL);
        gl.glBindRenderbuffer(RENDERBUFFER, g->depth);
        gl.glRenderbufferStorage(RENDERBUFFER, DEPTH_COMPONENT24, g->tw, g->th);
        gl.glBindFramebuffer(FRAMEBUFFER, g->fbo);
        gl.glFramebufferTexture2D(FRAMEBUFFER, COLOR_ATTACHMENT0, TEXTURE_2D, g->tex, 0);
        gl.glFramebufferRenderbuffer(FRAMEBUFFER, DEPTH_ATTACHMENT, RENDERBUFFER, g->depth);
    }
    float aspect = f->width / f->height;
    float n = 0.05f, fa = 20.f, k = 1.f / tanf(0.5f); /* a 57 degree field of view */
    float proj[16] = { k / aspect, 0, 0, 0, 0, k, 0, 0, 0, 0, (fa + n) / (n - fa), -1, 0, 0, 2 * fa * n / (n - fa), 0 };
    float cy = cosf(g->yaw), sy = sinf(g->yaw), cp = cosf(g->pitch), sp = sinf(g->pitch);
    float yaw[16] = { cy, 0, -sy, 0, 0, 1, 0, 0, sy, 0, cy, 0, 0, 0, 0, 1 };
    float pitch[16] = { 1, 0, 0, 0, 0, cp, sp, 0, 0, -sp, cp, 0, 0, 0, -g->dist, 1 }; /* (and moved away from the eye) */
    float mvp[16];
    mul(mvp, pitch, yaw);
    mul(mvp, proj, mvp);

    gl.glBindFramebuffer(FRAMEBUFFER, g->fbo);
    gl.glViewport(0, 0, g->tw, g->th);
    gl.glClearColor(0.07f, 0.07f, 0.11f, 1);
    gl.glClear(COLOR_BUFFER_BIT | DEPTH_BUFFER_BIT);
    gl.glEnable(DEPTH_TEST);
    gl.glDisable(BLEND);
    gl.glUseProgram(g->prog);
    gl.glUniformMatrix4fv(g->u_mvp, 1, 0, mvp);
    gl.glUniform1f(g->u_time, (float)g->time);
    gl.glUniform1f(g->u_aspect, aspect);
    gl.glBindVertexArray(g->vao);
    gl.glDrawArraysInstanced(TRIANGLE_STRIP, 0, 4, COUNT);
    return g->tex; /* (gpuic sets back the framebuffer, depth test, program... itself) */
}

static void tick(void* self, gp_ctx* cx, double time, void* data)
{
    Galaxy* g = self;
    if(g->paused) {
        return;
    }
    if(g->last_frame > 0) {
        g->time += time - g->last_frame;
    }
    g->last_frame = time;
    gp_custom_invalidate(cx->app, g->custom); /* only the particles are drawn again */
    gp_next_frame(cx, tick, NULL);
}

static void toggle(void* self, gp_ctx* cx, const gp_mouse_event* ev, void* data)
{
    Galaxy* g = self;
    g->paused = !g->paused;
    g->last_frame = 0;
    if(!g->paused) {
        gp_next_frame(cx, tick, NULL);
    }
    gp_notify(cx); /* the button's label */
}

static void reset_view(void* self, gp_ctx* cx, const gp_mouse_event* ev, void* data)
{
    Galaxy* g = self;
    g->yaw = 0;
    g->pitch = 0.5f;
    g->dist = 2.4f;
    gp_custom_invalidate(cx->app, g->custom);
}

static void press(void* self, gp_ctx* cx, const gp_mouse_event* ev, void* data)
{
    ((Galaxy*)self)->last = ev->position;
}

static void orbit(void* self, gp_ctx* cx, const gp_mouse_event* ev, void* data)
{
    Galaxy* g = self;
    g->yaw += (ev->position.x - g->last.x) * 0.01f;
    g->pitch = fminf(fmaxf(g->pitch + (ev->position.y - g->last.y) * 0.01f, -1.5f), 1.5f);
    g->last = ev->position;
    gp_custom_invalidate(cx->app, g->custom);
}

static void zoom(void* self, gp_ctx* cx, const gp_wheel_event* ev, void* data)
{
    Galaxy* g = self;
    g->dist = fminf(fmaxf(g->dist * expf(ev->dy * 0.002f), 0.6f), 8.f);
    gp_custom_invalidate(cx->app, g->custom);
}

static gp_node* button(const char* label, gp_node* on_click)
{
    return gp_div(gp_px(16), gp_py(8), gp_rounded(8), gp_bg(BLUE), gp_hover_bg(BLUE_HI), gp_text_color(BASE), gp_cursor_pointer(), on_click, gp_text(label, gp_font_bold()));
}

static gp_node* galaxy_render(void* self, gp_ctx* cx)
{
    Galaxy* g = self;
    return gp_div(gp_size_full(), gp_flex_row(), gp_gap(16), gp_p(16), gp_bg(BASE), gp_text_color(TEXT), gp_div(gp_flex_col(), gp_gap(12), gp_w(220), gp_p(16), gp_rounded(12), gp_bg(SURFACE), gp_text("Galaxy", gp_text_size(20), gp_font_bold()), gp_text(gp_fmt("%d particles, drawn by the app's own shaders", COUNT), gp_text_size(13), gp_text_color(SUBTEXT)), gp_text("Drag to orbit, wheel to zoom.", gp_text_size(13), gp_text_color(SUBTEXT)), button(g->paused ? "Resume" : "Pause", gp_on_click(toggle)), button("Reset view", gp_on_click(reset_view))), gp_custom_el(g->custom, gp_flex_1(), gp_rounded(12), gp_on_mouse_down(press), gp_on_drag(orbit), gp_on_wheel(zoom), gp_div(gp_absolute(), gp_left(12), gp_bottom(12), gp_px(8), gp_py(4), gp_rounded(6), gp_bg(gp_rgba(0x00000080)), /* a child, drawn over it */
                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                gp_text("gp_custom_el", gp_text_size(12), gp_text_color(SUBTEXT)))));
}

static void ready(gp_app* app, void* userdata)
{
    gp_entity root = GP_NEW_VIEW(app, Galaxy, galaxy_render, .pitch = 0.5f, .dist = 2.4f);
    Galaxy* g = gp_entity_get(app, root);
    g->custom = gp_custom_new(app, &(gp_custom_fns){ galaxy_init, galaxy_destroy, galaxy_draw }, g); /* (the entity's state does not move) */
    gp_open_window(app, &(gp_window_options){ .title = "gpuic — custom drawing", .width = 900, .height = 560, .background = BASE }, root);
    gp_ctx cx = gp_ctx_for(app, root);
    gp_next_frame(&cx, tick, NULL);
}

int main(void)
{
    return gp_application_run(ready, NULL);
}
