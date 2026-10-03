/* Natives for subphase 3.13 flight model: forces (see docs/PHASE3_PLAN.md and
 * docs/subphases/3.13.md).
 *
 * flight_forces runs in the timer interrupt every 3rd 18.2 Hz tick (about 6 Hz). It turns
 * the control positions, airspeed and attitude into lift, drag, acceleration, a pitch rate
 * and a turn rate, and integrates airspeed and flight-path pitch. control_coupling and
 * wing_cl_offsets run from the aileron, rudder and flaps key handlers and precompute the
 * control terms it uses.
 *
 * All quantities are 16-bit fixed point and wrap exactly as the original's 16-bit ADD/SUB.
 * The products use the original's IMUL idioms (see the mul_* helpers). Every native leaves
 * the registers and the words below SP (the return addresses of its inner CALLs) as the
 * original does, and adds the emulated cycles the original takes on the path it ran (the
 * cpu8086.c model), so the emulated timeline stays the same with the natives on. No caller
 * tests the flags afterwards, so none are reproduced (.flag_mask = 0). */
#include "native.h"
#include "game/state.h"
#include "fixmath.h"

/* ---- game variables (DS = 0618) --------------------------------------------------- */

/* controls and configuration */

/* aircraft state */

/* terms precomputed elsewhere (key handlers, flight_params) */

/* written by control_coupling / wing_cl_offsets */

/* constants (read only, set at start-up) */
#define K_SLIP_CL        0x08D0
#define K_SLIP_YAW       0x08D2
#define K_SLIP_ROLL      0x08D4
#define K_SLIP_DRAG      0x08E4

/* written by flight_forces */

/* ---- fixed-point products, exactly as the original's IMUL idioms ------------------- */

/* IMUL, then n times SHL AX,1 / RCL DX,1, keep DX: bits 16-n..31-n of the product.
 * n = 1 is the Q15 multiply (fx_mul_q15). */
static int16_t mul_shl(int16_t a, int16_t b, int n)
{
    return (int16_t)(((uint32_t)((int32_t)a * b) << n) >> 16);
}
/* IMUL, keep DX: the product >> 16. */
static int16_t mul_hi(int16_t a, int16_t b)
{
    return (int16_t)((uint32_t)((int32_t)a * b) >> 16);
}
/* IMUL; MOV AL,AH; MOV AH,DL: the product >> 8. */
static int16_t mul_mid(int16_t a, int16_t b)
{
    return (int16_t)((uint32_t)((int32_t)a * b) >> 8);
}

static uint16_t rd16(Pc *pc, uint16_t off) { return mem_read16(pc, pc->cpu.sregs[S_DS], off); }
static int16_t rds(Pc *pc, uint16_t off) { return (int16_t)rd16(pc, off); }
static void stack_write(Pc *pc, uint16_t sp, uint16_t v) { mem_write16(pc, pc->cpu.sregs[S_SS], sp, v); }

/* ---- cycles (cpu8086.c model) ------------------------------------------------------ */

#define JCC_TAKEN 12 /* a taken Jcc costs 16 instead of 4; the block costs count 4 */

/* sincos 0050:45C0 including its RET (see math.c). */
static uint32_t sincos_cycles(uint16_t angle)
{
    if (angle == 0x8000)
        return 84 + 597;
    if (angle & 0x8000)
        return 92 + sincos_cycles((uint16_t)-angle);
    return angle <= 0x4000 ? 616 : 597;
}

/* div_q15 0050:1BEE with DX:AX = 0280:0000 (positive) and CX = d, including its RET. */
static uint32_t div_q15_pos_cycles(uint16_t d)
{
    if (d == 0)
        return 66;
    bool dneg = d & 0x8000;
    uint16_t ad = dneg ? (uint16_t)-d : d;
    bool saturated = (int16_t)0x0280 >= (int16_t)ad;
    return dneg ? (saturated ? 94 : 241) : (saturated ? 84 : 226);
}

/* "CALL sincos" made with the stack pointer at sp: writes the return address and the
 * return addresses sincos's inner CALLs leave below it (as n_sincos in math.c), and
 * returns every register sincos leaves. */
static FxSincos call_sincos(Pc *pc, uint16_t sp, uint16_t ret_ip, uint16_t angle, uint32_t *cyc)
{
    sp = (uint16_t)(sp - 2);
    stack_write(pc, sp, ret_ip);
    *cyc += sincos_cycles(angle);
    FxSincos r = fx_sincos_regs(pc, pc->cpu.sregs[S_DS], angle);
    if (angle & 0x8000) {
        sp = (uint16_t)(sp - 2);
        stack_write(pc, sp, 0x45FA);
        angle = (uint16_t)-angle;
    }
    stack_write(pc, (uint16_t)(sp - 2), (angle & 0x8000) || angle > 0x4000 ? 0x45D5 : 0x45EF);
    return r;
}

/* ---- fix_loop_over 0050:18CE ------------------------------------------------------- */

#define FIX_LOOP_OVER_CYCLES 60 /* |pitch| <= 90 degrees: TEST, JE taken, RET */

/* Past the vertical (pitch in 4000h..BFFFh, tested on a pitch already made positive by the
 * caller), the flight path is mirrored: pitch becomes 180 degrees - pitch and the aircraft
 * is turned around, bank and heading + 180 degrees. Returns DX as the original leaves it. */
static uint16_t loop_over(Pc *pc, uint32_t *cyc)
{
    uint16_t pitch = (uint16_t)gs_pitch(pc);
    *cyc += 28 + 20;
    if (!(pitch & 0xC000)) {
        *cyc += JCC_TAKEN;
        return pitch;
    }
    *cyc += 67;
    pitch = (uint16_t)(0x8000 - pitch);
    gs_set_pitch(pc, pitch);
    gs_set_bank(pc, rd16(pc, GS_BANK) + 0x8000);
    gs_set_heading(pc, rd16(pc, GS_HEADING) + 0x8000);
    return pitch;
}

static void n_fix_loop_over(Pc *pc)
{
    uint32_t cyc = 0;
    pc->cpu.regs[R_DX] = loop_over(pc, &cyc);
    pc->cpu.cycles += cyc - FIX_LOOP_OVER_CYCLES;
    native_ret(pc);
}

/* ---- control_coupling 0050:10B6 and wing_cl_offsets 0050:10EF ---------------------- */

#define CONTROL_COUPLING_CYCLES 856 /* straight line, falls into wing_cl_offsets */
#define WING_CL_OFFSETS_CYCLES 138

/* Lift coefficient offsets of each wing: the flaps add to both, the ailerons add to the
 * left wing and take from the right one, and crossed controls (sideslip) add to the left
 * wing only. Leaves AX = left offset, DX = right offset. */
static void wing_cl_offsets(Pc *pc)
{
    uint16_t both = (uint16_t)((uint16_t)gs_flaps_cl(pc) + gs_cl0(pc));
    uint16_t left = (uint16_t)(both + (uint16_t)gs_aileron_cl(pc) + (uint16_t)gs_slip_cl(pc));
    uint16_t right = (uint16_t)(both - (uint16_t)gs_aileron_cl(pc));
    gs_set_cl_ofs_left(pc, left);
    gs_set_cl_ofs_right(pc, right);
    pc->cpu.regs[R_AX] = left;
    pc->cpu.regs[R_DX] = right;
}

static void n_wing_cl_offsets(Pc *pc)
{
    wing_cl_offsets(pc);
    native_ret(pc);
}

/* Aileron and rudder deflected against each other (a crossed-control slip) give extra
 * drag, a yaw rate, a roll term and extra lift on the left wing, each proportional to
 * aileron/2 - rudder/2. Then the wing offsets are recomputed. */
static void n_control_coupling(Pc *pc)
{
    int16_t cross = (int16_t)(((int16_t)(uint16_t)gs_aileron(pc) >> 1) - ((int16_t)(uint16_t)gs_rudder(pc) >> 1));
    gs_set_cross_control(pc, cross);
    gs_set_slip_drag(pc, mul_hi(cross, rds(pc, K_SLIP_DRAG)));
    gs_set_slip_yaw(pc, mul_hi(rds(pc, K_SLIP_YAW), cross));
    gs_set_crab_b(pc, mul_hi(rds(pc, K_SLIP_ROLL), cross));
    gs_set_slip_cl(pc, mul_hi(rds(pc, K_SLIP_CL), cross));
    wing_cl_offsets(pc);
    native_ret(pc);
}

/* ---- flight_forces 0050:1610 ------------------------------------------------------- */

#define FLIGHT_FORCES_CYCLES 54 /* paused or slewing: return at once */

/* AoA limits of the wing; outside them it stalls and gives no lift. */
#define AOA_STALL_HIGH 0x1F40
#define AOA_STALL_LOW ((int16_t)0xE4A8)
#define AIRSPEED_MAX 0x6400

static void n_flight_forces(Pc *pc)
{
    Cpu8086 *c = &pc->cpu;
    uint32_t cyc = 34;
    uint8_t frozen = gs_paused(pc) | gs_slew_mode(pc);
    if (frozen) {
        c->regs[R_AX] = (uint16_t)((c->regs[R_AX] & 0xFF00) | frozen);
        native_ret(pc);
        return; /* 54 cycles = .cycles */
    }
    cyc += JCC_TAKEN;
    uint16_t sp = c->regs[R_SP];

    /* Attitude. */
    cyc += 295;
    FxSincos tp = call_sincos(pc, sp, 0x1621, (uint16_t)gs_pitch(pc), &cyc);
    int16_t sin_pitch = (int16_t)tp.ax, cos_pitch = (int16_t)tp.cx;
    FxSincos tb = call_sincos(pc, sp, 0x162F, (uint16_t)gs_bank(pc), &cyc);
    int16_t sin_bank = (int16_t)tb.ax, cos_bank = (int16_t)tb.cx;
    gs_set_sin_pitch(pc, sin_pitch);
    gs_set_cos_pitch(pc, cos_pitch);
    gs_set_crab_a(pc, sin_bank);
    gs_set_cos_bank(pc, cos_bank);

    /* cos^2(bank), keeping the sign of cos: the share of lift that holds the aircraft up
     * (negative when inverted). */
    int16_t cos2_bank = mul_shl(cos_bank, cos_bank, 1);
    if (cos_bank < 0) {
        cos2_bank = (int16_t)-cos2_bank;
        cyc += 5;
    } else {
        cyc += JCC_TAKEN;
    }
    gs_set_cos2_bank(pc, cos2_bank);

    /* Angle of attack: elevator term times airspeed, the airspeed capped at 1E00h. The cap
     * tests the sign of AH - 1Eh, so AH = 9Fh..FFh (never reached) also pass uncapped. */
    uint16_t airspeed = gs_airspeed(pc);
    int16_t v_aoa = (int16_t)airspeed;
    cyc += 40;
    if ((uint8_t)((airspeed >> 8) - 0x1E) & 0x80) {
        cyc += JCC_TAKEN;
    } else {
        v_aoa = 0x1E00;
        cyc += 4;
    }
    int16_t aoa = mul_shl(v_aoa, gs_elev_aoa(pc), 3);
    gs_set_angle_of_attack(pc, aoa);

    /* Lift coefficient of each wing: AoA times the lift slope, plus the flaps, aileron and
     * slip offsets. A shot-off left wing has none. */
    cyc += 471;
    int16_t cl_aoa = mul_shl(aoa, gs_lift_slope(pc), 1);
    gs_set_cl_aoa(pc, cl_aoa);
    int16_t cl_left = (int16_t)(cl_aoa + gs_cl_ofs_left(pc));
    gs_set_cl_left(pc, cl_left);
    if (gs_wing_damage(pc) == 1) {
        cl_left = 0;
        gs_set_cl_left(pc, 0);
        cyc += 17;
    } else {
        cyc += JCC_TAKEN;
    }
    int16_t cl_right = (int16_t)(cl_aoa + gs_cl_ofs_right(pc));
    gs_set_cl_right(pc, cl_right);

    /* Stall: past either AoA limit both wings lose their lift and the stall horn sounds. */
    cyc += 69;
    bool stalled;
    if (aoa > AOA_STALL_HIGH) {
        stalled = true;
    } else {
        cyc += JCC_TAKEN + 10;
        stalled = aoa <= AOA_STALL_LOW;
        if (stalled)
            cyc += JCC_TAKEN;
    }
    if (stalled) {
        cyc += 93;
        cl_left = cl_right = 0;
        gs_set_cl_right(pc, 0);
        gs_set_cl_left(pc, 0);
        gs_set_sound_state(pc, 1);
        gs_set_stalled(pc, 1);
    } else {
        cyc += 19;
        gs_set_stalled(pc, 0);
    }

    /* Lift = V^2 * (CL_left + CL_right) * air density. The difference between the wings
     * is the roll moment. */
    cyc += 2338;
    int16_t v_sq = (int16_t)((((uint32_t)airspeed * airspeed) << 1) >> 16); /* MUL: unsigned */
    gs_set_airspeed_sq(pc, v_sq);
    int16_t lift_left = mul_shl(v_sq, cl_left, 3);
    int16_t lift_right = mul_shl(v_sq, cl_right, 3);
    gs_set_lift_left(pc, lift_left);
    gs_set_lift_right(pc, lift_right);
    int16_t lift = mul_shl((int16_t)(lift_left + lift_right), (int16_t)gs_air_density(pc), 1);
    gs_set_lift(pc, lift);
    gs_set_roll_moment(pc, (uint16_t)((lift_left - lift_right) << 2));

    /* Weight split along and across the flight path. */
    int16_t weight = (int16_t)gs_weight(pc);
    int16_t weight_along = mul_shl(sin_pitch, weight, 1);
    int16_t weight_normal = mul_shl(cos_pitch, weight, 1);
    gs_set_weight_along(pc, weight_along);
    gs_set_weight_normal(pc, weight_normal);

    /* Net force across the flight path: the vertical share of lift minus the weight. */
    int16_t normal_force = (int16_t)(mul_shl(cos2_bank, lift, 1) - weight_normal);
    gs_set_normal_force(pc, normal_force);

    /* Drag = (CL_left^2 + CL_right^2 + slip + flaps) * scale + gear, times V^2, plus the
     * weight along the path (gravity) and, while moving, a constant friction term. */
    int16_t cl2_left = mul_shl(cl_left, cl_left, 1);
    int16_t cl2_right = mul_shl(cl_right, cl_right, 1);
    gs_set_cl2_left(pc, cl2_left);
    gs_set_cl2_right(pc, cl2_right);
    int16_t drag_coef = (int16_t)(cl2_left + cl2_right + gs_slip_drag(pc) + gs_flaps_drag(pc));
    gs_set_drag_coef(pc, drag_coef);
    int16_t drag_scaled = mul_shl(drag_coef, gs_drag_scale(pc), 1);
    gs_set_drag_scaled(pc, drag_scaled);
    int16_t drag_total = (int16_t)(drag_scaled + gs_gear_drag(pc));
    gs_set_drag_total(pc, drag_total);
    int16_t drag = (int16_t)(mul_shl(drag_total, v_sq, 1) + weight_along);
    if (airspeed != 0) {
        drag = (int16_t)(drag + gs_friction(pc));
        cyc += 18;
    } else {
        cyc += JCC_TAKEN;
    }
    gs_set_drag(pc, drag);

    /* Acceleration along the path = (thrust - drag) / mass. */
    cyc += 274;
    int16_t net_force = (int16_t)(gs_thrust(pc) - drag);
    gs_set_net_force(pc, net_force);
    int16_t accel = mul_hi((int16_t)gs_inv_mass(pc), net_force);
    gs_set_accel(pc, accel);

    /* 1/V for the rates: div_q15(0280:0000, V), saturating at low speed. */
    stack_write(pc, (uint16_t)(sp - 2), 0x17E5);
    cyc += div_q15_pos_cycles(airspeed);
    int16_t inv_v = fx_div_q15(0x02800000, (int16_t)airspeed);
    uint16_t cx_out = (airspeed & 0x8000) ? (uint16_t)-airspeed : airspeed; /* div_q15 leaves |CX| */
    gs_set_inv_airspeed(pc, inv_v);

    /* Pitch acceleration = normal force / V. */
    cyc += 1059;
    int16_t pitch_accel = mul_shl(inv_v, normal_force, 1);
    gs_set_pitch_accel(pc, pitch_accel);

    /* Turn rate = horizontal lift (lift * sin(bank)) / V, plus the slip yaw * V. On the
     * ground while rolling, the rudder steers the nosewheel instead. */
    int16_t lift_horiz = mul_shl(sin_bank, lift, 1);
    gs_set_lift_horiz(pc, lift_horiz);
    int16_t turn = mul_shl(gs_turn_gain(pc), mul_shl(inv_v, lift_horiz, 6), 1);
    stack_write(pc, (uint16_t)(sp - 2), (uint16_t)turn); /* PUSH DX */
    turn = (int16_t)(turn + mul_shl((int16_t)airspeed, gs_slip_yaw(pc), 3));
    gs_set_turn_rate(pc, turn);
    bool on_ground = gs_on_ground(pc) != 0;
    if (!on_ground) {
        cyc += JCC_TAKEN;
    } else {
        cyc += 18;
        if (airspeed == 0) {
            cyc += JCC_TAKEN;
        } else {
            cyc += 48;
            gs_set_turn_rate(pc, rds(pc, GS_RUDDER) >> 3);
        }
    }

    /* Pitch rate; on the ground the nose cannot be pushed into the runway. */
    cyc += 216;
    int16_t pitch_rate = mul_shl((int16_t)gs_mass_term2(pc), pitch_accel, 1);
    gs_set_pitch_rate(pc, pitch_rate);
    if (!on_ground) {
        cyc += JCC_TAKEN;
    } else {
        cyc += 9;
        if (pitch_rate >= 0) {
            cyc += JCC_TAKEN;
        } else {
            cyc += 19;
            pitch_rate = 0;
            gs_set_pitch_rate(pc, 0);
        }
    }

    /* Integrate airspeed: the acceleration plus the speed traded for climbing. A
     * negative result stops the aircraft; an overflow or more than 6400h caps it at
     * 64xxh (the low byte is left from the increment). */
    cyc += 390;
    int16_t acc_to_pitch = mul_mid(accel, gs_accel_to_pitch_k(pc));
    gs_set_accel_to_pitch(pc, acc_to_pitch);
    int16_t pitch_to_v = mul_mid(pitch_rate, gs_pitch_to_v_k(pc));
    gs_set_pitch_to_speed(pc, pitch_to_v);
    uint16_t dv = (uint16_t)(accel + pitch_to_v);
    int32_t v_new = (int16_t)airspeed + (int16_t)dv;
    bool overflow = v_new != (int16_t)v_new;
    uint16_t v16 = (uint16_t)v_new;
    if (overflow) {
        cyc += JCC_TAKEN + 4 + 29;
        v16 = (uint16_t)(AIRSPEED_MAX | (dv & 0xFF));
    } else if (v16 & 0x8000) {
        cyc += 4 + JCC_TAKEN + 22 + 29;
        v16 = 0;
    } else if ((int16_t)v16 > AIRSPEED_MAX) {
        cyc += 4 + 23 + JCC_TAKEN + 4 + 29;
        v16 = (uint16_t)(AIRSPEED_MAX | (dv & 0xFF));
    } else {
        cyc += 4 + 23;
    }
    gs_set_airspeed(pc, v16);

    /* Integrate pitch, then fold a loop over the vertical (fix_loop_over works on a
     * positive pitch, so a negative one is negated around it). */
    cyc += 52;
    uint16_t pitch_step = (uint16_t)(pitch_rate + acc_to_pitch);
    uint16_t pitch = (uint16_t)((uint16_t)gs_pitch(pc) + pitch_step);
    gs_set_pitch(pc, pitch);
    uint16_t dx;
    if (pitch & 0x8000) {
        cyc += JCC_TAKEN + 45 + 20;
        stack_write(pc, (uint16_t)(sp - 2), 0x18F6);
        gs_set_pitch(pc, (uint16_t)-pitch);
        dx = loop_over(pc, &cyc);
        gs_set_pitch(pc, (uint16_t)-rd16(pc, GS_PITCH));
    } else {
        cyc += 38 + 20;
        stack_write(pc, (uint16_t)(sp - 2), 0x18CB);
        dx = loop_over(pc, &cyc);
    }

    c->regs[R_AX] = pitch_step;
    c->regs[R_BX] = tb.bx;
    c->regs[R_CX] = cx_out;
    c->regs[R_DX] = dx;
    c->regs[R_SI] = tb.si;
    c->regs[R_DI] = tb.di;
    c->regs[R_BP] = tb.bp;
    c->cycles += cyc - FLIGHT_FORCES_CYCLES;
    native_ret(pc);
}

NativeEntry native_flight_forces[] = {
    { .name = "flight_forces", .seg = GAME_CS, .off = 0x1610, .fn = n_flight_forces, .enabled = true,
      .flag_mask = 0, .cycles = FLIGHT_FORCES_CYCLES },
    { .name = "fix_loop_over", .seg = GAME_CS, .off = 0x18CE, .fn = n_fix_loop_over, .enabled = true,
      .flag_mask = 0, .cycles = FIX_LOOP_OVER_CYCLES },
    { .name = "control_coupling", .seg = GAME_CS, .off = 0x10B6, .fn = n_control_coupling, .enabled = true,
      .flag_mask = 0, .cycles = CONTROL_COUPLING_CYCLES },
    { .name = "wing_cl_offsets", .seg = GAME_CS, .off = 0x10EF, .fn = n_wing_cl_offsets, .enabled = true,
      .flag_mask = 0, .cycles = WING_CL_OFFSETS_CYCLES },
    { .name = NULL },
};
