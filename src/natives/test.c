/* Framework test natives (3.0). */
#include "native.h"

/* 0050:45FD sin_quadrant: BX = 0..4000h -> AX, from the word table at DS:3381 with
 * linear interpolation. Clobbers BX, DX, SI, DI like the original. Pass-through test
 * native for the framework (3.0); flags are not guaranteed. */
static void n_sin_quadrant(Pc *pc)
{
    Cpu8086 *c = &pc->cpu;
    uint16_t ds = c->sregs[S_DS];
    uint16_t x = c->regs[R_BX];
    uint16_t bx = (uint16_t)(((uint16_t)(x << 1) >> 8) << 1);
    uint16_t si = mem_read16(pc, ds, (uint16_t)(bx + 0x3381));
    uint16_t di = (uint16_t)(mem_read16(pc, ds, (uint16_t)(bx + 0x3383)) - si);
    int32_t p = (int32_t)(int16_t)(x & 0x7F) * (int16_t)di; /* IMUL DI -> DX:AX */
    uint16_t ax = (uint16_t)p, dx = (uint16_t)((uint32_t)p >> 16);
    dx = (uint16_t)(dx << 1 | ax >> 15); /* SHL AX,1 / RCL DX,1 */
    ax = (uint16_t)(ax << 1);
    ax = (uint16_t)((dx & 0xFF) << 8 | ax >> 8);
    c->regs[R_AX] = (uint16_t)(ax + si);
    c->regs[R_BX] = bx;
    c->regs[R_DX] = dx;
    c->regs[R_SI] = si;
    c->regs[R_DI] = di;
    native_ret(pc);
}

NativeEntry native_test[] = {
    { .name = "test_passthrough", .seg = GAME_CS, .off = 0x45FD, .fn = n_sin_quadrant, .enabled = false,
      .cycles = 253 },
    { .name = NULL },
};
