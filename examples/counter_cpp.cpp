// The counter of examples/counter.c in C++: the same view, written the way GPUI's Rust is.
#include <gpuic/gpuic.hpp>

#include <string>

using namespace gpuic;

static const Color BASE = rgb(0x1e1e2e), SURFACE = rgb(0x313244), TEXT = rgb(0xcdd6f4), SUBTEXT = rgb(0x9399b2);
static const Color BLUE = rgb(0x89b4fa), BLUE_HI = rgb(0xa6c4fb), BLUE_LO = rgb(0x6c9ef0), RED = rgb(0xf38ba8);

struct Footer {
    Element render(Ctx) { return div().text_color(SUBTEXT).child(text("Built with gpuic: GPUI-style UI in C++").text_size(13)); }
};

struct Counter {
    Entity<Footer> footer;
    int count = 0;

    explicit Counter(App app) : footer(app.new_view<Footer>()) {}

    // a button whose handler is given: `on_click` takes any callable (see gpuic.hpp for what its parameters can be)
    template <class F> static Element button(const char* label, F&& on_click) {
        return div().px_5().py_2p5().flex_shrink_0().rounded_lg().bg(BLUE).hover_bg(BLUE_HI).active_bg(BLUE_LO).text_color(BASE).cursor_pointer()
            .on_click(std::forward<F>(on_click))
            .child(text(label).font_bold());
    }

    Element render(Ctx) {
        return div()
            .size_full().flex_col().items_center().justify_center().gap_6().bg(BASE).text_color(TEXT)
            .child(div()
                       .flex_col().items_center().gap_5().p_8().w(460).rounded_2xl().bg(SURFACE).border_1().border_color(rgb(0x45475a)).shadow_lg()
                       .child(text("Counter").text_size(14).text_color(SUBTEXT))
                       .child(text(std::to_string(count)).text_size(64).font_bold().when(count < 0, [](Element e) { return e.text_color(RED); }))
                       .child(div().flex_row().gap_3()
                                  .child(button("-  Decrement", [](Counter& self, Ctx cx) { self.count--; cx.notify(); }))
                                  .child(button("+  Increment", [](Counter& self, Ctx cx) { self.count++; cx.notify(); }))
                                  .child(button("Reset", [](Counter& self, Ctx cx) { self.count = 0; cx.notify(); }))))
            .child(footer);
    }
};

int main() {
    return run([](App app) { app.open_window("gpuic counter (C++)", 640, 480, app.new_view<Counter>(app)); });
}
