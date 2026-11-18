/*
 * Vulkan 1.3 renderer: what render_gl.c does, with the same shaders (src/shaders/, compiled to SPIR-V by tools/gen_shaders.py) and the same
 * walk over the scene: one instanced draw, split where an image, a layer or a filter pass needs other textures or another target.
 *
 * A frame is drawn into an image of the window's own (the "main" target), then copied to the swapchain's: what is under a backdrop or a
 * blend is copied out of the target being drawn on, and a test reads the frame back, both of which a swapchain image would make harder.
 * Every target and texture stays in the GENERAL layout (the swapchain's images excepted), and a full barrier separates the uses of one, so
 * the walk only has to say where it draws and what it reads. One frame is in flight: a draw waits for the last one to be done, which is
 * also what makes reusing the instance and staging buffers safe. (Two in flight were tried: the wait is 0.001 ms a frame once the GPU has
 * clocked up, and while it does, the CPU waited about as long with two.) Textures are pushed per draw (VK_KHR_push_descriptor), and drawing is
 * dynamic rendering (no render passes). The loader is SDL's (no link to it); every entry point is looked up.
 */
#include "internal.h"

#if !__has_include(<vulkan/vulkan.h>)
gp_renderer* gp_vk_renderer_new(gp_app* app, gp_window* first)
{
    fprintf(stderr, "gpuic: built without the Vulkan headers: no Vulkan renderer\n");
    return NULL;
}
#else

# define VK_NO_PROTOTYPES
# include <vulkan/vulkan.h>

# include "shaders_vk.inc" /* vs_spv, fs_spv */

# define VK_INSTANCE_FUNCS(X)                     \
     X(vkDestroyInstance)                         \
     X(vkEnumeratePhysicalDevices)                \
     X(vkGetPhysicalDeviceProperties)             \
     X(vkGetPhysicalDeviceFeatures)               \
     X(vkGetPhysicalDeviceFeatures2)              \
     X(vkGetPhysicalDeviceQueueFamilyProperties)  \
     X(vkGetPhysicalDeviceMemoryProperties)       \
     X(vkGetPhysicalDeviceFormatProperties)       \
     X(vkEnumerateDeviceExtensionProperties)      \
     X(vkGetPhysicalDeviceSurfaceSupportKHR)      \
     X(vkGetPhysicalDeviceSurfaceCapabilitiesKHR) \
     X(vkGetPhysicalDeviceSurfaceFormatsKHR)      \
     X(vkGetPhysicalDeviceSurfacePresentModesKHR) \
     X(vkDestroySurfaceKHR)                       \
     X(vkCreateDevice)                            \
     X(vkGetDeviceProcAddr)

# define VK_DEVICE_FUNCS(X)           \
     X(vkDestroyDevice)               \
     X(vkGetDeviceQueue)              \
     X(vkDeviceWaitIdle)              \
     X(vkCreateSwapchainKHR)          \
     X(vkDestroySwapchainKHR)         \
     X(vkGetSwapchainImagesKHR)       \
     X(vkAcquireNextImageKHR)         \
     X(vkQueuePresentKHR)             \
     X(vkQueueSubmit)                 \
     X(vkCreateCommandPool)           \
     X(vkDestroyCommandPool)          \
     X(vkAllocateCommandBuffers)      \
     X(vkBeginCommandBuffer)          \
     X(vkEndCommandBuffer)            \
     X(vkResetCommandBuffer)          \
     X(vkCreateFence)                 \
     X(vkDestroyFence)                \
     X(vkWaitForFences)               \
     X(vkResetFences)                 \
     X(vkCreateSemaphore)             \
     X(vkDestroySemaphore)            \
     X(vkCreateImage)                 \
     X(vkDestroyImage)                \
     X(vkCreateImageView)             \
     X(vkDestroyImageView)            \
     X(vkGetImageMemoryRequirements)  \
     X(vkAllocateMemory)              \
     X(vkFreeMemory)                  \
     X(vkBindImageMemory)             \
     X(vkCreateBuffer)                \
     X(vkDestroyBuffer)               \
     X(vkGetBufferMemoryRequirements) \
     X(vkBindBufferMemory)            \
     X(vkMapMemory)                   \
     X(vkCreateSampler)               \
     X(vkDestroySampler)              \
     X(vkCreateShaderModule)          \
     X(vkDestroyShaderModule)         \
     X(vkCreateDescriptorSetLayout)   \
     X(vkDestroyDescriptorSetLayout)  \
     X(vkCreatePipelineLayout)        \
     X(vkDestroyPipelineLayout)       \
     X(vkCreateGraphicsPipelines)     \
     X(vkDestroyPipeline)             \
     X(vkCmdBeginRendering)           \
     X(vkCmdEndRendering)             \
     X(vkCmdBindPipeline)             \
     X(vkCmdBindVertexBuffers)        \
     X(vkCmdPushConstants)            \
     X(vkCmdPushDescriptorSetKHR)     \
     X(vkCmdDraw)                     \
     X(vkCmdSetViewport)              \
     X(vkCmdSetScissor)               \
     X(vkCmdPipelineBarrier)          \
     X(vkCmdCopyBufferToImage)        \
     X(vkCmdCopyImage)                \
     X(vkCmdBlitImage)                \
     X(vkCmdCopyImageToBuffer)        \
     X(vkCmdClearColorImage)          \
     X(vkCmdClearAttachments)

/* The entry points, each renderer's own: a device's are good for that device only, and two apps in one process (two plugins) have two */
typedef struct
{
    PFN_vkGetInstanceProcAddr vkGetInstanceProcAddr;
    PFN_vkCreateInstance vkCreateInstance;
# define X(name) PFN_##name name;
    VK_INSTANCE_FUNCS(X)
    VK_DEVICE_FUNCS(X)
# undef X
} vk_funcs;

/* the window-sized targets, as in render_gl.c: one layer per depth, two for filter passes, one aside (a drop shadow's shape), one for what is under
 * a blend or a backdrop */
enum { T_FX0 = GP_MAX_LAYERS,
    T_FX1,
    T_AUX,
    T_BACKDROP,
    T_COUNT,
};
/* the textures a draw reads, in the shader's bindings */
enum { B_ATLAS,
    B_COLOR,
    B_LCD,
    B_RAMPS,
    B_IMAGE,
    B_LAYER,
    B_AUX,
    B_BACKDROP,
    B_COUNT,
};

typedef struct
{
    VkImage image;
    VkDeviceMemory mem;
    VkImageView view;
    int w, h;
    uint32_t mips;
    bool fresh; /* made since the last frame: its layout is still UNDEFINED */
} vk_image;

typedef struct
{
    VkBuffer buf;
    VkDeviceMemory mem;
    void* map;
    VkDeviceSize size;
} vk_buffer;

/* What a window has: its surface and swapchain, and the image its frames are drawn into. */
typedef struct vk_window vk_window;
struct vk_window
{
    gp_window* w;
    vk_window* next;
    VkSurfaceKHR surface;
    VkSwapchainKHR swapchain;
    VkImage* images;
    VkSemaphore* done;  /* per swapchain image: its frame is drawn (what presenting it waits for) */
    uint32_t nimages;
    VkSemaphore acquired;
    int sw, sh;         /* the swapchain's size */
    int want_w, want_h; /* the window's size in pixels when it was made: made again when that changes (or presenting says so: 0) */
    vk_image main;
    int pending;        /* the swapchain image drawn and not yet presented, or -1 */
};

typedef struct
{
    gp_renderer base;
    vk_funcs fn;
    VkInstance instance;
    VkPhysicalDevice gpu;
    VkDevice device;
    VkQueue queue;
    uint32_t family;
    VkFormat format; /* of the swapchains, and so of every target (copies between them are bit for bit) */
    VkPhysicalDeviceMemoryProperties memprops;
    VkCommandPool pool;
    VkCommandBuffer cmd;
    VkFence fence;
    bool submitted; /* the fence will be signalled: wait for it before touching anything the last frame used */
    VkDescriptorSetLayout set_layout;
    VkPipelineLayout layout;
    VkPipeline blended, opaque;
    VkSampler nearest, linear, mipmapped, mip_nearest;
    vk_image atlas, color_atlas, lcd_atlas, ramps, dummy;
    vk_image targets[T_COUNT];
    int target_w, target_h;
    vk_image* textures;  /* images' textures: gp_gfx_image.tex is a slot here + 1 */
    uint32_t ntextures, cap_textures;
    vk_buffer instances, staging, readback;
    VkDeviceSize staged; /* bytes of the staging buffer used by this frame */
    vk_window* windows;
    /* the walk: the target being drawn into (NULL: none), whether rendering is on, the pipeline bound, and what binds 4..7 read */
    vk_image* target;
    bool rendering;
    VkPipeline bound;
    VkImageView bind_view[B_COUNT];
    VkSampler bind_sampler[B_COUNT];
    float push[4];
} vk_renderer;

static const gp_renderer_ops vk_ops;

/* ---------------------------------------------------------------- memory, images, buffers */

static uint32_t memory_type(vk_renderer* r, uint32_t bits, VkMemoryPropertyFlags want)
{
    for(uint32_t i = 0; i < r->memprops.memoryTypeCount; i++) {
        if((bits & (1u << i)) && (r->memprops.memoryTypes[i].propertyFlags & want) == want) {
            return i;
        }
    }
    return UINT32_MAX;
}

static bool image_make(vk_renderer* r, vk_image* im, int w, int h, VkFormat format, uint32_t mips, VkImageUsageFlags usage)
{
    memset(im, 0, sizeof *im);
    VkImageCreateInfo ci = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = format,
        .extent = { (uint32_t)w, (uint32_t)h, 1 },
        .mipLevels = mips,
        .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = usage,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED
    };
    if(r->fn.vkCreateImage(r->device, &ci, NULL, &im->image) != VK_SUCCESS) {
        return false;
    }
    VkMemoryRequirements req;
    r->fn.vkGetImageMemoryRequirements(r->device, im->image, &req);
    VkMemoryAllocateInfo ai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = req.size, .memoryTypeIndex = memory_type(r, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) };
    if(ai.memoryTypeIndex == UINT32_MAX || r->fn.vkAllocateMemory(r->device, &ai, NULL, &im->mem) != VK_SUCCESS) {
        r->fn.vkDestroyImage(r->device, im->image, NULL);
        im->image = VK_NULL_HANDLE;
        return false;
    }
    r->fn.vkBindImageMemory(r->device, im->image, im->mem, 0);
    VkImageViewCreateInfo vi = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image = im->image,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = format,
        .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, mips, 0, 1 }
    };
    r->fn.vkCreateImageView(r->device, &vi, NULL, &im->view);
    im->w = w;
    im->h = h;
    im->mips = mips;
    im->fresh = true;
    return true;
}

static void image_free(vk_renderer* r, vk_image* im)
{
    if(im->view) {
        r->fn.vkDestroyImageView(r->device, im->view, NULL);
    }
    if(im->image) {
        r->fn.vkDestroyImage(r->device, im->image, NULL);
    }
    if(im->mem) {
        r->fn.vkFreeMemory(r->device, im->mem, NULL);
    }
    memset(im, 0, sizeof *im);
}

/* everything before, done (writes visible) before anything after: what the walk puts between two uses of one image */
static void full_barrier(vk_renderer* r)
{
    VkMemoryBarrier mb = { .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT, .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT };
    r->fn.vkCmdPipelineBarrier(r->cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
}

static void layout_change(vk_renderer* r, VkImage image, uint32_t mips, VkImageLayout from, VkImageLayout to)
{
    VkImageMemoryBarrier b = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
        .oldLayout = from,
        .newLayout = to,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = image,
        .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, mips, 0, 1 }
    };
    r->fn.vkCmdPipelineBarrier(r->cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0, NULL, 1, &b);
}

/* an image made since the last frame: into GENERAL, cleared (nothing reads what was never written as anything but transparent) */
static void image_ready(vk_renderer* r, vk_image* im)
{
    if(!im->fresh) {
        return;
    }
    im->fresh = false;
    layout_change(r, im->image, im->mips, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
    VkClearColorValue zero = {
        { 0, 0, 0, 0 }
    };
    VkImageSubresourceRange range = { VK_IMAGE_ASPECT_COLOR_BIT, 0, im->mips, 0, 1 };
    r->fn.vkCmdClearColorImage(r->cmd, im->image, VK_IMAGE_LAYOUT_GENERAL, &zero, 1, &range);
    full_barrier(r);
}

static bool buffer_make(vk_renderer* r, vk_buffer* b, VkDeviceSize size, VkBufferUsageFlags usage)
{
    memset(b, 0, sizeof *b);
    VkBufferCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = size, .usage = usage, .sharingMode = VK_SHARING_MODE_EXCLUSIVE };
    if(r->fn.vkCreateBuffer(r->device, &ci, NULL, &b->buf) != VK_SUCCESS) {
        return false;
    }
    VkMemoryRequirements req;
    r->fn.vkGetBufferMemoryRequirements(r->device, b->buf, &req);
    VkMemoryAllocateInfo ai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = req.size, .memoryTypeIndex = memory_type(r, req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) };
    if(ai.memoryTypeIndex == UINT32_MAX || r->fn.vkAllocateMemory(r->device, &ai, NULL, &b->mem) != VK_SUCCESS) {
        r->fn.vkDestroyBuffer(r->device, b->buf, NULL);
        b->buf = VK_NULL_HANDLE;
        return false;
    }
    r->fn.vkBindBufferMemory(r->device, b->buf, b->mem, 0);
    r->fn.vkMapMemory(r->device, b->mem, 0, VK_WHOLE_SIZE, 0, &b->map);
    b->size = size;
    return true;
}

static void buffer_free(vk_renderer* r, vk_buffer* b)
{
    if(b->buf) {
        r->fn.vkDestroyBuffer(r->device, b->buf, NULL);
    }
    if(b->mem) {
        r->fn.vkFreeMemory(r->device, b->mem, NULL); /* (unmaps it) */
    }
    memset(b, 0, sizeof *b);
}

/* a buffer at least `size` big (what it held is not kept) */
static bool buffer_reserve(vk_renderer* r, vk_buffer* b, VkDeviceSize size, VkBufferUsageFlags usage)
{
    if(b->size >= size) {
        return true;
    }
    buffer_free(r, b);
    VkDeviceSize n = 1 << 16;
    while(n < size) {
        n *= 2;
    }
    return buffer_make(r, b, n, usage);
}

/* Bytes into the staging buffer for a copy recorded this frame: where they are, or -1 (the staging buffer only grows between frames: a frame that
 * needs more than it holds gets it next time, and draws what it has now) */
static VkDeviceSize stage(vk_renderer* r, const uint8_t* rows, size_t row_bytes, size_t stride, int nrows)
{
    VkDeviceSize need = (VkDeviceSize)row_bytes * (VkDeviceSize)nrows, at = (r->staged + 15) & ~(VkDeviceSize)15;
    if(at + need > r->staging.size) {
        return (VkDeviceSize)-1;
    }
    for(int y = 0; y < nrows; y++) {
        memcpy((uint8_t*)r->staging.map + at + (VkDeviceSize)y * row_bytes, rows + (size_t)y * stride, row_bytes);
    }
    r->staged = at + need;
    return at;
}

/* a rectangle of pixels (tightly packed rows of `bpp` bytes, `stride` apart) into an image */
static void upload(vk_renderer* r, vk_image* im, int x, int y, int w, int h, const uint8_t* px, size_t stride, int bpp)
{
    if(w <= 0 || h <= 0) {
        return;
    }
    VkDeviceSize at = stage(r, px, (size_t)w * (size_t)bpp, stride, h);
    if(at == (VkDeviceSize)-1) {
        return;
    }
    VkBufferImageCopy c = {
        .bufferOffset = at, .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
             .imageOffset = { x, y, 0 },
             .imageExtent = { (uint32_t)w, (uint32_t)h, 1 }
    };
    r->fn.vkCmdCopyBufferToImage(r->cmd, r->staging.buf, im->image, VK_IMAGE_LAYOUT_GENERAL, 1, &c);
}

/* ---------------------------------------------------------------- setup */

static bool load_device_funcs(vk_renderer* r, VkDevice d)
{
# define X(name)                                                   \
     r->fn.name = (PFN_##name)r->fn.vkGetDeviceProcAddr(d, #name); \
     if(!r->fn.name) {                                             \
         fprintf(stderr, "gpuic: Vulkan: missing %s\n", #name);    \
         return false;                                             \
     }
    VK_DEVICE_FUNCS(X)
# undef X
    return true;
}

static bool has_extension(vk_renderer* r, VkPhysicalDevice gpu, const char* name)
{
    uint32_t n = 0;
    r->fn.vkEnumerateDeviceExtensionProperties(gpu, NULL, &n, NULL);
    VkExtensionProperties* e = calloc(n ? n : 1, sizeof *e);
    r->fn.vkEnumerateDeviceExtensionProperties(gpu, NULL, &n, e);
    bool found = false;
    for(uint32_t i = 0; i < n && !found; i++) {
        found = strcmp(e[i].extensionName, name) == 0;
    }
    free(e);
    return found;
}

/* A GPU that can draw what gpuic draws (Vulkan 1.3: dynamic rendering; dual-source blending for LCD text; push descriptors) and present to the
 * surface: a discrete one if there is one. */
static bool pick_gpu(vk_renderer* r, VkSurfaceKHR surface)
{
    uint32_t n = 0;
    r->fn.vkEnumeratePhysicalDevices(r->instance, &n, NULL);
    VkPhysicalDevice* gpus = calloc(n ? n : 1, sizeof *gpus);
    r->fn.vkEnumeratePhysicalDevices(r->instance, &n, gpus);
    int best = -1;
    for(uint32_t i = 0; i < n; i++) {
        VkPhysicalDeviceProperties props;
        r->fn.vkGetPhysicalDeviceProperties(gpus[i], &props);
        VkPhysicalDeviceFeatures f;
        r->fn.vkGetPhysicalDeviceFeatures(gpus[i], &f);
        VkPhysicalDeviceVulkan13Features f13 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES };
        VkPhysicalDeviceFeatures2 f2 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &f13 };
        if(props.apiVersion < VK_API_VERSION_1_3) {
            continue;
        }
        r->fn.vkGetPhysicalDeviceFeatures2(gpus[i], &f2);
        if(!f.dualSrcBlend || !f13.dynamicRendering || !f13.shaderDemoteToHelperInvocation || !has_extension(r, gpus[i], VK_KHR_SWAPCHAIN_EXTENSION_NAME) || !has_extension(r, gpus[i], VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME)) {
            continue;
        }
        uint32_t nq = 0;
        r->fn.vkGetPhysicalDeviceQueueFamilyProperties(gpus[i], &nq, NULL);
        VkQueueFamilyProperties* q = calloc(nq ? nq : 1, sizeof *q);
        r->fn.vkGetPhysicalDeviceQueueFamilyProperties(gpus[i], &nq, q);
        for(uint32_t k = 0; k < nq; k++) {
            VkBool32 present = VK_FALSE;
            r->fn.vkGetPhysicalDeviceSurfaceSupportKHR(gpus[i], k, surface, &present);
            if((q[k].queueFlags & VK_QUEUE_GRAPHICS_BIT) && present) {
                if(best < 0 || props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) {
                    best = (int)i;
                    r->family = k;
                }
                break;
            }
        }
        free(q);
    }
    if(best >= 0) {
        r->gpu = gpus[best];
    }
    free(gpus);
    return best >= 0;
}

static VkShaderModule shader(vk_renderer* r, const uint32_t* code, size_t bytes)
{
    VkShaderModuleCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = bytes, .pCode = code };
    VkShaderModule m = VK_NULL_HANDLE;
    r->fn.vkCreateShaderModule(r->device, &ci, NULL, &m);
    return m;
}

/* the one pipeline, blending (premultiplied over, with the second output saying what each channel of the destination keeps: LCD text) or not
 * (a filter pass, a layer with a blend mode: they write their result whole) */
static VkPipeline pipeline(vk_renderer* r, VkShaderModule vs, VkShaderModule fs, bool blend)
{
    VkPipelineShaderStageCreateInfo stages[2] = {
        { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,   .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = vs, .pName = "main" },
        { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = fs, .pName = "main" },
    };
    VkVertexInputBindingDescription binding = { 0, sizeof(gp_instance), VK_VERTEX_INPUT_RATE_INSTANCE };
    VkVertexInputAttributeDescription attrs[14]; /* gp_instance is fourteen consecutive vec4s */
    for(uint32_t i = 0; i < 14; i++) {
        attrs[i] = (VkVertexInputAttributeDescription){ i, 0, VK_FORMAT_R32G32B32A32_SFLOAT, i * 4 * (uint32_t)sizeof(float) };
    }
    VkPipelineVertexInputStateCreateInfo vin = { .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO, .vertexBindingDescriptionCount = 1, .pVertexBindingDescriptions = &binding, .vertexAttributeDescriptionCount = 14, .pVertexAttributeDescriptions = attrs };
    VkPipelineInputAssemblyStateCreateInfo ia = { .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO, .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP };
    VkPipelineViewportStateCreateInfo vp = { .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, .viewportCount = 1, .scissorCount = 1 };
    VkPipelineRasterizationStateCreateInfo rs = { .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO, .polygonMode = VK_POLYGON_MODE_FILL, .cullMode = VK_CULL_MODE_NONE, .lineWidth = 1.f };
    VkPipelineMultisampleStateCreateInfo ms = { .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO, .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT };
    VkPipelineColorBlendAttachmentState att = { .blendEnable = blend,
        .srcColorBlendFactor = VK_BLEND_FACTOR_ONE,
        .dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC1_COLOR,
        .colorBlendOp = VK_BLEND_OP_ADD,
        .srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE,
        .dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC1_ALPHA,
        .alphaBlendOp = VK_BLEND_OP_ADD,
        .colorWriteMask = 0xf };
    VkPipelineColorBlendStateCreateInfo cb = { .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, .attachmentCount = 1, .pAttachments = &att };
    VkDynamicState dyn[2] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo ds = { .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, .dynamicStateCount = 2, .pDynamicStates = dyn };
    VkPipelineRenderingCreateInfo rend = { .sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO, .colorAttachmentCount = 1, .pColorAttachmentFormats = &r->format };
    VkGraphicsPipelineCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .pNext = &rend,
        .stageCount = 2,
        .pStages = stages,
        .pVertexInputState = &vin,
        .pInputAssemblyState = &ia,
        .pViewportState = &vp,
        .pRasterizationState = &rs,
        .pMultisampleState = &ms,
        .pColorBlendState = &cb,
        .pDynamicState = &ds,
        .layout = r->layout };
    VkPipeline p = VK_NULL_HANDLE;
    r->fn.vkCreateGraphicsPipelines(r->device, VK_NULL_HANDLE, 1, &ci, NULL, &p);
    return p;
}

static VkSampler sampler(vk_renderer* r, VkFilter mag, VkFilter min, bool mips)
{
    VkSamplerCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
        .magFilter = mag,
        .minFilter = min,
        .mipmapMode = mips ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST,
        .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .maxLod = mips ? VK_LOD_CLAMP_NONE : 0.f };
    VkSampler s = VK_NULL_HANDLE;
    r->fn.vkCreateSampler(r->device, &ci, NULL, &s);
    return s;
}

/* B8G8R8A8 or R8G8B8A8, not sRGB (blending is in the encoded values, as GL's default framebuffer does it) */
static VkFormat surface_format(vk_renderer* r, VkSurfaceKHR surface)
{
    uint32_t n = 0;
    r->fn.vkGetPhysicalDeviceSurfaceFormatsKHR(r->gpu, surface, &n, NULL);
    VkSurfaceFormatKHR* f = calloc(n ? n : 1, sizeof *f);
    r->fn.vkGetPhysicalDeviceSurfaceFormatsKHR(r->gpu, surface, &n, f);
    VkFormat out = VK_FORMAT_UNDEFINED;
    for(uint32_t i = 0; i < n && out == VK_FORMAT_UNDEFINED; i++) {
        if((f[i].format == VK_FORMAT_B8G8R8A8_UNORM || f[i].format == VK_FORMAT_R8G8B8A8_UNORM) && f[i].colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
            out = f[i].format;
        }
    }
    free(f);
    return out;
}

static void vk_free(gp_renderer* base, gp_gfx* gfx);

gp_renderer* gp_vk_renderer_new(gp_app* app, gp_window* first)
{
    vk_renderer* r = calloc(1, sizeof *r);
    r->base.ops = &vk_ops;
    r->fn.vkGetInstanceProcAddr = (PFN_vkGetInstanceProcAddr)gp_platform_vk_loader();
    if(!r->fn.vkGetInstanceProcAddr) {
        fprintf(stderr, "gpuic: Vulkan: no loader\n");
        free(r);
        return NULL;
    }
    r->fn.vkCreateInstance = (PFN_vkCreateInstance)r->fn.vkGetInstanceProcAddr(NULL, "vkCreateInstance");
    uint32_t next = 0;
    const char* const* exts = gp_platform_vk_extensions(&next);
    const char* validation = "VK_LAYER_KHRONOS_validation";
    const char* v = getenv("GPUIC_VK_VALIDATE");
    bool validate = v && *v && strcmp(v, "0") != 0; /* (the layer reports to stderr by itself) */
    VkApplicationInfo ai = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .pApplicationName = "gpuic", .pEngineName = "gpuic", .apiVersion = VK_API_VERSION_1_3 };
    VkInstanceCreateInfo ici = { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &ai, .enabledExtensionCount = next, .ppEnabledExtensionNames = exts, .enabledLayerCount = validate ? 1 : 0, .ppEnabledLayerNames = &validation };
    VkResult made = r->fn.vkCreateInstance ? r->fn.vkCreateInstance(&ici, NULL, &r->instance) : VK_ERROR_INITIALIZATION_FAILED;
    if(made != VK_SUCCESS) {
        fprintf(stderr, "gpuic: Vulkan: no instance (Vulkan 1.3; error %d)\n", (int)made);
        free(r);
        return NULL;
    }
# define X(name) r->fn.name = (PFN_##name)r->fn.vkGetInstanceProcAddr(r->instance, #name);
    VK_INSTANCE_FUNCS(X)
# undef X
    VkSurfaceKHR surface = (VkSurfaceKHR)gp_platform_vk_surface(first, r->instance);
    if(!surface || !pick_gpu(r, surface)) {
        fprintf(stderr, "gpuic: Vulkan: no GPU that can draw here (Vulkan 1.3 with dual-source blending and push descriptors)\n");
        if(surface) {
            r->fn.vkDestroySurfaceKHR(r->instance, surface, NULL);
        }
        r->fn.vkDestroyInstance(r->instance, NULL);
        free(r);
        return NULL;
    }
    r->format = surface_format(r, surface);
    r->fn.vkDestroySurfaceKHR(r->instance, surface, NULL); /* (the window makes its own when it is first drawn) */
    r->fn.vkGetPhysicalDeviceMemoryProperties(r->gpu, &r->memprops);

    float prio = 1.f;
    VkDeviceQueueCreateInfo qci = { .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueFamilyIndex = r->family, .queueCount = 1, .pQueuePriorities = &prio };
    VkPhysicalDeviceVulkan13Features f13 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES, .dynamicRendering = VK_TRUE, .shaderDemoteToHelperInvocation = VK_TRUE }; /* (discard, in SPIR-V for 1.3) */
    VkPhysicalDeviceFeatures2 f2 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &f13, .features = { .dualSrcBlend = VK_TRUE } };
    const char* dexts[2] = { VK_KHR_SWAPCHAIN_EXTENSION_NAME, VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME };
    VkDeviceCreateInfo dci = { .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .pNext = &f2, .queueCreateInfoCount = 1, .pQueueCreateInfos = &qci, .enabledExtensionCount = 2, .ppEnabledExtensionNames = dexts };
    if(r->format == VK_FORMAT_UNDEFINED || r->fn.vkCreateDevice(r->gpu, &dci, NULL, &r->device) != VK_SUCCESS || !load_device_funcs(r, r->device)) {
        fprintf(stderr, "gpuic: Vulkan: no device (or no 8-bit RGBA surface format)\n");
        if(r->device) {
            r->fn.vkDestroyDevice(r->device, NULL);
        }
        r->fn.vkDestroyInstance(r->instance, NULL);
        free(r);
        return NULL;
    }
    r->fn.vkGetDeviceQueue(r->device, r->family, 0, &r->queue);

    VkCommandPoolCreateInfo pci = { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, .queueFamilyIndex = r->family };
    r->fn.vkCreateCommandPool(r->device, &pci, NULL, &r->pool);
    VkCommandBufferAllocateInfo cai = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = r->pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1 };
    r->fn.vkAllocateCommandBuffers(r->device, &cai, &r->cmd);
    VkFenceCreateInfo fci = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    r->fn.vkCreateFence(r->device, &fci, NULL, &r->fence);

    VkDescriptorSetLayoutBinding b[B_COUNT];
    for(uint32_t i = 0; i < B_COUNT; i++) {
        b[i] = (VkDescriptorSetLayoutBinding){ i, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL };
    }
    VkDescriptorSetLayoutCreateInfo dsl = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR, .bindingCount = B_COUNT, .pBindings = b };
    r->fn.vkCreateDescriptorSetLayout(r->device, &dsl, NULL, &r->set_layout);
    VkPushConstantRange pcr = { VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, 4 * sizeof(float) }; /* u_viewport, u_scale, u_gamma */
    VkPipelineLayoutCreateInfo plci = { .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1, .pSetLayouts = &r->set_layout, .pushConstantRangeCount = 1, .pPushConstantRanges = &pcr };
    r->fn.vkCreatePipelineLayout(r->device, &plci, NULL, &r->layout);
    VkShaderModule vs = shader(r, vs_spv, sizeof vs_spv), fs = shader(r, fs_spv, sizeof fs_spv);
    r->blended = pipeline(r, vs, fs, true);
    r->opaque = pipeline(r, vs, fs, false);
    r->fn.vkDestroyShaderModule(r->device, vs, NULL);
    r->fn.vkDestroyShaderModule(r->device, fs, NULL);
    r->nearest = sampler(r, VK_FILTER_NEAREST, VK_FILTER_NEAREST, false);
    r->linear = sampler(r, VK_FILTER_LINEAR, VK_FILTER_LINEAR, false);
    r->mipmapped = sampler(r, VK_FILTER_LINEAR, VK_FILTER_LINEAR, true);
    r->mip_nearest = sampler(r, VK_FILTER_NEAREST, VK_FILTER_LINEAR, true); /* (a pixelated image: magnified without smoothing) */

    VkImageUsageFlags tex = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    bool ok = r->blended && r->opaque && r->nearest && r->linear && r->mipmapped && r->mip_nearest;
    ok = ok && image_make(r, &r->atlas, GP_ATLAS_SIZE, GP_ATLAS_SIZE, VK_FORMAT_R8_UNORM, 1, tex);
    ok = ok && image_make(r, &r->color_atlas, GP_COLOR_ATLAS_SIZE, GP_COLOR_ATLAS_SIZE, VK_FORMAT_R8G8B8A8_UNORM, 1, tex);
    ok = ok && image_make(r, &r->lcd_atlas, GP_LCD_ATLAS_SIZE, GP_LCD_ATLAS_SIZE, VK_FORMAT_R8G8B8A8_UNORM, 1, tex);
    ok = ok && image_make(r, &r->ramps, GP_GRAD_W, GP_GRAD_ROWS, VK_FORMAT_R8G8B8A8_UNORM, 1, tex);
    ok = ok && image_make(r, &r->dummy, 1, 1, VK_FORMAT_R8G8B8A8_UNORM, 1, tex);
    if(!ok) {
        fprintf(stderr, "gpuic: Vulkan: could not make the pipelines or textures\n");
        vk_free(&r->base, NULL);
        return NULL;
    }
    r->base.gpu = (gp_gpu){ .backend = GP_GPU_VULKAN, .vk_get_instance_proc_addr = (void (*(*)(void*, const char*))(void))(void (*)(void))r->fn.vkGetInstanceProcAddr, .vk_instance = r->instance, .vk_physical_device = r->gpu, .vk_device = r->device, .vk_queue = r->queue, .vk_queue_family = r->family };
    return &r->base;
}

/* ---------------------------------------------------------------- windows */

static void swapchain_free(vk_renderer* r, vk_window* vw)
{
    for(uint32_t i = 0; i < vw->nimages; i++) {
        r->fn.vkDestroySemaphore(r->device, vw->done[i], NULL);
    }
    free(vw->images);
    free(vw->done);
    vw->images = NULL;
    vw->done = NULL;
    vw->nimages = 0;
    if(vw->swapchain) {
        r->fn.vkDestroySwapchainKHR(r->device, vw->swapchain, NULL);
        vw->swapchain = VK_NULL_HANDLE;
    }
    image_free(r, &vw->main);
}

/* the swapchain for the window's size now (and the image frames are drawn into, as big): false if there is nothing to draw on (a window of no size) */
static bool swapchain_make(vk_renderer* r, vk_window* vw)
{
    VkSurfaceCapabilitiesKHR caps;
    r->fn.vkGetPhysicalDeviceSurfaceCapabilitiesKHR(r->gpu, vw->surface, &caps);
    VkExtent2D ext = caps.currentExtent;
    if(ext.width == UINT32_MAX) { /* (the surface takes its size from the swapchain: Wayland) */
        ext.width = (uint32_t)vw->w->px_w;
        ext.height = (uint32_t)vw->w->px_h;
    }
    if(ext.width == 0 || ext.height == 0) { /* (minimized: nothing to draw on; the old swapchain goes, so trying again waits for nothing) */
        swapchain_free(r, vw);
        return false;
    }
    VkSwapchainKHR old = vw->swapchain;
    uint32_t nmodes = 0;
    r->fn.vkGetPhysicalDeviceSurfacePresentModesKHR(r->gpu, vw->surface, &nmodes, NULL);
    VkPresentModeKHR modes[16];
    nmodes = nmodes > 16 ? 16 : nmodes;
    r->fn.vkGetPhysicalDeviceSurfacePresentModesKHR(r->gpu, vw->surface, &nmodes, modes);
    VkPresentModeKHR mode = VK_PRESENT_MODE_FIFO_KHR; /* mailbox where there is one: a window the compositor is not showing is not held to its
                                                       * frame callbacks (FIFO waited ~50 ms an image on KDE), and the app paces its frames itself */
    for(uint32_t i = 0; i < nmodes; i++) {
        if(modes[i] == VK_PRESENT_MODE_MAILBOX_KHR) {
            mode = VK_PRESENT_MODE_MAILBOX_KHR;
        }
    }
    vw->swapchain = VK_NULL_HANDLE;
    uint32_t count = caps.minImageCount + 1;
    if(caps.maxImageCount && count > caps.maxImageCount) {
        count = caps.maxImageCount;
    }
    VkSwapchainCreateInfoKHR ci = { .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
        .surface = vw->surface,
        .minImageCount = count,
        .imageFormat = r->format,
        .imageColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR,
        .imageExtent = ext,
        .imageArrayLayers = 1,
        .imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT,
        .imageSharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .preTransform = caps.currentTransform,
        .compositeAlpha = (caps.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR) ? VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR : (VkCompositeAlphaFlagBitsKHR)caps.supportedCompositeAlpha,
        .presentMode = mode,
        .clipped = VK_TRUE,
        .oldSwapchain = old };
    VkResult res = r->fn.vkCreateSwapchainKHR(r->device, &ci, NULL, &vw->swapchain);
    VkSwapchainKHR made = vw->swapchain;
    vw->swapchain = old;
    swapchain_free(r, vw); /* (the old one, and what went with it) */
    if(res != VK_SUCCESS) {
        return false;
    }
    vw->swapchain = made;
    r->fn.vkGetSwapchainImagesKHR(r->device, vw->swapchain, &vw->nimages, NULL);
    vw->images = calloc(vw->nimages, sizeof *vw->images);
    vw->done = calloc(vw->nimages, sizeof *vw->done);
    r->fn.vkGetSwapchainImagesKHR(r->device, vw->swapchain, &vw->nimages, vw->images);
    VkSemaphoreCreateInfo sci = { .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
    for(uint32_t i = 0; i < vw->nimages; i++) {
        r->fn.vkCreateSemaphore(r->device, &sci, NULL, &vw->done[i]);
    }
    vw->sw = (int)ext.width;
    vw->sh = (int)ext.height;
    if(!image_make(r, &vw->main, vw->sw, vw->sh, r->format, 1, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT)) {
        return false; /* (not up to date: tried again at the next draw) */
    }
    vw->want_w = vw->w->px_w;
    vw->want_h = vw->w->px_h;
    return true;
}

static vk_window* window_state(vk_renderer* r, gp_window* w)
{
    if(w->rdata) {
        return w->rdata;
    }
    vk_window* vw = calloc(1, sizeof *vw);
    vw->w = w;
    vw->pending = -1;
    vw->surface = (VkSurfaceKHR)gp_platform_vk_surface(w, r->instance);
    VkBool32 present = VK_FALSE;
    if(vw->surface) {
        r->fn.vkGetPhysicalDeviceSurfaceSupportKHR(r->gpu, r->family, vw->surface, &present);
    }
    if(!present && vw->surface) {
        fprintf(stderr, "gpuic: Vulkan: the GPU cannot present to this window: it stays blank\n");
        r->fn.vkDestroySurfaceKHR(r->instance, vw->surface, NULL);
        vw->surface = VK_NULL_HANDLE;
    }
    VkSemaphoreCreateInfo sci = { .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
    r->fn.vkCreateSemaphore(r->device, &sci, NULL, &vw->acquired);
    vw->next = r->windows;
    r->windows = vw;
    w->rdata = vw;
    return vw;
}

static void window_free(vk_renderer* r, vk_window* vw)
{
    swapchain_free(r, vw);
    r->fn.vkDestroySemaphore(r->device, vw->acquired, NULL);
    if(vw->surface) {
        r->fn.vkDestroySurfaceKHR(r->instance, vw->surface, NULL);
    }
    vw->w->rdata = NULL;
    free(vw);
}

static void wait_frame(vk_renderer* r)
{
    if(r->submitted) {
        r->fn.vkWaitForFences(r->device, 1, &r->fence, VK_TRUE, UINT64_MAX);
        r->fn.vkResetFences(r->device, 1, &r->fence);
        r->submitted = false;
    }
}

static void vk_window_gone(gp_renderer* base, gp_window* w)
{
    vk_renderer* r = (vk_renderer*)base;
    vk_window* vw = w->rdata;
    if(!vw) {
        return;
    }
    r->fn.vkDeviceWaitIdle(r->device);
    r->submitted = false;
    r->fn.vkResetFences(r->device, 1, &r->fence);
    for(vk_window** p = &r->windows; *p; p = &(*p)->next) {
        if(*p == vw) {
            *p = vw->next;
            break;
        }
    }
    window_free(r, vw);
}

static void vk_free(gp_renderer* base, gp_gfx* gfx)
{
    vk_renderer* r = (vk_renderer*)base;
    if(r->device) {
        r->fn.vkDeviceWaitIdle(r->device);
        gp_custom_teardown(base->app, base);
        while(r->windows) {
            vk_window* vw = r->windows;
            r->windows = vw->next;
            window_free(r, vw);
        }
        if(gfx) { /* (the images upload again if a new renderer draws them) */
            uint32_t ids[64];
            size_t n;
            while((n = gp_gfx_take_all_textures(gfx, ids, 64)) > 0 || (n = gp_gfx_take_dead_textures(gfx, ids, 64)) > 0) {
            }
        }
        for(uint32_t i = 0; i < r->ntextures; i++) {
            image_free(r, &r->textures[i]);
        }
        free(r->textures);
        for(int i = 0; i < T_COUNT; i++) {
            image_free(r, &r->targets[i]);
        }
        image_free(r, &r->atlas);
        image_free(r, &r->color_atlas);
        image_free(r, &r->lcd_atlas);
        image_free(r, &r->ramps);
        image_free(r, &r->dummy);
        buffer_free(r, &r->instances);
        buffer_free(r, &r->staging);
        buffer_free(r, &r->readback);
        VkSampler samplers[4] = { r->nearest, r->linear, r->mipmapped, r->mip_nearest };
        for(int i = 0; i < 4; i++) {
            if(samplers[i]) {
                r->fn.vkDestroySampler(r->device, samplers[i], NULL);
            }
        }
        if(r->blended) {
            r->fn.vkDestroyPipeline(r->device, r->blended, NULL);
        }
        if(r->opaque) {
            r->fn.vkDestroyPipeline(r->device, r->opaque, NULL);
        }
        r->fn.vkDestroyPipelineLayout(r->device, r->layout, NULL);
        r->fn.vkDestroyDescriptorSetLayout(r->device, r->set_layout, NULL);
        r->fn.vkDestroyFence(r->device, r->fence, NULL);
        r->fn.vkDestroyCommandPool(r->device, r->pool, NULL);
        r->fn.vkDestroyDevice(r->device, NULL);
    }
    r->fn.vkDestroyInstance(r->instance, NULL);
    free(r);
}

/* ---------------------------------------------------------------- the walk */

/* a target the window's size (d: a layer for depth d, or T_*): made when first needed, all of them again when the size changes */
static vk_image* target(vk_renderer* r, int d, int w, int h)
{
    if(r->target_w != w || r->target_h != h) {
        for(int i = 0; i < T_COUNT; i++) {
            image_free(r, &r->targets[i]);
        }
        r->target_w = w;
        r->target_h = h;
    }
    vk_image* t = &r->targets[d];
    if(!t->image && !image_make(r, t, w, h, r->format, 1, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT)) {
        static bool warned;
        if(!warned) {
            warned = true;
            fprintf(stderr, "gpuic: an offscreen layer could not be made: layers are drawn straight onto the window\n");
        }
        return NULL;
    }
    if(t->fresh) { /* (outside rendering: its layout changes) */
        bool was = r->rendering;
        if(was) {
            r->fn.vkCmdEndRendering(r->cmd);
            r->rendering = false;
        }
        image_ready(r, t);
    }
    return t;
}

static void end_rendering(vk_renderer* r)
{
    if(r->rendering) {
        r->fn.vkCmdEndRendering(r->cmd);
        r->rendering = false;
        full_barrier(r); /* (what was drawn may be read next) */
    }
}

/* draw into `t` from now on */
static void set_target(vk_renderer* r, vk_image* t)
{
    if(r->target != t) {
        end_rendering(r);
        r->target = t;
    }
}

static void tex_bind(vk_renderer* r, int b, vk_image* im, VkSampler s)
{
    r->bind_view[b] = im ? im->view : r->dummy.view;
    r->bind_sampler[b] = s ? s : r->nearest;
}

static void unbind(vk_renderer* r)
{
    for(int b = B_IMAGE; b < B_COUNT; b++) {
        tex_bind(r, b, NULL, NULL);
    }
}

/* rendering into the target, if it is not on already */
static void begin_rendering(vk_renderer* r)
{
    if(r->rendering || !r->target) {
        return;
    }
    VkRenderingAttachmentInfo att = { .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, .imageView = r->target->view, .imageLayout = VK_IMAGE_LAYOUT_GENERAL, .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD, .storeOp = VK_ATTACHMENT_STORE_OP_STORE };
    VkRenderingInfo ri = {
        .sType = VK_STRUCTURE_TYPE_RENDERING_INFO, .renderArea = { { 0, 0 }, { (uint32_t)r->target->w, (uint32_t)r->target->h } },
             .layerCount = 1, .colorAttachmentCount = 1, .pColorAttachments = &att
    };
    r->fn.vkCmdBeginRendering(r->cmd, &ri);
    r->rendering = true;
    VkViewport vp = { 0, 0, (float)r->target->w, (float)r->target->h, 0, 1 };
    VkRect2D sc = {
        {                      0,                      0 },
        { (uint32_t)r->target->w, (uint32_t)r->target->h }
    };
    r->fn.vkCmdSetViewport(r->cmd, 0, 1, &vp);
    r->fn.vkCmdSetScissor(r->cmd, 0, 1, &sc);
    r->bound = VK_NULL_HANDLE;
}

/* instances [first, first + count) in one instanced draw, into the target, reading what is bound */
static void draw_run(vk_renderer* r, size_t first, size_t count, VkPipeline p)
{
    if(!count || !r->target) {
        return;
    }
    begin_rendering(r);
    if(r->bound != p) {
        r->fn.vkCmdBindPipeline(r->cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, p);
        VkDeviceSize zero = 0;
        r->fn.vkCmdBindVertexBuffers(r->cmd, 0, 1, &r->instances.buf, &zero);
        r->fn.vkCmdPushConstants(r->cmd, r->layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof r->push, r->push);
        r->bound = p;
    }
    VkDescriptorImageInfo info[B_COUNT];
    VkWriteDescriptorSet wr[B_COUNT];
    for(uint32_t b = 0; b < B_COUNT; b++) {
        info[b] = (VkDescriptorImageInfo){ r->bind_sampler[b], r->bind_view[b], VK_IMAGE_LAYOUT_GENERAL };
        wr[b] = (VkWriteDescriptorSet){ .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = b, .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &info[b] };
    }
    r->fn.vkCmdPushDescriptorSetKHR(r->cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, r->layout, 0, B_COUNT, wr);
    r->fn.vkCmdDraw(r->cmd, 4, (uint32_t)count, 0, (uint32_t)first);
}

/* an instance's region (bounds) in device pixels, cut to the target */
static VkRect2D region(const gp_instance* in, float scale, int w, int h)
{
    int x0 = (int)floorf(in->bounds[0] * scale + 0.5f), x1 = (int)floorf((in->bounds[0] + in->bounds[2]) * scale + 0.5f);
    int y0 = (int)floorf(in->bounds[1] * scale + 0.5f), y1 = (int)floorf((in->bounds[1] + in->bounds[3]) * scale + 0.5f);
    x0 = x0 < 0 ? 0 : x0;
    y0 = y0 < 0 ? 0 : y0;
    x1 = x1 > w ? w : x1;
    y1 = y1 > h ? h : y1;
    if(x1 <= x0 || y1 <= y0) {
        return (VkRect2D){
            { 0, 0 },
            { 0, 0 }
        };
    }
    return (VkRect2D){
        {                  x0,                  y0 },
        { (uint32_t)(x1 - x0), (uint32_t)(y1 - y0) }
    };
}

/* what the target being drawn on holds over the region, into the backdrop target at the same place */
static void backdrop_copy(vk_renderer* r, vk_image* from, vk_image* to, VkRect2D rc)
{
    if(!rc.extent.width || !from) {
        return;
    }
    end_rendering(r);
    VkImageCopy c = {
        .srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
        .srcOffset = { rc.offset.x, rc.offset.y, 0 },
        .dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
        .dstOffset = { rc.offset.x, rc.offset.y, 0 },
        .extent = { rc.extent.width, rc.extent.height, 1 }
    };
    r->fn.vkCmdCopyImage(r->cmd, from->image, VK_IMAGE_LAYOUT_GENERAL, to->image, VK_IMAGE_LAYOUT_GENERAL, 1, &c);
    full_barrier(r);
}

/* an image's texture, made (with its mipmaps) the first time it is drawn, before the walk */
static vk_image* image_texture(vk_renderer* r, gp_gfx_image* im)
{
    if(im->tex) {
        return &r->textures[im->tex - 1];
    }
    uint32_t slot = 0;
    while(slot < r->ntextures && r->textures[slot].image) {
        slot++;
    }
    if(slot == r->ntextures) {
        if(r->ntextures == r->cap_textures) {
            r->cap_textures = r->cap_textures ? r->cap_textures * 2 : 16;
            r->textures = realloc(r->textures, r->cap_textures * sizeof *r->textures);
            if(!r->textures) {
                abort();
            }
        }
        memset(&r->textures[r->ntextures++], 0, sizeof *r->textures);
    }
    uint32_t mips = 1;
    while((im->w >> mips) > 0 || (im->h >> mips) > 0) {
        mips++;
    }
    vk_image* t = &r->textures[slot];
    if(!image_make(r, t, im->w, im->h, VK_FORMAT_R8G8B8A8_UNORM, mips, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT)) {
        return NULL;
    }
    image_ready(r, t);
    upload(r, t, 0, 0, im->w, im->h, im->pixels, (size_t)im->w * 4, 4);
    for(uint32_t m = 1; m < mips; m++) { /* each level from the one above it */
        full_barrier(r);
        int sw = im->w >> (m - 1), sh = im->h >> (m - 1);
        int dw = im->w >> m, dh = im->h >> m;
        VkImageBlit b = {
            .srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, m - 1, 0, 1 },
            .srcOffsets = { { 0, 0, 0 }, { sw > 0 ? sw : 1, sh > 0 ? sh : 1, 1 } },
            .dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, m, 0, 1 },
            .dstOffsets = { { 0, 0, 0 }, { dw > 0 ? dw : 1, dh > 0 ? dh : 1, 1 } }
        };
        r->fn.vkCmdBlitImage(r->cmd, t->image, VK_IMAGE_LAYOUT_GENERAL, t->image, VK_IMAGE_LAYOUT_GENERAL, 1, &b, VK_FILTER_LINEAR);
    }
    im->tex = slot + 1;
    return t;
}

/* Puts the frame's uploads in: glyphs rasterized since the last frame, ramps, the instances, and the textures of images drawn for the first time. */
static void uploads(vk_renderer* r, gp_text_system* ts, const gp_scene* scene)
{
    image_ready(r, &r->atlas);
    image_ready(r, &r->color_atlas);
    image_ready(r, &r->lcd_atlas);
    image_ready(r, &r->ramps);
    image_ready(r, &r->dummy);
    int dx, dy, dw, dh;
    if(gp_text_atlas_take_dirty(ts, &dx, &dy, &dw, &dh)) {
        upload(r, &r->atlas, dx, dy, dw, dh, gp_text_atlas(ts) + (size_t)dy * GP_ATLAS_SIZE + (size_t)dx, GP_ATLAS_SIZE, 1);
    }
    if(gp_text_color_atlas_take_dirty(ts, &dx, &dy, &dw, &dh)) {
        upload(r, &r->color_atlas, dx, dy, dw, dh, gp_text_color_atlas(ts) + ((size_t)dy * GP_COLOR_ATLAS_SIZE + (size_t)dx) * 4, (size_t)GP_COLOR_ATLAS_SIZE * 4, 4);
    }
    if(gp_text_lcd_atlas_take_dirty(ts, &dx, &dy, &dw, &dh)) {
        upload(r, &r->lcd_atlas, dx, dy, dw, dh, gp_text_lcd_atlas(ts) + ((size_t)dy * GP_LCD_ATLAS_SIZE + (size_t)dx) * 4, (size_t)GP_LCD_ATLAS_SIZE * 4, 4);
    }
    gp_gfx* gfx = gp_text_gfx(ts);
    int row0, row1;
    if(gfx && gp_gfx_gradient_take_dirty(gfx, &row0, &row1)) {
        upload(r, &r->ramps, 0, row0, GP_GRAD_W, row1 - row0, gp_gfx_gradient_data(gfx) + (size_t)row0 * GP_GRAD_W * 4, (size_t)GP_GRAD_W * 4, 4);
    }
    for(size_t i = 0; gfx && i < scene->len; i++) {
        const gp_instance* in = &scene->items[i];
        gp_gfx_image* im = (int)in->kind == GP_PRIM_IMAGE ? gp_gfx_image_get(gfx, (uint32_t)in->_pad, (uint32_t)in->border_color[0]) : NULL;
        if(im && !im->tex) {
            image_texture(r, im);
        }
    }
    full_barrier(r);
}

static void vk_draw(gp_renderer* base, gp_window* w, gp_text_system* ts, const gp_scene* scene, gp_color clear)
{
    vk_renderer* r = (vk_renderer*)base;
    vk_window* vw = window_state(r, w);
    wait_frame(r);
    vw->pending = -1;
    gp_custom_begin(w->app, base); /* (released ones are destroyed: the last frame is done) */
    {                              /* textures of images that were released (nothing draws with them now: the last frame is done) */
        gp_gfx* g = gp_text_gfx(ts);
        uint32_t dead[32];
        size_t n;
        while(g && (n = gp_gfx_take_dead_textures(g, dead, 32)) > 0) {
            for(size_t i = 0; i < n; i++) {
                if(dead[i] && dead[i] <= r->ntextures) {
                    image_free(r, &r->textures[dead[i] - 1]);
                }
            }
        }
    }
    if(!vw->surface) {
        return;
    }
    if(!vw->swapchain || vw->want_w != w->px_w || vw->want_h != w->px_h || !vw->main.image) {
        if(vw->swapchain) {
            r->fn.vkDeviceWaitIdle(r->device);
        }
        if(!swapchain_make(r, vw)) {
            return;
        }
    }
    /* the staging buffer, big enough for every atlas whole and the images drawn for the first time (made now: nothing uses it) */
    VkDeviceSize stage_need = (VkDeviceSize)GP_ATLAS_SIZE * GP_ATLAS_SIZE + 4u * ((VkDeviceSize)GP_COLOR_ATLAS_SIZE * GP_COLOR_ATLAS_SIZE + (VkDeviceSize)GP_LCD_ATLAS_SIZE * GP_LCD_ATLAS_SIZE + (VkDeviceSize)GP_GRAD_W * GP_GRAD_ROWS) + 64;
    gp_gfx* gfx = gp_text_gfx(ts);
    for(size_t i = 0; gfx && i < scene->len; i++) {
        const gp_instance* in = &scene->items[i];
        gp_gfx_image* im = (int)in->kind == GP_PRIM_IMAGE ? gp_gfx_image_get(gfx, (uint32_t)in->_pad, (uint32_t)in->border_color[0]) : NULL;
        if(im && !im->tex) {
            stage_need += (VkDeviceSize)im->w * (VkDeviceSize)im->h * 4 + 16;
        }
    }
    size_t bytes = scene->len * sizeof(gp_instance);
    if(!buffer_reserve(r, &r->staging, stage_need, VK_BUFFER_USAGE_TRANSFER_SRC_BIT) || (bytes && !buffer_reserve(r, &r->instances, bytes, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT))) {
        return;
    }
    uint32_t index = 0;
    VkResult res = r->fn.vkAcquireNextImageKHR(r->device, vw->swapchain, UINT64_MAX, vw->acquired, VK_NULL_HANDLE, &index);
    if(res == VK_ERROR_OUT_OF_DATE_KHR) {
        r->fn.vkDeviceWaitIdle(r->device);
        if(!swapchain_make(r, vw)) {
            return;
        }
        res = r->fn.vkAcquireNextImageKHR(r->device, vw->swapchain, UINT64_MAX, vw->acquired, VK_NULL_HANDLE, &index);
    }
    if(res != VK_SUCCESS && res != VK_SUBOPTIMAL_KHR) {
        return;
    }

    if(bytes) {
        memcpy(r->instances.map, scene->items, bytes);
    }
    VkCommandBufferBeginInfo bi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
    r->fn.vkResetCommandBuffer(r->cmd, 0);
    r->fn.vkBeginCommandBuffer(r->cmd, &bi);
    r->staged = 0;
    image_ready(r, &vw->main);
    uploads(r, ts, scene);
    for(size_t i = 0; i < scene->len; i++) { /* custom drawing, recorded by the app outside rendering, before the walk reads it */
        if(gp_inst_custom(&scene->items[i])) {
            gp_custom_texture(w->app, base, &scene->items[i], w->scale, r->cmd);
        }
    }
    VkClearColorValue cc = {
        { clear.r, clear.g, clear.b, clear.a }
    };
    VkImageSubresourceRange all = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    r->fn.vkCmdClearColorImage(r->cmd, vw->main.image, VK_IMAGE_LAYOUT_GENERAL, &cc, 1, &all);
    full_barrier(r);

    int px_w = vw->sw, px_h = vw->sh;
    float scale = w->scale;
    r->push[0] = (float)px_w / scale;
    r->push[1] = (float)px_h / scale;
    r->push[2] = scale;
    r->push[3] = gp_text_gamma(ts);
    r->target = NULL;
    r->rendering = false;
    tex_bind(r, B_ATLAS, &r->atlas, r->nearest);
    tex_bind(r, B_COLOR, &r->color_atlas, r->nearest);
    tex_bind(r, B_LCD, &r->lcd_atlas, r->nearest);
    tex_bind(r, B_RAMPS, &r->ramps, r->linear);
    unbind(r);
    set_target(r, &vw->main);

    /* The walk of render_gl.c's gl_draw, step for step: where it binds a framebuffer, this sets the target; where it binds a texture, this binds it. */
    size_t run = 0;
    int depth = 0, skipped = 0, cur = -1;
    vk_image* mask = NULL;
    bool backdrop = false;
# define PARENT (depth ? &r->targets[depth - 1] : &vw->main)
    for(size_t i = 0; bytes && i < scene->len; i++) {
        const gp_instance* in = &scene->items[i];
        int kind = (int)in->kind;
        bool special = kind == GP_PRIM_BACKDROP || kind == GP_PRIM_BACKDROP_END || kind == GP_PRIM_FX_MATRIX || kind == GP_PRIM_FX_BLUR || kind == GP_PRIM_FX_SHADOW || kind == GP_PRIM_MASK_END || kind == GP_PRIM_LAYER_BEGIN || kind == GP_PRIM_LAYER_END || kind == GP_PRIM_IMAGE;
        if(!special) {
            continue;
        }
        if(i > run) {
            draw_run(r, run, i - run, r->blended);
        }
        run = i + 1;
        if(kind == GP_PRIM_BACKDROP) { /* what is under the region, out of the target being drawn on, into the backdrop target */
            vk_image* t = target(r, T_BACKDROP, px_w, px_h);
            if(!t) {
                continue;
            }
            backdrop_copy(r, r->target, t, region(in, scale, px_w, px_h));
            cur = T_BACKDROP;
            backdrop = true;
        } else if(kind == GP_PRIM_BACKDROP_END) { /* back on the target, the result drawn in the box */
            if(!backdrop) {
                continue;
            }
            backdrop = false;
            set_target(r, PARENT);
            tex_bind(r, B_LAYER, &r->targets[cur], NULL);
            draw_run(r, i, 1, r->blended);
            unbind(r);
            cur = -1;
        } else if(kind == GP_PRIM_FX_MATRIX || kind == GP_PRIM_FX_BLUR || kind == GP_PRIM_FX_SHADOW) { /* a pass over the open layer */
            if(!backdrop && (skipped || !depth)) {
                continue;
            }
            int src = cur >= 0 ? cur : depth - 1;
            int dst = kind == GP_PRIM_FX_BLUR && in->side[0][1] > 0.5f ? T_AUX : (cur == T_FX0 ? T_FX1 : T_FX0);
            vk_image* t = target(r, dst, px_w, px_h);
            if(!t) {
                continue;
            }
            set_target(r, t);
            tex_bind(r, B_LAYER, &r->targets[src], NULL);
            tex_bind(r, B_AUX, kind == GP_PRIM_FX_SHADOW ? &r->targets[T_AUX] : NULL, NULL);
            draw_run(r, i, 1, r->opaque); /* (a pass writes its result whole) */
            unbind(r);
            if(dst != T_AUX) {
                cur = dst;
            }
        } else if(kind == GP_PRIM_MASK_END) { /* the mask's layer is done: kept for the END that follows */
            if(skipped) {
                skipped--;
                continue;
            }
            if(!depth) {
                continue;
            }
            depth--;
            mask = &r->targets[depth];
            set_target(r, PARENT);
        } else if(kind == GP_PRIM_LAYER_BEGIN) {
            vk_image* t = skipped || depth >= GP_MAX_LAYERS ? NULL : target(r, depth, px_w, px_h);
            if(!t) {
                skipped++;
                continue;
            }
            set_target(r, t);
            VkRect2D rc = region(in, scale, px_w, px_h);
            if(rc.extent.width) { /* only the region is cleared */
                begin_rendering(r);
                VkClearAttachment ca = { .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .colorAttachment = 0 };
                VkClearRect cr = { rc, 0, 1 };
                r->fn.vkCmdClearAttachments(r->cmd, 1, &ca, 1, &cr);
            }
            depth++;
        } else if(kind == GP_PRIM_LAYER_END) {
            if(skipped) {
                skipped--;
                continue;
            }
            if(!depth) {
                continue;
            }
            depth--;
            vk_image* parent = PARENT;
            set_target(r, parent);
            tex_bind(r, B_LAYER, &r->targets[cur >= 0 ? cur : depth], NULL);
            tex_bind(r, B_AUX, in->uv[1] > 0.5f ? mask : NULL, NULL);
            bool blended = in->uv[0] > 0.5f;
            vk_image* bt = blended ? target(r, T_BACKDROP, px_w, px_h) : NULL;
            if(bt) { /* what is under it, over the region, into the backdrop target: the quad computes the blend and writes it whole */
                backdrop_copy(r, parent, bt, region(in, scale, px_w, px_h));
                tex_bind(r, B_BACKDROP, bt, NULL);
            }
            draw_run(r, i, 1, bt ? r->opaque : r->blended);
            unbind(r);
            cur = -1;
            mask = NULL;
        } else if(gp_inst_custom(in)) { /* the view the app drew (above) */
            uint64_t view = gp_custom_texture(w->app, base, in, scale, r->cmd);
            if(!view) {
                continue;
            }
            r->bind_view[B_IMAGE] = (VkImageView)view;
            r->bind_sampler[B_IMAGE] = r->linear;
            draw_run(r, i, 1, r->blended);
            unbind(r);
        } else {          /* an image: its texture */
            gp_gfx_image* im = gp_gfx_image_get(gfx, (uint32_t)in->_pad, (uint32_t)in->border_color[0]);
            if(!im || !im->tex) {
                continue; /* (released since it was painted, or not made: nothing to draw) */
            }
            tex_bind(r, B_IMAGE, &r->textures[im->tex - 1], in->blur >= 500.f ? r->mip_nearest : r->mipmapped);
            draw_run(r, i, 1, r->blended);
            unbind(r);
        }
    }
# undef PARENT
    if(bytes && scene->len > run) {
        draw_run(r, run, scene->len - run, r->blended);
    }
    end_rendering(r);
    r->target = NULL;

    /* the frame onto the swapchain's image */
    VkImage sc = vw->images[index];
    layout_change(r, sc, 1, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkImageCopy c = {
        .srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
        .dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
        .extent = { (uint32_t)px_w, (uint32_t)px_h, 1 }
    };
    r->fn.vkCmdCopyImage(r->cmd, vw->main.image, VK_IMAGE_LAYOUT_GENERAL, sc, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
    layout_change(r, sc, 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
    r->fn.vkEndCommandBuffer(r->cmd);
    VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .waitSemaphoreCount = 1, .pWaitSemaphores = &vw->acquired, .pWaitDstStageMask = &wait_stage, .commandBufferCount = 1, .pCommandBuffers = &r->cmd, .signalSemaphoreCount = 1, .pSignalSemaphores = &vw->done[index] };
    if(r->fn.vkQueueSubmit(r->queue, 1, &si, r->fence) == VK_SUCCESS) {
        r->submitted = true;
        vw->pending = (int)index;
    } else { /* (the acquire's semaphore stays signalled, and acquiring may not signal it again: another one) */
        r->fn.vkDeviceWaitIdle(r->device);
        r->fn.vkDestroySemaphore(r->device, vw->acquired, NULL);
        VkSemaphoreCreateInfo sci = { .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
        r->fn.vkCreateSemaphore(r->device, &sci, NULL, &vw->acquired);
    }
}

static void vk_present(gp_renderer* base, gp_window* w)
{
    vk_renderer* r = (vk_renderer*)base;
    vk_window* vw = w->rdata;
    if(!vw || vw->pending < 0) {
        return;
    }
    uint32_t index = (uint32_t)vw->pending;
    vw->pending = -1;
    VkPresentInfoKHR pi = { .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR, .waitSemaphoreCount = 1, .pWaitSemaphores = &vw->done[index], .swapchainCount = 1, .pSwapchains = &vw->swapchain, .pImageIndices = &index };
    VkResult res = r->fn.vkQueuePresentKHR(r->queue, &pi);
    if(res == VK_ERROR_OUT_OF_DATE_KHR || res == VK_SUBOPTIMAL_KHR) {
        vw->want_w = 0; /* (made again at the next draw) */
    }
}

static uint8_t* vk_read_pixels(gp_renderer* base, gp_window* w)
{
    vk_renderer* r = (vk_renderer*)base;
    vk_window* vw = w->rdata;
    int pw = w->px_w, ph = w->px_h;
    uint8_t* out = calloc((size_t)pw * (size_t)ph * 3, 1);
    if(!vw || !vw->main.image) {
        return out;
    }
    wait_frame(r);
    int cw = vw->main.w < pw ? vw->main.w : pw, ch = vw->main.h < ph ? vw->main.h : ph;
    if(!buffer_reserve(r, &r->readback, (VkDeviceSize)vw->main.w * (VkDeviceSize)vw->main.h * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT)) {
        return out;
    }
    VkCommandBufferBeginInfo bi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
    r->fn.vkResetCommandBuffer(r->cmd, 0);
    r->fn.vkBeginCommandBuffer(r->cmd, &bi);
    VkBufferImageCopy c = {
        .bufferOffset = 0, .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
             .imageExtent = { (uint32_t)vw->main.w, (uint32_t)vw->main.h, 1 }
    };
    r->fn.vkCmdCopyImageToBuffer(r->cmd, vw->main.image, VK_IMAGE_LAYOUT_GENERAL, r->readback.buf, 1, &c);
    full_barrier(r);
    r->fn.vkEndCommandBuffer(r->cmd);
    VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &r->cmd };
    r->fn.vkQueueSubmit(r->queue, 1, &si, r->fence);
    r->submitted = true;
    wait_frame(r);
    const uint8_t* src = r->readback.map;
    bool bgra = r->format == VK_FORMAT_B8G8R8A8_UNORM;
    for(int y = 0; y < ch; y++) {
        for(int x = 0; x < cw; x++) {
            const uint8_t* p = src + ((size_t)y * (size_t)vw->main.w + (size_t)x) * 4;
            uint8_t* o = out + ((size_t)y * (size_t)pw + (size_t)x) * 3;
            o[0] = bgra ? p[2] : p[0];
            o[1] = p[1];
            o[2] = bgra ? p[0] : p[2];
        }
    }
    return out;
}

static const gp_renderer_ops vk_ops = { vk_free, vk_draw, vk_read_pixels, vk_present, vk_window_gone };

#endif
