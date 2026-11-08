/*
 * Observing entities. One model (a plain entity, no render function) and three views that depend
 * on it without knowing about each other:
 *
 *   Controls  changes the model and calls gp_notify on it, and emits a "milestone" event every 5
 *   Gauge     gp_observe: keeps state a pure render could not (the peak seen so far)
 *   Log       gp_subscribe to "milestone", gp_observe_release to notice the model going away
 *
 * The gauge and the log are cached views (gp_child_view_cached): pressing a button renders only the views that
 * were notified, and the rest of the window reuses what it built before.
 *
 * "Retire model" releases the model entity: subscriptions end by themselves, the release observers
 * run first (with the state still valid), and the buttons find the entity gone.
 */
#include <gpuic/gpuic.h>

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define BASE     gp_rgb(0x1e1e2e)
#define SURFACE  gp_rgb(0x313244)
#define OVERLAY  gp_rgb(0x45475a)
#define TEXT     gp_rgb(0xcdd6f4)
#define SUBTEXT  gp_rgb(0x9399b2)
#define BLUE     gp_rgb(0x89b4fa)
#define BLUE_HI  gp_rgb(0xa6c4fb)
#define GREEN    gp_rgb(0xa6e3a1)
#define RED      gp_rgb(0xf38ba8)

typedef struct { int value; } Model;
typedef struct { int value; } Milestone;   /* payload of the "milestone" event */

typedef struct { gp_entity model; } Controls;
typedef struct { int shown, peak; bool gone; } Gauge;
typedef struct { char lines[8][64]; int n; } Log;

/* ---- Controls: the only code that writes the model */

static void change(void *self, gp_ctx *cx, const gp_mouse_event *ev, void *data) {
    Controls *c = self;
    Model *m = gp_entity_get(cx->app, c->model);
    if (!m) return;   /* retired */
    m->value += (int)(intptr_t)data;
    gp_ctx mc = gp_ctx_for(cx->app, c->model);
    gp_notify(&mc);   /* the model is not a view: this only calls its observers, once this handler has returned */
    if (m->value > 0 && m->value % 5 == 0) gp_emit_as(&mc, "milestone", Milestone, .value = m->value);
}

static void retire(void *self, gp_ctx *cx, const gp_mouse_event *ev, void *data) {
    gp_entity_release(cx->app, ((Controls *)self)->model);
    gp_notify(cx);
}

static gp_node *button(const char *label, gp_mouse_fn fn, void *data) {
    return gp_div(gp_px(16), gp_py(8), gp_rounded(8), gp_bg(SURFACE), gp_hover_bg(OVERLAY),
                  gp_cursor_pointer(), gp_on_click_data(fn, data), gp_text(label, gp_font_bold()));
}

static gp_node *controls_render(void *self, gp_ctx *cx) {
    bool alive = gp_entity_get(cx->app, ((Controls *)self)->model) != NULL;
    return gp_div(gp_flex_row(), gp_gap(10),
                  button("−1", change, (void *)(intptr_t)-1), button("+1", change, (void *)(intptr_t)1),
                  button("+5", change, (void *)(intptr_t)5),
                  gp_div(gp_flex_1()),
                  alive ? button("Retire model", retire, NULL) : gp_text("model released", gp_text_color(RED)));
}

/* ---- Gauge: derived state kept by an observer */

static void gauge_changed(void *self, gp_ctx *cx, gp_entity observed, void *data) {
    Gauge *g = self;
    const Model *m = gp_entity_get(cx->app, observed);
    g->shown = m->value;
    if (g->shown > g->peak) g->peak = g->shown;
    gp_notify(cx);   /* cx->entity is the gauge: it renders again. An observer that changes its view's state says so */
}

static void gauge_model_gone(void *self, gp_ctx *cx, gp_entity released, void *state, void *data) {
    ((Gauge *)self)->gone = true;
    gp_notify(cx);
    (void)state;
}

static gp_node *gauge_render(void *self, gp_ctx *cx) {
    const Gauge *g = self;
    float w = 360.f * (float)(g->shown < 0 ? 0 : g->shown > 50 ? 50 : g->shown) / 50.f;
    return gp_div(
        gp_flex_col(), gp_gap(8), gp_p(16), gp_rounded(12), gp_bg(SURFACE),
        gp_div(gp_flex_row(), gp_gap(12), gp_items_center(),
               gp_text(gp_fmt("%d", g->shown), gp_text_size(40), gp_font_bold(),
                       g->shown < 0 ? gp_text_color(RED) : NULL),
               gp_text(gp_fmt("peak %d%s", g->peak, g->gone ? " · model released" : ""), gp_text_size(13),
                       gp_text_color(SUBTEXT))),
        gp_div(gp_h(10), gp_rounded(5), gp_bg(BASE),
               gp_div(gp_w(w), gp_h(10), gp_rounded(5), gp_bg(g->gone ? OVERLAY : BLUE))));
}

/* ---- Log: events and the end of the model */

static void log_add(Log *l, const char *s) {
    if (l->n == 8) { memmove(l->lines[0], l->lines[1], sizeof l->lines - sizeof l->lines[0]); l->n--; }
    snprintf(l->lines[l->n++], sizeof l->lines[0], "%s", s);
}

static void on_milestone(void *self, gp_ctx *cx, const gp_event *ev, void *data) {
    const Milestone *m = ev->payload;
    char buf[64];
    snprintf(buf, sizeof buf, "milestone: reached %d", m->value);
    log_add(self, buf);
    gp_notify(cx);
}

static void log_model_gone(void *self, gp_ctx *cx, gp_entity released, void *state, void *data) {
    char buf[64];
    snprintf(buf, sizeof buf, "model released (it held %d)", ((Model *)state)->value);   /* still valid here */
    log_add(self, buf);
    gp_notify(cx);
}

static gp_node *log_render(void *self, gp_ctx *cx) {
    const Log *l = self;
    gp_node **rows = gp_frame_alloc(8 * sizeof *rows);
    for (int i = 0; i < l->n; i++) rows[i] = gp_text(l->lines[i], gp_text_size(13), gp_text_color(i == l->n - 1 ? TEXT : SUBTEXT));
    return gp_div(gp_flex_col(), gp_gap(4), gp_p(16), gp_h(200), gp_rounded(12), gp_bg(SURFACE),
                  gp_text("events", gp_text_size(12), gp_text_color(SUBTEXT)),
                  l->n ? gp_children(rows, (size_t)l->n) : gp_text("press +5", gp_text_size(13), gp_text_color(OVERLAY)));
}

/* ---- wiring */

static gp_node *root_render(void *self, gp_ctx *cx) {
    gp_entity *kids = self;   /* controls, gauge, log */
    return gp_div(gp_size_full(), gp_flex_col(), gp_gap(14), gp_p(24), gp_bg(BASE), gp_text_color(TEXT),
                  gp_text("One model, three views", gp_text_size(20), gp_font_bold()),
                  gp_child_view(kids[0]),
                  /* cached: they render again only when they, or what they observe, say so */
                  gp_child_view_cached(kids[1]), gp_child_view_cached(kids[2]));
}

static void ready(gp_app *app, void *userdata) {
    gp_entity model = GP_NEW(app, Model, .value = 0);
    gp_entity controls = gp_view_new(app, sizeof(Controls), &(Controls){ .model = model }, controls_render, NULL);
    gp_entity gauge = gp_view_new(app, sizeof(Gauge), NULL, gauge_render, NULL);
    gp_entity log = gp_view_new(app, sizeof(Log), NULL, log_render, NULL);

    gp_ctx g = gp_ctx_for(app, gauge), l = gp_ctx_for(app, log);   /* the observer is the ctx's entity */
    gp_observe(&g, model, gauge_changed, NULL);
    gp_observe_release(&g, model, gauge_model_gone, NULL);
    gp_subscribe(&l, model, "milestone", on_milestone, NULL);
    gp_observe_release(&l, model, log_model_gone, NULL);

    gp_entity kids[3] = { controls, gauge, log };
    gp_entity root = gp_view_new(app, sizeof kids, kids, root_render, NULL);
    gp_open_window(app, &(gp_window_options){ .title = "gpuic — observing entities", .width = 520,
                                              .height = 520, .background = BASE },
                   root);
}

int main(void) { return gp_application_run(ready, NULL); }
