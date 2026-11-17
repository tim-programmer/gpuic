/*
 * OpenGL 3.3 renderer. The whole scene is one instanced draw: every primitive
 * (rounded quad with border, blurred shadow, glyph) is a 4-vertex strip
 * expanded in the vertex shader and shaded with signed-distance functions
 * (src/shaders/). Draw order == paint order, so there is no sorting and one draw call.
 */
#include "internal.h"

/* SDL ships its own copy of the GL headers, so neither libGL nor Mesa headers
 * are needed to build; every entry point is loaded at runtime. */
#define GL_GLEXT_PROTOTYPES 1 /* declarations only: they give the table its types */
#include <SDL3/SDL_opengl.h>

#define GL_FUNCS(X)              \
    X(glActiveTexture)           \
    X(glBindTexture)             \
    X(glBlendFunc)               \
    X(glClear)                   \
    X(glClearColor)              \
    X(glDeleteTextures)          \
    X(glEnable)                  \
    X(glGenTextures)             \
    X(glPixelStorei)             \
    X(glReadPixels)              \
    X(glTexImage2D)              \
    X(glTexParameteri)           \
    X(glTexSubImage2D)           \
    X(glViewport)                \
    X(glGenVertexArrays)         \
    X(glBindVertexArray)         \
    X(glDeleteVertexArrays)      \
    X(glGenBuffers)              \
    X(glDeleteBuffers)           \
    X(glBindBuffer)              \
    X(glBufferData)              \
    X(glEnableVertexAttribArray) \
    X(glVertexAttribPointer)     \
    X(glVertexAttribDivisor)     \
    X(glCreateShader)            \
    X(glShaderSource)            \
    X(glCompileShader)           \
    X(glGetShaderiv)             \
    X(glGetShaderInfoLog)        \
    X(glDeleteShader)            \
    X(glCreateProgram)           \
    X(glAttachShader)            \
    X(glLinkProgram)             \
    X(glGetProgramiv)            \
    X(glGetProgramInfoLog)       \
    X(glDeleteProgram)           \
    X(glUseProgram)              \
    X(glGetUniformLocation)      \
    X(glUniform1f)               \
    X(glUniform1i)               \
    X(glUniform2f)               \
    X(glGenerateMipmap)          \
    X(glGenFramebuffers)         \
    X(glDeleteFramebuffers)      \
    X(glBindFramebuffer)         \
    X(glFramebufferTexture2D)    \
    X(glCheckFramebufferStatus)  \
    X(glScissor)                 \
    X(glDisable)                 \
    X(glCopyTexSubImage2D)       \
    X(glFinish)                  \
    X(glGetString)               \
    X(glDrawArraysInstanced)

/* The headers declare these as prototypes; calls go through `gl.name` instead. */
static struct
{
#define X(name) __typeof__(&name) name;
    GL_FUNCS(X)
#undef X
} gl;

#include "shaders_gl.inc" /* vs_src, fs_src: tools/gen_shaders.py, from src/shaders/ */

/* the window-sized targets: one layer per depth, then two for filter passes to go back and forth between, one aside (a drop shadow's blurred shape),
 * and one for what is under a layer with a blend mode */
enum { T_FX0 = GP_MAX_LAYERS,
    T_FX1,
    T_AUX,
    T_BACKDROP,
    T_COUNT,
};

typedef struct
{
    gp_renderer base;
    GLuint prog, vao, vbo, atlas, color_atlas, lcd_atlas, ramps;
    GLint u_viewport, u_scale, u_gamma, u_atlas, u_color_atlas, u_lcd_atlas, u_ramps, u_image, u_layer, u_aux, u_backdrop;
    size_t vbo_cap;
    gp_instance* sent;       /* what the buffer holds now: a frame that paints the same instances again sends nothing */
    size_t sent_len, sent_cap;
    bool finish_before_copy; /* Mesa's llvmpipe: see backdrop_copy */
    /* offscreen targets (made when first needed, again when the window's size changes) */
    struct
    {
        GLuint fbo, tex;
    } layers[T_COUNT];
    int layer_w, layer_h;
} gl_renderer;

static GLuint compile(GLenum type, const char* src)
{
    GLuint s = gl.glCreateShader(type);
    gl.glShaderSource(s, 1, &src, NULL);
    gl.glCompileShader(s);
    GLint ok = 0;
    gl.glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if(!ok) {
        char log[2048];
        gl.glGetShaderInfoLog(s, sizeof log, NULL, log);
        fprintf(stderr, "gpuic: shader compile failed:\n%s\n", log);
        return 0;
    }
    return s;
}

static const gp_renderer_ops gl_ops;

gp_renderer* gp_gl_renderer_new(void* (*get_proc)(const char*))
{
#define X(name)                                                    \
    gl.name = (__typeof__(gl.name))get_proc(#name);                \
    if(!gl.name) {                                                 \
        fprintf(stderr, "gpuic: missing GL function %s\n", #name); \
        return NULL;                                               \
    }
    GL_FUNCS(X)
#undef X

    gl_renderer* r = calloc(1, sizeof *r);
    r->base.ops = &gl_ops;
    const char* renderer = (const char*)gl.glGetString(GL_RENDERER);
    r->finish_before_copy = renderer && strstr(renderer, "llvmpipe"); /* (see backdrop_copy) */
    GLuint vs = compile(GL_VERTEX_SHADER, vs_src), fs = compile(GL_FRAGMENT_SHADER, fs_src);
    if(!vs || !fs) {
        free(r);
        return NULL;
    }
    r->prog = gl.glCreateProgram();
    gl.glAttachShader(r->prog, vs);
    gl.glAttachShader(r->prog, fs);
    gl.glLinkProgram(r->prog);
    GLint ok = 0;
    gl.glGetProgramiv(r->prog, GL_LINK_STATUS, &ok);
    if(!ok) {
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
    for(GLuint i = 0; i < 14; i++) {
        /* gp_instance is fourteen consecutive vec4s */
        gl.glEnableVertexAttribArray(i);
        gl.glVertexAttribPointer(i, 4, GL_FLOAT, GL_FALSE, sizeof(gp_instance), (const void*)(uintptr_t)(i * 4 * sizeof(float)));
        gl.glVertexAttribDivisor(i, 1);
    }

    gl.glGenTextures(1, &r->atlas);
    gl.glBindTexture(GL_TEXTURE_2D, r->atlas);
    gl.glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    gl.glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, GP_ATLAS_SIZE, GP_ATLAS_SIZE, 0, GL_RED, GL_UNSIGNED_BYTE, NULL);
    gl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    gl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    gl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    gl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    gl.glGenTextures(1, &r->color_atlas);
    gl.glActiveTexture(GL_TEXTURE1);
    gl.glBindTexture(GL_TEXTURE_2D, r->color_atlas);
    gl.glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, GP_COLOR_ATLAS_SIZE, GP_COLOR_ATLAS_SIZE, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
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
    gl.glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, GP_LCD_ATLAS_SIZE, GP_LCD_ATLAS_SIZE, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    gl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    gl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    gl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    gl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    gl.glActiveTexture(GL_TEXTURE0);
    return &r->base;
}

static void gl_free(gp_renderer* base, gp_gfx* gfx)
{
    gl_renderer* r = (gl_renderer*)base;
    { /* the images' textures go with the context; they are made again if a new renderer draws them */
        uint32_t tex[64];
        size_t n;
        while(gfx && (n = gp_gfx_take_all_textures(gfx, tex, 64)) > 0) {
            gl.glDeleteTextures((GLsizei)n, tex);
        }
        uint32_t dead[64];
        while(gfx && (n = gp_gfx_take_dead_textures(gfx, dead, 64)) > 0) {
            gl.glDeleteTextures((GLsizei)n, dead);
        }
    }
    for(int i = 0; i < T_COUNT; i++) {
        if(r->layers[i].fbo) {
            gl.glDeleteFramebuffers(1, &r->layers[i].fbo);
        }
        if(r->layers[i].tex) {
            gl.glDeleteTextures(1, &r->layers[i].tex);
        }
    }
    gl.glDeleteProgram(r->prog);
    gl.glDeleteBuffers(1, &r->vbo);
    free(r->sent);
    gl.glDeleteVertexArrays(1, &r->vao);
    gl.glDeleteTextures(1, &r->atlas);
    gl.glDeleteTextures(1, &r->color_atlas);
    gl.glDeleteTextures(1, &r->lcd_atlas);
    gl.glDeleteTextures(1, &r->ramps);
    free(r);
}

/* instances [first, first + count) in one instanced draw */
/* What the target being drawn on (bound now) holds over the pixel region x0..x1, y0..y1 (rows from the top), into the texture bound now (the backdrop) at
 * the same place. Mesa's llvmpipe with its rasterizer threads (26.2 here) could copy the region before all the drawing queued into it had landed: a blend
 * then saw part of what was under it, a different part each run (a blit did the same). Waiting for the drawing first fixes it there; GPUs need no wait. */
static void backdrop_copy(gl_renderer* r, int px_h, int x0, int y0, int x1, int y1)
{
    if(x1 <= x0 || y1 <= y0) {
        return;
    }
    if(r->finish_before_copy) {
        gl.glFinish();
    }
    gl.glCopyTexSubImage2D(GL_TEXTURE_2D, 0, x0, px_h - y1, x0, px_h - y1, x1 - x0, y1 - y0);
}

static void draw_run(gl_renderer* r, const gp_scene* scene, size_t first, size_t count)
{
    (void)scene;
    for(GLuint a = 0; a < 14; a++) {
        gl.glVertexAttribPointer(a, 4, GL_FLOAT, GL_FALSE, sizeof(gp_instance), (const void*)(uintptr_t)(first * sizeof(gp_instance) + a * 4 * sizeof(float)));
    }
    gl.glDrawArraysInstanced(GL_TRIANGLE_STRIP, 0, 4, (GLsizei)count);
    (void)r;
}

/* Target d (a layer for depth d, or T_*), the size of the window: made when first needed; all of them made again when the window's size changed. */
static bool layer_target(gl_renderer* r, int d, int px_w, int px_h)
{
    if(r->layer_w != px_w || r->layer_h != px_h) {
        for(int i = 0; i < T_COUNT; i++) {
            if(r->layers[i].fbo) {
                gl.glDeleteFramebuffers(1, &r->layers[i].fbo);
            }
            if(r->layers[i].tex) {
                gl.glDeleteTextures(1, &r->layers[i].tex);
            }
            r->layers[i].fbo = r->layers[i].tex = 0;
        }
        r->layer_w = px_w;
        r->layer_h = px_h;
    }
    if(r->layers[d].fbo) {
        return true;
    }
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
    if(!ok) {
        static bool warned;
        if(!warned) {
            warned = true;
            fprintf(stderr, "gpuic: an offscreen layer could not be made: layers are drawn straight onto the window\n");
        }
        gl.glDeleteFramebuffers(1, &fbo);
        gl.glDeleteTextures(1, &tex);
        return false;
    }
    r->layers[d].fbo = fbo;
    r->layers[d].tex = tex;
    return true;
}

/* The texture of an image, made when it is first drawn: its pixels, with mipmaps for when it is shown smaller. */
static void upload_image(gl_renderer* r, gp_gfx_image* im)
{
    (void)r;
    if(im->tex) {
        return;
    }
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
    im->tex = tex;
}

static void gl_draw(gp_renderer* base, gp_window* w, gp_text_system* ts, const gp_scene* scene, gp_color clear)
{
    gl_renderer* r = (gl_renderer*)base;
    int px_w = w->px_w, px_h = w->px_h;
    float scale = w->scale;
    gp_platform_make_current(w);
    gl.glViewport(0, 0, px_w, px_h);
    gl.glClearColor(clear.r, clear.g, clear.b, clear.a);
    gl.glClear(GL_COLOR_BUFFER_BIT);

    { /* textures of images that were released */
        gp_gfx* g = gp_text_gfx(ts);
        uint32_t dead[32];
        size_t n;
        while(g && (n = gp_gfx_take_dead_textures(g, dead, 32)) > 0) {
            gl.glDeleteTextures((GLsizei)n, dead);
        }
    }

    /* upload any glyphs rasterized since the last frame */
    gl.glActiveTexture(GL_TEXTURE0);
    gl.glBindTexture(GL_TEXTURE_2D, r->atlas);
    int dx, dy, dw, dh;
    if(gp_text_atlas_take_dirty(ts, &dx, &dy, &dw, &dh)) {
        gl.glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        gl.glPixelStorei(GL_UNPACK_ROW_LENGTH, GP_ATLAS_SIZE);
        gl.glTexSubImage2D(GL_TEXTURE_2D, 0, dx, dy, dw, dh, GL_RED, GL_UNSIGNED_BYTE, gp_text_atlas(ts) + (size_t)dy * GP_ATLAS_SIZE + (size_t)dx);
        gl.glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    }
    gl.glActiveTexture(GL_TEXTURE1);
    gl.glBindTexture(GL_TEXTURE_2D, r->color_atlas);
    if(gp_text_color_atlas_take_dirty(ts, &dx, &dy, &dw, &dh)) {
        gl.glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        gl.glPixelStorei(GL_UNPACK_ROW_LENGTH, GP_COLOR_ATLAS_SIZE);
        gl.glTexSubImage2D(GL_TEXTURE_2D, 0, dx, dy, dw, dh, GL_RGBA, GL_UNSIGNED_BYTE, gp_text_color_atlas(ts) + ((size_t)dy * GP_COLOR_ATLAS_SIZE + (size_t)dx) * 4);
        gl.glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    }
    gl.glActiveTexture(GL_TEXTURE2);
    gl.glBindTexture(GL_TEXTURE_2D, r->lcd_atlas);
    if(gp_text_lcd_atlas_take_dirty(ts, &dx, &dy, &dw, &dh)) {
        gl.glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        gl.glPixelStorei(GL_UNPACK_ROW_LENGTH, GP_LCD_ATLAS_SIZE);
        gl.glTexSubImage2D(GL_TEXTURE_2D, 0, dx, dy, dw, dh, GL_RGBA, GL_UNSIGNED_BYTE, gp_text_lcd_atlas(ts) + ((size_t)dy * GP_LCD_ATLAS_SIZE + (size_t)dx) * 4);
        gl.glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    }
    gl.glActiveTexture(GL_TEXTURE3);
    gl.glBindTexture(GL_TEXTURE_2D, r->ramps);
    {
        gp_gfx* gfx = gp_text_gfx(ts);
        int row0, row1;
        if(gfx && gp_gfx_gradient_take_dirty(gfx, &row0, &row1)) {
            gl.glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
            gl.glTexSubImage2D(GL_TEXTURE_2D, 0, 0, row0, GP_GRAD_W, row1 - row0, GL_RGBA, GL_UNSIGNED_BYTE, gp_gfx_gradient_data(gfx) + (size_t)row0 * GP_GRAD_W * 4);
        }
    }
    gl.glActiveTexture(GL_TEXTURE0);
    if(scene->len == 0) {
        return;
    }

    gl.glEnable(GL_BLEND);
    gl.glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC1_COLOR); /* dual-source: LCD text keeps a different share of each channel */
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
    if(scene->len != r->sent_len || memcmp(scene->items, r->sent, bytes) != 0) {
        gl.glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)bytes, NULL, GL_STREAM_DRAW); /* orphan */
        gl.glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)bytes, scene->items, GL_STREAM_DRAW);
        if(scene->len > r->sent_cap) {
            r->sent_cap = scene->len;
            r->sent = realloc(r->sent, bytes);
            if(!r->sent) {
                abort();
            }
        }
        memcpy(r->sent, scene->items, bytes);
        r->sent_len = scene->len;
    }
    /* Everything but images and layers is one instanced draw. An image has a texture of its own, so it splits the scene: the run
     * before it, then the image alone, then on. A layer's BEGIN sends what follows into the layer's texture (only its region cleared), and its END
     * goes back to where it was and composites the region (the END is that quad). Order is kept, which is what painting needs. */
    gp_gfx* gfx = gp_text_gfx(ts);
    size_t run = 0;
    int depth = 0;         /* layers open */
    int skipped = 0;       /* ... of which these could not be made, inside the open ones: drawn straight on what is under them */
    int cur = -1;          /* where the layer about to end is now, after the filter passes so far (-1: its own target) */
    GLuint mask_tex = 0;   /* the mask of the layer about to end */
    bool backdrop = false; /* between a BACKDROP and its END: the passes are over the copy of what is under it */
    for(size_t i = 0; i < scene->len; i++) {
        const gp_instance* in = &scene->items[i];
        int kind = (int)in->kind;
        if(kind == GP_PRIM_BACKDROP) { /* what is under the region, out of the target being drawn on (bound now), into the backdrop texture */
            if(i > run) {
                draw_run(r, scene, run, i - run);
            }
            run = i + 1;
            if(!layer_target(r, T_BACKDROP, px_w, px_h)) {
                continue;
            }
            int x0 = (int)floorf(in->bounds[0] * scale + 0.5f), x1 = (int)floorf((in->bounds[0] + in->bounds[2]) * scale + 0.5f);
            int y0 = (int)floorf(in->bounds[1] * scale + 0.5f), y1 = (int)floorf((in->bounds[1] + in->bounds[3]) * scale + 0.5f);
            if(x0 < 0) {
                x0 = 0;
            }
            if(y0 < 0) {
                y0 = 0;
            }
            if(x1 > px_w) {
                x1 = px_w;
            }
            if(y1 > px_h) {
                y1 = px_h;
            }
            gl.glActiveTexture(GL_TEXTURE7);
            gl.glBindTexture(GL_TEXTURE_2D, r->layers[T_BACKDROP].tex);
            backdrop_copy(r, px_h, x0, y0, x1, y1);
            gl.glBindTexture(GL_TEXTURE_2D, 0);
            gl.glActiveTexture(GL_TEXTURE0);
            cur = T_BACKDROP;
            backdrop = true;
            continue;
        }
        if(kind == GP_PRIM_BACKDROP_END) { /* back on the target, the result drawn in the box */
            if(i > run) {
                draw_run(r, scene, run, i - run);
            }
            run = i + 1;
            if(!backdrop) {
                continue;
            }
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
        if(kind == GP_PRIM_FX_MATRIX || kind == GP_PRIM_FX_BLUR || kind == GP_PRIM_FX_SHADOW) {
            /* a pass over the open layer: from where it is now into the other of the two pass targets (a shadow's first pass: into the aside one) */
            if(i > run) {
                draw_run(r, scene, run, i - run);
            }
            run = i + 1;
            if(!backdrop && (skipped || !depth)) {
                continue;
            }
            int src = cur >= 0 ? cur : depth - 1;
            int dst = kind == GP_PRIM_FX_BLUR && in->side[0][1] > 0.5f ? T_AUX : (cur == T_FX0 ? T_FX1 : T_FX0);
            if(!layer_target(r, dst, px_w, px_h)) {
                continue;
            }
            gl.glBindFramebuffer(GL_FRAMEBUFFER, r->layers[dst].fbo);
            gl.glActiveTexture(GL_TEXTURE5);
            gl.glBindTexture(GL_TEXTURE_2D, r->layers[src].tex);
            gl.glActiveTexture(GL_TEXTURE6);
            gl.glBindTexture(GL_TEXTURE_2D, kind == GP_PRIM_FX_SHADOW ? r->layers[T_AUX].tex : 0);
            gl.glActiveTexture(GL_TEXTURE0);
            gl.glDisable(GL_BLEND); /* (a pass writes its result whole) */
            draw_run(r, scene, i, 1);
            gl.glEnable(GL_BLEND);
            gl.glActiveTexture(GL_TEXTURE5);
            gl.glBindTexture(GL_TEXTURE_2D, 0);
            gl.glActiveTexture(GL_TEXTURE6);
            gl.glBindTexture(GL_TEXTURE_2D, 0);
            gl.glActiveTexture(GL_TEXTURE0);
            if(dst != T_AUX) {
                cur = dst;
            }
            continue;
        }
        if(kind == GP_PRIM_MASK_END) { /* the mask's layer is done: it is kept for the END that follows (its own layer goes on being the open one) */
            if(i > run) {
                draw_run(r, scene, run, i - run);
            }
            run = i + 1;
            if(skipped) {
                skipped--;
                continue;
            }
            if(!depth) {
                continue;
            }
            depth--;
            mask_tex = r->layers[depth].tex;
            gl.glBindFramebuffer(GL_FRAMEBUFFER, depth ? r->layers[depth - 1].fbo : 0);
            continue;
        }
        if(kind == GP_PRIM_LAYER_BEGIN) {
            if(i > run) {
                draw_run(r, scene, run, i - run);
            }
            run = i + 1;
            if(skipped || depth >= GP_MAX_LAYERS || !layer_target(r, depth, px_w, px_h)) {
                skipped++;
                continue;
            }
            gl.glBindFramebuffer(GL_FRAMEBUFFER, r->layers[depth].fbo);
            int x0 = (int)floorf(in->bounds[0] * scale + 0.5f), x1 = (int)floorf((in->bounds[0] + in->bounds[2]) * scale + 0.5f);
            int y0 = (int)floorf(in->bounds[1] * scale + 0.5f), y1 = (int)floorf((in->bounds[1] + in->bounds[3]) * scale + 0.5f);
            if(x0 < 0) {
                x0 = 0;
            }
            if(y0 < 0) {
                y0 = 0;
            }
            if(x1 > px_w) {
                x1 = px_w;
            }
            if(y1 > px_h) {
                y1 = px_h;
            }
            if(x1 > x0 && y1 > y0) {
                gl.glEnable(GL_SCISSOR_TEST);
                gl.glScissor(x0, px_h - y1, x1 - x0, y1 - y0); /* (GL counts rows from the bottom) */
                gl.glClearColor(0, 0, 0, 0);
                gl.glClear(GL_COLOR_BUFFER_BIT);
                gl.glDisable(GL_SCISSOR_TEST);
            }
            depth++;
            continue;
        }
        if(kind == GP_PRIM_LAYER_END) {
            if(i > run) {
                draw_run(r, scene, run, i - run);
            }
            run = i + 1;
            if(skipped) {
                skipped--;
                continue;
            }
            if(!depth) {
                continue;
            }
            depth--;
            gl.glBindFramebuffer(GL_FRAMEBUFFER, depth ? r->layers[depth - 1].fbo : 0);
            gl.glActiveTexture(GL_TEXTURE5);
            gl.glBindTexture(GL_TEXTURE_2D, r->layers[cur >= 0 ? cur : depth].tex);
            gl.glActiveTexture(GL_TEXTURE6);
            gl.glBindTexture(GL_TEXTURE_2D, in->uv[1] > 0.5f ? mask_tex : 0);
            bool blended = in->uv[0] > 0.5f && layer_target(r, T_BACKDROP, px_w, px_h);
            if(blended) { /* what is under it, over the region, into the backdrop texture: the quad computes the blend and writes it whole */
                int x0 = (int)floorf(in->bounds[0] * scale + 0.5f), x1 = (int)floorf((in->bounds[0] + in->bounds[2]) * scale + 0.5f);
                int y0 = (int)floorf(in->bounds[1] * scale + 0.5f), y1 = (int)floorf((in->bounds[1] + in->bounds[3]) * scale + 0.5f);
                if(x0 < 0) {
                    x0 = 0;
                }
                if(y0 < 0) {
                    y0 = 0;
                }
                if(x1 > px_w) {
                    x1 = px_w;
                }
                if(y1 > px_h) {
                    y1 = px_h;
                }
                gl.glActiveTexture(GL_TEXTURE7);
                gl.glBindTexture(GL_TEXTURE_2D, r->layers[T_BACKDROP].tex);
                backdrop_copy(r, px_h, x0, y0, x1, y1);
                gl.glDisable(GL_BLEND);
            }
            gl.glActiveTexture(GL_TEXTURE0);
            draw_run(r, scene, i, 1);
            if(blended) {
                gl.glEnable(GL_BLEND);
            }
            for(GLenum u = GL_TEXTURE5; u <= GL_TEXTURE7; u++) { /* (none bound while it is drawn into again) */
                gl.glActiveTexture(u);
                gl.glBindTexture(GL_TEXTURE_2D, 0);
            }
            gl.glActiveTexture(GL_TEXTURE0);
            cur = -1;
            mask_tex = 0;
            continue;
        }
        if(kind != GP_PRIM_IMAGE) {
            continue;
        }
        gp_gfx_image* im = gp_gfx_image_get(gfx, (uint32_t)in->_pad, (uint32_t)in->border_color[0]);
        if(!im) {
            continue; /* released since it was painted: nothing to draw (the next frame has no instance) */
        }
        if(i > run) {
            draw_run(r, scene, run, i - run);
        }
        upload_image(r, im);
        gl.glActiveTexture(GL_TEXTURE4);
        gl.glBindTexture(GL_TEXTURE_2D, im->tex);
        gl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, in->blur >= 500.f ? GL_NEAREST : GL_LINEAR);
        gl.glActiveTexture(GL_TEXTURE0);
        draw_run(r, scene, i, 1);
        run = i + 1;
    }
    if(scene->len > run) {
        draw_run(r, scene, run, scene->len - run);
    }
    if(depth) {
        gl.glBindFramebuffer(GL_FRAMEBUFFER, 0); /* (a scene whose layers do not close: what was in them is lost, the window is what is shown) */
    }
}

static uint8_t* gl_read_pixels(gp_renderer* base, gp_window* w)
{
    int px_w = w->px_w, px_h = w->px_h;
    uint8_t* raw = malloc((size_t)px_w * px_h * 3);
    uint8_t* out = malloc((size_t)px_w * px_h * 3);
    gl.glPixelStorei(GL_PACK_ALIGNMENT, 1);
    gl.glReadPixels(0, 0, px_w, px_h, GL_RGB, GL_UNSIGNED_BYTE, raw);
    for(int y = 0; y < px_h; y++) { /* GL origin is bottom-left */
        memcpy(out + (size_t)y * px_w * 3, raw + (size_t)(px_h - 1 - y) * px_w * 3, (size_t)px_w * 3);
    }
    free(raw);
    return out;
}

static void gl_present(gp_renderer* base, gp_window* w)
{
    gp_platform_swap(w);
}

static void gl_window_gone(gp_renderer* base, gp_window* w)
{
}

static const gp_renderer_ops gl_ops = { gl_free, gl_draw, gl_read_pixels, gl_present, gl_window_gone };
