/*
 * Custom GPU drawing (gp_custom): the app's texture shown in the element's content box, the right way up, at the element's size in device pixels;
 * clipped by an ancestor, under what is painted after it; drawn again by gp_custom_invalidate with no view rendered and the cached view reused;
 * its children over it; GL state the callback left (a bound framebuffer, the scissor test) not leaking into gpuic's pass; `destroy` and `init` when the renderer is made
 * again (the last window closed, another opened); and `destroy` on release. The app side only clears (GL: an FBO; Vulkan: dynamic rendering),
 * so it needs no shaders. Frames are read back. Needs a display; runs as custom_test_vk with Vulkan.
 */
#include "../src/internal.h"

#include "compat.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if __has_include(<vulkan/vulkan.h>)
# define VK_NO_PROTOTYPES
# include <vulkan/vulkan.h>
# define HAVE_VK 1
#endif

static int failures;
#define CHECK(cond, ...)                                    \
    do {                                                    \
        if(!(cond)) {                                       \
            if(failures++ < 40) {                           \
                printf("FAIL %s:%d: ", __FILE__, __LINE__); \
                printf(__VA_ARGS__);                        \
                printf("\n");                               \
            }                                               \
        }                                                   \
    } while(0)

#define PAGE gp_rgb(0x14141e)

/* ---------------------------------------------------------------- the app's side: a texture whose top half is `top` and bottom half blue */

typedef struct
{
    float top[3];
    int inits, destroys, draws, px_w, px_h;
    float w, h;
    /* GL */
    unsigned tex, fbo;
#ifdef HAVE_VK
    PFN_vkGetDeviceProcAddr gdpa;
    VkDevice dev;
    VkPhysicalDeviceMemoryProperties mp;
    VkImage img;
    VkDeviceMemory mem;
    VkImageView view;
#endif
    int tw, th;
} Side;

typedef void (*gl_gen)(int, unsigned*);
typedef void (*gl_bind)(unsigned, unsigned);
typedef void (*gl_teximage)(unsigned, int, int, int, int, int, unsigned, unsigned, const void*);
typedef void (*gl_fbtex)(unsigned, unsigned, unsigned, unsigned, int);
typedef void (*gl_4i)(int, int, int, int);
typedef void (*gl_4f)(float, float, float, float);
typedef void (*gl_1u)(unsigned);

static void gl_destroy(Side* s, const gp_gpu* g)
{
    if(s->tex) {
        ((gl_gen)g->gl_get_proc_address("glDeleteTextures"))(1, &s->tex);
        ((gl_gen)g->gl_get_proc_address("glDeleteFramebuffers"))(1, &s->fbo);
    }
    s->tex = s->fbo = 0;
}

static uint64_t gl_draw(Side* s, const gp_custom_frame* f)
{
    void* (*p)(const char*) = f->gpu->gl_get_proc_address;
    if(!s->tex) {
        ((gl_gen)p("glGenTextures"))(1, &s->tex);
        ((gl_gen)p("glGenFramebuffers"))(1, &s->fbo);
        s->tw = 0;
    }
    if(s->tw != f->px_width || s->th != f->px_height) {
        s->tw = f->px_width;
        s->th = f->px_height;
        ((gl_bind)p("glBindTexture"))(0x0DE1 /* TEXTURE_2D */, s->tex);
        ((gl_teximage)p("glTexImage2D"))(0x0DE1, 0, 0x8058 /* RGBA8 */, s->tw, s->th, 0, 0x1908 /* RGBA */, 0x1401 /* UNSIGNED_BYTE */, NULL);
        ((gl_bind)p("glBindFramebuffer"))(0x8D40 /* FRAMEBUFFER */, s->fbo);
        ((gl_fbtex)p("glFramebufferTexture2D"))(0x8D40, 0x8CE0 /* COLOR_ATTACHMENT0 */, 0x0DE1, s->tex, 0);
    }
    ((gl_bind)p("glBindFramebuffer"))(0x8D40, s->fbo);
    ((gl_4i)p("glViewport"))(0, 0, s->tw, s->th);
    ((gl_4f)p("glClearColor"))(0, 0, 1, 1);
    ((gl_1u)p("glClear"))(0x4000 /* COLOR_BUFFER_BIT */);
    ((gl_1u)p("glEnable"))(0x0C11 /* SCISSOR_TEST */);
    ((gl_4i)p("glScissor"))(0, s->th / 2, s->tw, s->th - s->th / 2); /* (rows go up: the top half) */
    ((gl_4f)p("glClearColor"))(s->top[0], s->top[1], s->top[2], 1);
    ((gl_1u)p("glClear"))(0x4000);
    return s->tex;                                                   /* the framebuffer and the scissor test are left as they are: gpuic sets them back */
}

#ifdef HAVE_VK
# define VKF(name) ((PFN_##name)s->gdpa(s->dev, #name))
static void vk_destroy(Side* s)
{
    if(s->img) {
        VKF(vkDestroyImageView)(s->dev, s->view, NULL);
        VKF(vkDestroyImage)(s->dev, s->img, NULL);
        VKF(vkFreeMemory)(s->dev, s->mem, NULL);
    }
    s->img = VK_NULL_HANDLE;
}

static uint64_t vk_draw(Side* s, const gp_custom_frame* f)
{
    VkCommandBuffer cmd = f->vk_command_buffer;
    if(s->img && (s->tw != f->px_width || s->th != f->px_height)) {
        vk_destroy(s); /* (the last frame is done, and this one has not used it yet) */
    }
    if(!s->img) {
        s->tw = f->px_width;
        s->th = f->px_height;
        VkImageCreateInfo ici = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = VK_FORMAT_R8G8B8A8_UNORM, .extent = { (uint32_t)s->tw, (uint32_t)s->th, 1 },
                       .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL, .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT
        };
        VKF(vkCreateImage)(s->dev, &ici, NULL, &s->img);
        VkMemoryRequirements mr;
        VKF(vkGetImageMemoryRequirements)(s->dev, s->img, &mr);
        uint32_t type = 0;
        while(type < s->mp.memoryTypeCount && !(mr.memoryTypeBits & (1u << type))) {
            type++;
        }
        VkMemoryAllocateInfo mai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size, .memoryTypeIndex = type };
        VKF(vkAllocateMemory)(s->dev, &mai, NULL, &s->mem);
        VKF(vkBindImageMemory)(s->dev, s->img, s->mem, 0);
        VkImageViewCreateInfo vci = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = s->img, .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = ici.format, .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 }
        };
        VKF(vkCreateImageView)(s->dev, &vci, NULL, &s->view);
        VkImageMemoryBarrier b = { .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_GENERAL, .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = s->img, .subresourceRange = vci.subresourceRange };
        VKF(vkCmdPipelineBarrier)(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0, NULL, 0, NULL, 1, &b);
    }
    VkRenderingAttachmentInfo att = { .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, .imageView = s->view, .imageLayout = VK_IMAGE_LAYOUT_GENERAL, .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR, .storeOp = VK_ATTACHMENT_STORE_OP_STORE, .clearValue = { .color = { { 0, 0, 1, 1 } } } };
    VkRenderingInfo ri = {
        .sType = VK_STRUCTURE_TYPE_RENDERING_INFO, .renderArea = { { 0, 0 }, { (uint32_t)s->tw, (uint32_t)s->th } },
             .layerCount = 1, .colorAttachmentCount = 1, .pColorAttachments = &att
    };
    VKF(vkCmdBeginRendering)(cmd, &ri);
    VkClearAttachment ca = { .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .colorAttachment = 0, .clearValue = { .color = { { s->top[0], s->top[1], s->top[2], 1 } } } };
    VkClearRect cr = {
        { { 0, 0 }, { (uint32_t)s->tw, (uint32_t)(s->th / 2) } },
        0,
        1
    }; /* (rows go down: the top half) */
    VKF(vkCmdClearAttachments)(cmd, 1, &ca, 1, &cr);
    VKF(vkCmdEndRendering)(cmd);
    return (uint64_t)s->view;
}
#endif

static void side_init(void* data, const gp_gpu* g)
{
    Side* s = data;
    s->inits++;
#ifdef HAVE_VK
    if(g->backend == GP_GPU_VULKAN) {
        PFN_vkGetInstanceProcAddr gipa = (PFN_vkGetInstanceProcAddr)(void (*)(void))g->vk_get_instance_proc_addr;
        s->gdpa = (PFN_vkGetDeviceProcAddr)gipa(g->vk_instance, "vkGetDeviceProcAddr");
        s->dev = g->vk_device;
        ((PFN_vkGetPhysicalDeviceMemoryProperties)gipa(g->vk_instance, "vkGetPhysicalDeviceMemoryProperties"))(g->vk_physical_device, &s->mp);
    }
#endif
}

static void side_destroy(void* data, const gp_gpu* g)
{
    Side* s = data;
    s->destroys++;
#ifdef HAVE_VK
    if(g->backend == GP_GPU_VULKAN) {
        vk_destroy(s);
        return;
    }
#endif
    gl_destroy(s, g);
}

static uint64_t side_draw(void* data, const gp_custom_frame* f)
{
    Side* s = data;
    s->draws++;
    s->w = f->width;
    s->h = f->height;
    s->px_w = f->px_width;
    s->px_h = f->px_height;
#ifdef HAVE_VK
    if(f->gpu->backend == GP_GPU_VULKAN) {
        return vk_draw(s, f);
    }
#endif
    return gl_draw(s, f);
}

/* ---------------------------------------------------------------- the views */

static gp_app* app;
static Side side = {
    .top = { 0, 1, 0 }
};
static gp_custom custom;
static gp_entity inner;

/* cached: a 60 x 100 box at (10, 10) that clips; in it the custom, 80 x 40 at its top left (20 px of it cut off on the right), and a
 * white square at (20, 10) in the box painted after it; the custom's own child, a yellow square at (40, 20) in it */
static gp_node* inner_render(void* self, gp_ctx* cx)
{
    return gp_div(gp_size_full(), gp_div(gp_absolute(), gp_left(10), gp_top(10), gp_w(60), gp_h(100), gp_overflow_hidden(), gp_custom_el(custom, gp_absolute(), gp_left(0), gp_top(0), gp_w(80), gp_h(40), gp_div(gp_absolute(), gp_left(40), gp_top(20), gp_size_px(10), gp_bg(gp_rgb(0xffff00)))), gp_div(gp_absolute(), gp_left(20), gp_top(10), gp_size_px(10), gp_bg(gp_rgb(0xffffff)))));
}

static gp_node* root_render(void* self, gp_ctx* cx)
{
    return gp_div(gp_size_full(), gp_bg(PAGE), gp_child_view_cached(inner));
}

static gp_window* open_window(void)
{
    gp_entity root = gp_view_new(app, 0, NULL, root_render, NULL);
    return gp_open_window(app, &(gp_window_options){ .title = "custom_test", .width = 160, .height = 120 }, root);
}

static const uint8_t* frame(gp_window* w)
{
    gp_debug_capture_next(app);
    for(int i = 0; i < 500 && !app->capture_px; i++) {
        w->dirty_paint = true;
        gp_app_step(app, NULL);
        usleep(4000);
    }
    return app->capture_px;
}

/* the pixel at logical (x, y) is r, g, b (0..255, give or take) */
static bool px_is(gp_window* w, const uint8_t* px, float x, float y, int r, int g, int b)
{
    if(!px) {
        return false;
    }
    int ix = (int)(x * w->scale), iy = (int)(y * w->scale);
    const uint8_t* p = px + ((size_t)iy * (size_t)w->px_w + (size_t)ix) * 3;
    bool ok = abs(p[0] - r) <= 3 && abs(p[1] - g) <= 3 && abs(p[2] - b) <= 3;
    if(!ok) {
        printf("  (%g, %g) is %d %d %d, not %d %d %d\n", x, y, p[0], p[1], p[2], r, g, b);
    }
    return ok;
}

static void check_picture(gp_window* w, const char* when, int tr, int tg, int tb)
{
    const uint8_t* px = frame(w);
    CHECK(px_is(w, px, 15, 15, tr, tg, tb), "%s: the top half is the app's top color", when);
    CHECK(px_is(w, px, 15, 45, 0, 0, 255), "%s: the bottom half is blue (the right way up)", when);
    CHECK(px_is(w, px, 35, 25, 255, 255, 255), "%s: what is painted after it is on top", when);
    CHECK(px_is(w, px, 55, 35, 255, 255, 0), "%s: its children are drawn over it", when);
    CHECK(px_is(w, px, 75, 15, 0x14, 0x14, 0x1e), "%s: cut by the ancestor's clip", when);
    CHECK(px_is(w, px, 15, 55, 0x14, 0x14, 0x1e), "%s: nothing below the box", when);
    CHECK(side.w == 80 && side.h == 40, "%s: the logical size is 80 x 40 (%g x %g)", when, side.w, side.h);
    CHECK(side.px_w == (int)(80 * w->scale + 0.5f) && side.px_h == (int)(40 * w->scale + 0.5f), "%s: the size in device pixels (%d x %d at %g)", when, side.px_w, side.px_h, w->scale);
}

int main(void)
{
    setenv("GPUIC_IGNORE_REAL_MOUSE", "1", 1);
    app = gp_app_new();
    if(!app) {
        printf("custom_test: no platform\n");
        return 1;
    }
    custom = gp_custom_new(app, &(gp_custom_fns){ side_init, side_destroy, side_draw }, &side);
    CHECK(custom.generation, "made");
    inner = gp_view_new(app, 0, NULL, inner_render, NULL);
    gp_window* w = open_window();
    CHECK(w, "a window");

    check_picture(w, "first", 0, 255, 0);
    CHECK(side.inits == 1 && side.destroys == 0, "init once (%d, %d)", side.inits, side.destroys);

    /* a new picture: drawn again with nothing rendered, the cached view reused */
    gp_stats a, b;
    gp_app_stats(app, &a);
    side.top[0] = 1;
    side.top[1] = 0;
    gp_debug_capture_next(app);
    gp_custom_invalidate(app, custom); /* (alone: the test does not ask for frames of its own here) */
    for(int i = 0; i < 500 && !app->capture_px; i++) {
        gp_app_step(app, NULL);
        usleep(4000);
    }
    const uint8_t* px = app->capture_px;
    gp_app_stats(app, &b);
    CHECK(b.draws > a.draws, "invalidating draws");
    CHECK(px_is(w, px, 15, 15, 255, 0, 0), "the new picture shows");
    CHECK(b.view_renders == a.view_renders, "no view rendered (%lu)", b.view_renders - a.view_renders);
    CHECK(b.paint_hits > a.paint_hits && b.paint_misses == a.paint_misses, "the cached view was reused (hits %lu, misses %lu)", b.paint_hits - a.paint_hits, b.paint_misses - a.paint_misses);

    /* the last window closes: the renderer goes, and `destroy` with it; another window: a new renderer, `init` again */
    gp_window_close(w);
    for(int i = 0; i < 100 && app->nwindows; i++) {
        gp_app_step(app, NULL);
    }
    CHECK(!app->renderer && side.destroys == 1, "destroyed with the renderer (%d)", side.destroys);
    w = open_window();
    check_picture(w, "a new renderer", 255, 0, 0);
    CHECK(side.inits == 2, "init again (%d)", side.inits);

    /* released: `destroy` at the next frame, and nothing drawn where it was */
    gp_custom_release(app, custom);
    px = frame(w);
    CHECK(side.destroys == 2, "destroyed on release (%d)", side.destroys);
    CHECK(px_is(w, px, 15, 45, 0x14, 0x14, 0x1e), "nothing drawn once released");
    gp_custom_invalidate(app, custom); /* (a stale handle: nothing) */

    gp_app_free(app);
    CHECK(side.destroys == 2 && side.inits == 2, "no more calls at the end (%d, %d)", side.inits, side.destroys);
    printf(failures ? "custom_test: %d FAILURE(S)\n" : "custom_test: all checks passed\n", failures);
    return failures != 0;
}
