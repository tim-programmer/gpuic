/*
 * The order gp_application_run tears things down in, which a program may rely on: the windows and the renderer are released, then every entity's drop function runs while SDL is still initialised
 * (an entity may close an SDL audio device or any other SDL object in its destructor), and only then SDL_Quit. The audio device here is SDL's dummy driver. Needs a display.
 */
#include "../src/internal.h"

#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;
#define CHECK(cond, ...)                                                               \
    do {                                                                               \
        if (!(cond)) {                                                                 \
            if (failures++ < 20) {                                                     \
                printf("FAIL %s:%d: ", __FILE__, __LINE__);                            \
                printf(__VA_ARGS__);                                                   \
                printf("\n");                                                          \
            }                                                                          \
        }                                                                              \
    } while (0)

typedef struct { SDL_AudioDeviceID dev; } Audio;
static int drops, video_at_drop, audio_at_drop, closed_ok;
static gp_app *app;

static void audio_drop(void *self) {
    Audio *a = self;
    drops++;
    video_at_drop = SDL_WasInit(SDL_INIT_VIDEO) != 0;
    audio_at_drop = SDL_WasInit(SDL_INIT_AUDIO) != 0;
    if (a->dev) {
        SDL_CloseAudioDevice(a->dev);   /* must still work: SDL has not been shut down */
        closed_ok = 1;
    }
}

static gp_node *render(void *self, gp_ctx *cx) { return gp_div(gp_size_full()); }

static void driver(gp_co *co, void *self, gp_ctx *cx, void *data) {
    gp_sleep(co, 0.1);
    gp_quit(app);
}

static void ready(gp_app *a, void *userdata) {
    app = a;
    CHECK(SDL_InitSubSystem(SDL_INIT_AUDIO), "the dummy audio driver starts: %s", SDL_GetError());
    SDL_AudioSpec spec = { SDL_AUDIO_F32, 2, 48000 };
    Audio au = { SDL_OpenAudioDevice(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec) };
    CHECK(au.dev != 0, "an audio device opens: %s", SDL_GetError());
    gp_entity_new(app, sizeof au, &au, audio_drop);
    gp_entity v = gp_view_new(app, 1, &(char){ 0 }, render, NULL);
    gp_open_window(app, &(gp_window_options){ .title = "shutdown_test", .width = 100, .height = 100 }, v);
    gp_ctx cx = gp_ctx_for(app, GP_ENTITY_NULL);
    gp_spawn(&cx, driver, NULL);
}

int main(void) {
    setenv("SDL_AUDIODRIVER", "dummy", 1);
    int rc = gp_application_run(ready, NULL);
    CHECK(rc == 0, "application_run returned %d", rc);
    CHECK(drops == 1, "the entity's drop function ran once (%d)", drops);
    CHECK(video_at_drop && audio_at_drop, "SDL was still initialised when it ran (video %d, audio %d)", video_at_drop, audio_at_drop);
    CHECK(closed_ok, "and the audio device could be closed there");
    CHECK(SDL_WasInit(0) == 0, "and SDL has been shut down after the run (%u)", (unsigned)SDL_WasInit(0));
    printf(failures ? "shutdown_test: %d FAILURE(S)\n" : "shutdown_test: all checks passed\n", failures);
    return failures != 0;
}
