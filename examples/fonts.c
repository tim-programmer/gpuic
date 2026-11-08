/*
 * Font families and weights.
 *
 *   weights    gp_font_weight: 100 to 900 in one family. A weight the family has no face for draws the nearest
 *              one, the way CSS picks it; where it has nothing heavier than 600, bold is synthesized.
 *   families   gp_font_family: a name, a list ("Zzz, Noto Serif": an uninstalled name is skipped) or a
 *              generic family: sans-serif, serif, monospace (gp_font_sans / serif / mono are short for these).
 *   inherit    both inherit down the tree and are overridden by a closer one, like text size.
 *   italic     gp_font_italic: the family's italic face, or its upright one sheared when it has none.
 *   features   gp_font_features: OpenType features (tabular figures, small caps, ligatures off, ...).
 *   LCD        click the last card: grayscale / LCD RGB / LCD BGR (gp_fonts_set_antialiasing). Fringes show on a zoomed screenshot.
 *   animated   a weight that moves, with gp_with_animation: each step is laid out again, nothing renders.
 *
 * Which faces exist depends on what is installed; register your own with gp_font_add_file to pin them
 * (see "Bundling fonts" in the README). Try:  GPUIC_SCREENSHOT=out.ppm GPUIC_WAIT_MS=700 ./fonts
 */
#include <gpuic/gpuic.h>

#define BASE    gp_rgb(0x1e1e2e)
#define SURFACE gp_rgb(0x313244)
#define TEXT    gp_rgb(0xcdd6f4)
#define SUBTEXT gp_rgb(0x9399b2)
#define BLUE    gp_rgb(0x89b4fa)
#define PEACH   gp_rgb(0xfab387)

typedef struct { int family; int aa; } Demo;

static const char *const families[] = { "sans-serif", "serif", "monospace", "Zzz Not Installed, serif", "Noto Sans, Noto Sans CJK JP, sans-serif" };

static void next_family(void *self, gp_ctx *cx, const gp_mouse_event *ev, void *data) {
    Demo *d = self;
    d->family = (d->family + 1) % (int)(sizeof families / sizeof *families);
    gp_notify(cx);
}

static const int aa_modes[] = { GP_AA_AUTO, GP_AA_GRAYSCALE, GP_AA_LCD_RGB, GP_AA_LCD_BGR };
static const char *const aa_names[] = { "auto (the system's choice)", "grayscale", "LCD, RGB stripes", "LCD, BGR stripes" };

static void next_aa(void *self, gp_ctx *cx, const gp_mouse_event *ev, void *data) {
    Demo *d = self;
    d->aa = (d->aa + 1) % 4;
    gp_fonts_set_antialiasing(cx->app, aa_modes[d->aa]);
}

static gp_node *card(const char *title, gp_node *body) {
    return gp_div(gp_flex_col(), gp_gap(6), gp_p(12), gp_rounded(10), gp_bg(SURFACE),
                  gp_text(title, gp_text_size(12), gp_text_color(SUBTEXT), gp_font_sans()), body);
}

/* a weight breathing between light and black */
static gp_node *breathe(float t, void *data) { return gp_font_weight((int)gp_lerp(200.f, 900.f, t)); }

static gp_node *demo_render(void *self, gp_ctx *cx) {
    Demo *d = self;
    gp_node *rows[9];
    for (int i = 0; i < 9; i++)
        rows[i] = gp_div(gp_flex_row(), gp_gap(12), gp_items_center(),
                         gp_text(gp_fmt("%d", (i + 1) * 100), gp_text_size(12), gp_text_color(SUBTEXT), gp_w(32)),
                         gp_text("Sphinx of black quartz, judge my vow", gp_font_weight((i + 1) * 100)));
    return gp_div(
        gp_size_full(), gp_flex_row(), gp_gap(16), gp_p(20), gp_bg(BASE), gp_text_color(TEXT), gp_text_size(17),
        gp_div(gp_flex_1(), gp_flex_col(), gp_gap(14),
               gp_text("Font families and weights", gp_text_size(22), gp_font_bold()),
               card("weights 100 to 900", gp_div(gp_flex_col(), gp_gap(3), rows[0], rows[1], rows[2], rows[3], rows[4], rows[5], rows[6], rows[7], rows[8])),
               card("between the faces",
                    gp_div(gp_flex_col(), gp_gap(3),
                           gp_text("Weight 350: between light and regular", gp_font_weight(350)),
                           gp_text("Weight 450: the medium face if there is one", gp_font_weight(450)),
                           gp_text("Weight 550: the next heavier face", gp_font_weight(550)),
                           gp_text("Weight 650: the next heavier face", gp_font_weight(650))))),
        gp_div(gp_flex_1(), gp_flex_col(), gp_gap(14),
               card("generic families",
                    gp_div(gp_flex_col(), gp_gap(4),
                           gp_text("sans-serif: the quick brown fox", gp_font_sans()),
                           gp_text("serif: the quick brown fox", gp_font_serif()),
                           gp_text("monospace: the quick brown fox 0123", gp_font_mono()))),
               card("click to change the family of this card",
                    gp_div(gp_flex_col(), gp_gap(6), gp_font_family(families[d->family]), gp_cursor_pointer(), gp_on_click(next_family),
                           gp_text(families[d->family], gp_text_color(BLUE), gp_text_size(13)),
                           gp_text("Hamburgefonstiv 0123456789 日本語"),
                           gp_div(gp_font_bold(), gp_text("Bold, same family")),
                           gp_div(gp_font_weight(300), gp_text("Light, same family")))),
               card("inherit and override",
                    gp_div(gp_flex_col(), gp_gap(3), gp_font_serif(), gp_font_weight(300),
                           gp_text("A light serif box"),
                           gp_div(gp_font_bold(), gp_text("with a bold serif child")),
                           gp_div(gp_font_mono(), gp_text("and a light monospace one")))),
               card("italic: a real face, and a slanted upright one",
                    gp_div(gp_flex_col(), gp_gap(3),
                           gp_text("Noto Sans has an italic face", gp_font_italic()),
                           gp_text("a bold italic one too", gp_font_italic(), gp_font_bold()),
                           gp_text("monospace has none: it is slanted", gp_font_mono(), gp_font_italic()))),
               card("OpenType features",
                    gp_div(gp_flex_col(), gp_gap(3),
                           gp_text("1011 proportional", gp_font_features("pnum")),
                           gp_text("1011 tabular", gp_font_tabular_nums()),
                           gp_text("old-style 2024, slashed zero 100", gp_font_features("onum, zero")),
                           gp_text("Small Caps and fi fl ligatures", gp_font_features("smcp")),
                           gp_text("no ligatures: office affluent", gp_font_no_ligatures()))),
               card("click: antialiasing",
                    gp_div(gp_flex_col(), gp_gap(3), gp_cursor_pointer(), gp_on_click(next_aa),
                           gp_text(aa_names[d->aa], gp_text_color(BLUE), gp_text_size(13)),
                           gp_text("The quick brown fox Illumination", gp_text_size(15)))),
               card("an animated weight",
                    gp_text("Breathing", gp_text_size(30), gp_text_color(PEACH),
                            gp_with_animation("breathe", (gp_animation){ .duration = 1.6, .repeat = true, .ping_pong = true, .easing = GP_EASE_IN_OUT },
                                              breathe, NULL)))));
}

static void ready(gp_app *app, void *userdata) {
    gp_entity demo = gp_view_new(app, sizeof(Demo), &(Demo){ 0 }, demo_render, NULL);
    gp_open_window(app, &(gp_window_options){ .title = "gpuic: fonts", .width = 860, .height = 980 }, demo);
}

int main(void) { return gp_application_run(ready, NULL); }
