/*
 * The C++ layer's scroll and focus handles, key bindings, actions and clipboard: each wrapper against what the C call does, driven
 * by simulated keys and laid-out trees. Needs a display (an event loop). Exits non-zero on failure.
 */
#include <gpuic/gpuic.hpp>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

using namespace gpuic;

extern "C" int cpp_probe_smooth_ms(gp_app* app);   // tests/cpp_probe.c
extern "C" const char* cpp_probe_list_first(gp_app* app, int idx, float* y);

static int failures;
#define CHECK(cond, ...)                                                               \
    do {                                                                               \
        if (!(cond)) {                                                                 \
            failures++;                                                                \
            std::printf("FAIL %s:%d: ", __FILE__, __LINE__);                           \
            std::printf(__VA_ARGS__);                                                  \
            std::printf("\n");                                                          \
        }                                                                              \
    } while (0)

static bool near(float a, float b, float eps = 0.6f) { return std::fabs(a - b) < eps; }

using Tok = std::shared_ptr<int>;

struct Page {
    Scroll box, list;
    Focus f1, f2, f3, fscroll;
    int saves = 0, selects = 0, last_select = 0, chords = 0, editor_only = 0, plain = 0;
    int list_rows = 1000;
    std::string hover_log;
    Scroll vs;
    std::vector<int> ids;   // the variable-height list's rows (an id each); the first ten are short
    int tall_id = -1;
    int height_of(int id) const { return id == tall_id ? 300 : (id >= 1000 || id < 10) ? 40 : 100; }

    Element render(Ctx) {
        Element scroller = div().h(100).w(200).flex_none().overflow_y_scroll().track_scroll(box).flex_col();
        for (int i = 0; i < 20; i++) scroller.child(div().h(30).flex_none().child(text("row " + std::to_string(i))));
        // an element deep in the scroller to be scrolled to by focus
        Element far = div().h(30).flex_none().track_focus(fscroll).tab_stop().child("focusable");
        scroller.child(std::move(far));
        return div().size_full().flex_col().gap_2().p_2().overflow_y_scroll()
            .child(std::move(scroller))
            .child(uniform_list((size_t)list_rows, 20, [](size_t i) { return div().h(20).child(text("item " + std::to_string(i))); }).h(100).w(200).flex_none().track_scroll(list))
            .child(gpuic::list(ids.size(), 60, [](Page& p, size_t i) { return div().h(p.height_of(p.ids[i])).child(text("id " + std::to_string(p.ids[i]))); }).h(200).w(200).flex_none().track_scroll(vs))
            .child(div().key_context("Editor").flex_row().gap_2()
                       .child(div().w(60).h(30).flex_none().bg(rgb(0x335577)).track_focus(f1).tab_stop().child("one")
                                  .on_action("page::save", [](Page& p) { p.saves++; })
                                  .on_action("page::select", [](Page& p, Ctx cx) {
                                      if (const int* n = cx.payload<int>()) { p.selects++; p.last_select = *n; }
                                  })
                                  .on_action("page::chord", [](Page& p) { p.chords++; })
                                  .on_action("page::editor", [](Page& p) { p.editor_only++; }))
                       .child(div().w(60).h(30).flex_none().bg(rgb(0x553377)).track_focus(f2).tab_stop().child("two"))
                       .child(div().w(60).h(30).flex_none().bg(rgb(0x775533)).track_focus(f3).tab_stop().child("three")
                                  .on_action("page::plain", [](Page& p) { p.plain++; })))
            // on_hover with a capture
            .child(div().absolute().left(300).top(10).w(80).h(40).bg(rgb(0x446644))
                       .on_hover([tag = 7](Page& p, bool inside) { p.hover_log += (inside ? "+" : "-") + std::to_string(tag); }));
    }
};

static Entity<Page> g_page;
static Tok g_global_tok;
static int g_global_calls;

static void settle(Co& co, double s = 0.05) { co.sleep(s); }

static void test_scroll(Co& co, Ctx cx) {
    App app = cx.app();
    Page& p = *g_page.get(app);
    Ctx c = app.ctx();
    CHECK(p.box && p.list && p.f1 && p.fscroll, "the handles were made");
    CHECK(!Scroll{} && !Focus{}, "default handles are null");

    gp_size vp = p.box.viewport(c);
    gp_point mx = p.box.max(c);
    CHECK(near(vp.h, 100) && near(mx.y, 20 * 30 + 30 - 100) && near(mx.x, 0), "viewport %g x %g, max %g,%g", vp.w, vp.h, mx.x, mx.y);
    CHECK(near(p.box.offset(c).y, 0), "starts at the top");
    p.box.to(c, 0, 50);
    settle(co);
    CHECK(near(p.box.offset(c).y, 50), "to: %g", p.box.offset(c).y);
    p.box.by(c, 0, 25);
    settle(co);
    CHECK(near(p.box.offset(c).y, 75), "by: %g", p.box.offset(c).y);
    p.box.by(c, 0, -500);
    settle(co);
    CHECK(near(p.box.offset(c).y, 0), "by past the top is clamped: %g", p.box.offset(c).y);
    p.box.to_end(c);
    settle(co);
    CHECK(near(p.box.offset(c).y, mx.y), "to_end: %g of %g", p.box.offset(c).y, mx.y);
    p.box.to(c, 0, 0);
    settle(co);

    // a list: scroll to an item, and the splice / invalidate / reset calls reach the list
    p.list.to_item(c, 500, RowAlign::Start);
    settle(co);
    CHECK(near(p.list.offset(c).y, 500 * 20), "list to_item start: %g", p.list.offset(c).y);
    p.list.to_item(c, 520, RowAlign::Center);
    settle(co);
    CHECK(near(p.list.offset(c).y, 520 * 20 + 10 - 50), "list to_item center: %g", p.list.offset(c).y);
    p.list.to_item(c, 521, RowAlign::Nearest);
    settle(co);
    CHECK(near(p.list.offset(c).y, 520 * 20 + 10 - 50), "list to_item nearest, in view: no move: %g", p.list.offset(c).y);
    p.list.to_item(c, 530, RowAlign::Nearest);
    settle(co);
    CHECK(near(p.list.offset(c).y, 531 * 20 - 100), "list to_item nearest, below: bottom edge: %g", p.list.offset(c).y);
    float max_before = p.list.max(c).y;
    p.list_rows = 1100;
    p.list.splice(c, 1000, 0, 100);   // a hundred rows appended (the splice is told, the view rebuilt)
    app.notify();
    settle(co, 0.1);
    CHECK(near(p.list.max(c).y, max_before + 100 * 20), "splice: the extent grew by 100 rows: %g -> %g", max_before, p.list.max(c).y);
    p.list.invalidate(c, 0, 10);
    p.list.reset(c);
    settle(co);
    p.list.to(c, 0, 0);
    settle(co);
    CHECK(near(p.list.offset(c).y, 0), "list to: %g", p.list.offset(c).y);
}

static void test_list_calls(Co& co, Ctx cx) {
    App app = cx.app();
    Page& p = *g_page.get(app);
    Ctx c = app.ctx();
    // the root scrolls so the third list is reachable; its handle works wherever it is
    // (the list is a variable-height one: rows [0, 10) are 40 high, the rest 100)
    for (int i = 0; i < 200; i++) p.ids.push_back(i);
    app.notify();
    settle(co, 0.1);
    // splice: rows are inserted above the first visible one, which stays where it is
    for (int k = 0; k < 20; k++) { p.vs.by(c, 0, 180); settle(co, 0.03); }
    p.vs.to(c, 0, 3000);
    settle(co, 0.1);
    float y0 = 0, y1 = 0;
    std::string t0 = cpp_probe_list_first(app, 2, &y0) ? cpp_probe_list_first(app, 2, &y0) : "";
    CHECK(!t0.empty(), "the list shows rows");
    for (int k = 0; k < 10; k++) p.ids.insert(p.ids.begin(), 1000 + k);
    p.vs.splice(c, 0, 0, 10);
    app.notify();
    settle(co, 0.15);
    const char* t1 = cpp_probe_list_first(app, 2, &y1);
    CHECK(t1 && t0 == t1 && std::fabs(y0 - y1) < 1.f, "splice: the top row stays '%s' at %g (now '%s' at %g)", t0.c_str(), y0, t1 ? t1 : "(none)", y1);

    // invalidate of rows nobody measured changes nothing; reset forgets what was measured and the extent follows the estimate
    float m0 = p.vs.max(c).y;
    p.vs.invalidate(c, 190, 5);
    settle(co, 0.1);
    CHECK(std::fabs(p.vs.max(c).y - m0) < 0.5f, "invalidate of rows never seen: the extent stays %g -> %g", m0, p.vs.max(c).y);
    p.vs.reset(c);
    settle(co, 0.1);
    CHECK(std::fabs(p.vs.max(c).y - m0) > 300.f, "reset: the extent is the estimate again: %g -> %g", m0, p.vs.max(c).y);
    p.vs.to(c, 0, 0);
    settle(co);
}

static void test_focus(Co& co, Ctx cx) {
    App app = cx.app();
    Page& p = *g_page.get(app);
    Ctx c = app.ctx();
    c.blur();
    settle(co);
    CHECK(!c.is_focused(p.f1) && !c.is_focused(p.f2), "nothing is focused after blur");
    c.focus(p.f2);
    settle(co);
    CHECK(c.is_focused(p.f2) && !c.is_focused(p.f1) && !c.is_focused(p.f3), "focus(f2)");
    c.focus_next();
    settle(co);
    CHECK(c.is_focused(p.f3), "focus_next goes to the next tab stop");
    c.focus_prev();
    settle(co);
    CHECK(c.is_focused(p.f2), "focus_prev goes back one");
    c.focus_prev();
    settle(co);
    CHECK(c.is_focused(p.f1), "focus_prev twice goes back to the first");
    c.blur();
    settle(co);
    CHECK(!c.is_focused(p.f1), "blur");

    // scroll_into_view: the last row of the scroller is below its edge; the call brings it in view
    Ctx k = app.ctx();
    p.box.to(k, 0, 0);
    settle(co);
    c.scroll_into_view(p.fscroll);
    settle(co, 0.1);
    CHECK(near(p.box.offset(k).y, p.box.max(k).y), "scroll_into_view reached the last element: %g of %g", p.box.offset(k).y, p.box.max(k).y);
    p.box.to(k, 0, 0);
    settle(co);
}

static void test_keys(Co& co, Ctx cx) {
    App app = cx.app();
    Page& p = *g_page.get(app);
    Ctx c = app.ctx();
    CHECK(app.bind_key("ctrl-s", "page::save"), "bind_key parses");
    CHECK(!app.bind_key("not a key at all %%", "page::save"), "a bad keystroke is refused");
    CHECK(app.bind_key("ctrl-1", "page::select", nullptr, 1) && app.bind_key("ctrl-2", "page::select", nullptr, 2), "bind_key with a payload");
    CHECK(app.bind_key("ctrl-k ctrl-c", "page::chord"), "a chord");
    CHECK(app.bind_key("ctrl-e", "page::editor", "Editor"), "a key bound in a context");
    CHECK(app.bind_key("ctrl-p", "page::plain", "Terminal"), "a key bound in a context that is not on the path");
    CHECK(app.bind_key("ctrl-3", "page::select", "Terminal", 3), "...with a payload too");

    c.focus(p.f1);
    settle(co);
    gp_simulate_keystrokes(app, "ctrl-s");
    settle(co);
    CHECK(p.saves == 1, "ctrl-s fires page::save once (%d)", p.saves);
    gp_simulate_keystrokes(app, "ctrl-1");
    settle(co);
    CHECK(p.selects == 1 && p.last_select == 1, "payload 1 (%d, %d)", p.selects, p.last_select);
    gp_simulate_keystrokes(app, "ctrl-2");
    settle(co);
    CHECK(p.selects == 2 && p.last_select == 2, "payload 2 (%d, %d)", p.selects, p.last_select);

    gp_simulate_keystrokes(app, "ctrl-k");
    settle(co);
    CHECK(std::string(c.keys_pending()) == "ctrl-k" && p.chords == 0, "after the first key of the chord: pending '%s'", c.keys_pending());
    gp_simulate_keystrokes(app, "ctrl-c");
    settle(co);
    CHECK(p.chords == 1 && std::string(c.keys_pending()).empty(), "the chord ran (%d), nothing pending", p.chords);

    gp_simulate_keystrokes(app, "ctrl-e");
    settle(co);
    CHECK(p.editor_only == 1, "the context 'Editor' is on the path: ctrl-e runs (%d)", p.editor_only);
    c.focus(p.f3);
    settle(co);
    gp_simulate_keystrokes(app, "ctrl-p");
    settle(co);
    CHECK(p.plain == 0, "'Terminal' is not on the path: ctrl-p does nothing (%d)", p.plain);
    c.focus(p.f1);
    settle(co);
    int sel = p.selects;
    gp_simulate_keystrokes(app, "ctrl-3");
    settle(co);
    CHECK(p.selects == sel, "...nor does ctrl-3 with its payload (%d)", p.selects);

    // dispatch: a handler on the focus path takes it (true), one nobody has is false; with a payload
    c.focus(p.f1);
    settle(co);
    CHECK(c.dispatch("page::save") && p.saves == 2, "dispatch runs the handler (%d)", p.saves);
    CHECK(c.dispatch("page::select", 7) && p.selects == 3 && p.last_select == 7, "dispatch with a payload (%d)", p.last_select);
    int before = p.selects;
    c.dispatch("page::select", 3.5);   // a double where the handler reads an int: no payload for it
    CHECK(p.selects == before, "a payload of another size is not an int (%d)", p.selects);
    CHECK(!c.dispatch("page::nobody"), "an action nobody handles is not taken");
    const int* none = c.payload<int>();
    CHECK(none == nullptr, "outside a handler there is no payload");

    // a global action: runs when nobody on the path handled it, with a closure destroyed once at the end
    g_global_tok = std::make_shared<int>(0);
    app.on_global_action("app::global", [t = g_global_tok](Ctx) { g_global_calls++; });
    CHECK(g_global_tok.use_count() == 2, "the app holds the closure");
    CHECK(c.dispatch("app::global") && g_global_calls == 1, "a global action runs (%d)", g_global_calls);
    c.focus(p.f3);
    settle(co);
    CHECK(c.dispatch("app::global") && g_global_calls == 2, "...from anywhere (%d)", g_global_calls);

    // clearing the bindings
    app.clear_key_bindings();
    c.focus(p.f1);
    settle(co);
    gp_simulate_keystrokes(app, "ctrl-s");
    settle(co);
    CHECK(p.saves == 2, "after clear_key_bindings ctrl-s does nothing (%d)", p.saves);
}

static void test_clipboard(Co& co, Ctx cx) {
    App app = cx.app();
    app.set_clipboard("héllo clipboard");
    auto got = app.clipboard();
    CHECK(got && *got == "héllo clipboard", "clipboard round trip: '%s'", got ? got->c_str() : "(none)");
    app.set_primary("selected text");
    auto prim = app.primary();
    CHECK(prim && *prim == "selected text", "primary round trip: '%s'", prim ? prim->c_str() : "(none)");
    app.set_clipboard("second");
    CHECK(app.clipboard() && *app.clipboard() == "second" && *app.primary() == "selected text", "they are separate");
}

static void test_smooth(Co& co, Ctx cx) {
    App app = cx.app();
    app.set_smooth_scrolling(0.25f);
    CHECK(cpp_probe_smooth_ms(app) == 250, "set_smooth_scrolling(0.25): %d ms", cpp_probe_smooth_ms(app));
    app.set_smooth_scrolling(0);
    CHECK(cpp_probe_smooth_ms(app) == 0, "...and off: %d ms", cpp_probe_smooth_ms(app));
}

static void test_hover(Co& co, Ctx cx) {
    App app = cx.app();
    Page& p = *g_page.get(app);
    gp_simulate_mouse(app, GP_SIM_MOVE, 340, 30, 0, 0);
    settle(co, 0.1);
    gp_simulate_mouse(app, GP_SIM_MOVE, 410, 510, 0, 0);
    settle(co, 0.1);
    CHECK(p.hover_log == "+7-7", "on_hover with a captured tag: '%s'", p.hover_log.c_str());
}

static void driver(Co& co, Ctx cx) {
    co.sleep(0.2);
    test_scroll(co, cx);
    test_list_calls(co, cx);
    test_focus(co, cx);
    test_keys(co, cx);
    test_clipboard(co, cx);
    test_smooth(co, cx);
    test_hover(co, cx);
    cx.app().quit();
}

int main() {
    setenv("GPUIC_IGNORE_REAL_MOUSE", "1", 1);
    int rc = run([](App app) {
        g_page = app.new_view<Page>();
        Page& p = *g_page.get(app);
        p.box = app.new_scroll();
        p.list = app.new_scroll();
        p.vs = app.new_scroll();
        p.f1 = app.new_focus();
        p.f2 = app.new_focus();
        p.f3 = app.new_focus();
        p.fscroll = app.new_focus();
        app.open_window("cpp_input_test", 420, 520, g_page);
        app.ctx().spawn([](Co& co, Ctx cx) { driver(co, cx); });
    });
    CHECK(rc == 0, "run returned %d", rc);
    CHECK(g_global_tok.use_count() == 1, "the global action's closure was destroyed with the app (use_count %ld)", g_global_tok.use_count());
    std::printf(failures ? "cpp_input_test: %d FAILURE(S)\n" : "cpp_input_test: all checks passed\n", failures);
    return failures != 0;
}
