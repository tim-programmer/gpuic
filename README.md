# gpuic

A GPU-accelerated UI framework for C, modelled on Rust's [GPUI](https://gpui.rs/).

```c
static gp_node *counter_render(void *self, gp_ctx *cx) {
    Counter *c = self;
    return gp_div(
        gp_flex_col(), gp_items_center(), gp_gap(16), gp_p(24), gp_rounded(12), gp_bg(SURFACE),
        gp_text(gp_fmt("%d", c->count), gp_text_size(48), gp_font_bold()),
        gp_div(gp_px(16), gp_py(8), gp_rounded(8), gp_bg(BLUE), gp_hover_bg(BLUE_HI),
               gp_cursor_pointer(), gp_on_click(increment),
               gp_text("Increment")));
}
```

## Using gpuic in your project

gpuic is a CMake library with the target `gpuic::gpuic`. Its public header includes only the C
standard library, so none of its dependencies leak into your build.

```cmake
include(FetchContent)
FetchContent_Declare(gpuic GIT_REPOSITORY https://example.com/gpuic.git GIT_TAG main)
FetchContent_MakeAvailable(gpuic)            # or add_subdirectory(gpuic)

add_executable(app main.c)
target_link_libraries(app PRIVATE gpuic::gpuic)
```

Included this way it builds only the library: examples, tests and install rules are on by default
only when gpuic is the top-level project (`GPUIC_BUILD_EXAMPLES`, `GPUIC_BUILD_TESTS`,
`GPUIC_INSTALL`). An installed copy is used with `find_package(gpuic 0.1 REQUIRED)`.

### Dependencies

Each dependency is looked for first (an existing CMake target, a CMake package, then pkg-config)
and **downloaded and built from source when it isn't found**, pinned to a release and checked by
SHA-256:

| Dependency | Version fetched | How | Notes |
|---|---|---|---|
| SDL3 | 3.4.20 | CMake, static | windowing and input; OpenGL is loaded at runtime through it, so there is no GL link dependency |
| FreeType | 2.14.3 | CMake, static | color emoji (CBDT) needs libpng, which FreeType picks up if present |
| HarfBuzz | 14.6.0 | CMake, static | its CMake build enables C++, so fetching it needs a C++ compiler |
| FriBidi | 1.0.17 | built by `cmake/GpuicFriBidi.cmake` | upstream has no CMake build; the release tarball ships the generated tables |
| fontconfig | never fetched | system only | optional; see below |
| stb_image | 2.30 | vendored in `third_party/stb/` | image decoding (PNG, JPEG, GIF, BMP); one public-domain header, hash recorded next to it |

`-DGPUIC_DEPENDENCIES=` chooses the policy: `AUTO` (default: system first, download what is
missing), `SYSTEM` (never download; fail with a clear message instead) or `FETCH` (always download
the four above). A parent project that already provides `SDL3::SDL3`, `Freetype::Freetype`,
`harfbuzz::harfbuzz` or `FriBidi::FriBidi` has its targets reused rather than duplicated.

**fontconfig** can't be downloaded: upstream builds with meson or autotools and needs generated
sources. It is used when found (`-DGPUIC_FONTCONFIG=AUTO`, the default; `ON` requires it, `OFF`
skips it). Without it, gpuic scans the platform's font directories once (on first text use;
`GPUIC_FONT_PATH` adds more) and picks fonts itself: a preference list of common UI families,
the weight closest to regular/bold, monospace and italic faces last, and for a character the
primary lacks, the first font in that order that covers it. That works without any system
library, but it knows nothing about language, so Han unification takes whichever CJK font ranks
first, and Hebrew/Arabic come from the first covering font rather than fontconfig's script-aware
choice. When FreeType is downloaded fontconfig is not used (it would link a second FreeType).

Opening every font file is what makes a scan slow (about 0.3 s for ~3,000 files here, against 16 ms
with fontconfig), so the result is **cached**: one text file per set of scanned directories, in
`$XDG_CACHE_HOME/gpuic` (`~/.cache/gpuic`; `~/Library/Caches/gpuic` on macOS, `%LOCALAPPDATA%\gpuic`
on Windows). Every start still walks the directories (a listing and a `stat` per file, ~10 ms) and
trusts a cached entry only if the file's size *and* modification time are unchanged; new or changed
files are opened again, vanished ones drop out, and the cache is rewritten only when something
changed. A truncated, damaged or unknown-version cache is ignored as a whole (it ends with a
checksum) and rebuilt, and a cache that can't be written just means no caching. Warm start: about
10 ms. `GPUIC_FONT_CACHE=0` turns it off, `GPUIC_FONT_CACHE=<dir>` moves it.

`install` needs every dependency from the system (downloaded ones are static libraries inside your
build tree, which an installed libgpuic can't refer to), so it is skipped with a message otherwise.
The installed `gpuicConfig.cmake` finds the same dependencies again with `GPUIC_DEPENDENCIES=SYSTEM`.
gpuic builds as a static library; there are no Windows DLL export macros yet.

Only Linux has been built and run here. The other platforms' font directories and the SDL-based
GL loading follow SDL's own cross-platform APIs, but are untested.

## Build & run

From a checkout with the dependencies above available (or `-DGPUIC_DEPENDENCIES=FETCH`), plus
whatever fonts you want text in: a sans-serif font, and optionally a CJK font and a color emoji font.

```sh
cmake -S . -B build -G Ninja && cmake --build build
./build/counter
```

Headless checks: `GPUIC_SCREENSHOT=out.ppm` renders a few frames, writes a PPM and exits.
Inject input after the second frame with `GPUIC_HOVER=x,y`, `GPUIC_CLICK=x,y` and
`GPUIC_KEYS="tab h i shift-tab enter"` and `GPUIC_WHEEL="x,y,dx,dy;..."` (real SDL key / wheel
events, one per frame; SDL's sign convention: `dy < 0` scrolls down, `dx > 0` scrolls right), plus
`GPUIC_DRAG=x0,y0,x1,y1` (press, move, release) and `GPUIC_CLICKS="x,y@ms;..."` (clicks at times
after start). `GPUIC_WAIT_MS=n` holds the screenshot until n ms after start, so timers and
animations can be sampled. The screenshot otherwise waits for the script to drain.
`GPUIC_STATS=1` prints loop wakeups, draws and the per-draw build/layout/paint/GL-submit times at exit (an idle app should show ~none).
`GPUIC_IME=text` shows an input-method composition in the focused field (after any `GPUIC_KEYS`).
`GPUIC_ATLAS_RESET_EVERY=n` clears the glyph atlases every n draws, to exercise the overflow recovery.
`GPUIC_LCD=off|rgb|bgr|vrgb|vbgr|auto` chooses how glyph edges are smoothed (see LCD antialiasing; the tests set `off`).
`GPUIC_IGNORE_REAL_MOUSE=1` drops what the physical pointer does and keeps the injected events (the tests set it: a pointer resting over a test window changes hover and repaint behavior).

`ctest` (in the build dir) runs `tests/exec_test.c` (executors), `tests/window_test.c` (what belongs to a window
and ends with it), `tests/autohide_test.c` (fading scrollbars, with the clock jumped), `tests/fd_test.c` (descriptor watches and
awaits: pipes, sockets, a TCP echo through coroutines, closing and cancelling at awkward moments),
`tests/anim_test.c` (animations, with the clock jumped), `tests/fonts_test.c` (families, weights, italic and features through layout and cached views), `tests/lcd_test.c` (pixels read back for each antialiasing mode), `tests/gfx_test.c` / `tests/gradient_test.c` / `tests/image_test.c` / `tests/svg_test.c` / `tests/svgx_test.c` (SVG use, clip, mask, text) / `tests/box_test.c` (gradients, images, SVG, corners and borders), `tests/flex_test.c` (auto margins, wrapped lines), `tests/minwidth_test.c` (the automatic minimum width of row items), `tests/reveal_test.c` (scroll into view), `tests/track_test.c` (track clicks), `tests/smooth_test.c` (smooth scrolling), `tests/keys_test.c` (chords, key-up, payloads), `tests/bigdoc_test.c` (a 14 MB document in the text input), `tests/grapheme_test.c` (where a character ends), `tests/variations_test.c` (variable-font axes through layout), `tests/gamma_test.c` (linear-light text blending), `tests/touch_test.c` (touch drags and flings), `tests/sticky_test.c` (pinned list headers), `tests/hlist_test.c` (horizontal lists), `tests/hvlist_test.c` (horizontal gp_list: items of different widths), `tests/border_style_test.c` (per-side border colors, dashes, dots, gradient borders), `tests/state_style_test.c` (hover, pressed and focus styles), `tests/paintshift_test.c` (moved painted copies), `tests/transform_test.c` (rotate, scale, translate: pixels, pointer, clips, animation), `tests/cpp_test.cpp` (the C++ layer), `tests/cpp_async_test.cpp` (its timers, coroutines, observers, text fields and loaders: closures destroyed once), `tests/cpp_input_test.cpp` (its scroll and focus handles, key bindings, actions and clipboard), `tests/ime_wrap_test.c` (a composition in a soft-wrapped field), `tests/wrapedit_test.c` (rows kept up to date through edits), `tests/dualcaret_test.c` (the second caret at a direction change), `tests/input_test.c` (the text input
widget, end to end through `gp_simulate_*`), `tests/list_test.c` (the variable-height list, checked
against a model), `tests/text_test.c` (fonts and shaping, against the fonts actually installed) and
`tests/vlist_test.c` (the row-height index against a brute-force model). The first three need a
display, like the examples. `tests/observe_test.c` (entity observers and events) and `tests/views_test.c`
(dirty tracking and cached views) need one too. `cmake -DCMAKE_C_FLAGS=-fsanitize=address,undefined` and
`-fsanitize=thread` builds both run clean (ASan only warns that it can't fully model `ucontext`).

## How GPUI's model maps to C

| GPUI                         | gpuic                                                        |
|------------------------------|--------------------------------------------------------------|
| `Application::run`           | `gp_application_run(ready, userdata)`                        |
| `Entity<T>`                  | `gp_entity` — generational handle; `gp_entity_get` for state |
| `impl Render for T`          | `gp_view_new(app, size, init, render_fn, drop)`              |
| `div().flex().p_4()`         | `gp_div(gp_flex_row(), gp_p(16), ...)`                       |
| `.child(view)`               | `gp_child_view(entity)`                                      |
| `.child(view.cached(..))`    | `gp_child_view_cached(entity)`                               |
| `cx.notify()`                | `gp_notify(cx)`                                              |
| `cx.observe(&e, ..)`         | `gp_observe(cx, e, fn, data)`                                |
| `cx.subscribe(&e, ..)` / `cx.emit(ev)` | `gp_subscribe(cx, e, "name", fn, data)` / `gp_emit(cx, "name", &payload, size)` |
| `cx.observe_release(&e, ..)` | `gp_observe_release(cx, e, fn, data)`                        |
| `el.with_animation(id, Animation::new(d).repeat(), \|el, t\| ..)` | `gp_with_animation(id, (gp_animation){ .duration = d, .repeat = true }, fn, data)` |
| closures in `on_click`       | `fn(self, cx, event, data)` + optional `data` pointer        |

Elements and modifiers are the same type (`gp_node *`), allocated from a per-window
frame arena that is reset on every rebuild: no element is ever freed by hand. A
builder call takes modifiers (style the element) and elements (become children) in
any order; `NULL`s are skipped so `cond ? child : NULL` works.

## C++

`include/gpuic/gpuic.hpp` (C++20, header only) puts GPUI's chained style on top of the C library. It adds no library and calls only the
public C functions, so a C++ view can contain C elements and the other way round.

```cpp
#include <gpuic/gpuic.hpp>
using namespace gpuic;

struct Counter {
    int count = 0;
    Element render(Ctx cx) {
        return div().flex().flex_col().items_center().gap_4().p_8().bg(rgb(0x1e1e2e)).text_color(white())
            .child(text(std::to_string(count)).text_size(64).font_bold())
            .child(div().px_5().py_2().rounded_lg().bg(rgb(0x89b4fa)).hover_bg(rgb(0xa6c4fb)).cursor_pointer()
                       .on_click([](Counter& self, Ctx cx) { self.count++; cx.notify(); })
                       .child("Increment"))
            .when(count > 9, [](Element e) { return e.text_color(rgb(0xf38ba8)); });
    }
};
int main() {
    return run([](App app) { app.open_window("Counter", 480, 360, app.new_view<Counter>()); });
}
```

Link `gpuic::cpp` (CMake: `target_link_libraries(app PRIVATE gpuic::cpp)`; it is `gpuic` plus `cxx_std_20`). `examples/counter_cpp.cpp` is
`examples/counter.c` in this style.

* **Elements and the chain.** `div()`, `text("...")`, `img(image)`, `svg_img(svg)`, `uniform_list(count, h, row)`, `list(count, h, row)`
  and `style()` (modifiers on their own) make an `Element`; every method adds to it and returns it, by reference on a named element and as a
  temporary on a temporary, so `div().flex().child(x)` and `e.flex(); e.child(x);` both work. An `Element` converts to a `gp_node *` where
  the C API wants one, copies deeply (`Element b = a; b.bg(red)` leaves `a` alone), and as in C a later modifier wins over an earlier one.
* **Every C modifier is a method**, named like the function without `gp_` (`gp_flex_row()` is `.flex_row()`, `gp_gap(8)` is `.gap(8)`,
  `gp_text_color(c)` is `.text_color(c)`). They are generated from `gpuic.h` by `tools/gen_cpp_chain.py` into `gpuic_chain.inc`, so none can be
  forgotten, and a ctest fails if the file is not what the header gives now. A parameter that is a C string takes a `std::string` or
  `std::string_view` as well (copied into the frame). What needs more than a pass-through is written by hand: gradients take braced lists
  (`.bg_gradient(90, {stop(red(), 0), stop(blue(), 1)})`, also `bg_radial`, `bg_repeating`, `bg_repeating_radial`, `bg_conic`), and `.refine(style)`
  applies a `style()`, `.with(gp_node *)` one modifier made by C.
* **GPUI's spellings** are generated too: the spacing scale in Tailwind's 4 px steps for `gap p px py pt pr pb pl m mx my mt mr mb ml w h size
  min_w min_h max_w max_h top right bottom left inset` (`.gap_2()` is 8 px, `.p_4()` 16, `.w_px()` 1, `.m_0p5()` 2, `.h_96()` 384), fractions
  `.w_1_2() .h_3_4()` ..., `.rounded_sm/md/lg/xl/2xl/3xl()`, `.border_1/2/4/8()` and per side `.border_t_2()`, `.text_xs()` to `.text_4xl()`,
  `.font_light()` to `.font_black()`, `.shadow_sm()` to `.shadow_2xl()`, and the no-ops `.flex()` and `.relative()` (every element is a flex
  container, positioned unless `.absolute()`).
* **Children.** `.child(x)` takes an `Element`, a `gp_node *`, a string (a text element), an `Entity<T>` (a nested view) or `nullptr`
  (skipped); `.children(range)` and `.children({a, b})` take several. `.when(cond, f)`, `.when_some(optional, f)` and `.map(f)` are GPUI's:
  `f` gets the element and returns it changed.
* **Handlers take lambdas** (`.on_click .on_mouse_down .on_mouse_up .on_drag .on_key_down .on_key_up .on_text_input .on_action .on_layout
  .on_ime .with_animation`, and the rows of the lists). A lambda names what it wants by its parameter types, in any order and any subset: the
  state of the view that built the element by reference (`Counter&`, as GPUI's `this`), `Ctx` or `gp_ctx *`, and the event of that
  handler (`const gp_mouse_event&` or by pointer or by value, `const gp_key_event&`, `std::string_view` or `const char *` for text,
  `gp_size` for layout, an `int` for the IME cursor, a `float` for an animation's progress, a `size_t` for a row). Captures are copied into
  the frame's arena, so they must be trivially destructible (numbers, pointers, entity handles: a `static_assert` says so); a lambda that
  captures nothing costs nothing. A C function pointer and its data work as in C. `on_drag`, `on_mouse_up`, `on_layout` and `on_ime` have no data
  pointer in the C library, so their lambdas cannot capture.
* **State.** `app.new_entity<T>(args...)` and `app.new_view<T>(args...)` construct `T` inside the app (so it may hold `std::string`, `std::vector`...)
  and run its destructor when the entity is released; a view's `T` has `Element render(Ctx)`. `Entity<T>` is the typed handle: `.get(app)`,
  `.update(cx, [](T&, Ctx) {...})`, and as a child it is a nested view (`.cached()` for `gp_child_view_cached`). `new_view_with<T>(lambda, args...)` takes a
  capture-less render lambda instead of a member. `Ctx` has `notify()`, `propagate()`, `app()` and `raw()`; `run(ready)` and `App::open_window`
  start the program; everything else of the C API is reached through `app.raw()` and `cx.raw()`.
* **Lifetime** is the C library's: an `Element` is built while a view renders, out of the frame's arena, and lives until the next rebuild.
  Do not keep one between frames.
* **Tests.** `tests/cpp_test.cpp` builds the same interface twice, with the C macros and with the chain, and requires the laid-out trees (kinds, sizes,
  positions, texts) and the pixels of the two halves to be identical, for eighteen interfaces (layout, text, wrap, gradients, when, last-one-wins,
  borders and shadows, fractions, strings, both kinds of list, images, styles, copies, RTL...). Every spelling of the scale is checked against the
  number it means, written out in the test (several hundred, each on a temporary and on a named element), and every pass-through method
  against its C function with different arguments (so swapped parameters show: generated by the same script, ~280 rows). Then every kind of
  handler is run from simulated input with every parameter list, with captures, and so on, and entities and views with non-trivial state are
  constructed, grown, destroyed once, rendered as children and updated. About 50 mutants of the header and the generated file are all caught.
* **Timers and background work** are methods of `Ctx`: `cx.after(seconds, f)`, `cx.every(seconds, f)`, `cx.next_frame(f)` (`f` may ask for the frame's
  `double` time), `cx.animate(seconds, easing, f)` (a `float` t), `cx.background(work, done)`, `cx.spawn(f)` (a coroutine), `cx.watch_fd(fd, events, f)`
  (`FdEvent{fd, events}`) and `cx.load_image(path, f)` (a `gp_image`). The callable names its parameters as a handler does (`Self&` is the state of
  the entity the ctx belongs to, `Ctx`, the extra) and, unlike a handler's, is **owned**: it is moved to the heap, kept until the task is over and
  then destroyed exactly once, whether it ran, was cancelled (`Task::cancel()`), lost its entity or window, or the app shut down, so it may capture
  `std::string`, `std::shared_ptr` or a move-only value. That rests on two small C additions, `gp_task_on_drop` and `gp_subscription_on_drop`
  (`drop(data)` once, after the callback could last have run; a subscription's runs at the next flush, never inside the callback that
  unsubscribed it, so a closure can cancel itself). `background` passes what `work()` returned (any type, on a pool thread) to `done` as a parameter of that type.
  A coroutine's `Co&` has `sleep yield frame run(work) fd read write accept connect cancelled`; `co.run([]{ return compute(); })` is `gp_await_background` with a
  result (`std::optional<R>`, empty if cancelled). Handles are `Task` and `Subscription` (`active()`, `cancel()`).
* **Observing**: `cx.observe(entity, f)` (`gp_entity observed`), `cx.observe(entity)` (re-render with it), `cx.subscribe(emitter, "name", f)`
  (`const gp_event&`), `cx.on_release(entity, f)` (`gp_entity released, void *state`) and `cx.emit("name", payload)`.
* **Text inputs, pictures, fonts, windows.** `app.text_input().owner(me).text("hi").placeholder("Name").multiline().on_change(f).on_submit(f).build()` returns a
  `TextInput` (`.element()`, `.text(app)`, `.set_text(cx, s)`, `.select_all(cx)`, `.selection(app)`, `.preedit(app)`, `.focus(app)`, `.rows(app)`); its callbacks are owned and
  destroyed with the field. `app.load_image(path | bytes)`, `app.image_from_rgba`, `app.load_svg(path | text)`, `app.release(image | svg)`, `app.add_font(path | bytes,
  family, weight)`. `app.open_window(...)` returns a `Window` (`.close()`, `.ctx()`: a ctx tied to the window, whose work ends when it closes). `app.ctx()` and
  `window.ctx()` return a `Ctx` that owns its `gp_ctx`, so it can be copied and returned.
* **State styles.** `.hover(|s| ...)`, `.active(|s| ...)` and `.focus_style(|s| ...)` take a function that adds to a `style()` (`.hover([](Element s) { return s.bg(red()).opacity(0.8f); })`;
  a function taking `Element&` or `Element&&`, returning the element or nothing, works too): what it sets is painted while the pointer is over the element, pressed
  on it, or while it has focus (see "State styles" for what can change). `.hover_bg` and the others still exist.
* **Scroll and focus handles.** `app.new_scroll()` and `app.new_focus()` make a `Scroll` / `Focus` (keep them in the view's state; they convert to
  `gp_scroll` / `gp_focus`, so `.track_scroll(scroll)` and `.track_focus(focus)` take them). A `Scroll` has `offset max viewport to by to_end` (they take
  the `Ctx`) and, for a list it tracks, `to_item(cx, i, RowAlign::Nearest | Start | Center)`, `splice invalidate reset`; `cx.focus(f)`, `cx.blur()`, `cx.is_focused(f)`,
  `cx.focus_next()`, `cx.focus_prev()`, `cx.scroll_into_view(f)`, and `app.set_smooth_scrolling(seconds)`.
* **Key bindings and actions.** `app.bind_key("ctrl-s", "editor::save")`, with a context predicate (`"Editor && !ReadOnly"`; set by `.key_context("Editor")`) and with a payload
  (`app.bind_key("ctrl-1", "tab::select", nullptr, 1)`, read in the handler by `cx.payload<int>()`, which is null if the payload is of another size), chords
  (`"ctrl-k ctrl-c"`, `cx.keys_pending()` for a status line), `app.clear_key_bindings()`; `.on_action("editor::save", f)` is the handler, `cx.dispatch("editor::save"[, payload])` runs one
  as if a key had fired it (true if a handler took it), `app.on_global_action(name, f)` takes what nobody on the focus path handled (the closure lives as long as the app and is
  destroyed with it). `app.clipboard()` / `app.primary()` return an `std::optional<std::string>`, `set_clipboard` / `set_primary` set them.
* **`SharedString`** (GPUI's): an immutable string that is shared when copied (a `shared_ptr` to a `std::string`): hold one in a view's state and put it in the tree every frame (`.child(title)`,
  `text(title)`, any string argument) with no copy per frame, because a `Str` made from one uses its text as it is, which lives as long as any copy does. Compares by text (`same()` asks for the
  same object), orders, hashes (`std::hash`) and converts to `std::string_view`; an empty one is one shared empty text, and a null C string is empty.
* **Not wrapped:** the synthetic-input functions (`gp_simulate_*`) and the debug hooks stay C calls.

## Observing entities

State usually lives in a *model* (a plain entity) that several views show. The model calls
`gp_notify`; whoever depends on it registers with `gp_observe` or `gp_subscribe`:

```c
typedef struct { int count; } Counter;                  /* the model */
typedef struct { gp_entity counter; int shown; } Badge; /* a view that mirrors it */

static void counter_changed(void *self, gp_ctx *cx, gp_entity observed, void *data) {
    Badge *b = self;                                     /* the observer's own state */
    b->shown = ((Counter *)gp_entity_get(cx->app, observed))->count;
}

gp_ctx cx = gp_ctx_for(app, badge);                      /* the observer is cx.entity */
gp_observe(&cx, counter, counter_changed, NULL);         /* gp_notify(model) now calls it; it should
                                                            gp_notify(cx) itself if it changed the badge */
gp_observe(&cx, counter, NULL, NULL);                    /* or: just re-render me (and notify my observers) */

gp_subscribe(&cx, counter, "overflow", on_overflow, NULL);  /* events: named, with a payload struct */
gp_emit_as(&model_cx, "overflow", Overflow, .limit = 100);  /* the emitter is model_cx.entity */

gp_observe_release(&cx, counter, on_counter_gone, NULL);    /* called as it is released, state still valid */
```

* **The registering entity is the owner**: the ctx's `cx->entity` is what the callback gets as
  `self`. Pass a ctx for `GP_ENTITY_NULL` and the subscription belongs to the app (`self` is NULL).
* **Nothing runs inside the call that causes it.** `gp_notify` and `gp_emit` queue work, and the
  queue runs as soon as the callback in progress returns to the event loop: after each platform
  event, timer, frame callback, posted function and coroutine step, and before a window is rendered.
  So a handler is never re-entered by its own notifications, observers see the model as the caller
  left it, and the state they update is in place for the frame that is drawn next. Observers may
  notify and emit; all of that runs in the same flush, in order. Several notifies of one entity
  before the flush call its observers once. `gp_flush_effects(app)` runs the queue by hand for code
  outside the loop.
* **Lifetimes are handled.** A subscription ends when its owner or its target is released (a
  callback of a released owner never runs, even if already queued), or with `gp_unsubscribe`
  (callable from inside a callback, including to remove a later subscriber of the same delivery).
  Handles are generational: a stale one is harmless and cannot touch a subscription that reused
  its slot. A slot reused by a new entity inherits nothing.
* **Events are named, not typed**: agree on the payload struct for a name, as with actions.
  `gp_subscribe(.., NULL, ..)` hears every event of the emitter. The payload is copied at `gp_emit`
  and lives until the callback returns, so a stack struct is fine.
* A subscription made during a delivery waits for the next event. A cycle of observers notifying
  each other is cut off after 100,000 rounds, with a message, rather than hanging.
* Cost: `gp_notify` on an entity nobody observes is a slot lookup (a million take ~2 ms); with
  observers, one queued effect per entity until delivered, and delivery walks only that entity's
  subscriptions.
* A callback that changes its own view's state must say so with `gp_notify(cx)` (`cx->entity` is the
  view): observing a model does not by itself re-render the observer, as in GPUI. Passing `NULL` as the
  function does exactly that. See "Dirty tracking and cached views".

Tests: `tests/observe_test.c` (ordering, merging, lifetimes, slot reuse, unsubscribing mid-delivery,
cycles, where the flushes happen, and a randomized run against a model of the subscription table).

## Dirty tracking and cached views

`gp_notify(cx)` says *what* changed, `cx->entity`, and the framework does only what follows from that:

| `cx->entity` is | what happens |
|---|---|
| a view (it has a render function) | it renders again, the windows showing it rebuild, its observers are called |
| a plain entity (a model) | its observers are called. Nothing is repainted unless one of them notifies a view |
| none (`gp_ctx_for(app, GP_ENTITY_NULL)`, or `gp_app_notify(app)`) | the app itself: every window rebuilds and every cached view renders again |

So a window no longer rebuilds because *something* was notified: it rebuilds when a view it shows was.
Releasing a view rebuilds the windows that showed it. Layout and paint of a rebuilt window always cover
the whole tree; what can now be skipped is calling render functions and building elements.

```c
gp_child_view(entity)          /* as before: renders whenever the view that contains it renders */
gp_child_view_cached(entity)   /* GPUI's AnyView::cached: reused while it is clean */
```

A **cached view** gives back the elements of its last render, untouched, unless it was notified, a view
built inside it was, or everything was invalidated. A build walks down from the root: ordinary views
always render; a cached one renders only if marked. So pressing a button in one of 300 cached cards
renders that card and the views above it, and the other 299 are not visited.

* **The cache is only as fresh as the notifications.** A cached view must `gp_notify` (or observe
  something that does) whenever what its render function reads changes. State that is read but not
  announced goes stale: that is the price of skipping the render, exactly as in GPUI, and why caching is
  opt-in. (An uncached view cannot go stale: it renders with its parent.)
* **Also invalidated:** `gp_app_notify` (fonts registered, ...), and, when focus moves, the views whose last
  render called `gp_is_focused`. Handlers inside a cached view keep working (they belong to the view), and
  `gp_on_layout` handlers still run on every layout.
* **Layout and paint of a cached view are skipped too, while nothing they depend on changed.** The
  subtree is laid out for some inputs (the size and space its parent gave it, the text style it inherits);
  if the next layout hands it the same ones, nothing in it is walked or measured. Its painted instances are
  reused while where it is, the clip and opacity around it, the scale and the glyph atlas are the same,
  and, if it has focusable elements, focus; with hover styles or scrollbars, a mouse over anything in it
  paints it in full and what is painted then is not kept.
* **Scroll containers and lists inside a cached view are covered too.** The wheel and the scrollbar thumbs
  move them in place (the elements and the window's offsets stay in step), so those moves repaint just the
  cached views around the scroller and leave their layout alone. A change made from outside
  (`gp_scroll_to`, `gp_list_scroll_to_item`, `gp_list_splice`, ...) lays the views with scrollers out again
  once. So does a list measuring rows it had not seen: its extent is an estimate until then, so a
  `gp_list` that was just scrolled into new rows is laid out once more, and a list as tall as its rows
  (no height given) settles over a few layouts and is skipped after that. The views inside the rows of a
  list stay registered while the list is not laid out, so notifying one still reaches the screen.
  Everything reused is checked, after every frame of `views_test`, against a paint without caches and a
  full layout of the same tree.
* **Memory:** a cached view's elements live in an arena of its own (16 KB chunks), freed when the view is
  no longer reached by a build (hidden, removed, released). Strings from `gp_fmt` / `gp_frame_alloc`
  in its render live as long as its elements do.
* **Edge cases handled:** the same cached view placed twice in one window renders afresh for the second
  place (one element tree cannot be laid out in two); views in the rows of a list are rebuilt with the
  row and never reused; a text field, being a view, makes itself dirty when typed into or set from code,
  and a cached parent re-renders around it.
* **What it buys**, 300 small card views in a column, one card notified per frame (`GPUIC_STATS=1` prints
  the phases):

  | per frame | render fns | build | layout | paint |
  |---|---|---|---|---|
  | `gp_child_view` | 301 | 0.26 ms | 4.2 ms | 1.6 ms |
  | `gp_child_view_cached` | 2 (the root and the card) | 0.017 ms | 0.06 ms | 0.105 ms |

  That is about 6.1 ms of CPU per frame against 0.18 ms. Building was never the expensive part: the saving
  is in layout and paint, which skip the 299 untouched cards entirely. (`GPUIC_STATS=1` also prints how many
  cached subtrees skipped layout and reused their paint.)

Tests: `tests/views_test.c` counts render calls per view across notifies of views, models, observers,
the app and focus; checks that nothing cached ever shows stale state; that a cached view lays out exactly
like an identical one built afresh, through text-size changes and auto-height lists; that duplicated,
hidden, released and nested cached views behave; that scrollers and lists inside them follow the wheel,
the thumbs, `gp_scroll_to` and the list calls while their layout and painting are reused; and runs a random
tree of views, with scrollers, lists, wheels and thumb drags in it, against a model of these rules
(`VIEWS_TEST_SEED=n VIEWS_TEST_STEPS=n` run it with another seed or longer).

## Keyboard focus and actions

Same model as GPUI: a **focus handle** per focusable thing, **key contexts** on elements, a
**keymap** from keystrokes to named **actions**, and `on_action` handlers.

```c
/* once, at startup */
gp_bind_key(app, "backspace", "input::backspace", "TextInput");
gp_bind_key(app, "ctrl-s",    "file::save",       "Editor && !ReadOnly");
gp_bind_key(app, "ctrl-q",    "app::quit",        NULL);          /* global */
gp_on_global_action(app, "app::quit", quit, NULL);

/* in a render fn: Form { gp_focus field; ... } */
gp_div(gp_track_focus(f->field), gp_tab_stop(), gp_key_context("TextInput"),
       gp_focus_border_color(BLUE),
       gp_on_text_input(on_text),
       gp_on_action("input::backspace", backspace), ...)
```

* `gp_focus_new(app)` makes a stable id; keep it in view state. Clicking a tracked element
  focuses it, clicking elsewhere blurs. `gp_is_focused(cx, f)` is readable during render.
* A keystroke is resolved against the **focus path** (root → focused element; just the root
  when nothing is focused). Bindings whose context predicate matches a **deeper** element win;
  later bindings win ties.
* The action then bubbles from the focused element up through `gp_on_action` handlers, then to
  `gp_on_global_action`. A handler stops it unless it calls `gp_propagate(cx)`.
* Keys no binding handled go to `gp_on_key_down` handlers on the same path, and finally Tab /
  Shift-Tab cycle `gp_tab_stop` elements (tree order, or `gp_tab_index`).
* Text arrives separately via `gp_on_text_input`. The platform only turns text input (and the
  IME) on while the focused element, or an ancestor, has such a handler. A handled keydown
  suppresses the text event it would have produced.
* `gp_dispatch_action(cx, name)` fires an action programmatically from the focused element.

* **Chords.** A binding can be several keystrokes: `gp_bind_key(app, "ctrl-k ctrl-c", "ed::comment", "Editor")`, `"g g"`,
  `"ctrl-x ctrl-s ctrl-w"` (up to four, separated by spaces). The first key of a chord that has a binding in the focus path's contexts is
  *consumed* (no text is typed, no `on_key_down` sees it) and waits; `gp_keys_pending(cx)` returns what has been typed ("ctrl-k") for a
  status line. The next key either completes a binding (the action runs once, and a finished chord is consumed even if nobody handled
  its action) or is not part of one, which cancels the chord and consumes that key too (Emacs: "ctrl-x x is undefined"; VS Code does the
  same). Escape cancels a pending chord, and a half-typed chord is forgotten after 1.5 s (the next key is then an ordinary one). Chords are
  looked up in the contexts as they are when each key arrives, so a chord exists only where its context holds. A binding that is a whole
  chord's first keys wins at once ("ctrl-j" bound alone makes "ctrl-j ctrl-j" unreachable): do not bind a chord's prefix on its own. The
  releases of a chord's keys are delivered as ever, since only presses are held back.
* **Key release.** `gp_on_key_up` handlers get the release along the focus path, like `gp_on_key_down` (focused element first, bubbling
  up unless `gp_propagate`). It is not bound to actions. `gp_simulate_keystrokes` produces a press and a release for each key, and
  `gp_simulate_key(app, "shift-a", down)` one of them, for tests.
* **Action payloads.** `gp_bind_key_with(app, "ctrl-3", "tab::select", NULL, &three, sizeof three)` binds a copy of some bytes to the key, and
  the handler reads them with `gp_action_payload(cx, &size)` (NULL outside a handler and for an action that has none); `gp_dispatch_action_with(cx,
  name, &value, size)` dispatches an action with a payload from code. One handler then serves nine keys. A handler that dispatches
  another action gets its own payload back afterwards (tested).
* **Primary selection.** `gp_primary_get` / `gp_primary_set` (SDL's primary selection: X11 and Wayland) hold what is selected, separate
  from the clipboard. `gp_text_input` sets it when the mouse finishes a selection (a drag, a double- or triple-click; never in a password
  field) and pastes it at the click on a middle-button press (one undoable edit; not into a read-only field; the selection and the
  clipboard are untouched). Like the clipboard, a platform that refuses (an unfocused Wayland client) is covered by the app's own last
  selection, which is used until the platform has taken one of ours, after which a selection made in another application is what
  gets pasted. `gp_simulate_mouse_button` drives middle clicks in tests.
* **Tests:** `tests/keys_test.c` (chords of two and three keys, context, the broken, escaped and timed-out cases with the clock jumped,
  prefix and tie precedence, key release, payloads and nesting) and the primary-selection scenario in `tests/input_test.c`.

See `examples/form.c` for a caret-editing text field, focusable buttons, Escape bubbling to the
root, and a global quit.

## Scrolling

```c
gp_scroll list = gp_scroll_new(app);          /* optional handle, keep it in view state */

gp_div(gp_track_scroll(list), gp_overflow_y_scroll(), gp_flex_1(),
       gp_children(rows, n));                 /* rows: gp_node ** from gp_frame_alloc() */

gp_scroll_by(cx, list, 0, 240);               /* page down */
gp_scroll_to_end(cx, list);                   /* follow a log tail */
```

* `gp_overflow_x_scroll()`, `gp_overflow_y_scroll()` and `gp_overflow_scroll()` clip a container
  and let it scroll. Offsets live in a per-window table, so they survive every rebuild. Without
  a handle a container is keyed by its position in the tree (fine for static layouts); with
  `gp_track_scroll` the key is the handle, which is what you want for dynamic trees and for
  driving it from code. Offsets are clamped by layout, so `gp_scroll_to_end` can simply ask for
  "far enough".
* A scroller on its **main axis** lays its children out in unbounded space and never shrinks
  them; they overflow instead. On the cross axis children are sized to the viewport as usual.
* The wheel scrolls the innermost scroller under the pointer that can still move in that
  direction, then **chains** to its ancestors. Shift + wheel scrolls horizontally.
* Thin overlay scrollbars appear when there is something to scroll: draggable, brighter on
  hover. Restyle with `gp_scrollbar_color`, hide with `gp_hide_scrollbar`.
* **Track clicks.** A press on the scrollbar's track (the same narrow strip as the thumb's grab area, along its whole length) pages toward it
  by seven eighths of the view (the border not counted), once per press, and is consumed: it does not click the content under the strip,
  and does not start a drag. A press on the thumb drags as before. Lists page by the same amount (their offsets include the base of
  the rows scrolled away). Held, it **repeats**: another page after 0.4 s and then every 0.1 s, toward where the pointer is, until the
  button is released, the end is reached, or the thumb is under the pointer. The repeat is a window timer: nothing is woken once it stops.
  A hidden auto-hiding thumb has no track either.
* **Smooth scrolling.** `gp_set_smooth_scrolling(app, 0.15f)` (or `GPUIC_SMOOTH_SCROLL=150`, in milliseconds) makes a wheel tick move the
  scroller's *target* and lets the offset glide to it: an exponential approach that has covered 98% of the distance after the time
  given, so a fast wheel keeps adding to the target and speeds up, and a single tick eases out. Chaining to the scroller above is decided
  against the targets (a second tick that finds the inner target at its end goes outward at once, not after the glide). The thumb, a track
  click, `gp_scroll_to` and scroll-into-view end the glide where the offset is; a list builds its rows for each step. It runs from the
  draw path: a frame is asked for only while a glide lasts, and nothing after it arrives (a test checks the draw count). Off by default
  (the offset jumps), because an app that reads the offset right after a wheel event expects it to have moved. **Not done:** momentum
  (kinetic) scrolling after a touch-pad or touch fling; the touch-pad's own pixel deltas already arrive smooth, so turn this on for wheels.
* **Touch.** One finger (SDL's first finger; the pointer is not emulated from touch) scrolls and clicks: a **tap** (it moved less than 8 px) is a click at that
  point; a **drag** moves the scrollers under the finger with it, immediately and even with smooth scrolling on (the content goes down when the finger
  does), the innermost scroller that can move taking what it can and the ones around it the rest, as for the wheel; a **flick** (the finger moving faster than 120 px/s over its last
  100 ms when it lifted, and not stopped for 80 ms first) goes on as **momentum**: the speed falls with a time constant of 0.35 s, so a flick of *v* px/s travels
  *v* × 0.35 px, until it is under 8 px/s or the content ends. Each frame's step is the exact integral of that decay, so it comes out the same whatever the frame
  rate, a dropped frame included; frames are asked for only while it lasts. A new touch or a wheel turn stops it. The whole gesture logic is one function fed timestamped samples
  (`gp_scroll_touch`), which is also what `gp_simulate_touch(app, GP_TOUCH_DOWN/MOVE/UP, x, y, seconds)` calls, so `tests/touch_test.c` controls the speed to the millisecond
  and jumps the clock (taps and wobble, a drag with smooth scrolling on, a 1000 and a 3000 px/s flick against *v* × 0.35 in one step of the clock and in forty, the
  stale finger and the slow one, being stopped, the end of the content, a row, and a fling that passes from an inner scroller to its parent). I had no touch screen to try real
  events on: the SDL path (`SDL_EVENT_FINGER_*`, with SDL's touch-to-mouse synthesis turned off) is a few lines that only convert to the same function.
* **Auto-hiding** (`gp_scrollbar_autohide()`, on a scroller or a `gp_list`): the thumb is hidden until the content
  scrolls (wheel, thumb, `gp_scroll_to`) or the mouse is over the scroller, stays while either goes on (or while
  it is being dragged), and fades out over a quarter of a second, a second after the last of that. A hidden thumb
  cannot be grabbed; moving over the scroller shows it again. It costs nothing at rest: while hidden or shown
  steadily nothing is drawn or woken, a wake-up is set for the moment the hold ends (a window-bound timer), and only
  the fade itself asks for frames. Views with such a scrollbar stay cached: the window counts changes of visibility
  (`scroll_vis_gen`) and a painted copy is reused until it changes.
* Hover, click and focus hit-testing all use the scrolled coordinates, and content outside a
  scroller's box is neither visible nor hoverable.
* `gp_children(array, count)` appends a runtime-sized list of children (GPUI's `.children()`).
* **Scroll into view.** Tab, Shift-Tab and `gp_focus_next/prev` scroll every scroller around the element they focus by the least that
  shows it (its top edge or its bottom edge goes to the matching edge of the view, the border not counted), innermost scroller first, so
  that a scroller inside a scroller moves both by what each is short. An element already in view is left alone, as is one larger than the
  view and filling it; one larger than the view that begins inside it gets its top edge aligned. It happens once the next layout is
  done, so a row that grows with focus is measured at its new size. `gp_scroll_into_view(cx, focus)` does the same for any element with a
  `gp_track_focus` handle, e.g. after a search jumps to a result. Clicking, `gp_focus_set` and `gp_text_input`'s own caret reveal do not
  use it. An element not in the tree (a row of a `gp_list` that is scrolled out) is ignored; give a list `gp_list_scroll_to_item` instead.
  `tests/reveal_test.c` covers a bordered column, a row, nested scrollers, tall elements and wrapping around.

See `examples/scroll.c`: a 200-row list with keyboard paging, a log nested inside a scrolling
page (wheel chaining), and a horizontal strip.

## Virtualized lists

```c
static gp_node *row(void *self, gp_ctx *cx, size_t i, void *data) {
    return gp_div(gp_px(14), gp_on_click_data(select_row, (void *)i),
                  gp_text(gp_fmt("Row %zu", i)));
}

gp_uniform_list(100000, 32, row, NULL,          /* count, row height (0 = measure row 0) */
                gp_track_scroll(h), gp_flex_1()) /* needs a bounded height */
gp_list_scroll_to_item(cx, h, 5000, GP_ALIGN_ROW_NEAREST);
```

GPUI's `uniform_list`. The visible range depends on the viewport and the scroll offset, which
only layout knows, so the list builds **no children when the tree is built**. At layout it
works out the rows in view and calls `row(self, cx, index, data)` for just those, in the
context of the view that owns the list (so `gp_on_click` etc. dispatch to that view's state).

* Wheel and thumb-drag scrolling re-realize the visible rows **without rebuilding the view**.
* Rows live in a per-list arena that is reset on every re-realization: memory stays flat no
  matter how long you scroll (4,500 wheel events moved peak RSS by ~2 MB).
* Row identity is the *index*, so scroll offsets of lists nested inside rows survive scrolling.
* Rows inherit text style from the list. Strings from `gp_fmt` / `gp_frame_alloc` inside a row
  function live as long as the row does.
* Guard rail: more than 4096 visible rows (an unbounded list) is capped with a warning.
* Focus lives in the handle, not the element, so a focused row can scroll out of the realized
  range and back without being blurred; while absent, keys go to the focus path's root.
* Limits: every row must be the same height; positions are `float`, so keep
  `rows × height` under ~16 M px (≈ 500k rows of 32 px) to stay pixel-exact. (For rows of
  different heights use `gp_list`, below, which has no such limit.)

### Rows of different heights

```c
gp_list(count, 60, row, NULL,                  /* count, estimated row height */
        gp_track_scroll(h), gp_flex_1(),
        gp_list_stick_to_end())                /* optional: a chat log */
gp_list_splice(cx, h, 0, 0, 25);               /* 25 older rows were inserted at index 0 */
```

GPUI's `list`. Same contract as `gp_uniform_list` (the item function, a bounded height, the
handle), but a row is as tall as it lays out to: the list gives it its own width and takes
whatever height comes back. Heights are measured when a row is first built and remembered by
index in a Fenwick tree (`src/vlist.c`), so "where does row *i* start" and "which row is at
offset *y*" are both O(log n).

* **The scroll position is an anchor**, not a pixel offset: "this much into this row". Realizing
  builds that row, then rows downward until the viewport is covered, then upward for what is left
  above (and a quarter of a viewport of overdraw each way). Everything on screen is laid out from
  true heights; estimates never place a visible row. So a row above the viewport changing height,
  or being measured for the first time as you scroll up through it, does not move what you are
  reading. Wheel and thumb moves of up to ~3 viewports walk the actual rows from the old anchor,
  so they are exact to the pixel; longer jumps (dragging the thumb across 100,000 rows, `gp_scroll_to`
  far away) land by estimate, then correct themselves.
* **Unmeasured rows are estimated** as the average of the measured ones (the given estimate, or row 0's
  height if it is 0, before any). The extent and the thumb start out approximate and become exact
  as rows are seen; the scroll position does not jump when they do.
* **No float limit.** Rows are positioned relative to the first realized row, never in absolute
  content pixels, so a list of 3,000,000 rows (~150 M px, where a float can place pixels only
  to ±8) still lays out gap-free and scrolls by exact steps. Only the logical offset reported by
  `gp_scroll_offset` / the thumb is a `float`, and only to that precision.
* **Changes.** Rows appended or removed at the end need nothing: the count is picked up. Anything
  else tell the list about: `gp_list_splice(cx, h, start, old, new)` for inserting, removing or
  replacing rows (the row at the top of the viewport stays where it is, even when the change is above it, which
  is what loading older messages needs), `gp_list_invalidate` for rows whose content changed,
  `gp_list_reset` for everything. A visible row that changes height needs no call; rows off screen
  are remeasured the next time they are built, so these calls mostly keep the extent honest. The
  measurements are dropped when the list's width changes (wrapped text has other heights),
  keeping the row you were on.
* **`gp_list_scroll_to_item`** works on both kinds. On a `gp_list` the move is made by the next
  layout, which knows the heights: it lands on the row exactly, even one never seen.
* **`gp_list_stick_to_end()`**: the list starts at its end and stays there while rows are added
  or grow; scrolling up releases it, scrolling back to the end (or `gp_scroll_to_end`) takes hold again.
* Limits: the extent of rows nobody has seen is an average, so a thumb dragged across a very
  uneven list lands approximately; multiple windows showing one list each keep their own heights,
  and `gp_list_splice` etc. reach only the first window; rows have no margins (the list ignores them).

See `examples/list.c`: 100,000 fixed rows with keyboard selection that follows the cursor, 5,000
cards of different heights, and a chat log that follows new messages and takes older ones from
above without moving.

### Horizontal lists

`gp_list_horizontal()` on a `gp_uniform_list` lays the items out side by side and makes the list scroll sideways, so a carousel, a timeline or a strip of columns
of any length is as light as what is in view. The list's `item_height` argument becomes the items' **width** (0: the first item's own width), the items are as tall as
the list (give it a height; without one it is as tall as its first item, and does not fill the height it is offered, as a strip should not), and the list fills the
available width. Everything is the vertical list turned a quarter: the realized range follows `scroll.x`, item `i` sits at `padding + i * width`, a thumb
appears on the x axis only, the wheel's sideways motion (Shift + wheel, a trackpad), touch drags and flings, `gp_scroll_to(h, x, 0)` and smooth steps move it, and
`gp_list_scroll_to_item` aligns in x (start, centre, nearest). Not with `gp_list_sticky`.

**On a `gp_list` the items may be of different widths.** The same variable-extent machinery as the vertical `gp_list` runs along x: the scroll position is an anchor (the item at the left edge and how far into it), items are
built from the anchor outwards until the view (and some overdraw) is covered, each measured as it is built (at the list's height, its natural width), and the widths are remembered in
the same `gp_vlist` that keeps heights, so the extent is an estimate (the average of what has been seen, or the first item's width) that becomes exact as items are seen, a jump lands on the estimate and
the next layout corrects it, and `gp_list_splice`, `gp_list_invalidate`, `gp_list_reset`, `gp_scroll_to_end` (the right end) and `gp_list_scroll_to_item` work as they do down the page (a splice keeps
the item at the left edge where it is). The list is as tall as its first item unless given a height (and then the items are as tall as the list: a new height measures every item again), and as wide
as it is offered. The scroller's bookkeeping needed an x counterpart of what a column keeps for its y (`gp_el.scroll_base_x`: a long list keeps its offset small and adds the base), threaded through the wheel,
touch, thumb, thumb drag, page-by-click, smooth scrolling and scroll-into-view. `tests/hvlist_test.c`: widths from a table (40 + (i mod 7) * 20), so every item built is checked for its place
(to the pixel, at many offsets), width, text and height; the extent against the sum of the widths once seen; wheel, thumb and its drag, touch, every alignment of scroll-to-item, a far jump, scroll to the end,
splices at the front, a height change, padding, and a list with no height. `tests/hlist_test.c`: 100,000 items, the built range and positions, the extent, the thumb, the wheel
(a vertical turn does nothing), a jump to the middle, every alignment of scroll-to-item, clicks, a touch drag, items with no sizes given, padding and a border.

### Sticky group headers

`gp_list_sticky(fn, data)` on a `gp_uniform_list` or a `gp_list` pins a header at the top of the view for as long as its group is on screen, and the next group's
header pushes it up as it arrives (section headers on a phone, CSS `position: sticky`). `fn(self, cx, row, data)` is asked, for the first row that is at least
partly in view, which group it is in: it returns `gp_list_group { header, first, next }`: the element to pin (built like a row; NULL for a row with no group), the
index of the group's first row, and the index of the next group's first row (past the end if none). The pinned copy appears only once the group's own header row
(row `first`) has gone past the top, so at the top of the list, or when a header row is exactly at the top, nothing is drawn twice. It is as wide as the rows
and as tall as it lays out, is drawn above them, takes the mouse in its own area (clicks, hover, cursor) while the rows under it take theirs, and is rebuilt as the list
scrolls (wheel, thumb, `gp_scroll_to`, smooth steps, flings), in a small arena of its own, so `fn` should be cheap. It is not one of the list's children and is not
counted among its rows. `tests/sticky_test.c` walks both kinds of list through 150+ offsets each (17 px apart, so every kind of position), comparing the pinned header
with where the rows really are, and for the uniform list with arithmetic from the offset alone; then jumps, the wheel, clicks above and below the header's bottom edge,
and the pixels. Pinning a header in an ordinary scroller (not a list) is not done: there, give the header element a place in an overlay of your own and move it from a
`gp_on_scroll` handler.

## Timers and async

Everything runs on the main thread inside the event loop, except `gp_post` (any thread) and the
`work` half of `gp_background` (a pool thread). Callbacks belong to an entity: they get its state
as `self`, are silently dropped if it has been released, and tasks owned by a released entity are
cancelled. Use `gp_ctx_for(app, GP_ENTITY_NULL)` for app-level work. Every call returns a
`gp_task` handle for `gp_cancel` / `gp_task_active`; stale handles are harmless.

```c
gp_after(cx, 2.0, hide_toast, NULL);            /* one-shot; cancel + re-arm to restart */
gp_every(cx, 0.53, on_blink, NULL);             /* no drift; missed ticks are skipped */
gp_animate(cx, 0.8, GP_EASE_OUT_BOUNCE, tick, d);   /* tick(self, cx, t, data), last t == 1 */
gp_next_frame(cx, on_frame, NULL);              /* requestAnimationFrame; re-arm to loop */

gp_background(cx, work, done, data, free);      /* work: pool thread. done: main thread */
gp_post(app, entity, fn, data);                 /* thread-safe: worker -> main thread */
```

* **The loop sleeps.** Its timeout is derived from the next timer or frame request, and worker
  threads wake it with an SDL event. An idle app draws nothing: `counter` woke 4 times in 6 s;
  `form` woke on its 0.53 s blink timer but drew once, because the callback skips `gp_notify`
  when the field is unfocused.
* **Frames are paced to the display** (it asked for 240 Hz here and got 4.3 ms between draws)
  and only while someone has requested one.
* **`gp_background` data lifetime:** `data` stays valid until `free_data` runs on the main thread,
  exactly once, whether `done` ran, the job was cancelled, or the entity vanished. A cancelled job's
  `work` still runs to completion (it is never interrupted), so nothing is left with dangling data.


`gp_task_on_drop(app, task, drop, data)` and `gp_subscription_on_drop(app, sub, drop, data)` attach a destructor to a task or subscription: `drop(data)`
is called exactly once, on the main thread, after its callback could last have run (it finished, was cancelled, its entity or window went, or the app
shut down). It is how the C++ layer owns its closures, and it is there for C code that heap-allocates a callback's `data` and has no other
way to know when to free it. A subscription's drop runs at the next flush of effects (`gp_flush_effects`), never inside the callback that unsubscribed; on a handle that is already
over it runs at once.

### Timers that belong to a window

A ctx with a window ties what is started with it to that window. A ctx handed to an event handler, a layout
handler or a render function has one (`cx->window`); `gp_ctx_for_window(app, window, entity)` makes one; plain
`gp_ctx_for(app, entity)` does not, and its work lives as long as the app (or its entity).

* **Timers, frame requests, animations, descriptor watches and coroutines** started with a window's ctx are cancelled
  when that window closes (`gp_window_close(w)`, or the user closing it), in the order the loop notices it: a
  coroutine unwinds through its awaits like any cancelled one, *before* the window is destroyed, so its cleanup can
  still use `cx->window`; one waiting for background work unwinds when the work finishes, and by then its ctx no
  longer points at the window. Nothing keeps a pointer to a closed window (a new window can be allocated at the same
  address).
* **Callbacks see the window** they were started for as `cx->window`; a re-armed `gp_next_frame` called with that ctx
  stays on it.
* **Frames are paced per window**, by the refresh rate of the display that window is on, instead of everything
  following the first window. Work with no window follows the first one, as before.
* The last window closing ends the app, which cancels the rest.

### Watching file descriptors

Pipes, sockets, eventfds, inotify, a child process's stdout: anything `poll(2)` understands wakes the
event loop and runs your code on the main thread, like every other callback here.

```c
gp_fd_set_nonblocking(fd);
gp_task t = gp_watch_fd(cx, fd, GP_FD_READ, on_readable, data);   /* fn(self, cx, fd, events, data) */
gp_cancel(app, t);                                                /* stops watching; the fd stays yours */
```

* **Level-triggered**, as in `poll(2)`: the callback runs again for as long as the descriptor stays ready,
  so read (or write) until it would block, or cancel. `events` says what happened: `GP_FD_READ`,
  `GP_FD_WRITE`, `GP_FD_ERROR`, `GP_FD_HUP` (a closed pipe or socket; together with `READ` if data is
  left) and `GP_FD_INVALID` (the descriptor was closed under the watch: the callback runs once and the watch
  ends).
* **Nothing polls on a timer.** A small thread sleeps in `poll(2)` on the watched descriptors plus an
  eventfd, and wakes the loop (the same SDL event `gp_post` uses) when one is ready. The loop itself stays in
  its single wait, so windows, timers and animations are untouched; an idle watch costs no wakeups (the test
  counts them). A descriptor is handed to the main thread once, and only watched again after its callback
  returned, so a slow callback never piles events up.
* **Owned like the rest:** tasks belong to the entity in `cx`, which receives its state as `self` and
  cancels them when it is released. Cancel before you `close()`.

As an await point, in a coroutine, for the common non-blocking calls:

```c
static void fetch(gp_co *co, void *self, gp_ctx *cx, void *data) {
    int fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
    if (gp_co_connect(co, fd, (struct sockaddr *)&addr, sizeof addr) < 0) return;   /* errno says why */
    gp_co_write(co, fd, "GET / HTTP/1.0\r\n\r\n", 18);
    char buf[4096];
    ssize_t n = gp_co_read(co, fd, buf, sizeof buf);       /* waits for data; 0 at end of stream */
    close(fd);
    ...
}
```

`gp_await_fd(co, fd, events, timeout, &revents)` is the primitive (negative timeout = no limit; `revents`
is 0 if it timed out); `gp_co_read`, `gp_co_write` (all of it), `gp_co_accept` and `gp_co_connect` retry
on `EAGAIN` by awaiting. A cancelled coroutine gets `false` / `-1` with `errno == ECANCELED`. See
`examples/fd.c` (stdin lines as they are piped in, and an in-process TCP echo server and client, each a
coroutine).

### Declarative animations

`gp_animate` calls a function every frame and leaves the redrawing to you. `gp_with_animation` is the
GPUI `with_animation`: the *element* animates itself, with no view rendering per frame.

```c
static gp_node *pulse(float t, void *data) { return gp_opacity(gp_lerp(0.25f, 1.f, t)); }

gp_div(gp_size_px(14), gp_rounded(7), gp_bg(green),
       gp_with_animation("status",
                         (gp_animation){ .duration = 1.1, .repeat = true, .ping_pong = true,
                                         .easing = GP_EASE_IN_OUT },
                         pulse, NULL))
```

The function gets the eased progress and returns the style modifiers to lay over the element's own:
one, or `gp_group(gp_opacity(t), gp_w(40 + 60 * t), gp_bg(c))` for several. `gp_animation` has
`duration`, `delay`, `repeat`, `ping_pong` (with `repeat`: every other run goes backwards) and `easing`
(`GP_EASE_*`; `OUT_BACK`, `OUT_BOUNCE`, `SPRING` and `SPRING_BOUNCY` overshoot).
`GP_EASE_SPRING` (damping ratio 0.5, 16% overshoot) and `GP_EASE_SPRING_BOUNCY` (0.25, 44%, several swings) are the step response of a
damped spring, `x'' = -2 z w x' - w^2 (x - 1)`, with `w` chosen so that it has settled to a thousandth at the end of the duration (and then
set to exactly 1); `gp_ease_spring(t, damping)` is the same for any damping ratio from 0.1 to 1 (critically damped: the quickest way there
without overshoot). The test integrates the equation itself and compares. It is a curve over a fixed duration, not a simulation that carries
velocity: a spring restarted halfway through starts again from rest.

**`gp_spring`** is the physical one: `x'' = -k (x - target) - 2 z sqrt(k) x'` with a position, a velocity and a target, kept in a view's state.
`gp_spring_to(cx, &spring, target)` sets the target and has the spring stepped once a frame (notifying the view that reads `spring.value`) until it
settles, then asks for nothing; calling it again while it moves only changes the target, so the position **and the velocity** carry on (a panel
following the pointer, a scroll position being flicked back), which a curve over a fixed time cannot do. It is advanced by the closed-form solution
(underdamped, critically damped and overdamped), so a long frame cannot make it unstable, and `gp_spring_step(&s, dt)` does the same by hand, e.g. from a
`gp_next_frame` of your own. It lands on the target exactly once both the distance (0.005) and the speed (0.05) are tiny; `gp_spring_jump` puts one at
rest somewhere. `tests/exec_test.c` holds it to an RK4 integration of the equation (the three regimes, a second at 60 Hz, a long step against 400 short ones,
the textbook overshoot, a retarget in mid-flight against the piecewise solution, and that the frame job runs once and ends).

* **How it runs:** before every layout the tree is walked, but only along the paths that lead to an
  animated element; each animation's progress is worked out from the clock, and an element whose
  progress moved gets its style reset to what it was built with and the returned modifiers applied on top
  (nothing accumulates, and what an earlier half of an animation set is gone in the next). Layout and paint
  then run again. While anything is unfinished the window asks for a frame at the display's rate; when
  everything has finished it asks for none, and the loop sleeps (a finished animation keeps showing its last
  value, also after the view rebuilds).
* **The view does not render.** `examples/animate.c` shows a view that `gp_child_view_cached` keeps cached
  while a spinner inside it animates: its render count stays at 1. What an animation costs a cached view is
  its cached layout and painted copy, dropped for the roots around the element that moved, and only while
  it moves.
* **Identity is the id.** The window remembers when each named animation started; rebuilding the element
  continues it. Give animated elements that are on screen together different ids (two animations on one
  element are told apart by position). A new id starts a new run, which is how "open" and "close" are
  separate runs of one panel; an element absent from a layout is forgotten and starts over when it returns.
  In a `gp_list`, rows are animated as they are built and keep their progress while the list is not laid out
  (wheel scrolling); rows that scrolled away during a layout start over on the way back.
* **Style only.** Size, color, opacity, padding, text size... not handlers or children. An animation
  function cannot add animations. The modifiers it returns are built in a scratch arena that is reset
  before the next element, so `gp_fmt` strings in them do not outlive the call.
* **Testing** can jump the clock (`gp_debug_clock_skew` in `internal.h`) instead of waiting, which is how
  `tests/anim_test.c` checks exact progress values, repeats, delays and ping-pong.

### Transforms: rotate, scale, translate

```c
static gp_node *spin(float t, void *data) { return gp_rotate(t * 360.f); }
gp_img(spinner, gp_size_px(32), gp_with_animation("busy", (gp_animation){ .duration = 1, .repeat = true }, spin, NULL))
gp_div(gp_w(120), gp_h(40), gp_scale(1.1f), gp_translate(0, -2), gp_hover_bg(c))
```

`gp_rotate(degrees)` (clockwise: y goes down), `gp_scale(s)` / `gp_scale_xy(sx, sy)`, `gp_translate(dx, dy)` and `gp_transform_origin(fx, fy)` are, as in CSS, **paint-time**: nothing in the
layout moves or resizes; they apply to the element and everything in it, about the middle of its box (the origin is a fraction of the box: `0, 0` is its top left), in the order scale, rotate,
translate; nested ones compose. They are made for `gp_with_animation` (which could not reach more than size, color and opacity before) and cost nothing but an animation's layout.

* **How:** every instance carries the matrix it was painted under (2 x 3, as two more `vec4`s: the instance is 208 bytes now; all zeros is the identity), and the vertex shader maps the
  quad's corners through it while the fragment shader keeps working in the element's own coordinates, so rounded corners, borders, gradients, shadows, images and glyphs are all drawn the same way,
  turned. The edge softness follows the magnification (uniform scales and turns are exact; a stretch along one axis only leaves edges across it a little softer than a pixel). Text under a transform
  is not LCD antialiased (its stripes are not the screen's).
* **The pointer follows:** a click, a hover, a cursor and a mouse-down land on what is seen where the pointer is, not where the box would have been, and the event's `local` position is in
  the element's own coordinates (through the inverse of the matrix; a drag after a press keeps the offsets it started with). `gp_hit` has the point in the element's coordinates for it.
* **Clips:** an `overflow: hidden` box (and a scroller) inside a transformed element is cut to the box as it is seen when the transform keeps the axes (a scale, a move, a quarter turn); at any
  other angle there is no clip (a clip is an axis-aligned rectangle in screen space). A clip *above* a transformed element applies to it as usual.
* **Cached views:** a cached view painted under a transform is repainted when the matrix above it changes (the matrix is in the painted copy's signature) and is never just moved
  (`tests/paintshift_test.c`'s shifting is for views with no transform above or below). `tests/transform_test.c` compares frames: a quarter turn exactly, 45 degrees against a supersampled box,
  scales, moves, origins, nested, turned text (same ink, width and height swapped, no color fringes with LCD on), clicks (position included) and hover on a turned box and not where it was, clipping,
  a cached view under a turn by an animation against a full paint, and an animation of a rotation (with the clock jumped), and about 20 mutants.
* **Not done:** a skew, 3D, `transform` on a scroller's scrolling content (they work, but wheel events find scrollers without the matrix), and a drag handler's offsets under a rotation.

### Coroutines: async/await for C

Sequential code that suspends at await points while the loop keeps running, on its own
256 KiB guard-paged stack (`ucontext`):

```c
static void fetch(gp_co *co, void *self, gp_ctx *cx, void *data) {
    Job job = { .limit = 4000000 };                    /* lives on this coroutine's stack */
    if (!gp_sleep(co, 0.5)) return;                    /* false => cancelled: unwind */
    if (!gp_await_background(co, crunch, &job)) return;
    ((View *)self)->result = job.count;  gp_notify(cx);
}
gp_task t = gp_spawn(cx, fetch, NULL);                 /* starts on the next loop turn */
gp_cancel(app, t);
```

`gp_sleep`, `gp_yield`, `gp_await_frame`, `gp_await_background` and `gp_await_fd` (with the `gp_co_*`
I/O calls built on it, above) are the await points.
Cancellation is **cooperative**, which is what makes it leak-free: awaits on a cancelled
coroutine return `false` immediately without suspending, and the body returns normally through
its own cleanup code. Rules:

* Don't touch `self` after an await returned `false` — the entity may be gone.
* Cancelling a coroutine that waits for background work takes effect when that work finishes
  (the worker holds pointers into the coroutine's stack, so it must not unwind earlier).
* Cancelling a sleeping or yielding coroutine resumes it at once, from inside `gp_cancel`
  (or `gp_entity_release`), so it unwinds before the call returns.
* At exit `gp_application_run` cancels everything, joins the pool and unwinds every coroutine
  before entities are freed.
* Stack depth is fixed; keep big buffers off it. A blocking call blocks the whole UI, as anywhere.
* `ucontext` needs glibc/macOS-style libc (not musl); the platform-independent part is only the
  executor, so a different coroutine backend could slot into `coro.c`.

See `examples/async.c`: a stopwatch, a coroutine fetch with a frame-driven spinner, a worker
reporting progress via `gp_post`, and an interruptible eased animation. In `examples/form.c`
the caret blinks (and restarts on every edit) and the "Saved" status clears itself.

### Font families and weights

Both are text properties, so they inherit down the tree like size and color:

```c
gp_div(gp_font_family("Inter, Noto Sans, sans-serif"),        /* a list, like CSS */
       gp_text("Title", gp_font_weight(600)),                 /* 1..1000 */
       gp_text("Body"),
       gp_div(gp_font_mono(), gp_text("let x = 1;")))         /* also gp_font_serif(), gp_font_sans() */
```

* **Weights.** `gp_font_weight(w)` takes 1 to 1000 (`GP_WEIGHT_LIGHT` 300, `GP_WEIGHT_NORMAL` 400,
  `GP_WEIGHT_MEDIUM` 500, `GP_WEIGHT_BOLD` 700, ...; `gp_font_bold()` and `gp_font_normal()` are 700 and
  400). A family rarely has a face for every weight, so the face is chosen the way CSS does it: the exact
  weight if there is one; for 400 to 500 the weights up to 500 first, then lighter, then heavier; below 400
  lighter faces first; above 500 heavier first. Where a family has nothing heavier than 600 and 600 or more
  is asked for, the face is thickened (**synthetic bold**, below).
* **Families.** `gp_font_family("A, B, C")` is a list. Names are matched without regard to case against
  the family a font names itself (its typographic family, so "Noto Sans Light" belongs to "Noto Sans" and
  is chosen by weight); one that is not installed or registered is skipped. A character the first family
  lacks is drawn from the next, and then, as before, from the other registered fonts and the system.
  The generic names are `sans-serif` (also `system-ui`; what you get with no family at all), `serif` and
  `monospace`. A list that names no generic one ends in the default, as in a browser, so a missing
  family falls back to the default font rather than to nothing. The system's answer for a generic is
  fontconfig's, or, without fontconfig, the first of a list of common serif or monospace families
  that is installed (any fixed-width face for monospace).
* **Inheritance and cost.** A closer property overrides a farther one, each independently: a bold box
  inside a serif box is bold serif. Changing them re-lays-out the text below, including in cached views
  (which are not rendered again, but whose elements are laid out with the new inherited font, tested in
  `tests/fonts_test.c`). A text whose weight moves (an animation) is cheap: system lookups are rounded to
  weights of 10 and remembered (measured: 800 distinct weights in 194 ms the first time, nothing after).
* **Variable fonts.** With fontconfig, the *named instances* of a variable font (Adwaita Sans's Light,
  Bold, Black, ...) are separate weights of one family and one of them is chosen as for any family. The **weight axis** (`wght`)
  is then moved to the weight asked for: `gp_font_weight(412)` draws and measures at 410 (a face is opened per step of 10
  on first use, so animating the weight costs at most ~50 faces), clamped to the axis (a request past 900 is the axis's end, and a font
  whose axis reaches bold is never thickened synthetically); a request within 5 of the face's own weight keeps that face. A variable
  font registered from a file or memory (`gp_font_add_file` / `gp_font_add_memory`, the default instance or a named one) gets the same. The other axes stay where
  the instance put them unless **`gp_font_variations("opsz" 32, "wdth" 87.5)`** (CSS's `font-variation-settings`, also `opsz=32`) sets them: inherited like the
  weight, replacing the list above it (`"normal"` puts every axis back), values clamped to the axis and kept to the nearest half so that
  animating one makes at most a face per half-step, an axis the font does not have ignored (and a list naming none of a font's axes costs no face),
  a `wght` in the list winning over `gp_font_weight` (without synthetic bold: the face covers its axis, as in CSS), up to 8 axes. The text system
  takes it as part of the font selector (`GP_FONT_VARIATIONS`, 15 bits above the features), so shaping, measuring and cached views follow it.
  Tested against FreeType set to the same coordinates (`tests/text_test.c`, `test_variations`; through layout and cached views in `tests/variations_test.c`)
  on Adwaita Sans, which has optical size and weight. Without fontconfig the scan lists a variable font
  once, at its default instance, and the weight axis moves from there. Tested against FreeType set to the same axis value on its own
  (`tests/text_test.c`, `test_variable_weights`, for each variable font installed).
* **Italic.** `gp_font_italic()` (and `gp_font_upright()` to undo it inside) is inherited like the rest. The face is
  chosen by the same rules, with italic and oblique faces preferred (a roman one is the last resort, and the
  other way round for upright text: an italic face is chosen for upright text only when it is all a family has).
  A family with no italic face is drawn **synthetic oblique**: the upright face's outlines sheared by 0.25 (14 degrees,
  the same as browsers) about the baseline when rasterized. Advances are not touched, so measuring and drawing
  agree, and it composes with synthetic bold (bold italic of a family with only a regular face is one variant of
  that font, thickened and sheared). A fallback font (CJK, symbols) is slanted too; color emoji never. A real
  italic face is never sheared again. Italic is part of the system lookup (`FC_SLANT_ITALIC`; without fontconfig
  the scan prefers faces whose style flags say italic), and of the face registered with `gp_font_add_*` (read
  from the font's own style flags).

### OpenType features

`gp_font_features("tnum, liga=0, ss01")` turns OpenType features on and off for the text below (HarfBuzz's own
syntax: `tnum`, `+tnum`, `liga=0`, `-liga`, `cv02=3`). Sugar: `gp_font_tabular_nums()`, `gp_font_small_caps()`,
`gp_font_no_ligatures()`. Like the rest it inherits, and a list **replaces** the inherited one as in CSS
(`gp_font_features("normal")` clears it). Ligatures, kerning and the script's required features are on by
default, as before; a font only does what it has (Noto Sans has `tnum` `pnum` `onum` `lnum` `smcp` `c2sc` `frac` `zero`
`liga` `kern`; its mono has none of the figure ones). Notes: ranges (`liga[3:5]`) are ignored, because text is
shaped in runs, so a feature applies to a whole text element; an item HarfBuzz can't parse is skipped and the rest
still applies; feature lists are interned (a list costs one parse), part of the shape cache key, and layout and paint
use the same one, so widths always match what is drawn. Features apply to fallback fonts too (a font that lacks
them ignores them). Not covered: variable-font axes (`font-variation-settings`) and `font-variant-*` shorthands.

### Bundling fonts

With nothing registered, fonts come from the system (above). To make an app independent of what is
installed, ship the files, or embed them as byte arrays, and register them in the `ready` callback,
before the first window opens:

```c
static void ready(gp_app *app, void *userdata) {
    gp_font_add_file(app, "assets/Inter-Regular.ttf", NULL);       /* the family comes from the font: "Inter" */
    gp_font_add_file(app, "assets/Inter-Bold.ttf", NULL);
    gp_font_add_file(app, "assets/Lora-Regular.ttf", &(gp_font_options){ .family = "serif" });
    gp_font_add_memory(app, noto_cjk_bytes, sizeof noto_cjk_bytes, NULL);        /* copied */
    gp_font_add_file(app, "assets/Emoji.ttf", &(gp_font_options){ .weight = 400 });
    gp_fonts_use_system(app, false);   /* optional: never touch fontconfig or scan font directories */
    ...
}
```

* Registered fonts win over system fonts. They belong to the family the font names itself, or the one in
  `gp_font_options.family`, which also defines a generic family: above, `gp_font_serif()` draws Lora. Text
  that names no family uses the registered family called `"sans-serif"` if there is one, else the
  family of the font registered first (here Inter). The weight of a face comes from the font's OS/2
  table unless `gp_font_options.weight` says otherwise (a font with a wrong or missing weight).
  Within a family the weight is matched as described above. A bold request (600 or more) that lands on
  a font lighter than 600 (one registered font, or a system family without a bold face) is drawn with
  **synthetic bold**: outlines thickened by size/24 and every advance widened by the same amount, so measuring
  and drawing agree. It is thinner and tighter than
  a real bold face, so register one when it matters. Real bold faces, color glyphs and zero-width
  marks are never touched.
* A character the chosen font lacks comes from the rest of the family list, then the other registered
  fonts (the nearest weight wins, a tie goes to the earlier one), then from the system, unless it was
  disabled. A registered color font (bitmap CBDT, like Noto Color Emoji) draws emoji.
* `gp_fonts_use_system(app, false)` makes the app hermetic: what the registered fonts lack shows
  as the primary's missing-glyph box, and no font directory is ever scanned. With nothing
  registered either, there are no fonts and text draws nothing (it logs
  `could not load a sans-serif font`).
* The calls return `false`, changing nothing, for a file FreeType can't open, an out-of-range
  weight (valid: 0, or 100 to 900) or a bad face index. Registering a file again only updates its
  weight. `face_index` selects a face of a `.ttc` collection.
* Registering later (after windows exist) is supported and tested at the text-system level:
  cached font choices are dropped and the windows are scheduled to rebuild. I did not check that
  an existing text input re-wraps its content, so register at startup.
* Variable fonts: the default instance, unless you pass a named instance in `face_index`
  (`(instance + 1) << 16 | face`, as FreeType numbers them); the weight axis follows `gp_font_weight` from there.

## Fonts and text shaping

Text in any script just works if a font for it is installed: there is nothing to configure.

* **Fallback.** A codepoint the primary font (of the family and weight asked for) lacks is looked up in the
  fallbacks loaded so far, and only then asked of fontconfig, once. fontconfig costs about 1.3 ms
  per query, so doing it per character would stall for seconds on a CJK page (measured: 2,609 ms
  for 2,000 ideographs); this way it costs one query per *font*, then a cmap lookup. Every
  codepoint's font is cached, including "nothing has it", which draws the primary font's `.notdef`
  box. Bold text asks for bold fallbacks. In a CJK locale (`LANG=ja_JP...`) fontconfig is given the
  language, which selects the regional glyph shapes of shared Han characters.
* **Emoji.** Emoji-presentation codepoints, and anything followed by U+FE0F, go to a *color* font
  (fontconfig's first match for an emoji is usually a monochrome DejaVu glyph, which is why
  this is explicit). Color glyphs are premultiplied RGBA in a second atlas and are drawn as they
  are: never tinted by the text color, only faded by opacity. A CBDT font like Noto Color Emoji has
  a single 109 px bitmap strike, so it is downscaled with an area filter to the target size.
* **Sequences.** The itemizer keeps ZWJ, variation selectors, combining marks, skin tones and the
  partner of a ZWJ with the previous run's font, and HarfBuzz applies the font's own ligatures: a
  family (3 emoji + 2 ZWJ, 18 bytes), a flag, a skin-toned hand or a keycap each shape to **one**
  glyph. Glyph positions carry a *cluster* (byte offset), so a sequence is one unit for the caret.
* **CJK line breaking.** Text without spaces wraps between ideographs and kana, with kinsoku: a
  line never starts with closing punctuation or small kana, nor ends with an opening bracket.
  (Simplified UAX #14.) Hangul and Latin wrap at spaces.
* **Subpixel positioning.** Layout works in font units, so advances are exact; painting used to round every glyph's pen
  position to a whole pixel, which made the spacing of a line uneven by up to half a pixel from one glyph to the
  next (visible in repeated letters and at small sizes). Now a glyph is rasterized for the quarter of a pixel its pen
  origin falls on (`GP_SUBPIXEL_BINS` = 4, so the position is good to an eighth), by translating FreeType's outline
  before it is rendered: light hinting only touches the vertical direction, so shapes stay sharp. Each (glyph, size,
  quarter) is its own atlas entry, so the atlas holds up to four copies of a glyph in use. Not applied where it
  would not show or not apply: sizes above 40 device px, and color bitmap fonts (one shape per strike). The vertical
  position still snaps to a whole pixel, so baselines stay crisp. `GPUIC_SUBPIXEL=0` switches it off (whole
  pixels, as before), for comparing. `tests/text_test.c` measures where the ink of a glyph lands for each quarter, and
  that a run of identical glyphs at a fractional advance is evenly spaced.
* **LCD antialiasing.** `gp_fonts_set_antialiasing(app, GP_AA_LCD_RGB)` (or `GP_AA_LCD_BGR`, `GP_AA_LCD_VRGB`, `GP_AA_LCD_VBGR`, `GP_AA_GRAYSCALE`,
  `GP_AA_AUTO`; `GPUIC_LCD=rgb|bgr|vrgb|vbgr|off|auto` at startup) draws outline text with one coverage value per color
  stripe of an LCD pixel instead of one per pixel: FreeType renders the glyph at three times the horizontal resolution
  (`FT_RENDER_MODE_LCD`, with its default filter), the three samples go into an RGBA atlas of their own (1024², alpha
  unused), and the fragment shader blends each channel with its own coverage. That needs dual-source blending
  (`GL_ONE, GL_ONE_MINUS_SRC1_COLOR`: part of OpenGL 3.3, which is already required), and it is exact:
  `out = color * cov + dst * (1 - cov)` in every channel, with the text's opacity folded into the coverage, so
  fading, colored text and colored backgrounds work (the test checks that every channel stays between the text's and
  the background's). Nothing else changed: subpixel positions (the quarters above) apply as before; color emoji and
  bitmap strikes are never LCD; the ways of drawing a glyph are separate cache entries. **Vertical stripes** (`VRGB`: red on top, `VBGR`: blue on top; for a
  panel turned a quarter, and what fontconfig's `rgba` says for one) are rendered at three times the vertical resolution (`FT_RENDER_MODE_LCD_V`), and the three rows of
  samples to a pixel go to the same RGBA atlas, so the shader is the same; `text_test` compares the cells with FreeType's own `LCD_V` rendering sample for sample. `GP_AA_AUTO`, the
  default, asks fontconfig what the display's subpixel order is (its `rgba` setting, which stays "unknown" unless the
  user configured it, so on most setups the default is grayscale) and only does that below 1.5x scale, where it helps;
  without fontconfig it is grayscale unless you say otherwise. An explicit mode applies at every scale. Its limits are
  the technique's: color fringes on any panel with another subpixel order, in screenshots, when text is rotated or
  scaled (there is none here), and it blends in gamma-encoded space like the rest of the renderer, so light text on
  dark looks a little thinner than dark on light (unless you turn on **text gamma**, below). `tests/lcd_test.c` reads frames back from the renderer
  (`gp_debug_capture_next`) and checks fringes, per-channel bounds, opacity and the modes; `text_test` checks the
  rasterization (channel sums, BGR as the exact swap of RGB, quarter-pixel shifts, atlas bookkeeping).
* **Text gamma.** `gp_fonts_set_text_gamma(app, 2.2f)` (or `GPUIC_TEXT_GAMMA=2.2`; 1, the default, is no change) adjusts each glyph pixel's coverage in
  the fragment shader so that blending in the encoded space gives what blending in linear light would. The blend cannot read what is behind the
  text, so it assumes a background of the opposite luma to the text's (white behind black text, black behind white, the usual cases):
  with luma `lt` and `lb = 1 - lt`, the coverage `c` becomes `(g - lb) / (lt - lb)` where `g = (((1 - c) lb^γ + c lt^γ))^(1/γ)`; mid-gray text, where the
  two blends agree anyway, is left alone, and on a background that is not the opposite the correction is approximate. LCD text is adjusted stripe by stripe, emoji not at all,
  and changing the gamma repaints without rebuilding anything (the scene is the same; only the shader differs). `tests/gamma_test.c`
  reads frames back and compares every pixel a glyph alone covers (about 17,000 samples a mode) with the pixel computed from the glyph's coverage in the atlas: black on white and
  white on black, grayscale and LCD, at gamma 1 (the old output) and 2.2.
* **Atlases** are 2048² coverage, 1024² color and 1024² LCD. When one fills up the frame finishes without the
  glyphs that did not fit, both atlases are cleared, and the window repaints (verified
  pixel-identical to a run with no resets).
* **Limits.** No vertical text. Color is
  implemented and tested for fixed-size **CBDT** bitmap fonts (Noto Color Emoji) only; COLR/CPAL,
  SVG and sbix fonts are not handled and would draw as plain outlines or not at all. When wrapping,
  each word is measured separately while a line is painted as one shaped run, so kerning across a
  space can differ by a fraction of a pixel between the two. Text is never shaped across elements.

Cost: shaping is ~6 µs per cold 65-character line and ~3 µs cached, so a screenful is well under a
millisecond.

## Bidirectional text

Hebrew, Arabic and the other right-to-left scripts work without configuration: text is reordered per
the Unicode Bidirectional Algorithm (UAX #9) and each run is shaped in its own direction (Arabic
letters join, lam + alef ligates, brackets mirror). See `examples/bidi.c`.

```c
gp_text("שלום עולם");                         /* right-to-left by itself, aligned to the right */
gp_text("The word שלום means peace");         /* left-to-right paragraph with an RTL run inside */
gp_text("one two", gp_text_dir_rtl());        /* force a direction */
gp_text("שלום", gp_text_end());               /* `start` / `end` follow the paragraph direction */
```

* **Paragraph direction** is taken from the first strong character (digits and punctuation do not
  decide), per paragraph (a paragraph runs to the next line break); `gp_text_dir_ltr / _rtl / _auto`
  override it, and it is inherited. Alignment gains `gp_text_start()` / `gp_text_end()`, which mean left
  or right depending on that direction (`start` is the default, so Hebrew right-aligns on its own).
* **How it is done.** FriBidi resolves embedding levels and applies rules L1 and L2; the reordering is
  done on *runs* (one font, one level), each shaped by HarfBuzz in its own direction, so the shaper
  returns glyphs in visual order with `cluster` still a logical byte offset. A wrapped paragraph is
  wrapped in logical order and each line is reordered on its own, always with the *paragraph's*
  direction (a line holding only digits is still part of a Hebrew paragraph). Text with no
  right-to-left characters skips all of it: Latin shapes in 6 µs per line before and after, Hebrew in
  13 µs.
* **Text input.** Every caret stop is a *cell* with its own box and direction, so the widget never
  assumes logical order is visual order:
  * **Caret and affinity.** One offset can have two places on screen where the direction changes
    (the end of the character before, the start of the one after). The `upstream` affinity from soft
    wrap now also says which: typing Hebrew after Latin keeps the caret beside the Latin and the new
    letters grow leftwards, and a click picks the *nearest cell edge*, so the right half of a Hebrew
    letter lands before it.
  * **Arrow keys** step across the next *visible* character in rows that involve RTL text, so each press
    moves the caret one character on screen, wherever the logical order has gone; at the visual end of
    a row they continue into the next row in the paragraph's reading direction. Plain left-to-right
    text keeps the old logical movement exactly (the rendering is pixel-identical to before).
  * **Dual caret.** Where the two places differ, the one typing goes to is the caret and the other is drawn beside it as a thinner, fainter caret (as Windows and Firefox
    do), so at "abc |אבג" you see both the end of the Latin and the start of the Hebrew. `gp_text_input_caret_pos_alt` reports it (false where there is
    one place: inside a direction, at the ends of a soft-wrapped row, with a selection or a composition). `tests/dualcaret_test.c` walks a mixed line and compares frames a blink apart.
  * **Selection** is a set of cells, so a logical range can be several rectangles (`b␣` and `א` with the
    middle of a Hebrew word unselected); adjacent cells merge into one.
  * RTL paragraphs right-align inside the field, wrap by logical width, and `.direction =
    GP_TEXT_DIRECTION_RTL` forces a field's direction. Grapheme clusters split inside a ligature (the
    caret passes through the middle of lam-alef) by sharing the glyph's width equally.
* **Layout direction.** `gp_text_dir_rtl()` on a container (inherited, so on the window's root it is the whole UI) also mirrors its
  flex layout, as CSS `direction: rtl` does: a **row** starts at the right (first child at the right edge, `gp_justify_start` is the right
  and `end` the left, a wrapped row's lines fill from the right), and a **column's cross axis** is mirrored (`gp_items_start` aligns to the
  right edge, `end` to the left, wrapped columns stack from the right). Margins, padding, borders and the `gp_left/right` insets of absolute
  children stay physical (`gp_ml(8)` is still the left margin; a first child's right edge is at the container's right padding edge, and
  `gp_ml_auto()` is the left margin whichever direction it is). A child that says `gp_text_dir_ltr()` lays *its* children out left to right, inside
  a box placed from the right. `gp_text_dir_auto()` (the default) is left to right for layout, as the first strong character of text is
  not a property of a box. **Not mirrored:** a scroller along a row (scrolling starts at the left, as before), `gp_uniform_list` / `gp_list`
  (vertical only), and the scrollbar's side. Tested in `tests/flex_test.c`.
* **Limits, stated plainly.** The caret is a single caret
  with affinity, not two carets as some editors draw at a direction boundary. Ctrl+arrow and
  Home/End are logical (Ctrl+arrow swaps in RTL paragraphs; Home is the start of the row in reading
  order, i.e. the right edge of a Hebrew row). An IME composition is not part of the bidi context or the
  wrapping. Explicit embeddings and isolates (LRE, RLI, ...) are handled by FriBidi but not exercised
  by tests here. Hebrew and Arabic need a font on the system; this machine resolved both to DejaVu Sans.

## Text input

```c
gp_entity name = gp_text_input_new(app, &(gp_text_input_options){
    .owner = me, .placeholder = "Your name",
    .on_change = changed, .on_submit = submitted });
/* in me's render function: */  gp_child_view(name)
gp_text_input_text(app, name);            /* read it any time */
```

A ready-made editable field, built from the framework's own primitives (a scroller holding one text
element per line plus absolutely positioned selection / caret / composition-underline elements), so
clipping, wheel scrolling and scrollbars come from the scroll system and the caret blinks on the
timers. Options: `multiline` + `rows`, `password`, `read_only`, `max_bytes`, `font_size`, `width`,
colours (dark-theme defaults), `placeholder`, `text`. `gp_text_input_set_text / _selection /
_select_all / _focus / _scroll / _preedit` for programmatic access.

* **Editing:** UTF-8 aware (movement and deletion go by *extended characters*: combining marks,
  variation selectors, skin tones and ZWJ sequences stay together), selection with Shift+arrows,
  mouse drag (the mouse stays captured outside the field), double-click = word, triple-click =
  line (all, in a single-line field), Shift+click extends. Word and line movement, delete word /
  to line start / to line end, vertical movement with a remembered column, Page Up/Down that
  scroll the view too.
* **Undo/redo** is operation-based: typing runs, repeated Backspace and repeated Delete each
  coalesce into one step (a caret move or a 1 s pause ends the run); undo restores the selection
  as well; a new edit clears redo.
* **Clipboard:** Ctrl+C/X/V (and Ctrl+Insert, Shift+Insert/Delete). Single-line fields flatten
  pasted line breaks. A password field never exports its text. If the platform refuses the
  clipboard (Wayland only lets a focused client set it) the app falls back to its own last copy.
* **IME:** composition text is shown inline and underlined, the candidate window is placed at the
  caret, and committing, cancelling or starting to compose over a selection behave as expected.
* **Keys** are bound to `text_input::*` actions in the `"TextInput"` key context the first time a
  field is created. Rebind with `gp_bind_key` (later bindings win ties) to change them.
  Enter submits a single-line field and inserts a newline in a multi-line one; Ctrl+Enter submits
  either. Shortcuts are Ctrl-based only (no Cmd variants yet).
* **How positions work:** `build_line()` shapes one line and records the x of every caret stop;
  rendering (caret, selection) and hit-testing (click, drag) both use it, so what you see is what
  you click. The caret is scrolled into view using the scroller's viewport as of the last layout.

### Soft wrap

`.multiline = true, .soft_wrap = true` wraps long lines at the field's width instead of scrolling them
sideways (off by default, so existing multi-line fields are unchanged).

* **Rows.** The unit is the *visual row*, not the hard line: Up/Down, Home/End, Page Up/Down, the
  selection highlight, the caret and mouse hit-testing all work on rows. Without wrapping every hard
  line is exactly one row, so nothing else changes. `gp_text_input_rows / _row_range / _caret_row`
  expose them. Rows are cached per text revision and wrap width.
* **Where rows break:** after a run of spaces (the spaces *hang* past the edge instead of pushing the
  word they follow onto the next row), after a hyphen before a word, between CJK characters (with the
  kinsoku rules of the text system), and, for a word too long for any row, between characters.
* **Caret affinity.** A caret at a soft break is one byte offset that can be drawn at the end of a row
  or the start of the next. End, a click past the end of a row, and Up/Down landing on a row's end
  mean "end of this row"; everything else means "start of the next". Without this, End on a wrapped
  row would visibly jump down a line.
* **Where the width comes from.** Only layout knows it, so the widget uses a new general hook,
  **`gp_on_layout(fn)`**, called with an element's final size after every layout. If a handler calls
  `gp_notify` the tree is rebuilt and laid out again *before anything is painted* (up to 4 passes),
  so the first frame is already wrapped; there is a test that no frame is painted in between. A
  handler must only notify when something changed, or it rebuilds every frame (the widget's does not:
  a focused wrapping field redraws only for the caret blink).
* **Cost.** An edit wraps again only the hard lines it touched: the rows of the lines before are kept, the rows after are moved (their bytes by the change in length, their line numbers by the change in the number of
  lines), so a typed character in a document of 60,000 wrapped rows takes ~0.05 ms where making all the rows again takes ~210 ms (`tests/wrapedit_test.c`: after each of hundreds of random edits (inserts and
  deletes, line breaks, Hebrew and CJK, across lines, at the ends) the kept rows equal the rows made again from scratch, field for field; and an edit followed by another before the rows are read, or by a
  change of width, takes the from-scratch path). What still grows with the document is the pass that moves the rows after an edit and the one-off cost of the first wrap (made again when the width changes): it is
  comfortable to some millions of rows, but the rows are still all there, which is why a soft-wrapped field is not as light to open as a field without wrap (see "Large documents").
* **A composition (IME preedit) wraps like the text.** The rows of the line it is in are made with the composition spliced in at the caret: it breaks at the same
  opportunities as text (spaces, hyphens, between CJK characters) and a long one takes several rows, each of which knows which bytes of it it shows; the composition's own
  caret (its cursor, in characters) is on the row that holds that character, and the underline follows the rows. When it ends or is cancelled the rows are the text's again.
  `tests/ime_wrap_test.c` checks every composition against a second field holding the same text committed (same rows, same caret row and position), at the end of a line, in
  the middle of it, in a later paragraph, for Latin and CJK, growing one character at a time, and the drawn rows against the committed field's ink. A
  composition that ends exactly where its row ends keeps its caret at the end of that row (the committed text's would be at the start of the next).
  Wrapping is not tied to a window resize event but to layout, so it follows resizes automatically.

### Large documents

A multi-line field without soft wrap handles a document of any size the way a code editor does: what it keeps and does per keystroke is
proportional to the lines it touches, not to the document. (A 300,000-line, 14 MB document is a test: creating the field takes ~30 ms,
a typed character at the end reaches the screen in ~25 ms, and selecting all, deleting 14 MB and undoing it is a few frames.)

* **A line index** is kept up to date by every edit: the start of each hard line, and whether the line has right-to-left characters. An
  edit scans again only the lines it overlaps and moves the starts after them (one pass over a `size_t` per line, a memmove), so a row
  (without wrapping a row is a line), the row of an offset (a binary search) and "does this text involve right-to-left?" are all instant,
  and Up/Down, Page Up/Down, Home/End, clicks and the caret never touch lines they do not concern. The text itself is one buffer, so an edit
  moves the bytes after it (14 MB is a few milliseconds) and `gp_text_input_text` is still a plain pointer.
* **Only the rows near the view are built.** The scroller holds spacer elements the height of the rows left out, the rows in view and
  as many again on each side, so its extent is the whole document (the scroll bar, scroll-to-end and Page Down work as for any text) and the
  tree stays a few hundred elements whatever the length. The scroller reports every move of its offset to the field (the new
  `gp_on_scroll`, see below); when the view comes within half a screen of the edge of what was built the field asks for a rebuild, which is
  laid out again before anything is painted, so no blank rows are ever shown. Selection highlights, the composition underline and the
  caret are drawn for the built rows only (the caret is its own element, so it can be anywhere).
* **Across.** The content is as wide as the widest row *seen so far* (a zero-height spacer carries it): scrolling to a very long line
  far down widens the scroll bar when it is first built, and the width stays after the line scrolls away. A document whose longest
  line is not known cannot have its horizontal bar's size right without reading every line.
* **What is still whole-document:** soft wrap (a row's start depends on the rows before it, so every line is shaped and wrapped when the
  text or the width changes: fine up to a few hundred KB, as measured above; it also builds only the rows in view, which is the part that
  was the cost for long non-wrapping text); `gp_text_input_selection_rects` is bounded by the rows the selection reaches, but of course for a
  whole-document selection that is all of them (pass a `max`); undo keeps copies of what was replaced, so deleting 14 MB keeps 14 MB
  (the stack is 500 edits deep at most); and a **single very long line** (a minified file) is shaped whole every time it is drawn.
  Positions are floats: past ~16 million pixels of height (about 850,000 lines at the default size) rows are placed to the nearest 2 px.
* **Tests:** `tests/bigdoc_test.c`. The line index is checked against a rescan of the text after thousands of random edits (inserts of
  line breaks, right-to-left text and multi-byte characters, deletions that join several lines) in a field without wrap and in one with
  soft wrap (`gp_text_input_debug_check`, which also compares the row of an offset with the plain search it replaced); then the big document: the
  tree stays small, the rows are at `line * line height` after Ctrl-End, a wheel jump to the middle, 500 rows more, and Ctrl-Home; clicks and
  drags on deep rows, typing and Enter at the end with undo, select-all / delete / undo byte for byte, and the extent across.

* **Characters are grapheme clusters** as UAX #29 defines them, with the Indic conjunct rule (Unicode 15.1): combining marks and spacing marks,
  Hangul jamo (a decomposed syllable is one character), flags, emoji ZWJ sequences with skin tones and variation selectors, prepended signs
  (Arabic number signs), and an Indic consonant joined to the next by a virama (Devanagari, Bengali, Gujarati, Oriya, Telugu, Malayalam: `क्ष` is
  one character; Tamil, Kannada and Gurmukhi have no such rule and are not joined). Arrow keys, Backspace, Delete, double-click word
  bounds, and the mouse's caret stops all use the same state machine (`gp_gstate_joins` in `text.c`), going back from a codepoint that
  certainly starts a cluster. `tests/grapheme_test.c` compares it with the `regex` module's own segmentation (`\X`) of 933 strings, hand-picked
  and random over an alphabet of everything the rules look at, as the state machine alone and in the widget three ways (forwards, backwards,
  the caret stops of a laid-out row); `tools/gen_grapheme.py` makes the Unicode tables (`src/grapheme_data.h`) and the strings. The shaping of
  such text into the right glyphs is HarfBuzz's, and needs a font with the script.

Limits of the widget as a whole, stated plainly: soft wrap and very long single lines are not virtualized (see above); no
text drag-and-drop.
The IME path is verified up to the widget with simulated composition events; there was no real
input method available to try it against.

### Framework pieces the widget needed

These are general, and available to your own elements:

* `gp_on_mouse_down` + `gp_on_drag` / `gp_on_mouse_up`: **mouse capture**. Mouse events now carry
  `local` (relative to the element) and `mods`, and `click_count` for multi-clicks.
* `gp_clipboard_get / _set`, `gp_cursor_text()` (I-beam), `gp_mods(array, n)` (a runtime-sized list of
  modifiers), `gp_scroll_viewport`, `gp_on_ime` + `gp_ime_rect`, `gp_on_layout` (see Soft wrap), and **`gp_on_scroll(fn)`**: called with a
  scroller's offset whenever it moves (wheel, thumb, track click, scroll-into-view, smooth steps) and after every layout (where
  `gp_scroll_to` and content changes take effect), so an element that draws only what is in view can notice the view left it; `gp_notify` from the
  handler rebuilds and relays out before painting. It runs once per layout pass: do nothing unless something changed.
* **`gp_simulate_keystrokes / _text / _mouse / _ime`**: synthetic input for tests and demos. Keys,
  text and mouse are queued as real platform events, so bindings, focus and capture are exercised;
  composition is delivered synchronously, because compositors send their own composition updates
  (empty ones, for an idle IME) which would otherwise erase a queued fake preedit.

See `examples/inputs.c`.

## Gradients and images

### Gradients

```c
gp_div(gp_size_px(120), gp_rounded(12),
       gp_bg_gradient(90, gp_stop(gp_rgb(0xf38ba8), 0), gp_stop(gp_rgb(0x89b4fa), 1)))   /* left to right */
gp_div(..., gp_bg_radial(0.5, 0.5, gp_stop(white, 0), gp_stop(black, 1)))                 /* from the middle outwards */
```

* **What it is.** A gradient is a background: it takes the box's corners (`gp_rounded`), border, opacity and shadow like a
  plain `gp_bg`, and a `gp_bg` declared after it replaces it (a gradient after a `gp_bg` wins). `gp_hover_bg`,
  `gp_active_bg` and `gp_focus_bg` replace it while they apply. Linear angles are CSS's: 0 points up, 90 right, 180 down,
  and the gradient line is as long as the box needs to reach its corners (`|w sin a| + |h cos a|`). A radial gradient is a circle
  around `(cx, cy)`, fractions of the box, out to the farthest corner. Stops: up to 16, `at` in 0..1; a negative `at` is
  spread evenly between the neighbours that have one; one that goes backwards is held at the previous (a hard edge, so stripes
  are two stops at the same place); before the first and after the last stop the end colors hold.
* **Color.** Interpolated in sRGB, *premultiplied* by alpha, as CSS does, so a fade from a color to the same color transparent
  stays that color and never passes through a gray middle. (The test checks both, from the ramp bytes and from pixels.)
* **How.** A ramp is a row of 1024 premultiplied RGBA8 samples of the stops, baked once and shared by every box with the same
  stops, whatever its size or angle (`gfx.c`: a 128-row table uploaded to a 1024 x 128 texture, only the rows that changed).
  The fragment shader (the existing quad primitive, kinds 5 and 6) turns the pixel's place into a position `t` along the line
  or radius, reads the ramp with linear filtering, and adds a sub-level dither so 8-bit ramps do not band. Border and corner
  rounding come from the same signed-distance code as before. Painted copies of cached views hold ramp rows, so a full table
  clears with the glyph atlases (the frame is repainted, like an atlas overflow); 128 different gradients in one frame is the
  limit, which no ordinary window comes near.
* **Repeating and conic.** `gp_bg_repeating(angle, stops...)` and `gp_bg_repeating_radial(cx, cy, stops...)` take their stops as one
  *period* (positions along the line or radius, as fractions) and repeat it in both directions, as CSS's `repeating-*-gradient`
  does (the pattern jumps back at each end; end with a copy of the first stop for a zigzag): the shader takes
  `fract((t - start) / period)` and the ramp holds the stops stretched to 0..1, so the ramp is shared with any other box that has
  the same period. `gp_bg_conic(cx, cy, from_deg, stops...)` goes around a center, clockwise from up (`from_deg` = where it starts),
  0..1 being the full circle. `gradient_test` checks them against the CSS formulas, including a period that does not start at 0.
* **Not done:** gradients on borders or text, color interpolation in other spaces, repeating conic.

### Images

```c
gp_image logo = gp_image_load_file(app, "logo.png");                      /* PNG, JPEG, GIF (first frame), BMP */
gp_img(logo, gp_w(120), gp_rounded(12), gp_object_fit(GP_FIT_COVER))
gp_image_load_file_async(&cx, "photo.jpg", on_loaded, data);               /* decoded on a pool thread */
```

* **API.** `gp_image` is a generational handle (`GP_IMAGE_NULL` is zero); `gp_image_load_file`, `_load_memory`,
  `gp_image_from_rgba` (straight alpha, copied), `gp_image_size`, `gp_image_release`, and `gp_image_load_file_async`, which
  reads and decodes on a pool thread and calls you back on the main thread with the image (or `GP_IMAGE_NULL`);
  cancelling it frees what the worker made and never calls back. An image is made once and drawn by any number of elements and
  windows; a released handle, or one whose slot now holds another picture, draws nothing (and an element that only has it is
  0 x 0), never somebody else's pixels.
* **Decoding** is `stb_image` (vendored in `third_party/stb/`, public domain, v2.30, SHA-256
  `594c2fe35d49488b4382dbfaec8f98366defca819d916ac95becf3e75f4200b3`), restricted to PNG, JPEG, GIF and BMP. A picture over
  16384 pixels a side or 100 million pixels is refused, from the header, before anything is allocated. EXIF orientation of
  JPEGs and animated GIF frames are not handled. Pixels are premultiplied once at decode. No image library to find or fetch,
  so all four dependency configurations build alike.
* **Sizing.** `gp_img` is a leaf element, as big as the picture (1 image pixel = 1 logical px) unless the style says: a width
  alone or a height alone keeps the proportions (`gp_img(i, gp_w_full())` in a column is as tall as it needs at that width,
  including when it is stretched by the column and has no width of its own), both fix the box. Padding and border go around it.
  `gp_object_fit`: `GP_FIT_FILL` (default, as CSS), `CONTAIN`, `COVER` (crops), `NONE`, `SCALE_DOWN`, always centered. The
  corners follow `gp_rounded` (less the border); `gp_opacity` fades it; `gp_image_pixelated()` samples with nearest neighbor
  when magnified (pixel art).
* **Rendering.** The texture is made when an image is first drawn (RGBA8 with mipmaps, trilinear when shown smaller: a
  512 px checkerboard at 64 px is an even gray, tested, instead of a shimmering pattern). The CPU pixels stay next to it so
  the texture can be made again if the GL context is ever rebuilt (the renderer is torn down when the last window closes).
  An image cannot live in the glyph atlases (it can be any size), so an image instance splits the scene's one instanced draw:
  the run before it, the image with its texture bound, and on; order, and so painting order, is kept (tested against
  boxes drawn under and over an image). Many images in one frame mean that many draw calls, which is fine for UI (hundreds) and
  not for a particle system.
* **Tests:** `tests/image_test.c` (display): the API and the refusals, the registry's stale handles, sizing, every object-fit
  mode against an independent computation, rounded corners, padding and border, opacity, premultiplied alpha, smooth vs
  pixelated, mipmaps, order against other elements, a released image and its texture, async loading and cancelling;
  `tests/gfx_test.c` (no display): stop normalization and the ramp table; `tests/gradient_test.c` (display): every pixel of
  linear, radial, hard-stop, faded, rounded, bordered and transparent gradients against CSS's definition, written again.
* **Filters** for images and SVGs, in the shader, as in CSS: `gp_image_grayscale(0..1)`, `gp_image_brightness(f)`,
  `gp_image_contrast(f)`, `gp_image_saturate(f)`, `gp_image_tint(color)` (multiplies) and `gp_image_blur(px)` (applied in that order, on the
  unpremultiplied color, with CSS's luma weights; checked against the formulas on a picture of known colors). The blur is a 3 x 3
  tap blur of a coarser mip level, wide enough to cover the radius: cheap and smooth, not a true Gaussian, and it does not
  spread an image's own edge (taps are clamped inside the picture).
* **Not done:** `background-image` (draw a `gp_img` behind children instead), 9-slice, a decoded-image cache keyed by path, WebP/AVIF/JPEG XL,
  animated formats, `drop-shadow` and other filters that grow the box.

### SVG and paths

```c
gp_svg check = gp_svg_load_memory(app, "<svg width='24' height='24'><path d='M4 12l5 5 11-11' stroke='currentColor' fill='none' stroke-width='3'/></svg>", len);
gp_svg_img(check, gp_size_px(20), gp_text_color(green))            /* currentColor is the text color */
gp_svg arrow = gp_svg_from_path(app, "M0 0 L20 10 L0 20 Z", 20, 20, fill, stroke, 2);   /* one path: the vector-path primitive */
```

* **How.** A drawing is kept as text and *rasterized at the size, in device pixels, it is shown at* (and again, once, when that
  changes), by nanosvg (vendored in `third_party/nanosvg/`, zlib license: a parser that flattens everything to cubic Béziers and
  a scanline rasterizer), into an image that the image path then draws. So it is sharp at 16 px and at 128 px (tested: an edge is
  one pixel wide at both), and everything that applies to `gp_img` applies to `gp_svg_img`: sizing from the drawing's own size
  (width and height, else the viewBox, else the extent of its shapes) with the proportions kept when one side is given and when a
  column stretches it, `gp_object_fit`, padding, border, `gp_rounded`, `gp_opacity`. Rasters are cached per (drawing, width, color):
  two elements with the same size and color share one, and a drawing shown in four colors has four. When the rasters together pass a
  budget (512, or 96 MB) the frame is finished, they are all dropped together with the glyph atlases, and the frame is painted again.
* **currentColor** is the element's text color (`gp_text_color`, inherited); its alpha is not used (use `gp_opacity`). It is
  substituted in the text before parsing, once per raster.
* **Supported:** paths with every command (arcs too), `rect` (with `rx`), `circle`, `ellipse`, `line`, `polyline`, `polygon`, groups and
  transforms, fill and stroke (color, opacity, fill-rule, width, joins, caps, dashes), linear and radial gradients with `<defs>`
  and references, simple `<style>` class selectors, and, added in front of nanosvg by `src/svgx.c` (below), `<use>`, `<symbol>`, clip paths, masks, filters and `<text>`.
  **Not supported:** patterns, markers, embedded images, `textPath`, CSS beyond simple classes. A document over 8 MB, empty, or over 100000 px is refused.
* **`<use>`, `<symbol>`, clip paths, masks and text** (`src/svgx.c`; a document with none of them goes straight to nanosvg as before). The document is parsed into a small tree and
  rewritten before nanosvg sees it:
  * `<use href="#id">` (or `xlink:href`) is replaced by a `<g>` that carries the use's attributes (so `fill` and `stroke` pass down to a shape that sets none) and `transform` followed by
    `translate(x y)`, holding a copy of what it points at; a `<symbol>` becomes a group scaled by its `viewBox` into the use's `width` and `height` (kept in proportion, centered), uses of
    uses and of groups of uses work, and a document that points at itself or doubles at every step stops after a budget of copies.
  * `clip-path` and `mask` (attribute or `style`) cannot be done in nanosvg's single pass, so the document is cut into **layers** in drawing order: a run of shapes with neither is one
    layer, each element that has one (with everything in it) is a layer of its own. Each layer is a copy of the document holding only its shapes, drawn by nanosvg; the clip path becomes a
    second document (its shapes solid black, whatever they are painted with, `clip-rule` as the fill rule, a `<use>` inside it fine) in the coordinates of the clipped element, so the
    transforms of its ancestors and its own apply, and its alpha is the mask; a `<mask>` the same with luminance times alpha of what it draws (black, the default, hides). A layer is
    multiplied by them and put over what is below, in premultiplied bytes. A clip or mask that is not there, is not what the reference says it is, or has `objectBoundingBox` units is
    ignored (the element is drawn whole). Not done: the mask's region, a clip on something inside a clipped element (only the outermost counts), group opacity (nanosvg applies opacity per shape).
  * `filter="url(#f)"` (attribute or `style`): the element is a layer of its own like a clipped one, and after nanosvg has drawn it, its layer goes through the filter's primitives in order,
    each reading `in` / `in2` (`SourceGraphic`, `SourceAlpha`, a named `result`, by default the primitive before): `feGaussianBlur` (an exact gaussian, `stdDeviation` in user units times the raster's
    scale, one or two numbers), `feOffset`, `feFlood`, `feComposite` (over, in, out, atop, xor, arithmetic), `feMerge`, `feBlend` (normal, multiply, screen, darken, lighten), `feColorMatrix`
    (matrix, saturate, hueRotate, luminanceToAlpha) and `feDropShadow`. The work is in premultiplied floats in linear light, as SVG says, unless the filter says
    `color-interpolation-filters="sRGB"`, and comes after drawing and before the clip and the mask. A primitive that is not supported passes its input on, a filter that is not there (or no filter) leaves the
    element as it was, and the filter region is not applied (the filter works over the whole picture, so a big blur is not cut at the element's box + 10%). Not done: `feTurbulence`, `feImage`, lighting, `feMorphology`,
    `feConvolveMatrix`, `feDisplacementMap`, `feComponentTransfer`, `filterUnits` / `primitiveUnits` in `objectBoundingBox`, CSS filter functions.
  * `<text>` is drawn as the **outlines of its glyphs**: the text system shapes it (the fonts the app has: `font-family`, `font-size` in px, pt or em, `font-weight`, `font-style`) and FreeType's
    outlines become path data, so text is a shape like any other (a clip cuts it, a use copies it, `fill`, `stroke` and transforms apply; synthetic bold and italic as in the UI).
    `text-anchor`, `x`, `y`, `dx`, `dy`, tspans (with their own `x`, `y`, `dx`, `dy` and paint; the text's own text comes before its tspans), entities and white space as XML does. Not done: `textPath`,
    `letter-spacing`, `rotate`, `textLength`, right-to-left and vertical text.
  * `tests/svgx_test.c` (no display): the pixels of each against the same shapes drawn directly or against exact geometry (a circle clip equals the circle itself to 3 bytes, at 1x and 2x, through
    group transforms), and about 80 mutants of the rewriting, the compositing, the filters and the text.
* **A patch to nanosvg:** with gradient units `objectBoundingBox` (the SVG default) a plain number such as `x2="1"` is a fraction of
  the shape's box; upstream only treated percentages that way and read `1` as one pixel, so the commonest way of writing a gradient
  gave a flat color. `third_party/nanosvg/README.md` records the change and both hashes; `tests/svg_test.c` checks a gradient written
  with fractions.
* **`gp_svg_from_path`** wraps one path (`d` attribute syntax) in a width x height box with a fill and stroke (alpha 0: none): the
  vector-path primitive, so there is one drawing engine rather than a second one for paths. Path data that could end its own
  attribute (`"`, `<`, `>`, `&`) is refused.
* **Tests:** `tests/svg_test.c` (display): shapes against supersampled coverage computed independently, strokes, fractional gradients,
  fill-opacity, viewBox scaling, transforms, even-odd, `currentColor` in two colors, object-fit and corners, the registry's refusals and
  stale handles, the raster cache (sharing, per color, per size), the budget overflow and repaint, releasing. `examples/svg.c`.
* **Not done:** SVG animation and scripting, filters, `textPath`, loading from URLs, CSS `mask` / `clip-path` in `<style>` rules (the attribute and `style=""` work).

### Corners and borders

* **Per-corner radii.** `gp_rounded_corners(tl, tr, br, bl)`, and `gp_rounded_top/_bottom/_left/_right(px)` for two at once; a corner
  not named keeps `gp_rounded`'s value and a later `gp_rounded` sets all four again (the last one wins, like CSS). Radii that
  do not fit are scaled down together by CSS's rule (the largest factor that keeps every pair of neighbors within their side), so
  `gp_rounded(40)` on a 60 x 30 box is 15 all round, and `gp_rounded_full()` is still a pill. Gradients, backgrounds, borders, shadows
  and images all follow them.
* **Per-side borders.** `gp_border_left/_top/_right/_bottom(px)`, `gp_border_x/_y`; `gp_border(px)` sets all four and replaces what the
  sides said before, and a side not named keeps `gp_border`'s width. The border is inside the box, like padding: with any side set
  it is added to the padding (`pad = padding + border`, stored apart so padding and borders can be given in any order), which is
  why layout did not change, and why the content of `gp_border_left(8)` starts 8 px further in. One color (`gp_border_color`). In the
  shader the box is the outer rounded rectangle and the border's inside is the box less each side's width, shifted by half the difference,
  with each corner's radius reduced by the average of its two sides (CSS uses an ellipse there; the difference is under a pixel
  for ordinary widths). A scroller with per-side borders has its viewport and scrollbar measured from the outer edge.
* **Border colors per side.** `gp_border_color_top/_right/_bottom/_left(c)` (and `_x`, `_y`); the sides not named keep `gp_border_color`'s, and a
  `gp_border_color` after them sets all four again (last wins, like the widths). Where two sides meet the join is CSS's: the diagonal from the outer
  corner to the inner corner, whatever the two widths (the shader picks the side the pixel is nearest to *in units of that side's width*, which is that line),
  and a pixel on the diagonal is one side's or the other's (the left's on a tie), never both or neither. `tests/border_style_test.c` compares every pixel of a
  100 x 60 box with four colors, with equal widths and with 12, 6, 10 and 4, against the trapezoids of that geometry.
* **Dashed and dotted.** `gp_border_dashed()`, `gp_border_dotted()` (`gp_border_solid()` undoes them): dashes are 2 border-widths long with a gap of 1, dots are circles of the width
  2 widths apart; they start at the top left and run along each side, by that side's own width, anti-aliased, with the element's fill showing in the gaps and
  with the side's color if the sides have their own. A corner is not specially joined (the pattern is laid along each side and just goes on), and a rounded
  corner's arc takes the pattern of the side it is nearest to. The test compares dashes with the exact coverage of each pixel by the dash intervals and dots with a supersampled circle.
* **Gradient borders.** `gp_border_gradient(angle, stops...)`, `gp_border_radial(cx, cy, stops...)`, `gp_border_conic(cx, cy, from, stops...)` (or the `gp_border_*_gradient` functions) paint the
  ring with a gradient over the whole box, mapped exactly as a background's (so the test compares the ring with the same gradient as the background of a twin box, pixel for
  pixel, and the inside is the fill); a later `gp_border_color` replaces it (and it replaces earlier colors), a focus border color replaces it while focused, dashes and dots
  work with it. Not repeating, and per-side colors do not apply with it. The renderer's instance was 176 bytes then (see Transforms: 208 now) (four more vec4s: the three other side colors, and the ring's style).
* **Tests:** `tests/box_test.c` checks every corner against a supersampled rounded rectangle (radii, scaling, who wins), uniform
  and per-side borders pixel by pixel, the borders' effect on layout and its independence of padding order, and corners on a gradient and an image.
* **State styles.** `gp_hover_style(gp_group(...))`, `gp_active_style(...)` (pressed, over the hover) and `gp_focus_style(...)` hold a style that is put over the element's own
  while the pointer is over it, it is pressed or it has focus, in that order. What they can change is what only repaints: background (a color replaces a gradient and a gradient
  a color), border colors (all four, or some sides; `gp_border_color` replaces side colors, as it does at rest), border style and gradient, corner radii, shadow and opacity. Sizes,
  padding, border widths and text are ignored there (nothing is laid out again when the pointer moves), and so is text color, which the children inherit: restyle the children
  with a state of their own. `gp_hover_bg`, `gp_active_bg`, `gp_focus_bg` and `gp_focus_border_color` still work and win over these. A cached view that holds one is repainted
  on hover just as with `gp_hover_bg`. `tests/state_style_test.c` reads frames back for every property, each state, their order, a cached view and the
  mutants that remove one of them.

### Auto margins and wrapping

* **Auto margins.** `gp_ml_auto/_mr_/_mt_/_mb_auto()`, `gp_mx_auto()`, `gp_my_auto()`, `gp_m_auto()` on a flex item. Along the main axis the
  free space is shared equally by the auto margins and justify has nothing left to place (CSS's rule), so `gp_ml_auto()` on the last item of
  a row pushes it to the end and `gp_mx_auto()` centers an item between its neighbors; grow takes the space first, so an auto margin beside a
  growing item is zero. Across the axis an auto margin absorbs the extra room (both sides: centered; one side: pushed to the other)
  and the item is not stretched. A later number on that side (`gp_ml(5)`) sets the margin again and an auto after a number replaces it.
  Not for absolutely positioned elements.
* **`gp_flex_wrap()`.** The children are cut into lines by base size, margins and `gp_gap` (greedily: an item starts a new line when it
  would not fit; one wider than the line gets a line of its own and shrinks to it), and each line is laid out by the ordinary flex
  algorithm as a container of its own (grow, shrink, justify, align, auto margins all work per line). Lines are stacked across
  the container with the gap between them, each as tall as its tallest item, and an unsized container is
  as wide as its widest line, up to the room it is given. A column wraps by height into columns. Needs a size on the main axis
  (otherwise everything stays on one line), and not on a scroller along it; a scroller across (`gp_overflow_y_scroll()` on a wrapping
  row) scrolls the stacked lines. The break is decided from the items' sizes before shrinking, so items that could shrink to fit still wrap.
  When the container is taller than its lines (a height of its own), `gp_align_content_*` says what happens to the room, as in CSS: `stretch` (the
  default) shares it among the lines, and items that stretch then fill them; `start`, `center`, `end` put the lines together; `between`,
  `around`, `evenly` spread them like `gp_justify_*` does items. A container with no room, or none of its own, is unaffected. (For a column the
  lines are columns and "taller" is wider, and in right-to-left `end` is the left.)
* **Tests:** `tests/flex_test.c` reads the geometry layout produced: every auto margin on both axes and in rows and columns, who gets the space
  (grow, justify, numbers set later), wrapped rows and columns with and without gaps, margins, padding and borders, grow, justify,
  stretch and auto margins per line, unsized containers, an oversized item, absolute children, and a wrapping scroller keeping its offset.

## Pipeline

```
notify ──► render fns build element tree (arena) ──► flexbox layout ──┐
                                                                      ▼
mouse move/hover ───────────────────────────────────► paint → scene (instances)
                                                                      ▼
                                                    one instanced GL draw call
```

* **Hover and press only repaint** the existing tree (like GPUI); only a notified view, `gp_app_notify`,
  a focus change that views look at, and resizes re-run views (and then only the uncached ones, plus the
  cached ones that were marked).
* **Layout** (`layout.c`): own flexbox implementation with a Taffy-style
  measure/commit split and per-node memoization. Items of auto height in a column do not shrink
  below their content (CSS "automatic minimum size") unless they clip or scroll, and neither do the items
  of a row below their *min-content* width: the longest word of a text (text wrapped at every opportunity), the sum of
  a nested row's items (gaps, padding, margins and borders included), the widest of a column or a wrapping row, a picture's pixels, a
  width of the item's own as a cap (`min(min-content, width)`, as CSS), `max_w`/`min_w` of nested items applied. Items that clip
  (`gp_overflow_hidden`), scroll, are lists or have a `gp_min_w` of their own are exempt, and it is only worked out when
  the line has to shrink. A `gp_flex_basis` below the min-content width is raised to it, shrinking or not, and the items of a wrapping row get it too (an item alone on its line keeps its word and overflows the line). `tests/minwidth_test.c` checks widths against the text measurer.
* **Text** (`text.c`): fontconfig finds fonts, an itemizer splits text into per-font runs, HarfBuzz
  shapes them, FreeType rasterizes into two atlases; see "Fonts and text shaping" below.
  Advances are in font units, so layout is DPI-independent.
* **Renderer** (`render_gl.c`): quads, gradients, shadows and glyphs (coverage, LCD and color) are all instances of
  one SDF shader; paint order is draw order. Images have textures of their own, so they split the one instanced draw.
* **Platform** (`platform_sdl.c`): the only file that knows about SDL. A native
  Wayland/X11 backend means reimplementing the `gp_platform_*` functions.

## Not done yet

Roughly in the order I'd tackle them:

1. A virtualized editor for soft-wrapped documents (all rows are built when it opens and when its width changes: a lazily wrapped document needs an anchor-based scroll like `gp_list`'s; an edit already wraps only the lines it touches) and for very long single lines (non-wrapping large documents are done).
2. SVG / COLRv1 emoji fonts (FreeType gives the paint graph, it would need an interpreter with gradients, transforms and blend modes of its own).
3. SVG patterns, markers and `textPath`; the rest of the filter primitives (turbulence, lighting, morphology, convolution, displacement, component transfer) and the filter region.
4. `epoll`-style edge-triggered watches and file-descriptor readiness for other platforms (the watcher uses `poll(2)` and an eventfd: Linux).
5. A view with a scroller under the mouse is painted in full every frame (hover and scrollbars are evaluated, not cached), and a cached view that holds a scroller, a list or an overflow clip is
   painted in full when it or its scroller moves (their clip moves with them: only views without one are moved, see "Transforms" and `tests/paintshift_test.c`). The scene is still
   re-uploaded and drawn as a whole every frame, which was measured and is not worth a retained scene: `GPUIC_STATS=1` shows ~0.05 ms of GL submit for ~1,700 instances, so it only starts to matter in the tens of thousands.
6. Native Wayland/X11 and Vulkan backends; multi-window polish.
7. State styles cannot change text color or anything that lays out; transforms have no skew or 3D, and a scroller inside a transformed element does not follow the matrix for the wheel and its thumb.
