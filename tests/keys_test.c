/*
 * Keyboard: chords (several keystrokes in one binding), key-up events and action payloads, through the whole path: synthetic
 * SDL key events -> key mapping -> bindings -> focus path -> handlers. Needs a display.
 */
#include "../src/internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;
#define CHECK(cond, ...)                                                               \
    do {                                                                               \
        if (!(cond)) {                                                                 \
            if (failures++ < 40) {                                                     \
                printf("FAIL %s:%d: ", __FILE__, __LINE__);                            \
                printf(__VA_ARGS__);                                                   \
                printf("\n");                                                          \
            }                                                                          \
        }                                                                              \
    } while (0)

static gp_app *app;
static gp_co *co_;
static gp_window *win(void) { return app->windows[0]; }

typedef struct {
    gp_focus editor, other;
    int comment, uncomment, top, saveall, jump, jump2, one, two, global_sel;
    int select_sum, select_calls, last_payload;
    bool payload_outside_null;
    int nested_outer_before, nested_outer_after, nested_inner;
    char downs[512];      /* "key|mods " for every key that reached on_key_down */
    char ups[512];
    char texts[64];
} Fix;

static void logk(char *buf, size_t cap, const gp_key_event *ev) {
    size_t n = strlen(buf);
    snprintf(buf + n, cap - n, "%s|%u%s ", ev->key, ev->mods, ev->repeat ? "r" : "");
}
static void on_down(void *self, gp_ctx *cx, const gp_key_event *ev, void *data) { logk(((Fix *)self)->downs, sizeof ((Fix *)self)->downs, ev); }
static void on_up(void *self, gp_ctx *cx, const gp_key_event *ev, void *data) { logk(((Fix *)self)->ups, sizeof ((Fix *)self)->ups, ev); }
static void on_text(void *self, gp_ctx *cx, const char *utf8, void *data) { strncat(((Fix *)self)->texts, utf8, 63 - strlen(((Fix *)self)->texts)); }

static void a_comment(void *self, gp_ctx *cx, void *d) { ((Fix *)self)->comment++; }
static void a_uncomment(void *self, gp_ctx *cx, void *d) { ((Fix *)self)->uncomment++; }
static void a_top(void *self, gp_ctx *cx, void *d) { ((Fix *)self)->top++; }
static void a_saveall(void *self, gp_ctx *cx, void *d) { ((Fix *)self)->saveall++; }
static void a_jump(void *self, gp_ctx *cx, void *d) { ((Fix *)self)->jump++; }
static void a_jump2(void *self, gp_ctx *cx, void *d) { ((Fix *)self)->jump2++; }
static void a_one(void *self, gp_ctx *cx, void *d) { ((Fix *)self)->one++; }
static void a_two(void *self, gp_ctx *cx, void *d) { ((Fix *)self)->two++; }
static void a_select(void *self, gp_ctx *cx, void *d) {
    Fix *f = self;
    size_t size = 0;
    const void *p = gp_action_payload(cx, &size);
    f->select_calls++;
    if (p && size == sizeof(int)) { f->last_payload = *(const int *)p; f->select_sum += *(const int *)p; }
    else f->last_payload = -1;
}
static void a_outer(void *self, gp_ctx *cx, void *d) {
    Fix *f = self;
    size_t size;
    const int *mine = gp_action_payload(cx, &size);
    f->nested_outer_before = mine ? *mine : -1;
    int inner = 77;
    gp_dispatch_action_with(cx, "tab::select", &inner, sizeof inner);
    mine = gp_action_payload(cx, &size);
    f->nested_outer_after = mine ? *mine : -1;
    f->nested_inner = f->last_payload;
}

static gp_node *render(void *self, gp_ctx *cx) {
    Fix *f = self;
    return gp_div(
        gp_size_full(), gp_flex_col(), gp_gap(8), gp_p(8),
        gp_div(gp_size_px(60), gp_track_focus(f->editor), gp_tab_stop(), gp_key_context("Editor"), gp_on_key_down(on_down), gp_on_key_up(on_up), gp_on_text_input(on_text),
               gp_on_action("ed::comment", a_comment), gp_on_action("ed::uncomment", a_uncomment), gp_on_action("ed::top", a_top),
               gp_on_action("ed::saveall", a_saveall), gp_on_action("ed::jump", a_jump), gp_on_action("ed::jump2", a_jump2),
               gp_on_action("a::one", a_one), gp_on_action("a::two", a_two), gp_on_action("tab::select", a_select), gp_on_action("tab::outer", a_outer)),
        gp_div(gp_size_px(60), gp_track_focus(f->other), gp_tab_stop(), gp_on_key_down(on_down), gp_on_key_up(on_up)));
}

static void keys(const char *k) {
    gp_simulate_keystrokes(app, k);
    gp_sleep(co_, 0.12);
}

static void run(Fix *fx) {
    gp_sleep(co_, 0.15);
    gp_ctx cx = gp_ctx_for_window(app, win(), win()->root_view);
    gp_focus_set(&cx, fx->editor);
    gp_sleep(co_, 0.1);
    int skew = 0;

    /* a chord: ctrl-k, then ctrl-c */
    keys("ctrl-k");
    CHECK(strcmp(gp_keys_pending(&cx), "ctrl-k") == 0, "after ctrl-k the chord is pending: \"%s\"", gp_keys_pending(&cx));
    CHECK(fx->comment == 0 && fx->downs[0] == 0, "the first key of a chord is consumed: nothing yet (%d, \"%s\")", fx->comment, fx->downs);
    keys("ctrl-c");
    CHECK(fx->comment == 1 && fx->uncomment == 0 && gp_keys_pending(&cx)[0] == 0 && fx->downs[0] == 0, "ctrl-k ctrl-c ran the comment action once (%d), nothing is pending, no key reached on_key_down (\"%s\")", fx->comment, fx->downs);
    keys("ctrl-k ctrl-u");
    CHECK(fx->uncomment == 1 && fx->comment == 1, "ctrl-k ctrl-u is another chord");
    keys("ctrl-k ctrl-c ctrl-k ctrl-c");
    CHECK(fx->comment == 3, "a chord again and again (%d)", fx->comment);

    /* two plain keys, and three keystrokes */
    keys("g g");
    CHECK(fx->top == 1 && fx->texts[0] == 0, "g g (plain keys): the action ran, and no text was typed (\"%s\")", fx->texts);
    keys("ctrl-x ctrl-s ctrl-w");
    CHECK(fx->saveall == 1, "three keystrokes (%d)", fx->saveall);
    keys("ctrl-x ctrl-s");
    CHECK(strcmp(gp_keys_pending(&cx), "ctrl-x ctrl-s") == 0, "two of three: \"%s\"", gp_keys_pending(&cx));
    keys("ctrl-w");
    CHECK(fx->saveall == 2 && gp_keys_pending(&cx)[0] == 0, "and the third finishes it");

    /* a finished chord is consumed even if nobody handles its action (a single bound key falls through to on_key_down; a chord's last key is not an ordinary key) */
    fx->downs[0] = 0;
    keys("ctrl-m ctrl-n");
    CHECK(fx->downs[0] == 0 && gp_keys_pending(&cx)[0] == 0, "an unhandled chord is consumed (\"%s\")", fx->downs);

    /* a key that does not continue the chord cancels it and is consumed; the next key is an ordinary one */
    keys("ctrl-k x");
    CHECK(fx->comment == 3 && fx->uncomment == 1 && fx->downs[0] == 0 && gp_keys_pending(&cx)[0] == 0, "ctrl-k x: neither action, x is not delivered (\"%s\"), nothing pending", fx->downs);
    keys("x");
    CHECK(strstr(fx->downs, "x|0") != NULL, "and then x is an ordinary key again (\"%s\")", fx->downs);
    fx->downs[0] = 0;
    /* escape cancels a pending chord and is consumed; without one it is delivered */
    int top0 = fx->top;
    keys("ctrl-k");
    keys("escape");
    CHECK(gp_keys_pending(&cx)[0] == 0 && fx->downs[0] == 0, "escape cancels the chord and is consumed (\"%s\")", fx->downs);
    CHECK(fx->top == top0, "...even where \"ctrl-k escape\" is bound: escape cancels first (%d)", fx->top - top0);
    keys("ctrl-c");
    CHECK(fx->comment == 3 && strstr(fx->downs, "ctrl-c") == NULL ? true : strstr(fx->downs, "c|1") != NULL, "after the cancel ctrl-c alone does nothing as a chord");
    fx->downs[0] = 0;
    keys("escape");
    CHECK(strstr(fx->downs, "escape|0") != NULL, "escape with no chord pending reaches the handlers (\"%s\")", fx->downs);
    fx->downs[0] = 0;

    /* a half-typed chord is forgotten after 1.5 s: the next key is an ordinary one */
    keys("ctrl-k");
    skew += 2;
    gp_debug_clock_skew((int64_t)skew * 1000000000ll);
    keys("ctrl-c");
    CHECK(fx->comment == 3 && strstr(fx->downs, "c|1") != NULL, "a chord that waited two seconds is forgotten: ctrl-c arrives as a key (\"%s\", %d comments)", fx->downs, fx->comment);
    fx->downs[0] = 0;
    gp_debug_clock_skew((int64_t)skew * 1000000000ll);
    keys("ctrl-k ctrl-c");
    CHECK(fx->comment == 4, "(and a fresh chord works)");

    /* contexts: the chord only exists where its context holds */
    gp_focus_set(&cx, fx->other);
    gp_sleep(co_, 0.1);
    fx->downs[0] = 0;
    keys("ctrl-k");
    CHECK(strcmp(gp_keys_pending(&cx), "") == 0 && strstr(fx->downs, "k|1") != NULL, "outside the Editor context ctrl-k is just a key (\"%s\")", fx->downs);
    keys("ctrl-c");
    CHECK(fx->comment == 4, "and ctrl-k ctrl-c does nothing there");
    gp_focus_set(&cx, fx->editor);
    gp_sleep(co_, 0.1);
    fx->downs[0] = 0;

    /* a binding that is a whole chord's prefix wins at once; the later of two equal bindings wins */
    keys("ctrl-j");
    CHECK(fx->jump == 1 && fx->jump2 == 0 && gp_keys_pending(&cx)[0] == 0, "\"ctrl-j\" fires at once; \"ctrl-j ctrl-j\" is shadowed by it (%d, %d)", fx->jump, fx->jump2);
    keys("ctrl-d");
    CHECK(fx->two == 1 && fx->one == 0, "of two bindings for the same keys the later wins (%d, %d)", fx->one, fx->two);

    /* key up: after every simulated press, with the key and modifiers of the press; a lone release; handlers bubble */
    fx->downs[0] = fx->ups[0] = 0;
    keys("a");
    CHECK(strcmp(fx->downs, "a|0 ") == 0 && strcmp(fx->ups, "a|0 ") == 0, "key up follows key down: down \"%s\", up \"%s\"", fx->downs, fx->ups);
    fx->downs[0] = fx->ups[0] = 0;
    gp_simulate_key(app, "shift-b", true);
    gp_sleep(co_, 0.1);
    CHECK(strcmp(fx->downs, "b|4 ") == 0 && fx->ups[0] == 0, "a press alone: no release yet (\"%s\" / \"%s\")", fx->downs, fx->ups);
    gp_simulate_key(app, "shift-b", false);
    gp_sleep(co_, 0.1);
    CHECK(strcmp(fx->ups, "b|4 ") == 0, "and the release (\"%s\")", fx->ups);
    fx->downs[0] = fx->ups[0] = 0;
    keys("ctrl-k ctrl-c");
    CHECK(strcmp(fx->ups, "k|1 c|1 ") == 0, "the releases of a chord's keys are delivered (only presses are held back): \"%s\"", fx->ups);
    gp_focus_set(&cx, fx->other);
    gp_sleep(co_, 0.1);
    fx->ups[0] = 0;
    keys("q");
    CHECK(strcmp(fx->ups, "q|0 ") == 0, "key up goes to the focused element's handler, wherever focus is (\"%s\")", fx->ups);
    gp_focus_set(&cx, fx->editor);
    gp_sleep(co_, 0.1);

    /* payloads */
    keys("ctrl-1");
    CHECK(fx->select_calls == 1 && fx->last_payload == 1, "ctrl-1 delivers its payload (%d calls, %d)", fx->select_calls, fx->last_payload);
    keys("ctrl-2 ctrl-2 ctrl-1");
    CHECK(fx->select_sum == 1 + 2 + 2 + 1 && fx->select_calls == 4, "the same action with other payloads (sum %d)", fx->select_sum);
    int nine = 9;
    CHECK(gp_dispatch_action_with(&cx, "tab::select", &nine, sizeof nine) && fx->last_payload == 9, "gp_dispatch_action_with: %d", fx->last_payload);
    CHECK(gp_dispatch_action(&cx, "tab::select") && fx->last_payload == -1, "an action dispatched without one: the handler sees none");
    size_t sz = 99;
    CHECK(gp_action_payload(&cx, &sz) == NULL && sz == 0, "outside a handler there is no payload");
    int outer = 5;
    gp_dispatch_action_with(&cx, "tab::outer", &outer, sizeof outer);
    CHECK(fx->nested_outer_before == 5 && fx->nested_inner == 77 && fx->nested_outer_after == 5, "a handler that dispatches another action gets its own payload back: %d, inner %d, then %d", fx->nested_outer_before, fx->nested_inner, fx->nested_outer_after);

    /* parsing, and clearing */
    gp_clear_key_bindings(app);
    CHECK(gp_bind_key(app, "ctrl-e", "ed::sel", NULL) && !gp_bind_key(app, "", "x", NULL) && !gp_bind_key(app, "ctrl-a ctrl-b ctrl-c ctrl-d ctrl-e", "x", NULL) && !gp_bind_key(app, "ctrl-notamod-x", "x", NULL),
          "binding: a good keystroke; none, five and a bad modifier are refused");
    int before_clear = fx->comment;
    keys("ctrl-k ctrl-c");
    CHECK(fx->comment == before_clear, "after gp_clear_key_bindings the chord is gone");
    gp_debug_clock_skew(0);
}

static void driver(gp_co *co, void *self, gp_ctx *cx, void *data) {
    co_ = co;
    Fix *fx = gp_entity_get(app, win()->root_view);
    run(fx);
    gp_quit(app);
}

static void ready(gp_app *a, void *userdata) {
    app = a;
    Fix init = { .editor = gp_focus_new(app), .other = gp_focus_new(app) };
    gp_entity v = gp_view_new(app, sizeof(Fix), &init, render, NULL);
    static const int one = 1, two = 2;
    gp_bind_key(app, "ctrl-k ctrl-c", "ed::comment", "Editor");
    gp_bind_key(app, "ctrl-k ctrl-u", "ed::uncomment", "Editor");
    gp_bind_key(app, "g g", "ed::top", "Editor");
    gp_bind_key(app, "ctrl-k escape", "ed::top", "Editor");   /* never runs: escape cancels a chord */
    gp_bind_key(app, "ctrl-x ctrl-s ctrl-w", "ed::saveall", "Editor");
    gp_bind_key(app, "ctrl-j", "ed::jump", "Editor");
    gp_bind_key(app, "ctrl-j ctrl-j", "ed::jump2", "Editor");
    gp_bind_key(app, "ctrl-d", "a::one", "Editor");
    gp_bind_key(app, "ctrl-d", "a::two", "Editor");
    gp_bind_key(app, "ctrl-m ctrl-n", "x::nobody-handles-this", "Editor");
    gp_bind_key_with(app, "ctrl-1", "tab::select", NULL, &one, sizeof one);
    gp_bind_key_with(app, "ctrl-2", "tab::select", NULL, &two, sizeof two);
    gp_open_window(app, &(gp_window_options){ .title = "keys_test", .width = 240, .height = 180 }, v);
    gp_ctx cx = gp_ctx_for(app, GP_ENTITY_NULL);
    gp_spawn(&cx, driver, NULL);
}

int main(void) {
    setenv("GPUIC_IGNORE_REAL_MOUSE", "1", 1);
    int rc = gp_application_run(ready, NULL);
    CHECK(rc == 0, "application_run returned %d", rc);
    printf(failures ? "keys_test: %d FAILURE(S)\n" : "keys_test: all checks passed\n", failures);
    return failures != 0;
}
