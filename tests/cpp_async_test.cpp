/*
 * The C++ layer's timers, background work, coroutines, observation, watches, text fields and loaders: each takes a callable that is
 * owned by the task or subscription and destroyed exactly once, however it ends (ran, cancelled, owner released, app shut down).
 * A callable holding a shared_ptr tells: use_count() is back to 1 when it has been destroyed, and the object it points to counts
 * the destructions. Needs a display (an event loop). Exits non-zero on failure.
 */
#include <gpuic/gpuic.hpp>

#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <thread>

using namespace gpuic;

static int failures;
#define CHECK(cond, ...)                                                               \
    do {                                                                               \
        if (!(cond)) {                                                                 \
            failures++;                                                                \
            std::printf("FAIL %s:%d: ", __FILE__, __LINE__);                           \
            std::printf(__VA_ARGS__);                                                  \
            std::printf("\n");                                                         \
        }                                                                              \
    } while (0)

/** A callable's payload: counts how many copies exist, so "destroyed once" is use_count() == 1 again at the end. */
using Tok = std::shared_ptr<int>;
static Tok make_tok() { return std::make_shared<int>(0); }

struct Model {
    int fired = 0, ticks = 0;
    std::string log;
    bool saw_self = false;
};

static std::thread::id main_id;

// ------------------------------------------------------------------------------------------------ timers

static void test_timers(Co& co, Ctx cx) {
    App app = cx.app();
    auto m = app.new_entity<Model>();
    Ctx mc = app.ctx(m.id);
    Model* model = m.get(app);

    {   // after: self by reference, captured std::string and shared_ptr (not trivially destructible), runs once, closure destroyed
        Tok tok = make_tok();
        std::string name = "later";
        Task t = mc.after(0.05, [tok, name](Model& self, Ctx c) { self.fired++; self.log += name; (*tok)++; });
        CHECK(t.active() && tok.use_count() == 2, "an armed timer holds its closure (use_count %ld)", tok.use_count());
        co.sleep(0.2);
        CHECK(model->fired == 1 && model->log == "later" && *tok == 1, "after ran once with the model as self and its captures (%d, '%s')", model->fired, model->log.c_str());
        CHECK(!t.active(), "a one-shot is over");
        co.sleep(0.05);
        CHECK(tok.use_count() == 1, "the closure of a timer that ran is destroyed (use_count %ld)", tok.use_count());
    }
    {   // every: ticks, then cancelled: destroyed once, no more ticks
        Tok tok = make_tok();
        Task t = mc.every(0.02, [tok](Model& self) { self.ticks++; });
        co.sleep(0.25);
        CHECK(model->ticks >= 5 && model->ticks <= 13, "every(20 ms) ticked %d times in 250 ms", model->ticks);
        t.cancel();
        co.sleep(0.05);
        int ticks = model->ticks;
        co.sleep(0.1);
        CHECK(model->ticks == ticks, "a cancelled timer stops (%d -> %d)", ticks, model->ticks);
        CHECK(tok.use_count() == 1, "a cancelled interval's closure is destroyed (use_count %ld)", tok.use_count());
        t.cancel();   // stale: harmless
        Task none;
        none.cancel();
        CHECK(!none.active() && !none, "a default Task is inactive");
    }
    {   // cancelled before it ever ran
        Tok tok = make_tok();
        bool ran = false;
        Task t = mc.after(0.05, [tok, &ran] { ran = true; });
        t.cancel();
        co.sleep(0.15);
        CHECK(!ran && tok.use_count() == 1, "a timer cancelled early does not run and is destroyed (ran %d, use_count %ld)", ran, tok.use_count());
    }
    {   // the entity is released first: the callback never runs (its self would be gone), the closure is still destroyed
        auto m2 = app.new_entity<Model>();
        Tok tok = make_tok();
        bool ran = false;
        app.ctx(m2.id).after(0.05, [tok, &ran](Model& self) { ran = true; });
        app.release(m2.id);
        co.sleep(0.2);
        CHECK(!ran && tok.use_count() == 1, "a timer of a released entity does not run and is destroyed (ran %d, use_count %ld)", ran, tok.use_count());
    }
    {   // a ctx for no entity: the closure takes no self; the callable may be mutable and move-only
        auto owned = std::make_unique<int>(41);
        int seen = 0;
        cx.after(0.02, [p = std::move(owned), &seen]() mutable { seen = ++*p; });
        co.sleep(0.15);
        CHECK(seen == 42, "an app-level, move-only, mutable closure ran (%d)", seen);
    }
    {   // next_frame / animate
        Tok tok = make_tok();
        double t0 = -1;
        mc.next_frame([tok, &t0](double time) { t0 = time; });
        co.sleep(0.2);
        CHECK(t0 > 0 && tok.use_count() == 1, "next_frame got the frame time (%g) and was destroyed", t0);
        Tok tok2 = make_tok();
        float last = -1;
        int calls = 0;
        mc.animate(0.1, GP_EASE_LINEAR, [tok2, &last, &calls](float t) { last = t; calls++; });
        co.sleep(0.3);
        CHECK(last == 1.f && calls >= 2 && tok2.use_count() == 1, "animate ends with t == 1 (%g, %d calls) and is destroyed", last, calls);
    }
    app.release(m.id);
}

// ------------------------------------------------------------------------------------------------ background

static void test_background(Co& co, Ctx cx) {
    App app = cx.app();
    auto m = app.new_entity<Model>();
    Ctx mc = app.ctx(m.id);
    Model* model = m.get(app);
    {   // a result crosses threads: work on a pool thread, done on this one with the value
        Tok tok = make_tok();
        std::thread::id worker, done_id;
        int got = 0;
        mc.background(
            [&worker, tok] { worker = std::this_thread::get_id(); return 7 * 6; },
            [&done_id, &got, tok](Model& self, int& r) { done_id = std::this_thread::get_id(); got = r; self.fired = 100; });
        co.sleep(0.3);
        CHECK(got == 42 && model->fired == 100, "done got the work's result (%d)", got);
        CHECK(worker != main_id && done_id == main_id, "work ran on a pool thread, done on the main one");
        CHECK(tok.use_count() == 1, "both closures are destroyed once it is over (use_count %ld)", tok.use_count());
    }
    {   // a std::string result, void work, cancelled
        std::string got;
        mc.background([] { return std::string("from the pool, long enough to leave the small string buffer"); }, [&got](std::string s) { got = s; });
        bool ran = false;
        mc.background([] {}, [&ran] { ran = true; });
        co.sleep(0.25);
        CHECK(got.size() > 30 && ran, "string result and void work (%zu, %d)", got.size(), ran);
        Tok tok = make_tok();
        bool done = false;
        Task t = mc.background([tok] { usleep(30000); }, [&done, tok] { done = true; });
        t.cancel();
        co.sleep(0.25);
        CHECK(!done && tok.use_count() == 1, "cancelled background: done did not run and the closures are destroyed (use_count %ld)", tok.use_count());
        auto m2 = app.new_entity<Model>();
        Tok tok2 = make_tok();
        bool done2 = false;
        app.ctx(m2.id).background([tok2] { usleep(30000); }, [&done2, tok2](Model&) { done2 = true; });
        app.release(m2.id);
        co.sleep(0.25);
        CHECK(!done2 && tok2.use_count() == 1, "released owner: done did not run, closures destroyed (use_count %ld)", tok2.use_count());
    }
    app.release(m.id);
}

// ------------------------------------------------------------------------------------------------ coroutines

static void test_coroutines(Co& co, Ctx cx) {
    App app = cx.app();
    auto m = app.new_entity<Model>();
    Ctx mc = app.ctx(m.id);
    Model* model = m.get(app);
    {   // a coroutine with all its awaits, self and a result from the pool
        Tok tok = make_tok();
        std::optional<int> result;
        bool void_ran = false;
        double frame_time = 0;
        Task t = mc.spawn([tok, &result, &void_ran, &frame_time](Co& c, Model& self, Ctx k) {
            self.log += "a";
            c.sleep(0.02);
            self.log += "b";
            c.yield();
            c.frame(&frame_time);
            result = c.run([] { return 5 + 6; });
            void_ran = c.run([] {});
            self.log += "c";
        });
        CHECK(t.active(), "a spawned coroutine is a task");
        co.sleep(0.4);
        CHECK(model->log == "abc" && result == 11 && void_ran && frame_time > 0, "the coroutine ran through its awaits ('%s', %d)", model->log.c_str(), result.value_or(-1));
        CHECK(!t.active() && tok.use_count() == 1, "it ends and its closure is destroyed (use_count %ld)", tok.use_count());
    }
    {   // cancelled while sleeping: the await says so, the body unwinds, the closure is destroyed once
        Tok tok = make_tok();
        bool saw_cancel = false, after_ran = false;
        Task t = mc.spawn([tok, &saw_cancel, &after_ran](Co& c) {
            if (!c.sleep(5)) saw_cancel = c.cancelled();
            else after_ran = true;
        });
        co.sleep(0.05);
        t.cancel();
        co.sleep(0.1);
        CHECK(saw_cancel && !after_ran, "a cancelled coroutine's await returned false (%d %d)", saw_cancel, after_ran);
        CHECK(tok.use_count() == 1, "and its closure is destroyed (use_count %ld)", tok.use_count());
    }
    {   // its entity is released while it sleeps
        auto m2 = app.new_entity<Model>();
        Tok tok = make_tok();
        bool saw_cancel = false;
        app.ctx(m2.id).spawn([tok, &saw_cancel](Co& c) { saw_cancel = !c.sleep(5); });
        co.sleep(0.05);
        app.release(m2.id);
        co.sleep(0.1);
        CHECK(saw_cancel && tok.use_count() == 1, "a coroutine of a released entity unwinds and is destroyed (%d, use_count %ld)", saw_cancel, tok.use_count());
    }
    {   // I/O: a pipe, read in a coroutine and watched with a callback
        int fds[2];
        CHECK(pipe(fds) == 0, "pipe");
        gp_fd_set_nonblocking(fds[0]);
        std::string got;
        Tok tok = make_tok();
        mc.spawn([tok, &got, fd = fds[0]](Co& c) {
            char buf[16];
            ssize_t n = c.read(fd, buf, sizeof buf);
            if (n > 0) got.assign(buf, (size_t)n);
        });
        co.sleep(0.05);
        CHECK(write(fds[1], "ping", 4) == 4, "write");
        co.sleep(0.15);
        CHECK(got == "ping" && tok.use_count() == 1, "Co::read waited for the data ('%s')", got.c_str());

        Tok tok2 = make_tok();
        int events = 0;
        char buf[16];
        Task w = mc.watch_fd(fds[0], GP_FD_READ, [tok2, &events, &buf](Model& self, FdEvent ev) {
            events |= ev.events;
            while (read(ev.fd, buf, sizeof buf) > 0) {}
            self.fired++;
        });
        CHECK(write(fds[1], "x", 1) == 1, "write");
        co.sleep(0.15);
        CHECK((events & GP_FD_READ) && model->fired >= 1, "watch_fd called back (%d)", events);
        w.cancel();
        co.sleep(0.1);
        CHECK(tok2.use_count() == 1, "a cancelled watch's closure is destroyed (use_count %ld)", tok2.use_count());
        close(fds[0]);
        close(fds[1]);
    }
    app.release(m.id);
}

// ------------------------------------------------------------------------------------------------ observation

struct Source { int value = 0; };

static void test_observation(Co& co, Ctx cx) {
    App app = cx.app();
    auto src = app.new_entity<Source>();
    auto obs = app.new_entity<Model>();
    Model* model = obs.get(app);
    Ctx oc = app.ctx(obs.id);
    Ctx sc = app.ctx(src.id);
    {   // observe: called on notify with the observed entity; destroyed when cancelled (not inside the call that cancels it)
        Tok tok = make_tok();
        gp_entity seen{};
        Subscription sub = oc.observe(src.id, [tok, &seen](Model& self, gp_entity observed) { self.ticks++; seen = observed; });
        CHECK(sub.active() && tok.use_count() == 2, "a subscription holds its closure");
        sc.notify();
        app.ctx().app();  // (a no-op: keeps the ctx path exercised)
        gp_flush_effects(app);
        CHECK(model->ticks == 1 && seen.index == src.id.index, "observe was called with the observed entity (%d)", model->ticks);
        sub.cancel();
        gp_flush_effects(app);
        CHECK(!sub.active() && tok.use_count() == 1, "cancelling destroys the closure (use_count %ld)", tok.use_count());
        sc.notify();
        gp_flush_effects(app);
        CHECK(model->ticks == 1, "and it is not called again");
    }
    {   // a closure that cancels its own subscription is still alive until it returns
        Tok tok = make_tok();
        long inside = 0;
        Subscription sub;
        sub = oc.observe(src.id, [tok, &inside, &sub](Model& self) {
            sub.cancel();
            inside = tok.use_count();   // reads the capture after the cancel: must not be destroyed yet
            self.ticks += 10;
        });
        sc.notify();
        gp_flush_effects(app);
        CHECK(model->ticks == 11 && inside == 2, "a closure cancelling itself runs to the end (%d, use_count %ld)", model->ticks, inside);
        CHECK(tok.use_count() == 1, "and is destroyed afterwards (use_count %ld)", tok.use_count());
    }
    {   // subscribe / emit with a payload; on_release with the state
        struct Ping { int a; float b; };
        Tok tok = make_tok();
        int a = 0;
        float b = 0;
        std::string name;
        oc.subscribe(src.id, "ping", [tok, &a, &b, &name](const gp_event& ev) {
            const Ping* p = static_cast<const Ping*>(ev.payload);
            a = p->a;
            b = p->b;
            name = ev.name;
        });
        sc.emit("ping", Ping{ 3, 2.5f });
        gp_flush_effects(app);
        CHECK(a == 3 && b == 2.5f && name == "ping", "subscribe got the payload (%d %g '%s')", a, b, name.c_str());
        sc.emit("other");
        gp_flush_effects(app);
        CHECK(a == 3, "other events are not delivered to a named subscription");

        Tok tok2 = make_tok();
        void* state_seen = nullptr;
        int value_seen = -1;
        auto victim = app.new_entity<Source>();
        victim.get(app)->value = 77;
        oc.on_release(victim.id, [tok2, &state_seen, &value_seen](gp_entity released, void* state) {
            state_seen = state;
            value_seen = static_cast<Source*>(state)->value;
        });
        app.release(victim.id);
        gp_flush_effects(app);
        CHECK(state_seen != nullptr && value_seen == 77, "on_release saw the state before it was destroyed (%d)", value_seen);
        CHECK(tok2.use_count() == 1, "and its closure is destroyed once the entity is gone (use_count %ld)", tok2.use_count());
        CHECK(tok.use_count() == 2, "the live subscription still holds its own");
    }
    {   // the observer is released: its closure is destroyed, nothing is called
        auto obs2 = app.new_entity<Model>();
        Tok tok = make_tok();
        bool called = false;
        app.ctx(obs2.id).observe(src.id, [tok, &called](Model&) { called = true; });
        app.release(obs2.id);
        gp_flush_effects(app);
        sc.notify();
        gp_flush_effects(app);
        CHECK(!called && tok.use_count() == 1, "a released observer is not called and its closure is destroyed (use_count %ld)", tok.use_count());
        // plain observe(target): the observer re-renders; here it only has to exist and be cancellable
        Subscription plain = oc.observe(src.id);
        CHECK(plain.active(), "observe(target) with no callable");
        plain.cancel();
    }
    app.release(obs.id);
    app.release(src.id);
}

// ------------------------------------------------------------------------------------------------ images, fonts, text fields

static void write_bmp(const char* path) {   // 2 x 2, 24 bits: red, green / blue, white
    unsigned char px[2][2][3] = { { { 0, 0, 255 }, { 0, 255, 0 } }, { { 255, 0, 0 }, { 255, 255, 255 } } };   // BGR, bottom row first
    unsigned char file[54 + 16] = { 'B', 'M' };
    auto put32 = [&](int at, unsigned v) { for (int i = 0; i < 4; i++) file[at + i] = (unsigned char)(v >> (8 * i)); };
    put32(2, sizeof file);
    put32(10, 54);
    put32(14, 40);
    put32(18, 2);
    put32(22, 2);
    file[26] = 1;
    file[28] = 24;
    put32(34, 16);
    for (int y = 0; y < 2; y++) {
        for (int x = 0; x < 2; x++) for (int c = 0; c < 3; c++) file[54 + y * 8 + x * 3 + c] = px[y][x][c];
    }
    FILE* f = std::fopen(path, "wb");
    std::fwrite(file, 1, sizeof file, f);
    std::fclose(f);
}

static void test_loaders(Co& co, Ctx cx) {
    App app = cx.app();
    std::string path = "/tmp/gpuic_cpp_async_test.bmp";
    write_bmp(path.c_str());
    gp_image im = app.load_image(path.c_str());
    int w = 0, h = 0;
    CHECK(im.generation != 0 && gp_image_size(app, im, &w, &h) && w == 2 && h == 2, "load_image decoded the file (%d x %d)", w, h);
    app.release(im);
    CHECK(app.load_image("/nonexistent/x.png").generation == 0, "a missing file is a null image");
    unsigned char rgba[4 * 4] = { 255, 0, 0, 255, 0, 255, 0, 255, 0, 0, 255, 255, 255, 255, 255, 255 };
    gp_image raw = app.image_from_rgba(2, 2, rgba);
    CHECK(raw.generation != 0, "image_from_rgba");
    app.release(raw);

    {   // asynchronously: with a result, and for a file that is not there
        Tok tok = make_tok();
        gp_image got{};
        int gw = 0;
        bool called = false;
        cx.load_image(path.c_str(), [tok, &got, &called, &gw, app](gp_image image) { called = true; got = image; gp_image_size(app, image, &gw, nullptr); });
        co.sleep(0.3);
        CHECK(called && got.generation != 0 && gw == 2 && tok.use_count() == 1, "load_image async delivered an image and was destroyed (%d, use_count %ld)", called, tok.use_count());
        app.release(got);
        Tok tok2 = make_tok();
        gp_image bad{ 12345, 1 };
        cx.load_image("/nonexistent/y.png", [tok2, &bad](gp_image image) { bad = image; });
        co.sleep(0.3);
        CHECK(bad.generation == 0 && tok2.use_count() == 1, "async failure delivers a null image");
    }
    std::remove(path.c_str());

    const char* svg = "<svg width='10' height='10'><rect width='10' height='10' fill='red'/></svg>";
    gp_svg s = app.load_svg(std::string_view(svg));
    float sw = 0, sh = 0;
    CHECK(s.generation != 0 && gp_svg_size(app, s, &sw, &sh) && sw == 10 && sh == 10, "load_svg from text");
    app.release(s);
    CHECK(!app.add_font("/nonexistent/font.ttf") && !app.add_font("not a font", 10), "fonts that are not fonts are refused");
}

static TextInput g_field;

struct Form {
    int changes = 0, submits = 0;
    std::string last;
};

static void test_text_input(Co& co, Ctx cx) {
    App app = cx.app();
    auto form = app.new_entity<Form>();
    Form* f = form.get(app);
    Tok tok = make_tok();
    std::string suffix = "!";
    TextInput in = app.text_input()
                       .owner(form.id)
                       .text("hello")
                       .placeholder("Name")
                       .max_bytes(40)
                       .on_change([tok, suffix](Form& self, Ctx c, gp_entity input) { self.changes++; self.last = gp_text_input_text(c.app(), input) + suffix; })
                       .on_submit([tok](Form& self) { self.submits++; })
                       .build();
    CHECK(in && in.text(app) == "hello", "the field starts with its text ('%s')", in.text(app).c_str());
    CHECK(tok.use_count() >= 2, "the field holds the callbacks");

    // show it (a field receives keys when a window has it) and type
    g_field = in;
    app.notify();
    co.sleep(0.1);
    gp_ctx c = gp_ctx_for(app, GP_ENTITY_NULL);
    gp_focus_set(&c, in.focus(app));
    co.sleep(0.05);
    gp_simulate_text(app, " there");
    co.sleep(0.05);
    CHECK(in.text(app) == "hello there" && f->changes >= 1 && f->last == "hello there!", "typing calls on_change ('%s', %d, '%s')", in.text(app).c_str(), f->changes, f->last.c_str());
    gp_simulate_keystrokes(app, "enter");
    co.sleep(0.05);
    CHECK(f->submits == 1, "enter calls on_submit (%d)", f->submits);
    int changes = f->changes;
    in.set_text(cx, "set by code");
    CHECK(in.text(app) == "set by code" && f->changes == changes, "set_text does not call on_change");
    in.select_all(cx);
    auto sel = in.selection(app);
    CHECK(sel.first == 0 && sel.second == 11, "select_all (%zu, %zu)", sel.first, sel.second);
    CHECK(in.preedit(app).empty() && in.rows(app) >= 1, "preedit and rows");

    app.release(in.id);
    gp_flush_effects(app);
    CHECK(tok.use_count() == 1, "releasing the field destroys its callbacks (use_count %ld)", tok.use_count());
    app.release(form.id);
}

// ------------------------------------------------------------------------------------------------ ctx copies, handles that are over

static void test_ctx_and_handles(Co& co, Ctx cx) {
    App app = cx.app();
    auto m = app.new_entity<Model>();
    Ctx a = app.ctx(m.id);
    Ctx b = a;
    CHECK(b.raw() != a.raw() && b.entity().index == m.id.index, "a copy of an owning Ctx owns its own gp_ctx");
    Ctx c = app.ctx();
    c = a;
    CHECK(c.raw() != a.raw() && c.entity().index == m.id.index && c.entity().generation == m.id.generation, "and so does an assigned one");
    Ctx d(a.raw());
    Ctx e = d;
    Ctx f = app.ctx();
    f = d;
    CHECK(e.raw() == a.raw() && f.raw() == a.raw(), "a Ctx that wraps a pointer keeps pointing at it when copied");
    {   // the copy is good after the original is gone
        Ctx* heap = new Ctx(app.ctx(m.id));
        Ctx kept = *heap;
        delete heap;
        Tok tok = make_tok();
        kept.after(0.02, [tok](Model& self) { self.fired++; });
        co.sleep(0.1);
        CHECK(m.get(app)->fired == 1, "a copied Ctx works once the original is destroyed");
    }
    {   // a drop function attached to something already over runs at once, exactly once
        int n = 0;
        auto count = [](void* p) { ++*static_cast<int*>(p); };
        Task t = cx.after(0.01, [] {});
        co.sleep(0.15);
        gp_task_on_drop(app, t.raw(), count, &n);
        CHECK(n == 1, "gp_task_on_drop on a finished task calls it at once (%d)", n);
        n = 0;
        gp_task_on_drop(app, gp_task{ 0 }, count, &n);
        CHECK(n == 1, "and on a zero handle (%d)", n);
        auto src = app.new_entity<Source>();
        Subscription sub = app.ctx(m.id).observe(src.id, [](Model&) {});
        sub.cancel();
        gp_flush_effects(app);
        n = 0;
        gp_subscription_on_drop(app, sub.raw(), count, &n);
        CHECK(n == 1, "gp_subscription_on_drop on an unsubscribed one calls it at once (%d)", n);
        app.release(src.id);
    }
    {   // Co::run is false when the coroutine was cancelled while it waited
        std::optional<bool> ran;
        Task t = cx.spawn([&ran](Co& co2) {
            ran = co2.run([] { usleep(100000); });
        });
        co.sleep(0.03);
        t.cancel();
        co.sleep(0.3);
        CHECK(ran.has_value() && !*ran, "Co::run(void work) is false for a cancelled coroutine (%d)", ran.has_value() ? (int)*ran : -1);
        std::optional<std::optional<int>> got;
        Task t2 = cx.spawn([&got](Co& co2) { got = co2.run([] { usleep(100000); return 3; }); });
        co.sleep(0.03);
        t2.cancel();
        co.sleep(0.3);
        CHECK(got.has_value() && !got->has_value(), "and Co::run(work) is empty");
    }
    app.release(m.id);
}

// ------------------------------------------------------------------------------------------------ windows

static void test_window(Co& co, Ctx cx) {
    App app = cx.app();
    auto view = app.new_view_with<int>([](int&, Ctx) { return div().size_full(); });
    Window w = app.open_window("second", 120, 80, view);
    CHECK(w && w.raw() != nullptr && !Window{}, "open_window gives a Window");
    Tok tok = make_tok();
    int ticks = 0;
    Task t = w.ctx().every(0.02, [tok, &ticks] { ticks++; });
    Tok tok2 = make_tok();
    int app_ticks = 0;
    Task t2 = cx.every(0.02, [tok2, &app_ticks] { app_ticks++; });
    co.sleep(0.15);
    CHECK(ticks >= 2 && t.active(), "a timer started with the window's ctx runs (%d)", ticks);
    w.close();
    co.sleep(0.3);
    CHECK(!t.active() && tok.use_count() == 1, "it ends with the window and its closure is destroyed (use_count %ld)", tok.use_count());
    int after_close = ticks;
    co.sleep(0.1);
    CHECK(ticks == after_close, "and does not tick again (%d -> %d)", after_close, ticks);
    CHECK(t2.active() && app_ticks > 5, "a timer of the app's ctx goes on (%d)", app_ticks);
    t2.cancel();
}

// ------------------------------------------------------------------------------------------------ shutdown

static Tok g_end_tok;
static Tok g_end_tok2;
static Tok g_end_tok3;
static Tok g_end_tok4;
static Tok g_end_tok5;

static void driver(Co& co, Ctx cx) {
    test_timers(co, cx);
    test_background(co, cx);
    test_coroutines(co, cx);
    test_observation(co, cx);
    test_loaders(co, cx);
    test_text_input(co, cx);
    test_window(co, cx);
    test_ctx_and_handles(co, cx);

    // leave closures of every kind alive and quit: the app's shutdown destroys them all
    App app = cx.app();
    static auto keeper = app.new_entity<Source>();
    g_end_tok = make_tok();
    g_end_tok2 = make_tok();
    g_end_tok3 = make_tok();
    g_end_tok4 = make_tok();
    g_end_tok5 = make_tok();
    cx.every(10, [t = g_end_tok] {});
    cx.after(100, [t = g_end_tok] {});
    cx.observe(keeper.id, [t = g_end_tok2](Source&) {});
    cx.subscribe(keeper.id, "x", [t = g_end_tok3](const gp_event&) {});
    cx.spawn([t = g_end_tok4](Co& c) { c.sleep(100); });
    // a coroutine that starts a timer while it unwinds at shutdown: nothing cancels that one, the app's teardown must still free it
    cx.spawn([t = g_end_tok5](Co& c, Ctx k) { if (!c.sleep(100)) k.after(100, [t] {}); });
    auto longer = app.text_input().on_change([t = g_end_tok4] {}).build();
    (void)longer;
    co.sleep(0.05);
    app.quit();
}

int main() {
    setenv("GPUIC_IGNORE_REAL_MOUSE", "1", 1);
    main_id = std::this_thread::get_id();
    int rc = run([](App app) {
        auto root = app.new_view_with<int>([](int&, Ctx) { return div().size_full().child(g_field ? g_field.element() : div()); });
        app.open_window("cpp_async_test", 300, 160, root);
        app.ctx().spawn([](Co& co, Ctx cx) { driver(co, cx); });
    });
    CHECK(rc == 0, "run returned %d", rc);
    CHECK(g_end_tok.use_count() == 1 && g_end_tok2.use_count() == 1 && g_end_tok3.use_count() == 1 && g_end_tok4.use_count() == 1 && g_end_tok5.use_count() == 1,
          "shutdown destroyed every closure still alive (%ld %ld %ld %ld %ld)", g_end_tok.use_count(), g_end_tok2.use_count(), g_end_tok3.use_count(),
          g_end_tok4.use_count(), g_end_tok5.use_count());
    std::printf(failures ? "cpp_async_test: %d FAILURE(S)\n" : "cpp_async_test: all checks passed\n", failures);
    return failures != 0;
}
