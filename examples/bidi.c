/*
 * Bidirectional text. Each row is a <text> element; nothing here asks for RTL: the paragraph
 * direction comes from the first strong character, and Hebrew / Arabic runs are reordered and
 * shaped (Arabic letters join) automatically. The last rows force a direction or alignment.
 */
#include <gpuic/gpuic.h>

#include <stdio.h>

#define BASE    gp_rgb(0x1e1e2e)
#define SURFACE gp_rgb(0x313244)
#define MANTLE  gp_rgb(0x181825)
#define TEXT    gp_rgb(0xcdd6f4)
#define SUBTEXT gp_rgb(0x9399b2)
#define BLUE    gp_rgb(0x89b4fa)

static gp_node *row(const char *caption, gp_node *text) {
    return gp_div(gp_flex_col(), gp_gap(4),
                  gp_text(caption, gp_text_size(11), gp_text_color(SUBTEXT)),
                  gp_div(gp_px(12), gp_py(8), gp_rounded(8), gp_bg(MANTLE), text));
}

typedef struct { gp_entity line, para; } Demo;

static gp_node *view(void *self, gp_ctx *cx) {
    Demo *d = self;
    return gp_div(
        gp_size_full(), gp_flex_col(), gp_gap(10), gp_p(22), gp_bg(BASE), gp_text_color(TEXT),
        gp_text("Bidirectional text", gp_text_size(20), gp_font_bold()),
        gp_div(
            gp_flex_col(), gp_gap(10), gp_p(16), gp_rounded(12), gp_bg(SURFACE),
            row("Latin: left to right", gp_text("Hello, world 2026")),
            row("Hebrew: right to left, aligned to the start (right)", gp_text("שלום עולם, זהו טקסט בעברית 2026")),
            row("Arabic: right to left, letters join (and lam + alef ligates)", gp_text("مرحبا بالعالم، لا بأس")),
            row("A Hebrew word inside an English sentence", gp_text("The word שלום means peace, and עולם means world.")),
            row("English names and digits inside a Hebrew sentence", gp_text("השם שלי John והגיל שלי 42 בשנת 2026")),
            row("Brackets mirror inside right-to-left text", gp_text("(שלום) [עולם] {abc} <xyz>")),
            row("Latin text forced right to left (gp_text_dir_rtl): aligned right, reading order kept",
                gp_text("one two three 123", gp_text_dir_rtl())),
            row("Hebrew with gp_text_end: the END of an RTL paragraph is the left",
                gp_text("שלום עולם", gp_text_end())),
            row("A wrapped Hebrew paragraph in a 280 px column",
                gp_div(gp_w(280), gp_text("זוהי פסקה ארוכה בעברית שנועדה להראות כיצד טקסט מימין לשמאל נשבר לשורות ומיושר לצד ימין, גם כאשר מופיעות בו מילים באנגלית כמו GPUI וגם מספרים כמו 2026.",
                                          gp_text_size(14))))),
        gp_div(
            gp_flex_col(), gp_gap(10), gp_p(16), gp_rounded(12), gp_bg(SURFACE),
            row("Editor: shift+arrows walk the screen, so a selection can be in two pieces", gp_child_view(d->line)),
            row("Editor: wrapping paragraphs of either direction (each one right-aligns by its own first strong letter)", gp_child_view(d->para))));
}

static void ready(gp_app *app, void *userdata) {
    Demo init = { 0 };
    gp_entity root = gp_view_new(app, sizeof init, &init, view, NULL);
    Demo *d = gp_entity_get(app, root);
    d->line = gp_text_input_new(app, &(gp_text_input_options){ .text = "ab אבג cd" });
    d->para = gp_text_input_new(app, &(gp_text_input_options){
        .multiline = true, .soft_wrap = true, .rows = 5,
        .text = "A left-to-right paragraph that is long enough to wrap onto a second line, with a Hebrew word שלום in it.\n"
                "זוהי פסקה בעברית שארוכה מספיק כדי להישבר לכמה שורות בשדה העריכה, וכוללת מילה באנגלית כמו GPUI ומספר 2026.\n"
                "مرحبا بالعالم، هذه فقرة بالعربية تلتف على عدة أسطر داخل حقل التحرير." });
    gp_open_window(app, &(gp_window_options){ .title = "gpuic — bidi", .width = 640, .height = 960, .background = BASE }, root);
}

int main(void) { return gp_application_run(ready, NULL); }
