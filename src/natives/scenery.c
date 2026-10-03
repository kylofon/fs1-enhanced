/* Natives for subphase 3.8 scenery interpreter (see docs/PHASE3_PLAN.md,
 * docs/SCENERY_FORMAT.md and docs/subphases/3.8.md).
 *
 * The interpreter (0050:3CC0) and every opcode handler it dispatches to are translated label
 * for label (labels Lxxxx = 0050:xxxx). The translation runs on the emulated CPU's register
 * file and stack, so every register, every memory byte and every stack word below SP ends as
 * the original leaves it (--verify compares all of them). Each translated instruction adds
 * the cycles the emulator charges for it, so a scenery pass costs what the original costs.
 *
 * Calls out of the interpreter:
 * - to routines another area replaces (world_to_eye_delta, rotate_point, the outcodes and
 *   clipping steps, project_dot, draw_line, draw_horizon_list, build_view_matrix,
 *   clear_view_buffer): the CALL is emulated (return address on the stack) and that
 *   native's C function is called directly, charging its .cycles;
 * - to original code with no native (atis_start 4FBF, cga_program_regs 5600), or to a
 *   native turned off with --native-off: the routine runs as original code until it returns.
 * - Paths the original cannot finish (opcodes 03, 0F, 42 and the unassigned ones hang; a
 *   point exactly at the eye hangs in world_to_eye_delta) hand over to the original code at
 *   that point: the CPU is left exactly where the original would be, and the native returns
 *   without its RET, so the game continues (and hangs) in original code. */
#include <setjmp.h>
#include <stdbool.h>

#include "native.h"
#include "game/state.h"



/* ---- CPU access --------------------------------------------------------------------- */

static Pc *P;
static Cpu8086 *C;
static jmp_buf bail_jb;

#define rAX (C->regs[R_AX])
#define rBX (C->regs[R_BX])
#define rCX (C->regs[R_CX])
#define rDX (C->regs[R_DX])
#define rSI (C->regs[R_SI])
#define rDI (C->regs[R_DI])
#define rBP (C->regs[R_BP])
#define rSP (C->regs[R_SP])
#define sDS (C->sregs[S_DS])
#define sES (C->sregs[S_ES])
#define sSS (C->sregs[S_SS])

static inline uint8_t lo8(uint16_t v) { return (uint8_t)v; }
static inline uint8_t hi8(uint16_t v) { return (uint8_t)(v >> 8); }
static inline void set_lo(uint16_t *r, uint8_t v) { *r = (uint16_t)((*r & 0xFF00) | v); }
static inline void set_hi(uint16_t *r, uint8_t v) { *r = (uint16_t)((*r & 0x00FF) | v << 8); }
#define AL lo8(rAX)
#define AH hi8(rAX)
#define BL lo8(rBX)
#define BH hi8(rBX)
#define CL lo8(rCX)
#define CH hi8(rCX)
#define DL lo8(rDX)
#define DH hi8(rDX)

static inline uint16_t sar1(uint16_t v) { return (uint16_t)((v >> 1) | (v & 0x8000)); }
static inline bool neg16(uint16_t v) { return (v & 0x8000) != 0; }

/* DS / ES relative memory, as the original addresses it */
static inline uint8_t rd8(uint16_t o) { return cpu_read8(C, cpu_linear(sDS, o)); }
static inline uint16_t rd16(uint16_t o) { return mem_read16(P, sDS, o); }
static inline void wr8(uint16_t o, uint8_t v) { cpu_write8(C, cpu_linear(sDS, o), v); }
static inline void wr16(uint16_t o, uint16_t v) { mem_write16(P, sDS, o, v); }
static inline uint8_t es_rd8(uint16_t o) { return cpu_read8(C, cpu_linear(sES, o)); }
static inline void es_wr8(uint16_t o, uint8_t v) { cpu_write8(C, cpu_linear(sES, o), v); }
static inline void es_wr16(uint16_t o, uint16_t v) { mem_write16(P, sES, o, v); }

/* ---- cycles: what the emulator (cpu8086.c) charges per instruction form ---------------
 * r = register, m = memory (effective address +7), i = immediate, a = AL/AX with a direct
 * address (A0-A3). Conditional jumps are 16 taken, 4 not taken. */
enum {
    MOV_RM = 17, MOV_MR = 18, MOV_RR = 4, MOV_AM = 12, MOV_RI = 4, MOV_MI = 19,
    ALU_RM = 18, ALU_RR = 5, ALU_AI = 6, ALU_MI = 19, ALU_RI = 6,
    TEST_MI = 14, INC_R = 2, INC_M = 12, INC_R8 = 5, NEG_R = 5,
    JMP = 17, CALL = 21, CALL_R = 23, JMP_M = 20, RET = 20,
    PUSH_R = 15, POP_R = 12, PUSH_S = 16, POP_S = 14, POP_M = 26,
    LODS = 15, STOS = 12, SH1_R = 8, SH1_M = 15, CWD_ = 5, FLAG_OP = 2,
    XCHG_RM = 26, MOV_SR = 4, MOV_SM = 11, MOV_RS = 4,
    IMUL_M16 = 137, IMUL_M8 = 89, IDIV_R = 167, DIV_R = 146,
    LOOP_T = 17, LOOP_N = 5, INT0 = 53, STI_ = 2, IRET = 34,
};
#define CY(n) (C->cycles += (uint64_t)(n))

static inline bool J(bool taken)
{
    CY(taken ? 16 : 4);
    return taken;
}

/* ---- stack, calls, hand-over ----------------------------------------------------------- */

static inline void push(uint16_t v)
{
    rSP = (uint16_t)(rSP - 2);
    mem_write16(P, sSS, rSP, v);
}

static inline uint16_t pop(void)
{
    uint16_t v = mem_read16(P, sSS, rSP);
    rSP = (uint16_t)(rSP + 2);
    return v;
}

static inline void ret_(void)
{
    C->ip = pop();
    CY(RET);
}

/* Leaves the native: the CPU continues as original code at CS:ip with the current state. */
static _Noreturn void bail_at(uint16_t ip)
{
    C->ip = ip;
    C->sregs[S_CS] = GAME_CS;
    longjmp(bail_jb, 1);
}

/* Runs original code (natives off) until it returns to GAME_CS:ret with SP == sp_end. */
static void run_orig(uint16_t ret, uint16_t sp_end)
{
    const uint8_t *map = C->hook_map;
    C->hook_map = NULL;
    for (long n = 0; !(C->ip == ret && C->sregs[S_CS] == GAME_CS && rSP == sp_end); n++) {
        if (n == 20000000L) { /* it hangs (as the original does): stay in original code */
            C->hook_map = map;
            longjmp(bail_jb, 1);
        }
        native_or_cpu_step(P);
    }
    C->hook_map = map;
}

/* Natives of the other areas, found by address (enabled ones only; the last one wins,
 * as in native.c). Enabling only changes at startup, so lookups are cached. */
#define NATIVE_AREA(area) extern NativeEntry native_##area[];
#include "areas.h"
#undef NATIVE_AREA

static NativeEntry *find_native(uint16_t off)
{
    static uint16_t key[32];
    static NativeEntry *val[32];
    static int n;
    for (int i = 0; i < n; i++)
        if (key[i] == off)
            return val[i];
    NativeEntry *tables[] = {
#define NATIVE_AREA(area) native_##area,
#include "areas.h"
#undef NATIVE_AREA
    };
    NativeEntry *found = NULL;
    for (size_t t = 0; t < sizeof tables / sizeof tables[0]; t++) {
        if (tables[t] == native_scenery)
            continue;
        for (NativeEntry *e = tables[t]; e->name; e++)
            if (e->enabled && e->seg == GAME_CS && e->off == off)
                found = e;
    }
    if (n < 32) {
        key[n] = off;
        val[n++] = found;
    }
    return found;
}

/* The routine at `target` runs with its return address already on the stack (after an
 * emulated CALL, or for a tail JMP), until it returns. */
static void ext(uint16_t target)
{
    uint16_t sp = rSP, ret = mem_read16(P, sSS, sp);
    NativeEntry *e = find_native(target);
    C->ip = target;
    C->sregs[S_CS] = GAME_CS;
    if (e) {
        e->fn(P);
        CY(e->cycles ? e->cycles : NATIVE_CALL_CYCLES);
    }
    if (!(C->ip == ret && C->sregs[S_CS] == GAME_CS && rSP == (uint16_t)(sp + 2)))
        run_orig(ret, (uint16_t)(sp + 2));
}

static void call_ext(uint16_t target, uint16_t ret)
{
    push(ret);
    CY(CALL);
    ext(target);
}

static void call_int(uint16_t ret, void (*fn)(void))
{
    push(ret);
    CY(CALL);
    fn();
}

/* ---- flags for the INT 0 path ------------------------------------------------------------
 * Only needed for the FLAGS word a divide error pushes. */
#define ARITH_FLAGS (F_CF | F_PF | F_AF | F_ZF | F_SF | F_OF)

static uint16_t szp(uint16_t v)
{
    uint8_t b = (uint8_t)v;
    b ^= b >> 4;
    b ^= b >> 2;
    b ^= b >> 1;
    return (uint16_t)((v == 0 ? F_ZF : 0) | (v & 0x8000 ? F_SF : 0) | (b & 1 ? 0 : F_PF));
}

static uint16_t flags_with(uint16_t arith) { return (uint16_t)((C->flags & ~ARITH_FLAGS) | arith); }

/* After xor ax,ax / sar dx,1 / rcr ax,1 (dx0 = DX before the sar). */
static uint16_t flags_proj(uint16_t dx0)
{
    return flags_with((uint16_t)(szp(sar1(dx0)) | (dx0 & 1 ? F_OF : 0)));
}

/* After sub a,b (16-bit). */
static uint16_t flags_sub(uint16_t a, uint16_t b)
{
    uint16_t r = (uint16_t)(a - b);
    return flags_with((uint16_t)(szp(r) | (a < b ? F_CF : 0) | (((a ^ b) & (a ^ r) & 0x8000) ? F_OF : 0) |
                                 (((a ^ b ^ r) & 0x10) ? F_AF : 0)));
}

/* After xor ax,ax. */
static uint16_t flags_zero(void) { return flags_with(F_ZF | F_PF); }

/* A divide error at div_ip (next instruction next_ip): INT 0 into the game's handler
 * int_4DC6, which sets AX = +-7FFF by the sign of DX^BX and IRETs. */
static void int0(uint16_t div_ip, uint16_t next_ip, uint16_t flags)
{
    if (mem_read16(P, 0, 0) != 0x4DC6 || mem_read16(P, 0, 2) != GAME_CS)
        bail_at(div_ip); /* someone else's INT 0 handler: let the original run it */
    CY(INT0);
    uint16_t sp = rSP;
    push(flags);
    push(GAME_CS);
    push(next_ip);
    bool negative = neg16((uint16_t)(rDX ^ rBX));
    rAX = negative ? 0x8001 : 0x7FFF;
    CY(MOV_RR + ALU_RR + MOV_RI + (negative ? 4 + NEG_R : 16) + STI_ + IRET);
    rSP = sp;
}

/* idiv r16 at div_ip */
static void idiv(uint16_t divisor, uint16_t div_ip, uint16_t next_ip, uint16_t flags)
{
    int32_t n = (int32_t)((uint32_t)rDX << 16 | rAX);
    int32_t d = (int16_t)divisor;
    if (d == 0 || n / d > 32767 || n / d < -32768) {
        int0(div_ip, next_ip, flags);
        return;
    }
    rAX = (uint16_t)(n / d);
    rDX = (uint16_t)(n % d);
    CY(IDIV_R);
}

/* div r16 at div_ip */
static void udiv(uint16_t divisor, uint16_t div_ip, uint16_t next_ip, uint16_t flags)
{
    uint32_t n = (uint32_t)rDX << 16 | rAX;
    if (divisor == 0 || n / divisor > 0xFFFF) {
        int0(div_ip, next_ip, flags);
        return;
    }
    rAX = (uint16_t)(n / divisor);
    rDX = (uint16_t)(n % divisor);
    CY(DIV_R);
}

/* ---- string instructions (DS:SI / ES:DI, direction flag respected) ------------------------ */

static inline int16_t dstep(int16_t n) { return (C->flags & F_DF) ? (int16_t)-n : n; }
static inline void lodsw_(void)
{
    rAX = rd16(rSI);
    rSI = (uint16_t)(rSI + dstep(2));
    CY(LODS);
}
static inline void lodsb_(void)
{
    set_lo(&rAX, rd8(rSI));
    rSI = (uint16_t)(rSI + dstep(1));
    CY(LODS);
}
static inline void stosw_(void)
{
    es_wr16(rDI, rAX);
    rDI = (uint16_t)(rDI + dstep(2));
    CY(STOS);
}
static inline void stosb_(void)
{
    es_wr8(rDI, AL);
    rDI = (uint16_t)(rDI + dstep(1));
    CY(STOS);
}

/* small instruction helpers */
static inline void mov_ax_m(uint16_t o) { rAX = rd16(o); CY(MOV_AM); }
static inline void mov_m_ax(uint16_t o) { wr16(o, rAX); CY(MOV_AM); }
static inline void mov_al_m(uint16_t o) { set_lo(&rAX, rd8(o)); CY(MOV_AM); }
static inline void mov_m_al(uint16_t o) { wr8(o, AL); CY(MOV_AM); }
static inline void add_ip(uint16_t v) { wr16(0x30C2, (uint16_t)(rd16(0x30C2) + v)); } /* add [scenery_ip],v */
static inline void inc_ip(void) { add_ip(1); CY(INC_M); }
static inline bool dec_m8(uint16_t o) /* dec byte [o]; result != 0 */
{
    uint8_t v = (uint8_t)(rd8(o) - 1);
    wr8(o, v);
    CY(INC_M);
    return v != 0;
}

/* ---- scenery_ip and points ------------------------------------------------------------------ */


/* 0050:47E3: world_to_eye_delta (point at [BX+1]), scenery_ip = after the point, then 47EA:
 * rotate_point into point 1 (310F). */
static void f_47E3(void)
{
    call_ext(0x48E8, 0x47E6);
    gs_set_scenery_ip(P, rSI);
    CY(MOV_MR);
    call_ext(0x481F, 0x47ED);
    gs_set_eye_p1_w(P, 0, rBX);
    gs_set_eye_p1_w(P, 1, rCX);
    gs_set_eye_p1_w(P, 2, rDX);
    CY(3 * MOV_MR);
    ret_();
}

/* 0050:47DD: 47E3, then JMP outcode_p1 */
static void f_47DD(void)
{
    call_int(0x47E0, f_47E3);
    CY(JMP);
    ext(0x4A1A);
}

/* 0050:47FA: the same into point 2 (311B) and its copy (3127), then JMP outcode_p2 */
static void f_47FA(void)
{
    call_ext(0x48E8, 0x47FD);
    gs_set_scenery_ip(P, rSI);
    CY(MOV_MR);
    call_ext(0x481F, 0x4804);
    gs_set_eye_p2_w(P, 0, rBX);
    gs_set_eye_p3_w(P, 0, rBX);
    gs_set_eye_p2_w(P, 1, rCX);
    gs_set_eye_p3_w(P, 1, rCX);
    gs_set_eye_p2_w(P, 2, rDX);
    gs_set_eye_p3_w(P, 2, rDX);
    CY(6 * MOV_MR);
    CY(JMP);
    ext(0x4A4E);
}

/* ---- projection and clip_project_line --------------------------------------------------- */

/* One axis of 4D18/4D56: DX = coordinate, BX = z; IDIV, then AH = (q.hi * scale * 2).hi +
 * centre. Returns AH. */
static uint8_t project_axis(uint16_t v_off, uint16_t z_off, uint16_t scale, uint16_t centre, uint16_t div_ip)
{
    rDX = rd16(v_off);
    rBX = rd16(z_off);
    CY(2 * MOV_RM);
    rAX = 0;
    CY(ALU_RR);
    uint16_t dx0 = rDX;
    rAX = (dx0 & 1) ? 0x8000 : 0; /* sar dx,1 / rcr ax,1 */
    rDX = sar1(dx0);
    CY(2 * SH1_R);
    idiv(rBX, div_ip, (uint16_t)(div_ip + 2), flags_proj(dx0));
    set_lo(&rAX, AH);
    CY(MOV_RR);
    rAX = (uint16_t)((int16_t)(int8_t)AL * (int8_t)rd8(scale));
    CY(IMUL_M8);
    rAX = (uint16_t)(rAX << 1);
    CY(SH1_R);
    set_hi(&rAX, (uint8_t)(AH + rd8(centre)));
    CY(ALU_RM);
    return AH;
}

/* 0050:4D56: point 2 projected; the line from SI (screen point 1) is drawn, or captured. */
static void f_4D56(void)
{
    gs_set_move_pending(P, 0);
    CY(MOV_MI);
    set_lo(&rCX, project_axis(0x311B, 0x311F, 0x3167, 0x3169, 0x4D69));
    CY(MOV_RR);
    set_hi(&rCX, project_axis(0x311D, 0x311F, 0x3168, 0x316A, 0x4D87));
    CY(MOV_RR);
    wr16(0x3165, rCX);
    CY(MOV_MR);
    CY(TEST_MI);
    if (!J(gs_capture_mode(P) == 0)) {
        rBX = gs_capture_ptr(P);
        CY(MOV_RM);
        wr16(rBX, rSI);
        wr16((uint16_t)(rBX + 2), rCX);
        CY(2 * MOV_MR);
        gs_set_capture_ptr(P, (uint16_t)(rd16(GS_CAPTURE_PTR) + 4));
        CY(ALU_MI);
        ret_();
        return;
    }
    rBX = 0;
    CY(ALU_RR);
    set_lo(&rBX, CH);
    rBP = rBX;
    rAX = rSI;
    set_lo(&rBX, AL);
    rSI = rBX;
    set_lo(&rBX, AH);
    rDI = rBX;
    set_lo(&rBX, CL);
    CY(8 * MOV_RR);
    CY(JMP);
    ext(0x5691); /* draw_line */
}

/* 0050:4D18: both points projected */
static void f_4D18(void)
{
    set_lo(&rCX, project_axis(0x310F, 0x3113, 0x3167, 0x3169, 0x4D26));
    CY(MOV_RR);
    set_hi(&rCX, project_axis(0x3111, 0x3113, 0x3168, 0x316A, 0x4D44));
    CY(MOV_RR);
    rSI = rCX;
    CY(MOV_RR);
    f_4D56();
}

/* 0050:4CA3: point 1 is the previous line's end (screen word [3165]) */
static void f_4CA3(void)
{
    rCX = rd16(0x3165);
    CY(MOV_RM);
    rSI = rCX;
    CY(MOV_RR);
    CY(JMP);
    f_4D56();
}

/* 0050:40C8 clip_project_line: Cohen-Sutherland on [315E]/[315F] with clip_p1_plane and
 * clip_line_planes (at most [3162] steps), then project and draw. Clears [3163]. */
static void f_clip_project_line(void)
{
    wr8(0x3164, 1);
    CY(MOV_MI);
    rCX = rd16(GS_OUTCODE1);
    CY(MOV_RM);
    set_hi(&rCX, CH & CL);
    CY(ALU_RR);
    if (J(CH != 0))
        goto L412F;
    CY(ALU_RR);
    if (J(CL != 0))
        goto L40F3;
    gs_set_clip_steps(P, 5);
    CY(MOV_MI);
L40DE:
    set_hi(&rCX, gs_outcode2(P));
    CY(MOV_RM + ALU_RR);
    if (J(CH != 0))
        goto L410F;
    CY(TEST_MI);
    if (J(gs_move_pending(P) & 1)) {
        CY(JMP);
        f_4D18();
        return;
    }
    CY(JMP);
    f_4CA3();
    return;
L40F3:
    gs_set_clip_steps(P, 0x0A);
    CY(MOV_MI);
L40F8:
    call_ext(0x4AF7, 0x40FB); /* clip_p1_plane */
    rCX = rd16(GS_OUTCODE1);
    CY(MOV_RM + ALU_RR);
    if (J(CL == 0))
        goto L411B;
    set_hi(&rCX, CH & CL);
    CY(ALU_RR);
    if (J(CH != 0))
        goto L412F;
    if (J(dec_m8(0x3162)))
        goto L40F8;
    CY(16); /* je 412F */
    goto L412F;
L410F:
    call_ext(0x4BCD, 0x4112); /* clip_line_planes */
    if (J(dec_m8(0x3162)))
        goto L40DE;
    CY(JMP);
    goto L412F;
L411B:
    set_hi(&rCX, gs_outcode2(P));
    CY(MOV_RM + ALU_RR);
    if (!J(CH != 0)) {
        CY(JMP);
        f_4D18();
        return;
    }
    call_ext(0x4BCD, 0x4129);
    if (J(dec_m8(0x3162)))
        goto L411B;
L412F:
    gs_set_move_pending(P, 0);
    CY(MOV_MI);
    ret_();
}

/* ---- opcode handlers (entered by CALL CX with BX = opcode address) --------------------------- */

/* 00 dot (3D17): 47E3, then falls into project_dot */
static void h_dot(void)
{
    call_int(0x3D1A, f_47E3);
    ext(0x3D1A);
}

/* 01 move (3D8E); 40 move_flat (3D8A) */
static void h_move(void)
{
    call_int(0x3D91, f_47DD);
    for (uint16_t i = 0; i < 6; i += 2) {
        mov_ax_m((uint16_t)(0x310F + i));
        mov_m_ax((uint16_t)(0x3133 + i));
    }
    mov_al_m(0x315E);
    mov_m_al(0x3161);
    gs_set_move_pending(P, 1); /* L3DA9 */
    CY(MOV_MI);
    ret_();
}

static void h_move_flat(void)
{
    gs_set_flat_flag(P, (uint8_t)(gs_flat_flag(P) + 1));
    CY(INC_M);
    h_move();
}

/* line start = previous line end (3127 -> 310F, [3160] -> [315E]) unless a move is pending */
static void line_continue(void)
{
    CY(TEST_MI);
    if (J(gs_move_pending(P) != 0))
        return;
    for (uint16_t i = 0; i < 6; i += 2) {
        mov_ax_m((uint16_t)(0x3127 + i));
        mov_m_ax((uint16_t)(0x310F + i));
    }
    mov_al_m(0x3160);
    mov_m_al(0x315E);
}

/* 02 line (3DB3); 41 line_flat (3DAF) */
static void h_line(void)
{
    line_continue();
    call_int(0x3DD5, f_47FA);
    mov_m_al(0x3160);
    CY(JMP);
    f_clip_project_line();
}

static void h_line_flat(void)
{
    gs_set_flat_flag(P, (uint8_t)(gs_flat_flag(P) + 1));
    CY(INC_M);
    h_line();
}

/* 29 close (3DDC): line back to the move start (3133) */
static void h_close(void)
{
    inc_ip();
    line_continue();
    for (uint16_t i = 0; i < 6; i += 2) {
        mov_ax_m((uint16_t)(0x3133 + i));
        mov_m_ax((uint16_t)(0x311B + i));
    }
    mov_al_m(0x3161);
    mov_m_al(0x315F);
    CY(JMP);
    f_clip_project_line();
}

/* 05 viewpoint (3E1A) */
static void h_viewpoint(void)
{
    static const uint16_t dst[6] = { 0x30EB, 0x30ED, 0x30EF, 0x30F1, 0x30F3, 0x30F5 };
    for (int i = 0; i < 6; i++) {
        rAX = rd16((uint16_t)(rBX + 1 + 2 * i));
        CY(MOV_RM);
        mov_m_ax(dst[i]);
    }
    call_ext(0x4621, 0x3E41); /* build_view_matrix */
    add_ip(0x0D);
    CY(ALU_MI);
    ret_();
}

/* 06 line2d (3E47): screen line from bytes, unclipped */
static void h_line2d(void)
{
    uint16_t bx = rBX;
    rBP = rd16((uint16_t)(bx + 2));
    rSI = rd16((uint16_t)(bx + 3));
    rDI = rd16((uint16_t)(bx + 4));
    rBX = rd16((uint16_t)(bx + 1));
    CY(4 * MOV_RM);
    rBP &= 0xFF;
    rSI &= 0xFF;
    rDI &= 0xFF;
    CY(3 * ALU_RI);
    set_hi(&rBX, 0);
    CY(MOV_RI);
    add_ip(5);
    CY(ALU_MI + JMP);
    ext(0x5691); /* draw_line */
}

/* 08 clear_view (3E69) */
static void h_clear_view(void)
{
    inc_ip();
    CY(JMP);
    ext(0x561E); /* clear_view_buffer */
}

/* 0B jump (3E70) */
static void h_jump(void)
{
    rAX = rd16((uint16_t)(rBX + 1));
    CY(MOV_RM);
    add_ip(rAX);
    CY(ALU_RM);
    ret_();
}

/* 0D capture (3E78) */
static void h_capture(void)
{
    rAX = rd16((uint16_t)(rBX + 1));
    CY(MOV_RM + ALU_RR);
    if (!J(rAX == 0)) {
        gs_set_capture_mode(P, 0xFF);
        CY(MOV_MI);
        mov_m_ax(GS_CAPTURE_PTR);
    } else {
        gs_set_capture_mode(P, 0);
        CY(MOV_MI);
    }
    add_ip(3);
    CY(ALU_MI);
    ret_();
}

/* 0E proj_scale (3E98) */
static void h_proj_scale(void)
{
    rAX = rd16((uint16_t)(rBX + 1));
    CY(MOV_RM);
    mov_m_ax(0x3167);
    rCX = rd16((uint16_t)(rBX + 3));
    CY(MOV_RM + ALU_RI);
    gs_set_proj_params_w(P, 1, rCX);
    CY(MOV_MR);
    add_ip(5);
    CY(ALU_MI);
    ret_();
}

/* 10 cga_regs (3EAF) */
static void h_cga_regs(void)
{
    inc_ip();
    CY(JMP);
    ext(0x5600); /* cga_program_regs */
}

/* 11 nop (3EB6) */
static void h_nop(void)
{
    inc_ip();
    ret_();
}

/* 12 colour (3EBB) */
static void h_colour(void)
{
    set_lo(&rAX, rd8((uint16_t)(rBX + 1)));
    CY(MOV_RM);
    set_hi(&rAX, (uint8_t)(AL << 4));
    CY(MOV_RR + 4 * SH1_R);
    mov_m_ax(0x30C8);
    set_lo(&rAX, AL | AH);
    set_hi(&rAX, AL);
    CY(ALU_RR + MOV_RR);
    mov_m_ax(0x3159);
    add_ip(2);
    CY(ALU_MI);
    ret_();
}

/* 2E colour_night (3ED8), 0C colour_dusk (3EEC) */
static void colour_if(uint16_t mask)
{
    CY(TEST_MI);
    if (!J((gs_time_of_day(P) & mask) == 0)) {
        rAX = rd16((uint16_t)(rBX + 1));
        CY(MOV_RM);
        mov_m_ax(0x30C8);
    }
    add_ip(3);
    CY(ALU_MI);
    ret_();
}

static void h_colour_night(void) { colour_if(4); }

static void h_colour_dusk(void)
{
    CY(JMP);
    colour_if(2);
}

/* 1B colour_dark_a (3EF4), 1C colour_dark_b (3F07) */
static void colour_dark(uint16_t colour)
{
    rAX = colour;
    CY(MOV_RI + TEST_MI);
    if (!J((gs_time_of_day(P) & 6) == 0))
        mov_m_ax(0x30C8);
    inc_ip();
    ret_();
}

static void h_colour_dark_a(void) { colour_dark(0x5005); }

static void h_colour_dark_b(void)
{
    CY(JMP);
    colour_dark(0x0002);
}

/* 25 store (3F0C) */
static void h_store(void)
{
    rSI = rd16((uint16_t)(rBX + 1));
    rCX = rd16((uint16_t)(rBX + 3));
    CY(2 * MOV_RM);
    wr16(rSI, rCX);
    CY(MOV_MR);
    add_ip(5);
    CY(ALU_MI);
    ret_();
}

/* 3F33: IP += rel */
static void skip_rel(void)
{
    rAX = rd16((uint16_t)(rBX + 1));
    CY(MOV_RM);
    add_ip(rAX);
    CY(ALU_RM);
    ret_();
}

/* one range test of 20/21/22: lo <= [v] <= hi (signed), record at BX+k */
static bool in_range(uint16_t k)
{
    rSI = rd16((uint16_t)(rBX + k));
    rAX = rd16((uint16_t)(rBX + k + 2));
    rCX = rd16((uint16_t)(rBX + k + 4));
    rDX = rd16(rSI);
    CY(4 * MOV_RM + ALU_RR);
    if (J((int16_t)rDX < (int16_t)rAX))
        return false;
    CY(ALU_RR);
    return !J((int16_t)rDX > (int16_t)rCX);
}

static void if_in(int n)
{
    for (int i = 0; i < n; i++) {
        if (!in_range((uint16_t)(3 + 6 * i))) {
            skip_rel();
            return;
        }
    }
    add_ip((uint16_t)(3 + 6 * n));
    CY(ALU_MI);
    ret_();
}

static void h_if_in(void) { if_in(1); }
static void h_if_in2(void) { if_in(2); }
static void h_if_in3(void) { if_in(3); }

/* 23 if_bits (3FA6) */
static void h_if_bits(void)
{
    rSI = rd16((uint16_t)(rBX + 3));
    rAX = rd16(rSI);
    CY(2 * MOV_RM);
    rAX &= rd16((uint16_t)(rBX + 5));
    CY(ALU_RM);
    if (J(rAX == 0)) {
        rAX = rd16((uint16_t)(rBX + 1)); /* L3FB6 */
        CY(MOV_RM);
        add_ip(rAX);
        CY(ALU_RM);
        ret_();
        return;
    }
    add_ip(7);
    CY(ALU_MI);
    ret_();
}

/* 28 jump_if (3FBE): through the je/jg/jl table at DS:3176 */
static void h_jump_if(void)
{
    uint16_t bx = rBX;
    rCX = rd16((uint16_t)(bx + 2));
    rSI = rd16(rd16((uint16_t)(bx + 4)));
    rDI = rd16(rd16((uint16_t)(bx + 6)));
    set_lo(&rBX, rd8((uint16_t)(bx + 1)));
    CY(6 * MOV_RM);
    set_hi(&rBX, 0);
    CY(ALU_RR + ALU_RR);
    uint16_t target = rd16((uint16_t)(rBX + 0x3176));
    int16_t a = (int16_t)rSI, b = (int16_t)rDI;
    bool cond;
    switch (target) {
    case 0x3FD6: cond = a == b; break;
    case 0x3FDB: cond = a > b; break;
    case 0x3FE0: cond = a < b; break;
    default: bail_at(0x3FD2); /* jmp [bx+3176] into something else */
    }
    CY(JMP_M);
    if (J(cond)) {
        add_ip(rCX); /* L3FE5 */
        CY(ALU_RM);
        ret_();
        return;
    }
    CY(JMP);
    add_ip(8); /* L3FEA */
    CY(ALU_MI);
    ret_();
}

/* 24 origin (3FF0): local frame; eye_pos = (pos - O) >> s, low words */
static void shift_405F(void)
{
    uint16_t target = rd16((uint16_t)(rDI + 0x316C));
    CY(JMP_M);
    switch (target) {
    case 0x4073: break;
    case 0x4063:
        for (int i = 0; i < 4; i++) {
            rAX = (uint16_t)(rAX >> 1 | (rDX & 1) << 15);
            rDX >>= 1;
        }
        CY(8 * SH1_R);
        break;
    case 0x4074:
        set_lo(&rAX, AH);
        set_hi(&rAX, DL);
        CY(2 * MOV_RR);
        break;
    case 0x4079:
        for (int i = 0; i < 4; i++) {
            rDX = (uint16_t)(rDX << 1 | rAX >> 15);
            rAX = (uint16_t)(rAX << 1);
        }
        rAX = rDX;
        CY(8 * SH1_R + MOV_RR);
        break;
    case 0x4089:
        rAX = rDX;
        CY(MOV_RR);
        break;
    default: bail_at(0x405F);
    }
    ret_();
}

static void origin_axis(uint16_t pos, uint16_t org, uint16_t eye, uint16_t ret)
{
    mov_ax_m(pos);
    rDX = rd16((uint16_t)(pos + 2));
    CY(MOV_RM);
    uint32_t v = ((uint32_t)rDX << 16 | rAX) - ((uint32_t)rd16((uint16_t)(org + 2)) << 16 | rd16(org));
    rAX = (uint16_t)v;
    rDX = (uint16_t)(v >> 16);
    CY(2 * ALU_RM);
    rDI = rd16(GS_SCENERY_SCALE);
    CY(MOV_RM);
    call_int(ret, shift_405F);
    mov_m_ax(eye);
}

static void h_origin(void)
{
    rSI = rBX;
    CY(MOV_RR);
    rSI++;
    CY(INC_R);
    lodsb_();
    mov_m_al(0x30DD);
    for (uint16_t i = 0; i < 12; i += 2) {
        lodsw_();
        mov_m_ax((uint16_t)(0x30DF + i));
    }
    gs_set_scenery_ip(P, rSI);
    CY(MOV_MR);
    origin_axis(0x30D1, 0x30DF, 0x30EB, 0x4029);
    origin_axis(0x30D5, 0x30E3, 0x30ED, 0x4042);
    origin_axis(0x30D9, 0x30E7, 0x30EF, 0x405B);
    ret_();
}

/* 31 cache_point (4140) and the slot helpers 415C/416D */
static void f_415C(void)
{
    rBX++;
    CY(INC_R);
    set_lo(&rCX, rd8(rBX));
    CY(MOV_RM);
    set_hi(&rCX, 0);
    CY(ALU_RR);
    rCX = (uint16_t)(rCX << 3);
    CY(3 * SH1_R);
    rBP = (uint16_t)(0x3288 + rCX);
    CY(MOV_RI + ALU_RR);
    ret_();
}

static void f_416D(void)
{
    call_int(0x4170, f_415C);
    rBX++;
    CY(INC_R);
    gs_set_scenery_ip(P, rBX);
    CY(MOV_MR);
    rSI = rBP;
    CY(MOV_RR);
    ret_();
}

static void h_cache_point(void)
{
    call_int(0x4143, f_415C);
    push(rBP);
    CY(PUSH_R);
    call_int(0x4147, f_47DD);
    rBP = pop();
    CY(POP_R);
    rSI = 0x310F;
    rBX = rBP;
    CY(MOV_RI + MOV_RR);
    wr8((uint16_t)(rBX + 6), AL);
    CY(MOV_MR);
    for (uint16_t i = 0; i < 6; i += 2) {
        lodsw_();
        wr16((uint16_t)(rBX + i), rAX);
        CY(MOV_MR);
    }
    ret_();
}

/* 32 move_cached (4178) */
static void h_move_cached(void)
{
    call_int(0x417B, f_416D);
    for (uint16_t i = 0; i < 6; i += 2) {
        lodsw_();
        mov_m_ax((uint16_t)(0x310F + i));
    }
    lodsb_();
    mov_m_al(0x315E);
    CY(JMP);
    gs_set_move_pending(P, 1); /* L3DA9 */
    CY(MOV_MI);
    ret_();
}

/* 33 line_cached (418E) */
static void h_line_cached(void)
{
    line_continue();
    call_int(0x41B0, f_416D);
    rCX = 3;
    rBX = 0x311B;
    CY(2 * MOV_RI);
    do {
        lodsw_();
        wr16(rBX, rAX);
        wr16((uint16_t)(rBX + 0x0C), rAX);
        CY(2 * MOV_MR);
        rBX = (uint16_t)(rBX + 2);
        CY(ALU_RI);
        rCX--;
        CY(rCX ? LOOP_T : LOOP_N);
    } while (rCX);
    lodsb_();
    mov_m_al(0x315F);
    mov_m_al(0x3160);
    CY(JMP);
    f_clip_project_line();
}

/* 34 skip1 (41CB) */
static void h_skip1(void)
{
    add_ip(2);
    CY(ALU_MI);
    ret_();
}

/* 35 dot_cached (41D1) */
static void h_dot_cached(void)
{
    call_int(0x41D4, f_416D);
    lodsw_();
    rBX = rAX;
    lodsw_();
    rCX = rAX;
    lodsw_();
    rBP = rAX;
    CY(3 * MOV_RR + JMP);
    ext(0x3D26); /* project_dot_regs */
}

/* ---- 2A dotted / 2B dashed: interpolated segments --------------------------------------------- */

/* 4234: point at DS:SI - eye_pos, scaled (CF -> bit queue [3180]), rotated into ES:DI */
static void f_4234(void)
{
    bool cf;
    lodsw_();
    uint16_t a = rAX, b = gs_eye_pos_w(P, 0);
    rAX = (uint16_t)(a - b);
    CY(ALU_RM);
    if (J(((a ^ b) & (a ^ rAX)) & 0x8000)) { /* L41E0: x overflow */
        cf = !(a < b);                        /* cmc */
        rAX = (uint16_t)(rAX >> 1 | (cf ? 0x8000 : 0));
        rAX = sar1(sar1(rAX));
        rBX = rAX;
        CY(FLAG_OP + 3 * SH1_R + MOV_RR);
        lodsw_();
        rDX = sar1(gs_eye_pos_w(P, 1));
        rAX = sar1(rAX);
        rAX = (uint16_t)(rAX - rDX);
        rAX = sar1(sar1(rAX));
        rCX = rAX;
        CY(MOV_RM + 2 * SH1_R + ALU_RR + 2 * SH1_R + MOV_RR + JMP);
        goto L420B;
    }
    rBX = rAX;
    CY(MOV_RR);
    lodsw_();
    a = rAX;
    b = gs_eye_pos_w(P, 1);
    rAX = (uint16_t)(a - b);
    CY(ALU_RM);
    if (J(((a ^ b) & (a ^ rAX)) & 0x8000)) { /* L41FC: y overflow */
        cf = !(a < b);
        rAX = (uint16_t)(rAX >> 1 | (cf ? 0x8000 : 0));
        rAX = sar1(sar1(rAX));
        rCX = rAX;
        rBX = sar1(sar1(sar1(rBX)));
        CY(FLAG_OP + 3 * SH1_R + MOV_RR + 3 * SH1_R);
        goto L420B;
    }
    rCX = rAX;
    CY(MOV_RR);
    lodsw_();
    a = rAX;
    b = gs_eye_pos_w(P, 2);
    rAX = (uint16_t)(a - b);
    CY(ALU_RM);
    if (J(((a ^ b) & (a ^ rAX)) & 0x8000)) { /* L421E: z overflow */
        cf = !(a < b);
        rAX = (uint16_t)(rAX >> 1 | (cf ? 0x8000 : 0));
        rAX = sar1(sar1(rAX));
        rCX = sar1(sar1(sar1(rCX)));
        rBX = sar1(sar1(sar1(rBX)));
        CY(FLAG_OP + 9 * SH1_R + FLAG_OP + JMP);
        cf = true;
        goto L4275;
    }
    {
        uint8_t dh = (uint8_t)(BH + 0x20);
        uint8_t dl = (uint8_t)(CH + 0x20);
        dh |= dl;
        dl = (uint8_t)(AH + 0x20);
        dh |= dl;
        dh &= 0xC0;
        rDX = (uint16_t)(dh << 8 | dl);
        CY(3 * (MOV_RR + ALU_RI) + 2 * ALU_RR + ALU_RI);
        if (!J(dh == 0)) {
            rBX = sar1(sar1(rBX));
            rCX = sar1(sar1(rCX));
            rAX = sar1(sar1(rAX));
            CY(6 * SH1_R + FLAG_OP + JMP);
            cf = true;
        } else {
            CY(FLAG_OP);
            cf = false;
        }
        goto L4275;
    }
L420B:
    lodsw_();
    rDX = sar1(gs_eye_pos_w(P, 2));
    rAX = sar1(rAX);
    rAX = (uint16_t)(rAX - rDX);
    rAX = sar1(sar1(rAX));
    CY(MOV_RM + 2 * SH1_R + ALU_RR + 2 * SH1_R + FLAG_OP + JMP);
    cf = true;
L4275:
    wr8(0x3180, (uint8_t)(rd8(0x3180) << 1 | cf)); /* rcl byte [3180],1 */
    CY(SH1_M);
    gs_set_xform_in_w(P, 0, rBX);
    gs_set_xform_in_w(P, 1, rCX);
    CY(2 * MOV_MR);
    mov_m_ax(0x313D);
    /* row x: imul [30FD], [3103], [3109] */
    static const uint16_t m[3][3] = { { 0x30FD, 0x3103, 0x3109 }, { 0x30FF, 0x3105, 0x310B },
                                      { 0x3101, 0x3107, 0x310D } };
    for (int r = 0; r < 3; r++) {
        int32_t p;
        if (r == 0) {
            rAX = rBX;
            CY(MOV_RR);
        } else {
            mov_ax_m(0x3139);
        }
        p = (int32_t)(int16_t)rAX * (int16_t)rd16(m[r][0]);
        rAX = (uint16_t)p;
        rDX = (uint16_t)((uint32_t)p >> 16);
        CY(IMUL_M16);
        rBP = rAX;
        CY(MOV_RR);
        if (r == 0) {
            rAX = rCX;
            rCX = rDX;
            CY(2 * MOV_RR);
        } else {
            rCX = rDX;
            CY(MOV_RR);
            mov_ax_m(0x313B);
        }
        p = (int32_t)(int16_t)rAX * (int16_t)rd16(m[r][1]);
        rAX = (uint16_t)p;
        rDX = (uint16_t)((uint32_t)p >> 16);
        CY(IMUL_M16);
        uint32_t s = (uint32_t)rBP + rAX;
        rBP = (uint16_t)s;
        rCX = (uint16_t)(rCX + rDX + (s >> 16));
        CY(2 * ALU_RR);
        mov_ax_m(0x313D);
        p = (int32_t)(int16_t)rAX * (int16_t)rd16(m[r][2]);
        rAX = (uint16_t)p;
        rDX = (uint16_t)((uint32_t)p >> 16);
        CY(IMUL_M16);
        s = (uint32_t)rAX + rBP;
        rAX = (uint16_t)s;
        rDX = (uint16_t)(rDX + rCX + (s >> 16));
        CY(2 * ALU_RR);
        if (r == 1) { /* shl ax,1 / rcl dx,1 */
            rDX = (uint16_t)(rDX << 1 | rAX >> 15);
            rAX = (uint16_t)(rAX << 1);
            CY(2 * SH1_R);
        } else if (r == 2) { /* sar dx,1 / rcr ax,1 */
            rAX = (uint16_t)(rAX >> 1 | (rDX & 1) << 15);
            rDX = sar1(rDX);
            CY(2 * SH1_R);
        }
        rAX = rDX;
        CY(MOV_RR);
        stosw_();
    }
    ret_();
}

/* 4465: rescale the endpoint whose bit in [3180] is clear (>> 3), unless both are clear */
static void f_4465(void)
{
    set_lo(&rDX, rd8(0x3180));
    CY(MOV_RM + ALU_RR);
    if (!J(DL != 0)) {
        ret_();
        return;
    }
    for (int k = 0; k < 2; k++) {
        bool bit = DL & 1;
        set_lo(&rDX, DL >> 1);
        CY(SH1_R);
        if (J(bit))
            continue;
        uint16_t base = k == 0 ? 0x3187 : 0x3181;
        for (uint16_t i = 0; i < 6; i += 2) {
            mov_ax_m((uint16_t)(base + i));
            rAX = sar1(sar1(sar1(rAX)));
            CY(3 * SH1_R);
            mov_m_ax((uint16_t)(base + i));
        }
    }
    ret_();
}

/* 43B4: both endpoints, n, and the 32-bit step (B -> A) / (n - 1); BX, CX, BP = B */
static void f_43B4(void)
{
    rAX = 0;
    CY(ALU_RR);
    sES = 0;
    CY(MOV_SR);
    sES = mem_read16(P, 0, 0x120);
    CY(MOV_SM);
    rSI = rBX;
    CY(MOV_RR);
    rSI++;
    CY(INC_R);
    rDI = 0x3181;
    CY(MOV_RI);
    call_int(0x43C6, f_4234);
    call_int(0x43C9, f_4234);
    rBX = rSI;
    CY(MOV_RR);
    set_hi(&rCX, 0);
    CY(ALU_RR);
    set_lo(&rCX, rd8(rBX));
    CY(MOV_RM);
    add_ip(0x0E);
    CY(ALU_MI);
    call_int(0x43D7, f_4465);
    wr8(0x31CD, CL);
    CY(MOV_MR);
    rCX--;
    CY(INC_R);
    rBX = 4;
    CY(MOV_RI);
    for (;;) { /* L43DF */
        rAX = rd16((uint16_t)(rBX + 0x3181));
        rDX = rd16((uint16_t)(rBX + 0x3187));
        CY(2 * MOV_RM);
        uint16_t f = flags_sub(rAX, rDX);
        rAX = (uint16_t)(rAX - rDX);
        CY(ALU_RR);
        wr16((uint16_t)(rBX + 0x31B5), rDX);
        CY(MOV_MR);
        wr16((uint16_t)(rBX + 0x31C1), 0);
        CY(MOV_MI);
        rDX = neg16(rAX) ? 0xFFFF : 0;
        CY(CWD_);
        idiv(rCX, 0x43F4, 0x43F6, f);
        wr16((uint16_t)(rBX + 0x31BB), rAX);
        CY(MOV_MR + ALU_RR);
        if (J(!neg16(rDX))) {
            rAX = 0; /* L440E */
            CY(ALU_RR);
            udiv(rCX, 0x4410, 0x4412, flags_zero());
        } else {
            rDX = (uint16_t)-rDX;
            CY(NEG_R);
            rAX = 0;
            CY(ALU_RR);
            udiv(rCX, 0x4402, 0x4404, flags_zero());
            bool borrow = rAX != 0;
            rAX = (uint16_t)-rAX;
            CY(NEG_R);
            uint16_t o = (uint16_t)(rBX + 0x31BB);
            wr16(o, (uint16_t)(rd16(o) - borrow));
            CY(ALU_MI + JMP);
        }
        wr16((uint16_t)(rBX + 0x31C7), rAX); /* L4412 */
        CY(MOV_MR);
        rBX = (uint16_t)(rBX - 2);
        CY(ALU_RI);
        if (!J(!neg16(rBX)))
            break;
    }
    rBX = rd16(0x31B5);
    rCX = rd16(0x31B7);
    rBP = rd16(0x31B9);
    CY(3 * MOV_RM);
    ret_();
}

/* 4428: one step: (31B5, 31B7, 31B9) . (31C1..) += (31BB..) . (31C7..); BX, CX, BP = point */
static void f_4428(void)
{
    static const uint16_t r[3] = { R_BX, R_CX, R_BP };
    for (int i = 0; i < 3; i++) {
        uint16_t k = (uint16_t)(2 * i);
        uint16_t v = rd16((uint16_t)(0x31C7 + k));
        uint32_t s = (uint32_t)rd16((uint16_t)(0x31C1 + k)) + v;
        wr16((uint16_t)(0x31C1 + k), (uint16_t)s);
        uint16_t w = (uint16_t)(rd16((uint16_t)(0x31B5 + k)) + rd16((uint16_t)(0x31BB + k)) + (s >> 16));
        wr16((uint16_t)(0x31B5 + k), w);
        C->regs[r[i]] = w;
        CY(MOV_RM + ALU_RM + MOV_RM + ALU_RM + MOV_MR);
    }
    ret_();
}

/* 2B dashed (44BF) */
static void h_dashed(void)
{
    call_int(0x44C2, f_43B4);
    CY(JMP);
    for (bool first = true;; first = false) {
        if (!first)
            call_int(0x44C8, f_4428);
        gs_set_eye_p1_w(P, 0, rBX);
        gs_set_eye_p1_w(P, 1, rCX);
        gs_set_eye_p1_w(P, 2, rBP);
        CY(3 * MOV_MR);
        call_ext(0x4A1A, 0x44D7); /* outcode_p1 */
        call_int(0x44DA, f_4428);
        gs_set_eye_p2_w(P, 0, rBX);
        gs_set_eye_p2_w(P, 1, rCX);
        gs_set_eye_p2_w(P, 2, rBP);
        CY(3 * MOV_MR);
        call_ext(0x4A4E, 0x44E9); /* outcode_p2 */
        gs_set_move_pending(P, 1);
        CY(MOV_MI);
        push(sDS);
        CY(PUSH_S);
        call_int(0x44F2, f_clip_project_line);
        sDS = pop();
        CY(POP_S);
        uint8_t n = (uint8_t)(rd8(0x31CD) - 2);
        wr8(0x31CD, n);
        CY(ALU_MI);
        if (!J(n != 0))
            break;
    }
    ret_();
}

/* 2A dotted (43A1) */
static void h_dotted(void)
{
    call_int(0x43A4, f_43B4);
    CY(JMP);
    for (bool first = true;; first = false) {
        if (!first)
            call_int(0x43AA, f_4428);
        call_ext(0x3D26, 0x43AD); /* project_dot_regs */
        if (!J(dec_m8(0x31CD)))
            break;
    }
    ret_();
}

/* ---- 2F / 2D polygons ---------------------------------------------------------------------- */

/* 2F poly_begin (44FB) */
static void h_poly_begin(void)
{
    gs_set_poly_mode(P, 1);
    gs_set_poly_edges_w(P, 0, 0);
    wr16(0x3734, 0);
    wr16(0x3736, 0);
    CY(4 * MOV_MI);
    inc_ip();
    ret_();
}

/* 0050:59B7: span fill from BX (ES = DS = back buffer): replaces [31CE] by [3159] */
static void f_span_fill(void)
{
    rDX = gs_fill_pattern(P);
    rCX = gs_colour_byte(P);
    CY(2 * MOV_RM);
    push(sDS);
    CY(PUSH_S);
    rAX = sES;
    CY(MOV_RS);
    sDS = rAX;
    CY(MOV_SR);
    set_lo(&rAX, rd8(rBX));
    CY(MOV_RM + ALU_RR + FLAG_OP);
    if (J(AL == CL))
        goto L59E1;
    CY(ALU_RR + FLAG_OP);
    if (J(AL != DL))
        goto L59E1;
    push(rBX);
    CY(PUSH_R);
    do { /* L59D1 */
        rBX--;
        set_lo(&rAX, rd8(rBX));
        CY(INC_R + MOV_RM + ALU_RR);
    } while (J(AL == DL));
    rBX++;
    CY(INC_R);
    wr16(0x1F40, rBX);
    CY(MOV_MR + JMP);
    goto L59E9;
L59DF:
    CY(FLAG_OP);
    rBX = pop();
    CY(POP_R);
L59E1:
    sDS = pop();
    CY(POP_S);
    ret_();
    return;
L59E3:
    CY(ALU_RM);
    if (J(rBX == rd16(0x1F40)))
        goto L59DF;
L59E9:
    rSI = rBX;
    rDI = rBX;
    CY(2 * MOV_RR + JMP);
    for (;;) { /* L59F0 */
        lodsw_();
        CY(ALU_RR);
        if (J(rAX != rDX))
            break;
        rAX = rCX;
        CY(MOV_RR);
        stosw_();
        CY(JMP);
    }
    CY(ALU_RR); /* L59FA */
    if (!J(AL != DL)) {
        set_lo(&rAX, CL);
        CY(MOV_RR);
        stosb_();
    }
#define ROW_DOWN(step)                                   \
    do {                                                 \
        rBX = (uint16_t)(rBX + (step));                  \
        CY(ALU_RI);                                      \
        if (!J(!neg16(rBX))) {                           \
            rBX = (uint16_t)(rBX + 0x3FB0);              \
            CY(ALU_RI);                                  \
        }                                                \
    } while (0)
#define LOAD_IS(lbl_dl)                                  \
    set_lo(&rAX, rd8(rBX));                              \
    CY(MOV_RM + ALU_RR);                                 \
    if (J(AL == DL))                                     \
        goto lbl_dl;                                     \
    CY(ALU_RR)
    ROW_DOWN(0xE050); /* L5A01 */
    LOAD_IS(L5A15);   /* L5A0B */
    if (J(AL != CL))
        goto L5A92;
L5A15:
    rBX--;
    CY(INC_R);
    LOAD_IS(L5A20);
    if (J(AL != CL))
        goto L5A2B;
L5A20:
    rBX--;
    CY(INC_R);
    LOAD_IS(L5A2E);
    if (J(AL == CL))
        goto L5A2E;
L5A2B:
    rBX++;
    CY(INC_R + JMP);
    goto L59E3;
L5A2E:
    ROW_DOWN(0xE000);
    LOAD_IS(L5A42); /* L5A38 */
    if (J(AL != CL))
        goto L5A56;
L5A42:
    ROW_DOWN(0xE000);
    LOAD_IS(L5A62); /* L5A4C */
    if (J(AL == CL))
        goto L5A62;
L5A56:
    ROW_DOWN(0xE050);
    CY(JMP); /* L5A60 */
    goto L5A20;
L5A62:
    rBX++;
    CY(INC_R);
    LOAD_IS(L5A6D);
    if (J(AL != CL))
        goto L5A78;
L5A6D:
    rBX++;
    CY(INC_R);
    LOAD_IS(L5A7B);
    if (J(AL == CL))
        goto L5A7B;
L5A78:
    rBX--;
    CY(INC_R + JMP);
    goto L5A42;
L5A7B:
    ROW_DOWN(0xE050);
    LOAD_IS(L5A8F); /* L5A85 */
    if (J(AL != CL))
        goto L5A92;
L5A8F:
    CY(JMP);
    goto L59E3;
L5A92:
    ROW_DOWN(0xE000);
    CY(JMP); /* L5A9C */
    goto L5A6D;
#undef ROW_DOWN
#undef LOAD_IS
}

/* 0050:549B poly_fill: BX = index of the last seed record ({neighbour, pixel} at 3734+i) */
static void f_poly_fill(void)
{
    rBP = 0xFFFF;
    rDX = 0x7FFF;
    CY(2 * MOV_RI);
    sES = gs_view_buf_seg(P);
    CY(MOV_SM);
    /* 54A5: drop duplicate seed pixels and plot the seeds */
    for (;;) {
        rSI = rd16((uint16_t)(rBX + 0x3736));
        CY(MOV_RM);
        rBX = (uint16_t)(rBX - 4);
        CY(ALU_RI);
        if (J(neg16(rBX)))
            break;
        CY(ALU_RR);
        if (J(rSI == rBP))
            continue;
        push(rBX);
        CY(PUSH_R);
        bool dup = false;
        for (;;) { /* L54B3 */
            CY(ALU_RM);
            if (J(rSI == rd16((uint16_t)(rBX + 0x3736)))) {
                dup = true;
                break;
            }
            rBX = (uint16_t)(rBX - 4);
            CY(ALU_RI);
            if (!J(!neg16(rBX)))
                break;
        }
        if (!dup) {
            rBX = pop();
            CY(POP_R + JMP);
            continue;
        }
        for (;;) {
            wr16((uint16_t)(rBX + 0x3734), rBP); /* L54C1 */
            wr16((uint16_t)(rBX + 0x3736), rBP);
            CY(2 * MOV_MR);
            bool again = false;
            for (;;) { /* L54C9 */
                rBX = (uint16_t)(rBX - 4);
                CY(ALU_RI);
                if (J(neg16(rBX)))
                    break;
                CY(ALU_RM);
                if (J(rSI == rd16((uint16_t)(rBX + 0x3736)))) {
                    again = true;
                    break;
                }
                CY(16); /* jne 54C9 */
            }
            if (!again)
                break;
        }
        rBX = pop(); /* L54D6 */
        CY(POP_R);
        wr16((uint16_t)(rBX + 0x3738), rBP);
        wr16((uint16_t)(rBX + 0x373A), rBP);
        CY(2 * MOV_MR + ALU_RR);
        if (!J(neg16(rSI))) {
            es_wr8(rSI, es_rd8(rSI) | 0x0F);
            CY(ALU_MI + JMP);
        } else {
            rSI &= rDX; /* L54E9 */
            CY(ALU_RR);
            es_wr8(rSI, es_rd8(rSI) | 0xF0);
            CY(ALU_MI + JMP);
        }
    }
    /* 54F1: set the pixel nibbles; drop records whose pixel is already set */
    rBX = 0;
    CY(ALU_RR);
    for (;;) { /* L54F3 */
        rDI = rd16((uint16_t)(rBX + 0x3736));
        CY(MOV_RM);
        rBX = (uint16_t)(rBX + 4);
        CY(ALU_RI + ALU_RR);
        bool set;
        if (!J(!neg16(rDI))) {
            CY(ALU_RR);
            if (J(rDI == rBP))
                continue;
            rDI &= rDX;
            CY(ALU_RR);
            set_lo(&rAX, es_rd8(rDI));
            set_hi(&rAX, AL & 0xF0);
            CY(MOV_RM + MOV_RR + 2 * ALU_RI);
            set = J(AH == 0xF0);
            if (!set) {
                set_lo(&rAX, AL | 0xF0);
                CY(ALU_AI);
                stosb_();
                CY(JMP);
                continue;
            }
        } else {
            if (J(rDI == 0)) /* L5516 */
                break;
            set_lo(&rAX, es_rd8(rDI));
            set_hi(&rAX, AL & 0x0F);
            CY(MOV_RM + MOV_RR + 2 * ALU_RI);
            set = !J(AH != 0x0F);
            if (!set) {
                set_lo(&rAX, AL | 0x0F); /* L552F */
                CY(ALU_AI);
                stosb_();
                CY(JMP);
                continue;
            }
        }
        wr16((uint16_t)(rBX + 0x3730), rBP); /* L5525 */
        wr16((uint16_t)(rBX + 0x3732), rBP);
        CY(2 * MOV_MR + JMP);
    }
    /* 5534: drop records whose neighbour is already set (or off the view) */
    rBX = 0;
    CY(ALU_RR);
    for (;;) { /* L5536 */
        rDI = rd16((uint16_t)(rBX + 0x3734));
        rSI = rd16((uint16_t)(rBX + 0x3736));
        CY(2 * MOV_RM);
        rBX = (uint16_t)(rBX + 4);
        CY(ALU_RI + ALU_RR);
        if (J(rSI == 0))
            break;
        CY(ALU_RR);
        if (J(rDI == rBP))
            continue;
        rAX = (uint16_t)((rDI - rSI) & rDX);
        CY(MOV_RR + 2 * ALU_RR);
        bool drop;
        if (!J(rAX != 0)) {
            CY(ALU_RR);
            if (!J(!neg16(rSI))) {
                rSI--;
                CY(INC_R);
                set_lo(&rCX, 0x0F); /* L5556 */
                CY(MOV_RI + JMP);
            } else {
                rSI++; /* L555A */
                CY(INC_R);
                set_lo(&rCX, 0xF0); /* L555B */
                CY(MOV_RI);
            }
            rSI &= rDX; /* L555D */
            CY(ALU_RR);
            set_lo(&rAX, es_rd8(rSI));
            CY(MOV_RM + JMP);
        } else {
            CY(ALU_RR); /* L5564 */
            if (J(rAX == rDX)) {
                set_lo(&rCX, 0x0F); /* L5556 */
                CY(MOV_RI + JMP);
                rSI &= rDX;
                CY(ALU_RR);
                set_lo(&rAX, es_rd8(rSI));
                CY(MOV_RM + JMP);
                goto L559E;
            }
            rAX--;
            CY(INC_R);
            if (J(rAX == 0)) {
                set_lo(&rCX, 0xF0); /* L555B */
                CY(MOV_RI);
                rSI &= rDX;
                CY(ALU_RR);
                set_lo(&rAX, es_rd8(rSI));
                CY(MOV_RM + JMP);
                goto L559E;
            }
            set_lo(&rCX, 0x0F);
            CY(MOV_RI + ALU_RR);
            if (!J(!neg16(rDI))) {
                set_lo(&rCX, 0xF0);
                CY(MOV_RI);
            }
            rAX = rSI & rDX; /* L5573 */
            CY(MOV_RR + ALU_RR);
            int32_t t = (int16_t)rAX - 0x2000;
            rAX = (uint16_t)t;
            CY(ALU_AI);
            bool down;
            if (!J(t >= 0)) { /* jge on sub ax,2000h: AX <= 7FFF here */
                rAX = (uint16_t)(rAX + 0x4000);
                CY(ALU_AI);
                rAX = (uint16_t)((rAX - rDI) & rDX);
                CY(2 * ALU_RR);
                down = J(rAX != 0);
                if (!down)
                    CY(16); /* je 558D */
            } else {
                rAX = (uint16_t)((rAX - rDI) & rDX); /* L5587 */
                CY(2 * ALU_RR);
                down = J(rAX == 0);
            }
            rDI &= rDX;
            CY(ALU_RR);
            if (down) {
                rDI = (uint16_t)(rDI + 0x50); /* L5596 */
                CY(ALU_RI);
            } else {
                rDI = (uint16_t)(rDI - 0x50); /* L558D */
                CY(ALU_RI);
                if (J(neg16(rDI)))
                    goto L55A4;
                CY(JMP);
            }
            set_lo(&rAX, es_rd8(rDI)); /* L559B */
            CY(MOV_RM);
        }
    L559E:
        set_lo(&rAX, AL & CL);
        CY(2 * ALU_RR);
        drop = AL == CL;
        if (J(!drop))
            continue;
    L55A4:
        wr16((uint16_t)(rBX + 0x3730), rBP);
        wr16((uint16_t)(rBX + 0x3732), rBP);
        CY(2 * MOV_MR + JMP);
    }
    /* 55AE: flood fill from the remaining neighbours */
    rBX = 0;
    CY(ALU_RR);
    for (;;) { /* L55B0 */
        rDI = rd16((uint16_t)(rBX + 0x3734));
        rBX = (uint16_t)(rBX + 4);
        rDX = gs_fill_pattern(P);
        CY(MOV_RM + ALU_RI + MOV_RM + ALU_RR);
        if (!J(!neg16(rDI))) {
            CY(ALU_RR);
            if (J(rDI == rBP))
                continue;
            rDI &= 0x7FFF;
            CY(ALU_RI);
            push(rBX);
            CY(PUSH_R);
            rBX = rDI;
            CY(MOV_RR);
            set_lo(&rAX, es_rd8(rBX));
            CY(MOV_RM + ALU_RR);
            if (!J(AL == DL)) {
                set_lo(&rAX, AL ^ DL);
                set_lo(&rAX, AL & 0xF0);
                CY(ALU_RR + ALU_AI);
                if (J(AL != 0))
                    goto L55F1;
                rBX--;
                CY(INC_R + JMP);
            }
        } else {
            if (J(rDI == 0)) /* L55DB */
                break;
            push(rBX);
            CY(PUSH_R);
            rBX = rDI;
            CY(MOV_RR);
            set_lo(&rAX, es_rd8(rBX));
            CY(MOV_RM + ALU_RR);
            if (!J(AL == DL)) {
                set_lo(&rAX, AL ^ DL);
                set_lo(&rAX, AL & 0x0F);
                CY(ALU_RR + ALU_AI);
                if (J(AL != 0))
                    goto L55F1;
                rBX++;
                CY(INC_R);
            }
        }
        call_int(0x55F1, f_span_fill); /* L55EE */
    L55F1:
        rBX = pop();
        CY(POP_R + JMP);
    }
    ret_(); /* L55F4 */
}

/* 2D poly_end (5468) */
static void h_poly_end(void)
{
    gs_set_poly_mode(P, 0);
    CY(MOV_MI);
    rBX = gs_poly_edges_w(P, 0);
    CY(MOV_RM);
    wr16((uint16_t)(rBX + 0x3734), 0);
    wr16((uint16_t)(rBX + 0x3736), 0);
    CY(2 * MOV_MI);
    rBX = (uint16_t)(rBX - 4);
    CY(ALU_RI);
    wr16(0x3732, rBX);
    CY(MOV_MR);
    inc_ip();
    CY(TEST_MI);
    if (!J((gs_time_of_day(P) & 1) == 0)) {
        CY(ALU_MI);
        if (!J(gs_radar_view(P) == 1))
            call_int(0x549A, f_poly_fill);
    }
    ret_();
}

/* ---- control and data opcodes ---------------------------------------------------------------- */

/* 17 demo_script (45A2) */
static void h_demo_script(void)
{
    rAX = rd16((uint16_t)(rBX + 1));
    CY(MOV_RM);
    rBX = (uint16_t)(rBX + 3);
    CY(ALU_RI);
    gs_set_demo_script(P, rBX);
    CY(MOV_MR);
    add_ip(rAX);
    CY(ALU_RM);
    ret_();
}

/* 18 call (4517) */
static void h_call(void)
{
    mov_ax_m(GS_SCENERY_IP);
    rAX = (uint16_t)(rAX + 3);
    CY(ALU_AI);
    mov_m_ax(0x31D1);
    CY(JMP);
    h_jump();
}

/* 19 return (4523) */
static void h_return(void)
{
    mov_ax_m(0x31D1);
    mov_m_ax(GS_SCENERY_IP);
    ret_();
}

/* 1A copy (4592) */
static void h_copy(void)
{
    uint16_t bx = rBX;
    rSI = rd16((uint16_t)(bx + 1));
    rBX = rd16((uint16_t)(bx + 3));
    rAX = rd16(rBX);
    CY(3 * MOV_RM);
    wr16(rSI, rAX);
    CY(MOV_MR);
    add_ip(5);
    CY(ALU_MI);
    ret_();
}

/* 1D nav_station (452A): while searching, a frequency match copies east/north to 31D6 */
static void h_nav_station(void)
{
    CY(ALU_MI);
    if (!J(rd8(0x31D4) == 0)) {
        gs_set_nav_search(P, 0);
        CY(MOV_MI);
        mov_ax_m(0x03FF);
        CY(ALU_RM);
        if (!J(rAX != rd16((uint16_t)(rBX + 1)))) {
            rSI = 7;
            CY(MOV_RI);
            do { /* L4541 */
                set_lo(&rAX, rd8((uint16_t)(rBX + rSI + 3)));
                wr8((uint16_t)(rSI + 0x31D6), AL);
                rSI--;
                CY(MOV_RM + MOV_MR + INC_R);
            } while (J(!neg16(rSI)));
            gs_set_nav_found(P, 1);
            CY(MOV_MI);
        }
    }
    add_ip(0x0B); /* L4550 */
    CY(ALU_MI);
    ret_();
}

/* 1E com_station (4556): while searching, a frequency match copies the runway/temperature
 * bytes to 31E0, starts the ATIS text and formats its fields (atis_start, original code) */
static void h_com_station(void)
{
    rDX = rd16((uint16_t)(rBX + 1));
    CY(MOV_RM + ALU_MI);
    if (!J(rd8(0x31DF) == 0)) {
        gs_set_com_search(P, 0);
        CY(MOV_MI);
        mov_ax_m(0x03F2);
        CY(ALU_RM);
        if (!J(rAX != rd16((uint16_t)(rBX + 3)))) {
            rBX = (uint16_t)(rBX + 5);
            CY(ALU_RI);
            set_lo(&rCX, 8);
            rSI = 0x31E0;
            CY(2 * MOV_RI);
            do { /* L4575 */
                set_lo(&rAX, rd8(rBX));
                wr8(rSI, AL);
                rSI++;
                rBX++;
                set_lo(&rCX, (uint8_t)(CL - 1));
                CY(MOV_RM + MOV_MR + 2 * INC_R + INC_R8);
            } while (J(CL != 0));
            gs_set_atis_ptr(P, rBX);
            CY(MOV_MR);
            gs_set_atis_idle(P, 0);
            CY(MOV_MI);
            push(rDX);
            CY(PUSH_R);
            call_ext(0x4FBF, 0x458C); /* atis_start */
            rDX = pop();
            CY(POP_R);
        }
    }
    add_ip(rDX); /* L458D */
    CY(ALU_RM);
    ret_();
}

/* ---- the interpreter ---------------------------------------------------------------------- */

static void dispatch(uint16_t handler)
{
    switch (handler) {
    case 0x3D17: h_dot(); break;
    case 0x3D8E: h_move(); break;
    case 0x3DB3: h_line(); break;
    case 0x3E1A: h_viewpoint(); break;
    case 0x3E47: h_line2d(); break;
    case 0x3E69: h_clear_view(); break;
    case 0x3E70: h_jump(); break;
    case 0x3EEC: h_colour_dusk(); break;
    case 0x3E78: h_capture(); break;
    case 0x3E98: h_proj_scale(); break;
    case 0x3EAF: h_cga_regs(); break;
    case 0x3EB6: h_nop(); break;
    case 0x3EBB: h_colour(); break;
    case 0x408C: ext(0x408C); break; /* 15: draw_horizon_list */
    case 0x45A2: h_demo_script(); break;
    case 0x4517: h_call(); break;
    case 0x4523: h_return(); break;
    case 0x4592: h_copy(); break;
    case 0x3EF4: h_colour_dark_a(); break;
    case 0x3F07: h_colour_dark_b(); break;
    case 0x452A: h_nav_station(); break;
    case 0x4556: h_com_station(); break;
    case 0x3F1A: h_if_in(); break;
    case 0x3F3B: h_if_in2(); break;
    case 0x3F67: h_if_in3(); break;
    case 0x3FA6: h_if_bits(); break;
    case 0x3FF0: h_origin(); break;
    case 0x3F0C: h_store(); break;
    case 0x3FBE: h_jump_if(); break;
    case 0x3DDC: h_close(); break;
    case 0x43A1: h_dotted(); break;
    case 0x44BF: h_dashed(); break;
    case 0x5468: h_poly_end(); break;
    case 0x3ED8: h_colour_night(); break;
    case 0x44FB: h_poly_begin(); break;
    case 0x4140: h_cache_point(); break;
    case 0x4178: h_move_cached(); break;
    case 0x418E: h_line_cached(); break;
    case 0x41CB: h_skip1(); break;
    case 0x41D1: h_dot_cached(); break;
    case 0x3D8A: h_move_flat(); break;
    case 0x3DAF: h_line_flat(); break;
    /* 03 and 0F (a bare RET: the IP never advances), 45B1 (jmp $), 42 (through the word at
     * DS:3288) and anything else hang or run off into data: hand over to the original. */
    default: bail_at(handler);
    }
}

/* 0050:3CC0 scenery_interp (entered by CALL, return address on the stack) */
static void f_interp(void)
{
    push(sDS);
    CY(PUSH_S);
    rAX = 0;
    CY(ALU_RR);
    sDS = 0;
    CY(MOV_SR);
    sDS = mem_read16(P, 0, 0x120);
    CY(MOV_SM);
    mov_al_m(0x31D3);
    mov_m_al(0x31D4);
    mov_al_m(0x31DE);
    mov_m_al(0x31DF);
    for (;;) { /* L3CD5 */
        rBX = gs_scenery_ip(P);
        set_lo(&rAX, rd8(rBX));
        CY(2 * MOV_RM + ALU_AI);
        if (J(AL == 0x79))
            break;
        CY(ALU_AI);
        if (J(AL > 0x42))
            bail_at(0x3D14); /* jmp 45B1: hang */
        set_hi(&rAX, 0);
        rAX = (uint16_t)(rAX << 1);
        rBP = (uint16_t)(0x3204 + rAX);
        CY(MOV_RI + SH1_R + MOV_RI + ALU_RR);
        rCX = rd16(rBP);
        CY(MOV_RM);
        push(0x3CF2);
        CY(CALL_R);
        dispatch(rCX);
        rAX = 0;
        CY(ALU_RR);
        sDS = 0;
        CY(MOV_SR);
        sDS = mem_read16(P, 0, 0x120);
        CY(MOV_SM + JMP);
    }
    CY(TEST_MI); /* L3CFC */
    if (!J(gs_capture_mode(P) == 0)) {
        rBX = gs_capture_ptr(P);
        CY(MOV_RM);
        wr8(rBX, 0xFF);
        CY(MOV_MI);
    }
    set_lo(&rAX, 0);
    CY(ALU_RR);
    mov_m_al(0x31DF);
    mov_m_al(0x31D4);
    sDS = pop();
    CY(POP_S);
    ret_();
}

/* 0050:045D draw_scenery: runs the program (radar view: with altitude = radar_zoom), then
 * the SPLASH test: below 4 m with the water pattern 2222h under the nose -> crash code 8 */
static void f_draw_scenery(void)
{
    CY(TEST_MI);
    if (!J(gs_radar_view(P) != 0)) {
        call_int(0x0467, f_interp);
        mov_ax_m(0x30D5);
        rDX = rd16((uint16_t)(GS_ALTITUDE + 2));
        CY(MOV_RM);
        uint32_t v = ((uint32_t)rDX << 16 | rAX) - 0x400;
        rAX = (uint16_t)v;
        rDX = (uint16_t)(v >> 16);
        CY(ALU_AI + ALU_RI);
        if (!J(!neg16(rDX))) {
            sES = gs_view_buf_seg(P);
            CY(MOV_SM + ALU_MI);
            if (!J(mem_read16(P, sES, 0x3068) != 0x2222)) {
                gs_set_crash_code(P, 8);
                CY(MOV_MI);
            }
        }
        ret_();
        return;
    }
    mov_ax_m(0x0586); /* L0489 */
    uint16_t t = rd16(GS_ALTITUDE);
    wr16(GS_ALTITUDE, rAX);
    rAX = t;
    CY(XCHG_RM);
    push(rAX);
    CY(PUSH_R);
    mov_ax_m(0x0588);
    t = rd16((uint16_t)(GS_ALTITUDE + 2));
    wr16((uint16_t)(GS_ALTITUDE + 2), rAX);
    rAX = t;
    CY(XCHG_RM);
    push(rAX);
    CY(PUSH_R);
    call_int(0x049C, f_interp);
    wr16((uint16_t)(GS_ALTITUDE + 2), pop());
    wr16(GS_ALTITUDE, pop());
    CY(2 * POP_M);
    ret_();
}

/* 0050:03BE scenery_reset_ip */
static void f_scenery_reset_ip(void)
{
    mov_ax_m(0x03F0);
    mov_m_ax(GS_SCENERY_IP);
    gs_set_capture_mode(P, 0);
    CY(MOV_MI);
    ret_();
}

/* ---- natives ---------------------------------------------------------------------------------
 * Each charges the original's cycles as it goes; the entry's .cycles is the final RET. A
 * hand-over to original code leaves without the RET. */

static void run(Pc *pc, void (*fn)(void))
{
    P = pc;
    C = &pc->cpu;
    if (setjmp(bail_jb)) {
        C->cycles -= RET;
        return;
    }
    fn();
    C->cycles -= RET;
}

static void n_scenery_interp(Pc *pc) { run(pc, f_interp); }
static void n_draw_scenery(Pc *pc) { run(pc, f_draw_scenery); }

static void n_scenery_reset_ip(Pc *pc) { run(pc, f_scenery_reset_ip); }
static void n_clip_project_line(Pc *pc) { run(pc, f_clip_project_line); }

NativeEntry native_scenery[] = {
    { .name = "scenery_interp", .seg = GAME_CS, .off = 0x3CC0, .fn = n_scenery_interp, .enabled = true,
      .cycles = RET },
    { .name = "draw_scenery", .seg = GAME_CS, .off = 0x045D, .fn = n_draw_scenery, .enabled = true,
      .cycles = RET },
    { .name = "scenery_reset_ip", .seg = GAME_CS, .off = 0x03BE, .fn = n_scenery_reset_ip, .enabled = true,
      .cycles = RET },
    { .name = "clip_project_line", .seg = GAME_CS, .off = 0x40C8, .fn = n_clip_project_line, .enabled = true,
      .cycles = RET },
    { .name = NULL },
};
