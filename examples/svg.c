/*
 * SVG: gp_svg_load_memory / gp_svg_load_file and gp_svg_img; currentColor taking the text color, crisp at any size, gradients,
 * strokes, gp_svg_from_path, object-fit. The drawings are written inline, so the example needs no files:
 *
 *   ./svg [drawing.svg ...]     show these too
 */
#include <gpuic/gpuic.h>

#include <stdio.h>

#define BASE    gp_rgb(0x1e1e2e)
#define SURFACE gp_rgb(0x313244)
#define TEXT    gp_rgb(0xcdd6f4)
#define SUBTEXT gp_rgb(0x9399b2)

enum { MAX_FILES = 4 };
typedef struct { gp_svg check, heart, star, grad, badge, path, files[MAX_FILES]; int nfiles; } Demo;

static gp_node *card(const char *title, gp_node *body) {
    return gp_div(gp_flex_col(), gp_gap(6), gp_p(10), gp_rounded(10), gp_bg(SURFACE),
                  gp_text(title, gp_text_size(12), gp_text_color(SUBTEXT)), body);
}

static gp_node *demo_render(void *self, gp_ctx *cx) {
    Demo *d = self;
    gp_node *loaded[MAX_FILES];
    for (int i = 0; i < d->nfiles; i++) loaded[i] = gp_svg_img(d->files[i], gp_h(72));
    const gp_color pink = gp_rgb(0xf38ba8), green = gp_rgb(0xa6e3a1), blue = gp_rgb(0x89b4fa), yellow = gp_rgb(0xf9e2af);
    return gp_div(
        gp_size_full(), gp_flex_col(), gp_gap(12), gp_p(16), gp_bg(BASE), gp_text_color(TEXT),
        gp_text("SVG", gp_text_size(22), gp_font_bold()),
        gp_div(gp_flex_row(), gp_gap(12),
               card("currentColor: one icon, four text colors",
                    gp_div(gp_flex_row(), gp_gap(12),
                           gp_svg_img(d->check, gp_size_px(40), gp_text_color(green)), gp_svg_img(d->check, gp_size_px(40), gp_text_color(pink)),
                           gp_svg_img(d->check, gp_size_px(40), gp_text_color(blue)), gp_svg_img(d->check, gp_size_px(40), gp_text_color(yellow)))),
               card("sharp at any size: 16, 32, 64, 128 px",
                    gp_div(gp_flex_row(), gp_gap(10), gp_items_end(),
                           gp_svg_img(d->heart, gp_size_px(16), gp_text_color(pink)), gp_svg_img(d->heart, gp_size_px(32), gp_text_color(pink)),
                           gp_svg_img(d->heart, gp_size_px(64), gp_text_color(pink)), gp_svg_img(d->heart, gp_size_px(128), gp_text_color(pink))))),
        gp_div(gp_flex_row(), gp_gap(12),
               card("a gradient, strokes and an even-odd star", gp_div(gp_flex_row(), gp_gap(10),
                    gp_svg_img(d->grad, gp_w(120)), gp_svg_img(d->star, gp_size_px(80), gp_text_color(yellow)))),
               card("one path (gp_svg_from_path)", gp_svg_img(d->path, gp_size_px(80))),
               card("object-fit contain / cover in 90 x 50", gp_div(gp_flex_row(), gp_gap(8),
                    gp_svg_img(d->badge, gp_w(90), gp_h(50), gp_object_fit(GP_FIT_CONTAIN), gp_bg(BASE)),
                    gp_svg_img(d->badge, gp_w(90), gp_h(50), gp_object_fit(GP_FIT_COVER), gp_bg(BASE))))),
        d->nfiles ? card("from the command line", gp_div(gp_flex_row(), gp_gap(10), gp_children(loaded, (size_t)d->nfiles))) : NULL);
}

static void ready(gp_app *app, void *userdata) {
    Demo init = { 0 };
#define S(s) gp_svg_load_memory(app, s, sizeof(s) - 1)
    init.check = S("<svg xmlns='http://www.w3.org/2000/svg' width='24' height='24' viewBox='0 0 24 24'><circle cx='12' cy='12' r='10' fill='none' stroke='currentColor' stroke-width='2'/>"
                   "<path d='M7 12.5l3.5 3.5L17 9' fill='none' stroke='currentColor' stroke-width='2.4' stroke-linecap='round' stroke-linejoin='round'/></svg>");
    init.heart = S("<svg xmlns='http://www.w3.org/2000/svg' width='24' height='24' viewBox='0 0 24 24'><path fill='currentColor' d='M12 21s-8-5.2-8-11a4.5 4.5 0 0 1 8-2.8A4.5 4.5 0 0 1 20 10c0 5.8-8 11-8 11z'/></svg>");
    init.star = S("<svg xmlns='http://www.w3.org/2000/svg' width='100' height='100' viewBox='0 0 100 100'><path fill='currentColor' fill-rule='evenodd' d='M50 5 L61 38 L95 38 L67 58 L78 92 L50 71 L22 92 L33 58 L5 38 L39 38 Z M50 30 L56 44 L44 44 Z'/></svg>");
    init.grad = S("<svg xmlns='http://www.w3.org/2000/svg' width='120' height='80' viewBox='0 0 120 80'><defs><linearGradient id='g' x1='0' y1='0' x2='1' y2='1'><stop offset='0' stop-color='#f38ba8'/><stop offset='1' stop-color='#89b4fa'/></linearGradient>"
                  "<radialGradient id='r' cx='0.5' cy='0.5' r='0.5'><stop offset='0' stop-color='#fff' stop-opacity='0.9'/><stop offset='1' stop-color='#fff' stop-opacity='0'/></radialGradient></defs>"
                  "<rect width='120' height='80' rx='12' fill='url(#g)'/><circle cx='84' cy='30' r='24' fill='url(#r)'/><path d='M10 66 Q40 36 70 66 T120 60' fill='none' stroke='#1e1e2e' stroke-width='3' stroke-dasharray='6 4'/></svg>");
    init.badge = S("<svg xmlns='http://www.w3.org/2000/svg' width='60' height='60' viewBox='0 0 60 60'><circle cx='30' cy='30' r='28' fill='#a6e3a1'/><text/><rect x='14' y='26' width='32' height='8' fill='#1e1e2e'/><rect x='26' y='14' width='8' height='32' fill='#1e1e2e'/></svg>");
    init.path = gp_svg_from_path(app, "M10 70 L40 10 L70 70 L10 30 L70 30 Z", 80, 80, gp_rgba(0xf9e2af80), gp_rgb(0xf9e2af), 3);
#undef S
    for (int i = 0; i < MAX_FILES; i++) {
        extern int g_argc_svg; extern char **g_argv_svg;
        if (i + 1 < g_argc_svg) init.files[init.nfiles] = gp_svg_load_file(app, g_argv_svg[i + 1]);
        if (init.files[init.nfiles].generation) init.nfiles++;
    }
    gp_entity demo = gp_view_new(app, sizeof(Demo), &init, demo_render, NULL);
    gp_open_window(app, &(gp_window_options){ .title = "gpuic: svg", .width = 760, .height = 420 }, demo);
}

int g_argc_svg;
char **g_argv_svg;
int main(int argc, char **argv) {
    g_argc_svg = argc;
    g_argv_svg = argv;
    return gp_application_run(ready, NULL);
}
