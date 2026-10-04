/*
 * Microsoft Flight Simulator 1.0x (IBM PC, 1982) - SDL3 port.
 *
 * Phase 1: the original program runs on an embedded minimal PC (pc.c) and is
 * displayed, heard and controlled through SDL3. See docs/PORT_PLAN.md.
 *
 * Port keys (F1-F10 all belong to the game): F11 toggles composite/RGB monitor,
 *       F12 cycles emulation speed, Ctrl+F12 dumps memory to extracted/mem_dump.bin.
 * Enhanced options (src/enhanced/enhanced.h):
 *   --classic           every enhancement off: the faithful port v0.1.0
 * Dev options:
 *   --frames N          quit after N frames (each frame is exactly 1/60 s of emulated time)
 *   --screenshot FILE   save the last frame as BMP on quit
 *   --keys LIST         scripted key presses, comma separated FRAME:KEY[*HOLD_FRAMES],
 *                       KEY is an SDL scancode name, e.g. "300:F2,400:Keypad 8*30"
 *   --poke LIST         scripted writes to the game data segment (DS 0618), comma separated
 *                       FRAME:OFF=VAL[*FRAMES], hex OFF and VAL; VAL of 1-2 hex digits writes a
 *                       byte, 3-4 a word; *FRAMES repeats the write at the start of each frame,
 *                       e.g. "3000:1DC8=02,3000:08A8=DA20*120". VAL @SRC[.N][+-DELTA] copies N
 *                       bytes (1-3, default 2) from SRC, plus hex DELTA, e.g. "5000:1F3B=@30D2.3-800"
 *                       (applied in list order, each frame). Used to reach rare states
 *                       (tools/rare_sessions.py)
 *   --shot-at FRAME     also save FILE_FRAME.bmp at that frame (repeatable)
 *   --type TEXT         type TEXT, one key per second, starting after 2 seconds
 *   --dump-on-exit      write extracted/mem_dump.bin on quit
 *   --rgb               start with the RGB (mono) display instead of composite
 *   --trace FILE        write a 1 MB execution/data trace map (cpu8086.h T_* flags) on quit;
 *                       an existing FILE is merged (OR) so several sessions accumulate
 *   --native-off NAME   run the original code instead of native NAME ("all" = every native)
 *   --native-on NAME    enable native NAME (or "all"), e.g. the default-off test_passthrough
 *   --list-natives      print the native replacement registry and exit
 *   --verify NAME       differential check of native NAME (or "all") against the original on
 *                       every call; prints "verify-summary NAME calls N mismatches M" on quit
 *   --stats             count the original (non-native) instructions executed and print the total,
 *                       the native calls and the hottest addresses on quit
 *   --stats-from FRAME  start counting at that frame (default 0)
 *   --stats-out FILE    also write every executed address as "LINEAR COUNT" lines (for
 *                       tools/insn_stats.py, which totals them per routine)
 *   --csched            (FS1_EMULATOR build) run with the C scheduler, as the default build does;
 *                       original instructions are then only run where a native is missing
 *   --emulate           (FS1_EMULATOR build) run on the emulated PC (the default of that build)
 *   --check-boot        (FS1_EMULATOR build) compare the C boot with the original loader and exit
 *   --verify, --trace and --native-off need the FS1_EMULATOR build (they run original code).
 *   Native options apply in command-line order.
 */
#define SDL_MAIN_USE_CALLBACKS 1
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include "disk.h"
#include "native.h"
#include "pc.h"
#include "game/state.h"
#include "enhanced/enhanced.h"   /* ENH: options */
#include "enhanced/sharp.h"      /* ENH: sharp renderer */

#define WINDOW_W 960
#define WINDOW_H 720
#define AUDIO_RATE 44100
#define DEFAULT_DISK_NAME "Microsoft Flight Simulator v1.05 (198x)(Microsoft Corporation).ima"

typedef struct App {
    SDL_Window *window;
    SDL_Renderer *renderer;
    SDL_Texture *screen;
    SDL_AudioStream *audio;
    uint32_t pixels[CGA_W * CGA_H];
    float samples[AUDIO_RATE / 5];
    Disk disk;
    Pc *pc;
    long start_frame;        /* ENH: --start: frame at which the editor keys were scheduled (0 = not yet) */
    SDL_Surface *sharp_shot; /* ENH: last sharp frame read back for --screenshot */

    uint64_t last_ticks;
    /* Emulated-time target. Kept absolute so that when a long native step overshoots a
     * frame's budget, the excess is paid back next frame instead of piling up. */
    uint64_t target_cycles;
    int speed;

    long frame;
    long max_frames;
    const char *screenshot;
    const char *type_text;
    struct { long frame, hold; uint8_t sc; } keys[4096];
    int key_count;
    struct { long frame, hold; uint16_t off, src; uint32_t val; int size; bool copy; } pokes[1024];
    int poke_count;
    long shot_at[32];
    int shot_count;
    bool dump_on_exit;
    bool start_rgb;
    const char *trace_path;
    bool stats;
    long stats_from;
    const char *stats_out;
    uint32_t *exec_count;
    uint64_t stats_native_calls0;
    bool csched;
} App;

static uint8_t xt_scancode(SDL_Scancode sc)
{
    switch (sc) {
    case SDL_SCANCODE_ESCAPE: return 0x01;
    case SDL_SCANCODE_1: return 0x02;
    case SDL_SCANCODE_2: return 0x03;
    case SDL_SCANCODE_3: return 0x04;
    case SDL_SCANCODE_4: return 0x05;
    case SDL_SCANCODE_5: return 0x06;
    case SDL_SCANCODE_6: return 0x07;
    case SDL_SCANCODE_7: return 0x08;
    case SDL_SCANCODE_8: return 0x09;
    case SDL_SCANCODE_9: return 0x0A;
    case SDL_SCANCODE_0: return 0x0B;
    case SDL_SCANCODE_MINUS: return 0x0C;
    case SDL_SCANCODE_EQUALS: return 0x0D;
    case SDL_SCANCODE_BACKSPACE: return 0x0E;
    case SDL_SCANCODE_TAB: return 0x0F;
    case SDL_SCANCODE_Q: return 0x10;
    case SDL_SCANCODE_W: return 0x11;
    case SDL_SCANCODE_E: return 0x12;
    case SDL_SCANCODE_R: return 0x13;
    case SDL_SCANCODE_T: return 0x14;
    case SDL_SCANCODE_Y: return 0x15;
    case SDL_SCANCODE_U: return 0x16;
    case SDL_SCANCODE_I: return 0x17;
    case SDL_SCANCODE_O: return 0x18;
    case SDL_SCANCODE_P: return 0x19;
    case SDL_SCANCODE_LEFTBRACKET: return 0x1A;
    case SDL_SCANCODE_RIGHTBRACKET: return 0x1B;
    case SDL_SCANCODE_RETURN: return 0x1C;
    case SDL_SCANCODE_KP_ENTER: return 0x1C;
    case SDL_SCANCODE_LCTRL: case SDL_SCANCODE_RCTRL: return 0x1D;
    case SDL_SCANCODE_A: return 0x1E;
    case SDL_SCANCODE_S: return 0x1F;
    case SDL_SCANCODE_D: return 0x20;
    case SDL_SCANCODE_F: return 0x21;
    case SDL_SCANCODE_G: return 0x22;
    case SDL_SCANCODE_H: return 0x23;
    case SDL_SCANCODE_J: return 0x24;
    case SDL_SCANCODE_K: return 0x25;
    case SDL_SCANCODE_L: return 0x26;
    case SDL_SCANCODE_SEMICOLON: return 0x27;
    case SDL_SCANCODE_APOSTROPHE: return 0x28;
    case SDL_SCANCODE_GRAVE: return 0x29;
    case SDL_SCANCODE_LSHIFT: return 0x2A;
    case SDL_SCANCODE_BACKSLASH: return 0x2B;
    case SDL_SCANCODE_Z: return 0x2C;
    case SDL_SCANCODE_X: return 0x2D;
    case SDL_SCANCODE_C: return 0x2E;
    case SDL_SCANCODE_V: return 0x2F;
    case SDL_SCANCODE_B: return 0x30;
    case SDL_SCANCODE_N: return 0x31;
    case SDL_SCANCODE_M: return 0x32;
    case SDL_SCANCODE_COMMA: return 0x33;
    case SDL_SCANCODE_PERIOD: return 0x34;
    case SDL_SCANCODE_SLASH: return 0x35;
    case SDL_SCANCODE_RSHIFT: return 0x36;
    case SDL_SCANCODE_KP_MULTIPLY: return 0x37;
    case SDL_SCANCODE_LALT: case SDL_SCANCODE_RALT: return 0x38;
    case SDL_SCANCODE_SPACE: return 0x39;
    case SDL_SCANCODE_CAPSLOCK: return 0x3A;
    case SDL_SCANCODE_F1: return 0x3B;
    case SDL_SCANCODE_F2: return 0x3C;
    case SDL_SCANCODE_F3: return 0x3D;
    case SDL_SCANCODE_F4: return 0x3E;
    case SDL_SCANCODE_F5: return 0x3F;
    case SDL_SCANCODE_F6: return 0x40;
    case SDL_SCANCODE_F7: return 0x41;
    case SDL_SCANCODE_F8: return 0x42;
    case SDL_SCANCODE_F9: return 0x43;
    case SDL_SCANCODE_F10: return 0x44;
    case SDL_SCANCODE_NUMLOCKCLEAR: return 0x45;
    case SDL_SCANCODE_SCROLLLOCK: return 0x46;
    case SDL_SCANCODE_KP_7: case SDL_SCANCODE_HOME: return 0x47;
    case SDL_SCANCODE_KP_8: case SDL_SCANCODE_UP: return 0x48;
    case SDL_SCANCODE_KP_9: case SDL_SCANCODE_PAGEUP: return 0x49;
    case SDL_SCANCODE_KP_MINUS: return 0x4A;
    case SDL_SCANCODE_KP_4: case SDL_SCANCODE_LEFT: return 0x4B;
    case SDL_SCANCODE_KP_5: return 0x4C;
    case SDL_SCANCODE_KP_6: case SDL_SCANCODE_RIGHT: return 0x4D;
    case SDL_SCANCODE_KP_PLUS: return 0x4E;
    case SDL_SCANCODE_KP_1: case SDL_SCANCODE_END: return 0x4F;
    case SDL_SCANCODE_KP_2: case SDL_SCANCODE_DOWN: return 0x50;
    case SDL_SCANCODE_KP_3: case SDL_SCANCODE_PAGEDOWN: return 0x51;
    case SDL_SCANCODE_KP_0: case SDL_SCANCODE_INSERT: return 0x52;
    case SDL_SCANCODE_KP_PERIOD: case SDL_SCANCODE_DELETE: return 0x53;
    default: return 0;
    }
}

static uint8_t ascii_scancode(char ch, bool *shift)
{
    static const char lower[] = "\0\x1b" "1234567890-=\b\tqwertyuiop[]\r\0asdfghjkl;'`\0\\zxcvbnm,./\0*\0 ";
    static const char upper[] = "\0\x1b" "!@#$%^&*()_+\b\tQWERTYUIOP{}\r\0ASDFGHJKL:\"~\0|ZXCVBNM<>?\0*\0 ";
    for (int i = 1; i < (int)sizeof lower - 1; i++) {
        if (lower[i] == ch) {
            *shift = false;
            return (uint8_t)i;
        }
        if (upper[i] == ch) {
            *shift = true;
            return (uint8_t)i;
        }
    }
    return 0;
}

static void type_char(Pc *pc, char ch)
{
    bool shift;
    uint8_t sc = ascii_scancode(ch, &shift);
    if (!sc)
        return;
    if (shift)
        pc_key_event(pc, 0x2A);
    pc_key_event(pc, sc);
    pc_key_event(pc, sc | 0x80);
    if (shift)
        pc_key_event(pc, 0x2A | 0x80);
}

static void dump_memory(App *app)
{
    SDL_CreateDirectory("extracted");
    if (SDL_SaveFile("extracted/mem_dump.bin", app->pc->mem, CPU_MEM_SIZE))
        SDL_Log("Memory dumped to extracted/mem_dump.bin (CS:IP %04X:%04X)", app->pc->cpu.sregs[S_CS],
                app->pc->cpu.ip);
    else
        SDL_Log("Memory dump failed: %s", SDL_GetError());
}

/* ENH: true when this frame ends at a screenshot (or the last frame, saved at quit). */
static bool shot_due(const App *app)
{
    long next = app->frame + 1;
    if (app->screenshot && app->max_frames && next >= app->max_frames)
        return true;
    for (int i = 0; i < app->shot_count; i++)
        if (app->shot_at[i] == next)
            return true;
    return false;
}

static void save_screenshot(App *app, const char *path)
{
    if (enh.sharp && app->sharp_shot) { /* ENH: the window picture of the sharp renderer */
        if (SDL_SaveBMP(app->sharp_shot, path))
            SDL_Log("Screenshot saved to %s", path);
        else
            SDL_Log("Screenshot failed: %s", SDL_GetError());
        return;
    }
    SDL_Surface *s = SDL_CreateSurfaceFrom(CGA_W, CGA_H, SDL_PIXELFORMAT_XRGB8888, app->pixels,
                                           CGA_W * (int)sizeof(uint32_t));
    if (s && SDL_SaveBMP(s, path))
        SDL_Log("Screenshot saved to %s", path);
    else
        SDL_Log("Screenshot failed: %s", SDL_GetError());
    SDL_DestroySurface(s);
}

/* Builds the CGA text-mode font without shipping IBM's ROM: printable ASCII is read back
 * from SDL's built-in public-domain debug font, and a few block/line glyphs are drawn here. */
static void build_text_font(SDL_Renderer *renderer, uint8_t font[256][8])
{
    SDL_memset(font, 0, 256 * 8);
    SDL_Texture *t = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA8888, SDL_TEXTUREACCESS_TARGET, 95 * 8, 8);
    if (t && SDL_SetRenderTarget(renderer, t)) {
        SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
        SDL_RenderClear(renderer);
        SDL_SetRenderDrawColor(renderer, 255, 255, 255, 255);
        for (int ch = 32; ch < 127; ch++) {
            char s[2] = { (char)ch, 0 };
            SDL_RenderDebugText(renderer, (float)((ch - 32) * 8), 0, s);
        }
        SDL_Surface *surf = SDL_RenderReadPixels(renderer, NULL);
        SDL_Surface *rgba = surf ? SDL_ConvertSurface(surf, SDL_PIXELFORMAT_RGBA32) : NULL;
        if (rgba) {
            for (int ch = 32; ch < 127; ch++) {
                for (int y = 0; y < 8; y++) {
                    const uint8_t *row = (const uint8_t *)rgba->pixels + y * rgba->pitch + (ch - 32) * 8 * 4;
                    for (int x = 0; x < 8; x++)
                        if (row[x * 4] > 127)
                            font[ch][y] |= (uint8_t)(0x80 >> x);
                }
            }
        } else {
            SDL_Log("Text font readback failed: %s", SDL_GetError());
        }
        SDL_DestroySurface(rgba);
        SDL_DestroySurface(surf);
        SDL_SetRenderTarget(renderer, NULL);
    }
    SDL_DestroyTexture(t);

    for (int y = 0; y < 8; y++) {
        font[0xDB][y] = 0xFF;                       /* full block */
        font[0xDC][y] = y >= 4 ? 0xFF : 0x00;       /* lower half */
        font[0xDF][y] = y < 4 ? 0xFF : 0x00;        /* upper half */
        font[0xDD][y] = 0xF0;                       /* left half */
        font[0xDE][y] = 0x0F;                       /* right half */
        font[0xB0][y] = (y & 1) ? 0x22 : 0x88;      /* light shade */
        font[0xB1][y] = (y & 1) ? 0x55 : 0xAA;      /* medium shade */
        font[0xB2][y] = (y & 1) ? 0xDD : 0x77;      /* dark shade */
        font[0xB3][y] = 0x18;                       /* vertical line */
    }
    font[0xC4][3] = font[0xC4][4] = 0xFF;           /* horizontal line */
    static const uint8_t right[8] = { 0x80, 0xE0, 0xF8, 0xFE, 0xF8, 0xE0, 0x80, 0x00 };
    static const uint8_t left[8] = { 0x02, 0x0E, 0x3E, 0xFE, 0x3E, 0x0E, 0x02, 0x00 };
    SDL_memcpy(font[0x10], right, 8);
    SDL_memcpy(font[0x11], left, 8);
}

static void parse_keys(App *app, const char *list)
{
    char *copy = SDL_strdup(list), *save = NULL;
    for (char *tok = SDL_strtok_r(copy, ",", &save); tok; tok = SDL_strtok_r(NULL, ",", &save)) {
        char *colon = SDL_strchr(tok, ':');
        if (!colon)
            continue;
        if (app->key_count >= (int)SDL_arraysize(app->keys)) {
            SDL_Log("--keys: more than %d keys, the rest are ignored", (int)SDL_arraysize(app->keys));
            break;
        }
        *colon = 0;
        char *name = colon + 1;
        long hold = 1;
        char *star = SDL_strrchr(name, '*');
        if (star && star != name && star[1] >= '0' && star[1] <= '9') {
            *star = 0;
            hold = SDL_atoi(star + 1);
        }
        uint8_t sc = xt_scancode(SDL_GetScancodeFromName(name));
        if (!sc) {
            SDL_Log("--keys: unknown key '%s'", name);
            continue;
        }
        app->keys[app->key_count].frame = SDL_atoi(tok);
        app->keys[app->key_count].hold = hold < 1 ? 1 : hold;
        app->keys[app->key_count].sc = sc;
        app->key_count++;
    }
    SDL_free(copy);
}

/* ENH: --start N. Once the start-up menus are over (the game's frame counter is running) press Esc, type the user
 * mode and Esc again, as the campaign sessions do: the editor recalls the preset (position, time, weather). */
static void start_mode_tick(App *app)
{
    if (!enh.start_set || app->start_frame || !app->pc)
        return;
    const uint8_t *fc = app->pc->cpu.mem + cpu_linear(GAME_DS, 0x03F6); /* frame_counter */
    unsigned frames = (unsigned)(fc[0] | fc[1] << 8);
    if (frames < 3 || frames > 200) /* 0 or boot leftovers while the menus run; counts up in flight */
        return;
    long s = app->frame + 30;
    char list[160];
    SDL_snprintf(list, sizeof list, "%ld:Escape*2,%ld:%d*2,%ld:Return*2,%ld:Escape*2", s, s + 40, enh.start_mode, s + 60,
                 s + 100);
    parse_keys(app, list);
    app->start_frame = s;
}

static void run_key_script(App *app)
{
    /* Like a real keyboard, only the most recently pressed held key auto-repeats. */
    int newest = -1;
    for (int i = 0; i < app->key_count; i++) {
        long rel = app->frame - app->keys[i].frame;
        if (rel >= 0 && rel < app->keys[i].hold && (newest < 0 || app->keys[i].frame >= app->keys[newest].frame))
            newest = i;
    }
    for (int i = 0; i < app->key_count; i++) {
        long rel = app->frame - app->keys[i].frame;
        if (rel < 0 || rel > app->keys[i].hold)
            continue;
        if (rel == app->keys[i].hold)
            pc_key_event(app->pc, app->keys[i].sc | 0x80);
        else if (rel == 0 || (i == newest && rel >= 30 && rel % 6 == 0)) /* 500 ms delay, then 10/s */
            pc_key_event(app->pc, app->keys[i].sc);
    }
}

static void parse_pokes(App *app, const char *list)
{
    char *copy = SDL_strdup(list), *save = NULL;
    for (char *tok = SDL_strtok_r(copy, ",", &save); tok; tok = SDL_strtok_r(NULL, ",", &save)) {
        char *colon = SDL_strchr(tok, ':'), *eq = SDL_strchr(tok, '=');
        if (!colon || !eq || eq < colon) {
            SDL_Log("--poke: bad entry '%s'", tok);
            continue;
        }
        if (app->poke_count >= (int)SDL_arraysize(app->pokes)) {
            SDL_Log("--poke: more than %d entries, the rest are ignored", (int)SDL_arraysize(app->pokes));
            break;
        }
        *eq = 0;
        char *val = eq + 1, *star = SDL_strchr(val, '*');
        long hold = 1;
        if (star) {
            *star = 0;
            hold = SDL_atoi(star + 1);
        }
        app->pokes[app->poke_count].frame = SDL_atoi(tok);
        app->pokes[app->poke_count].hold = hold < 1 ? 1 : hold;
        app->pokes[app->poke_count].off = (uint16_t)SDL_strtoul(colon + 1, NULL, 16);
        if (val[0] == '@') { /* @SRC[.N][+-DELTA]: copy N bytes (default 2) from SRC, plus DELTA */
            char *end;
            app->pokes[app->poke_count].copy = true;
            app->pokes[app->poke_count].src = (uint16_t)SDL_strtoul(val + 1, &end, 16);
            app->pokes[app->poke_count].size = 2;
            if (*end == '.')
                app->pokes[app->poke_count].size = (int)SDL_strtoul(end + 1, &end, 10);
            app->pokes[app->poke_count].val = *end == '+' || *end == '-' ? (uint32_t)SDL_strtol(end, NULL, 16) : 0;
        } else {
            app->pokes[app->poke_count].copy = false;
            app->pokes[app->poke_count].val = (uint32_t)SDL_strtoul(val, NULL, 16);
            app->pokes[app->poke_count].size = SDL_strlen(val) > 2 ? 2 : 1;
        }
        if (app->pokes[app->poke_count].size < 1 || app->pokes[app->poke_count].size > 3) {
            SDL_Log("--poke: bad size in '%s'", val);
            continue;
        }
        app->poke_count++;
    }
    SDL_free(copy);
}

static void run_poke_script(App *app)
{
    for (int i = 0; i < app->poke_count; i++) {
        long rel = app->frame - app->pokes[i].frame;
        if (rel < 0 || rel >= app->pokes[i].hold)
            continue;
        uint8_t *mem = app->pc->mem + 0x6180u; /* DS 0618 */
        uint32_t v = app->pokes[i].val;
        if (app->pokes[i].copy)
            for (int b = 0; b < app->pokes[i].size; b++)
                v += (uint32_t)mem[(uint16_t)(app->pokes[i].src + b)] << (8 * b);
        for (int b = 0; b < app->pokes[i].size; b++)
            mem[(uint16_t)(app->pokes[i].off + b)] = (uint8_t)(v >> (8 * b));
    }
}

static bool file_exists(const char *path)
{
    SDL_PathInfo info;
    return SDL_GetPathInfo(path, &info) && info.type == SDL_PATHTYPE_FILE;
}

/* Looks for the default disk image relative to the working directory and to the executable,
 * so the game starts both from the project root and from inside build/. */
static char *find_default_disk(void)
{
    static const char *dirs[] = { "", "original/", "../original/", "../../original/" };
    const char *bases[2] = { "", SDL_GetBasePath() };
    for (int b = 0; b < 2; b++) {
        if (!bases[b])
            continue;
        for (int d = 0; d < (int)SDL_arraysize(dirs); d++) {
            char *path = NULL;
            SDL_asprintf(&path, "%s%s%s", bases[b], dirs[d], DEFAULT_DISK_NAME);
            if (path && file_exists(path))
                return path;
            SDL_free(path);
        }
    }
    /* Release zips: any 160K .ima next to the executable or in the working directory. */
    for (int b = 1; b >= 0; b--) {
        if (!bases[b])
            continue;
        const char *dir = bases[b][0] ? bases[b] : ".";
        int count = 0;
        char **names = SDL_GlobDirectory(dir, "*.ima", SDL_GLOB_CASEINSENSITIVE, &count);
        char *found = NULL;
        for (int i = 0; names && i < count && !found; i++) {
            char *path = NULL;
            SDL_asprintf(&path, "%s%s", bases[b], names[i]);
            SDL_PathInfo info;
            if (path && SDL_GetPathInfo(path, &info) && info.type == SDL_PATHTYPE_FILE && info.size == 163840)
                found = path;
            else
                SDL_free(path);
        }
        SDL_free(names);
        if (found)
            return found;
    }
    return NULL;
}

static SDL_AppResult fail(const char *message)
{
    SDL_Log("%s", message);
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Flight Simulator 1", message, NULL);
    return SDL_APP_FAILURE;
}

SDL_AppResult SDL_AppInit(void **appstate, int argc, char *argv[])
{
    SDL_SetAppMetadata("Flight Simulator 1", FS1_VERSION, "fs1-sdl3");

    App *app = SDL_calloc(1, sizeof(App));
    if (!app)
        return SDL_APP_FAILURE;
    *appstate = app;
    app->speed = 1;
#ifndef FS1_EMULATOR
    app->csched = true;
#endif

    const char *disk_path = NULL;
    bool check_boot = false;
    for (int i = 1; i < argc; i++) {
        if (enh_parse_arg(argc, argv, &i))   /* ENH: --classic and the enhanced options */
            ;
        else if (SDL_strcmp(argv[i], "--frames") == 0 && i + 1 < argc)
            app->max_frames = SDL_atoi(argv[++i]);
        else if (SDL_strcmp(argv[i], "--screenshot") == 0 && i + 1 < argc)
            app->screenshot = argv[++i];
        else if (SDL_strcmp(argv[i], "--trace") == 0 && i + 1 < argc) {
#ifndef FS1_EMULATOR
            return fail("--trace needs the FS1_EMULATOR build (cmake -DFS1_EMULATOR=ON)");
#endif
            app->trace_path = argv[++i];
        }
        else if (SDL_strcmp(argv[i], "--csched") == 0)
            app->csched = true;
        else if (SDL_strcmp(argv[i], "--emulate") == 0) {
#ifdef FS1_EMULATOR
            app->csched = false;
#else
            return fail("--emulate needs the FS1_EMULATOR build (cmake -DFS1_EMULATOR=ON)");
#endif
        } else if (SDL_strcmp(argv[i], "--check-boot") == 0) {
#ifdef FS1_EMULATOR
            check_boot = true;
#else
            return fail("--check-boot needs the FS1_EMULATOR build (cmake -DFS1_EMULATOR=ON)");
#endif
        } else if (SDL_strcmp(argv[i], "--rgb") == 0)
            app->start_rgb = true;
        else if (SDL_strcmp(argv[i], "--stats") == 0)
            app->stats = true;
        else if (SDL_strcmp(argv[i], "--stats-from") == 0 && i + 1 < argc) {
            app->stats = true;
            app->stats_from = SDL_atoi(argv[++i]);
        } else if (SDL_strcmp(argv[i], "--stats-out") == 0 && i + 1 < argc) {
            app->stats = true;
            app->stats_out = argv[++i];
        } else if (SDL_strcmp(argv[i], "--dump-on-exit") == 0)
            app->dump_on_exit = true;
        else if (SDL_strcmp(argv[i], "--keys") == 0 && i + 1 < argc)
            parse_keys(app, argv[++i]);
        else if (SDL_strcmp(argv[i], "--poke") == 0 && i + 1 < argc)
            parse_pokes(app, argv[++i]);
        else if (SDL_strcmp(argv[i], "--shot-at") == 0 && i + 1 < argc) {
            if (app->shot_count < (int)SDL_arraysize(app->shot_at))
                app->shot_at[app->shot_count++] = SDL_atoi(argv[++i]);
            else
                i++;
        } else if (SDL_strcmp(argv[i], "--type") == 0 && i + 1 < argc)
            app->type_text = argv[++i];
        else if ((SDL_strcmp(argv[i], "--native-off") == 0 || SDL_strcmp(argv[i], "--native-on") == 0 ||
                  SDL_strcmp(argv[i], "--verify") == 0) && i + 1 < argc) {
            const char *name = argv[i + 1];
#ifndef FS1_EMULATOR
            if (SDL_strcmp(argv[i], "--native-on") != 0)
                return fail("--native-off and --verify run original code: they need the FS1_EMULATOR build "
                            "(cmake -DFS1_EMULATOR=ON)");
#endif
            bool ok = SDL_strcmp(argv[i], "--verify") == 0 ? native_set_verify(name)
                                                           : native_set_enabled(name, argv[i][10] == 'n');
            if (!ok) {
                SDL_Log("Unknown native: %s (see --list-natives)", name);
                return SDL_APP_FAILURE;
            }
            i++;
        } else if (SDL_strcmp(argv[i], "--list-natives") == 0) {
            native_list();
            return SDL_APP_SUCCESS;
        } else if (SDL_strcmp(argv[i], "--version") == 0) {
            SDL_Log("fs1-enhanced %s", FS1_VERSION);
            return SDL_APP_SUCCESS;
        }
        else
            disk_path = argv[i];
    }

    char *found = NULL;
    if (!disk_path) {
        found = find_default_disk();
        if (!found)
            return fail("Disk image not found.\n\nPut your 160K PC disk image (.ima) next to fs1-enhanced.exe or in the "
                        "original/ folder, or pass its path as the first argument.");
        disk_path = found;
    }

    bool loaded = disk_load(&app->disk, disk_path);
    char *message = NULL;
    if (!loaded)
        SDL_asprintf(&message, "Cannot load disk image:\n%s", disk_path);
    else if (app->disk.format != DISK_PC_160K)
        SDL_asprintf(&message, "Not a PC disk image (only the PC version runs):\n%s", disk_path);
    if (message) {
        SDL_AppResult r = fail(message);
        SDL_free(message);
        SDL_free(found);
        return r;
    }
    SDL_Log("Booting %s (%s)", disk_path, app->csched ? "C scheduler" : "emulated PC");
    SDL_free(found);
#ifdef FS1_EMULATOR
    if (check_boot)
        return sched_check_boot(&app->disk) ? SDL_APP_SUCCESS : SDL_APP_FAILURE;
#else
    (void)check_boot;
#endif

    app->pc = SDL_malloc(sizeof(Pc));
    if (!app->pc || !pc_init(app->pc, &app->disk, app->csched))
        return SDL_APP_FAILURE;
    if (app->trace_path)
        app->pc->cpu.trace = SDL_calloc(1, CPU_MEM_SIZE);
    if (app->stats) {
        app->exec_count = SDL_calloc(CPU_MEM_SIZE, sizeof(uint32_t));
        if (!app->stats_from)
            app->pc->cpu.exec_count = app->exec_count;
    }
    native_init(app->pc);
    pc_boot(app->pc);
    if (app->start_rgb)
        app->pc->composite = false;
    enh_start(app->start_rgb); /* ENH: sharp view on unless --classic / --rgb / original code */

    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMEPAD)) {
        SDL_Log("SDL_Init failed: %s", SDL_GetError());
        return SDL_APP_FAILURE;
    }
    if (!SDL_CreateWindowAndRenderer("Flight Simulator 1", WINDOW_W, WINDOW_H, SDL_WINDOW_RESIZABLE,
                                     &app->window, &app->renderer)) {
        SDL_Log("Window creation failed: %s", SDL_GetError());
        return SDL_APP_FAILURE;
    }
    SDL_SetRenderVSync(app->renderer, 1);
    build_text_font(app->renderer, app->pc->font);
    /* CGA's 200 lines fill a 4:3 display */
    SDL_SetRenderLogicalPresentation(app->renderer, 640, 480, SDL_LOGICAL_PRESENTATION_LETTERBOX);

    app->screen = SDL_CreateTexture(app->renderer, SDL_PIXELFORMAT_XRGB8888, SDL_TEXTUREACCESS_STREAMING,
                                    CGA_W, CGA_H);
    if (!app->screen)
        return SDL_APP_FAILURE;
    SDL_SetTextureScaleMode(app->screen, SDL_SCALEMODE_PIXELART);

    SDL_AudioSpec spec = { SDL_AUDIO_F32, 1, AUDIO_RATE };
    app->audio = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, NULL, NULL);
    if (app->audio)
        SDL_ResumeAudioStreamDevice(app->audio);
    else
        SDL_Log("No audio: %s", SDL_GetError());

    app->last_ticks = SDL_GetTicksNS();
    return SDL_APP_CONTINUE;
}

/* Keypad keys that pick a view direction after Scroll Lock: KP7 8 9 4 5 6 1 2 3. */
static bool is_view_key(uint8_t xt)
{
    return (xt >= 0x47 && xt <= 0x49) || (xt >= 0x4B && xt <= 0x4D) || (xt >= 0x4F && xt <= 0x51);
}

static uint8_t numlock_sent = 0x45; /* XT scancode the last Num Lock press was sent as */

SDL_AppResult SDL_AppEvent(void *appstate, SDL_Event *event)
{
    App *app = appstate;
    switch (event->type) {
    case SDL_EVENT_QUIT:
        return SDL_APP_SUCCESS;
    case SDL_EVENT_KEY_DOWN:
        if (event->key.scancode == SDL_SCANCODE_F11) {
            if (!event->key.repeat && !enh_cycle_monitor(&app->pc->composite)) { /* ENH: sharp / composite / RGB */
                app->pc->composite = !app->pc->composite;
                SDL_Log("Monitor: %s", app->pc->composite ? "composite" : "RGB");
            }
            break;
        }
        if (event->key.scancode == SDL_SCANCODE_F12 && !(event->key.mod & SDL_KMOD_CTRL)) {
            if (!event->key.repeat) {
                app->speed = app->speed >= 8 ? 1 : app->speed * 2;
                SDL_Log("Emulation speed x%d", app->speed);
            }
            break;
        }
        if (event->key.scancode == SDL_SCANCODE_F12) {
            if (!event->key.repeat)
                dump_memory(app);
            break;
        }
        if (event->key.scancode == SDL_SCANCODE_NUMLOCKCLEAR && !event->key.repeat) {
            /* Port convenience: modern keyboards lack Scroll Lock, so Num Lock toggles the
             * radar. With the radar on, send Scroll Lock (radar off) instead. */
            numlock_sent = cpu_read8(&app->pc->cpu, cpu_linear(GAME_DS, GS_RADAR_VIEW)) ? 0x46 : 0x45;
            pc_key_event(app->pc, numlock_sent);
            break;
        }
        if ((event->key.mod & SDL_KMOD_LSHIFT) && !event->key.repeat
            && is_view_key(xt_scancode(event->key.scancode))
            && !cpu_read8(&app->pc->cpu, cpu_linear(GAME_DS, GS_RADAR_VIEW))) {
            /* Port convenience: Left Shift + keypad = the original Scroll Lock, keypad key
             * (choose the view direction). Tap Scroll Lock first, then send the key. */
            pc_key_event(app->pc, 0x46);
            pc_key_event(app->pc, 0x46 | 0x80);
        }
        if (xt_scancode(event->key.scancode))
            pc_key_event(app->pc, xt_scancode(event->key.scancode)); /* repeats = typematic */
        break;
    case SDL_EVENT_KEY_UP:
        if (event->key.scancode == SDL_SCANCODE_NUMLOCKCLEAR) {
            pc_key_event(app->pc, numlock_sent | 0x80);
            break;
        }
        if (xt_scancode(event->key.scancode))
            pc_key_event(app->pc, xt_scancode(event->key.scancode) | 0x80);
        break;
    default:
        break;
    }
    return SDL_APP_CONTINUE;
}

/* --stats: totals and the hottest addresses of original code. */
static void print_stats(App *app)
{
    const uint32_t *n = app->exec_count;
    uint64_t total = 0;
    enum { TOP = 20 };
    uint32_t top[TOP] = { 0 };
    for (uint32_t a = 0; a < CPU_MEM_SIZE; a++) {
        total += n[a];
        if (n[a] > n[top[TOP - 1]]) {
            int j = TOP - 1;
            while (j > 0 && n[a] > n[top[j - 1]]) {
                top[j] = top[j - 1];
                j--;
            }
            top[j] = a;
        }
    }
    SDL_Log("stats: from frame %ld: original instructions %llu, native calls %llu", app->stats_from,
            (unsigned long long)total, (unsigned long long)(native_total_calls() - app->stats_native_calls0));
    if (app->pc->csched)
        SDL_Log("stats: C scheduler: %llu native steps, %llu interrupts, %llu original instructions%s",
                (unsigned long long)app->pc->sched_steps, (unsigned long long)app->pc->sched_irqs,
                (unsigned long long)app->pc->sched_fallback,
#ifdef FS1_EMULATOR
                ""
#else
                " (no 8086 interpreter in this build)"
#endif
        );
    for (int i = 0; i < TOP && n[top[i]]; i++)
        SDL_Log("stats:   %05X %10u", top[i], n[top[i]]);
    if (app->stats_out) {
        SDL_IOStream *f = SDL_IOFromFile(app->stats_out, "w");
        if (f) {
            for (uint32_t a = 0; a < CPU_MEM_SIZE; a++)
                if (n[a])
                    SDL_IOprintf(f, "%05X %u\n", a, n[a]);
            SDL_CloseIO(f);
        }
    }
    app->pc->cpu.exec_count = NULL;
    SDL_free(app->exec_count);
    app->exec_count = NULL;
}

SDL_AppResult SDL_AppIterate(void *appstate)
{
    App *app = appstate;
    Pc *pc = app->pc;

    uint64_t now = SDL_GetTicksNS();
    double seconds = (double)(now - app->last_ticks) / 1e9;
    app->last_ticks = now;
    if (app->max_frames || seconds > 0.1)
        seconds = 1.0 / 60.0;

    if (app->type_text && app->frame >= 120 && app->frame % 60 == 0) {
        long i = (app->frame - 120) / 60;
        if (i < (long)SDL_strlen(app->type_text))
            type_char(pc, app->type_text[i]);
    }

    start_mode_tick(app); /* ENH */
    run_key_script(app);
    run_poke_script(app);
    if (app->stats && app->frame == app->stats_from && !pc->cpu.exec_count) {
        pc->cpu.exec_count = app->exec_count;
        app->stats_native_calls0 = native_total_calls();
    }
    /* Resync only after a real stall: one native step can be long (3.22: with the C scheduler a
     * scenery load is one 0.3 s step), and that excess is paid back over the next frames. */
    uint64_t stall = (uint64_t)(2.0 * PC_CPU_HZ);
    if ((pc->cpu.cycles > stall && app->target_cycles < pc->cpu.cycles - stall) || app->target_cycles == 0)
        app->target_cycles = pc->cpu.cycles; /* resync after a stall (or the first frame) */
    app->target_cycles += (uint64_t)(seconds * PC_CPU_HZ * app->speed);
    if (pc->cpu.cycles < app->target_cycles)
        pc_run(pc, app->target_cycles);

    int n = pc_speaker_render(pc, app->samples, SDL_arraysize(app->samples), AUDIO_RATE);
    if (app->audio && app->speed == 1 && SDL_GetAudioStreamQueued(app->audio) < AUDIO_RATE / 5 * 4)
        SDL_PutAudioStreamData(app->audio, app->samples, n * (int)sizeof(float));

    pc->blink_phase = (SDL_GetTicks() / 267) & 1; /* CGA blinks at ~1.9 Hz */
    if (enh.sharp)                                                      /* ENH: sharp panel (needles) */
        sharp_render_cga(pc, app->pixels);
    else
        pc_render_cga(pc, app->pixels);
    SDL_UpdateTexture(app->screen, NULL, app->pixels, CGA_W * sizeof(uint32_t));
    SDL_SetRenderDrawColor(app->renderer, 0, 0, 0, 255);
    SDL_RenderClear(app->renderer);
    SDL_RenderTexture(app->renderer, app->screen, NULL, NULL);
    if (enh.sharp)                                                      /* ENH: sharp 3D view */
        sharp_view(app->renderer, pc, app->screen);
    if (enh.sharp && shot_due(app)) {                                   /* ENH: screenshot of the sharp output */
        SDL_DestroySurface(app->sharp_shot);
        app->sharp_shot = SDL_RenderReadPixels(app->renderer, NULL);
    }
    SDL_RenderPresent(app->renderer);

    app->frame++;
    for (int i = 0; i < app->shot_count; i++) {
        if (app->shot_at[i] == app->frame && app->screenshot) {
            char *path = NULL;
            SDL_asprintf(&path, "%s_%ld.bmp", app->screenshot, app->frame);
            save_screenshot(app, path);
            SDL_free(path);
        }
    }
    if (app->max_frames && app->frame >= app->max_frames)
        return SDL_APP_SUCCESS;
    return SDL_APP_CONTINUE;
}

void SDL_AppQuit(void *appstate, SDL_AppResult result)
{
    (void)result;
    App *app = appstate;
    if (!app)
        return;
    if (app->trace_path && app->pc && app->pc->cpu.trace) {
        uint8_t *trace = app->pc->cpu.trace;
        size_t old_size = 0;
        uint8_t *old = SDL_LoadFile(app->trace_path, &old_size);
        for (size_t i = 0; i < CPU_MEM_SIZE; i++)
            trace[i] = (uint8_t)((trace[i] & ~T_RUN) | (old && old_size == CPU_MEM_SIZE ? old[i] : 0));
        SDL_free(old);
        if (SDL_SaveFile(app->trace_path, trace, CPU_MEM_SIZE))
            SDL_Log("Trace written to %s", app->trace_path);
        SDL_free(trace);
        app->pc->cpu.trace = NULL;
    }
    if (app->dump_on_exit && app->pc)
        dump_memory(app);
    if (app->screenshot && app->pc)
        save_screenshot(app, app->screenshot);
    if (app->pc) {
        SDL_Log("Stopped at CS:IP %04X:%04X after %llu cycles, CGA mode %02X color %02X CRTC R1=%02X R6=%02X R9=%02X", app->pc->cpu.sregs[S_CS],
                app->pc->cpu.ip,
                (unsigned long long)app->pc->cpu.cycles, app->pc->cga_mode, app->pc->cga_color,
                app->pc->crtc[1], app->pc->crtc[6], app->pc->crtc[9]);
        native_verify_summary();
        if (app->exec_count)
            print_stats(app);
        native_shutdown();
        pc_free(app->pc);
        SDL_free(app->pc);
    }
    SDL_DestroyAudioStream(app->audio);
    SDL_DestroyTexture(app->screen);
    SDL_DestroyRenderer(app->renderer);
    SDL_DestroyWindow(app->window);
    disk_free(&app->disk);
    SDL_free(app);
}
