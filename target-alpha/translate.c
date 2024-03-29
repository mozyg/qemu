/*
 *  Alpha emulation cpu translation for qemu.
 *
 *  Copyright (c) 2007 Jocelyn Mayer
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, see <http://www.gnu.org/licenses/>.
 */

#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>

#include "cpu.h"
#include "exec-all.h"
#include "disas.h"
#include "host-utils.h"
#include "tcg-op.h"
#include "qemu-common.h"

#include "helper.h"
#define GEN_HELPER 1
#include "helper.h"

#undef ALPHA_DEBUG_DISAS

#ifdef ALPHA_DEBUG_DISAS
#  define LOG_DISAS(...) qemu_log_mask(CPU_LOG_TB_IN_ASM, ## __VA_ARGS__)
#else
#  define LOG_DISAS(...) do { } while (0)
#endif

typedef struct DisasContext DisasContext;
struct DisasContext {
    uint64_t pc;
    int mem_idx;
#if !defined (CONFIG_USER_ONLY)
    int pal_mode;
#endif
    CPUAlphaState *env;
    uint32_t amask;
};

/* global register indexes */
static TCGv_ptr cpu_env;
static TCGv cpu_ir[31];
static TCGv cpu_fir[31];
static TCGv cpu_pc;
static TCGv cpu_lock;
#ifdef CONFIG_USER_ONLY
static TCGv cpu_uniq;
#endif

/* register names */
static char cpu_reg_names[10*4+21*5 + 10*5+21*6];

#include "gen-icount.h"

static void alpha_translate_init(void)
{
    int i;
    char *p;
    static int done_init = 0;

    if (done_init)
        return;

    cpu_env = tcg_global_reg_new_ptr(TCG_AREG0, "env");

    p = cpu_reg_names;
    for (i = 0; i < 31; i++) {
        sprintf(p, "ir%d", i);
        cpu_ir[i] = tcg_global_mem_new_i64(TCG_AREG0,
                                           offsetof(CPUState, ir[i]), p);
        p += (i < 10) ? 4 : 5;

        sprintf(p, "fir%d", i);
        cpu_fir[i] = tcg_global_mem_new_i64(TCG_AREG0,
                                            offsetof(CPUState, fir[i]), p);
        p += (i < 10) ? 5 : 6;
    }

    cpu_pc = tcg_global_mem_new_i64(TCG_AREG0,
                                    offsetof(CPUState, pc), "pc");

    cpu_lock = tcg_global_mem_new_i64(TCG_AREG0,
                                      offsetof(CPUState, lock), "lock");

#ifdef CONFIG_USER_ONLY
    cpu_uniq = tcg_global_mem_new_i64(TCG_AREG0,
                                      offsetof(CPUState, unique), "uniq");
#endif

    /* register helpers */
#define GEN_HELPER 2
#include "helper.h"

    done_init = 1;
}

static inline void gen_excp(DisasContext *ctx, int exception, int error_code)
{
    TCGv_i32 tmp1, tmp2;

    tcg_gen_movi_i64(cpu_pc, ctx->pc);
    tmp1 = tcg_const_i32(exception);
    tmp2 = tcg_const_i32(error_code);
    gen_helper_excp(tmp1, tmp2);
    tcg_temp_free_i32(tmp2);
    tcg_temp_free_i32(tmp1);
}

static inline void gen_invalid(DisasContext *ctx)
{
    gen_excp(ctx, EXCP_OPCDEC, 0);
}

static inline void gen_qemu_ldf(TCGv t0, TCGv t1, int flags)
{
    TCGv tmp = tcg_temp_new();
    TCGv_i32 tmp32 = tcg_temp_new_i32();
    tcg_gen_qemu_ld32u(tmp, t1, flags);
    tcg_gen_trunc_i64_i32(tmp32, tmp);
    gen_helper_memory_to_f(t0, tmp32);
    tcg_temp_free_i32(tmp32);
    tcg_temp_free(tmp);
}

static inline void gen_qemu_ldg(TCGv t0, TCGv t1, int flags)
{
    TCGv tmp = tcg_temp_new();
    tcg_gen_qemu_ld64(tmp, t1, flags);
    gen_helper_memory_to_g(t0, tmp);
    tcg_temp_free(tmp);
}

static inline void gen_qemu_lds(TCGv t0, TCGv t1, int flags)
{
    TCGv tmp = tcg_temp_new();
    TCGv_i32 tmp32 = tcg_temp_new_i32();
    tcg_gen_qemu_ld32u(tmp, t1, flags);
    tcg_gen_trunc_i64_i32(tmp32, tmp);
    gen_helper_memory_to_s(t0, tmp32);
    tcg_temp_free_i32(tmp32);
    tcg_temp_free(tmp);
}

static inline void gen_qemu_ldl_l(TCGv t0, TCGv t1, int flags)
{
    tcg_gen_mov_i64(cpu_lock, t1);
    tcg_gen_qemu_ld32s(t0, t1, flags);
}

static inline void gen_qemu_ldq_l(TCGv t0, TCGv t1, int flags)
{
    tcg_gen_mov_i64(cpu_lock, t1);
    tcg_gen_qemu_ld64(t0, t1, flags);
}

static inline void gen_load_mem(DisasContext *ctx,
                                void (*tcg_gen_qemu_load)(TCGv t0, TCGv t1,
                                                          int flags),
                                int ra, int rb, int32_t disp16, int fp,
                                int clear)
{
    TCGv addr;

    if (unlikely(ra == 31))
        return;

    addr = tcg_temp_new();
    if (rb != 31) {
        tcg_gen_addi_i64(addr, cpu_ir[rb], disp16);
        if (clear)
            tcg_gen_andi_i64(addr, addr, ~0x7);
    } else {
        if (clear)
            disp16 &= ~0x7;
        tcg_gen_movi_i64(addr, disp16);
    }
    if (fp)
        tcg_gen_qemu_load(cpu_fir[ra], addr, ctx->mem_idx);
    else
        tcg_gen_qemu_load(cpu_ir[ra], addr, ctx->mem_idx);
    tcg_temp_free(addr);
}

static inline void gen_qemu_stf(TCGv t0, TCGv t1, int flags)
{
    TCGv_i32 tmp32 = tcg_temp_new_i32();
    TCGv tmp = tcg_temp_new();
    gen_helper_f_to_memory(tmp32, t0);
    tcg_gen_extu_i32_i64(tmp, tmp32);
    tcg_gen_qemu_st32(tmp, t1, flags);
    tcg_temp_free(tmp);
    tcg_temp_free_i32(tmp32);
}

static inline void gen_qemu_stg(TCGv t0, TCGv t1, int flags)
{
    TCGv tmp = tcg_temp_new();
    gen_helper_g_to_memory(tmp, t0);
    tcg_gen_qemu_st64(tmp, t1, flags);
    tcg_temp_free(tmp);
}

static inline void gen_qemu_sts(TCGv t0, TCGv t1, int flags)
{
    TCGv_i32 tmp32 = tcg_temp_new_i32();
    TCGv tmp = tcg_temp_new();
    gen_helper_s_to_memory(tmp32, t0);
    tcg_gen_extu_i32_i64(tmp, tmp32);
    tcg_gen_qemu_st32(tmp, t1, flags);
    tcg_temp_free(tmp);
    tcg_temp_free_i32(tmp32);
}

static inline void gen_qemu_stl_c(TCGv t0, TCGv t1, int flags)
{
    int l1, l2;

    l1 = gen_new_label();
    l2 = gen_new_label();
    tcg_gen_brcond_i64(TCG_COND_NE, cpu_lock, t1, l1);
    tcg_gen_qemu_st32(t0, t1, flags);
    tcg_gen_movi_i64(t0, 1);
    tcg_gen_br(l2);
    gen_set_label(l1);
    tcg_gen_movi_i64(t0, 0);
    gen_set_label(l2);
    tcg_gen_movi_i64(cpu_lock, -1);
}

static inline void gen_qemu_stq_c(TCGv t0, TCGv t1, int flags)
{
    int l1, l2;

    l1 = gen_new_label();
    l2 = gen_new_label();
    tcg_gen_brcond_i64(TCG_COND_NE, cpu_lock, t1, l1);
    tcg_gen_qemu_st64(t0, t1, flags);
    tcg_gen_movi_i64(t0, 1);
    tcg_gen_br(l2);
    gen_set_label(l1);
    tcg_gen_movi_i64(t0, 0);
    gen_set_label(l2);
    tcg_gen_movi_i64(cpu_lock, -1);
}

static inline void gen_store_mem(DisasContext *ctx,
                                 void (*tcg_gen_qemu_store)(TCGv t0, TCGv t1,
                                                            int flags),
                                 int ra, int rb, int32_t disp16, int fp,
                                 int clear, int local)
{
    TCGv addr;
    if (local)
        addr = tcg_temp_local_new();
    else
        addr = tcg_temp_new();
    if (rb != 31) {
        tcg_gen_addi_i64(addr, cpu_ir[rb], disp16);
        if (clear)
            tcg_gen_andi_i64(addr, addr, ~0x7);
    } else {
        if (clear)
            disp16 &= ~0x7;
        tcg_gen_movi_i64(addr, disp16);
    }
    if (ra != 31) {
        if (fp)
            tcg_gen_qemu_store(cpu_fir[ra], addr, ctx->mem_idx);
        else
            tcg_gen_qemu_store(cpu_ir[ra], addr, ctx->mem_idx);
    } else {
        TCGv zero;
        if (local)
            zero = tcg_const_local_i64(0);
        else
            zero = tcg_const_i64(0);
        tcg_gen_qemu_store(zero, addr, ctx->mem_idx);
        tcg_temp_free(zero);
    }
    tcg_temp_free(addr);
}

static void gen_bcond_pcload(DisasContext *ctx, int32_t disp, int lab_true)
{
    int lab_over = gen_new_label();

    tcg_gen_movi_i64(cpu_pc, ctx->pc);
    tcg_gen_br(lab_over);
    gen_set_label(lab_true);
    tcg_gen_movi_i64(cpu_pc, ctx->pc + (int64_t)(disp << 2));
    gen_set_label(lab_over);
}

static void gen_bcond(DisasContext *ctx, TCGCond cond, int ra,
                      int32_t disp, int mask)
{
    int lab_true = gen_new_label();

    if (likely(ra != 31)) {
        if (mask) {
            TCGv tmp = tcg_temp_new();
            tcg_gen_andi_i64(tmp, cpu_ir[ra], 1);
            tcg_gen_brcondi_i64(cond, tmp, 0, lab_true);
            tcg_temp_free(tmp);
        } else {
            tcg_gen_brcondi_i64(cond, cpu_ir[ra], 0, lab_true);
        }
    } else {
        /* Very uncommon case - Do not bother to optimize.  */
        TCGv tmp = tcg_const_i64(0);
        tcg_gen_brcondi_i64(cond, tmp, 0, lab_true);
        tcg_temp_free(tmp);
    }
    gen_bcond_pcload(ctx, disp, lab_true);
}

/* Generate a forward TCG branch to LAB_TRUE if RA cmp 0.0.
   This is complicated by the fact that -0.0 compares the same as +0.0.  */

static void gen_fbcond_internal(TCGCond cond, TCGv src, int lab_true)
{
    int lab_false = -1;
    uint64_t mzero = 1ull << 63;
    TCGv tmp;

    switch (cond) {
    case TCG_COND_LE:
    case TCG_COND_GT:
        /* For <= or >, the -0.0 value directly compares the way we want.  */
        tcg_gen_brcondi_i64(cond, src, 0, lab_true);
        break;

    case TCG_COND_EQ:
    case TCG_COND_NE:
        /* For == or !=, we can simply mask off the sign bit and compare.  */
        /* ??? Assume that the temporary is reclaimed at the branch.  */
        tmp = tcg_temp_new();
        tcg_gen_andi_i64(tmp, src, mzero - 1);
        tcg_gen_brcondi_i64(cond, tmp, 0, lab_true);
        break;

    case TCG_COND_GE:
        /* For >=, emit two branches to the destination.  */
        tcg_gen_brcondi_i64(cond, src, 0, lab_true);
        tcg_gen_brcondi_i64(TCG_COND_EQ, src, mzero, lab_true);
        break;

    case TCG_COND_LT:
        /* For <, first filter out -0.0 to what will be the fallthru.  */
        lab_false = gen_new_label();
        tcg_gen_brcondi_i64(TCG_COND_EQ, src, mzero, lab_false);
        tcg_gen_brcondi_i64(cond, src, 0, lab_true);
        gen_set_label(lab_false);
        break;

    default:
        abort();
    }
}

static void gen_fbcond(DisasContext *ctx, TCGCond cond, int ra, int32_t disp)
{
    int lab_true;

    if (unlikely(ra == 31)) {
        /* Very uncommon case, but easier to optimize it to an integer
           comparison than continuing with the floating point comparison.  */
        gen_bcond(ctx, cond, ra, disp, 0);
        return;
    }

    lab_true = gen_new_label();
    gen_fbcond_internal(cond, cpu_fir[ra], lab_true);
    gen_bcond_pcload(ctx, disp, lab_true);
}

static inline void gen_cmov(TCGCond inv_cond, int ra, int rb, int rc,
                            int islit, uint8_t lit, int mask)
{
    int l1;

    if (unlikely(rc == 31))
        return;

    l1 = gen_new_label();

    if (ra != 31) {
        if (mask) {
            TCGv tmp = tcg_temp_new();
            tcg_gen_andi_i64(tmp, cpu_ir[ra], 1);
            tcg_gen_brcondi_i64(inv_cond, tmp, 0, l1);
            tcg_temp_free(tmp);
        } else
            tcg_gen_brcondi_i64(inv_cond, cpu_ir[ra], 0, l1);
    } else {
        /* Very uncommon case - Do not bother to optimize.  */
        TCGv tmp = tcg_const_i64(0);
        tcg_gen_brcondi_i64(inv_cond, tmp, 0, l1);
        tcg_temp_free(tmp);
    }

    if (islit)
        tcg_gen_movi_i64(cpu_ir[rc], lit);
    else
        tcg_gen_mov_i64(cpu_ir[rc], cpu_ir[rb]);
    gen_set_label(l1);
}

static void gen_fcmov(TCGCond inv_cond, int ra, int rb, int rc)
{
    TCGv va = cpu_fir[ra];
    int l1;

    if (unlikely(rc == 31))
        return;
    if (unlikely(ra == 31)) {
        /* ??? Assume that the temporary is reclaimed at the branch.  */
        va = tcg_const_i64(0);
    }

    l1 = gen_new_label();
    gen_fbcond_internal(inv_cond, va, l1);

    if (rb != 31)
        tcg_gen_mov_i64(cpu_fir[rc], cpu_fir[rb]);
    else
        tcg_gen_movi_i64(cpu_fir[rc], 0);
    gen_set_label(l1);
}

#define FARITH2(name)                                       \
static inline void glue(gen_f, name)(int rb, int rc)        \
{                                                           \
    if (unlikely(rc == 31))                                 \
      return;                                               \
                                                            \
    if (rb != 31)                                           \
        gen_helper_ ## name (cpu_fir[rc], cpu_fir[rb]);    \
    else {                                                  \
        TCGv tmp = tcg_const_i64(0);                        \
        gen_helper_ ## name (cpu_fir[rc], tmp);            \
        tcg_temp_free(tmp);                                 \
    }                                                       \
}
FARITH2(sqrts)
FARITH2(sqrtf)
FARITH2(sqrtg)
FARITH2(sqrtt)
FARITH2(cvtgf)
FARITH2(cvtgq)
FARITH2(cvtqf)
FARITH2(cvtqg)
FARITH2(cvtst)
FARITH2(cvtts)
FARITH2(cvttq)
FARITH2(cvtqs)
FARITH2(cvtqt)
FARITH2(cvtlq)
FARITH2(cvtql)
FARITH2(cvtqlv)
FARITH2(cvtqlsv)

#define FARITH3(name)                                                     \
static inline void glue(gen_f, name)(int ra, int rb, int rc)              \
{                                                                         \
    if (unlikely(rc == 31))                                               \
        return;                                                           \
                                                                          \
    if (ra != 31) {                                                       \
        if (rb != 31)                                                     \
            gen_helper_ ## name (cpu_fir[rc], cpu_fir[ra], cpu_fir[rb]);  \
        else {                                                            \
            TCGv tmp = tcg_const_i64(0);                                  \
            gen_helper_ ## name (cpu_fir[rc], cpu_fir[ra], tmp);          \
            tcg_temp_free(tmp);                                           \
        }                                                                 \
    } else {                                                              \
        TCGv tmp = tcg_const_i64(0);                                      \
        if (rb != 31)                                                     \
            gen_helper_ ## name (cpu_fir[rc], tmp, cpu_fir[rb]);          \
        else                                                              \
            gen_helper_ ## name (cpu_fir[rc], tmp, tmp);                   \
        tcg_temp_free(tmp);                                               \
    }                                                                     \
}

FARITH3(addf)
FARITH3(subf)
FARITH3(mulf)
FARITH3(divf)
FARITH3(addg)
FARITH3(subg)
FARITH3(mulg)
FARITH3(divg)
FARITH3(cmpgeq)
FARITH3(cmpglt)
FARITH3(cmpgle)
FARITH3(adds)
FARITH3(subs)
FARITH3(muls)
FARITH3(divs)
FARITH3(addt)
FARITH3(subt)
FARITH3(mult)
FARITH3(divt)
FARITH3(cmptun)
FARITH3(cmpteq)
FARITH3(cmptlt)
FARITH3(cmptle)
FARITH3(cpys)
FARITH3(cpysn)
FARITH3(cpyse)

static inline uint64_t zapnot_mask(uint8_t lit)
{
    uint64_t mask = 0;
    int i;

    for (i = 0; i < 8; ++i) {
        if ((lit >> i) & 1)
            mask |= 0xffull << (i * 8);
    }
    return mask;
}

/* Implement zapnot with an immediate operand, which expands to some
   form of immediate AND.  This is a basic building block in the
   definition of many of the other byte manipulation instructions.  */
static void gen_zapnoti(TCGv dest, TCGv src, uint8_t lit)
{
    switch (lit) {
    case 0x00:
        tcg_gen_movi_i64(dest, 0);
        break;
    case 0x01:
        tcg_gen_ext8u_i64(dest, src);
        break;
    case 0x03:
        tcg_gen_ext16u_i64(dest, src);
        break;
    case 0x0f:
        tcg_gen_ext32u_i64(dest, src);
        break;
    case 0xff:
        tcg_gen_mov_i64(dest, src);
        break;
    default:
        tcg_gen_andi_i64 (dest, src, zapnot_mask (lit));
        break;
    }
}

static inline void gen_zapnot(int ra, int rb, int rc, int islit, uint8_t lit)
{
    if (unlikely(rc == 31))
        return;
    else if (unlikely(ra == 31))
        tcg_gen_movi_i64(cpu_ir[rc], 0);
    else if (islit)
        gen_zapnoti(cpu_ir[rc], cpu_ir[ra], lit);
    else
        gen_helper_zapnot (cpu_ir[rc], cpu_ir[ra], cpu_ir[rb]);
}

static inline void gen_zap(int ra, int rb, int rc, int islit, uint8_t lit)
{
    if (unlikely(rc == 31))
        return;
    else if (unlikely(ra == 31))
        tcg_gen_movi_i64(cpu_ir[rc], 0);
    else if (islit)
        gen_zapnoti(cpu_ir[rc], cpu_ir[ra], ~lit);
    else
        gen_helper_zap (cpu_ir[rc], cpu_ir[ra], cpu_ir[rb]);
}


/* EXTWH, EXTLH, EXTQH */
static void gen_ext_h(int ra, int rb, int rc, int islit,
                      uint8_t lit, uint8_t byte_mask)
{
    if (unlikely(rc == 31))
        return;
    else if (unlikely(ra == 31))
        tcg_gen_movi_i64(cpu_ir[rc], 0);
    else {
        if (islit) {
            lit = (64 - (lit & 7) * 8) & 0x3f;
            tcg_gen_shli_i64(cpu_ir[rc], cpu_ir[ra], lit);
        } else {
            TCGv tmp1 = tcg_temp_new();
            tcg_gen_andi_i64(tmp1, cpu_ir[rb], 7);
            tcg_gen_shli_i64(tmp1, tmp1, 3);
            tcg_gen_neg_i64(tmp1, tmp1);
            tcg_gen_andi_i64(tmp1, tmp1, 0x3f);
            tcg_gen_shl_i64(cpu_ir[rc], cpu_ir[ra], tmp1);
            tcg_temp_free(tmp1);
        }
        gen_zapnoti(cpu_ir[rc], cpu_ir[rc], byte_mask);
    }
}

/* EXTBL, EXTWL, EXTLL, EXTQL */
static void gen_ext_l(int ra, int rb, int rc, int islit,
                      uint8_t lit, uint8_t byte_mask)
{
    if (unlikely(rc == 31))
        return;
    else if (unlikely(ra == 31))
        tcg_gen_movi_i64(cpu_ir[rc], 0);
    else {
        if (islit) {
            tcg_gen_shri_i64(cpu_ir[rc], cpu_ir[ra], (lit & 7) * 8);
        } else {
            TCGv tmp = tcg_temp_new();
            tcg_gen_andi_i64(tmp, cpu_ir[rb], 7);
            tcg_gen_shli_i64(tmp, tmp, 3);
            tcg_gen_shr_i64(cpu_ir[rc], cpu_ir[ra], tmp);
            tcg_temp_free(tmp);
        }
        gen_zapnoti(cpu_ir[rc], cpu_ir[rc], byte_mask);
    }
}

/* INSWH, INSLH, INSQH */
static void gen_ins_h(int ra, int rb, int rc, int islit,
                      uint8_t lit, uint8_t byte_mask)
{
    if (unlikely(rc == 31))
        return;
    else if (unlikely(ra == 31) || (islit && (lit & 7) == 0))
        tcg_gen_movi_i64(cpu_ir[rc], 0);
    else {
        TCGv tmp = tcg_temp_new();

        /* The instruction description has us left-shift the byte mask
           and extract bits <15:8> and apply that zap at the end.  This
           is equivalent to simply performing the zap first and shifting
           afterward.  */
        gen_zapnoti (tmp, cpu_ir[ra], byte_mask);

        if (islit) {
            /* Note that we have handled the lit==0 case above.  */
            tcg_gen_shri_i64 (cpu_ir[rc], tmp, 64 - (lit & 7) * 8);
        } else {
            TCGv shift = tcg_temp_new();

            /* If (B & 7) == 0, we need to shift by 64 and leave a zero.
               Do this portably by splitting the shift into two parts:
               shift_count-1 and 1.  Arrange for the -1 by using
               ones-complement instead of twos-complement in the negation:
               ~((B & 7) * 8) & 63.  */

            tcg_gen_andi_i64(shift, cpu_ir[rb], 7);
            tcg_gen_shli_i64(shift, shift, 3);
            tcg_gen_not_i64(shift, shift);
            tcg_gen_andi_i64(shift, shift, 0x3f);

            tcg_gen_shr_i64(cpu_ir[rc], tmp, shift);
            tcg_gen_shri_i64(cpu_ir[rc], cpu_ir[rc], 1);
            tcg_temp_free(shift);
        }
        tcg_temp_free(tmp);
    }
}

/* INSBL, INSWL, INSLL, INSQL */
static void gen_ins_l(int ra, int rb, int rc, int islit,
                      uint8_t lit, uint8_t byte_mask)
{
    if (unlikely(rc == 31))
        return;
    else if (unlikely(ra == 31))
        tcg_gen_movi_i64(cpu_ir[rc], 0);
    else {
        TCGv tmp = tcg_temp_new();

        /* The instruction description has us left-shift the byte mask
           the same number of byte slots as the data and apply the zap
           at the end.  This is equivalent to simply performing the zap
           first and shifting afterward.  */
        gen_zapnoti (tmp, cpu_ir[ra], byte_mask);

        if (islit) {
            tcg_gen_shli_i64(cpu_ir[rc], tmp, (lit & 7) * 8);
        } else {
            TCGv shift = tcg_temp_new();
            tcg_gen_andi_i64(shift, cpu_ir[rb], 7);
            tcg_gen_shli_i64(shift, shift, 3);
            tcg_gen_shl_i64(cpu_ir[rc], tmp, shift);
            tcg_temp_free(shift);
        }
        tcg_temp_free(tmp);
    }
}

/* MSKWH, MSKLH, MSKQH */
static void gen_msk_h(int ra, int rb, int rc, int islit,
                      uint8_t lit, uint8_t byte_mask)
{
    if (unlikely(rc == 31))
        return;
    else if (unlikely(ra == 31))
        tcg_gen_movi_i64(cpu_ir[rc], 0);
    else if (islit) {
        gen_zapnoti (cpu_ir[rc], cpu_ir[ra], ~((byte_mask << (lit & 7)) >> 8));
    } else {
        TCGv shift = tcg_temp_new();
        TCGv mask = tcg_temp_new();

        /* The instruction description is as above, where the byte_mask
           is shifted left, and then we extract bits <15:8>.  This can be
           emulated with a right-shift on the expanded byte mask.  This
           requires extra care because for an input <2:0> == 0 we need a
           shift of 64 bits in order to generate a zero.  This is done by
           splitting the shift into two parts, the variable shift - 1
           followed by a constant 1 shift.  The code we expand below is
           equivalent to ~((B & 7) * 8) & 63.  */

        tcg_gen_andi_i64(shift, cpu_ir[rb], 7);
        tcg_gen_shli_i64(shift, shift, 3);
        tcg_gen_not_i64(shift, shift);
        tcg_gen_andi_i64(shift, shift, 0x3f);
        tcg_gen_movi_i64(mask, zapnot_mask (byte_mask));
        tcg_gen_shr_i64(mask, mask, shift);
        tcg_gen_shri_i64(mask, mask, 1);

        tcg_gen_andc_i64(cpu_ir[rc], cpu_ir[ra], mask);

        tcg_temp_free(mask);
        tcg_temp_free(shift);
    }
}

/* MSKBL, MSKWL, MSKLL, MSKQL */
static void gen_msk_l(int ra, int rb, int rc, int islit,
                      uint8_t lit, uint8_t byte_mask)
{
    if (unlikely(rc == 31))
        return;
    else if (unlikely(ra == 31))
        tcg_gen_movi_i64(cpu_ir[rc], 0);
    else if (islit) {
        gen_zapnoti (cpu_ir[rc], cpu_ir[ra], ~(byte_mask << (lit & 7)));
    } else {
        TCGv shift = tcg_temp_new();
        TCGv mask = tcg_temp_new();

        tcg_gen_andi_i64(shift, cpu_ir[rb], 7);
        tcg_gen_shli_i64(shift, shift, 3);
        tcg_gen_movi_i64(mask, zapnot_mask (byte_mask));
        tcg_gen_shl_i64(mask, mask, shift);

        tcg_gen_andc_i64(cpu_ir[rc], cpu_ir[ra], mask);

        tcg_temp_free(mask);
        tcg_temp_free(shift);
    }
}

/* Code to call arith3 helpers */
#define ARITH3(name)                                                  \
static inline void glue(gen_, name)(int ra, int rb, int rc, int islit,\
                                    uint8_t lit)                      \
{                                                                     \
    if (unlikely(rc == 31))                                           \
        return;                                                       \
                                                                      \
    if (ra != 31) {                                                   \
        if (islit) {                                                  \
            TCGv tmp = tcg_const_i64(lit);                            \
            gen_helper_ ## name(cpu_ir[rc], cpu_ir[ra], tmp);         \
            tcg_temp_free(tmp);                                       \
        } else                                                        \
            gen_helper_ ## name (cpu_ir[rc], cpu_ir[ra], cpu_ir[rb]); \
    } else {                                                          \
        TCGv tmp1 = tcg_const_i64(0);                                 \
        if (islit) {                                                  \
            TCGv tmp2 = tcg_const_i64(lit);                           \
            gen_helper_ ## name (cpu_ir[rc], tmp1, tmp2);             \
            tcg_temp_free(tmp2);                                      \
        } else                                                        \
            gen_helper_ ## name (cpu_ir[rc], tmp1, cpu_ir[rb]);       \
        tcg_temp_free(tmp1);                                          \
    }                                                                 \
}
ARITH3(cmpbge)
ARITH3(addlv)
ARITH3(sublv)
ARITH3(addqv)
ARITH3(subqv)
ARITH3(umulh)
ARITH3(mullv)
ARITH3(mulqv)
ARITH3(minub8)
ARITH3(minsb8)
ARITH3(minuw4)
ARITH3(minsw4)
ARITH3(maxub8)
ARITH3(maxsb8)
ARITH3(maxuw4)
ARITH3(maxsw4)
ARITH3(perr)

#define MVIOP2(name)                                    \
static inline void glue(gen_, name)(int rb, int rc)     \
{                                                       \
    if (unlikely(rc == 31))                             \
        return;                                         \
    if (unlikely(rb == 31))                             \
        tcg_gen_movi_i64(cpu_ir[rc], 0);                \
    else                                                \
        gen_helper_ ## name (cpu_ir[rc], cpu_ir[rb]);   \
}
MVIOP2(pklb)
MVIOP2(pkwb)
MVIOP2(unpkbl)
MVIOP2(unpkbw)

static inline void gen_cmp(TCGCond cond, int ra, int rb, int rc, int islit,
                           uint8_t lit)
{
    int l1, l2;
    TCGv tmp;

    if (unlikely(rc == 31))
        return;

    l1 = gen_new_label();
    l2 = gen_new_label();

    if (ra != 31) {
        tmp = tcg_temp_new();
        tcg_gen_mov_i64(tmp, cpu_ir[ra]);
    } else
        tmp = tcg_const_i64(0);
    if (islit)
        tcg_gen_brcondi_i64(cond, tmp, lit, l1);
    else
        tcg_gen_brcond_i64(cond, tmp, cpu_ir[rb], l1);

    tcg_gen_movi_i64(cpu_ir[rc], 0);
    tcg_gen_br(l2);
    gen_set_label(l1);
    tcg_gen_movi_i64(cpu_ir[rc], 1);
    gen_set_label(l2);
}

static inline int translate_one(DisasContext *ctx, uint32_t insn)
{
    uint32_t palcode;
    int32_t disp21, disp16, disp12;
    uint16_t fn11, fn16;
    uint8_t opc, ra, rb, rc, sbz, fpfn, fn7, fn2, islit, real_islit;
    uint8_t lit;
    int ret;

    /* Decode all instruction fields */
    opc = insn >> 26;
    ra = (insn >> 21) & 0x1F;
    rb = (insn >> 16) & 0x1F;
    rc = insn & 0x1F;
    sbz = (insn >> 13) & 0x07;
    real_islit = islit = (insn >> 12) & 1;
    if (rb == 31 && !islit) {
        islit = 1;
        lit = 0;
    } else
        lit = (insn >> 13) & 0xFF;
    palcode = insn & 0x03FFFFFF;
    disp21 = ((int32_t)((insn & 0x001FFFFF) << 11)) >> 11;
    disp16 = (int16_t)(insn & 0x0000FFFF);
    disp12 = (int32_t)((insn & 0x00000FFF) << 20) >> 20;
    fn16 = insn & 0x0000FFFF;
    fn11 = (insn >> 5) & 0x000007FF;
    fpfn = fn11 & 0x3F;
    fn7 = (insn >> 5) & 0x0000007F;
    fn2 = (insn >> 5) & 0x00000003;
    ret = 0;
    LOG_DISAS("opc %02x ra %2d rb %2d rc %2d disp16 %6d\n",
              opc, ra, rb, rc, disp16);

    switch (opc) {
    case 0x00:
        /* CALL_PAL */
#ifdef CONFIG_USER_ONLY
        if (palcode == 0x9E) {
            /* RDUNIQUE */
            tcg_gen_mov_i64(cpu_ir[IR_V0], cpu_uniq);
            break;
        } else if (palcode == 0x9F) {
            /* WRUNIQUE */
            tcg_gen_mov_i64(cpu_uniq, cpu_ir[IR_A0]);
            break;
        }
#endif
        if (palcode >= 0x80 && palcode < 0xC0) {
            /* Unprivileged PAL call */
            gen_excp(ctx, EXCP_CALL_PAL + ((palcode & 0x3F) << 6), 0);
            ret = 3;
            break;
        }
#ifndef CONFIG_USER_ONLY
        if (palcode < 0x40) {
            /* Privileged PAL code */
            if (ctx->mem_idx & 1)
                goto invalid_opc;
            gen_excp(ctx, EXCP_CALL_PALP + ((palcode & 0x3F) << 6), 0);
            ret = 3;
        }
#endif
        /* Invalid PAL call */
        goto invalid_opc;
    case 0x01:
        /* OPC01 */
        goto invalid_opc;
    case 0x02:
        /* OPC02 */
        goto invalid_opc;
    case 0x03:
        /* OPC03 */
        goto invalid_opc;
    case 0x04:
        /* OPC04 */
        goto invalid_opc;
    case 0x05:
        /* OPC05 */
        goto invalid_opc;
    case 0x06:
        /* OPC06 */
        goto invalid_opc;
    case 0x07:
        /* OPC07 */
        goto invalid_opc;
    case 0x08:
        /* LDA */
        if (likely(ra != 31)) {
            if (rb != 31)
                tcg_gen_addi_i64(cpu_ir[ra], cpu_ir[rb], disp16);
            else
                tcg_gen_movi_i64(cpu_ir[ra], disp16);
        }
        break;
    case 0x09:
        /* LDAH */
        if (likely(ra != 31)) {
            if (rb != 31)
                tcg_gen_addi_i64(cpu_ir[ra], cpu_ir[rb], disp16 << 16);
            else
                tcg_gen_movi_i64(cpu_ir[ra], disp16 << 16);
        }
        break;
    case 0x0A:
        /* LDBU */
        if (!(ctx->amask & AMASK_BWX))
            goto invalid_opc;
        gen_load_mem(ctx, &tcg_gen_qemu_ld8u, ra, rb, disp16, 0, 0);
        break;
    case 0x0B:
        /* LDQ_U */
        gen_load_mem(ctx, &tcg_gen_qemu_ld64, ra, rb, disp16, 0, 1);
        break;
    case 0x0C:
        /* LDWU */
        if (!(ctx->amask & AMASK_BWX))
            goto invalid_opc;
        gen_load_mem(ctx, &tcg_gen_qemu_ld16u, ra, rb, disp16, 0, 0);
        break;
    case 0x0D:
        /* STW */
        gen_store_mem(ctx, &tcg_gen_qemu_st16, ra, rb, disp16, 0, 0, 0);
        break;
    case 0x0E:
        /* STB */
        gen_store_mem(ctx, &tcg_gen_qemu_st8, ra, rb, disp16, 0, 0, 0);
        break;
    case 0x0F:
        /* STQ_U */
        gen_store_mem(ctx, &tcg_gen_qemu_st64, ra, rb, disp16, 0, 1, 0);
        break;
    case 0x10:
        switch (fn7) {
        case 0x00:
            /* ADDL */
            if (likely(rc != 31)) {
                if (ra != 31) {
                    if (islit) {
                        tcg_gen_addi_i64(cpu_ir[rc], cpu_ir[ra], lit);
                        tcg_gen_ext32s_i64(cpu_ir[rc], cpu_ir[rc]);
                    } else {
                        tcg_gen_add_i64(cpu_ir[rc], cpu_ir[ra], cpu_ir[rb]);
                        tcg_gen_ext32s_i64(cpu_ir[rc], cpu_ir[rc]);
                    }
                } else {
                    if (islit)
                        tcg_gen_movi_i64(cpu_ir[rc], lit);
                    else
                        tcg_gen_ext32s_i64(cpu_ir[rc], cpu_ir[rb]);
                }
            }
            break;
        case 0x02:
            /* S4ADDL */
            if (likely(rc != 31)) {
                if (ra != 31) {
                    TCGv tmp = tcg_temp_new();
                    tcg_gen_shli_i64(tmp, cpu_ir[ra], 2);
                    if (islit)
                        tcg_gen_addi_i64(tmp, tmp, lit);
                    else
                        tcg_gen_add_i64(tmp, tmp, cpu_ir[rb]);
                    tcg_gen_ext32s_i64(cpu_ir[rc], tmp);
                    tcg_temp_free(tmp);
                } else {
                    if (islit)
                        tcg_gen_movi_i64(cpu_ir[rc], lit);
                    else
                        tcg_gen_ext32s_i64(cpu_ir[rc], cpu_ir[rb]);
                }
            }
            break;
        case 0x09:
            /* SUBL */
            if (likely(rc != 31)) {
                if (ra != 31) {
                    if (islit)
                        tcg_gen_subi_i64(cpu_ir[rc], cpu_ir[ra], lit);
                    else
                        tcg_gen_sub_i64(cpu_ir[rc], cpu_ir[ra], cpu_ir[rb]);
                    tcg_gen_ext32s_i64(cpu_ir[rc], cpu_ir[rc]);
                } else {
                    if (islit)
                        tcg_gen_movi_i64(cpu_ir[rc], -lit);
                    else {
                        tcg_gen_neg_i64(cpu_ir[rc], cpu_ir[rb]);
                        tcg_gen_ext32s_i64(cpu_ir[rc], cpu_ir[rc]);
                }
            }
            break;
        case 0x0B:
            /* S4SUBL */
            if (likely(rc != 31)) {
                if (ra != 31) {
                    TCGv tmp = tcg_temp_new();
                    tcg_gen_shli_i64(tmp, cpu_ir[ra], 2);
                    if (islit)
                        tcg_gen_subi_i64(tmp, tmp, lit);
                    else
                        tcg_gen_sub_i64(tmp, tmp, cpu_ir[rb]);
                    tcg_gen_ext32s_i64(cpu_ir[rc], tmp);
                    tcg_temp_free(tmp);
                } else {
                    if (islit)
                        tcg_gen_movi_i64(cpu_ir[rc], -lit);
                    else {
                        tcg_gen_neg_i64(cpu_ir[rc], cpu_ir[rb]);
                        tcg_gen_ext32s_i64(cpu_ir[rc], cpu_ir[rc]);
                    }
                }
            }
            break;
        case 0x0F:
            /* CMPBGE */
            gen_cmpbge(ra, rb, rc, islit, lit);
            break;
        case 0x12:
            /* S8ADDL */
            if (likely(rc != 31)) {
                if (ra != 31) {
                    TCGv tmp = tcg_temp_new();
                    tcg_gen_shli_i64(tmp, cpu_ir[ra], 3);
                    if (islit)
                        tcg_gen_addi_i64(tmp, tmp, lit);
                    else
                        tcg_gen_add_i64(tmp, tmp, cpu_ir[rb]);
                    tcg_gen_ext32s_i64(cpu_ir[rc], tmp);
                    tcg_temp_free(tmp);
                } else {
                    if (islit)
                        tcg_gen_movi_i64(cpu_ir[rc], lit);
                    else
                        tcg_gen_ext32s_i64(cpu_ir[rc], cpu_ir[rb]);
                }
            }
            break;
        case 0x1B:
            /* S8SUBL */
            if (likely(rc != 31)) {
                if (ra != 31) {
                    TCGv tmp = tcg_temp_new();
                    tcg_gen_shli_i64(tmp, cpu_ir[ra], 3);
                    if (islit)
                        tcg_gen_subi_i64(tmp, tmp, lit);
                    else
                       tcg_gen_sub_i64(tmp, tmp, cpu_ir[rb]);
                    tcg_gen_ext32s_i64(cpu_ir[rc], tmp);
                    tcg_temp_free(tmp);
                } else {
                    if (islit)
                        tcg_gen_movi_i64(cpu_ir[rc], -lit);
                    else
                        tcg_gen_neg_i64(cpu_ir[rc], cpu_ir[rb]);
                        tcg_gen_ext32s_i64(cpu_ir[rc], cpu_ir[rc]);
                    }
                }
            }
            break;
        case 0x1D:
            /* CMPULT */
            gen_cmp(TCG_COND_LTU, ra, rb, rc, islit, lit);
            break;
        case 0x20:
            /* ADDQ */
            if (likely(rc != 31)) {
                if (ra != 31) {
                    if (islit)
                        tcg_gen_addi_i64(cpu_ir[rc], cpu_ir[ra], lit);
                    else
                        tcg_gen_add_i64(cpu_ir[rc], cpu_ir[ra], cpu_ir[rb]);
                } else {
                    if (islit)
                        tcg_gen_movi_i64(cpu_ir[rc], lit);
                    else
                        tcg_gen_mov_i64(cpu_ir[rc], cpu_ir[rb]);
                }
            }
            break;
        case 0x22:
            /* S4ADDQ */
            if (likely(rc != 31)) {
                if (ra != 31) {
                    TCGv tmp = tcg_temp_new();
                    tcg_gen_shli_i64(tmp, cpu_ir[ra], 2);
                    if (islit)
                        tcg_gen_addi_i64(cpu_ir[rc], tmp, lit);
                    else
                        tcg_gen_add_i64(cpu_ir[rc], tmp, cpu_ir[rb]);
                    tcg_temp_free(tmp);
                } else {
                    if (islit)
                        tcg_gen_movi_i64(cpu_ir[rc], lit);
                    else
                        tcg_gen_mov_i64(cpu_ir[rc], cpu_ir[rb]);
                }
            }
            break;
        case 0x29:
            /* SUBQ */
            if (likely(rc != 31)) {
                if (ra != 31) {
                    if (islit)
                        tcg_gen_subi_i64(cpu_ir[rc], cpu_ir[ra], lit);
                    else
                        tcg_gen_sub_i64(cpu_ir[rc], cpu_ir[ra], cpu_ir[rb]);
                } else {
                    if (islit)
                        tcg_gen_movi_i64(cpu_ir[rc], -lit);
                    else
                        tcg_gen_neg_i64(cpu_ir[rc], cpu_ir[rb]);
                }
            }
            break;
        case 0x2B:
            /* S4SUBQ */
            if (likely(rc != 31)) {
                if (ra != 31) {
                    TCGv tmp = tcg_temp_new();
                    tcg_gen_shli_i64(tmp, cpu_ir[ra], 2);
                    if (islit)
                        tcg_gen_subi_i64(cpu_ir[rc], tmp, lit);
                    else
                        tcg_gen_sub_i64(cpu_ir[rc], tmp, cpu_ir[rb]);
                    tcg_temp_free(tmp);
                } else {
                    if (islit)
                        tcg_gen_movi_i64(cpu_ir[rc], -lit);
                    else
                        tcg_gen_neg_i64(cpu_ir[rc], cpu_ir[rb]);
                }
            }
            break;
        case 0x2D:
            /* CMPEQ */
            gen_cmp(TCG_COND_EQ, ra, rb, rc, islit, lit);
            break;
        case 0x32:
            /* S8ADDQ */
            if (likely(rc != 31)) {
                if (ra != 31) {
                    TCGv tmp = tcg_temp_new();
                    tcg_gen_shli_i64(tmp, cpu_ir[ra], 3);
                    if (islit)
                        tcg_gen_addi_i64(cpu_ir[rc], tmp, lit);
                    else
                        tcg_gen_add_i64(cpu_ir[rc], tmp, cpu_ir[rb]);
                    tcg_temp_free(tmp);
                } else {
                    if (islit)
                        tcg_gen_movi_i64(cpu_ir[rc], lit);
                    else
                        tcg_gen_mov_i64(cpu_ir[rc], cpu_ir[rb]);
                }
            }
            break;
        case 0x3B:
            /* S8SUBQ */
            if (likely(rc != 31)) {
                if (ra != 31) {
                    TCGv tmp = tcg_temp_new();
                    tcg_gen_shli_i64(tmp, cpu_ir[ra], 3);
                    if (islit)
                        tcg_gen_subi_i64(cpu_ir[rc], tmp, lit);
                    else
                        tcg_gen_sub_i64(cpu_ir[rc], tmp, cpu_ir[rb]);
                    tcg_temp_free(tmp);
                } else {
                    if (islit)
                        tcg_gen_movi_i64(cpu_ir[rc], -lit);
                    else
                        tcg_gen_neg_i64(cpu_ir[rc], cpu_ir[rb]);
                }
            }
            break;
        case 0x3D:
            /* CMPULE */
            gen_cmp(TCG_COND_LEU, ra, rb, rc, islit, lit);
            break;
        case 0x40:
            /* ADDL/V */
            gen_addlv(ra, rb, rc, islit, lit);
            break;
        case 0x49:
            /* SUBL/V */
            gen_sublv(ra, rb, rc, islit, lit);
            break;
        case 0x4D:
            /* CMPLT */
            gen_cmp(TCG_COND_LT, ra, rb, rc, islit, lit);
            break;
        case 0x60:
            /* ADDQ/V */
            gen_addqv(ra, rb, rc, islit, lit);
            break;
        case 0x69:
            /* SUBQ/V */
            gen_subqv(ra, rb, rc, islit, lit);
            break;
        case 0x6D:
            /* CMPLE */
            gen_cmp(TCG_COND_LE, ra, rb, rc, islit, lit);
            break;
        default:
            goto invalid_opc;
        }
        break;
    case 0x11:
        switch (fn7) {
        case 0x00:
            /* AND */
            if (likely(rc != 31)) {
                if (ra == 31)
                    tcg_gen_movi_i64(cpu_ir[rc], 0);
                else if (islit)
                    tcg_gen_andi_i64(cpu_ir[rc], cpu_ir[ra], lit);
                else
                    tcg_gen_and_i64(cpu_ir[rc], cpu_ir[ra], cpu_ir[rb]);
            }
            break;
        case 0x08:
            /* BIC */
            if (likely(rc != 31)) {
                if (ra != 31) {
                    if (islit)
                        tcg_gen_andi_i64(cpu_ir[rc], cpu_ir[ra], ~lit);
                    else
                        tcg_gen_andc_i64(cpu_ir[rc], cpu_ir[ra], cpu_ir[rb]);
                } else
                    tcg_gen_movi_i64(cpu_ir[rc], 0);
            }
            break;
        case 0x14:
            /* CMOVLBS */
            gen_cmov(TCG_COND_EQ, ra, rb, rc, islit, lit, 1);
            break;
        case 0x16:
            /* CMOVLBC */
            gen_cmov(TCG_COND_NE, ra, rb, rc, islit, lit, 1);
            break;
        case 0x20:
            /* BIS */
            if (likely(rc != 31)) {
                if (ra != 31) {
                    if (islit)
                        tcg_gen_ori_i64(cpu_ir[rc], cpu_ir[ra], lit);
                    else
                        tcg_gen_or_i64(cpu_ir[rc], cpu_ir[ra], cpu_ir[rb]);
                } else {
                    if (islit)
                        tcg_gen_movi_i64(cpu_ir[rc], lit);
                    else
                        tcg_gen_mov_i64(cpu_ir[rc], cpu_ir[rb]);
                }
            }
            break;
        case 0x24:
            /* CMOVEQ */
            gen_cmov(TCG_COND_NE, ra, rb, rc, islit, lit, 0);
            break;
        case 0x26:
            /* CMOVNE */
            gen_cmov(TCG_COND_EQ, ra, rb, rc, islit, lit, 0);
            break;
        case 0x28:
            /* ORNOT */
            if (likely(rc != 31)) {
                if (ra != 31) {
                    if (islit)
                        tcg_gen_ori_i64(cpu_ir[rc], cpu_ir[ra], ~lit);
                    else
                        tcg_gen_orc_i64(cpu_ir[rc], cpu_ir[ra], cpu_ir[rb]);
                } else {
                    if (islit)
                        tcg_gen_movi_i64(cpu_ir[rc], ~lit);
                    else
                        tcg_gen_not_i64(cpu_ir[rc], cpu_ir[rb]);
                }
            }
            break;
        case 0x40:
            /* XOR */
            if (likely(rc != 31)) {
                if (ra != 31) {
                    if (islit)
                        tcg_gen_xori_i64(cpu_ir[rc], cpu_ir[ra], lit);
                    else
                        tcg_gen_xor_i64(cpu_ir[rc], cpu_ir[ra], cpu_ir[rb]);
                } else {
                    if (islit)
                        tcg_gen_movi_i64(cpu_ir[rc], lit);
                    else
                        tcg_gen_mov_i64(cpu_ir[rc], cpu_ir[rb]);
                }
            }
            break;
        case 0x44:
            /* CMOVLT */
            gen_cmov(TCG_COND_GE, ra, rb, rc, islit, lit, 0);
            break;
        case 0x46:
            /* CMOVGE */
            gen_cmov(TCG_COND_LT, ra, rb, rc, islit, lit, 0);
            break;
        case 0x48:
            /* EQV */
            if (likely(rc != 31)) {
                if (ra != 31) {
                    if (islit)
                        tcg_gen_xori_i64(cpu_ir[rc], cpu_ir[ra], ~lit);
                    else
                        tcg_gen_eqv_i64(cpu_ir[rc], cpu_ir[ra], cpu_ir[rb]);
                } else {
                    if (islit)
                        tcg_gen_movi_i64(cpu_ir[rc], ~lit);
                    else
                        tcg_gen_not_i64(cpu_ir[rc], cpu_ir[rb]);
                }
            }
            break;
        case 0x61:
            /* AMASK */
            if (likely(rc != 31)) {
                if (islit)
                    tcg_gen_movi_i64(cpu_ir[rc], lit);
                else
                    tcg_gen_mov_i64(cpu_ir[rc], cpu_ir[rb]);
                switch (ctx->env->implver) {
                case IMPLVER_2106x:
                    /* EV4, EV45, LCA, LCA45 & EV5 */
                    break;
                case IMPLVER_21164:
                case IMPLVER_21264:
                case IMPLVER_21364:
                    tcg_gen_andi_i64(cpu_ir[rc], cpu_ir[rc],
                                     ~(uint64_t)ctx->amask);
                    break;
                }
            }
            break;
        case 0x64:
            /* CMOVLE */
            gen_cmov(TCG_COND_GT, ra, rb, rc, islit, lit, 0);
            break;
        case 0x66:
            /* CMOVGT */
            gen_cmov(TCG_COND_LE, ra, rb, rc, islit, lit, 0);
            break;
        case 0x6C:
            /* IMPLVER */
            if (rc != 31)
                tcg_gen_movi_i64(cpu_ir[rc], ctx->env->implver);
            break;
        default:
            goto invalid_opc;
        }
        break;
    case 0x12:
        switch (fn7) {
        case 0x02:
            /* MSKBL */
            gen_msk_l(ra, rb, rc, islit, lit, 0x01);
            break;
        case 0x06:
            /* EXTBL */
            gen_ext_l(ra, rb, rc, islit, lit, 0x01);
            break;
        case 0x0B:
            /* INSBL */
            gen_ins_l(ra, rb, rc, islit, lit, 0x01);
            break;
        case 0x12:
            /* MSKWL */
            gen_msk_l(ra, rb, rc, islit, lit, 0x03);
            break;
        case 0x16:
            /* EXTWL */
            gen_ext_l(ra, rb, rc, islit, lit, 0x03);
            break;
        case 0x1B:
            /* INSWL */
            gen_ins_l(ra, rb, rc, islit, lit, 0x03);
            break;
        case 0x22:
            /* MSKLL */
            gen_msk_l(ra, rb, rc, islit, lit, 0x0f);
            break;
        case 0x26:
            /* EXTLL */
            gen_ext_l(ra, rb, rc, islit, lit, 0x0f);
            break;
        case 0x2B:
            /* INSLL */
            gen_ins_l(ra, rb, rc, islit, lit, 0x0f);
            break;
        case 0x30:
            /* ZAP */
            gen_zap(ra, rb, rc, islit, lit);
            break;
        case 0x31:
            /* ZAPNOT */
            gen_zapnot(ra, rb, rc, islit, lit);
            break;
        case 0x32:
            /* MSKQL */
            gen_msk_l(ra, rb, rc, islit, lit, 0xff);
            break;
        case 0x34:
            /* SRL */
            if (likely(rc != 31)) {
                if (ra != 31) {
                    if (islit)
                        tcg_gen_shri_i64(cpu_ir[rc], cpu_ir[ra], lit & 0x3f);
                    else {
                        TCGv shift = tcg_temp_new();
                        tcg_gen_andi_i64(shift, cpu_ir[rb], 0x3f);
                        tcg_gen_shr_i64(cpu_ir[rc], cpu_ir[ra], shift);
                        tcg_temp_free(shift);
                    }
                } else
                    tcg_gen_movi_i64(cpu_ir[rc], 0);
            }
            break;
        case 0x36:
            /* EXTQL */
            gen_ext_l(ra, rb, rc, islit, lit, 0xff);
            break;
        case 0x39:
            /* SLL */
            if (likely(rc != 31)) {
                if (ra != 31) {
                    if (islit)
                        tcg_gen_shli_i64(cpu_ir[rc], cpu_ir[ra], lit & 0x3f);
                    else {
                        TCGv shift = tcg_temp_new();
                        tcg_gen_andi_i64(shift, cpu_ir[rb], 0x3f);
                        tcg_gen_shl_i64(cpu_ir[rc], cpu_ir[ra], shift);
                        tcg_temp_free(shift);
                    }
                } else
                    tcg_gen_movi_i64(cpu_ir[rc], 0);
            }
            break;
        case 0x3B:
            /* INSQL */
            gen_ins_l(ra, rb, rc, islit, lit, 0xff);
            break;
        case 0x3C:
            /* SRA */
            if (likely(rc != 31)) {
                if (ra != 31) {
                    if (islit)
                        tcg_gen_sari_i64(cpu_ir[rc], cpu_ir[ra], lit & 0x3f);
                    else {
                        TCGv shift = tcg_temp_new();
                        tcg_gen_andi_i64(shift, cpu_ir[rb], 0x3f);
                        tcg_gen_sar_i64(cpu_ir[rc], cpu_ir[ra], shift);
                        tcg_temp_free(shift);
                    }
                } else
                    tcg_gen_movi_i64(cpu_ir[rc], 0);
            }
            break;
        case 0x52:
            /* MSKWH */
            gen_msk_h(ra, rb, rc, islit, lit, 0x03);
            break;
        case 0x57:
            /* INSWH */
            gen_ins_h(ra, rb, rc, islit, lit, 0x03);
            break;
        case 0x5A:
            /* EXTWH */
            gen_ext_h(ra, rb, rc, islit, lit, 0x03);
            break;
        case 0x62:
            /* MSKLH */
            gen_msk_h(ra, rb, rc, islit, lit, 0x0f);
            break;
        case 0x67:
            /* INSLH */
            gen_ins_h(ra, rb, rc, islit, lit, 0x0f);
            break;
        case 0x6A:
            /* EXTLH */
            gen_ext_h(ra, rb, rc, islit, lit, 0x0f);
            break;
        case 0x72:
            /* MSKQH */
            gen_msk_h(ra, rb, rc, islit, lit, 0xff);
            break;
        case 0x77:
            /* INSQH */
            gen_ins_h(ra, rb, rc, islit, lit, 0xff);
            break;
        case 0x7A:
            /* EXTQH */
            gen_ext_h(ra, rb, rc, islit, lit, 0xff);
            break;
        default:
            goto invalid_opc;
        }
        break;
    case 0x13:
        switch (fn7) {
        case 0x00:
            /* MULL */
            if (likely(rc != 31)) {
                if (ra == 31)
                    tcg_gen_movi_i64(cpu_ir[rc], 0);
                else {
                    if (islit)
                        tcg_gen_muli_i64(cpu_ir[rc], cpu_ir[ra], lit);
                    else
                        tcg_gen_mul_i64(cpu_ir[rc], cpu_ir[ra], cpu_ir[rb]);
                    tcg_gen_ext32s_i64(cpu_ir[rc], cpu_ir[rc]);
                }
            }
            break;
        case 0x20:
            /* MULQ */
            if (likely(rc != 31)) {
                if (ra == 31)
                    tcg_gen_movi_i64(cpu_ir[rc], 0);
                else if (islit)
                    tcg_gen_muli_i64(cpu_ir[rc], cpu_ir[ra], lit);
                else
                    tcg_gen_mul_i64(cpu_ir[rc], cpu_ir[ra], cpu_ir[rb]);
            }
            break;
        case 0x30:
            /* UMULH */
            gen_umulh(ra, rb, rc, islit, lit);
            break;
        case 0x40:
            /* MULL/V */
            gen_mullv(ra, rb, rc, islit, lit);
            break;
        case 0x60:
            /* MULQ/V */
            gen_mulqv(ra, rb, rc, islit, lit);
            break;
        default:
            goto invalid_opc;
        }
        break;
    case 0x14:
        switch (fpfn) { /* f11 & 0x3F */
        case 0x04:
            /* ITOFS */
            if (!(ctx->amask & AMASK_FIX))
                goto invalid_opc;
            if (likely(rc != 31)) {
                if (ra != 31) {
                    TCGv_i32 tmp = tcg_temp_new_i32();
                    tcg_gen_trunc_i64_i32(tmp, cpu_ir[ra]);
                    gen_helper_memory_to_s(cpu_fir[rc], tmp);
                    tcg_temp_free_i32(tmp);
                } else
                    tcg_gen_movi_i64(cpu_fir[rc], 0);
            }
            break;
        case 0x0A:
            /* SQRTF */
            if (!(ctx->amask & AMASK_FIX))
                goto invalid_opc;
            gen_fsqrtf(rb, rc);
            break;
        case 0x0B:
            /* SQRTS */
            if (!(ctx->amask & AMASK_FIX))
                goto invalid_opc;
            gen_fsqrts(rb, rc);
            break;
        case 0x14:
            /* ITOFF */
            if (!(ctx->amask & AMASK_FIX))
                goto invalid_opc;
            if (likely(rc != 31)) {
                if (ra != 31) {
                    TCGv_i32 tmp = tcg_temp_new_i32();
                    tcg_gen_trunc_i64_i32(tmp, cpu_ir[ra]);
                    gen_helper_memory_to_f(cpu_fir[rc], tmp);
                    tcg_temp_free_i32(tmp);
                } else
                    tcg_gen_movi_i64(cpu_fir[rc], 0);
            }
            break;
        case 0x24:
            /* ITOFT */
            if (!(ctx->amask & AMASK_FIX))
                goto invalid_opc;
            if (likely(rc != 31)) {
                if (ra != 31)
                    tcg_gen_mov_i64(cpu_fir[rc], cpu_ir[ra]);
                else
                    tcg_gen_movi_i64(cpu_fir[rc], 0);
            }
            break;
        case 0x2A:
            /* SQRTG */
            if (!(ctx->amask & AMASK_FIX))
                goto invalid_opc;
            gen_fsqrtg(rb, rc);
            break;
        case 0x02B:
            /* SQRTT */
            if (!(ctx->amask & AMASK_FIX))
                goto invalid_opc;
            gen_fsqrtt(rb, rc);
            break;
        default:
            goto invalid_opc;
        }
        break;
    case 0x15:
        /* VAX floating point */
        /* XXX: rounding mode and trap are ignored (!) */
        switch (fpfn) { /* f11 & 0x3F */
        case 0x00:
            /* ADDF */
            gen_faddf(ra, rb, rc);
            break;
        case 0x01:
            /* SUBF */
            gen_fsubf(ra, rb, rc);
            break;
        case 0x02:
            /* MULF */
            gen_fmulf(ra, rb, rc);
            break;
        case 0x03:
            /* DIVF */
            gen_fdivf(ra, rb, rc);
            break;
        case 0x1E:
            /* CVTDG */
#if 0 // TODO
            gen_fcvtdg(rb, rc);
#else
            goto invalid_opc;
#endif
            break;
        case 0x20:
            /* ADDG */
            gen_faddg(ra, rb, rc);
            break;
        case 0x21:
            /* SUBG */
            gen_fsubg(ra, rb, rc);
            break;
        case 0x22:
            /* MULG */
            gen_fmulg(ra, rb, rc);
            break;
        case 0x23:
            /* DIVG */
            gen_fdivg(ra, rb, rc);
            break;
        case 0x25:
            /* CMPGEQ */
            gen_fcmpgeq(ra, rb, rc);
            break;
        case 0x26:
            /* CMPGLT */
            gen_fcmpglt(ra, rb, rc);
            break;
        case 0x27:
            /* CMPGLE */
            gen_fcmpgle(ra, rb, rc);
            break;
        case 0x2C:
            /* CVTGF */
            gen_fcvtgf(rb, rc);
            break;
        case 0x2D:
            /* CVTGD */
#if 0 // TODO
            gen_fcvtgd(rb, rc);
#else
            goto invalid_opc;
#endif
            break;
        case 0x2F:
            /* CVTGQ */
            gen_fcvtgq(rb, rc);
            break;
        case 0x3C:
            /* CVTQF */
            gen_fcvtqf(rb, rc);
            break;
        case 0x3E:
            /* CVTQG */
            gen_fcvtqg(rb, rc);
            break;
        default:
            goto invalid_opc;
        }
        break;
    case 0x16:
        /* IEEE floating-point */
        /* XXX: rounding mode and traps are ignored (!) */
        switch (fpfn) { /* f11 & 0x3F */
        case 0x00:
            /* ADDS */
            gen_fadds(ra, rb, rc);
            break;
        case 0x01:
            /* SUBS */
            gen_fsubs(ra, rb, rc);
            break;
        case 0x02:
            /* MULS */
            gen_fmuls(ra, rb, rc);
            break;
        case 0x03:
            /* DIVS */
            gen_fdivs(ra, rb, rc);
            break;
        case 0x20:
            /* ADDT */
            gen_faddt(ra, rb, rc);
            break;
        case 0x21:
            /* SUBT */
            gen_fsubt(ra, rb, rc);
            break;
        case 0x22:
            /* MULT */
            gen_fmult(ra, rb, rc);
            break;
        case 0x23:
            /* DIVT */
            gen_fdivt(ra, rb, rc);
            break;
        case 0x24:
            /* CMPTUN */
            gen_fcmptun(ra, rb, rc);
            break;
        case 0x25:
            /* CMPTEQ */
            gen_fcmpteq(ra, rb, rc);
            break;
        case 0x26:
            /* CMPTLT */
            gen_fcmptlt(ra, rb, rc);
            break;
        case 0x27:
            /* CMPTLE */
            gen_fcmptle(ra, rb, rc);
            break;
        case 0x2C:
            /* XXX: incorrect */
            if (fn11 == 0x2AC || fn11 == 0x6AC) {
                /* CVTST */
                gen_fcvtst(rb, rc);
            } else {
                /* CVTTS */
                gen_fcvtts(rb, rc);
            }
            break;
        case 0x2F:
            /* CVTTQ */
            gen_fcvttq(rb, rc);
            break;
        case 0x3C:
            /* CVTQS */
            gen_fcvtqs(rb, rc);
            break;
        case 0x3E:
            /* CVTQT */
            gen_fcvtqt(rb, rc);
            break;
        default:
            goto invalid_opc;
        }
        break;
    case 0x17:
        switch (fn11) {
        case 0x010:
            /* CVTLQ */
            gen_fcvtlq(rb, rc);
            break;
        case 0x020:
            if (likely(rc != 31)) {
                if (ra == rb) {
                    /* FMOV */
                    if (ra == 31)
                        tcg_gen_movi_i64(cpu_fir[rc], 0);
                    else
                        tcg_gen_mov_i64(cpu_fir[rc], cpu_fir[ra]);
                } else {
                    /* CPYS */
                    gen_fcpys(ra, rb, rc);
                }
            }
            break;
        case 0x021:
            /* CPYSN */
            gen_fcpysn(ra, rb, rc);
            break;
        case 0x022:
            /* CPYSE */
            gen_fcpyse(ra, rb, rc);
            break;
        case 0x024:
            /* MT_FPCR */
            if (likely(ra != 31))
                gen_helper_store_fpcr(cpu_fir[ra]);
            else {
                TCGv tmp = tcg_const_i64(0);
                gen_helper_store_fpcr(tmp);
                tcg_temp_free(tmp);
            }
            break;
        case 0x025:
            /* MF_FPCR */
            if (likely(ra != 31))
                gen_helper_load_fpcr(cpu_fir[ra]);
            break;
        case 0x02A:
            /* FCMOVEQ */
            gen_fcmov(TCG_COND_NE, ra, rb, rc);
            break;
        case 0x02B:
            /* FCMOVNE */
            gen_fcmov(TCG_COND_EQ, ra, rb, rc);
            break;
        case 0x02C:
            /* FCMOVLT */
            gen_fcmov(TCG_COND_GE, ra, rb, rc);
            break;
        case 0x02D:
            /* FCMOVGE */
            gen_fcmov(TCG_COND_LT, ra, rb, rc);
            break;
        case 0x02E:
            /* FCMOVLE */
            gen_fcmov(TCG_COND_GT, ra, rb, rc);
            break;
        case 0x02F:
            /* FCMOVGT */
            gen_fcmov(TCG_COND_LE, ra, rb, rc);
            break;
        case 0x030:
            /* CVTQL */
            gen_fcvtql(rb, rc);
            break;
        case 0x130:
            /* CVTQL/V */
            gen_fcvtqlv(rb, rc);
            break;
        case 0x530:
            /* CVTQL/SV */
            gen_fcvtqlsv(rb, rc);
            break;
        default:
            goto invalid_opc;
        }
        break;
    case 0x18:
        switch ((uint16_t)disp16) {
        case 0x0000:
            /* TRAPB */
            /* No-op. Just exit from the current tb */
            ret = 2;
            break;
        case 0x0400:
            /* EXCB */
            /* No-op. Just exit from the current tb */
            ret = 2;
            break;
        case 0x4000:
            /* MB */
            /* No-op */
            break;
        case 0x4400:
            /* WMB */
            /* No-op */
            break;
        case 0x8000:
            /* FETCH */
            /* No-op */
            break;
        case 0xA000:
            /* FETCH_M */
            /* No-op */
            break;
        case 0xC000:
            /* RPCC */
            if (ra != 31)
                gen_helper_load_pcc(cpu_ir[ra]);
            break;
        case 0xE000:
            /* RC */
            if (ra != 31)
                gen_helper_rc(cpu_ir[ra]);
            break;
        case 0xE800:
            /* ECB */
            break;
        case 0xF000:
            /* RS */
            if (ra != 31)
                gen_helper_rs(cpu_ir[ra]);
            break;
        case 0xF800:
            /* WH64 */
            /* No-op */
            break;
        default:
            goto invalid_opc;
        }
        break;
    case 0x19:
        /* HW_MFPR (PALcode) */
#if defined (CONFIG_USER_ONLY)
        goto invalid_opc;
#else
        if (!ctx->pal_mode)
            goto invalid_opc;
        if (ra != 31) {
            TCGv tmp = tcg_const_i32(insn & 0xFF);
            gen_helper_mfpr(cpu_ir[ra], tmp, cpu_ir[ra]);
            tcg_temp_free(tmp);
        }
        break;
#endif
    case 0x1A:
        if (rb != 31)
            tcg_gen_andi_i64(cpu_pc, cpu_ir[rb], ~3);
        else
            tcg_gen_movi_i64(cpu_pc, 0);
        if (ra != 31)
            tcg_gen_movi_i64(cpu_ir[ra], ctx->pc);
        /* Those four jumps only differ by the branch prediction hint */
        switch (fn2) {
        case 0x0:
            /* JMP */
            break;
        case 0x1:
            /* JSR */
            break;
        case 0x2:
            /* RET */
            break;
        case 0x3:
            /* JSR_COROUTINE */
            break;
        }
        ret = 1;
        break;
    case 0x1B:
        /* HW_LD (PALcode) */
#if defined (CONFIG_USER_ONLY)
        goto invalid_opc;
#else
        if (!ctx->pal_mode)
            goto invalid_opc;
        if (ra != 31) {
            TCGv addr = tcg_temp_new();
            if (rb != 31)
                tcg_gen_addi_i64(addr, cpu_ir[rb], disp12);
            else
                tcg_gen_movi_i64(addr, disp12);
            switch ((insn >> 12) & 0xF) {
            case 0x0:
                /* Longword physical access (hw_ldl/p) */
                gen_helper_ldl_raw(cpu_ir[ra], addr);
                break;
            case 0x1:
                /* Quadword physical access (hw_ldq/p) */
                gen_helper_ldq_raw(cpu_ir[ra], addr);
                break;
            case 0x2:
                /* Longword physical access with lock (hw_ldl_l/p) */
                gen_helper_ldl_l_raw(cpu_ir[ra], addr);
                break;
            case 0x3:
                /* Quadword physical access with lock (hw_ldq_l/p) */
                gen_helper_ldq_l_raw(cpu_ir[ra], addr);
                break;
            case 0x4:
                /* Longword virtual PTE fetch (hw_ldl/v) */
                tcg_gen_qemu_ld32s(cpu_ir[ra], addr, 0);
                break;
            case 0x5:
                /* Quadword virtual PTE fetch (hw_ldq/v) */
                tcg_gen_qemu_ld64(cpu_ir[ra], addr, 0);
                break;
            case 0x6:
                /* Incpu_ir[ra]id */
                goto invalid_opc;
            case 0x7:
                /* Incpu_ir[ra]id */
                goto invalid_opc;
            case 0x8:
                /* Longword virtual access (hw_ldl) */
                gen_helper_st_virt_to_phys(addr, addr);
                gen_helper_ldl_raw(cpu_ir[ra], addr);
                break;
            case 0x9:
                /* Quadword virtual access (hw_ldq) */
                gen_helper_st_virt_to_phys(addr, addr);
                gen_helper_ldq_raw(cpu_ir[ra], addr);
                break;
            case 0xA:
                /* Longword virtual access with protection check (hw_ldl/w) */
                tcg_gen_qemu_ld32s(cpu_ir[ra], addr, 0);
                break;
            case 0xB:
                /* Quadword virtual access with protection check (hw_ldq/w) */
                tcg_gen_qemu_ld64(cpu_ir[ra], addr, 0);
                break;
            case 0xC:
                /* Longword virtual access with alt access mode (hw_ldl/a)*/
                gen_helper_set_alt_mode();
                gen_helper_st_virt_to_phys(addr, addr);
                gen_helper_ldl_raw(cpu_ir[ra], addr);
                gen_helper_restore_mode();
                break;
            case 0xD:
                /* Quadword virtual access with alt access mode (hw_ldq/a) */
                gen_helper_set_alt_mode();
                gen_helper_st_virt_to_phys(addr, addr);
                gen_helper_ldq_raw(cpu_ir[ra], addr);
                gen_helper_restore_mode();
                break;
            case 0xE:
                /* Longword virtual access with alternate access mode and
                 * protection checks (hw_ldl/wa)
                 */
                gen_helper_set_alt_mode();
                gen_helper_ldl_data(cpu_ir[ra], addr);
                gen_helper_restore_mode();
                break;
            case 0xF:
                /* Quadword virtual access with alternate access mode and
                 * protection checks (hw_ldq/wa)
                 */
                gen_helper_set_alt_mode();
                gen_helper_ldq_data(cpu_ir[ra], addr);
                gen_helper_restore_mode();
                break;
            }
            tcg_temp_free(addr);
        }
        break;
#endif
    case 0x1C:
        switch (fn7) {
        case 0x00:
            /* SEXTB */
            if (!(ctx->amask & AMASK_BWX))
                goto invalid_opc;
            if (likely(rc != 31)) {
                if (islit)
                    tcg_gen_movi_i64(cpu_ir[rc], (int64_t)((int8_t)lit));
                else
                    tcg_gen_ext8s_i64(cpu_ir[rc], cpu_ir[rb]);
            }
            break;
        case 0x01:
            /* SEXTW */
            if (!(ctx->amask & AMASK_BWX))
                goto invalid_opc;
            if (likely(rc != 31)) {
                if (islit)
                    tcg_gen_movi_i64(cpu_ir[rc], (int64_t)((int16_t)lit));
                else
                    tcg_gen_ext16s_i64(cpu_ir[rc], cpu_ir[rb]);
            }
            break;
        case 0x30:
            /* CTPOP */
            if (!(ctx->amask & AMASK_CIX))
                goto invalid_opc;
            if (likely(rc != 31)) {
                if (islit)
                    tcg_gen_movi_i64(cpu_ir[rc], ctpop64(lit));
                else
                    gen_helper_ctpop(cpu_ir[rc], cpu_ir[rb]);
            }
            break;
        case 0x31:
            /* PERR */
            if (!(ctx->amask & AMASK_MVI))
                goto invalid_opc;
            gen_perr(ra, rb, rc, islit, lit);
            break;
        case 0x32:
            /* CTLZ */
            if (!(ctx->amask & AMASK_CIX))
                goto invalid_opc;
            if (likely(rc != 31)) {
                if (islit)
                    tcg_gen_movi_i64(cpu_ir[rc], clz64(lit));
                else
                    gen_helper_ctlz(cpu_ir[rc], cpu_ir[rb]);
            }
            break;
        case 0x33:
            /* CTTZ */
            if (!(ctx->amask & AMASK_CIX))
                goto invalid_opc;
            if (likely(rc != 31)) {
                if (islit)
                    tcg_gen_movi_i64(cpu_ir[rc], ctz64(lit));
                else
                    gen_helper_cttz(cpu_ir[rc], cpu_ir[rb]);
            }
            break;
        case 0x34:
            /* UNPKBW */
            if (!(ctx->amask & AMASK_MVI))
                goto invalid_opc;
            if (real_islit || ra != 31)
                goto invalid_opc;
            gen_unpkbw (rb, rc);
            break;
        case 0x35:
            /* UNPKBL */
            if (!(ctx->amask & AMASK_MVI))
                goto invalid_opc;
            if (real_islit || ra != 31)
                goto invalid_opc;
            gen_unpkbl (rb, rc);
            break;
        case 0x36:
            /* PKWB */
            if (!(ctx->amask & AMASK_MVI))
                goto invalid_opc;
            if (real_islit || ra != 31)
                goto invalid_opc;
            gen_pkwb (rb, rc);
            break;
        case 0x37:
            /* PKLB */
            if (!(ctx->amask & AMASK_MVI))
                goto invalid_opc;
            if (real_islit || ra != 31)
                goto invalid_opc;
            gen_pklb (rb, rc);
            break;
        case 0x38:
            /* MINSB8 */
            if (!(ctx->amask & AMASK_MVI))
                goto invalid_opc;
            gen_minsb8 (ra, rb, rc, islit, lit);
            break;
        case 0x39:
            /* MINSW4 */
            if (!(ctx->amask & AMASK_MVI))
                goto invalid_opc;
            gen_minsw4 (ra, rb, rc, islit, lit);
            break;
        case 0x3A:
            /* MINUB8 */
            if (!(ctx->amask & AMASK_MVI))
                goto invalid_opc;
            gen_minub8 (ra, rb, rc, islit, lit);
            break;
        case 0x3B:
            /* MINUW4 */
            if (!(ctx->amask & AMASK_MVI))
                goto invalid_opc;
            gen_minuw4 (ra, rb, rc, islit, lit);
            break;
        case 0x3C:
            /* MAXUB8 */
            if (!(ctx->amask & AMASK_MVI))
                goto invalid_opc;
            gen_maxub8 (ra, rb, rc, islit, lit);
            break;
        case 0x3D:
            /* MAXUW4 */
            if (!(ctx->amask & AMASK_MVI))
                goto invalid_opc;
            gen_maxuw4 (ra, rb, rc, islit, lit);
            break;
        case 0x3E:
            /* MAXSB8 */
            if (!(ctx->amask & AMASK_MVI))
                goto invalid_opc;
            gen_maxsb8 (ra, rb, rc, islit, lit);
            break;
        case 0x3F:
            /* MAXSW4 */
            if (!(ctx->amask & AMASK_MVI))
                goto invalid_opc;
            gen_maxsw4 (ra, rb, rc, islit, lit);
            break;
        case 0x70:
            /* FTOIT */
            if (!(ctx->amask & AMASK_FIX))
                goto invalid_opc;
            if (likely(rc != 31)) {
                if (ra != 31)
                    tcg_gen_mov_i64(cpu_ir[rc], cpu_fir[ra]);
                else
                    tcg_gen_movi_i64(cpu_ir[rc], 0);
            }
            break;
        case 0x78:
            /* FTOIS */
            if (!(ctx->amask & AMASK_FIX))
                goto invalid_opc;
            if (rc != 31) {
                TCGv_i32 tmp1 = tcg_temp_new_i32();
                if (ra != 31)
                    gen_helper_s_to_memory(tmp1, cpu_fir[ra]);
                else {
                    TCGv tmp2 = tcg_const_i64(0);
                    gen_helper_s_to_memory(tmp1, tmp2);
                    tcg_temp_free(tmp2);
                }
                tcg_gen_ext_i32_i64(cpu_ir[rc], tmp1);
                tcg_temp_free_i32(tmp1);
            }
            break;
        default:
            goto invalid_opc;
        }
        break;
    case 0x1D:
        /* HW_MTPR (PALcode) */
#if defined (CONFIG_USER_ONLY)
        goto invalid_opc;
#else
        if (!ctx->pal_mode)
            goto invalid_opc;
        else {
            TCGv tmp1 = tcg_const_i32(insn & 0xFF);
            if (ra != 31)
                gen_helper_mtpr(tmp1, cpu_ir[ra]);
            else {
                TCGv tmp2 = tcg_const_i64(0);
                gen_helper_mtpr(tmp1, tmp2);
                tcg_temp_free(tmp2);
            }
            tcg_temp_free(tmp1);
            ret = 2;
        }
        break;
#endif
    case 0x1E:
        /* HW_REI (PALcode) */
#if defined (CONFIG_USER_ONLY)
        goto invalid_opc;
#else
        if (!ctx->pal_mode)
            goto invalid_opc;
        if (rb == 31) {
            /* "Old" alpha */
            gen_helper_hw_rei();
        } else {
            TCGv tmp;

            if (ra != 31) {
                tmp = tcg_temp_new();
                tcg_gen_addi_i64(tmp, cpu_ir[rb], (((int64_t)insn << 51) >> 51));
            } else
                tmp = tcg_const_i64(((int64_t)insn << 51) >> 51);
            gen_helper_hw_ret(tmp);
            tcg_temp_free(tmp);
        }
        ret = 2;
        break;
#endif
    case 0x1F:
        /* HW_ST (PALcode) */
#if defined (CONFIG_USER_ONLY)
        goto invalid_opc;
#else
        if (!ctx->pal_mode)
            goto invalid_opc;
        else {
            TCGv addr, val;
            addr = tcg_temp_new();
            if (rb != 31)
                tcg_gen_addi_i64(addr, cpu_ir[rb], disp12);
            else
                tcg_gen_movi_i64(addr, disp12);
            if (ra != 31)
                val = cpu_ir[ra];
            else {
                val = tcg_temp_new();
                tcg_gen_movi_i64(val, 0);
            }
            switch ((insn >> 12) & 0xF) {
            case 0x0:
                /* Longword physical access */
                gen_helper_stl_raw(val, addr);
                break;
            case 0x1:
                /* Quadword physical access */
                gen_helper_stq_raw(val, addr);
                break;
            case 0x2:
                /* Longword physical access with lock */
                gen_helper_stl_c_raw(val, val, addr);
                break;
            case 0x3:
                /* Quadword physical access with lock */
                gen_helper_stq_c_raw(val, val, addr);
                break;
            case 0x4:
                /* Longword virtual access */
                gen_helper_st_virt_to_phys(addr, addr);
                gen_helper_stl_raw(val, addr);
                break;
            case 0x5:
                /* Quadword virtual access */
                gen_helper_st_virt_to_phys(addr, addr);
                gen_helper_stq_raw(val, addr);
                break;
            case 0x6:
                /* Invalid */
                goto invalid_opc;
            case 0x7:
                /* Invalid */
                goto invalid_opc;
            case 0x8:
                /* Invalid */
                goto invalid_opc;
            case 0x9:
                /* Invalid */
                goto invalid_opc;
            case 0xA:
                /* Invalid */
                goto invalid_opc;
            case 0xB:
                /* Invalid */
                goto invalid_opc;
            case 0xC:
                /* Longword virtual access with alternate access mode */
                gen_helper_set_alt_mode();
                gen_helper_st_virt_to_phys(addr, addr);
                gen_helper_stl_raw(val, addr);
                gen_helper_restore_mode();
                break;
            case 0xD:
                /* Quadword virtual access with alternate access mode */
                gen_helper_set_alt_mode();
                gen_helper_st_virt_to_phys(addr, addr);
                gen_helper_stl_raw(val, addr);
                gen_helper_restore_mode();
                break;
            case 0xE:
                /* Invalid */
                goto invalid_opc;
            case 0xF:
                /* Invalid */
                goto invalid_opc;
            }
            if (ra == 31)
                tcg_temp_free(val);
            tcg_temp_free(addr);
        }
        break;
#endif
    case 0x20:
        /* LDF */
        gen_load_mem(ctx, &gen_qemu_ldf, ra, rb, disp16, 1, 0);
        break;
    case 0x21:
        /* LDG */
        gen_load_mem(ctx, &gen_qemu_ldg, ra, rb, disp16, 1, 0);
        break;
    case 0x22:
        /* LDS */
        gen_load_mem(ctx, &gen_qemu_lds, ra, rb, disp16, 1, 0);
        break;
    case 0x23:
        /* LDT */
        gen_load_mem(ctx, &tcg_gen_qemu_ld64, ra, rb, disp16, 1, 0);
        break;
    case 0x24:
        /* STF */
        gen_store_mem(ctx, &gen_qemu_stf, ra, rb, disp16, 1, 0, 0);
        break;
    case 0x25:
        /* STG */
        gen_store_mem(ctx, &gen_qemu_stg, ra, rb, disp16, 1, 0, 0);
        break;
    case 0x26:
        /* STS */
        gen_store_mem(ctx, &gen_qemu_sts, ra, rb, disp16, 1, 0, 0);
        break;
    case 0x27:
        /* STT */
        gen_store_mem(ctx, &tcg_gen_qemu_st64, ra, rb, disp16, 1, 0, 0);
        break;
    case 0x28:
        /* LDL */
        gen_load_mem(ctx, &tcg_gen_qemu_ld32s, ra, rb, disp16, 0, 0);
        break;
    case 0x29:
        /* LDQ */
        gen_load_mem(ctx, &tcg_gen_qemu_ld64, ra, rb, disp16, 0, 0);
        break;
    case 0x2A:
        /* LDL_L */
        gen_load_mem(ctx, &gen_qemu_ldl_l, ra, rb, disp16, 0, 0);
        break;
    case 0x2B:
        /* LDQ_L */
        gen_load_mem(ctx, &gen_qemu_ldq_l, ra, rb, disp16, 0, 0);
        break;
    case 0x2C:
        /* STL */
        gen_store_mem(ctx, &tcg_gen_qemu_st32, ra, rb, disp16, 0, 0, 0);
        break;
    case 0x2D:
        /* STQ */
        gen_store_mem(ctx, &tcg_gen_qemu_st64, ra, rb, disp16, 0, 0, 0);
        break;
    case 0x2E:
        /* STL_C */
        gen_store_mem(ctx, &gen_qemu_stl_c, ra, rb, disp16, 0, 0, 1);
        break;
    case 0x2F:
        /* STQ_C */
        gen_store_mem(ctx, &gen_qemu_stq_c, ra, rb, disp16, 0, 0, 1);
        break;
    case 0x30:
        /* BR */
        if (ra != 31)
            tcg_gen_movi_i64(cpu_ir[ra], ctx->pc);
        tcg_gen_movi_i64(cpu_pc, ctx->pc + (int64_t)(disp21 << 2));
        ret = 1;
        break;
    case 0x31: /* FBEQ */
        gen_fbcond(ctx, TCG_COND_EQ, ra, disp21);
        ret = 1;
        break;
    case 0x32: /* FBLT */
        gen_fbcond(ctx, TCG_COND_LT, ra, disp21);
        ret = 1;
        break;
    case 0x33: /* FBLE */
        gen_fbcond(ctx, TCG_COND_LE, ra, disp21);
        ret = 1;
        break;
    case 0x34:
        /* BSR */
        if (ra != 31)
            tcg_gen_movi_i64(cpu_ir[ra], ctx->pc);
        tcg_gen_movi_i64(cpu_pc, ctx->pc + (int64_t)(disp21 << 2));
        ret = 1;
        break;
    case 0x35: /* FBNE */
        gen_fbcond(ctx, TCG_COND_NE, ra, disp21);
        ret = 1;
        break;
    case 0x36: /* FBGE */
        gen_fbcond(ctx, TCG_COND_GE, ra, disp21);
        ret = 1;
        break;
    case 0x37: /* FBGT */
        gen_fbcond(ctx, TCG_COND_GT, ra, disp21);
        ret = 1;
        break;
    case 0x38:
        /* BLBC */
        gen_bcond(ctx, TCG_COND_EQ, ra, disp21, 1);
        ret = 1;
        break;
    case 0x39:
        /* BEQ */
        gen_bcond(ctx, TCG_COND_EQ, ra, disp21, 0);
        ret = 1;
        break;
    case 0x3A:
        /* BLT */
        gen_bcond(ctx, TCG_COND_LT, ra, disp21, 0);
        ret = 1;
        break;
    case 0x3B:
        /* BLE */
        gen_bcond(ctx, TCG_COND_LE, ra, disp21, 0);
        ret = 1;
        break;
    case 0x3C:
        /* BLBS */
        gen_bcond(ctx, TCG_COND_NE, ra, disp21, 1);
        ret = 1;
        break;
    case 0x3D:
        /* BNE */
        gen_bcond(ctx, TCG_COND_NE, ra, disp21, 0);
        ret = 1;
        break;
    case 0x3E:
        /* BGE */
        gen_bcond(ctx, TCG_COND_GE, ra, disp21, 0);
        ret = 1;
        break;
    case 0x3F:
        /* BGT */
        gen_bcond(ctx, TCG_COND_GT, ra, disp21, 0);
        ret = 1;
        break;
    invalid_opc:
        gen_invalid(ctx);
        ret = 3;
        break;
    }

    return ret;
}

static inline void gen_intermediate_code_internal(CPUState *env,
                                                  TranslationBlock *tb,
                                                  int search_pc)
{
    DisasContext ctx, *ctxp = &ctx;
    target_ulong pc_start;
    uint32_t insn;
    uint16_t *gen_opc_end;
    CPUBreakpoint *bp;
    int j, lj = -1;
    int ret;
    int num_insns;
    int max_insns;

    pc_start = tb->pc;
    gen_opc_end = gen_opc_buf + OPC_MAX_SIZE;
    ctx.pc = pc_start;
    ctx.amask = env->amask;
    ctx.env = env;
#if defined (CONFIG_USER_ONLY)
    ctx.mem_idx = 0;
#else
    ctx.mem_idx = ((env->ps >> 3) & 3);
    ctx.pal_mode = env->ipr[IPR_EXC_ADDR] & 1;
#endif
    num_insns = 0;
    max_insns = tb->cflags & CF_COUNT_MASK;
    if (max_insns == 0)
        max_insns = CF_COUNT_MASK;

    gen_icount_start();
    for (ret = 0; ret == 0;) {
        if (unlikely(!QTAILQ_EMPTY(&env->breakpoints))) {
            QTAILQ_FOREACH(bp, &env->breakpoints, entry) {
                if (bp->pc == ctx.pc) {
                    gen_excp(&ctx, EXCP_DEBUG, 0);
                    break;
                }
            }
        }
        if (search_pc) {
            j = gen_opc_ptr - gen_opc_buf;
            if (lj < j) {
                lj++;
                while (lj < j)
                    gen_opc_instr_start[lj++] = 0;
            }
            gen_opc_pc[lj] = ctx.pc;
            gen_opc_instr_start[lj] = 1;
            gen_opc_icount[lj] = num_insns;
        }
        if (num_insns + 1 == max_insns && (tb->cflags & CF_LAST_IO))
            gen_io_start();
        insn = ldl_code(ctx.pc);
        num_insns++;

	if (unlikely(qemu_loglevel_mask(CPU_LOG_TB_OP))) {
            tcg_gen_debug_insn_start(ctx.pc);
        }

        ctx.pc += 4;
        ret = translate_one(ctxp, insn);
        if (ret != 0)
            break;
        /* if we reach a page boundary or are single stepping, stop
         * generation
         */
        if (env->singlestep_enabled) {
            gen_excp(&ctx, EXCP_DEBUG, 0);
            break;
        }

        if ((ctx.pc & (TARGET_PAGE_SIZE - 1)) == 0)
            break;

        if (gen_opc_ptr >= gen_opc_end)
            break;

        if (num_insns >= max_insns)
            break;

        if (singlestep) {
            break;
        }
    }
    if (ret != 1 && ret != 3) {
        tcg_gen_movi_i64(cpu_pc, ctx.pc);
    }
    if (tb->cflags & CF_LAST_IO)
        gen_io_end();
    /* Generate the return instruction */
    tcg_gen_exit_tb(0);
    gen_icount_end(tb, num_insns);
    *gen_opc_ptr = INDEX_op_end;
    if (search_pc) {
        j = gen_opc_ptr - gen_opc_buf;
        lj++;
        while (lj <= j)
            gen_opc_instr_start[lj++] = 0;
    } else {
        tb->size = ctx.pc - pc_start;
        tb->icount = num_insns;
    }
#ifdef DEBUG_DISAS
    if (qemu_loglevel_mask(CPU_LOG_TB_IN_ASM)) {
        qemu_log("IN: %s\n", lookup_symbol(pc_start));
        log_target_disas(pc_start, ctx.pc - pc_start, 1);
        qemu_log("\n");
    }
#endif
}

void gen_intermediate_code (CPUState *env, struct TranslationBlock *tb)
{
    gen_intermediate_code_internal(env, tb, 0);
}

void gen_intermediate_code_pc (CPUState *env, struct TranslationBlock *tb)
{
    gen_intermediate_code_internal(env, tb, 1);
}

struct cpu_def_t {
    const char *name;
    int implver, amask;
};

static const struct cpu_def_t cpu_defs[] = {
    { "ev4",   IMPLVER_2106x, 0 },
    { "ev5",   IMPLVER_21164, 0 },
    { "ev56",  IMPLVER_21164, AMASK_BWX },
    { "pca56", IMPLVER_21164, AMASK_BWX | AMASK_MVI },
    { "ev6",   IMPLVER_21264, AMASK_BWX | AMASK_FIX | AMASK_MVI | AMASK_TRAP },
    { "ev67",  IMPLVER_21264, (AMASK_BWX | AMASK_FIX | AMASK_CIX
			       | AMASK_MVI | AMASK_TRAP | AMASK_PREFETCH), },
    { "ev68",  IMPLVER_21264, (AMASK_BWX | AMASK_FIX | AMASK_CIX
			       | AMASK_MVI | AMASK_TRAP | AMASK_PREFETCH), },
    { "21064", IMPLVER_2106x, 0 },
    { "21164", IMPLVER_21164, 0 },
    { "21164a", IMPLVER_21164, AMASK_BWX },
    { "21164pc", IMPLVER_21164, AMASK_BWX | AMASK_MVI },
    { "21264", IMPLVER_21264, AMASK_BWX | AMASK_FIX | AMASK_MVI | AMASK_TRAP },
    { "21264a", IMPLVER_21264, (AMASK_BWX | AMASK_FIX | AMASK_CIX
				| AMASK_MVI | AMASK_TRAP | AMASK_PREFETCH), }
};

CPUAlphaState * cpu_alpha_init (const char *cpu_model)
{
    CPUAlphaState *env;
    int implver, amask, i, max;

    env = qemu_mallocz(sizeof(CPUAlphaState));
    cpu_exec_init(env);
    alpha_translate_init();
    tlb_flush(env, 1);

    /* Default to ev67; no reason not to emulate insns by default.  */
    implver = IMPLVER_21264;
    amask = (AMASK_BWX | AMASK_FIX | AMASK_CIX | AMASK_MVI
	     | AMASK_TRAP | AMASK_PREFETCH);

    max = ARRAY_SIZE(cpu_defs);
    for (i = 0; i < max; i++) {
        if (strcmp (cpu_model, cpu_defs[i].name) == 0) {
            implver = cpu_defs[i].implver;
            amask = cpu_defs[i].amask;
            break;
        }
    }
    env->implver = implver;
    env->amask = amask;

    env->ps = 0x1F00;
#if defined (CONFIG_USER_ONLY)
    env->ps |= 1 << 3;
    cpu_alpha_store_fpcr(env, (FPCR_INVD | FPCR_DZED | FPCR_OVFD
                               | FPCR_UNFD | FPCR_INED | FPCR_DNOD));
#endif
    pal_init(env);

    /* Initialize IPR */
#if defined (CONFIG_USER_ONLY)
    env->ipr[IPR_EXC_ADDR] = 0;
    env->ipr[IPR_EXC_SUM] = 0;
    env->ipr[IPR_EXC_MASK] = 0;
#else
    {
        uint64_t hwpcb;
        hwpcb = env->ipr[IPR_PCBB];
        env->ipr[IPR_ASN] = 0;
        env->ipr[IPR_ASTEN] = 0;
        env->ipr[IPR_ASTSR] = 0;
        env->ipr[IPR_DATFX] = 0;
        /* XXX: fix this */
        //    env->ipr[IPR_ESP] = ldq_raw(hwpcb + 8);
        //    env->ipr[IPR_KSP] = ldq_raw(hwpcb + 0);
        //    env->ipr[IPR_SSP] = ldq_raw(hwpcb + 16);
        //    env->ipr[IPR_USP] = ldq_raw(hwpcb + 24);
        env->ipr[IPR_FEN] = 0;
        env->ipr[IPR_IPL] = 31;
        env->ipr[IPR_MCES] = 0;
        env->ipr[IPR_PERFMON] = 0; /* Implementation specific */
        //    env->ipr[IPR_PTBR] = ldq_raw(hwpcb + 32);
        env->ipr[IPR_SISR] = 0;
        env->ipr[IPR_VIRBND] = -1ULL;
    }
#endif

    qemu_init_vcpu(env);
    return env;
}

void gen_pc_load(CPUState *env, TranslationBlock *tb,
                unsigned long searched_pc, int pc_pos, void *puc)
{
    env->pc = gen_opc_pc[pc_pos];
}
