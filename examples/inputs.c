/*
 * The text input widget: single-line with live feedback, password, multi-line, read-only.
 * Tab moves between fields; Enter submits the name; Ctrl+Enter submits the notes.
 * Try: shift+arrows, ctrl+arrows, double- and triple-click, drag, ctrl+z / ctrl+y, ctrl+c/x/v.
 */
#include <gpuic/gpuic.h>

#include <stdio.h>
#include <string.h>

#define BASE     gp_rgb(0x1e1e2e)
#define SURFACE  gp_rgb(0x313244)
#define TEXT     gp_rgb(0xcdd6f4)
#define SUBTEXT  gp_rgb(0x9399b2)
#define GREEN    gp_rgb(0xa6e3a1)

typedef struct {
    gp_entity name, secret, notes, readonly;
    char status[200];
    int changes;
} Form;

static void changed(void *self, gp_ctx *cx, gp_entity input, void *data) {
    ((Form *)self)->changes++;
    gp_notify(cx);
}

static void submitted(void *self, gp_ctx *cx, gp_entity input, void *data) {
    Form *f = self;
    snprintf(f->status, sizeof f->status, "Submitted %s: \"%.120s\"", (const char *)data,
             gp_text_input_text(cx->app, input));
    gp_notify(cx);
}

static gp_node *labelled(const char *label, gp_entity input) {
    return gp_div(gp_flex_col(), gp_gap(5),
                  gp_text(label, gp_text_size(12), gp_text_color(SUBTEXT)),
                  gp_child_view(input));
}

static gp_node *form_render(void *self, gp_ctx *cx) {
    Form *f = self;
    gp_app *app = cx->app;
    const char *name = gp_text_input_text(app, f->name);
    const char *notes = gp_text_input_text(app, f->notes);
    int lines = 1, words = 0;
    bool in_word = false;
    for (const char *p = notes; *p; p++) {
        lines += *p == '\n';
        bool w = *p != ' ' && *p != '\n' && *p != '\t';
        words += w && !in_word;
        in_word = w;
    }
    size_t s0, s1;
    gp_text_input_selection(app, f->notes, &s0, &s1);

    return gp_div(
        gp_size_full(), gp_flex_col(), gp_gap(16), gp_p(28), gp_bg(BASE), gp_text_color(TEXT),
        gp_text("Text inputs  \xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e \xf0\x9f\x98\x80", gp_text_size(22), gp_font_bold()),
        gp_div(gp_flex_col(), gp_gap(14), gp_p(20), gp_rounded(14), gp_bg(SURFACE),
               gp_text(gp_fmt("Hello, %s!", *name ? name : "world"), gp_text_size(17), gp_font_bold()),
               labelled("Name  (Enter to submit)", f->name),
               labelled("Password", f->secret),
               gp_text(gp_fmt("%zu bytes typed", strlen(gp_text_input_text(app, f->secret))),
                       gp_text_size(12), gp_text_color(SUBTEXT)),
               labelled("Notes  (Ctrl+Enter to submit)", f->notes),
               gp_text(gp_fmt("%d lines · %d words · %zu bytes · selection %zu–%zu · %d edits", lines, words,
                              strlen(notes), s0, s1, f->changes),
                       gp_text_size(12), gp_text_color(SUBTEXT)),
               labelled("Read-only (selectable, copyable)", f->readonly),
               f->status[0] ? gp_text(f->status, gp_text_color(GREEN), gp_text_size(13)) : NULL),
        /* CJK has no spaces: it wraps between characters, and never starts a line with 。or 、 */
        gp_text("\xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e\xe3\x81\xae\xe3\x83\x86\xe3\x82\xad\xe3\x82\xb9\xe3\x83\x88\xe3\x81\xaf\xe3\x82\xb9\xe3\x83\x9a\xe3\x83\xbc\xe3\x82\xb9\xe3\x81\x8c\xe3\x81\xaa\xe3\x81\x84\xe3\x81\xae\xe3\x81\xa7\xe3\x80\x81\xe6\x96\x87\xe5\xad\x97\xe3\x81\x94\xe3\x81\xa8\xe3\x81\xab\xe6\x8a\x98\xe3\x82\x8a\xe8\xbf\x94\xe3\x81\x95\xe3\x82\x8c\xe3\x81\xbe\xe3\x81\x99\xe3\x80\x82\xe4\xb8\xad\xe6\x96\x87\xe4\xb9\x9f\xe6\x98\xaf\xe4\xb8\x80\xe6\xa0\xb7\xe3\x80\x82 \xf0\x9f\x8e\x89\xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e\xe3\x81\xae\xe3\x83\x86\xe3\x82\xad\xe3\x82\xb9\xe3\x83\x88\xe3\x81\xaf\xe3\x82\xb9\xe3\x83\x9a\xe3\x83\xbc\xe3\x82\xb9\xe3\x81\x8c\xe3\x81\xaa\xe3\x81\x84\xe3\x81\xae\xe3\x81\xa7\xe3\x80\x81\xe6\x96\x87\xe5\xad\x97\xe3\x81\x94\xe3\x81\xa8\xe3\x81\xab\xe6\x8a\x98\xe3\x82\x8a\xe8\xbf\x94\xe3\x81\x95\xe3\x82\x8c\xe3\x81\xbe\xe3\x81\x99\xe3\x80\x82\xe4\xb8\xad\xe6\x96\x87\xe4\xb9\x9f\xe6\x98\xaf\xe4\xb8\x80\xe6\xa0\xb7\xe3\x80\x82 \xf0\x9f\x8e\x89",
                gp_text_size(13), gp_text_color(SUBTEXT)));
}

static void ready(gp_app *app, void *userdata) {
    Form init = { 0 };
    gp_entity root = gp_view_new(app, sizeof init, &init, form_render, NULL);
    Form *f = gp_entity_get(app, root);
    f->name = gp_text_input_new(app, &(gp_text_input_options){
        .owner = root, .placeholder = "Your name", .on_change = changed, .on_submit = submitted, .data = "name" });
    f->secret = gp_text_input_new(app, &(gp_text_input_options){
        .owner = root, .password = true, .placeholder = "Secret", .on_change = changed });
    f->notes = gp_text_input_new(app, &(gp_text_input_options){
        .owner = root, .multiline = true, .soft_wrap = true, .rows = 6, .on_change = changed, .on_submit = submitted, .data = "notes",
        .text = "gpuic text input \xf0\x9f\x96\x8b\xef\xb8\x8f\nCJK: \xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e \xe4\xb8\xad\xe6\x96\x87 \xed\x95\x9c\xea\xb5\xad\xec\x96\xb4 \xe3\x81\xaf\xe3\x81\x84\nEmoji: \xf0\x9f\x98\x80 \xf0\x9f\x91\x8d\xf0\x9f\x8f\xbd \xf0\x9f\x87\xaf\xf0\x9f\x87\xb5 \xf0\x9f\x91\xa8\xe2\x80\x8d\xf0\x9f\x91\xa9\xe2\x80\x8d\xf0\x9f\x91\xa7 \xe2\x9d\xa4\xef\xb8\x8f 1\xef\xb8\x8f\xe2\x83\xa3\nLatin: h\xc3\xa9llo na\xc3\xafve \xc2\xbd \xc2\xa9\nLong lines wrap at the field's width instead of scrolling sideways: this sentence is deliberately long enough to need several rows, and the caret, selection, Home, End, Up and Down all follow the visual rows, not the line breaks.\n\xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e\xe3\x81\xae\xe3\x83\x86\xe3\x82\xad\xe3\x82\xb9\xe3\x83\x88\xe3\x82\x82\xe6\x96\x87\xe5\xad\x97\xe3\x81\x94\xe3\x81\xa8\xe3\x81\xab\xe6\x8a\x98\xe3\x82\x8a\xe8\xbf\x94\xe3\x81\x95\xe3\x82\x8c\xe3\x81\xbe\xe3\x81\x99\xe3\x80\x82\xe4\xb8\xad\xe6\x96\x87\xe4\xb9\x9f\xe6\x98\xaf\xe4\xb8\x80\xe6\xa0\xb7\xe3\x80\x82\nLast line." });
    f->readonly = gp_text_input_new(app, &(gp_text_input_options){
        .read_only = true, .text = "You can select and copy this, but not change it." });
    gp_open_window(app, &(gp_window_options){ .title = "gpuic — text inputs", .width = 640, .height = 740,
                                              .background = BASE }, root);
}

int main(void) { return gp_application_run(ready, NULL); }
