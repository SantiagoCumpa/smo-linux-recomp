/* Host display: a real window (SDL3: Wayland or X11), 60 Hz pacing and the guest's vsync event.
 * The window's contents are drawn by the graphics layer's render thread (nvn_gl.c's front end, Vulkan).
 * SDL wants its video calls and its event loop on the program's main thread: display_init makes the window there and
 * boot.c's main thread then runs display_wait_thread, which pumps the window's events while the game runs. Other threads
 * ask for window changes (title, fullscreen, minimise) through requests the event loop applies. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "plat.h"
#include "runtime.h"
#include "hostrt.h"
#include "gfx_options.h"
#include "vk_codes.h"
#include <SDL3/SDL.h>

#define A0 (c->x[0])
#define A1 (c->x[1])
#define RET(v) (c->x[0] = (uint64_t)(v))
#define WIDTH 1280
#define HEIGHT 720
#define FRAME_NS 16666667LL

void os_init_event(uint64_t key, int autoclear);      /* os_threads.c */
void os_signal_event(uint64_t key);

static SDL_Window *g_win;
static volatile int32_t g_ready;
static volatile int64_t g_frames;
void nvn_gl_start(void *window);                      /* nvn_gl.c: the SDL_Window */
void nvn_gl_report(FILE *out);
void nvn_tl(int ev, int arg);                          /* nvn_gl.c: SMO_FRAMETL timeline */
static uint64_t g_vsync_event;
static plat_counter g_freq;

/* ONE refresh clock. The game counts the signals of the vsync event on a thread of its own ("VSync Count
 * Thread", 0x7103810a40: window object + 0x138) and, at every present (0x710386fc30), advances its frame counter
 * (0x710593a440) by the number of refreshes counted since the previous present. So the refresh count IS the game's
 * time. Before this, the event came from a plat_sleep_ms() loop and presentation was held by a second, independent 60 Hz
 * clock that restarted its phase after every late frame: between two presents the game counted 0, 1 or 2 refreshes,
 * and "0" counts as one step while "2" counts as two, so the game's clock gained (10.77 s per 600 frames; measured by
 * ssbu-recomp in Super Smash Bros. Ultimate, whose main the addresses above are in).
 * Now: refresh n happens at t0 + n/60 s; at each one the vsync event is signalled and then the frames waiting to be
 * shown are released. A frame queued between refresh n and n+1 is shown at n+1, and no sooner than `interval`
 * refreshes after the frame before it (nvnWindowSetPresentInterval); the game is held in its wait for the window
 * texture (nvnSyncWait on the sync of nvnWindowAcquireTexture), which is AFTER it has read the refresh count, as on
 * the console. SMO_OLD_PACING=1: the two clocks as before. SMO_CLOCKSTAT=1: one line every 600 presented frames. */
static int g_old_pacing = -1, g_clockstat;
static volatile int64_t g_vsync_n;            /* refreshes passed (published after the event was signalled) */
static volatile int64_t g_vsync_signals, g_vsync_late;
static plat_lock_t g_vsync_lock = PLAT_LOCK_INIT;
static plat_cond_t g_vsync_cv = PLAT_COND_INIT;
static int64_t g_latch;                       /* refresh at which the last queued frame is shown */
static volatile int32_t g_latch_pending;        /* a frame was queued and nobody has waited for its refresh yet */
static int old_pacing(void) {
    if (g_old_pacing < 0) { const char *e = getenv("SMO_OLD_PACING"); g_old_pacing = e && atoi(e) != 0; g_clockstat = getenv("SMO_CLOCKSTAT") && atoi(getenv("SMO_CLOCKSTAT")) != 0; }
    return g_old_pacing;
}
static void vsync_test_key(void);

/* while the amiibo menu (smo_amiibo.c) holds the game, the game's vsync event is not signalled: its VSync Count
 * Thread would count every refresh of the wait and the game would step that many frames at once when it goes on. The menu
 * paces itself on the same refresh clock (display_menu_wait). */
static volatile int32_t g_menu_hold;
void display_menu_hold(int on) { plat_xchg(&g_menu_hold, on); }
int display_menu_held(void) { return g_menu_hold != 0; }     /* nvn_vk_watchdog.c: a game held on purpose (menu, guide) is not frozen */
static int vsync_signal_ok(void) { return !g_menu_hold; }

/* TEST ONLY, SMO_SPEED=<n>: the refresh clock (vsync_thread) runs n times faster, 60 x n refreshes a second (any number
 * > 0; 0 = 960 a second, as fast as the PC can run the game). The game waits for a refresh before each frame and steps
 * once per frame, so it runs up to n times faster where the PC keeps up; input sequences count frames, so they play the
 * same. Not set or 1 = nothing changes. (A lockstep clock, a refresh only once the game has queued a frame, never starts
 * this game: it waits for a refresh before its first frame.) */
static double g_rate = 60.0;                          /* refreshes a second */
static void speed_init(void) {
    const char *e = getenv("SMO_SPEED");
    if (!e || !e[0]) return;
    double speed = atof(e);
    g_rate = speed > 0 ? 60.0 * speed : 960.0;
    if (g_rate != 60.0) printf("display: SMO_SPEED=%s (TEST ONLY): the refresh clock runs at %.0f Hz\n", e, g_rate);
}

int oe_exit_request(void);                            /* nnmisc.c */
static volatile int32_t g_closing;
int display_closing(void) { return g_closing != 0; }  /* smo_amiibo.c: the menu gives way to the game when the window is being closed */

/* The window's mode (config/graphics.ini, gfx_options.c): windowed, borderless fullscreen, fullscreen (on top), or the
 * default, a borderless window of the configured size. F11, or a double-click, switches between covering the monitor
 * (SDL's desktop fullscreen) and the window. On the main thread (the event loop). */
static int g_win_full;                                /* the window covers its monitor now */
static volatile int32_t g_req_full_toggle, g_req_title, g_req_minimise;   /* requests of other threads, applied by the event loop */
static volatile int32_t g_focus_flags;                 /* DISPLAY_FOCUSED 1, DISPLAY_MINIMISED 2, DISPLAY_SHOWN 4 */
static volatile int32_t g_keep_awake;                  /* sources that hold the screen saver off (bit mask) */
static char g_title_req[192];
static plat_lock_t g_title_lock = PLAT_LOCK_INIT;
static volatile int32_t g_win_w = WIDTH, g_win_h = HEIGHT; /* the window's size in pixels */
static void window_fullscreen(int on) {
    if (!g_win || on == g_win_full) return;
    SDL_SetWindowFullscreenMode(g_win, NULL);           /* borderless desktop fullscreen */
    if (!SDL_SetWindowFullscreen(g_win, on)) { printf("display: fullscreen %s refused: %s\n", on ? "on" : "off", SDL_GetError()); return; }
    g_win_full = on;
    printf("display: the window %s (F11)\n", on ? "covers its monitor" : "is a window again");
    fflush(stdout);
}

void *display_hwnd(void) { return g_win; }
void display_toggle_fullscreen(void) { plat_xchg(&g_req_full_toggle, 1); }         /* any thread: applied by the event loop */
void display_set_title(const char *t) {                                             /* any thread */
    plat_lock(&g_title_lock);
    snprintf(g_title_req, sizeof g_title_req, "%s", t);
    plat_unlock(&g_title_lock);
    plat_xchg(&g_req_title, 1);
}
int display_focus_flags(void) { return g_focus_flags; }
void display_test_minimise(int on) { plat_xchg(&g_req_minimise, on ? 1 : 2); }
void display_keep_awake(int source, int on) { if (on) plat_or(&g_keep_awake, 1 << source); else plat_and(&g_keep_awake, ~(1 << source)); }
void display_size(int *w, int *h) { *w = g_win_w; *h = g_win_h; }

/* ---- keyboard: Windows virtual-key numbers (the configuration's codes) <-> SDL scancodes */
int vk_to_scancode(int vk) {
    if (vk >= 'A' && vk <= 'Z') return SDL_SCANCODE_A + (vk - 'A');
    if (vk >= '1' && vk <= '9') return SDL_SCANCODE_1 + (vk - '1');
    if (vk == '0') return SDL_SCANCODE_0;
    if (vk >= VK_F1 && vk < VK_F1 + 12) return SDL_SCANCODE_F1 + (vk - VK_F1);
    if (vk >= VK_NUMPAD0 + 1 && vk <= VK_NUMPAD0 + 9) return SDL_SCANCODE_KP_1 + (vk - VK_NUMPAD0 - 1);
    switch (vk) {
    case VK_NUMPAD0: return SDL_SCANCODE_KP_0;
    case VK_BACK: return SDL_SCANCODE_BACKSPACE;
    case VK_TAB: return SDL_SCANCODE_TAB;
    case VK_RETURN: return SDL_SCANCODE_RETURN;
    case VK_SHIFT: case VK_LSHIFT: return SDL_SCANCODE_LSHIFT;
    case VK_RSHIFT: return SDL_SCANCODE_RSHIFT;
    case VK_CONTROL: case VK_LCONTROL: return SDL_SCANCODE_LCTRL;
    case VK_RCONTROL: return SDL_SCANCODE_RCTRL;
    case VK_MENU: case VK_LMENU: return SDL_SCANCODE_LALT;
    case VK_RMENU: return SDL_SCANCODE_RALT;
    case VK_ESCAPE: return SDL_SCANCODE_ESCAPE;
    case VK_SPACE: return SDL_SCANCODE_SPACE;
    case VK_LEFT: return SDL_SCANCODE_LEFT;
    case VK_UP: return SDL_SCANCODE_UP;
    case VK_RIGHT: return SDL_SCANCODE_RIGHT;
    case VK_DOWN: return SDL_SCANCODE_DOWN;
    case 0xBC: return SDL_SCANCODE_COMMA;
    case 0xBE: return SDL_SCANCODE_PERIOD;
    case 0xBA: return SDL_SCANCODE_SEMICOLON;
    case 0xBF: return SDL_SCANCODE_SLASH;
    case 0xDB: return SDL_SCANCODE_LEFTBRACKET;
    case 0xDD: return SDL_SCANCODE_RIGHTBRACKET;
    }
    return 0;
}
int vk_from_scancode(int sc) {
    static int16_t table[SDL_SCANCODE_COUNT];
    static int ready;
    if (!ready) { for (int vk = 1; vk < 256; vk++) { int s = vk_to_scancode(vk); if (s > 0 && s < SDL_SCANCODE_COUNT && !table[s]) table[s] = (int16_t)vk; } ready = 1; }
    return sc > 0 && sc < SDL_SCANCODE_COUNT ? table[sc] : 0;
}
static volatile uint8_t g_synth_key[256];               /* test keys (SMO_AUTOKEY) */
int display_key_down(int vk) {
    if (vk <= 0 || vk > 255) return 0;
    if (g_synth_key[vk]) return 1;
    if (!(g_focus_flags & 1)) return 0;                 /* only while the game's window has the keyboard focus */
    int sc = vk_to_scancode(vk), n = 0;
    const bool *state = SDL_GetKeyboardState(&n);
    if (!state || sc <= 0 || sc >= n) return 0;
    if (vk == VK_SHIFT) return state[SDL_SCANCODE_LSHIFT] || state[SDL_SCANCODE_RSHIFT];
    if (vk == VK_CONTROL) return state[SDL_SCANCODE_LCTRL] || state[SDL_SCANCODE_RCTRL];
    if (vk == VK_MENU) return state[SDL_SCANCODE_LALT] || state[SDL_SCANCODE_RALT];
    return state[sc];
}

static char g_title[96];
static void window_event(const SDL_Event *e) {
    switch (e->type) {
    case SDL_EVENT_QUIT:
    case SDL_EVENT_WINDOW_CLOSE_REQUESTED:          /* the exit request: nn::oe decides (nnmisc.c oe_exit_request) whether the game ends now or after its exit-request handling section */
        if (plat_xchg(&g_closing, 1)) break;
        printf("\nwindow closed by the user after %lld frames\n", (long long)g_frames);
        if (!oe_exit_request()) report_and_exit(0);
        break;
    case SDL_EVENT_WINDOW_FOCUS_GAINED: plat_or(&g_focus_flags, 1); break;
    case SDL_EVENT_WINDOW_FOCUS_LOST: plat_and(&g_focus_flags, ~1); break;
    case SDL_EVENT_WINDOW_MINIMIZED: plat_or(&g_focus_flags, 2); break;
    case SDL_EVENT_WINDOW_RESTORED: case SDL_EVENT_WINDOW_MAXIMIZED: plat_and(&g_focus_flags, ~2); break;
    case SDL_EVENT_WINDOW_SHOWN: plat_or(&g_focus_flags, 4); break;
    case SDL_EVENT_WINDOW_HIDDEN: plat_and(&g_focus_flags, ~4); break;
    case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED: plat_xchg(&g_win_w, e->window.data1); plat_xchg(&g_win_h, e->window.data2); break;
    case SDL_EVENT_WINDOW_ENTER_FULLSCREEN: g_win_full = 1; break;
    case SDL_EVENT_WINDOW_LEAVE_FULLSCREEN: g_win_full = 0; break;
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
        if (e->button.button == SDL_BUTTON_LEFT && e->button.clicks == 2) window_fullscreen(!g_win_full);
        break;
    case SDL_EVENT_KEY_DOWN:
        if (e->key.repeat) break;
        if (e->key.scancode == SDL_SCANCODE_F11) { window_fullscreen(!g_win_full); break; }
        if (e->key.scancode == SDL_SCANCODE_F12) { void smo_capture_request(void); smo_capture_request(); break; }   /* F12 = the console's Capture button */
        {   /* the amiibo menu's key (controllers.ini [hotkeys], F1 by default) */
            int nnpad_hotkey_vk(int which); void amiibo_menu_request(void);
            int vk = nnpad_hotkey_vk(0);
            if (vk && vk_to_scancode(vk) == (int)e->key.scancode) amiibo_menu_request();
        }
        break;
    }
}
/* One round of the event loop (main thread): window events, then the requests of other threads. */
static void display_pump(uint32_t wait_ms) {
    SDL_Event e;
    if (SDL_WaitEventTimeout(&e, (int32_t)wait_ms)) {
        window_event(&e);
        while (SDL_PollEvent(&e)) window_event(&e);
    }
    if (plat_xchg(&g_req_full_toggle, 0)) window_fullscreen(!g_win_full);
    if (plat_xchg(&g_req_title, 0)) { char t[192]; plat_lock(&g_title_lock); snprintf(t, sizeof t, "%s", g_title_req); plat_unlock(&g_title_lock); SDL_SetWindowTitle(g_win, t); }
    {   int32_t m = plat_xchg(&g_req_minimise, 0);
        if (m == 1) SDL_MinimizeWindow(g_win); else if (m == 2) SDL_RestoreWindow(g_win); }
    {   /* The screen saver is held off while a source asks (the game's ending) and, with screen_saver =
         * off_while_playing, while the window is shown, active and not minimised: a pad is not activity the desktop
         * sees. On Wayland SDL holds an idle inhibitor on the window's surface (D-Bus ScreenSaver as the fallback). */
        static int awake;
        int32_t ff = g_focus_flags;
        int playing = gfx_options()->keep_awake_playing && (ff & 1) && (ff & 4) && !(ff & 2);
        int want = g_keep_awake != 0 || playing;
        if (want != awake) {
            if (want) SDL_DisableScreenSaver(); else SDL_EnableScreenSaver();
            awake = want;
            printf("display: screen saver %s\n", want ? (playing ? "held off (playing)" : "held off (the game asks)") : "allowed again");
            fflush(stdout);
        } }
}
/* boot.c, main thread: runs the event loop until the guest's main thread t ends (1) or ms milliseconds pass (0). */
int display_wait_thread(plat_thread *t, uint32_t ms) {
    uint64_t end = ms == PLAT_INFINITE ? UINT64_MAX : plat_ms() + ms;
    for (;;) {
        if (plat_thread_join(t, 0)) return 1;
        uint64_t now = plat_ms();
        if (now >= end) return 0;
        display_pump(end - now < 10 ? (uint32_t)(end - now) : 10);
    }
}

static void window_create(void) {
    /* window mode and size from config/graphics.ini (gfx_options.c). borderless window (the default): frameless, the
     * configured size; borderless fullscreen / fullscreen: covering the monitor (fullscreen: also on top). F11 switches. */
    const GfxOptions *go = gfx_options();
    int cw = go->width, ch = go->height;
    SDL_WindowFlags flags = SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY;
    if (go->window_mode == 3) flags |= SDL_WINDOW_BORDERLESS;
    if (go->window_mode == 2) flags |= SDL_WINDOW_ALWAYS_ON_TOP;
    if (go->window_mode == 1 || go->window_mode == 2) flags |= SDL_WINDOW_FULLSCREEN;
    printf("display: window %s, client %dx%d (graphics.ini; F11 switches between covering the monitor and the window)\n",
           go->window_mode == 2 ? "fullscreen (on top)" : go->window_mode == 1 ? "borderless fullscreen" : go->window_mode == 3 ? "borderless window" : "windowed", cw, ch);
    /* SMO_SHOW=0 keeps a test copy's window hidden (a hidden Wayland window gets no frame callbacks: presents use the
     * mailbox mode then, see nvn_vk.c); by default every copy shows its window. SMO_TITLE names the window. */
    const char *show = getenv("SMO_SHOW");
    if (show && atoi(show) == 0) flags |= SDL_WINDOW_HIDDEN;
    snprintf(g_title, sizeof g_title, "%s", getenv("SMO_TITLE") ? getenv("SMO_TITLE") : "Super Mario Odyssey (smo-recomp)");
    g_win = SDL_CreateWindow(g_title, cw, ch, flags);
    if (!g_win) { fprintf(stderr, "display: SDL_CreateWindow failed: %s\n", SDL_GetError()); plat_exit(1); }
    g_win_full = (flags & SDL_WINDOW_FULLSCREEN) != 0;
    int pw = cw, ph = ch;
    SDL_GetWindowSizeInPixels(g_win, &pw, &ph);
    g_win_w = pw; g_win_h = ph;
    plat_xchg(&g_focus_flags, (flags & SDL_WINDOW_HIDDEN ? 0 : 4) | (SDL_GetWindowFlags(g_win) & SDL_WINDOW_INPUT_FOCUS ? 1 : 0));
    SDL_SetWindowMinimumSize(g_win, 320, 180);
    printf("display: SDL %d.%d.%d, video driver %s\n", SDL_GetVersion() / 1000000, SDL_GetVersion() / 1000 % 1000, SDL_GetVersion() % 1000, SDL_GetCurrentVideoDriver());
    plat_xchg(&g_ready, 1);
}

/* Signals the guest's vsync event 60 times per second. */
static uint32_t vsync_thread(void * arg) {
    (void)arg;
    plat_counter next;
    plat_qpc(&next);
    if (!old_pacing()) {
        plat_counter t0 = next, now;
        int64_t n = 0;
        plat_set_thread_name("smo-vsync");
        for (;;) {
            int64_t target = t0.QuadPart + (int64_t)((double)(n + 1) * (double)g_freq.QuadPart / g_rate);      /* no accumulated rounding */
            plat_qpc(&now);
            int64_t left = target - now.QuadPart;
            if (left > 0) {                               /* clock_nanosleep to 0.5 ms before the refresh, then a spin */
                if (left > g_freq.QuadPart / 2000) plat_sleep_until(target - g_freq.QuadPart / 2000);
                for (plat_qpc(&now); now.QuadPart < target; plat_qpc(&now)) plat_pause();
            }
            int64_t passed = (int64_t)((double)(now.QuadPart - t0.QuadPart) * g_rate / (double)g_freq.QuadPart);   /* >= n + 1; more when this thread was kept off the CPU */
            if (passed < n + 1) passed = n + 1;
            if (passed > n + 1) plat_xadd(&g_vsync_late, passed - (n + 1));
            n = passed;
            if (g_vsync_event && vsync_signal_ok()) { os_signal_event(g_vsync_event); plat_inc(&g_vsync_signals); }
            plat_lock(&g_vsync_lock);
            plat_xchg(&g_vsync_n, n);
            plat_unlock(&g_vsync_lock);
            plat_cond_broadcast(&g_vsync_cv);
            nvn_tl(8 /* TL_VSYNC */, (int)(n & 0xffff));
            vsync_test_key();
        }
    }
    for (;;) {
        next.QuadPart += g_freq.QuadPart / 60;
        plat_counter now;
        plat_qpc(&now);
        int64_t ms = (next.QuadPart - now.QuadPart) * 1000 / g_freq.QuadPart;
        if (ms > 0) plat_sleep_ms((uint32_t)ms); else next = now;
        if (g_vsync_event && vsync_signal_ok()) { os_signal_event(g_vsync_event); plat_inc(&g_vsync_signals); }
        vsync_test_key();
    }
}
static void vsync_test_key(void) {
    {
        /* Test aid: SMO_AUTOKEY=<letter>,<first second>,<every n seconds> presses a key now and then, so a run without a person can get
         * past "press any button". The key is a synthetic one the game's keyboard reading sees (display_key_down): it never
         * reaches another program. */
        static int key = -1, first, every, tick, down;
        if (key == -1) {
            const char *e = getenv("SMO_AUTOKEY");
            key = 0;
            if (e && e[0]) { key = e[0] >= 'a' && e[0] <= 'z' ? e[0] - 32 : e[0]; first = 30; every = 5; sscanf(e + 1, ",%d,%d", &first, &every); if (every < 1) every = 1; }
        }
        tick++;
        if (key && down && tick - down >= 8) { g_synth_key[key & 255] = 0; down = 0; }
        else if (key && !down && tick >= first * 60 && (tick - first * 60) % (every * 60) == 0) { g_synth_key[key & 255] = 1; down = tick; printf("display: test key %c pressed at frame tick %d%c", key, tick, 10); }
    }
}

void hitch_init(void); void hitch_pacing(int on);      /* hitch.c: SMO_HITCH */
void display_init(void) {
    plat_qpf(&g_freq);
    /* the console's waits with a time-out (nn::os::TimedWaitEvent, ...: os_threads.c) end at the time asked for: Linux
     * timers are precise to tens of microseconds (Windows needed timeBeginPeriod(1) here). The window and SDL's event
     * loop live on this thread, the main thread (SDL's rule); SMO_SDL_VIDEO=x11 picks XWayland instead of Wayland. */
    SDL_SetHint(SDL_HINT_NO_SIGNAL_HANDLERS, "1");            /* the crash report owns the signals (hostrt.c) */
    SDL_SetHint(SDL_HINT_APP_NAME, "Super Mario Odyssey (smo-recomp)");
    SDL_SetHint(SDL_HINT_APP_ID, "smo-recomp");
    SDL_SetHint(SDL_HINT_VIDEO_ALLOW_SCREENSAVER, "1");
    /* Wayland first, X11 if there is no Wayland session. SDL 3 picks X11 (Xwayland) on its own when the compositor lacks
     * fifo-v1 / commit-timing-v1 (niri), for its FIFO pacing; this runtime presents in mailbox mode on its own 60 Hz
     * clock and does not need them. SMO_SDL_VIDEO=<driver> or SDL_VIDEO_DRIVER chooses instead. */
    if (getenv("SMO_SDL_VIDEO")) SDL_SetHint(SDL_HINT_VIDEO_DRIVER, getenv("SMO_SDL_VIDEO"));
    else if (!getenv("SDL_VIDEO_DRIVER")) SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "wayland,x11");
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS | SDL_INIT_JOYSTICK | SDL_INIT_GAMEPAD)) { fprintf(stderr, "display: SDL_Init failed: %s\n", SDL_GetError()); plat_exit(1); }
    hitch_init();
    window_create();
    nvn_gl_start(g_win);
    speed_init();
    plat_thread_detach(plat_thread_start(vsync_thread, NULL, 0));
    { void nvn_vk_selftest_hook(void); nvn_vk_selftest_hook(); }      /* nvn_vk.c: SMO_RENDERER=vk SMO_VK_SELFTEST=<frames>, then exit */
}

/* Called after every presented frame (nvn_gl.c has already put it in the window): holds the caller to 60 Hz. */
static volatile int32_t g_interval = 1;         /* nvnWindowSetPresentInterval: display refreshes per presented frame (0 = do not wait) */
void display_set_interval(int n) { plat_xchg(&g_interval, n < 0 ? 0 : n > 5 ? 5 : n); }
void nvn_pf_pace(int64_t ticks);            /* nvn_gl.c: timing scopes */
/* SMO_CLOCKSTAT=1: per 600 presented frames, the real time, the vsync signals sent, the refreshes the game counted
 * (its frame counter at 0x710593a440: 600 = one step per presented frame) and the refreshes this thread slept through. */
static int64_t g_cs_steps, g_cs_hist[4];      /* game steps (1 + refreshes skipped) and the histogram of "refreshes counted since the last present" 0, 1, 2, 3+ */
/* nvn_gl.c, at nvnWindowAcquireTexture: the game's window object is the index pointer - 0x30; + 0x138 = refreshes its
 * VSync Count Thread has counted, + 0x140 = the count at its previous present. Right after this call the game computes
 * steps = max(count - last, 1) (0x710386fc30) and then stores last = count. */
void display_clock_sample(uint64_t index_ptr) {
    if (!g_clockstat || !index_ptr) return;
    uint64_t count = rd64(index_ptr - 0x30 + 0x138), last = rd64(index_ptr - 0x30 + 0x140);
    int64_t d = (int64_t)(count - last);
    g_cs_steps += d > 1 ? d : 1;
    g_cs_hist[d <= 0 ? 0 : d >= 3 ? 3 : d]++;
}
static void clock_stat(int64_t n, plat_counter now) {
    static plat_counter t_last; static int64_t sig_last, late_last, steps_last, h_last[4];
    if (!g_clockstat || n % 600) return;
    int64_t sig = g_vsync_signals, late = g_vsync_late;
    if (t_last.QuadPart) printf("clock: frames %lld-%lld real %.3f s, vsync signals %lld (%.2f Hz), game steps %lld = %.2f s of game time, refreshes counted between presents 0:%lld 1:%lld 2:%lld 3+:%lld, slept through by the vsync thread %lld%c", (long long)(n - 600), (long long)n,
        (double)(now.QuadPart - t_last.QuadPart) / (double)g_freq.QuadPart, (long long)(sig - sig_last), (double)(sig - sig_last) * (double)g_freq.QuadPart / (double)(now.QuadPart - t_last.QuadPart), (long long)(g_cs_steps - steps_last), (double)(g_cs_steps - steps_last) / 60.0,
        (long long)(g_cs_hist[0] - h_last[0]), (long long)(g_cs_hist[1] - h_last[1]), (long long)(g_cs_hist[2] - h_last[2]), (long long)(g_cs_hist[3] - h_last[3]), (long long)(late - late_last), 10);
    t_last = now; sig_last = sig; late_last = late; steps_last = g_cs_steps; memcpy(h_last, g_cs_hist, sizeof h_last);
}
/* New pacing, part 1 (nvnQueuePresentTexture, after the frame is in the window): decides the refresh that shows it. */
static void frame_queued(void) {
    static plat_counter second_start;
    static int64_t second_frames;
    plat_counter now;
    plat_qpc(&now);
    if (!second_start.QuadPart) second_start = now;
    int64_t n = plat_inc(&g_frames);
    second_frames++;
    int64_t at = g_vsync_n + 1, iv = g_interval;
    if (!iv) at = g_vsync_n;                                       /* interval 0: shown at once, nobody waits */
    else if (at < g_latch + iv) at = g_latch + iv;
    g_latch = at;
    plat_xchg(&g_latch_pending, iv != 0);
    clock_stat(n, now);
    {   /* TEST ONLY, SMO_FRAMELOG=<k>: a line "frame-mark: <n>" every k presented frames, to place log events in game frames */
        static int every = -1;
        if (every < 0) { const char *e = getenv("SMO_FRAMELOG"); every = e ? atoi(e) : 0; }
        if (every > 0 && n % every == 0) printf("frame-mark: %lld%c", (long long)n, 10);
    }
    if ((now.QuadPart - second_start.QuadPart) >= g_freq.QuadPart) {
        char title[160];
        if (gfx_options()->overlay && second_frames) {       /* frame_time_overlay = on */
            double ms = (double)(now.QuadPart - second_start.QuadPart) * 1000.0 / (double)g_freq.QuadPart / (double)second_frames;
            printf("frame-time: %.2f ms per frame over the last second (%lld frames)\n", ms, (long long)second_frames);
            snprintf(title, sizeof title, "%s - frame %lld - %lld fps - %.2f ms per frame", g_title, (long long)n, (long long)second_frames, ms);
        } else
        snprintf(title, sizeof title, "%s - frame %lld - %lld fps", g_title, (long long)n, (long long)second_frames);
        display_set_title(title);
        second_frames = 0;
        second_start = now;
    }
}
/* New pacing, part 2: holds the caller until the refresh that shows the last queued frame. Returns 1 if it waited. */
int display_wait_refresh(void) {
    if (old_pacing() || !plat_xchg(&g_latch_pending, 0)) return 0;
    if (g_vsync_n >= g_latch) return 0;
    plat_counter a, b;
    plat_qpc(&a);
    hitch_pacing(1);
    nvn_tl(6 /* TL_PACE_BEGIN */, (int)(g_latch & 0xffff));
    plat_lock(&g_vsync_lock);
    while (g_vsync_n < g_latch) plat_cond_wait_ms(&g_vsync_cv, &g_vsync_lock, 100);
    plat_unlock(&g_vsync_lock);
    nvn_tl(7 /* TL_PACE_END */, (int)(g_vsync_n & 0xffff));
    hitch_pacing(0);
    plat_qpc(&b);
    nvn_pf_pace(b.QuadPart - a.QuadPart);
    return 1;
}
/* smo_amiibo.c: the menu's own pace while it holds the game: until the next refresh of the clock (16 ms where there is no
 * refresh clock: SMO_OLD_PACING) */
void display_menu_wait(void) {
    if (old_pacing()) { plat_sleep_ms(16); return; }
    int64_t n = g_vsync_n;
    plat_lock(&g_vsync_lock);
    while (g_vsync_n == n) if (!plat_cond_wait_ms(&g_vsync_cv, &g_vsync_lock, 100)) break;
    plat_unlock(&g_vsync_lock);
}
/* Before a present: a frame queued earlier that nobody waited for (a caller that does not wait on the window sync). */
void display_before_present(void) {
    static int said;
    if (old_pacing() || !g_latch_pending) return;
    if (!said) { said = 1; printf("display: a present without a wait on the window texture's sync: held at the next present instead%c", 10); }
    display_wait_refresh();
}
void display_pace(void) {
    static plat_counter next, second_start;
    static int64_t second_frames;
    plat_counter now;
    if (!old_pacing()) { frame_queued(); return; }
    plat_qpc(&now);
    if (!next.QuadPart) { next = now; second_start = now; }
    int64_t n = plat_inc(&g_frames);
    second_frames++;
    clock_stat(n, now);
    next.QuadPart += g_freq.QuadPart * g_interval / 60;
    /* wait until `next` exactly. plat_sleep_ms(ms) cut the wait to whole milliseconds and, at the default 15.6 ms timer period,
     * could overshoot by a frame; a frame that came late was then followed by frames shown early (up to 100 ms of debt).
     * Now: clock_nanosleep up to 0.5 ms before the deadline, then a spin; a late frame may be made up for
     * by at most one refresh interval, as a display that missed a refresh shows the next frame at the next one. */
    int64_t left = next.QuadPart - now.QuadPart, frame_ticks = g_freq.QuadPart * (g_interval ? g_interval : 1) / 60;
    if (!g_interval) next = now;
    else if (left > 0) {
        plat_counter after;
        hitch_pacing(1);
        if (left > g_freq.QuadPart / 2000) plat_sleep_until(next.QuadPart - g_freq.QuadPart / 2000);
        for (plat_qpc(&after); after.QuadPart < next.QuadPart; plat_qpc(&after)) plat_pause();
        hitch_pacing(0);
        nvn_pf_pace(after.QuadPart - now.QuadPart);
    } else if (-left > frame_ticks) next.QuadPart = now.QuadPart - frame_ticks;
    if ((now.QuadPart - second_start.QuadPart) >= g_freq.QuadPart) {
        char title[160];
        if (gfx_options()->overlay && second_frames) printf("frame-time: %.2f ms per frame over the last second (%lld frames)\n", (double)(now.QuadPart - second_start.QuadPart) * 1000.0 / (double)g_freq.QuadPart / (double)second_frames, (long long)second_frames);
        if (gfx_options()->overlay && second_frames) snprintf(title, sizeof title, "%s - frame %lld - %lld fps - %.2f ms per frame", g_title, (long long)n, (long long)second_frames, (double)(now.QuadPart - second_start.QuadPart) * 1000.0 / (double)g_freq.QuadPart / (double)second_frames);
        else
        snprintf(title, sizeof title, "%s - frame %lld - %lld fps", g_title, (long long)n, (long long)second_frames);
        display_set_title(title);
        second_frames = 0;
        second_start = now;
    }
}

long long display_frames(void) { return g_frames; }  /* smo_nerve.c (SMO_STAGE_LIST): frames presented so far */

void display_report(FILE *out) {
    fprintf(out, "\ndisplay: %lld frames presented\n", (long long)g_frames);
    fprintf(out, "display: %s; %lld vsync signals, %lld refreshes slept through by the vsync thread\n", old_pacing() ? "old pacing (SMO_OLD_PACING=1: two clocks)" : "one refresh clock", (long long)g_vsync_signals, (long long)g_vsync_late);
    nvn_gl_report(out);
}

/* ---- nn::vi: one display, one layer; the native window is an opaque token for NVN */
static uint8_t g_display_token[16], g_layer_token[16], g_native_window_token[16];
static void vi_OpenDefaultDisplay(Ctx *c) { wr64(A0, (uint64_t)(uintptr_t)g_display_token); RET(0); }
static void vi_CreateLayer(Ctx *c) { wr64(A0, (uint64_t)(uintptr_t)g_layer_token); RET(0); }
static void vi_GetNativeWindow(Ctx *c) { wr64(A0, (uint64_t)(uintptr_t)g_native_window_token); RET(0); }
static void vi_GetDisplayVsyncEvent(Ctx *c) {         /* (SystemEventType *out, Display *) */
    os_init_event(A0, 1);
    g_vsync_event = A0;
    RET(0);
}
static void nothing(Ctx *c) { (void)c; }

static const ImportDef vi_imports[] = {
    {"_ZN2nn2vi10InitializeEv", nothing},
    {"_ZN2nn2vi18OpenDefaultDisplayEPPNS0_7DisplayE", vi_OpenDefaultDisplay},
    {"_ZN2nn2vi11CreateLayerEPPNS0_5LayerEPNS0_7DisplayE", vi_CreateLayer},
    {"_ZN2nn2vi15GetNativeWindowEPPvPNS0_5LayerE", vi_GetNativeWindow},
    {"_ZN2nn2vi20GetDisplayVsyncEventEPNS_2os15SystemEventTypeEPNS0_7DisplayE", vi_GetDisplayVsyncEvent},
};

void register_display_imports(void) {
    register_imports(vi_imports, (int)(sizeof vi_imports / sizeof vi_imports[0]));
}
