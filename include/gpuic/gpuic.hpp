/*
 * gpuic.hpp: a C++20 layer over the C library, written to read like GPUI's Rust API.
 *
 *     using namespace gpuic;
 *
 *     struct Counter {
 *         int count = 0;
 *         Element render(Ctx cx) {
 *             return div().flex().flex_col().items_center().gap_4().p_8().bg(rgb(0x1e1e2e)).text_color(white())
 *                 .child(text(std::to_string(count)).text_size(64).font_bold())
 *                 .child(div().px_5().py_2().rounded_lg().bg(rgb(0x89b4fa)).hover_bg(rgb(0xa6c4fb)).cursor_pointer()
 *                            .on_click([](Counter& self, Ctx cx) { self.count++; cx.notify(); })
 *                            .child("Increment"))
 *                 .when(count > 9, [](Element e) { return e.text_color(rgb(0xf38ba8)); });
 *         }
 *     };
 *     int main() {
 *         return run([](App app) {
 *             app.open_window({ .title = "Counter", .width = 480, .height = 360 }, app.new_view<Counter>());
 *         });
 *     }
 *
 * Header only: it adds no library and calls only the public C functions, so everything the C API does it does, and a C++ view can
 * contain C elements and the other way round (an Element converts to a gp_node *, and a gp_node * is accepted wherever a child is).
 *
 * Elements.   div(), text("..."), img(image), svg_img(svg), uniform_list(...), list(...) and style() make an Element, and the chain
 *             adds to it: every modifier of the C header is a method named like the C function without its gp_ prefix (gp_flex_row() is
 *             .flex_row(), gp_gap(8) is .gap(8)), plus GPUI's spellings (.gap_2() is 8 px, .p_4() is 16 px, .w_1_2(), .rounded_md(),
 *             .text_lg(), .shadow_md() ...: the steps are Tailwind's, 4 px each). The methods are generated from gpuic.h by
 *             tools/gen_cpp_chain.py into gpuic_chain.inc, so none is missing; a ctest checks the file is current.
 *             Like the C macros, a later modifier wins over an earlier one for the same property.
 * Children.   .child(x) takes an Element, a gp_node *, a string (a text element), an Entity (a nested view) or nullptr (skipped);
 *             .children(range) takes any range of those; .when(cond, f), .when_some(optional, f) and .map(f) are GPUI's.
 * Handlers.   .on_click, .on_mouse_down/up, .on_drag, .on_wheel, .on_key_down/up, .on_text_input, .on_action, .on_layout, .on_ime and
 *             .with_animation take lambdas. A lambda names what it wants by its parameter types, in any order and any subset: a
 *             reference to the view's state (`Counter&`: the view that built the element), `Ctx` or `gp_ctx *`, and the event
 *             (`const gp_mouse_event&`, `const gp_key_event&`, `std::string_view` for text, `gp_size` for layout...). Captures are
 *             copied into the frame's arena, so they must be trivially destructible (numbers, pointers, entity handles).
 * State.      App::new_entity<T>(args...) and App::new_view<T>(args...) construct T inside the app, so T may hold std::string or
 *             std::vector; its destructor runs when the entity is released. A view's T has `Element render(Ctx)`.
 * Lifetime.   Elements are built while a view renders, from memory that lives until the next rebuild, exactly as in C: do not keep
 *             an Element (or a handler's captures that point into one) between frames.
 */
#pragma once

#include <gpuic/gpuic.h>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <initializer_list>
#include <memory>
#include <new>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>

#include <sys/socket.h>

namespace gpuic {

class App;
class Ctx;
class Element;
template <class S> struct Entity;

// ---------------------------------------------------------------- colors

using Color = gp_color;
inline Color rgb(uint32_t hex) { return gp_rgb(hex); }       // 0xRRGGBB
inline Color rgba(uint32_t hex) { return gp_rgba(hex); }     // 0xRRGGBBAA
inline Color with_alpha(Color c, float a) { return gp_with_alpha(c, a); }
/** GPUI's hsla: hue, saturation, lightness and alpha, each 0 to 1. */
inline Color hsla(float h, float s, float l, float a = 1.f) {
    h = h - std::floor(h);
    auto f = [&](float n) {
        float k = std::fmod(n + h * 12.f, 12.f);
        return l - s * std::fmin(l, 1.f - l) * std::fmax(-1.f, std::fmin(std::fmin(k - 3.f, 9.f - k), 1.f));
    };
    return Color{ f(0), f(8), f(4), a };
}
inline Color black() { return { 0, 0, 0, 1 }; }
inline Color white() { return { 1, 1, 1, 1 }; }
inline Color transparent() { return { 0, 0, 0, 0 }; }
inline Color red() { return rgb(0xff0000); }
inline Color green() { return rgb(0x00ff00); }
inline Color blue() { return rgb(0x0000ff); }
inline Color yellow() { return rgb(0xffff00); }
inline gp_color_stop stop(Color c, float at) { return gp_color_stop{ c, at }; }

// ---------------------------------------------------------------- strings

namespace detail {
/** Copies into the frame's arena, so a temporary std::string may be passed to something that keeps the pointer. */
inline const char* frame_dup(std::string_view s) {
    char* p = static_cast<char*>(gp_frame_alloc(s.size() + 1));
    std::memcpy(p, s.data(), s.size());
    p[s.size()] = 0;
    return p;
}
}  // namespace detail

/**
 * An immutable string that is shared, not copied, when it is copied: Rust's SharedString (GPUI's `SharedString`). Hold one in a view's state and put it in the tree
 * every frame (`.child(title)`, `text(title)`, anywhere a string goes) without a copy per frame: the text lives as long as any copy of it does, so the pointer a
 * text element gets stays valid while the state holds it.
 */
class SharedString {
public:
    SharedString() : p_(the_empty()) {}
    SharedString(const char* s) : p_(s && *s ? std::make_shared<const std::string>(s) : the_empty()) {}
    SharedString(std::string_view s) : p_(s.empty() ? the_empty() : std::make_shared<const std::string>(s)) {}
    SharedString(const std::string& s) : p_(s.empty() ? the_empty() : std::make_shared<const std::string>(s)) {}
    SharedString(std::string&& s) : p_(s.empty() ? the_empty() : std::make_shared<const std::string>(std::move(s))) {}
    const char* c_str() const { return p_->c_str(); }
    std::string_view view() const { return *p_; }
    operator std::string_view() const { return *p_; }
    std::size_t size() const { return p_->size(); }
    bool empty() const { return p_->empty(); }
    /** True if the two are the same text object (cheaper than comparing the text, and what a copy gives). */
    bool same(const SharedString& o) const { return p_ == o.p_; }
    long use_count() const { return p_.use_count(); }
    friend bool operator==(const SharedString& a, const SharedString& b) { return a.p_ == b.p_ || *a.p_ == *b.p_; }
    friend auto operator<=>(const SharedString& a, const SharedString& b) { return a.view() <=> b.view(); }

private:
    static const std::shared_ptr<const std::string>& the_empty() {
        static const std::shared_ptr<const std::string> e = std::make_shared<const std::string>();
        return e;
    }
    std::shared_ptr<const std::string> p_;
};

/** A string argument: a C string is used as it is, a std::string or string_view is copied into the frame first, a SharedString is used as it is (it outlives the frame). */
class Str {
public:
    Str(const char* s) : p_(s) {}
    Str(std::nullptr_t) : p_(nullptr) {}
    Str(const std::string& s) : p_(detail::frame_dup(s)) {}
    Str(std::string_view s) : p_(detail::frame_dup(s)) {}
    Str(const SharedString& s) : p_(s.c_str()) {}
    const char* c_str() const { return p_; }

private:
    const char* p_;
};

// ---------------------------------------------------------------- plumbing

namespace detail {

/** A growable array in the frame's arena (its old buffers stay until the rebuild, which is what an arena does). */
struct Vec {
    gp_node** p = nullptr;
    uint32_t n = 0, cap = 0;
    void push(gp_node* x) {
        if (!x) return;
        if (n == cap) {
            uint32_t ncap = cap ? cap * 2 : 8;
            auto** np = static_cast<gp_node**>(gp_frame_alloc(ncap * sizeof(gp_node*)));
            if (n) std::memcpy(np, p, n * sizeof(gp_node*));
            p = np;
            cap = ncap;
        }
        p[n++] = x;
    }
    Vec() = default;
    Vec(const Vec& o) {
        if (o.n) {
            p = static_cast<gp_node**>(gp_frame_alloc(o.n * sizeof *p));
            std::memcpy(p, o.p, o.n * sizeof *p);
            n = cap = o.n;
        }
    }
    Vec& operator=(const Vec& o) {
        if (this != &o) { Vec t(o); *this = std::move(t); }
        return *this;
    }
    Vec(Vec&& o) noexcept : p(o.p), n(o.n), cap(o.cap) { o.p = nullptr; o.n = o.cap = 0; }
    Vec& operator=(Vec&& o) noexcept {
        p = o.p; n = o.n; cap = o.cap;
        o.p = nullptr; o.n = o.cap = 0;
        return *this;
    }
};

// What a handler's parameters can be. fn_traits reads the parameter list of a lambda (or function pointer);
// `pick<P>` produces an argument of type P from the things the handler has: the view's state, a ctx and the event.
template <class T> struct fn_traits : fn_traits<decltype(&T::operator())> {};
template <class C, class R, class... A> struct fn_traits<R (C::*)(A...) const> { using args = std::tuple<A...>; };
template <class C, class R, class... A> struct fn_traits<R (C::*)(A...)> { using args = std::tuple<A...>; };
template <class C, class R, class... A> struct fn_traits<R (C::*)(A...) const noexcept> { using args = std::tuple<A...>; };
template <class C, class R, class... A> struct fn_traits<R (C::*)(A...) noexcept> { using args = std::tuple<A...>; };
template <class R, class... A> struct fn_traits<R (*)(A...)> { using args = std::tuple<A...>; };

template <class P, class Av>
inline constexpr bool matches_v =
    std::is_same_v<std::decay_t<P>, std::decay_t<Av>> ||
    (std::is_pointer_v<std::decay_t<P>> &&
     std::is_same_v<std::remove_cv_t<std::remove_pointer_t<std::decay_t<P>>>, std::decay_t<Av>>);

template <class P, class... Av> constexpr std::size_t find_avail() {
    std::size_t i = 0, found = sizeof...(Av);
    ((matches_v<P, Av> && found == sizeof...(Av) ? (found = i, ++i) : ++i), ...);
    return found;
}

template <class P, class... Av> decltype(auto) pick(void* self, Av&... av) {
    constexpr std::size_t at = find_avail<P, Av...>();
    if constexpr (at < sizeof...(Av)) {
        auto& x = std::get<at>(std::forward_as_tuple(av...));
        if constexpr (std::is_pointer_v<std::decay_t<P>> && !std::is_same_v<std::decay_t<P>, std::decay_t<decltype(x)>>) return static_cast<P>(&x);
        else return static_cast<P>(x);
    } else if constexpr (std::is_same_v<std::decay_t<P>, void*>) {
        return static_cast<P>(self);
    } else if constexpr (std::is_pointer_v<std::decay_t<P>>) {
        return static_cast<P>(static_cast<std::remove_cv_t<std::remove_pointer_t<std::decay_t<P>>>*>(self));
    } else {
        static_assert(std::is_reference_v<P>,
                      "gpuic: a handler's parameter must be the view's state by reference, a Ctx or gp_ctx *, or the event of this handler");
        using T = std::remove_cv_t<std::remove_reference_t<P>>;
        return static_cast<P>(*static_cast<T*>(self));
    }
}

/** Calls `f` with whatever of `self` and `av` its parameters ask for. */
template <class F, class... Av> decltype(auto) call(F& f, void* self, Av&... av) {
    using Args = typename fn_traits<F>::args;
    return [&]<std::size_t... I>(std::index_sequence<I...>) -> decltype(auto) {
        return f(pick<std::tuple_element_t<I, Args>>(self, av...)...);
    }(std::make_index_sequence<std::tuple_size_v<Args>>{});
}

template <class D> inline constexpr bool stateless_v = std::is_empty_v<D> && std::is_default_constructible_v<D>;

/** Keeps a handler's callable in the frame's arena for the C library's `data` pointer; a callable with nothing in it needs none. */
template <class F> void* box(F&& f) {
    using D = std::decay_t<F>;
    if constexpr (stateless_v<D>) {
        return nullptr;
    } else {
        static_assert(std::is_trivially_destructible_v<D>,
                      "gpuic: a handler's captures are kept in the frame's arena and never destroyed: capture numbers, pointers or handles, not std::string");
        static_assert(alignof(D) <= 16);
        void* m = gp_frame_alloc(sizeof(D));
        new (m) D(std::forward<F>(f));
        return m;
    }
}
template <class D, class... Av> void run_boxed(void* self, void* data, Av&... av) {
    if constexpr (stateless_v<D>) { D f{}; call(f, self, av...); }
    else call(*static_cast<D*>(data), self, av...);
}

}  // namespace detail

// ---------------------------------------------------------------- tasks, subscriptions, coroutines

/** A timer, animation, frame request, background job, watch or coroutine in flight (gp_task). */
class Task {
public:
    Task() = default;
    Task(gp_app* app, gp_task t) : app_(app), t_(t) {}
    gp_task raw() const { return t_; }
    /** Still going: not finished, cancelled or dropped with its entity or window. */
    bool active() const { return app_ && gp_task_active(app_, t_); }
    explicit operator bool() const { return active(); }
    /** Safe on a finished task. The callable is destroyed once, after the point where it could last have run. */
    void cancel() const { if (app_) gp_cancel(app_, t_); }

private:
    gp_app* app_ = nullptr;
    gp_task t_{};
};

/** An observer or event subscription (gp_subscription). */
class Subscription {
public:
    Subscription() = default;
    Subscription(gp_app* app, gp_subscription s) : app_(app), s_(s) {}
    gp_subscription raw() const { return s_; }
    bool active() const { return app_ && gp_subscription_active(app_, s_); }
    explicit operator bool() const { return active(); }
    void cancel() const { if (app_) gp_unsubscribe(app_, s_); }

private:
    gp_app* app_ = nullptr;
    gp_subscription s_{};
};

/** What a descriptor watch reports: the descriptor and the GP_FD_* flags that are set. */
struct FdEvent {
    int fd;
    int events;
};

/** What a coroutine started with Ctx::spawn awaits with. Every await returns false once the coroutine is cancelled (its entity or
 *  window went away, or Task::cancel): clean up and return then, and do not touch the entity's state. */
class Co {
public:
    explicit Co(gp_co* c) : c_(c) {}
    gp_co* raw() const { return c_; }
    bool cancelled() const { return gp_co_cancelled(c_); }
    bool sleep(double seconds) const { return gp_sleep(c_, seconds); }
    bool yield() const { return gp_yield(c_); }
    /** Resumes at the next frame; `time` receives the frame's time. */
    bool frame(double* time = nullptr) const { double t; return gp_await_frame(c_, time ? time : &t); }
    /** Runs `work` on a pool thread (it must not touch the app) and resumes with its result: nullopt if cancelled meanwhile.
     *  A void `work` returns whether it ran. */
    template <class W> auto run(W&& work) const {
        using R = std::invoke_result_t<W&>;
        if constexpr (std::is_void_v<R>) {
            auto w = [](void* d) { (*static_cast<std::remove_reference_t<W>*>(d))(); };
            return gp_await_background(c_, w, &work);
        } else {
            struct Slot { W* work; std::optional<R> result; } slot{ &work, std::nullopt };
            auto w = [](void* d) { auto* sl = static_cast<Slot*>(d); sl->result.emplace((*sl->work)()); };
            if (!gp_await_background(c_, w, &slot)) return std::optional<R>{};
            return std::move(slot.result);
        }
    }
    /** Waits for `events` (GP_FD_*) on `fd`, at most `timeout` seconds (negative: no limit); `revents` gets what happened. */
    bool fd(int fd, int events, double timeout = -1, int* revents = nullptr) const { return gp_await_fd(c_, fd, events, timeout, revents); }
    ssize_t read(int fd, void* buf, std::size_t len) const { return gp_co_read(c_, fd, buf, len); }
    ssize_t write(int fd, const void* buf, std::size_t len) const { return gp_co_write(c_, fd, buf, len); }
    int accept(int listen_fd) const { return gp_co_accept(c_, listen_fd); }
    int connect(int fd, const sockaddr* addr, socklen_t len) const { return gp_co_connect(c_, fd, addr, len); }

private:
    gp_co* c_;
};

namespace detail {
/** The closures the C++ layer hands to timers, tasks and subscriptions are owned by them: new'd here, deleted by the drop function the
 *  library calls exactly once when the task or subscription is over, whether it ran, was cancelled or went away with its owner. */
template <class D> void drop_box(void* p) { delete static_cast<D*>(p); }
}  // namespace detail

// ---------------------------------------------------------------- focus and scroll handles

/** A focus handle (gp_focus): a stable id for an element, kept in a view's state and given to .track_focus(...) each render. */
class Focus {
public:
    Focus() = default;
    explicit Focus(gp_focus f) : f_(f) {}
    static Focus create(gp_app* app) { return Focus(gp_focus_new(app)); }
    gp_focus raw() const { return f_; }
    operator gp_focus() const { return f_; }
    explicit operator bool() const { return f_.id != 0; }

private:
    gp_focus f_{};
};

/** Where a list row goes when it is scrolled to. */
enum class RowAlign { Nearest = GP_ALIGN_ROW_NEAREST, Start = GP_ALIGN_ROW_START, Center = GP_ALIGN_ROW_CENTER };

/** A scroll handle (gp_scroll): control a scroller, or a list's position, from code. Given to an element with .track_scroll(scroll). */
class Scroll {
public:
    Scroll() = default;
    explicit Scroll(gp_scroll h) : h_(h) {}
    static Scroll create(gp_app* app) { return Scroll(gp_scroll_new(app)); }
    gp_scroll raw() const { return h_; }
    operator gp_scroll() const { return h_; }
    explicit operator bool() const { return h_.id != 0; }
    // (all of these take the ctx of the code that calls them: a handler's, a Ctx from App::ctx())
    gp_point offset(gp_ctx* cx) const { return gp_scroll_offset(cx, h_); }
    gp_point max(gp_ctx* cx) const { return gp_scroll_max(cx, h_); }          // as of the last layout
    gp_size viewport(gp_ctx* cx) const { return gp_scroll_viewport(cx, h_); }  // the visible area, as of the last layout
    void to(gp_ctx* cx, float x, float y) const { gp_scroll_to(cx, h_, x, y); }
    void by(gp_ctx* cx, float dx, float dy) const { gp_scroll_by(cx, h_, dx, dy); }
    /** To the bottom, and it stays there as content grows until called again. */
    void to_end(gp_ctx* cx) const { gp_scroll_to_end(cx, h_); }
    // For a uniform_list / list this tracks:
    void to_item(gp_ctx* cx, std::size_t index, RowAlign align = RowAlign::Nearest) const { gp_list_scroll_to_item(cx, h_, index, (int)align); }
    /** `old_count` rows from `start` became `new_count` rows (an insertion, a removal, a replacement): rows below keep their place. */
    void splice(gp_ctx* cx, std::size_t start, std::size_t old_count, std::size_t new_count) const { gp_list_splice(cx, h_, start, old_count, new_count); }
    /** `count` rows from `start` changed height or content: they are measured again. */
    void invalidate(gp_ctx* cx, std::size_t start, std::size_t count) const { gp_list_invalidate(cx, h_, start, count); }
    /** All rows are new: forget what was measured. */
    void reset(gp_ctx* cx) const { gp_list_reset(cx, h_); }

private:
    gp_scroll h_{};
};

// ---------------------------------------------------------------- Ctx

/** The context of a callback: what Rust passes as `cx`. */
class Ctx {
public:
    Ctx(gp_ctx* c) : c_(c) {}
    /** A ctx that owns its gp_ctx (App::ctx makes these): it stays valid when copied and returned. */
    explicit Ctx(const gp_ctx& value) : store_(value), c_(&store_) {}
    Ctx(const Ctx& o) : store_(o.store_), c_(o.c_ == &o.store_ ? &store_ : o.c_) {}
    Ctx& operator=(const Ctx& o) { store_ = o.store_; c_ = o.c_ == &o.store_ ? &store_ : o.c_; return *this; }
    gp_ctx* raw() const { return c_; }
    operator gp_ctx*() const { return c_; }
    inline App app() const;
    gp_entity entity() const { return c_->entity; }
    /** The state this callback belongs to changed: the view renders again, observers are called. */
    void notify() const { gp_notify(c_); }
    /** Lets the event go on to the next handler up (otherwise the first one to take it stops it). */
    void propagate() const { gp_propagate(c_); }

    // Timers and background work. The callable is `f(Self&, Ctx)` with any subset of its parameters (see Handlers), where Self is the
    // state of the entity this ctx belongs to (cancelled with it, and with the window of this ctx). Unlike a handler's, its captures
    // may be anything movable: it is kept until the task is over and then destroyed, exactly once.
    template <class F> Task after(double seconds, F&& f) const;
    template <class F> Task every(double seconds, F&& f) const;
    /** Before the next frame is drawn; `f(Self&, Ctx, double time)`. Call next_frame again from it to keep animating. */
    template <class F> Task next_frame(F&& f) const;
    /** `f(Self&, Ctx, float t)` each frame with t in [0, 1] eased by `easing` (GP_EASE_*), last with 1. */
    template <class F> Task animate(double seconds, int easing, F&& f) const;
    /** `work()` runs on a pool thread and must not touch the app; then `done(Self&, Ctx, R&)` runs here with what it returned (no
     *  parameter for it when it returns void). Not called if the task was cancelled or the entity is gone; both are destroyed either way. */
    template <class W, class D> Task background(W&& work, D&& done) const;
    /** A coroutine: `f(Co&, Self&, Ctx)` runs on the main thread and may await (Co::sleep ...) in between. */
    template <class F> Task spawn(F&& f) const;
    /** `f(Self&, Ctx, FdEvent)` while `fd` is ready for `events` (GP_FD_*); level-triggered, as poll(2). */
    template <class F> Task watch_fd(int fd, int events, F&& f) const;
    /** Decodes on a pool thread; `f(Self&, Ctx, gp_image)` gets GP_IMAGE_NULL if it failed. */
    template <class F> Task load_image(const char* path, F&& f) const;

    // Observation. `f(Self&, Ctx, gp_entity observed)`.
    template <class F> Subscription observe(gp_entity target, F&& f) const;
    /** The entity re-renders and notifies when `target` does: a view mirroring a model. */
    Subscription observe(gp_entity target) const { return Subscription(c_->app, gp_observe(c_, target, nullptr, nullptr)); }
    /** `f(Self&, Ctx, const gp_event&)` for events `emitter` sends with emit() (`event` null: all of them). */
    template <class F> Subscription subscribe(gp_entity emitter, const char* event, F&& f) const;
    /** `f(Self&, Ctx, gp_entity released, void* state)` as `target` is released, before its state is destroyed. */
    template <class F> Subscription on_release(gp_entity target, F&& f) const;
    /** Sends an event from this entity to its subscribers; the payload (trivially copyable) is copied. */
    template <class P> void emit(const char* event, const P& payload) const {
        static_assert(std::is_trivially_copyable_v<P>);
        gp_emit(c_, event, &payload, sizeof(P));
    }
    void emit(const char* event) const { gp_emit(c_, event, nullptr, 0); }

    // Focus. The handles are made with App::new_focus() and given to an element with .track_focus(focus).
    void focus(Focus f) const;
    void blur() const { gp_blur(c_); }
    bool is_focused(Focus f) const;
    void focus_next() const { gp_focus_next(c_); }
    void focus_prev() const { gp_focus_prev(c_); }
    /** Scrolls the scrollers around the focused-able element by the least that shows it, once the next layout is done. */
    void scroll_into_view(Focus f) const;

    // Actions and key bindings (App::bind_key). dispatch() runs an action as if a key had fired it, from the focused element up.
    bool dispatch(const char* action) const { return gp_dispatch_action(c_, action); }
    template <class P> bool dispatch(const char* action, const P& payload) const {
        static_assert(std::is_trivially_copyable_v<P>);
        return gp_dispatch_action_with(c_, action, &payload, sizeof(P));
    }
    /** The keystrokes of a chord typed so far in this window ("ctrl-k"), "" when none: for a status line. */
    const char* keys_pending() const { return gp_keys_pending(c_); }
    /** The payload the action being handled was bound with (or dispatched with), or nullptr; valid until the handler returns. */
    template <class P> const P* payload() const {
        static_assert(std::is_trivially_copyable_v<P>);
        std::size_t size = 0;
        const void* p = gp_action_payload(c_, &size);
        return p && size == sizeof(P) ? static_cast<const P*>(p) : nullptr;
    }

private:
    gp_ctx store_{};
    gp_ctx* c_;
};

namespace detail {

/** Runs a boxed handler with a Ctx, the raw ctx and the handler's extras (the event...) available to its parameters. */
template <class D, class... Ex> void run_handler(void* self, void* data, gp_ctx* cx, Ex&... ex) {
    Ctx ctx(cx);
    if constexpr (stateless_v<D>) { D f{}; call(f, self, ctx, cx, ex...); }
    else call(*static_cast<D*>(data), self, ctx, cx, ex...);
}

template <class D> void mouse_tramp(void* self, gp_ctx* cx, const gp_mouse_event* ev, void* data) { run_handler<D>(self, data, cx, *ev); }
template <class D> void key_tramp(void* self, gp_ctx* cx, const gp_key_event* ev, void* data) { run_handler<D>(self, data, cx, *ev); }
template <class D> void action_tramp(void* self, gp_ctx* cx, void* data) { run_handler<D>(self, data, cx); }
template <class D> void text_tramp(void* self, gp_ctx* cx, const char* utf8, void* data) {
    std::string_view sv(utf8);
    run_handler<D>(self, data, cx, utf8, sv);
}
template <class D> void wheel_tramp(void* self, gp_ctx* cx, const gp_wheel_event* ev, void* data) { run_handler<D>(self, data, cx, *ev); }
template <class D> void scroll_tramp(void* self, gp_ctx* cx, gp_point offset, void* data) { run_handler<D>(self, data, cx, offset); }
template <class D> void layout_tramp(void* self, gp_ctx* cx, gp_size size, void* data) { run_handler<D>(self, data, cx, size); }
template <class D> void ime_tramp(void* self, gp_ctx* cx, const char* preedit, int cursor, void* data) {
    std::string_view sv(preedit);
    run_handler<D>(self, data, cx, preedit, sv, cursor);
}

template <class F> gp_node* state_style(F& f);
inline gp_node* into_node(Element&& e);
inline gp_node* into_node(const Element& e);
inline gp_node* into_node(gp_node* n) { return n; }
inline gp_node* into_node(std::nullptr_t) { return nullptr; }
inline gp_node* into_node(const char* s);
inline gp_node* into_node(const std::string& s);
inline gp_node* into_node(std::string_view s);
inline gp_node* into_node(const SharedString& s);
template <class S> inline gp_node* into_node(const Entity<S>& e);

/** What a gp_list_sticky function returns for a row: the header to pin (any IntoElement; nullptr for none), the first row of the group and the first of the next. */
struct ListGroup {
    gp_node* header = nullptr;
    size_t first = 0, next = static_cast<size_t>(-1);
    template <class H> ListGroup(H&& h, size_t first_row, size_t next_row) : header(detail::into_node(std::forward<H>(h))), first(first_row), next(next_row) {}
};

template <class D> gp_list_group group_tramp(void* self, gp_ctx* cx, size_t row, void* data) {
    Ctx ctx(cx);
    ListGroup g = [&] { if constexpr (stateless_v<D>) { D f{}; return call(f, self, ctx, cx, row); } else return call(*static_cast<D*>(data), self, ctx, cx, row); }();
    return gp_list_group{ g.header, g.first, g.next };
}
template <class D> gp_node* list_tramp(void* self, gp_ctx* cx, size_t index, void* data) {
    Ctx ctx(cx);
    if constexpr (stateless_v<D>) { D f{}; return into_node(call(f, self, ctx, cx, index)); }
    else return into_node(call(*static_cast<D*>(data), self, ctx, cx, index));
}
template <class D> gp_node* anim_tramp(float t, void* data) {
    if constexpr (stateless_v<D>) { D f{}; return into_node(call(f, nullptr, t)); }
    else return into_node(call(*static_cast<D*>(data), nullptr, t));
}

}  // namespace detail

// ---------------------------------------------------------------- Element

/**
 * What div(), text(), img()... return, and what the chain builds on. Every method adds to the element and returns it: on a named
 * Element by reference, on a temporary as a temporary, so `div().flex().child(x)` and `e.flex(); e.child(x);` both work. The C
 * nodes are made as the chain runs and put together when the element is used (converted to a gp_node *, or given to a parent).
 */
class Element {
public:
    Element() = default;   // an empty div

    // ---- the C node this is, made now (it can be asked for again)
    gp_node* build() const {
        gp_node* mods = mods_.n ? gp_mods(mods_.p, mods_.n) : nullptr;
        gp_node* kids = kids_.n ? gp_children(kids_.p, kids_.n) : nullptr;
        switch (kind_) {
        case Kind::Div: return gp_div_(0, mods, kids, GP_END);
        case Kind::Text: return gp_text_(str_, mods, GP_END);
        case Kind::Img: return gp_img_(img_, mods, GP_END);
        case Kind::Svg: return gp_svg_img_(svg_, mods, GP_END);
        case Kind::Curve: return gp_curve_(pts_[0], pts_[1], pts_[2], pts_[3], mods, GP_END);
        case Kind::Polyline: return gp_polyline_(pts_, npts_, mods, GP_END);
        case Kind::UList: return gp_uniform_list_(count_, height_, list_fn_, data_, mods, GP_END);
        case Kind::List: return gp_list_(count_, height_, list_fn_, data_, mods, GP_END);
        case Kind::Group: return gp_mods(mods_.p, mods_.n);
        case Kind::Raw: return raw_;
        }
        return nullptr;
    }
    operator gp_node*() const { return build(); }

    // ---- children
    template <class T> Element& child(T&& x) & { kids_.push(detail::into_node(std::forward<T>(x))); return *this; }
    template <class T> Element&& child(T&& x) && { child(std::forward<T>(x)); return std::move(*this); }
    template <class R> Element& children(R&& range) & {
        for (auto&& x : range) kids_.push(detail::into_node(std::forward<decltype(x)>(x)));
        return *this;
    }
    template <class R> Element&& children(R&& range) && { children(std::forward<R>(range)); return std::move(*this); }
    Element& children(std::initializer_list<Element> list) & { for (const Element& x : list) kids_.push(x.build()); return *this; }
    Element&& children(std::initializer_list<Element> list) && { children(list); return std::move(*this); }

    // ---- GPUI's conditionals: the function gets the element and returns it (changed)
    template <class F> Element when(bool cond, F&& f) && {
        if (!cond) return std::move(*this);
        return Element(std::invoke(std::forward<F>(f), std::move(*this)));
    }
    template <class F> Element when(bool cond, F&& f) & { return Element(*this).when(cond, std::forward<F>(f)); }
    template <class T, class F> Element when_some(const std::optional<T>& v, F&& f) && {
        if (!v) return std::move(*this);
        return Element(std::invoke(std::forward<F>(f), std::move(*this), *v));
    }
    template <class T, class F> Element when_some(const std::optional<T>& v, F&& f) & { return Element(*this).when_some(v, std::forward<F>(f)); }
    template <class F> Element map(F&& f) && { return Element(std::invoke(std::forward<F>(f), std::move(*this))); }
    template <class F> Element map(F&& f) & { return Element(*this).map(std::forward<F>(f)); }

    /** Applies the modifiers of a style() element (or any element's, ignoring its children) to this one. */
    Element& refine(const Element& style) & { for (uint32_t i = 0; i < style.mods_.n; i++) mods_.push(style.mods_.p[i]); return *this; }   // (flat: the C library does not apply a group inside a group)
    Element&& refine(const Element& style) && { refine(style); return std::move(*this); }
    /** GPUI's `.hover(|style| style.bg(...))`: `f` gets a style() to add to (by value, or by reference, returning it or not) and what it sets holds
     *  while the pointer is over this element; `.active` while it is pressed, `.focus_style` while it has focus. Colors, borders, corners, shadow, opacity,
     *  filters, and what lays out (sizes, padding, fonts...): see gp_hover_style. */
    template <class F> Element& hover(F&& f) & { return add(gp_hover_style(detail::state_style(f))); }
    template <class F> Element&& hover(F&& f) && { hover(std::forward<F>(f)); return std::move(*this); }
    template <class F> Element& active(F&& f) & { return add(gp_active_style(detail::state_style(f))); }
    template <class F> Element&& active(F&& f) && { active(std::forward<F>(f)); return std::move(*this); }
    template <class F> Element& focus_style(F&& f) & { return add(gp_focus_style(detail::state_style(f))); }
    template <class F> Element&& focus_style(F&& f) && { focus_style(std::forward<F>(f)); return std::move(*this); }
    /** A mask (gp_mask): any element, laid out over this one's box; where it is transparent this one is not seen. `.mask(div().size_full().bg_gradient(180, {...}))` */
    template <class T> Element& mask(T&& x) & { return add(gp_mask(detail::into_node(std::forward<T>(x)))); }
    template <class T> Element&& mask(T&& x) && { mask(std::forward<T>(x)); return std::move(*this); }
    /** One modifier made by a C function, for anything the methods do not name. */
    Element& with(gp_node* mod) & { return add(mod); }
    Element&& with(gp_node* mod) && { return std::move(add(mod)); }

    // ---- gradients: the colors as a braced list, `.bg_gradient(90, {stop(red(), 0), stop(blue(), 1)})`
    Element& bg_gradient(float angle_deg, std::initializer_list<gp_color_stop> s) & { return add(gp_bg_linear_gradient(angle_deg, s.begin(), (int)s.size())); }
    Element&& bg_gradient(float angle_deg, std::initializer_list<gp_color_stop> s) && { return std::move(bg_gradient(angle_deg, s)); }
    Element& bg_radial(float cx, float cy, std::initializer_list<gp_color_stop> s) & { return add(gp_bg_radial_gradient(cx, cy, s.begin(), (int)s.size())); }
    Element&& bg_radial(float cx, float cy, std::initializer_list<gp_color_stop> s) && { return std::move(bg_radial(cx, cy, s)); }
    Element& bg_repeating(float angle_deg, std::initializer_list<gp_color_stop> s) & { return add(gp_bg_repeating_gradient(angle_deg, s.begin(), (int)s.size())); }
    Element&& bg_repeating(float angle_deg, std::initializer_list<gp_color_stop> s) && { return std::move(bg_repeating(angle_deg, s)); }
    Element& bg_repeating_radial(float cx, float cy, std::initializer_list<gp_color_stop> s) & { return add(gp_bg_repeating_radial_gradient(cx, cy, s.begin(), (int)s.size())); }
    Element&& bg_repeating_radial(float cx, float cy, std::initializer_list<gp_color_stop> s) && { return std::move(bg_repeating_radial(cx, cy, s)); }
    Element& bg_conic(float cx, float cy, float from_deg, std::initializer_list<gp_color_stop> s) & { return add(gp_bg_conic_gradient(cx, cy, from_deg, s.begin(), (int)s.size())); }
    Element&& bg_conic(float cx, float cy, float from_deg, std::initializer_list<gp_color_stop> s) && { return std::move(bg_conic(cx, cy, from_deg, s)); }

    // ---- the border painted with a gradient (the colors as a braced list, as for the background's)
    Element& border_gradient(float angle_deg, std::initializer_list<gp_color_stop> s) & { return add(gp_border_linear_gradient(angle_deg, s.begin(), (int)s.size())); }
    Element&& border_gradient(float angle_deg, std::initializer_list<gp_color_stop> s) && { return std::move(border_gradient(angle_deg, s)); }
    // a stroke's color along its length, `.stroke_gradient({stop(red(), 0), stop(blue(), 1)})`, and its dashes, `.stroke_dashes({8, 4})`
    Element& stroke_gradient(std::initializer_list<gp_color_stop> s) & { return add(gp_stroke_gradient_stops(s.begin(), (int)s.size())); }
    Element&& stroke_gradient(std::initializer_list<gp_color_stop> s) && { return std::move(stroke_gradient(s)); }
    Element& stroke_dashes(std::initializer_list<float> d) & { return add(gp_stroke_dash(d.begin(), (int)d.size())); }
    Element&& stroke_dashes(std::initializer_list<float> d) && { return std::move(stroke_dashes(d)); }
    Element& border_radial(float cx, float cy, std::initializer_list<gp_color_stop> s) & { return add(gp_border_radial_gradient(cx, cy, s.begin(), (int)s.size())); }
    Element&& border_radial(float cx, float cy, std::initializer_list<gp_color_stop> s) && { return std::move(border_radial(cx, cy, s)); }
    Element& border_conic(float cx, float cy, float from_deg, std::initializer_list<gp_color_stop> s) & { return add(gp_border_conic_gradient(cx, cy, from_deg, s.begin(), (int)s.size())); }
    Element&& border_conic(float cx, float cy, float from_deg, std::initializer_list<gp_color_stop> s) && { return std::move(border_conic(cx, cy, from_deg, s)); }

    // ---- handlers. A lambda takes what it names (see the top of this file); a C function pointer and its data work as in C.
    Element& on_click(gp_mouse_fn fn, void* data = nullptr) & { return add(gp_on_click_data(fn, data)); }
    Element&& on_click(gp_mouse_fn fn, void* data = nullptr) && { return std::move(add(gp_on_click_data(fn, data))); }
    template <class F> requires(!std::is_convertible_v<F, gp_mouse_fn>) Element& on_click(F&& f) & {
        using D = std::decay_t<F>;
        return add(gp_on_click_data(&detail::mouse_tramp<D>, detail::box(std::forward<F>(f))));
    }
    template <class F> requires(!std::is_convertible_v<F, gp_mouse_fn>) Element&& on_click(F&& f) && { on_click(std::forward<F>(f)); return std::move(*this); }

    Element& on_mouse_down(gp_mouse_fn fn, void* data = nullptr) & { return add(gp_on_mouse_down_data(fn, data)); }
    Element&& on_mouse_down(gp_mouse_fn fn, void* data = nullptr) && { return std::move(add(gp_on_mouse_down_data(fn, data))); }
    template <class F> requires(!std::is_convertible_v<F, gp_mouse_fn>) Element& on_mouse_down(F&& f) & {
        return add(gp_on_mouse_down_data(&detail::mouse_tramp<std::decay_t<F>>, detail::box(std::forward<F>(f))));
    }
    template <class F> requires(!std::is_convertible_v<F, gp_mouse_fn>) Element&& on_mouse_down(F&& f) && { on_mouse_down(std::forward<F>(f)); return std::move(*this); }

    // (the C library has no data pointer for these: the lambda must capture nothing)
    Element& on_drag(gp_mouse_fn fn) & { return add(gp_on_drag(fn)); }
    Element&& on_drag(gp_mouse_fn fn) && { return std::move(add(gp_on_drag(fn))); }
    template <class F> requires(!std::is_convertible_v<F, gp_mouse_fn>) Element& on_drag(F&&) & {
        static_assert(detail::stateless_v<std::decay_t<F>>, "gpuic: on_drag has no data pointer in the C library: the lambda cannot capture");
        return add(gp_on_drag(&detail::mouse_tramp<std::decay_t<F>>));
    }
    template <class F> requires(!std::is_convertible_v<F, gp_mouse_fn>) Element&& on_drag(F&& f) && { on_drag(std::forward<F>(f)); return std::move(*this); }
    Element& on_mouse_up(gp_mouse_fn fn) & { return add(gp_on_mouse_up(fn)); }
    Element&& on_mouse_up(gp_mouse_fn fn) && { return std::move(add(gp_on_mouse_up(fn))); }
    template <class F> requires(!std::is_convertible_v<F, gp_mouse_fn>) Element& on_mouse_up(F&&) & {
        static_assert(detail::stateless_v<std::decay_t<F>>, "gpuic: on_mouse_up has no data pointer in the C library: the lambda cannot capture");
        return add(gp_on_mouse_up(&detail::mouse_tramp<std::decay_t<F>>));
    }
    template <class F> requires(!std::is_convertible_v<F, gp_mouse_fn>) Element&& on_mouse_up(F&& f) && { on_mouse_up(std::forward<F>(f)); return std::move(*this); }

    /** The wheel turned over this element (see gp_on_wheel): `const gp_wheel_event&` has dx, dy, mods and the pointer; it is used up unless the handler calls cx.propagate(). */
    Element& on_wheel(gp_wheel_fn fn, void* data = nullptr) & { return add(gp_on_wheel_data(fn, data)); }
    Element&& on_wheel(gp_wheel_fn fn, void* data = nullptr) && { return std::move(add(gp_on_wheel_data(fn, data))); }
    template <class F> requires(!std::is_convertible_v<F, gp_wheel_fn>) Element& on_wheel(F&& f) & {
        return add(gp_on_wheel_data(&detail::wheel_tramp<std::decay_t<F>>, detail::box(std::forward<F>(f))));
    }
    template <class F> requires(!std::is_convertible_v<F, gp_wheel_fn>) Element&& on_wheel(F&& f) && { on_wheel(std::forward<F>(f)); return std::move(*this); }

    Element& on_key_down(gp_key_fn fn, void* data = nullptr) & { return add(gp_on_key_down_data(fn, data)); }
    Element&& on_key_down(gp_key_fn fn, void* data = nullptr) && { return std::move(add(gp_on_key_down_data(fn, data))); }
    template <class F> requires(!std::is_convertible_v<F, gp_key_fn>) Element& on_key_down(F&& f) & {
        return add(gp_on_key_down_data(&detail::key_tramp<std::decay_t<F>>, detail::box(std::forward<F>(f))));
    }
    template <class F> requires(!std::is_convertible_v<F, gp_key_fn>) Element&& on_key_down(F&& f) && { on_key_down(std::forward<F>(f)); return std::move(*this); }
    Element& on_key_up(gp_key_fn fn, void* data = nullptr) & { return add(gp_on_key_up_data(fn, data)); }
    Element&& on_key_up(gp_key_fn fn, void* data = nullptr) && { return std::move(add(gp_on_key_up_data(fn, data))); }
    template <class F> requires(!std::is_convertible_v<F, gp_key_fn>) Element& on_key_up(F&& f) & {
        return add(gp_on_key_up_data(&detail::key_tramp<std::decay_t<F>>, detail::box(std::forward<F>(f))));
    }
    template <class F> requires(!std::is_convertible_v<F, gp_key_fn>) Element&& on_key_up(F&& f) && { on_key_up(std::forward<F>(f)); return std::move(*this); }

    Element& on_text_input(gp_text_input_fn fn, void* data = nullptr) & { return add(gp_on_text_input_data(fn, data)); }
    Element&& on_text_input(gp_text_input_fn fn, void* data = nullptr) && { return std::move(add(gp_on_text_input_data(fn, data))); }
    template <class F> requires(!std::is_convertible_v<F, gp_text_input_fn>) Element& on_text_input(F&& f) & {
        return add(gp_on_text_input_data(&detail::text_tramp<std::decay_t<F>>, detail::box(std::forward<F>(f))));
    }
    template <class F> requires(!std::is_convertible_v<F, gp_text_input_fn>) Element&& on_text_input(F&& f) && { on_text_input(std::forward<F>(f)); return std::move(*this); }

    /** An action bound to a key in a keymap (gp_bind_key), handled here; `name` is the action's name, as bound. */
    Element& on_action(Str name, gp_action_fn fn, void* data = nullptr) & { return add(gp_on_action_data(name.c_str(), fn, data)); }
    Element&& on_action(Str name, gp_action_fn fn, void* data = nullptr) && { return std::move(add(gp_on_action_data(name.c_str(), fn, data))); }
    template <class F> requires(!std::is_convertible_v<F, gp_action_fn>) Element& on_action(Str name, F&& f) & {
        return add(gp_on_action_data(name.c_str(), &detail::action_tramp<std::decay_t<F>>, detail::box(std::forward<F>(f))));
    }
    template <class F> requires(!std::is_convertible_v<F, gp_action_fn>) Element&& on_action(Str name, F&& f) && { on_action(name, std::forward<F>(f)); return std::move(*this); }

    Element& on_layout(gp_layout_fn fn) & { return add(gp_on_layout(fn)); }
    Element&& on_layout(gp_layout_fn fn) && { return std::move(add(gp_on_layout(fn))); }
    template <class F> requires(!std::is_convertible_v<F, gp_layout_fn>) Element& on_layout(F&&) & {
        static_assert(detail::stateless_v<std::decay_t<F>>, "gpuic: on_layout has no data pointer in the C library: the lambda cannot capture");
        return add(gp_on_layout(&detail::layout_tramp<std::decay_t<F>>));
    }
    template <class F> requires(!std::is_convertible_v<F, gp_layout_fn>) Element&& on_layout(F&& f) && { on_layout(std::forward<F>(f)); return std::move(*this); }
    /** On a uniform_list or list: the header of the group of the first row in view stays pinned at the top (see gp_list_sticky). `f(Self&, Ctx, size_t row)`
     *  returns a ListGroup{header, first row of the group, first row of the next}. */
    template <class F> Element& list_sticky(F&& f) & {
        return add(gp_list_sticky(&detail::group_tramp<std::decay_t<F>>, detail::box(std::forward<F>(f))));
    }
    template <class F> Element&& list_sticky(F&& f) && { list_sticky(std::forward<F>(f)); return std::move(*this); }

    /** On an element that scrolls: called with its offset (a gp_point) when it moves and after every layout; see gp_on_scroll. */
    Element& on_scroll(gp_scroll_fn fn) & { return add(gp_on_scroll(fn)); }
    Element&& on_scroll(gp_scroll_fn fn) && { return std::move(add(gp_on_scroll(fn))); }
    template <class F> requires(!std::is_convertible_v<F, gp_scroll_fn>) Element& on_scroll(F&&) & {
        static_assert(detail::stateless_v<std::decay_t<F>>, "gpuic: on_scroll has no data pointer in the C library: the lambda cannot capture");
        return add(gp_on_scroll(&detail::scroll_tramp<std::decay_t<F>>));
    }
    template <class F> requires(!std::is_convertible_v<F, gp_scroll_fn>) Element&& on_scroll(F&& f) && { on_scroll(std::forward<F>(f)); return std::move(*this); }
    Element& on_ime(gp_ime_fn fn) & { return add(gp_on_ime(fn)); }
    Element&& on_ime(gp_ime_fn fn) && { return std::move(add(gp_on_ime(fn))); }
    template <class F> requires(!std::is_convertible_v<F, gp_ime_fn>) Element& on_ime(F&&) & {
        static_assert(detail::stateless_v<std::decay_t<F>>, "gpuic: on_ime has no data pointer in the C library: the lambda cannot capture");
        return add(gp_on_ime(&detail::ime_tramp<std::decay_t<F>>));
    }
    template <class F> requires(!std::is_convertible_v<F, gp_ime_fn>) Element&& on_ime(F&& f) && { on_ime(std::forward<F>(f)); return std::move(*this); }

    /** GPUI's with_animation: `f(t)` gets the eased progress and returns the modifiers to lay over the element, as a style(). */
    template <class F> Element& with_animation(Str id, gp_animation animation, F&& f) & {
        return add(gp_with_animation(id.c_str(), animation, &detail::anim_tramp<std::decay_t<F>>, detail::box(std::forward<F>(f))));
    }
    template <class F> Element&& with_animation(Str id, gp_animation animation, F&& f) && { with_animation(id, animation, std::forward<F>(f)); return std::move(*this); }

    // ---- everything else: the modifiers of the C header and GPUI's spellings, generated
#include <gpuic/gpuic_chain.inc>

private:
    friend Element div();
    friend Element text(Str);
    friend Element img(gp_image);
    friend Element svg_img(gp_svg);
    friend Element curve(gp_point, gp_point, gp_point, gp_point);
    friend Element polyline(const gp_point*, size_t);
    friend Element style();
    friend Element raw(gp_node*);
    template <class F> friend Element uniform_list(size_t, float, F&&);
    template <class F> friend Element list(size_t, float, F&&);

    enum class Kind : uint8_t { Div, Text, Img, Svg, Curve, Polyline, UList, List, Group, Raw };
    explicit Element(Kind k) : kind_(k) {}

    Element& add(gp_node* mod) { mods_.push(mod); return *this; }

    Kind kind_ = Kind::Div;
    const char* str_ = nullptr;
    gp_image img_{};
    gp_svg svg_{};
    gp_point* pts_ = nullptr;   // a curve's four points, a polyline's n (copied into the frame's memory)
    size_t npts_ = 0;
    gp_node* raw_ = nullptr;
    size_t count_ = 0;
    float height_ = 0;
    gp_list_item_fn list_fn_ = nullptr;
    void* data_ = nullptr;
    detail::Vec mods_, kids_;
};

/** A div: every element is a flex container, a column unless .flex_row(). */
inline Element div() { return Element(Element::Kind::Div); }
/** A text element; the string is copied. Its modifiers (text_size, font_bold, text_color) apply to this text and are inherited. */
inline Element text(Str s) { Element e(Element::Kind::Text); e.str_ = s.c_str() ? s.c_str() : ""; return e; }
inline Element img(gp_image image) { Element e(Element::Kind::Img); e.img_ = image; return e; }
inline Element svg_img(gp_svg svg) { Element e(Element::Kind::Svg); e.svg_ = svg; return e; }
/** A point, for the curve and polyline below: `xy(10, 20)` (or a braced `{10, 20}`). */
inline gp_point xy(float x, float y) { return gp_point{ x, y }; }
/** A stroked cubic Bezier from p0 to p3 with control points c1 and c2 (see gp_curve). The points are in the parent's space, where an absolute child's .left() and .top() are; the
 *  element places itself, and its modifiers are the stroke ones: .stroke_width, .stroke_color, .stroke_cap_butt/_round, .stroke_join_miter/_round, .miter_limit, .hit_tolerance (and the
 *  handlers and state styles: only the stroke is hit). */
inline Element curve(gp_point p0, gp_point c1, gp_point c2, gp_point p3) {
    Element e(Element::Kind::Curve);
    e.pts_ = static_cast<gp_point*>(gp_frame_alloc(4 * sizeof(gp_point)));
    e.pts_[0] = p0; e.pts_[1] = c1; e.pts_[2] = c2; e.pts_[3] = p3;
    e.npts_ = 4;
    return e;
}
/** A stroked polyline through the points (copied now: a std::vector or an initializer list may go away). */
inline Element polyline(const gp_point* pts, size_t n) {
    Element e(Element::Kind::Polyline);
    e.pts_ = static_cast<gp_point*>(gp_frame_alloc((n ? n : 1) * sizeof(gp_point)));
    for (size_t i = 0; i < n; i++) e.pts_[i] = pts[i];
    e.npts_ = n;
    return e;
}
inline Element polyline(std::initializer_list<gp_point> pts) { return polyline(pts.begin(), pts.size()); }
template <class R> requires(!std::is_convertible_v<R, const gp_point*>) Element polyline(const R& range) {
    size_t n = 0;
    for (const auto& p : range) { (void)p; n++; }
    gp_point* tmp = static_cast<gp_point*>(gp_frame_alloc((n ? n : 1) * sizeof(gp_point)));
    size_t i = 0;
    for (const auto& p : range) tmp[i++] = gp_point{ p.x, p.y };
    return polyline(tmp, n);
}
/** Modifiers on their own, to be returned from a with_animation function or given to .refine(). */
inline Element style() { return Element(Element::Kind::Group); }

namespace detail {
template <class F> gp_node* state_style(F& f) {
    Element s = style();
    if constexpr (std::is_invocable_v<F&, Element&&>) {
        if constexpr (std::is_void_v<std::invoke_result_t<F&, Element&&>>) f(std::move(s));
        else s = f(std::move(s));
    } else {
        if constexpr (std::is_void_v<std::invoke_result_t<F&, Element&>>) f(s);
        else s = f(s);
    }
    return s.build();
}
}  // namespace detail
/** Wraps a node made by the C API (no modifiers can be added to it afterwards). */
inline Element raw(gp_node* node) { Element e(Element::Kind::Raw); e.raw_ = node; return e; }

using detail::ListGroup;

/** The settings of an animation, for with_animation (duration in seconds; 0 jumps to the end). */
inline gp_animation animation(double duration, int easing = GP_EASE_LINEAR, bool repeat = false, bool ping_pong = false, double delay = 0) {
    gp_animation a{};
    a.duration = duration;
    a.delay = delay;
    a.repeat = repeat;
    a.ping_pong = ping_pong;
    a.easing = easing;
    return a;
}

/**
 * A virtualized list of rows of one height; `row(Self&, Ctx, size_t index)` (any subset, any order) makes the row at `index`, and
 * is called for the rows in view only. Needs a bounded height (.h(...), .flex_1() ...); see gp_uniform_list.
 */
template <class F> Element uniform_list(size_t count, float item_height, F&& row) {
    Element e(Element::Kind::UList);
    e.count_ = count;
    e.height_ = item_height;
    e.list_fn_ = &detail::list_tramp<std::decay_t<F>>;
    e.data_ = detail::box(std::forward<F>(row));
    return e;
}
/** The same for rows of different heights (gp_list); `estimated_height` is used for rows not measured yet. */
template <class F> Element list(size_t count, float estimated_height, F&& row) {
    Element e(Element::Kind::List);
    e.count_ = count;
    e.height_ = estimated_height;
    e.list_fn_ = &detail::list_tramp<std::decay_t<F>>;
    e.data_ = detail::box(std::forward<F>(row));
    return e;
}

namespace detail {
inline gp_node* into_node(Element&& e) { return e.build(); }
inline gp_node* into_node(const Element& e) { return e.build(); }
inline gp_node* into_node(const char* s) { return s ? text(s).build() : nullptr; }
inline gp_node* into_node(const std::string& s) { return text(s).build(); }
inline gp_node* into_node(std::string_view s) { return text(s).build(); }
inline gp_node* into_node(const SharedString& s) { return text(s).build(); }
}  // namespace detail

// ---------------------------------------------------------------- state: entities, views, the app

/** A typed handle to state the app owns (gp_entity): Rust's Entity<T>. */
template <class S> struct Entity {
    gp_entity id{};
    Entity() = default;
    explicit Entity(gp_entity e) : id(e) {}
    operator gp_entity() const { return id; }
    explicit operator bool() const { return id.generation != 0; }
    inline S* get(App app) const;
    /** The state, or nullptr once it has been released. */
    inline S* get(Ctx cx) const;
    /** `f(S&, Ctx)` with a ctx for this entity: what gp_notify inside it refers to. */
    template <class F> void update(Ctx cx, F&& f) const {
        gp_ctx c = gp_ctx_for(cx.raw()->app, id);
        c.window = cx.raw()->window;
        if (S* s = get(cx)) f(*s, Ctx(&c));
    }
    /** This view as a child of another (gp_child_view), re-rendered with its parent; .cached() only when it notifies. */
    Element element() const { return raw(gp_child_view(id)); }
    Element cached() const { return raw(gp_child_view_cached(id)); }
};

namespace detail {
template <class S> gp_node* into_node_entity(const Entity<S>& e) { return gp_child_view(e.id); }
template <class S> inline gp_node* into_node(const Entity<S>& e) { return into_node_entity(e); }
template <class S> void drop(void* p) { static_cast<S*>(p)->~S(); }
template <class S> gp_node* render_member(void* self, gp_ctx* cx) { return into_node(static_cast<S*>(self)->render(Ctx(cx))); }
template <class S, class D> gp_node* render_lambda(void* self, gp_ctx* cx) {
    static_assert(stateless_v<D>, "gpuic: new_view_with takes a lambda with no captures (the state is its first parameter)");
    D f{};
    Ctx ctx(cx);
    return into_node(call(f, self, ctx, cx));
}
}  // namespace detail

/** A window (gp_window): what App::open_window returns. The handle is only good until the window closes. */
class Window {
public:
    Window() = default;
    Window(gp_app* app, gp_window* w) : app_(app), w_(w) {}
    gp_window* raw() const { return w_; }
    operator gp_window*() const { return w_; }
    explicit operator bool() const { return w_ != nullptr; }
    /** Closes it at the next turn of the loop; the app ends with its last window. */
    void close() const { if (w_) gp_window_close(w_); }
    /** A ctx tied to this window, for work that should end with it (timers, animations, coroutines started with it are cancelled
     *  when the window closes) and for focus and scroll calls that act on it. */
    Ctx ctx(gp_entity e = GP_ENTITY_NULL) const { return Ctx(gp_ctx_for_window(app_, w_, e)); }

private:
    gp_app* app_ = nullptr;
    gp_window* w_ = nullptr;
};

class App {
public:
    explicit App(gp_app* a) : a_(a) {}
    gp_app* raw() const { return a_; }
    operator gp_app*() const { return a_; }

    /** State without a view. T is constructed inside the app and destroyed when the entity is released. */
    template <class S, class... A> Entity<S> new_entity(A&&... args) const {
        static_assert(alignof(S) <= alignof(std::max_align_t));
        gp_entity e = gp_entity_new(a_, sizeof(S), nullptr, std::is_trivially_destructible_v<S> ? nullptr : &detail::drop<S>);
        new (gp_entity_get(a_, e)) S(std::forward<A>(args)...);
        return Entity<S>(e);
    }
    /** A view: state that renders itself. S has `Element render(Ctx)` (or returning a gp_node *). */
    template <class S, class... A> Entity<S> new_view(A&&... args) const {
        static_assert(alignof(S) <= alignof(std::max_align_t));
        static_assert(requires(S& s, Ctx c) { detail::into_node(s.render(c)); }, "gpuic: a view needs a member `Element render(Ctx cx)`");
        gp_entity e = gp_view_new(a_, sizeof(S), nullptr, &detail::render_member<S>, std::is_trivially_destructible_v<S> ? nullptr : &detail::drop<S>);
        new (gp_entity_get(a_, e)) S(std::forward<A>(args)...);
        return Entity<S>(e);
    }
    /** A view whose render function is a lambda with no captures: `[](S& self, Ctx cx) { return div()...; }`. */
    template <class S, class R, class... A> Entity<S> new_view_with(R, A&&... args) const {
        static_assert(alignof(S) <= alignof(std::max_align_t));
        gp_entity e = gp_view_new(a_, sizeof(S), nullptr, &detail::render_lambda<S, R>, std::is_trivially_destructible_v<S> ? nullptr : &detail::drop<S>);
        new (gp_entity_get(a_, e)) S(std::forward<A>(args)...);
        return Entity<S>(e);
    }
    template <class S> S* get(Entity<S> e) const { return static_cast<S*>(gp_entity_get(a_, e.id)); }
    void release(gp_entity e) const { gp_entity_release(a_, e); }

    Window open_window(const gp_window_options& options, gp_entity root_view) const { return Window(a_, gp_open_window(a_, &options, root_view)); }
    Window open_window(const char* title, int width, int height, gp_entity root_view, Color background = Color{ 0, 0, 0, 0 }) const {
        gp_window_options o{};
        o.title = title;
        o.width = width;
        o.height = height;
        o.background = background;
        return Window(a_, gp_open_window(a_, &o, root_view));
    }
    /** Pictures and fonts (main thread). A null image / svg means it could not be read or decoded. */
    gp_image load_image(const char* path) const { return gp_image_load_file(a_, path); }
    gp_image load_image(const void* data, std::size_t size) const { return gp_image_load_memory(a_, data, size); }
    gp_image image_from_rgba(int width, int height, const void* rgba) const { return gp_image_from_rgba(a_, width, height, rgba); }
    void release(gp_image image) const { gp_image_release(a_, image); }
    gp_svg load_svg(const char* path) const { return gp_svg_load_file(a_, path); }
    gp_svg load_svg(std::string_view svg) const { return gp_svg_load_memory(a_, svg.data(), svg.size()); }
    void release(gp_svg svg) const { gp_svg_release(a_, svg); }
    /** Registers a font file / the bytes of one; `family` null takes the font's own name, `weight` 0 its OS/2 weight. */
    bool add_font(const char* path, const char* family = nullptr, int weight = 0, int face_index = 0) const {
        gp_font_options o{}; o.family = family; o.weight = weight; o.face_index = face_index;
        return gp_font_add_file(a_, path, &o);
    }
    bool add_font(const void* data, std::size_t size, const char* family = nullptr, int weight = 0, int face_index = 0) const {
        gp_font_options o{}; o.family = family; o.weight = weight; o.face_index = face_index;
        return gp_font_add_memory(a_, data, size, &o);
    }
    inline class TextInputBuilder text_input() const;
    Focus new_focus() const { return Focus::create(a_); }
    Scroll new_scroll() const { return Scroll::create(a_); }
    /** Wheel scrolling glides to its target over about `seconds` (0: it jumps, the default). */
    void set_smooth_scrolling(float seconds) const { gp_set_smooth_scrolling(a_, seconds); }

    /** Binds a keystroke ("ctrl-shift-s", "enter", "ctrl-k ctrl-c" for a chord) to an action name ("editor::save"), for `context` (a key_context
     *  predicate over the focus path: "Editor && !ReadOnly"; null: anywhere). False if the keystroke does not parse. Handlers are .on_action(name, f). */
    bool bind_key(const char* keystroke, const char* action, const char* context = nullptr) const { return gp_bind_key(a_, keystroke, action, context); }
    /** ... with a payload (trivially copyable) that the handler reads with cx.payload<P>(): "ctrl-1".."ctrl-9" -> "tab::select" with 1..9. */
    template <class P> bool bind_key(const char* keystroke, const char* action, const char* context, const P& payload) const {
        static_assert(std::is_trivially_copyable_v<P>);
        return gp_bind_key_with(a_, keystroke, action, context, &payload, sizeof(P));
    }
    void clear_key_bindings() const { gp_clear_key_bindings(a_); }
    /** Handles an action nobody on the focus path handled: `f(Ctx)`; kept for the life of the app and destroyed once with it. */
    template <class F> void on_global_action(const char* action, F&& f) const;

    /** The clipboard / primary selection text, if it holds any. */
    std::optional<std::string> clipboard() const { return take(gp_clipboard_get(a_)); }
    void set_clipboard(const std::string& utf8) const { gp_clipboard_set(a_, utf8.c_str()); }
    std::optional<std::string> primary() const { return take(gp_primary_get(a_)); }
    void set_primary(const std::string& utf8) const { gp_primary_set(a_, utf8.c_str()); }
    void quit() const { gp_quit(a_); }
    /** Everything is stale: every window rebuilds. */
    void notify() const { gp_app_notify(a_); }
    /** A ctx for code outside any callback (setup, tests), optionally for one entity. */
    Ctx ctx(gp_entity e = GP_ENTITY_NULL) const { return Ctx(gp_ctx_for(a_, e)); }

private:
    static std::optional<std::string> take(char* p) {
        if (!p) return std::nullopt;
        std::string r(p);
        std::free(p);
        return r;
    }
    gp_app* a_;
};

inline App Ctx::app() const { return App(c_->app); }
inline void Ctx::focus(Focus f) const { gp_focus_set(c_, f.raw()); }
inline bool Ctx::is_focused(Focus f) const { return gp_is_focused(c_, f.raw()); }
inline void Ctx::scroll_into_view(Focus f) const { gp_scroll_into_view(c_, f.raw()); }

namespace detail {

template <class D> void task_tramp(void* self, gp_ctx* cx, void* data) { Ctx ctx(cx); call(*static_cast<D*>(data), self, ctx, cx); }
template <class D> void frame_tramp(void* self, gp_ctx* cx, double time, void* data) { Ctx ctx(cx); call(*static_cast<D*>(data), self, ctx, cx, time); }
template <class D> void anim_fn_tramp(void* self, gp_ctx* cx, float t, void* data) { Ctx ctx(cx); call(*static_cast<D*>(data), self, ctx, cx, t); }
template <class D> void fd_tramp(void* self, gp_ctx* cx, int fd, int events, void* data) { Ctx ctx(cx); FdEvent ev{ fd, events }; call(*static_cast<D*>(data), self, ctx, cx, ev); }
template <class D> void image_tramp(void* self, gp_ctx* cx, gp_image image, void* data) { Ctx ctx(cx); call(*static_cast<D*>(data), self, ctx, cx, image); }
template <class D> void co_tramp(gp_co* co, void* self, gp_ctx* cx, void* data) { Co c(co); Ctx ctx(cx); call(*static_cast<D*>(data), self, c, ctx, cx); }
template <class D> void observe_tramp(void* self, gp_ctx* cx, gp_entity observed, void* data) { Ctx ctx(cx); call(*static_cast<D*>(data), self, ctx, cx, observed); }
template <class D> void event_tramp(void* self, gp_ctx* cx, const gp_event* ev, void* data) { Ctx ctx(cx); call(*static_cast<D*>(data), self, ctx, cx, *ev); }
template <class D> void release_tramp(void* self, gp_ctx* cx, gp_entity released, void* state, void* data) {
    Ctx ctx(cx);
    call(*static_cast<D*>(data), self, ctx, cx, released, state);
}

/** What background() keeps: both callables and the result between them. */
template <class W, class D, class R> struct Bg {
    W work;
    D done;
    std::optional<R> result;
};
template <class W, class D> struct Bg<W, D, void> {
    W work;
    D done;
};
template <class B> void bg_work(void* data) {
    auto* b = static_cast<B*>(data);
    if constexpr (requires { b->result; }) b->result.emplace(b->work());
    else b->work();
}
template <class B> void bg_done(void* self, gp_ctx* cx, void* data) {
    auto* b = static_cast<B*>(data);
    Ctx ctx(cx);
    if constexpr (requires { b->result; }) call(b->done, self, ctx, cx, *b->result);
    else call(b->done, self, ctx, cx);
}

template <class D, class F> D* own(F&& f) { return new D(std::forward<F>(f)); }

}  // namespace detail

namespace detail {
/** What on_global_action keeps: the callable, destroyed with the entity that holds it (the app frees its entities at the end). */
template <class D> struct GlobalHolder {
    D* d;
    explicit GlobalHolder(D* p) : d(p) {}
    GlobalHolder(const GlobalHolder&) = delete;
    ~GlobalHolder() { delete d; }
};
template <class D> void global_tramp(void*, gp_ctx* cx, void* data) { Ctx ctx(cx); call(*static_cast<D*>(data), nullptr, ctx, cx); }
}  // namespace detail

template <class F> void App::on_global_action(const char* action, F&& f) const {
    using D = std::decay_t<F>;
    D* d = detail::own<D>(std::forward<F>(f));
    new_entity<detail::GlobalHolder<D>>(d);
    gp_on_global_action(a_, action, &detail::global_tramp<D>, d);
}

template <class F> Task Ctx::after(double seconds, F&& f) const {
    using D = std::decay_t<F>;
    D* d = detail::own<D>(std::forward<F>(f));
    Task t(c_->app, gp_after(c_, seconds, &detail::task_tramp<D>, d));
    gp_task_on_drop(c_->app, t.raw(), &detail::drop_box<D>, d);
    return t;
}
template <class F> Task Ctx::every(double seconds, F&& f) const {
    using D = std::decay_t<F>;
    D* d = detail::own<D>(std::forward<F>(f));
    Task t(c_->app, gp_every(c_, seconds, &detail::task_tramp<D>, d));
    gp_task_on_drop(c_->app, t.raw(), &detail::drop_box<D>, d);
    return t;
}
template <class F> Task Ctx::next_frame(F&& f) const {
    using D = std::decay_t<F>;
    D* d = detail::own<D>(std::forward<F>(f));
    Task t(c_->app, gp_next_frame(c_, &detail::frame_tramp<D>, d));
    gp_task_on_drop(c_->app, t.raw(), &detail::drop_box<D>, d);
    return t;
}
template <class F> Task Ctx::animate(double seconds, int easing, F&& f) const {
    using D = std::decay_t<F>;
    D* d = detail::own<D>(std::forward<F>(f));
    Task t(c_->app, gp_animate(c_, seconds, easing, &detail::anim_fn_tramp<D>, d));
    gp_task_on_drop(c_->app, t.raw(), &detail::drop_box<D>, d);
    return t;
}
template <class W, class D2> Task Ctx::background(W&& work, D2&& done) const {
    using Wd = std::decay_t<W>;
    using Dd = std::decay_t<D2>;
    using R = std::invoke_result_t<Wd&>;
    using B = detail::Bg<Wd, Dd, std::conditional_t<std::is_void_v<R>, void, R>>;
    B* b;
    if constexpr (std::is_void_v<R>) b = new B{ std::forward<W>(work), std::forward<D2>(done) };
    else b = new B{ std::forward<W>(work), std::forward<D2>(done), std::nullopt };
    Task t(c_->app, gp_background(c_, &detail::bg_work<B>, &detail::bg_done<B>, b, nullptr));
    gp_task_on_drop(c_->app, t.raw(), &detail::drop_box<B>, b);
    return t;
}
template <class F> Task Ctx::spawn(F&& f) const {
    using D = std::decay_t<F>;
    D* d = detail::own<D>(std::forward<F>(f));
    Task t(c_->app, gp_spawn(c_, &detail::co_tramp<D>, d));
    gp_task_on_drop(c_->app, t.raw(), &detail::drop_box<D>, d);
    return t;
}
template <class F> Task Ctx::watch_fd(int fd, int events, F&& f) const {
    using D = std::decay_t<F>;
    D* d = detail::own<D>(std::forward<F>(f));
    Task t(c_->app, gp_watch_fd(c_, fd, events, &detail::fd_tramp<D>, d));
    gp_task_on_drop(c_->app, t.raw(), &detail::drop_box<D>, d);
    return t;
}
template <class F> Task Ctx::load_image(const char* path, F&& f) const {
    using D = std::decay_t<F>;
    D* d = detail::own<D>(std::forward<F>(f));
    Task t(c_->app, gp_image_load_file_async(c_, path, &detail::image_tramp<D>, d));
    gp_task_on_drop(c_->app, t.raw(), &detail::drop_box<D>, d);
    return t;
}
template <class F> Subscription Ctx::observe(gp_entity target, F&& f) const {
    using D = std::decay_t<F>;
    D* d = detail::own<D>(std::forward<F>(f));
    Subscription s(c_->app, gp_observe(c_, target, &detail::observe_tramp<D>, d));
    gp_subscription_on_drop(c_->app, s.raw(), &detail::drop_box<D>, d);
    return s;
}
template <class F> Subscription Ctx::subscribe(gp_entity emitter, const char* event, F&& f) const {
    using D = std::decay_t<F>;
    D* d = detail::own<D>(std::forward<F>(f));
    Subscription s(c_->app, gp_subscribe(c_, emitter, event, &detail::event_tramp<D>, d));
    gp_subscription_on_drop(c_->app, s.raw(), &detail::drop_box<D>, d);
    return s;
}
template <class F> Subscription Ctx::on_release(gp_entity target, F&& f) const {
    using D = std::decay_t<F>;
    D* d = detail::own<D>(std::forward<F>(f));
    Subscription s(c_->app, gp_observe_release(c_, target, &detail::release_tramp<D>, d));
    gp_subscription_on_drop(c_->app, s.raw(), &detail::drop_box<D>, d);
    return s;
}
template <class S> S* Entity<S>::get(App app) const { return app.get(*this); }
template <class S> S* Entity<S>::get(Ctx cx) const { return cx.app().get(*this); }

// ---------------------------------------------------------------- text inputs

/** A text field made by App::text_input(): a view, shown with .element() (or as a child directly). */
struct TextInput {
    gp_entity id{};
    TextInput() = default;
    explicit TextInput(gp_entity e) : id(e) {}
    operator gp_entity() const { return id; }
    explicit operator bool() const { return id.generation != 0; }
    Element element() const { return raw(gp_child_view(id)); }
    std::string text(App app) const { return gp_text_input_text(app, id); }
    void set_text(Ctx cx, const std::string& utf8) const { gp_text_input_set_text(cx, id, utf8.c_str()); }
    void select_all(Ctx cx) const { gp_text_input_select_all(cx, id); }
    /** Byte offsets of the selection (equal when there is none). */
    std::pair<std::size_t, std::size_t> selection(App app) const { std::size_t a = 0, b = 0; gp_text_input_selection(app, id, &a, &b); return { a, b }; }
    std::string preedit(App app) const { return gp_text_input_preedit(app, id); }
    gp_focus focus(App app) const { return gp_text_input_focus(app, id); }
    std::size_t rows(App app) const { return gp_text_input_rows(app, id); }
};

namespace detail {
struct TextInputCallbacks {
    std::function<void(void*, gp_ctx*, gp_entity)> change, submit;
};
inline void text_input_change(void* self, gp_ctx* cx, gp_entity input, void* data) { static_cast<TextInputCallbacks*>(data)->change(self, cx, input); }
inline void text_input_submit(void* self, gp_ctx* cx, gp_entity input, void* data) { static_cast<TextInputCallbacks*>(data)->submit(self, cx, input); }
inline void text_input_released(void*, gp_ctx*, gp_entity, void*, void*) {}
}  // namespace detail

/** Chained settings for a text field: `app.text_input().owner(me).placeholder("Name").on_submit(...).build()`. The callbacks are
 *  `f(Self&, Ctx, gp_entity input)` (any subset, as for handlers) with Self the owner's state; they may capture anything copyable, and
 *  are destroyed once, when the field is released. Colors left unset take the dark-theme defaults. */
class TextInputBuilder {
public:
    explicit TextInputBuilder(gp_app* app) : app_(app) {}
    TextInputBuilder& owner(gp_entity e) { o_.owner = e; return *this; }
    TextInputBuilder& text(std::string s) { text_ = std::move(s); return *this; }
    TextInputBuilder& placeholder(std::string s) { placeholder_ = std::move(s); return *this; }
    TextInputBuilder& multiline(bool on = true) { o_.multiline = on; return *this; }
    TextInputBuilder& password(bool on = true) { o_.password = on; return *this; }
    TextInputBuilder& read_only(bool on = true) { o_.read_only = on; return *this; }
    TextInputBuilder& soft_wrap(bool on = true) { o_.soft_wrap = on; return *this; }
    TextInputBuilder& direction(int d) { o_.direction = d; return *this; }
    TextInputBuilder& rows(int n) { o_.rows = n; return *this; }
    TextInputBuilder& max_bytes(std::size_t n) { o_.max_bytes = n; return *this; }
    TextInputBuilder& font_size(float px) { o_.font_size = px; return *this; }
    TextInputBuilder& width(float px) { o_.width = px; return *this; }
    TextInputBuilder& radius(float px) { o_.radius = px; return *this; }
    TextInputBuilder& padding(float x, float y) { o_.padding_x = x; o_.padding_y = y; return *this; }
    TextInputBuilder& text_color(Color c) { o_.text_color = c; return *this; }
    TextInputBuilder& placeholder_color(Color c) { o_.placeholder_color = c; return *this; }
    TextInputBuilder& bg(Color c) { o_.bg = c; return *this; }
    TextInputBuilder& border_color(Color c) { o_.border_color = c; return *this; }
    TextInputBuilder& focus_border_color(Color c) { o_.focus_border_color = c; return *this; }
    TextInputBuilder& selection_color(Color c) { o_.selection_color = c; return *this; }
    TextInputBuilder& caret_color(Color c) { o_.caret_color = c; return *this; }
    template <class F> TextInputBuilder& on_change(F f) {
        change_ = [f = std::move(f)](void* self, gp_ctx* cx, gp_entity input) mutable { Ctx ctx(cx); detail::call(f, self, ctx, cx, input); };
        return *this;
    }
    template <class F> TextInputBuilder& on_submit(F f) {
        submit_ = [f = std::move(f)](void* self, gp_ctx* cx, gp_entity input) mutable { Ctx ctx(cx); detail::call(f, self, ctx, cx, input); };
        return *this;
    }
    TextInput build() {
        gp_text_input_options o = o_;
        o.text = text_.c_str();
        o.placeholder = placeholder_.empty() ? nullptr : placeholder_.c_str();
        auto* cb = new detail::TextInputCallbacks{ std::move(change_), std::move(submit_) };
        o.data = cb;
        o.on_change = cb->change ? &detail::text_input_change : nullptr;
        o.on_submit = cb->submit ? &detail::text_input_submit : nullptr;
        gp_entity e = gp_text_input_new(app_, &o);
        gp_ctx c = gp_ctx_for(app_, GP_ENTITY_NULL);
        gp_subscription sub = gp_observe_release(&c, e, &detail::text_input_released, nullptr);
        gp_subscription_on_drop(app_, sub, &detail::drop_box<detail::TextInputCallbacks>, cb);
        return TextInput(e);
    }

private:
    gp_app* app_;
    gp_text_input_options o_{};
    std::string text_, placeholder_;
    std::function<void(void*, gp_ctx*, gp_entity)> change_, submit_;
};

inline TextInputBuilder App::text_input() const { return TextInputBuilder(a_); }

namespace detail {
template <class F> void ready_tramp(gp_app* a, void* ud) { (*static_cast<F*>(ud))(App(a)); }
}  // namespace detail

/** Creates the app, calls `ready(App)` once and runs until the last window closes or quit(); the exit code. */
template <class F> int run(F&& ready) {
    return gp_application_run(&detail::ready_tramp<std::remove_reference_t<F>>, &ready);
}

}  // namespace gpuic

template <> struct std::hash<gpuic::SharedString> {
    std::size_t operator()(const gpuic::SharedString& s) const noexcept { return std::hash<std::string_view>{}(s.view()); }
};

