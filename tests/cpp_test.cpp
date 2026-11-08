/*
 * The C++ layer (include/gpuic/gpuic.hpp). The same interface is built twice, once with the C macros and once with the chained C++
 * methods, side by side in one window; the laid-out trees (kinds, sizes, positions, texts) and the pixels of the two halves must be
 * identical. The Tailwind-style spellings are checked against the numbers they mean, written out here. Then the handlers: lambdas
 * with every sort of parameter list, captures, C function pointers, keys, text, actions, layout and animation callbacks run from
 * simulated input. Then state: entities and views with non-trivial members (constructed, destroyed once, typed access), a view
 * as a child, a view given a lambda for render. Needs a display.
 */
#include <gpuic/gpuic.hpp>

#include <unordered_map>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <utility>
#include <vector>

extern "C" {
int cpp_probe_children(gp_app* app, const int* path, int depth);
int cpp_probe_rect(gp_app* app, const int* path, int depth, float out[4]);
const char* cpp_probe_text(gp_app* app, const int* path, int depth);
const char* cpp_probe_same(gp_app* app, const int* pa, int da, const int* pb, int db, char* why, size_t cap);
const unsigned char* cpp_probe_capture(gp_app* app, gp_co* co, int* w, int* h);
unsigned long cpp_probe_draws(gp_app* app);
void cpp_probe_repaint(gp_app* app);
void cpp_probe_rebuild(gp_app* app);
float cpp_probe_style(gp_app* app, const int* path, int depth, int field);
void cpp_probe_wheel(gp_app* app, float x, float y, float dx, float dy);
const char* cpp_probe_sticky_text(gp_app* app, const int* path, int depth);
const char* cpp_probe_el_same(gp_app* app, int ia, int ib, char* why, size_t cap);
}

using namespace gpuic;

static int failures;
#define CHECK(cond, ...)                                                               \
    do {                                                                               \
        if (!(cond)) {                                                                 \
            if (failures++ < 60) {                                                     \
                std::printf("FAIL %s:%d: ", __FILE__, __LINE__);                       \
                std::printf(__VA_ARGS__);                                              \
                std::printf("\n");                                                     \
            }                                                                          \
        }                                                                              \
    } while (0)

static gp_app* g_app;
static gp_co* g_co;

// ---- the types the chain returns: a named element by reference, a temporary as a temporary
static_assert(std::is_same_v<decltype(std::declval<Element&>().flex_row()), Element&>);
static_assert(std::is_same_v<decltype(div().flex_row()), Element&&>);
static_assert(std::is_same_v<decltype(div().child("x")), Element&&>);
static_assert(std::is_convertible_v<Element, gp_node*>);

// ---------------------------------------------------------------- state

static int g_free_calls, g_cdata_seen, g_cptr_hits, g_layouts;
static gp_size g_layout_size;
static void c_click(void* self, gp_ctx* cx, const gp_mouse_event* ev, void* data) { g_cdata_seen = *static_cast<int*>(data); }

struct Child {
    int value = 1;
    int renders = 0;
    Element render(Ctx) {
        renders++;
        return div().w(50).h(14).child(text("child " + std::to_string(value)));
    }
};
struct Plain { int v; };

struct Fix {
    App app;
    gp_focus focus;
    Entity<Child> child;
    Entity<Plain> plain;
    int n1 = 0, n2 = 0, n3 = 0, total = 0, ups = 0, downs = 0, actions = 0;
    float lastx = -1;
    bool ctx_ok = false;
    bool flag = true;
    std::string keys, ups_k, typed, ime;
    int drags = 0, ime_cursor = -1, scrolls = 0;
    gp_image solid;

    explicit Fix(App a) : app(a), focus(gp_focus_new(a.raw())), child(a.new_view<Child>()),
          plain(a.new_view_with<Plain>([](Plain& p, Ctx) { return div().w(50).h(14).child(text(std::to_string(p.v))); }, Plain{ 5 })) {
        unsigned char px[4 * 4 * 4];
        for (int i = 0; i < 16; i++) { px[i * 4] = 0; px[i * 4 + 1] = 160; px[i * 4 + 2] = 0; px[i * 4 + 3] = 255; }
        solid = gp_image_from_rgba(a.raw(), 4, 4, px);
    }

    Element render(Ctx cx);
};

#define PAGE rgb(0x14141e)
#define C1 rgb(0x304060)
#define C2 rgb(0xe0b040)
#define RED rgb(0xc03030)

// the C half of each pair is made here with the C API, the C++ half with the chain
static gp_node* c_box(float w, float h, gp_color c) { return gp_div(gp_w(w), gp_h(h), gp_bg(c)); }
static Element box(float w, float h, Color c) { return div().w(w).h(h).bg(c); }
static gp_node* c_row_fn(void*, gp_ctx*, size_t i, void*) { return gp_div(gp_h(20), gp_text(gp_fmt("row %zu", i))); }


// ---------------------------------------------------------------- the spellings, against the numbers they mean (written out here)

enum Field { F_GAP, F_PL, F_PT, F_PR, F_PB, F_ML, F_MT, F_MR, F_MB, F_W, F_W_KIND, F_H, F_H_KIND, F_MIN_W, F_MIN_H, F_MAX_W, F_MAX_H,
             F_LEFT, F_TOP, F_RIGHT, F_BOTTOM, F_RADIUS, F_TEXT_SIZE, F_WEIGHT, F_BORDER, F_SH_DX, F_SH_DY, F_SH_BLUR, F_SH_ALPHA,
             F_BL, F_BT, F_BR, F_BB, F_BSET };
// (the order of F_PL.. is the order of the probe's codes: gap 0, pad 1-4, margin 5-8, w 9, w kind 10, h 11, h kind 12, min/max 13-16, insets 17-20, ...)

struct Case { const char* name; Element (*make)(); int field; float want; };
// every case twice: on a temporary (the && overloads) and on a named element (the & ones)
#define CASE(expr, field, want) { #expr, [] { return div().expr; }, field, (float)(want) }, { #expr " (named)", [] { Element e = div(); e.expr; return e; }, field, (float)(want) },
// Tailwind's steps, 4 px each
#define STEPS(X) X(0, 0) X(px, 1) X(0p5, 2) X(1, 4) X(1p5, 6) X(2, 8) X(2p5, 10) X(3, 12) X(3p5, 14) X(4, 16) X(5, 20) X(6, 24) X(7, 28) X(8, 32) X(9, 36) \
                 X(10, 40) X(11, 44) X(12, 48) X(14, 56) X(16, 64) X(20, 80) X(24, 96) X(28, 112) X(32, 128) X(36, 144) X(40, 160) X(44, 176) \
                 X(48, 192) X(52, 208) X(56, 224) X(60, 240) X(64, 256) X(72, 288) X(80, 320) X(96, 384)
#define GAP_CASE(s, px) CASE(gap_##s(), F_GAP, px)
#define P_CASE(s, px) CASE(p_##s(), F_PL, px) CASE(p_##s(), F_PT, px) CASE(p_##s(), F_PR, px) CASE(p_##s(), F_PB, px)
#define W_CASE(s, px) CASE(w_##s(), F_W, px) CASE(w_##s(), F_W_KIND, 1)
#define FRACTION_CASES(prefix, f, k) CASE(prefix##_1_2(), f, 0.5) CASE(prefix##_1_3(), f, 1.0 / 3) CASE(prefix##_2_3(), f, 2.0 / 3) CASE(prefix##_1_4(), f, 0.25) \
    CASE(prefix##_3_4(), f, 0.75) CASE(prefix##_1_5(), f, 0.2) CASE(prefix##_2_5(), f, 0.4) CASE(prefix##_3_5(), f, 0.6) CASE(prefix##_4_5(), f, 0.8) \
    CASE(prefix##_1_2(), k, 2) CASE(prefix##_4_5(), k, 2)
#define ROUNDED_CASE(s, px) CASE(rounded_##s(), F_RADIUS, px)
#define TEXT_CASE(s, px) CASE(text_##s(), F_TEXT_SIZE, px)
#define WEIGHT_CASE(s, w) CASE(font_##s(), F_WEIGHT, w)

static const Case kCases[] = {
    STEPS(GAP_CASE) STEPS(P_CASE) STEPS(W_CASE)
    // one step (16 px) and the smallest (2 px) of every other prefix: the point is that each is wired to its own C function
    CASE(px_4(), F_PL, 16) CASE(px_4(), F_PR, 16) CASE(px_4(), F_PT, 0) CASE(py_4(), F_PT, 16) CASE(py_4(), F_PB, 16) CASE(py_4(), F_PL, 0)
    CASE(pt_4(), F_PT, 16) CASE(pt_4(), F_PB, 0) CASE(pr_4(), F_PR, 16) CASE(pr_4(), F_PL, 0) CASE(pb_4(), F_PB, 16) CASE(pb_4(), F_PT, 0)
    CASE(pl_4(), F_PL, 16) CASE(pl_4(), F_PR, 0) CASE(pl_0p5(), F_PL, 2)
    CASE(m_4(), F_ML, 16) CASE(m_4(), F_MT, 16) CASE(m_4(), F_MR, 16) CASE(m_4(), F_MB, 16)
    CASE(mx_4(), F_ML, 16) CASE(mx_4(), F_MR, 16) CASE(mx_4(), F_MT, 0) CASE(my_4(), F_MT, 16) CASE(my_4(), F_MB, 16) CASE(my_4(), F_ML, 0)
    CASE(mt_4(), F_MT, 16) CASE(mt_4(), F_MB, 0) CASE(mr_4(), F_MR, 16) CASE(mr_4(), F_ML, 0) CASE(mb_4(), F_MB, 16) CASE(mb_4(), F_MT, 0)
    CASE(ml_4(), F_ML, 16) CASE(ml_4(), F_MR, 0) CASE(ml_0p5(), F_ML, 2)
    CASE(h_4(), F_H, 16) CASE(h_4(), F_H_KIND, 1) CASE(h_0p5(), F_H, 2) CASE(size_4(), F_W, 16) CASE(size_4(), F_H, 16) CASE(size(7), F_W, 7) CASE(size(7), F_H, 7)
    CASE(min_w_4(), F_MIN_W, 16) CASE(min_h_4(), F_MIN_H, 16) CASE(max_w_4(), F_MAX_W, 16) CASE(max_h_4(), F_MAX_H, 16) CASE(min_w_96(), F_MIN_W, 384)
    CASE(left_4(), F_LEFT, 16) CASE(top_4(), F_TOP, 16) CASE(right_4(), F_RIGHT, 16) CASE(bottom_4(), F_BOTTOM, 16)
    CASE(inset_4(), F_LEFT, 16) CASE(inset_4(), F_TOP, 16) CASE(inset_4(), F_RIGHT, 16) CASE(inset_4(), F_BOTTOM, 16)
    FRACTION_CASES(w, F_W, F_W_KIND) FRACTION_CASES(h, F_H, F_H_KIND)
    ROUNDED_CASE(none, 0) ROUNDED_CASE(sm, 2) ROUNDED_CASE(md, 6) ROUNDED_CASE(lg, 8) ROUNDED_CASE(xl, 12) ROUNDED_CASE(2xl, 16) ROUNDED_CASE(3xl, 24)
    TEXT_CASE(xs, 12) TEXT_CASE(sm, 14) TEXT_CASE(base, 16) TEXT_CASE(lg, 18) TEXT_CASE(xl, 20) TEXT_CASE(2xl, 24) TEXT_CASE(3xl, 30) TEXT_CASE(4xl, 36)
    WEIGHT_CASE(thin, 100) WEIGHT_CASE(extralight, 200) WEIGHT_CASE(light, 300) WEIGHT_CASE(medium, 500) WEIGHT_CASE(semibold, 600)
    WEIGHT_CASE(bold, 700) WEIGHT_CASE(extrabold, 800) WEIGHT_CASE(black, 900)
    CASE(border_0(), F_BORDER, 0) CASE(border_1(), F_BORDER, 1) CASE(border_2(), F_BORDER, 2) CASE(border_4(), F_BORDER, 4) CASE(border_8(), F_BORDER, 8)
    CASE(border_t_2(), F_BT, 2) CASE(border_t_2(), F_BL, 0) CASE(border_r_4(), F_BR, 4) CASE(border_b_1(), F_BB, 1) CASE(border_l_8(), F_BL, 8)
    CASE(border_x_2(), F_BL, 2) CASE(border_x_2(), F_BR, 2) CASE(border_x_2(), F_BT, 0) CASE(border_y_4(), F_BT, 4) CASE(border_y_4(), F_BB, 4)
    CASE(shadow_sm(), F_SH_DY, 1) CASE(shadow_sm(), F_SH_BLUR, 3) CASE(shadow_sm(), F_SH_ALPHA, 26 / 255.0)
    CASE(shadow_md(), F_SH_DY, 4) CASE(shadow_md(), F_SH_BLUR, 8) CASE(shadow_md(), F_SH_ALPHA, 36 / 255.0)
    CASE(shadow_lg(), F_SH_DY, 10) CASE(shadow_lg(), F_SH_BLUR, 16) CASE(shadow_lg(), F_SH_ALPHA, 41 / 255.0)
    CASE(shadow_xl(), F_SH_DY, 20) CASE(shadow_xl(), F_SH_BLUR, 25) CASE(shadow_xl(), F_SH_ALPHA, 46 / 255.0)
    CASE(shadow_2xl(), F_SH_DY, 25) CASE(shadow_2xl(), F_SH_BLUR, 50) CASE(shadow_2xl(), F_SH_ALPHA, 64 / 255.0)
    CASE(shadow_md().shadow_none(), F_SH_BLUR, 0) CASE(shadow_md().shadow_none(), F_SH_ALPHA, 0)
    CASE(gap(9).flex().relative().whitespace_normal(), F_GAP, 9)
};
static constexpr int kNumCases = (int)(sizeof kCases / sizeof *kCases);

// every pass-through modifier of the C header: the chained method and the C function, same arguments (generated, see tools/gen_cpp_chain.py)
struct PassThrough { const char* name; Element (*cpp)(); gp_node* (*c)(); };
static const PassThrough kPass[] = {
#include "cpp_passthrough.inc"
    // written by hand in gpuic.hpp: the state styles take a function that makes a style()
#define STATE_ROW(cpp_name, c_name, extra_c)                                                                                                                   \
    { #cpp_name, [] { return div().cpp_name([](Element s) { return s.bg(rgb(0xff0000)).opacity(0.5f); }); },                                                  \
      [] { return gp_div(c_name(gp_group(gp_bg(gp_rgb(0xff0000)), gp_opacity(0.5f)))); } },                                                                    \
    { #cpp_name " (named, by reference, returning nothing)", [] { Element e = div(); e.cpp_name([](Element& s) { s.bg(rgb(0xff0000)).opacity(0.5f); }); return e; }, \
      [] { return gp_div(c_name(gp_group(gp_bg(gp_rgb(0xff0000)), gp_opacity(0.5f)))); } },                                                                    \
    { #cpp_name " (rvalue reference)", [] { return div().cpp_name([](Element&& s) { return std::move(s).bg(rgb(0xff0000)).opacity(0.5f); }); },                \
      [] { return gp_div(c_name(gp_group(gp_bg(gp_rgb(0xff0000)), gp_opacity(0.5f)))); } },
    STATE_ROW(hover, gp_hover_style, 0) STATE_ROW(active, gp_active_style, 0) STATE_ROW(focus_style, gp_focus_style, 0)
};
static constexpr int kNumPass = (int)(sizeof kPass / sizeof *kPass);

struct Pair { const char* name; gp_node* c; Element cpp; };

static SharedString g_shared("shared text");
static std::vector<Pair> pairs(Fix& f) {
    std::vector<Pair> p;
    const gp_color_stop stops[2] = { { C1, 0 }, { C2, 0.6f } };

    p.push_back({ "layout", gp_div(gp_size_full(), gp_flex_row(), gp_gap(8), gp_p(6), gp_bg(C1), gp_items_center(), gp_justify_between(), gp_rounded(6),
                                   c_box(20, 20, RED), c_box(20, 20, RED), c_box(20, 20, RED)),
                  div().size_full().flex_row().gap_2().p_1p5().bg(C1).items_center().justify_between().rounded_md()
                      .children(std::vector<Element>{ div().w_5().h_5().bg(RED), div().w_5().h_5().bg(RED), div().w_5().h_5().bg(RED) }) });
    p.push_back({ "text", gp_div(gp_p(10), gp_text("Hello", gp_text_size(18), gp_font_bold(), gp_text_color(C2))),
                  div().p_2p5().child(text("Hello").text_lg().font_bold().text_color(C2)) });
    p.push_back({ "wrap and align-content",
                  gp_div(gp_size_full(), gp_flex_row(), gp_flex_wrap(), gp_align_content_center(), gp_gap(4),
                         c_box(60, 15, RED), c_box(60, 15, C1), gp_div(gp_w(60), gp_h(15), gp_bg(C2), gp_ml_auto()), c_box(80, 15, RED), c_box(90, 15, C1)),
                  div().size_full().flex_row().flex_wrap().align_content_center().gap_1()
                      .child(box(60, 15, RED)).child(box(60, 15, C1)).child(box(60, 15, C2).ml_auto()).child(box(80, 15, RED)).child(box(90, 15, C1)) });
    p.push_back({ "gradient and corners",
                  gp_div(gp_size_full(), gp_bg_linear_gradient(45, stops, 2), gp_rounded_corners(10, 0, 10, 0)),
                  div().size_full().bg_gradient(45, { stop(C1, 0), stop(C2, 0.6f) }).rounded_corners(10, 0, 10, 0) });
    p.push_back({ "when",
                  gp_div(gp_p(4), gp_bg(C1), f.flag ? gp_border(2) : nullptr, !f.flag ? gp_bg(RED) : nullptr, f.flag ? gp_text_color(C2) : nullptr, c_box(30, 30, RED), gp_text("x")),
                  div().p_1().bg(C1).when(f.flag, [](Element e) { return e.border_2(); }).when(!f.flag, [](Element e) { return e.bg(RED); })
                      .when_some(std::optional<Color>(C2), [](Element e, Color c) { return e.text_color(c); }).when_some(std::optional<Color>(), [](Element e, Color c) { return e.bg(RED); }).map([](Element e) { return e.child(box(30, 30, RED)); })
                      .child("x") });
    p.push_back({ "the last one wins", gp_div(gp_p(16), gp_p(8), gp_bg(C1), gp_bg(C2), c_box(20, 20, RED)),
                  div().p_4().p_2().bg(C1).bg(C2).child(box(20, 20, RED)) });
    p.push_back({ "border, shadow, margin, corners",
                  gp_div(gp_m(10), gp_w(100), gp_h(40), gp_bg(C1), gp_border(2), gp_border_color(C2), gp_shadow(0, 4, 8, gp_rgba(0x00000024)), gp_rounded(8)),
                  div().m_2p5().w(100).h_10().bg(C1).border_2().border_color(C2).shadow_md().rounded_lg() });
    p.push_back({ "fractions", gp_div(gp_flex_row(), gp_size_full(), gp_div(gp_w_frac(0.5f), gp_h_frac(0.5f), gp_bg(RED)), gp_div(gp_w_frac(1.0f / 3), gp_h_frac(0.75f), gp_bg(C2))),
                  div().flex_row().size_full().child(div().w_1_2().h_1_2().bg(RED)).child(div().w_1_3().h_3_4().bg(C2)) });
    p.push_back({ "strings", gp_div(gp_font_family("sans-serif"), gp_text("a"), gp_text("b"), gp_text("c"), gp_text("d"), gp_text("e")),
                  div().font_family(std::string("sans-serif")).child("a").child(std::string("b")).child(std::string_view("c")).child(nullptr).child(text("d"))
                      .children(std::vector<std::string>{ "e" }) });
    p.push_back({ "uniform list", gp_uniform_list(5, 20, c_row_fn, nullptr, gp_size_full()),
                  uniform_list(5, 20, [](size_t i) { return div().h(20).child(text("row " + std::to_string(i))); }).size_full() });
    p.push_back({ "image", gp_div(gp_p(6), gp_img(f.solid, gp_size_px(30), gp_rounded(5))),
                  div().p_1p5().child(img(f.solid).size(30).rounded(5)) });
    p.push_back({ "style and refine", gp_div(gp_p(8), gp_bg(C1), gp_rounded(4), c_box(20, 20, RED)),
                  div().refine(style().p_2().bg(C1).rounded(4)).child(box(20, 20, RED)) });
    {
        Element base = div().w(30).h(30);
        Element a = base, b = base;   // both go on from the same place
        a.bg(RED);
        b.p_0p5();
        p.push_back({ "a copy is its own", gp_div(gp_flex_row(), gp_gap(4), gp_div(gp_w(30), gp_h(30)), gp_div(gp_w(30), gp_h(30), gp_bg(RED)), gp_div(gp_w(30), gp_h(30), gp_p(2))),
                      div().flex_row().gap_1().child(base).child(a).child(b) });
    }
    {
        const gp_color_stop three[3] = { { C1, 0 }, { C2, 0.5f }, { RED, 1 } };
        auto g = [&](gp_node* m) { return gp_div(gp_w(40), gp_h(60), m); };
        {
        const gp_color_stop two[2] = { { C1, 0 }, { C2, 1 } };
        p.push_back({ "borders: gradient, sides and dashes",
                      gp_div(gp_size_full(), gp_flex_row(), gp_gap(4), gp_div(gp_w(60), gp_h(60), gp_border(4), gp_border_linear_gradient(45, two, 2), gp_rounded(6)),
                             gp_div(gp_w(60), gp_h(60), gp_border(4), gp_border_color_top(RED), gp_border_color_left(C2), gp_border_dashed(), gp_border_color_bottom(C1)),
                             gp_div(gp_w(30), gp_h(30), gp_border(3), gp_border_color(C2), gp_border_dotted(), gp_border_conic_gradient(0.5f, 0.5f, 10, two, 2))),
                      div().size_full().flex_row().gap_1()
                          .child(div().w(60).h(60).border_4().border_gradient(45, { stop(C1, 0), stop(C2, 1) }).rounded_md())
                          .child(div().w(60).h(60).border_4().border_color_top(RED).border_color_left(C2).border_dashed().border_color_bottom(C1))
                          .child(div().w(30).h(30).border(3).border_color(C2).border_dotted().border_conic(0.5f, 0.5f, 10, { stop(C1, 0), stop(C2, 1) })) });
    }
    p.push_back({ "the other gradients",
                      gp_div(gp_flex_row(), gp_gap(4), g(gp_bg_radial_gradient(0.5f, 0.5f, three, 3)), g(gp_bg_repeating_gradient(30, three, 3)),
                             g(gp_bg_repeating_radial_gradient(0.3f, 0.6f, three, 3)), g(gp_bg_conic_gradient(0.5f, 0.5f, 90, three, 3))),
                      div().flex_row().gap_1().child(div().w(40).h(60).bg_radial(0.5f, 0.5f, { stop(C1, 0), stop(C2, 0.5f), stop(RED, 1) }))
                          .child(div().w(40).h(60).bg_repeating(30, { stop(C1, 0), stop(C2, 0.5f), stop(RED, 1) }))
                          .child(div().w(40).h(60).bg_repeating_radial(0.3f, 0.6f, { stop(C1, 0), stop(C2, 0.5f), stop(RED, 1) }))
                          .child(div().w(40).h(60).bg_conic(0.5f, 0.5f, 90, { stop(C1, 0), stop(C2, 0.5f), stop(RED, 1) })) });
    }
    p.push_back({ "list", gp_list(30, 20, c_row_fn, nullptr, gp_size_full()),
                  list(30, 20, [](size_t i) { return div().h(20).child(text("row " + std::to_string(i))); }).size_full() });
    p.push_back({ "C nodes inside", gp_div(gp_p(4), gp_div(gp_w(30), gp_h(30), gp_bg(RED)), gp_bg(C1)),
                  div().p_1().child(gp_div(gp_w(30), gp_h(30), gp_bg(RED))).with(gp_bg(C1)) });
    p.push_back({ "the scale", gp_div(gp_flex_row(), gp_gap(12), gp_pl(20), gp_pt(4), gp_pr(2), gp_pb(6), gp_min_h(24), gp_max_w(192), gp_bg(C1),
                                      gp_div(gp_w(30), gp_h(30), gp_mx(8), gp_my(2), gp_bg(RED)), gp_div(gp_w(28), gp_h(10), gp_ml(4), gp_bg(C2))),
                  div().flex_row().gap_3().pl_5().pt_1().pr_0p5().pb_1p5().min_h_6().max_w_48().bg(C1)
                      .child(div().w(30).h(30).mx_2().my_0p5().bg(RED)).child(div().w_7().h_2p5().ml_1().bg(C2)) });
    p.push_back({ "a shared string", gp_div(gp_flex_col(), gp_text("shared text"), gp_text("")),
                  div().flex_col().child(g_shared).child(SharedString()) });
    p.push_back({ "right to left", gp_div(gp_size_full(), gp_flex_row(), gp_text_dir_rtl(), c_box(30, 30, RED), c_box(50, 20, C2)),
                  div().size_full().flex_row().text_dir_rtl().children({ box(30, 30, RED), box(50, 20, C2) }) });
    return p;
}

static constexpr int kHandlerBoxes = 17;   // the boxes after the pairs, before the style carriers
static constexpr int kCellW = 200, kCellH = 70, kPairs = 20;

Element Fix::render(Ctx) {
    std::vector<Pair> ps = pairs(*this);
    Element root = div().size_full().flex_row().flex_wrap().align_content_start().items_start().gap_2().p_2().bg(PAGE).text_color(white());
    auto cell = [](gp_node* inner) { return div().w(kCellW).h(kCellH).flex_none().overflow_hidden().bg(rgb(0x1c1c2a)).child(inner); };
    for (Pair& pr : ps) root.child(div().flex_row().gap_2().flex_none().child(cell(pr.c)).child(cell(pr.cpp)));

    // ---- handler boxes, each 60 x 30, in the order the test clicks them
    auto hb = [](Color c) { return div().w(60).h(30).flex_none().bg(c); };
    int add = 5;
    int* hits = &g_cptr_hits;
    static int cdata = 77;
    root.child(hb(C1).on_click([](Fix& s) { s.n1++; }))                                                   // 0
        .child(hb(C1).on_click([](Fix& s, Ctx cx) { s.n2++; cx.notify(); }))                              // 1
        .child(hb(C1).on_click([](const gp_mouse_event* ev, Fix& s) { s.lastx = ev->local.x; }))           // 2 (the event by pointer)
        .child(hb(C1).on_click([add, hits](Fix& s) { s.total += add; ++*hits; }))                         // 3
        .child(hb(C1).on_click([](gp_ctx* cx, Fix& s) { s.ctx_ok = cx != nullptr && cx->app != nullptr; s.n3++; }))   // 4
        .child(hb(C1).on_click([](gp_mouse_event ev) { g_free_calls += ev.click_count; }))                // 5 (the event by value, no state)
        .child(hb(C1).on_click(c_click, &cdata))                                                          // 6
        .child(hb(C1).on_mouse_down([add](Fix& s) { s.downs += add; }).on_mouse_up([](Fix& s) { s.ups++; }).on_drag([](Fix& s) { s.drags++; }))   // 7
        .child(hb(C2).track_focus(focus).tab_stop().key_context("CppBox")                                 // 8: keys, text, actions
                   .on_key_down([](Fix& s, const gp_key_event& ev) { s.keys += ev.key; s.keys += ';'; })
                   .on_key_up([](const gp_key_event& ev, Fix& s) { s.ups_k += ev.key; })
                   .on_ime([](Fix& s, std::string_view pre, int cursor) { s.ime = std::string(pre); s.ime_cursor = cursor; })
                   .on_text_input([](Fix& s, std::string_view t) { s.typed += t; })
                   .on_action(std::string("cpp::bump"), [](Fix& s, Ctx) { s.actions++; }))
        .child(hb(C1).on_layout([](gp_size sz) { g_layout_size = sz; g_layouts++; }))                     // 9
        .child(hb(C1).with_animation("cpp-anim", animation(0), [](float t) { return style().w(60 + 17 * t); }))   // 10
        .child(hb(C1).with_animation("cpp-anim2", animation(0), [target = 91.f](float t) { return style().h(target * t); }))   // 11
        .child(child.element().w(80).h(14))                                                                // 12 (a view as a child)
        .child(plain)                                                                                      // 13 (given as an Entity)
        .child(div().w(60).h(14).child(text(std::to_string(n1 * 100 + n2))))                               // 14: the counters shown
        .child(div().w(60).h(30).flex_none().overflow_y_scroll().on_scroll([](Fix& s, gp_point o) { s.scrolls++; s.lastx = o.y; })   // 15: scrolls
                   .child(div().w(40).h(100).flex_none()))
        .child(uniform_list(100, 20, [](size_t i) { return div().h(20).child(text("r" + std::to_string(i))); })   // 16: a list with a pinned header
                   .w(100).h(60).flex_none()
                   .list_sticky([](size_t r) { return ListGroup(div().h(20).child(text("G" + std::to_string(r / 10))), r / 10 * 10, r / 10 * 10 + 10); }));
    for (const Case& c : kCases) root.child(c.make().flex_none());                                       // 15..: the style carriers
    for (const PassThrough& pt : kPass) root.child(pt.cpp()).child(raw(pt.c()));              // then two per pass-through: C++, C
    return root;
}

// ---------------------------------------------------------------- the test

static int base_index() { return kPairs; }
static void click_box(int i, int button_count = 1) {
    int path[1] = { base_index() + i };
    float r[4];
    if (!cpp_probe_rect(g_app, path, 1, r)) { CHECK(false, "handler box %d not found", i); return; }
    float x = r[0] + 10, y = r[1] + 10;
    gp_simulate_mouse(g_app, GP_SIM_MOVE, x, y, 0, 0);
    gp_simulate_mouse(g_app, GP_SIM_DOWN, x, y, button_count, 0);
    gp_simulate_mouse(g_app, GP_SIM_UP, x, y, button_count, 0);
    gp_sleep(g_co, 0.1);
}

static int g_alive;
struct Tracked {
    std::string name;
    std::vector<int> data;
    explicit Tracked(std::string n) : name(std::move(n)), data(3, 7) { g_alive++; }
    ~Tracked() { g_alive--; }
};

static void test_shared_string() {
    SharedString a("hello"), b = a, c(std::string("hello")), e;
    CHECK(a.same(b) && !a.same(c) && a == c && !(a == SharedString("hellp")), "copies are the same text, equal ones compare equal");
    CHECK(a.use_count() >= 2 && a.size() == 5 && !a.empty() && a.view() == "hello" && std::string_view(a) == "hello" && std::strcmp(a.c_str(), "hello") == 0, "size, view and c_str");
    CHECK(e.empty() && e.view().empty() && e == SharedString("") && e == SharedString(std::string()) && e.same(SharedString()) && e.same(SharedString(std::string_view())) && e.same(SharedString("")) && std::strlen(e.c_str()) == 0, "the empty string is one shared empty text");
    const char* ptr = a.c_str();
    { SharedString d = a; CHECK(d.c_str() == ptr, "a copy does not copy the text"); }
    CHECK(a.c_str() == ptr, "the text stays where it was while a copy is dropped");
    SharedString outlive;
    { std::string tmp = "temp text"; outlive = SharedString(tmp); tmp = "xxxxxxxxxxxxxxxxxxxx"; }
    CHECK(outlive.view() == "temp text", "it does not depend on the string it was made from");
    std::string moved(60, 'm');
    SharedString m(std::move(moved));
    CHECK(m.size() == 60 && m.view()[59] == 'm', "from a moved string");
    CHECK(SharedString("a") < SharedString("b") && SharedString("b") > SharedString("a") && SharedString("a") <= SharedString("a") && (SharedString("a") <=> SharedString("b")) < 0, "ordering");
    std::unordered_map<SharedString, int> map;
    map[SharedString("one")] = 1;
    map[SharedString(std::string("one"))] += 10;
    map["two"] = 2;
    CHECK(map.size() == 2 && map[SharedString("one")] == 11, "as a hash key");
    Str s(a);
    CHECK(s.c_str() == a.c_str(), "as a Str it is used as it is: no copy into the frame");
    SharedString from_null(static_cast<const char*>(nullptr));
    CHECK(from_null.empty(), "a null C string is empty");
}

static void test_colors() {
    Color o = rgb(0xff8000);
    CHECK(o.r == 1.f && std::fabs(o.g - 128.f / 255) < 1e-6 && o.b == 0 && o.a == 1, "rgb");
    Color a = rgba(0x00ff0080);
    CHECK(a.g == 1.f && std::fabs(a.a - 128.f / 255) < 1e-6, "rgba");
    auto close = [](Color x, float r, float g, float b, float al) { return std::fabs(x.r - r) < 1e-5 && std::fabs(x.g - g) < 1e-5 && std::fabs(x.b - b) < 1e-5 && std::fabs(x.a - al) < 1e-6; };
    CHECK(close(hsla(0, 1, 0.5f), 1, 0, 0, 1), "hsla red");
    CHECK(close(hsla(1.f / 3, 1, 0.5f), 0, 1, 0, 1), "hsla green");
    CHECK(close(hsla(2.f / 3, 1, 0.5f, 0.25f), 0, 0, 1, 0.25f), "hsla blue with alpha");
    CHECK(close(hsla(0.5f, 0, 0.5f), 0.5f, 0.5f, 0.5f, 1), "hsla gray");
    CHECK(close(hsla(0.5f, 1, 0.75f), 0.5f, 1, 1, 1), "hsla light cyan");
    CHECK(close(hsla(1.f, 1, 0.5f), 1, 0, 0, 1), "hsla wraps the hue");
    CHECK(close(with_alpha(red(), 0.5f), 1, 0, 0, 0.5f) && close(white(), 1, 1, 1, 1) && close(black(), 0, 0, 0, 1) && close(transparent(), 0, 0, 0, 0), "named colors");
}

static void test_trees() {
    char why[256];
    int checked = 0;
    for (int i = 0; i < kPairs; i++) {
        int a[3] = { i, 0, 0 }, b[3] = { i, 1, 0 };
        const char* bad = cpp_probe_same(g_app, a, 3, b, 3, why, sizeof why);
        CHECK(!bad, "pair %d: the C++ half differs from the C half: %s", i, bad ? bad : "");
        checked++;
    }
    CHECK(checked == kPairs, "all pairs compared");
    int nroot = cpp_probe_children(g_app, nullptr, 0);
    CHECK(nroot == kPairs + kHandlerBoxes + kNumCases + 2 * kNumPass, "the root has %d children, wanted %d", nroot, kPairs + kHandlerBoxes + kNumCases + 2 * kNumPass);
    // the layout really is something: the first pair's C++ cell holds three 20 px boxes
    int kid[4] = { 0, 1, 0, 0 };
    CHECK(cpp_probe_children(g_app, kid, 3) == 3, "the layout pair has three children");
}

static void test_spellings() {
    int bad = 0;
    for (int i = 0; i < kNumCases; i++) {
        int path[1] = { base_index() + kHandlerBoxes + i };
        float got = cpp_probe_style(g_app, path, 1, kCases[i].field);
        // (the `.flex_none()` appended to every carrier changes nothing the table looks at)
        if (std::fabs(got - kCases[i].want) > 1e-5f * std::fmax(1.f, std::fabs(kCases[i].want))) {
            CHECK(false, ".%s: field %d is %.7g, wanted %.7g", kCases[i].name, kCases[i].field, got, kCases[i].want);
            bad++;
        }
    }
    CHECK(bad == 0 && kNumCases > 600, "%d spellings checked, %d wrong", kNumCases, bad);
}

static void test_passthrough() {
    char why[200];
    int bad = 0;
    for (int i = 0; i < kNumPass; i++) {
        const char* d = cpp_probe_el_same(g_app, base_index() + kHandlerBoxes + kNumCases + 2 * i, base_index() + kHandlerBoxes + kNumCases + 2 * i + 1, why, sizeof why);
        if (d) { CHECK(false, ".%s differs from gp_%s: %s", kPass[i].name, kPass[i].name, d); bad++; }
    }
    CHECK(bad == 0 && kNumPass > 250, "%d pass-through methods checked, %d differ", kNumPass, bad);
}

static void test_pixels() {
    int pw, ph;
    const unsigned char* px = cpp_probe_capture(g_app, g_co, &pw, &ph);
    CHECK(px != nullptr, "no frame");
    if (!px) return;
    float winw = 900;
    float scale = (float)pw / winw;
    for (int i = 0; i < kPairs; i++) {
        int a[2] = { i, 0 }, b[2] = { i, 1 };
        float ra[4], rb[4];
        if (!cpp_probe_rect(g_app, a, 2, ra) || !cpp_probe_rect(g_app, b, 2, rb)) { CHECK(false, "pair %d cells", i); continue; }
        int ax = (int)std::lround(ra[0] * scale), ay = (int)std::lround(ra[1] * scale), bx = (int)std::lround(rb[0] * scale), by = (int)std::lround(rb[1] * scale);
        int w = (int)std::lround(ra[2] * scale), h = (int)std::lround(ra[3] * scale);
        int tol = (i == 3 || i == 13 || i == 14) ? 2 : 0;   // a gradient is dithered by the pixel's position on screen, so the two halves differ by a level or two
        long diff = 0, nonbg = 0;
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++) {
                const unsigned char* p = px + ((size_t)(ay + y) * pw + (size_t)(ax + x)) * 3;
                const unsigned char* q = px + ((size_t)(by + y) * pw + (size_t)(bx + x)) * 3;
                if (std::abs(p[0] - q[0]) > tol || std::abs(p[1] - q[1]) > tol || std::abs(p[2] - q[2]) > tol) diff++;
                if (p[0] != 0x1c || p[1] != 0x1c || p[2] != 0x2a) nonbg++;
            }
        CHECK(diff == 0, "pair %d: %ld pixels differ between the C and the C++ half", i, diff);
        CHECK(nonbg > 50, "pair %d draws something (%ld pixels differ from the cell background)", i, nonbg);
    }
}

static void test_handlers(Fix& f) {
    click_box(0);
    CHECK(f.n1 == 1, "[](Fix&): %d", f.n1);
    click_box(1);
    CHECK(f.n2 == 1, "[](Fix&, Ctx): %d", f.n2);
    click_box(2);
    CHECK(std::fabs(f.lastx - 10) < 1.5f, "[](const gp_mouse_event&, Fix&): local x %g", f.lastx);
    click_box(3);
    click_box(3);
    CHECK(f.total == 10 && g_cptr_hits == 2, "a capturing lambda (a copy of an int and a pointer): %d, %d", f.total, g_cptr_hits);
    click_box(4);
    CHECK(f.n3 == 1 && f.ctx_ok, "[](gp_ctx*, Fix&): %d %d", f.n3, (int)f.ctx_ok);
    click_box(5);
    CHECK(g_free_calls == 1, "a lambda taking the event by value and nothing else: %d", g_free_calls);
    click_box(6);
    CHECK(g_cdata_seen == 77, "a C function pointer with its data: %d", g_cdata_seen);
    click_box(7);
    CHECK(f.downs == 5 && f.ups == 1, "on_mouse_down (capturing) and on_mouse_up (stateless): %d %d", f.downs, f.ups);
    {
        int path[1] = { base_index() + 7 };
        float r[4];
        cpp_probe_rect(g_app, path, 1, r);
        gp_simulate_mouse(g_app, GP_SIM_MOVE, r[0] + 10, r[1] + 10, 0, 0);
        gp_simulate_mouse(g_app, GP_SIM_DOWN, r[0] + 10, r[1] + 10, 1, 0);
        gp_simulate_mouse(g_app, GP_SIM_MOVE, r[0] + 20, r[1] + 12, 0, 0);
        gp_simulate_mouse(g_app, GP_SIM_UP, r[0] + 20, r[1] + 12, 1, 0);
        gp_sleep(g_co, 0.1);
        CHECK(f.drags >= 1, "on_drag (stateless): %d", f.drags);
    }

    click_box(8);   // clicking a tracked element focuses it
    gp_simulate_keystrokes(g_app, "a b enter");
    gp_sleep(g_co, 0.15);
    CHECK(f.keys == "a;b;enter;", "on_key_down with a gp_key_event: \"%s\"", f.keys.c_str());
    CHECK(f.ups_k == "abenter", "on_key_up: \"%s\"", f.ups_k.c_str());
    gp_simulate_ime(g_app, "ab", 1);
    gp_sleep(g_co, 0.1);
    CHECK(f.ime == "ab" && f.ime_cursor == 1, "on_ime with a string_view and an int: \"%s\" %d", f.ime.c_str(), f.ime_cursor);
    gp_simulate_text(g_app, "h\xc3\xa9llo");
    gp_sleep(g_co, 0.1);
    CHECK(f.typed == "h\xc3\xa9llo", "on_text_input with a string_view: \"%s\"", f.typed.c_str());
    gp_bind_key(g_app, "ctrl-j", "cpp::bump", "CppBox");
    gp_simulate_keystrokes(g_app, "ctrl-j ctrl-j");
    gp_sleep(g_co, 0.15);
    CHECK(f.actions == 2, "on_action with a lambda: %d", f.actions);

    CHECK(g_layouts >= 1 && g_layout_size.w == 60 && g_layout_size.h == 30, "on_layout got the size: %g x %g (%d calls)", g_layout_size.w, g_layout_size.h, g_layouts);

    float r[4];
    int p10[1] = { base_index() + 10 }, p11[1] = { base_index() + 11 }, p9[1] = { base_index() + 9 };
    cpp_probe_rect(g_app, p10, 1, r);
    CHECK(std::fabs(r[2] - 77) < 0.01f, "with_animation (stateless) laid its style over the element: width %g", r[2]);
    cpp_probe_rect(g_app, p11, 1, r);
    CHECK(std::fabs(r[3] - 91) < 0.01f, "with_animation (capturing) : height %g", r[3]);
    cpp_probe_rect(g_app, p9, 1, r);
    CHECK(std::fabs(r[2] - 60) < 0.01f, "an element without animation is as it was: %g", r[2]);

    CHECK(f.scrolls >= 1, "on_scroll is called after a layout: %d", f.scrolls);
    {
        int path[1] = { base_index() + 15 };
        float r[4];
        cpp_probe_rect(g_app, path, 1, r);
        int before = f.scrolls;
        cpp_probe_wheel(g_app, r[0] + 5, r[1] + 5, 0, 20);
        gp_sleep(g_co, 0.1);
        CHECK(f.scrolls > before && std::fabs(f.lastx - 20) < 0.01f, "on_scroll with the new offset when the wheel moves it: %d calls, y %g", f.scrolls - before, f.lastx);
    }
    {   // gp_list_sticky from a lambda: the header of the group of the first row in view, once its own row has gone past the top
        int path[1] = { base_index() + 16 };
        float r[4];
        cpp_probe_rect(g_app, path, 1, r);
        CHECK(cpp_probe_sticky_text(g_app, path, 1) == nullptr, "nothing pinned at the top of the list");
        cpp_probe_wheel(g_app, r[0] + 5, r[1] + 5, 0, 45);
        gp_sleep(g_co, 0.15);
        const char* t = cpp_probe_sticky_text(g_app, path, 1);
        CHECK(t && std::string(t) == "G0", "list_sticky: after 45 px \"G0\" is pinned: \"%s\"", t ? t : "(none)");
        cpp_probe_wheel(g_app, r[0] + 5, r[1] + 5, 0, 450);
        gp_sleep(g_co, 0.15);
        t = cpp_probe_sticky_text(g_app, path, 1);
        CHECK(t && std::string(t) == "G2", "...and \"G2\" at 495 px: \"%s\"", t ? t : "(none)");
    }
    // the view that notified rendered again: the counters element shows n1 * 100 + n2
    int shown[2] = { base_index() + 14, 0 };
    const char* t = cpp_probe_text(g_app, shown, 2);
    CHECK(t && std::string(t) == "101", "the notified view rendered again: \"%s\"", t ? t : "(null)");
}

static void test_state(Fix& f, App app) {
    // a child view, given as an Entity or by .element(), is rendered by the C library; a view may have a lambda for its render
    int c12[2] = { base_index() + 12, 0 };
    const char* t = cpp_probe_text(g_app, c12, 2);
    CHECK(t && std::string(t) == "child 1", "an Entity as a child view renders: %s", t ? t : "(null)");
    int p13[2] = { base_index() + 13, 0 };
    t = cpp_probe_text(g_app, p13, 2);
    CHECK(t && std::string(t) == "5", "a view given a lambda for render: %s", t ? t : "(null)");

    // update(): a ctx for that entity; its notify renders that view again
    int renders = f.child.get(app)->renders;
    f.child.update(app.ctx(), [](Child& c, Ctx cx) { c.value = 42; cx.notify(); });
    gp_sleep(g_co, 0.2);
    t = cpp_probe_text(g_app, c12, 2);
    CHECK(t && std::string(t) == "child 42" && f.child.get(app)->renders > renders, "Entity::update then notify: \"%s\", %d renders", t ? t : "(null)", f.child.get(app)->renders);
    CHECK(app.get(f.child) == f.child.get(app) && static_cast<bool>(f.child), "typed access");

    // state with members that need construction and destruction
    CHECK(g_alive == 0, "nothing alive yet");
    {
        Entity<Tracked> e = app.new_entity<Tracked>("abc");
        CHECK(g_alive == 1, "constructed once: %d", g_alive);
        Tracked* tr = app.get(e);
        CHECK(tr && tr->name == "abc" && tr->data.size() == 3 && tr->data[2] == 7, "a std::string and a std::vector live in the entity");
        tr->name += std::string(100, 'x');   // grows on the heap: a byte copy of the object would break it
        CHECK(app.get(e)->name.size() == 103, "...and can grow");
        app.release(e.id);
        gp_flush_effects(app.raw());
        CHECK(g_alive == 0, "destroyed once when released: %d", g_alive);
        CHECK(app.get(e) == nullptr, "a released entity is gone");
    }
    {
        struct TV { std::string s; Tracked t; TV() : s("v"), t("w") {} Element render(Ctx) { return div().child(text(s)); } };
        Entity<TV> v = app.new_view<TV>();
        CHECK(g_alive == 1, "a view's state is constructed too: %d", g_alive);
        app.release(v.id);
        gp_flush_effects(app.raw());
        CHECK(g_alive == 0, "...and destroyed: %d", g_alive);
    }
}

static Entity<Fix> g_root;

static void driver(gp_co* co, void* self, gp_ctx* cx, void* data) {
    App app(g_app);
    Fix* f = app.get(g_root);
    g_co = co;
    gp_sleep(co, 0.4);
    test_colors();
    test_shared_string();
    test_trees();
    test_spellings();
    test_passthrough();
    test_pixels();
    test_handlers(*f);
    test_state(*f, app);
    gp_quit(g_app);
}

int main() {
    setenv("GPUIC_IGNORE_REAL_MOUSE", "1", 1);
    int rc = run([](App app) {
        g_app = app.raw();
        g_root = app.new_view<Fix>(app);
        app.open_window("cpp_test", 900, 840, g_root);
        gp_ctx cx = gp_ctx_for(g_app, GP_ENTITY_NULL);
        gp_spawn(&cx, driver, nullptr);
    });
    CHECK(rc == 0, "application_run returned %d", rc);
    std::printf(failures ? "cpp_test: %d FAILURE(S)\n" : "cpp_test: all checks passed\n", failures);
    return failures != 0;
}
