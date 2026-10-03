#include "native.h"

#include <SDL3/SDL.h>
#include <stddef.h>

/* ---- registry ------------------------------------------------------------------
 * Natives live in one file per area, src/natives/<area>.c, each defining a
 * NativeEntry native_<area>[] table terminated by an entry with name == NULL.
 * The areas are listed in src/natives/areas.h. */

#define NATIVE_AREA(area) extern NativeEntry native_##area[];
#include "natives/areas.h"
#undef NATIVE_AREA

#define MAX_ENTRIES 1024
static NativeEntry *entries[MAX_ENTRIES];
static int entry_count;

static void collect_entries(void)
{
    if (entry_count)
        return;
    NativeEntry *tables[] = {
#define NATIVE_AREA(area) native_##area,
#include "natives/areas.h"
#undef NATIVE_AREA
    };
    for (size_t t = 0; t < SDL_arraysize(tables); t++)
        for (NativeEntry *e = tables[t]; e->name && entry_count < MAX_ENTRIES; e++)
            entries[entry_count++] = e;
}
#define ENTRY_COUNT entry_count

/* ---- dispatch ------------------------------------------------------------------- */

static Pc *g_pc;
static uint8_t hook_map[0x10000]; /* nonzero where an enabled entry starts (GAME_CS) */
static uint16_t hook_index[0x10000]; /* entry index + 1 per offset */
static bool verifying;            /* inside a --verify run: everything executes as original */

static void rebuild_map(void)
{
    collect_entries();
    bool any = false;
    SDL_memset(hook_map, 0, sizeof hook_map);
    SDL_memset(hook_index, 0, sizeof hook_index);
    for (int i = 0; i < ENTRY_COUNT; i++) {
        if (entries[i]->enabled && entries[i]->seg == GAME_CS) {
            uint16_t prev = hook_index[entries[i]->off];
            if (prev)
                SDL_Log("native: %s and %s are both enabled at %04X:%04X; using %s", entries[prev - 1]->name,
                        entries[i]->name, GAME_CS, entries[i]->off, entries[i]->name);
            hook_map[entries[i]->off] = 1;
            hook_index[entries[i]->off] = (uint16_t)(i + 1);
            any = true;
        }
    }
    if (g_pc)
        g_pc->cpu.hook_map = any ? hook_map : NULL;
}

/* ---- differential verification ---------------------------------------------------
 * The original routine runs first with an undo log of every RAM write (cpu_write8), then
 * the writes are undone and the native runs with a second log. Only the addresses in the
 * two logs are compared, then the original's result is put back. Device state (PIC, PIT,
 * keyboard, CGA, speaker) and cycles end as the original left them. Memory written behind
 * cpu_write8 (BIOS HLE services, e.g. disk reads) is not tracked. */

#define VERIFY_STEP_CAP 5000000L
#define VERIFY_LOG_CAP (4u << 20)
#define VERIFY_MAX_REPORTS 10

static CpuWriteLog log_orig, log_native;
static uint8_t *mark;        /* CPU_MEM_SIZE: 1 = written by the original, 2 = seen in native log */
static uint32_t *orig_addr;  /* unique addresses written by the original */
static uint8_t *orig_post;   /* and their values after the original */
static uint32_t orig_count;

/* Pc device state: everything after mem/disk except the speaker event array. */
#define DEV_A_START offsetof(Pc, pic_irr)
#define DEV_A_END offsetof(Pc, speaker)
#define DEV_B_START offsetof(Pc, speaker_count)
typedef struct DevState {
    uint8_t a[DEV_A_END - DEV_A_START];
    uint8_t b[sizeof(Pc) - DEV_B_START];
} DevState;
static DevState dev_pre, dev_orig;
static SpeakerEvent speaker_orig[PC_MAX_SPEAKER_EVENTS];

static void dev_save(const Pc *pc, DevState *d)
{
    SDL_memcpy(d->a, (const uint8_t *)pc + DEV_A_START, sizeof d->a);
    SDL_memcpy(d->b, (const uint8_t *)pc + DEV_B_START, sizeof d->b);
}

static void dev_load(Pc *pc, const DevState *d)
{
    SDL_memcpy((uint8_t *)pc + DEV_A_START, d->a, sizeof d->a);
    SDL_memcpy((uint8_t *)pc + DEV_B_START, d->b, sizeof d->b);
}

static bool log_alloc(CpuWriteLog *l)
{
    l->addr = SDL_malloc(VERIFY_LOG_CAP * sizeof *l->addr);
    l->old = SDL_malloc(VERIFY_LOG_CAP);
    l->cap = VERIFY_LOG_CAP;
    return l->addr && l->old;
}

static bool verify_alloc(void)
{
    if (mark)
        return true;
    mark = SDL_calloc(1, CPU_MEM_SIZE);
    orig_addr = SDL_malloc(VERIFY_LOG_CAP * sizeof *orig_addr);
    orig_post = SDL_malloc(VERIFY_LOG_CAP);
    return mark && orig_addr && orig_post && log_alloc(&log_orig) && log_alloc(&log_native);
}

static void log_undo(Pc *pc, const CpuWriteLog *l)
{
    for (uint32_t i = l->count; i-- > 0;)
        pc->mem[l->addr[i]] = l->old[i];
}

static bool ignored(const NativeEntry *e, uint32_t a)
{
    for (int i = 0; i < e->ignore_count; i++)
        if (a >= e->ignore[i].lo && a < e->ignore[i].hi)
            return true;
    return false;
}

/* Single-steps the original until it returns. No interrupts are delivered. */
static bool run_original(Pc *pc, const NativeEntry *e)
{
    Cpu8086 *c = &pc->cpu;
    uint16_t sp0 = c->regs[R_SP], ss = c->sregs[S_SS];
    uint32_t sa = cpu_linear(ss, sp0);
    uint16_t ret_ip = (uint16_t)(pc->mem[sa] | pc->mem[(sa + 1) & CPU_MEM_MASK] << 8);
    uint16_t ret_cs = c->sregs[S_CS];
    if (e->far) {
        uint32_t sb = cpu_linear(ss, (uint16_t)(sp0 + 2));
        ret_cs = (uint16_t)(pc->mem[sb] | pc->mem[(sb + 1) & CPU_MEM_MASK] << 8);
    }
    for (long n = 0; n < VERIFY_STEP_CAP; n++) {
        cpu_step(c);
        if (e->stop_hi) {
            if (c->sregs[S_CS] != GAME_CS || c->ip < e->stop_lo || c->ip >= e->stop_hi)
                return true;
            for (const uint16_t *s = e->stops; s && *s; s++)
                if (c->ip == *s)
                    return true;
            continue;
        }
        uint16_t d = (uint16_t)(c->regs[R_SP] - sp0);
        if (c->ip == ret_ip && c->sregs[S_CS] == ret_cs && d >= 2 && d < 0x8000)
            return true;
    }
    return false;
}

/* Runs the entry's C function; false if it declined the call. */
/* The natives running now, innermost last, with SP at their entry (for native_or_cpu_step). */
#define ACTIVE_MAX 64
static struct { const NativeEntry *e; uint16_t sp; } active[ACTIVE_MAX];
static int active_n;

static bool run_native(Pc *pc, NativeEntry *e)
{
    bool r = true;
    if (active_n < ACTIVE_MAX) {
        active[active_n].e = e;
        active[active_n].sp = pc->cpu.regs[R_SP];
    }
    active_n++;
    if (e->try_fn)
        r = e->try_fn(pc);
    else
        e->fn(pc);
    active_n--;
    return r;
}

/* True when e is running with SP as it is now: a native that runs its own routine as original
 * code (a rare path it does not implement) is at its own entry. */
static bool running_itself(const Pc *pc, const NativeEntry *e)
{
    for (int i = active_n < ACTIVE_MAX ? active_n : ACTIVE_MAX; i-- > 0;)
        if (active[i].e == e && active[i].sp == pc->cpu.regs[R_SP])
            return true;
    return false;
}

static bool verify_call(Pc *pc, NativeEntry *e)
{
    Cpu8086 *c = &pc->cpu;
    if (!verify_alloc()) {
        SDL_Log("verify: out of memory");
        e->verify = false;
        if (!run_native(pc, e))
            return false;
        e->calls++;
        c->cycles += e->cycles ? e->cycles : NATIVE_CALL_CYCLES;
        return true;
    }
    e->calls++;

    /* 1. snapshot */
    Cpu8086 cpu_pre = *c;
    dev_save(pc, &dev_pre);
    int speaker_pre = pc->speaker_count;

    /* 2. original, logged */
    verifying = true;
    log_orig.count = 0;
    log_orig.overflow = false;
    c->write_log = &log_orig;
    bool returned = run_original(pc, e);
    c->write_log = NULL;
    verifying = false;
    if (!returned || log_orig.overflow) {
        e->cap_hits++;
        e->mismatches++;
        e->verify = false;
        SDL_Log("verify %s: call %llu: %s; verification of this entry stopped", e->name,
                (unsigned long long)e->calls,
                !returned ? "step cap hit before return" : "write log overflow");
        return true; /* keep the original's (partial) progress */
    }

    /* 3. save the original's result, then undo its writes */
    Cpu8086 cpu_orig = *c;
    uint64_t used = cpu_orig.cycles - cpu_pre.cycles;
    if (!e->orig_cycles_max || used < e->orig_cycles_min)
        e->orig_cycles_min = used;
    if (used > e->orig_cycles_max)
        e->orig_cycles_max = used;
    dev_save(pc, &dev_orig);
    int speaker_new = pc->speaker_count - speaker_pre;
    if (speaker_new > 0)
        SDL_memcpy(speaker_orig, pc->speaker + speaker_pre, (size_t)speaker_new * sizeof(SpeakerEvent));
    orig_count = 0;
    for (uint32_t i = 0; i < log_orig.count; i++) {
        uint32_t a = log_orig.addr[i];
        if (!mark[a]) {
            mark[a] = 1;
            orig_addr[orig_count] = a;
            orig_post[orig_count++] = pc->mem[a];
        }
    }
    log_undo(pc, &log_orig);

    /* 4. native from the same state */
    *c = cpu_pre;
    dev_load(pc, &dev_pre);
    log_native.count = 0;
    log_native.overflow = false;
    c->write_log = &log_native;
    bool ran = run_native(pc, e);
    c->write_log = NULL;

    /* 5. compare (nothing to compare when the native declined: the original's result stays) */
    char what[160] = "";
    if (!ran)
        e->declines++;
    static const char *rn[8] = { "AX", "CX", "DX", "BX", "SP", "BP", "SI", "DI" };
    static const char *sn[4] = { "ES", "CS", "SS", "DS" };
    for (int r = 0; r < 8 && ran && !what[0]; r++)
        if (c->regs[r] != cpu_orig.regs[r])
            SDL_snprintf(what, sizeof what, "%s original %04X native %04X", rn[r], cpu_orig.regs[r], c->regs[r]);
    for (int r = 0; r < 4 && ran && !what[0]; r++)
        if (c->sregs[r] != cpu_orig.sregs[r])
            SDL_snprintf(what, sizeof what, "%s original %04X native %04X", sn[r], cpu_orig.sregs[r], c->sregs[r]);
    if (ran && !what[0] && c->ip != cpu_orig.ip)
        SDL_snprintf(what, sizeof what, "IP original %04X native %04X", cpu_orig.ip, c->ip);
    if (ran && !what[0] && ((c->flags ^ cpu_orig.flags) & e->flag_mask))
        SDL_snprintf(what, sizeof what, "flags original %04X native %04X (mask %04X)", cpu_orig.flags, c->flags,
                     e->flag_mask);
    if (ran && !what[0] && e->exact_cycles) {
        uint64_t charged = c->cycles - cpu_pre.cycles + (e->cycles ? e->cycles : NATIVE_CALL_CYCLES);
        if (charged != used)
            SDL_snprintf(what, sizeof what, "cycles original %llu native %llu", (unsigned long long)used,
                         (unsigned long long)charged);
    }
    if (log_native.overflow && !what[0])
        SDL_snprintf(what, sizeof what, "native write log overflow");
    for (uint32_t i = 0; i < orig_count && !what[0]; i++) {
        uint32_t a = orig_addr[i];
        if (pc->mem[a] != orig_post[i] && !ignored(e, a))
            SDL_snprintf(what, sizeof what, "memory %05X original %02X native %02X", a, orig_post[i], pc->mem[a]);
    }
    for (uint32_t i = 0; i < log_native.count; i++) {
        uint32_t a = log_native.addr[i];
        if (mark[a])
            continue; /* written by the original (checked above) or already seen */
        mark[a] = 2;
        if (!what[0] && pc->mem[a] != log_native.old[i] && !ignored(e, a))
            SDL_snprintf(what, sizeof what, "memory %05X original %02X native %02X", a, log_native.old[i],
                         pc->mem[a]);
    }
    if (what[0]) {
        e->mismatches++;
        if (e->mismatches <= VERIFY_MAX_REPORTS)
            SDL_Log("verify %s: call %llu at %04X:%04X: %s", e->name, (unsigned long long)e->calls, e->seg, e->off,
                    what);
    }

    /* 6. keep the original's result */
    for (uint32_t i = 0; i < log_native.count; i++)
        mark[log_native.addr[i]] = 0;
    log_undo(pc, &log_native);
    for (uint32_t i = 0; i < orig_count; i++) {
        pc->mem[orig_addr[i]] = orig_post[i];
        mark[orig_addr[i]] = 0;
    }
    *c = cpu_orig;
    dev_load(pc, &dev_orig);
    if (speaker_new > 0)
        SDL_memcpy(pc->speaker + speaker_pre, speaker_orig, (size_t)speaker_new * sizeof(SpeakerEvent));
    return true;
}

static bool pre_exec(void *ctx, Cpu8086 *c)
{
    Pc *pc = ctx;
    if (verifying)
        return false;
    NativeEntry *e = entries[hook_index[c->ip] - 1];
    if (e->verify)
        return verify_call(pc, e);
    if (!run_native(pc, e)) {
        e->declines++;
        return false; /* the CPU executes the original instruction */
    }
    e->calls++;
    c->cycles += e->cycles ? e->cycles : NATIVE_CALL_CYCLES;
    return true;
}

/* ---- C scheduler (3.22) ---------------------------------------------------------- */

bool native_step(Pc *pc)
{
    Cpu8086 *c = &pc->cpu;
    if (!c->hook_map || c->sregs[S_CS] != GAME_CS || !c->hook_map[c->ip])
        return false;
    return pre_exec(pc, c);
}

int native_or_cpu_step(Pc *pc)
{
    Cpu8086 *c = &pc->cpu;
    if (pc->csched && !verifying && c->sregs[S_CS] == GAME_CS && hook_index[c->ip]) {
        NativeEntry *e = entries[hook_index[c->ip] - 1];
        if (e->enabled && !running_itself(pc, e)) {
            const uint8_t *map = c->hook_map;
            uint64_t start = c->cycles;
            c->hook_map = hook_map; /* the native's own calls out see the natives */
            bool ran = run_native(pc, e);
            c->hook_map = map;
            if (ran) {
                e->calls++;
                c->cycles += e->cycles ? e->cycles : NATIVE_CALL_CYCLES;
                pc->sched_steps++;
                return (int)(c->cycles - start);
            }
            e->declines++;
        }
    }
    return cpu_step(c);
}

/* ---- setup and reporting -------------------------------------------------------- */

bool native_call(Pc *pc, uint16_t off)
{
    if (verifying || !hook_index[off] || !g_pc || !pc->cpu.hook_map)
        return false;
    NativeEntry *e = entries[hook_index[off] - 1];
    if (!e->enabled || e->seg != GAME_CS)
        return false;
    Cpu8086 *c = &pc->cpu;
    uint16_t ip = c->ip, cs = c->sregs[S_CS];
    c->ip = off;
    c->sregs[S_CS] = GAME_CS;
    if (!run_native(pc, e)) {
        e->declines++;
        c->ip = ip;
        c->sregs[S_CS] = cs;
        return false;
    }
    e->calls++;
    c->cycles += e->cycles ? e->cycles : NATIVE_CALL_CYCLES;
    return true;
}

void native_init(Pc *pc)
{
    collect_entries();
    g_pc = pc;
    pc->cpu.hook_seg = GAME_CS;
    pc->cpu.pre_exec = pre_exec;
    pc->cpu.hook_ctx = pc;
    rebuild_map();
}

static bool for_name(const char *name, bool (*fn)(NativeEntry *, bool), bool arg)
{
    collect_entries();
    bool all = SDL_strcmp(name, "all") == 0, found = false;
    for (int i = 0; i < ENTRY_COUNT; i++) {
        if (all || SDL_strcmp(entries[i]->name, name) == 0) {
            fn(entries[i], arg);
            found = true;
        }
    }
    rebuild_map();
    return found;
}

static bool set_enabled(NativeEntry *e, bool on)
{
    e->enabled = on;
    return true;
}

static bool set_verify(NativeEntry *e, bool on)
{
    e->verify = on;
    return true;
}

bool native_set_enabled(const char *name, bool enabled)
{
    return for_name(name, set_enabled, enabled);
}

bool native_set_verify(const char *name)
{
    return for_name(name, set_verify, true);
}

void native_list(void)
{
    collect_entries();
    SDL_Log("%-24s %-9s %-8s %s", "name", "address", "default", "flags");
    for (int i = 0; i < ENTRY_COUNT; i++)
        SDL_Log("%-24s %04X:%04X %-8s %04X", entries[i]->name, entries[i]->seg, entries[i]->off,
                entries[i]->enabled ? "on" : "off", entries[i]->flag_mask);
}

void native_verify_summary(void)
{
    collect_entries();
    for (int i = 0; i < ENTRY_COUNT; i++) {
        const NativeEntry *e = entries[i];
        if (!e->verify && !e->cap_hits)
            continue;
        if (!e->enabled)
            SDL_Log("verify-summary %s calls 0 mismatches 0 (disabled)", e->name);
        else
            SDL_Log("verify-summary %s calls %llu mismatches %llu original-cycles %llu..%llu declined %llu", e->name,
                    (unsigned long long)e->calls, (unsigned long long)e->mismatches,
                    (unsigned long long)e->orig_cycles_min, (unsigned long long)e->orig_cycles_max,
                    (unsigned long long)e->declines);
    }
}

uint64_t native_total_calls(void)
{
    collect_entries();
    uint64_t n = 0;
    for (int i = 0; i < ENTRY_COUNT; i++)
        n += entries[i]->calls;
    return n;
}

void native_shutdown(void)
{
    SDL_free(mark);
    SDL_free(orig_addr);
    SDL_free(orig_post);
    SDL_free(log_orig.addr);
    SDL_free(log_orig.old);
    SDL_free(log_native.addr);
    SDL_free(log_native.old);
    mark = NULL;
    orig_addr = NULL;
    orig_post = NULL;
    log_orig = log_native = (CpuWriteLog){ 0 };
    if (g_pc)
        g_pc->cpu.hook_map = NULL;
    g_pc = NULL;
}
