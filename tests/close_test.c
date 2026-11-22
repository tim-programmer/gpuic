/*
 * gp_window_on_close_request: the close button asks the callback first; false keeps the window open, true closes it, gp_window_close
 * never asks, no callback closes as before, and the callback's data is dropped once when the window goes. Needs a display.
 */
#include "../src/internal.h"

#include <stdio.h>

static int failures;
#define CHECK(cond, ...)                                \
    do {                                                \
        if(!(cond)) {                                   \
            failures++;                                 \
            printf("FAIL %s:%d: ", __FILE__, __LINE__); \
            printf(__VA_ARGS__);                        \
            printf("\n");                               \
        }                                               \
    } while(0)

typedef struct
{
    int asked, dropped;
    bool answer;
    gp_entity view; /* cx->entity seen */
} Ask;

static bool ask(void* self, gp_ctx* cx, void* data)
{
    Ask* a = data;
    a->asked++;
    a->view = cx->entity;
    return a->answer;
}
static void drop(void* data)
{
    ((Ask*)data)->dropped++;
}
static gp_node* render(void* self, gp_ctx* cx)
{
    return gp_div(gp_size_full());
}

static bool is_open(gp_app* app, gp_window* w)
{
    for(int i = 0; i < app->nwindows; i++) {
        if(app->windows[i] == w) {
            return true;
        }
    }
    return false;
}

int main(void)
{
    gp_app* app = gp_app_new();
    if(!app) {
        printf("close_test: no display\n");
        return 1;
    }
    gp_entity v = gp_view_new(app, 1, &(char){ 0 }, render, NULL);
    gp_window* keep = gp_open_window(app, &(gp_window_options){ .title = "close_test", .width = 100, .height = 100 }, v); /* keeps the app alive */
    gp_window_options o = { .title = "close_test 2", .width = 100, .height = 100 };

    /* false: kept open, asked once a press; then gp_window_close closes it without asking */
    Ask a = { 0 };
    gp_window* w = gp_open_window(app, &o, v);
    gp_window_on_close_request(w, ask, &a, drop);
    gp_simulate_close_request(w);
    gp_app_step(app, NULL);
    CHECK(a.asked == 1 && is_open(app, w) && a.view.index == v.index, "false keeps it open: asked %d, open %d", a.asked, is_open(app, w));
    gp_simulate_close_request(w);
    gp_app_step(app, NULL);
    CHECK(a.asked == 2 && is_open(app, w), "asked again on the next press: %d", a.asked);
    gp_window_close(w);
    gp_app_step(app, NULL);
    CHECK(a.asked == 2 && !is_open(app, w) && a.dropped == 1, "gp_window_close closes without asking: asked %d, open %d, dropped %d", a.asked, is_open(app, w), a.dropped);

    /* true: closes */
    Ask b = { .answer = true };
    w = gp_open_window(app, &o, v);
    gp_window_on_close_request(w, ask, &b, drop);
    gp_simulate_close_request(w);
    gp_app_step(app, NULL);
    CHECK(b.asked == 1 && !is_open(app, w) && b.dropped == 1, "true closes it: asked %d, open %d, dropped %d", b.asked, is_open(app, w), b.dropped);

    /* replaced: the old data is dropped; NULL: closes as without one */
    Ask c = { 0 };
    w = gp_open_window(app, &o, v);
    gp_window_on_close_request(w, ask, &c, drop);
    gp_window_on_close_request(w, NULL, NULL, NULL);
    CHECK(c.dropped == 1, "replacing it drops the old data: %d", c.dropped);
    gp_simulate_close_request(w);
    gp_app_step(app, NULL);
    CHECK(c.asked == 0 && !is_open(app, w), "no callback: closes as before");

    /* the app ending does not ask, and drops the data */
    Ask d = { 0 };
    gp_window_on_close_request(keep, ask, &d, drop);
    gp_app_free(app);
    CHECK(d.asked == 0 && d.dropped == 1, "the app's end: asked %d, dropped %d", d.asked, d.dropped);

    printf(failures ? "close_test: %d FAILURE(S)\n" : "close_test: all checks passed\n", failures);
    return failures != 0;
}
