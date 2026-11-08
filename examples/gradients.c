/*
 * Gradients: gp_bg_gradient (linear, CSS angles), gp_bg_radial, several stops, hard stops, a fade to transparent, a
 * rounded box with a border, and an animated angle. The hover color replaces a gradient like it does a plain background.
 *
 * Try:  GPUIC_SCREENSHOT=out.ppm GPUIC_WAIT_MS=600 ./gradients
 */
#include <gpuic/gpuic.h>

#define BASE    gp_rgb(0x1e1e2e)
#define SURFACE gp_rgb(0x313244)
#define TEXT    gp_rgb(0xcdd6f4)
#define SUBTEXT gp_rgb(0x9399b2)

static gp_node *card(const char *title, gp_node *box) {
    return gp_div(gp_flex_col(), gp_gap(6), gp_p(10), gp_rounded(10), gp_bg(SURFACE), gp_flex_1(),
                  gp_text(title, gp_text_size(12), gp_text_color(SUBTEXT)), box);
}

static gp_node *spin(float t, void *data) {
    return gp_bg_gradient(360.f * t, gp_stop(gp_rgb(0xf38ba8), 0), gp_stop(gp_rgb(0x89b4fa), 1));
}

static gp_node *demo_render(void *self, gp_ctx *cx) {
    const gp_color red = gp_rgb(0xf38ba8), blue = gp_rgb(0x89b4fa), green = gp_rgb(0xa6e3a1), yellow = gp_rgb(0xf9e2af);
    return gp_div(
        gp_size_full(), gp_flex_col(), gp_gap(14), gp_p(20), gp_bg(BASE), gp_text_color(TEXT),
        gp_text("Gradients", gp_text_size(22), gp_font_bold()),
        gp_div(gp_flex_row(), gp_gap(14),
               card("90 deg: left to right", gp_div(gp_h(80), gp_rounded(8), gp_bg_gradient(90, gp_stop(red, 0), gp_stop(blue, 1)))),
               card("180 deg, three stops", gp_div(gp_h(80), gp_rounded(8), gp_bg_gradient(180, gp_stop(red, 0), gp_stop(yellow, 0.5f), gp_stop(green, 1)))),
               card("45 deg", gp_div(gp_h(80), gp_rounded(8), gp_bg_gradient(45, gp_stop(blue, 0), gp_stop(red, 1))))),
        gp_div(gp_flex_row(), gp_gap(14),
               card("radial", gp_div(gp_h(80), gp_rounded(8), gp_bg_radial(0.5f, 0.5f, gp_stop(yellow, 0), gp_stop(BASE, 1)))),
               card("radial off center", gp_div(gp_h(80), gp_rounded(8), gp_bg_radial(0.2f, 0.3f, gp_stop(green, 0), gp_stop(blue, 0.6f), gp_stop(SURFACE, 1)))),
               card("hard stops (stripes)", gp_div(gp_h(80), gp_rounded(8),
                                                    gp_bg_gradient(90, gp_stop(red, 0), gp_stop(red, 0.25f), gp_stop(yellow, 0.25f), gp_stop(yellow, 0.5f),
                                                                   gp_stop(green, 0.5f), gp_stop(green, 0.75f), gp_stop(blue, 0.75f), gp_stop(blue, 1))))),
        gp_div(gp_flex_row(), gp_gap(14),
               card("repeating (stripes)", gp_div(gp_h(80), gp_rounded(8), gp_bg_repeating(45, gp_stop(yellow, 0), gp_stop(yellow, 0.04f), gp_stop(BASE, 0.04f), gp_stop(BASE, 0.08f)))),
               card("repeating radial", gp_div(gp_h(80), gp_rounded(8), gp_bg_repeating_radial(0.5f, 0.5f, gp_stop(blue, 0), gp_stop(BASE, 0.07f)))),
               card("conic", gp_div(gp_h(80), gp_rounded_corners(40, 8, 40, 8), gp_bg_conic(0.5f, 0.5f, 0, gp_stop(red, 0), gp_stop(yellow, 0.33f), gp_stop(green, 0.66f), gp_stop(red, 1)))),
               card("per-corner radii, per-side borders",
                    gp_div(gp_h(80), gp_rounded_corners(30, 4, 30, 4), gp_border_left(6), gp_border_bottom(3), gp_border_color(TEXT), gp_bg_gradient(135, gp_stop(green, 0), gp_stop(blue, 1))))),
        gp_div(gp_flex_row(), gp_gap(14),
               card("a color to transparent, over text",
                    gp_div(gp_h(80), gp_rounded(8), gp_bg(gp_rgb(0x45475a)), gp_flex_col(), gp_justify_center(), gp_px(10),
                           gp_text("no gray band in the middle of the fade"),
                           gp_div(gp_absolute(), gp_left(0), gp_top(0), gp_w_full(), gp_h_full(), gp_rounded(8),
                                  gp_bg_gradient(90, gp_stop(gp_rgba(0xf38ba8ff), 0), gp_stop(gp_rgba(0xf38ba800), 1))))),
               card("rounded, bordered, 60% opaque",
                    gp_div(gp_h(80), gp_rounded(24), gp_border(3), gp_border_color(TEXT), gp_opacity(0.6f), gp_bg_gradient(135, gp_stop(green, 0), gp_stop(blue, 1)))),
               card("hover me: a plain color takes over",
                    gp_div(gp_h(80), gp_rounded(8), gp_bg_gradient(90, gp_stop(red, 0), gp_stop(blue, 1)), gp_hover_bg(gp_rgb(0x585b70)))),
               card("angle animated",
                    gp_div(gp_h(80), gp_rounded(8), gp_with_animation("spin", (gp_animation){ .duration = 4, .repeat = true }, spin, NULL)))));
}

static void ready(gp_app *app, void *userdata) {
    gp_entity demo = gp_view_new(app, 0, NULL, demo_render, NULL);
    gp_open_window(app, &(gp_window_options){ .title = "gpuic: gradients", .width = 900, .height = 720 }, demo);
}

int main(void) { return gp_application_run(ready, NULL); }
