/*
 * A CLAP plugin whose editor is a gpuic view: what a plugin needs from gpuic, in one file. The audio passes through unchanged (a stereo
 * effect); the editor has a text field (keys inside the host's window) and a button that counts clicks, and follows the host's resizing.
 *
 *   cmake -B build -DGPUIC_BUILD_CLAP_EXAMPLE=ON && cmake --build build --target clap_view
 *   cp build/clap_view.clap ~/.clap/          (Windows: %LOCALAPPDATA%\Programs\Common\CLAP\)
 *
 * The parts that matter: the editor is an app the host runs (gp_app_new in gui.create, gp_app_free in gui.destroy), stepped from the host's
 * timer (clap.timer-support: gp_app_step every 16 ms), and its window opens inside the host's (gui.set_parent: gp_window_options.parent).
 * On Linux the host's window is an X11 one, so SDL is pointed at its x11 driver before the app starts.
 */
#include <gpuic/gpuic.h>

#include <clap/clap.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
# define GUI_API CLAP_WINDOW_API_WIN32
#else
# define GUI_API CLAP_WINDOW_API_X11
#endif

typedef struct
{
    clap_plugin_t plugin;
    const clap_host_t* host;
    const clap_host_timer_support_t* timers;
    clap_id timer;
    gp_app* app; /* the editor, while it exists */
    gp_window* window;
    gp_entity view, input;
    uint32_t width, height;
    int clicks;
} Plugin;

/* ---------------------------------------------------------------- the editor's view */

static void clicked(void* self, gp_ctx* cx, const gp_mouse_event* ev, void* data)
{
    Plugin* p = *(Plugin**)self;
    p->clicks++;
    gp_notify(cx);
}

static gp_node* view(void* self, gp_ctx* cx)
{
    Plugin* p = *(Plugin**)self;
    return gp_div(gp_size_full(), gp_flex_col(), gp_gap(12), gp_p(16), gp_bg(gp_rgb(0x1e1e2e)), gp_text("gpuic inside a host", gp_font_bold(), gp_text_size(18), gp_text_color(gp_rgb(0xcdd6f4))), gp_text("Click the field and type: the keys come to the plugin, not the host.", gp_text_size(13), gp_text_color(gp_rgb(0x9399b2))), gp_child_view(p->input), gp_div(gp_self_start(), gp_px(14), gp_py(8), gp_rounded(6), gp_bg(gp_rgb(0x89b4fa)), gp_hover_bg(gp_rgb(0xb4befe)), gp_cursor_pointer(), gp_on_click(clicked), gp_text(gp_fmt("Clicked %d times", p->clicks), gp_text_color(gp_rgb(0x1e1e2e)))), gp_text(gp_fmt("%u x %u", p->width, p->height), gp_text_size(12), gp_text_color(gp_rgb(0x6c7086))));
}

/* ---------------------------------------------------------------- clap.gui */

static Plugin* of(const clap_plugin_t* plugin)
{
    return plugin->plugin_data;
}

static bool gui_is_api_supported(const clap_plugin_t* plugin, const char* api, bool is_floating)
{
    return !is_floating && strcmp(api, GUI_API) == 0;
}
static bool gui_get_preferred_api(const clap_plugin_t* plugin, const char** api, bool* is_floating)
{
    *api = GUI_API;
    *is_floating = false;
    return true;
}
static bool gui_create(const clap_plugin_t* plugin, const char* api, bool is_floating)
{
    Plugin* p = of(plugin);
    if(is_floating || strcmp(api, GUI_API) != 0 || p->app || !p->timers) {
        return false;
    }
#ifndef _WIN32
    setenv("SDL_VIDEO_DRIVER", "x11", 0); /* (the host's window is an X11 one, XWayland's on a Wayland desktop) */
#endif
    p->app = gp_app_new();
    if(!p->app) {
        return false;
    }
    p->input = gp_text_input_new(p->app, &(gp_text_input_options){ .placeholder = "Type here" });
    p->view = gp_view_new(p->app, sizeof p, &p, view, NULL);
    if(!p->timers->register_timer(p->host, 16, &p->timer)) { /* the host's timer steps the app: without it the editor would stand still */
        gp_app_free(p->app);
        p->app = NULL;
        return false;
    }
    return true;
}
static void gui_destroy(const clap_plugin_t* plugin)
{
    Plugin* p = of(plugin);
    if(!p->app) {
        return;
    }
    p->timers->unregister_timer(p->host, p->timer);
    gp_app_free(p->app); /* (its window goes before the host's, as embedding asks) */
    p->app = NULL;
    p->window = NULL;
}
static bool gui_set_scale(const clap_plugin_t* plugin, double scale)
{
    return false; /* (SDL takes the scale from the window) */
}
static bool gui_get_size(const clap_plugin_t* plugin, uint32_t* width, uint32_t* height)
{
    *width = of(plugin)->width;
    *height = of(plugin)->height;
    return true;
}
static bool gui_can_resize(const clap_plugin_t* plugin)
{
    return true;
}
static bool gui_get_resize_hints(const clap_plugin_t* plugin, clap_gui_resize_hints_t* hints)
{
    *hints = (clap_gui_resize_hints_t){ .can_resize_horizontally = true, .can_resize_vertically = true };
    return true;
}
static bool gui_adjust_size(const clap_plugin_t* plugin, uint32_t* width, uint32_t* height)
{
    *width = *width < 320 ? 320 : *width;
    *height = *height < 220 ? 220 : *height;
    return true;
}
static bool gui_set_size(const clap_plugin_t* plugin, uint32_t width, uint32_t height)
{
    Plugin* p = of(plugin);
    p->width = width;
    p->height = height;
    if(p->window) {
        gp_window_set_size(p->window, (int)width, (int)height);
        gp_ctx cx = gp_ctx_for(p->app, p->view);
        gp_notify(&cx);
    }
    return true;
}
static bool gui_set_parent(const clap_plugin_t* plugin, const clap_window_t* window)
{
    Plugin* p = of(plugin);
    if(!p->app || p->window || strcmp(window->api, GUI_API) != 0) {
        return false;
    }
#ifdef _WIN32
    void* parent = window->win32;
#else
    void* parent = (void*)(uintptr_t)window->x11;
#endif
    p->window = gp_open_window(p->app, &(gp_window_options){ .width = (int)p->width, .height = (int)p->height, .parent = parent }, p->view);
    return p->window != NULL;
}
static bool gui_set_transient(const clap_plugin_t* plugin, const clap_window_t* window)
{
    return false;
}
static void gui_suggest_title(const clap_plugin_t* plugin, const char* title)
{
}
static bool gui_show(const clap_plugin_t* plugin)
{
    return true;
}
static bool gui_hide(const clap_plugin_t* plugin)
{
    return true;
}
static const clap_plugin_gui_t gui = { gui_is_api_supported, gui_get_preferred_api, gui_create, gui_destroy, gui_set_scale, gui_get_size, gui_can_resize, gui_get_resize_hints, gui_adjust_size, gui_set_size, gui_set_parent, gui_set_transient, gui_suggest_title, gui_show, gui_hide };

/* ---------------------------------------------------------------- clap.timer-support */

static void on_timer(const clap_plugin_t* plugin, clap_id timer_id)
{
    Plugin* p = of(plugin);
    if(p->app && timer_id == p->timer) {
        gp_app_step(p->app, NULL);
    }
}
static const clap_plugin_timer_support_t timer_support = { on_timer };

/* ---------------------------------------------------------------- clap.audio-ports: one stereo pair in, one out */

static uint32_t ports_count(const clap_plugin_t* plugin, bool is_input)
{
    return 1;
}
static bool ports_get(const clap_plugin_t* plugin, uint32_t index, bool is_input, clap_audio_port_info_t* info)
{
    if(index != 0) {
        return false;
    }
    memset(info, 0, sizeof *info);
    info->id = 0;
    strcpy(info->name, is_input ? "In" : "Out");
    info->flags = CLAP_AUDIO_PORT_IS_MAIN;
    info->channel_count = 2;
    info->port_type = CLAP_PORT_STEREO;
    info->in_place_pair = 0;
    return true;
}
static const clap_plugin_audio_ports_t audio_ports = { ports_count, ports_get };

/* ---------------------------------------------------------------- the plugin */

static bool plugin_init(const clap_plugin_t* plugin)
{
    Plugin* p = of(plugin);
    p->timers = p->host->get_extension(p->host, CLAP_EXT_TIMER_SUPPORT);
    p->width = 420;
    p->height = 260;
    return true;
}
static void plugin_destroy(const clap_plugin_t* plugin)
{
    gui_destroy(plugin);
    free(of(plugin));
}
static bool plugin_activate(const clap_plugin_t* plugin, double sample_rate, uint32_t min_frames, uint32_t max_frames)
{
    return true;
}
static void plugin_deactivate(const clap_plugin_t* plugin)
{
}
static bool plugin_start_processing(const clap_plugin_t* plugin)
{
    return true;
}
static void plugin_stop_processing(const clap_plugin_t* plugin)
{
}
static void plugin_reset(const clap_plugin_t* plugin)
{
}
static clap_process_status plugin_process(const clap_plugin_t* plugin, const clap_process_t* process)
{
    if(process->audio_inputs_count && process->audio_outputs_count) { /* (through, unchanged) */
        const clap_audio_buffer_t *in = &process->audio_inputs[0], *out = &process->audio_outputs[0];
        for(uint32_t c = 0; c < 2 && c < in->channel_count && c < out->channel_count; c++) {
            if(in->data32 && out->data32 && in->data32[c] != out->data32[c]) {
                memcpy(out->data32[c], in->data32[c], process->frames_count * sizeof(float));
            }
        }
    }
    return CLAP_PROCESS_CONTINUE;
}
static const void* plugin_get_extension(const clap_plugin_t* plugin, const char* id)
{
    if(strcmp(id, CLAP_EXT_GUI) == 0) {
        return &gui;
    }
    if(strcmp(id, CLAP_EXT_TIMER_SUPPORT) == 0) {
        return &timer_support;
    }
    if(strcmp(id, CLAP_EXT_AUDIO_PORTS) == 0) {
        return &audio_ports;
    }
    return NULL;
}
static void plugin_on_main_thread(const clap_plugin_t* plugin)
{
}

static const char* const features[] = { CLAP_PLUGIN_FEATURE_AUDIO_EFFECT, CLAP_PLUGIN_FEATURE_UTILITY, CLAP_PLUGIN_FEATURE_STEREO, NULL };
static const clap_plugin_descriptor_t descriptor = {
    .clap_version = CLAP_VERSION_INIT,
    .id = "dev.gpuic.clap-view",
    .name = "gpuic view",
    .vendor = "gpuic",
    .url = "",
    .manual_url = "",
    .support_url = "",
    .version = "0.1.0",
    .description = "A gpuic view as a CLAP plugin's editor (audio passes through)",
    .features = features,
};

static const clap_plugin_t* create_plugin(const clap_plugin_factory_t* factory, const clap_host_t* host, const char* id)
{
    if(!clap_version_is_compatible(host->clap_version) || strcmp(id, descriptor.id) != 0) {
        return NULL;
    }
    Plugin* p = calloc(1, sizeof *p);
    p->host = host;
    p->plugin = (clap_plugin_t){ &descriptor, p, plugin_init, plugin_destroy, plugin_activate, plugin_deactivate, plugin_start_processing, plugin_stop_processing, plugin_reset, plugin_process, plugin_get_extension, plugin_on_main_thread };
    return &p->plugin;
}
static uint32_t factory_count(const clap_plugin_factory_t* factory)
{
    return 1;
}
static const clap_plugin_descriptor_t* factory_descriptor(const clap_plugin_factory_t* factory, uint32_t index)
{
    return index == 0 ? &descriptor : NULL;
}
static const clap_plugin_factory_t factory = { factory_count, factory_descriptor, create_plugin };

static bool entry_init(const char* path)
{
    return true;
}
static void entry_deinit(void)
{
}
static const void* entry_get_factory(const char* id)
{
    return strcmp(id, CLAP_PLUGIN_FACTORY_ID) == 0 ? &factory : NULL;
}
CLAP_EXPORT const clap_plugin_entry_t clap_entry = { CLAP_VERSION_INIT, entry_init, entry_deinit, entry_get_factory };
