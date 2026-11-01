/*
 * Black Arrow: native versions of hot game routines
 *
 * Rainbow Six 3: Black Arrow (title update 201) converts its menu background
 * video from YUV to RGB with an MMX loop at 0x4bd0e0. Under TCG that loop is
 * about half of the game CPU's time on every menu. When the translator meets
 * exactly those code bytes it calls helper_xbl_yuv_hle instead, which does the
 * same work in C: the same memory reads and writes, the same registers, flags
 * and MMX registers on return. XEMU_YUV_HLE=0 turns it off, =1 forces it on.
 * It is on by default only where the SSE2 loop is built: the plain C path
 * (e.g. on arm64 Macs) is slower than the JIT.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include "qemu/osdep.h"
#include "qemu/bswap.h"
#include "cpu.h"
#include "exec/helper-proto.h"
#include "accel/tcg/cpu-ldst.h"
#include "accel/tcg/probe.h"
#include "exec/target_page.h"
#include "exec/tb-flush.h"
#include "helper-tcg.h"

#define YUV_PC 0x4bd0e0u

/* The routine, 0x4bd0e0 up to its "ret 4" */
static const uint8_t yuv_code[] = {
#include "xbl_hle_yuv.inc"
};

/* Its data: row pointers, the four colour tables, the constants */
#define G_DST0 0x6512c0u
#define G_DST1 0x6512c4u
#define G_Y0 0x6512c8u
#define G_Y1 0x6512ccu
#define G_U 0x6512d0u
#define G_V 0x6512d4u
#define T_START 0x652310u /* tables at 0x652310, 710, b10, f10 */
#define T_LEN 0x1000u
#define K_START 0x581488u /* constants at 0x581488, 490, 498 */
#define K_LEN 24u

static int yuv_enabled = -1;

bool xbl_yuv_hle_enabled(void)
{
    if (yuv_enabled < 0) {
        const char *e = getenv("XEMU_YUV_HLE");
#if defined(__SSE2__)
        yuv_enabled = !(e && e[0] == '0');
#else
        yuv_enabled = e && e[0] == '1';
#endif
    }
    return yuv_enabled;
}

bool xbl_yuv_hle_match(CPUX86State *env, target_ulong pc)
{
    if (pc != YUV_PC || !xbl_yuv_hle_enabled()) {
        return false;
    }
    for (size_t i = 0; i < sizeof(yuv_code); i++) {
        if (cpu_ldub_code(env, pc + i) != yuv_code[i]) {
            return false;
        }
    }
    return true;
}

/* A guest range reached through host pointers, one per page */
typedef struct Span {
    uint32_t start, len;
    uint8_t *host[17]; /* 64 KB row + 1 */
} Span;

static bool span_probe(CPUX86State *env, Span *s, uint32_t start,
                       uint32_t len, MMUAccessType type, int mmu_idx,
                       uintptr_t ra)
{
    s->start = start;
    s->len = len;
    uint32_t first = start & TARGET_PAGE_MASK;
    uint32_t last = (start + len - 1) & TARGET_PAGE_MASK;
    if (len == 0 || start + len < start ||
        ((last - first) >> TARGET_PAGE_BITS) >= ARRAY_SIZE(s->host)) {
        return false;
    }
    for (uint32_t page = first;; page += TARGET_PAGE_SIZE) {
        uint32_t a = MAX(page, start);
        uint32_t b = MIN(page + TARGET_PAGE_SIZE, start + len);
        uint8_t *h = probe_access(env, a, b - a, type, mmu_idx, ra);
        if (!h) {
            return false;
        }
        s->host[(page - first) >> TARGET_PAGE_BITS] = h - (a - page);
        if (page == last) {
            break;
        }
    }
    return true;
}

static inline uint8_t *span_at(const Span *s, uint32_t addr)
{
    uint32_t first = s->start & TARGET_PAGE_MASK;
    return s->host[((addr & TARGET_PAGE_MASK) - first) >> TARGET_PAGE_BITS] +
           (addr & ~TARGET_PAGE_MASK);
}

static inline void span_read(const Span *s, uint32_t addr, void *out, int n)
{
    if ((addr & ~TARGET_PAGE_MASK) + n <= TARGET_PAGE_SIZE) {
        memcpy(out, span_at(s, addr), n);
    } else {
        for (int i = 0; i < n; i++) {
            ((uint8_t *)out)[i] = *span_at(s, addr + i);
        }
    }
}

static inline void span_write(const Span *s, uint32_t addr, const void *in,
                              int n)
{
    if ((addr & ~TARGET_PAGE_MASK) + n <= TARGET_PAGE_SIZE) {
        memcpy(span_at(s, addr), in, n);
    } else {
        for (int i = 0; i < n; i++) {
            *span_at(s, addr + i) = ((const uint8_t *)in)[i];
        }
    }
}

/* MMX lanes, as the instructions define them */
static inline int16_t w(uint64_t v, int i) { return v >> (16 * i); }

static inline uint64_t pack_w(const int32_t *x)
{
    uint64_t v = 0;
    for (int i = 0; i < 4; i++) {
        v |= (uint64_t)(uint16_t)x[i] << (16 * i);
    }
    return v;
}

static inline uint64_t paddw(uint64_t a, uint64_t b)
{
    int32_t r[4];
    for (int i = 0; i < 4; i++) r[i] = (uint16_t)(w(a, i) + w(b, i));
    return pack_w(r);
}

static inline uint64_t paddsw(uint64_t a, uint64_t b)
{
    int32_t r[4];
    for (int i = 0; i < 4; i++) {
        int32_t s = w(a, i) + w(b, i);
        r[i] = s > 32767 ? 32767 : s < -32768 ? -32768 : s;
    }
    return pack_w(r);
}

static inline uint64_t psubusw(uint64_t a, uint64_t b)
{
    int32_t r[4];
    for (int i = 0; i < 4; i++) {
        int32_t s = (uint16_t)w(a, i) - (uint16_t)w(b, i);
        r[i] = s < 0 ? 0 : s;
    }
    return pack_w(r);
}

static inline uint64_t pmulhw(uint64_t a, uint64_t b)
{
    int32_t r[4];
    for (int i = 0; i < 4; i++) r[i] = (w(a, i) * w(b, i)) >> 16;
    return pack_w(r);
}

static inline uint64_t psllw(uint64_t a, int n)
{
    int32_t r[4];
    for (int i = 0; i < 4; i++) r[i] = (uint16_t)w(a, i) << n;
    return pack_w(r);
}

static inline uint64_t pslld(uint64_t a, int n)
{
    return ((uint64_t)(uint32_t)((uint32_t)a << n)) |
           ((uint64_t)(uint32_t)((uint32_t)(a >> 32) << n) << 32);
}

/* punpcklbw x, 0 / punpcklwd x, 0 / punpckhwd x, 0 / punpckldq x, m32 */
static inline uint64_t unpack_lbw0(uint64_t a)
{
    int32_t r[4];
    for (int i = 0; i < 4; i++) r[i] = (a >> (8 * i)) & 0xff;
    return pack_w(r);
}

static inline uint64_t unpack_lwd0(uint64_t a)
{
    return (a & 0xffff) | ((a >> 16 & 0xffff) << 32);
}

static inline uint64_t unpack_hwd0(uint64_t a)
{
    return (a >> 32 & 0xffff) | ((a >> 48 & 0xffff) << 32);
}

static inline uint64_t unpack_ldq(uint64_t a, uint32_t m)
{
    return (uint32_t)a | ((uint64_t)m << 32);
}

typedef struct Yuv {
    uint8_t tab[T_LEN];
    uint64_t k_sub, k_mul, k_clamp;
    uint64_t mm[8];
} Yuv;

static inline uint32_t tab(const Yuv *y, uint32_t addr)
{
    return ldl_le_p(&y->tab[addr - T_START]);
}

/*
 * One row: n groups of 4 pixels (at least one, as the loop is a do-while).
 * Returns the Y pointer after it; *uv advances like ebp/ebx.
 */
static uint32_t yuv_row(Yuv *y, const Span *dst, const Span *ys,
                        const Span *us, const Span *vs, uint32_t edi,
                        uint32_t end, uint32_t edx, uint32_t *ebp,
                        uint32_t *ebx, uint32_t *final_edi)
{
    uint64_t *mm = y->mm;
    mm[7] = y->k_clamp;
    do {
        uint32_t d;
        uint8_t b;
        span_read(ys, edx, &d, 4);
        mm[4] = le32_to_cpu(d);
        mm[0] = 0;
        edx += 4;
        span_read(us, *ebp, &b, 1);
        mm[4] = unpack_lbw0(mm[4]);
        mm[2] = tab(y, 0x652b10 + b * 4);
        mm[1] = tab(y, 0x652310 + b * 4);
        span_read(us, *ebp + 1, &b, 1);
        *ebp += 2;
        mm[4] = psubusw(mm[4], y->k_sub);
        mm[6] = tab(y, 0x652b10 + b * 4);
        mm[1] = unpack_ldq(mm[1], tab(y, 0x652310 + b * 4));
        span_read(vs, *ebx, &b, 1);
        mm[4] = psllw(mm[4], 2);
        mm[4] = pmulhw(mm[4], y->k_mul);
        mm[5] = tab(y, 0x652710 + b * 4);
        mm[3] = tab(y, 0x652f10 + b * 4);
        mm[2] = paddw(mm[2], mm[5]);
        span_read(vs, *ebx + 1, &b, 1);
        *ebx += 2;
        mm[5] = tab(y, 0x652710 + b * 4);
        mm[3] = unpack_ldq(mm[3], tab(y, 0x652f10 + b * 4));
        mm[6] = paddw(mm[6], mm[5]);
        mm[2] = unpack_ldq(mm[2], (uint32_t)mm[6]);
        mm[1] = paddw(mm[1], mm[4]);
        mm[2] = paddw(mm[2], mm[4]);
        mm[1] = paddsw(mm[1], mm[7]);
        mm[3] = paddw(mm[3], mm[4]);
        mm[1] = psubusw(mm[1], mm[7]);
        mm[2] = paddsw(mm[2], mm[7]);
        mm[4] = mm[1];
        mm[2] = psubusw(mm[2], mm[7]);
        mm[1] = unpack_lwd0(mm[1]);
        mm[5] = mm[2];
        mm[2] = unpack_lwd0(mm[2]);
        mm[3] = paddsw(mm[3], mm[7]);
        mm[2] = pslld(mm[2], 8);
        mm[3] = psubusw(mm[3], mm[7]);
        mm[4] = unpack_hwd0(mm[4]);
        mm[6] = mm[3];
        mm[3] = unpack_lwd0(mm[3]);
        mm[3] = pslld(mm[3], 16);
        mm[1] |= mm[2];
        mm[1] |= mm[3];
        mm[5] = unpack_hwd0(mm[5]);
        mm[6] = unpack_hwd0(mm[6]);
        mm[5] = pslld(mm[5], 8);
        uint64_t q = cpu_to_le64(mm[1]);
        span_write(dst, edi, &q, 8);
        mm[6] = pslld(mm[6], 16);
        mm[4] |= mm[5];
        edi += 16;
        mm[4] |= mm[6];
        q = cpu_to_le64(mm[4]);
        span_write(dst, edi - 8, &q, 8);
    } while (edi < end);
    *final_edi = edi;
    return edx;
}

#include "xbl_hle_yuv_fast.inc"

static bool ranges_overlap32(uint32_t a, uint32_t alen, uint32_t b,
                             uint32_t blen)
{
    return a < b + blen && b < a + alen;
}

/* A row: the fast loop for all but the last group, then the exact one */
static uint32_t yuv_row_any(Yuv *y, bool fast, const Span *dst,
                            const Span *ys, const Span *us, const Span *vs,
                            uint32_t edi, uint32_t end, uint32_t k,
                            uint32_t edx, uint32_t *ebp, uint32_t *ebx,
                            uint32_t *final_edi)
{
#ifdef HAVE_YUV_ROW_FAST
    if (fast && k > 1) {
        yuv_row_fast(y, dst, ys, us, vs, &edi, k - 1, &edx, ebp, ebx);
    }
#endif
    return yuv_row(y, dst, ys, us, vs, edi, end, edx, ebp, ebx, final_edi);
}

/* Called instead of the routine at YUV_PC: does it and returns to the caller */
void helper_xbl_yuv_hle(CPUX86State *env)
{
    uintptr_t ra = GETPC();
    int mmu_idx = cpu_mmu_index(env_cpu(env), false);
    uint32_t ds = env->segs[R_DS].base, ss = env->segs[R_SS].base;
    uint32_t esp = env->regs[R_ESP];

    uint32_t ret = cpu_ldl_data_ra(env, ss + esp, ra);
    uint32_t n = cpu_ldl_data_ra(env, ss + esp + 4, ra);
    uint32_t dst0 = cpu_ldl_data_ra(env, ds + G_DST0, ra);
    uint32_t dst1 = cpu_ldl_data_ra(env, ds + G_DST1, ra);
    uint32_t y0 = cpu_ldl_data_ra(env, ds + G_Y0, ra);
    uint32_t y1 = cpu_ldl_data_ra(env, ds + G_Y1, ra);
    uint32_t u = cpu_ldl_data_ra(env, ds + G_U, ra);
    uint32_t v = cpu_ldl_data_ra(env, ds + G_V, ra);

    /* Groups per row: the loop is a do-while, so n = 0 still does one */
    uint32_t k = n ? n : 1;
    uint32_t end0 = (n << 4) + dst0, end1 = (n << 4) + dst1;
    bool sane = n <= 4096 && dst0 + k * 16 > dst0 && dst1 + k * 16 > dst1;

    static Yuv y;
    Span sd0, sd1, sy0, sy1, su, sv, st, sk;
    bool fast =
        ds == 0 && sane &&
        span_probe(env, &st, T_START, T_LEN, MMU_DATA_LOAD, mmu_idx, ra) &&
        span_probe(env, &sk, K_START, K_LEN, MMU_DATA_LOAD, mmu_idx, ra) &&
        span_probe(env, &sy0, y0, k * 4, MMU_DATA_LOAD, mmu_idx, ra) &&
        span_probe(env, &sy1, y1, k * 4, MMU_DATA_LOAD, mmu_idx, ra) &&
        span_probe(env, &su, u, k * 2, MMU_DATA_LOAD, mmu_idx, ra) &&
        span_probe(env, &sv, v, k * 2, MMU_DATA_LOAD, mmu_idx, ra) &&
        span_probe(env, &sd0, dst0, k * 16, MMU_DATA_STORE, mmu_idx, ra) &&
        span_probe(env, &sd1, dst1, k * 16, MMU_DATA_STORE, mmu_idx, ra);
    if (!fast) {
        /*
         * Unusual pointers (MMIO, a huge or wrapping row): run the game's
         * own code instead. Turn the hook off for good and retranslate.
         */
        fprintf(stderr, "xbl_yuv_hle: unusual call (n=%u dst0=%08x), "
                "using the game's code from now on\n", n, dst0);
        yuv_enabled = 0;
        queue_tb_flush(env_cpu(env));
        env->eip = YUV_PC - env->segs[R_CS].base;
        cpu_loop_exit(env_cpu(env));
    }

    span_read(&st, T_START, y.tab, T_LEN);
    uint64_t kc[3];
    span_read(&sk, K_START, kc, sizeof(kc));
    y.k_sub = le64_to_cpu(kc[0]);
    y.k_mul = le64_to_cpu(kc[1]);
    y.k_clamp = le64_to_cpu(kc[2]);

    uint32_t ebp = u, ebx = v, edi;

    /* The SSE2 loop reads a group before writing it: only when no overlap */
    bool quick = true;
    uint32_t src_at[] = { y0, y1, u, v, T_START, K_START };
    uint32_t src_len[] = { k * 4, k * 4, k * 2, k * 2, T_LEN, K_LEN };
    for (unsigned i = 0; i < ARRAY_SIZE(src_at); i++) {
        if (ranges_overlap32(dst0, k * 16, src_at[i], src_len[i]) ||
            ranges_overlap32(dst1, k * 16, src_at[i], src_len[i])) {
            quick = false;
        }
    }

    /* Row 0: the end pointer is stored first, as the game does */
    cpu_stl_data_ra(env, G_DST0, end0, ra);
    y0 = yuv_row_any(&y, quick, &sd0, &sy0, &su, &sv, dst0, end0, k, y0, &ebp,
                     &ebx, &edi);
    cpu_stl_data_ra(env, G_Y0, y0, ra);

    /* Row 1 reuses the same U and V */
    ebp = u;
    ebx = v;
    cpu_stl_data_ra(env, G_DST1, end1, ra);
    y1 = yuv_row_any(&y, quick, &sd1, &sy1, &su, &sv, dst1, end1, k, y1, &ebp,
                     &ebx, &edi);
    cpu_stl_data_ra(env, G_Y1, y1, ra);
    cpu_stl_data_ra(env, G_U, ebp, ra);
    cpu_stl_data_ra(env, G_V, ebx, ra);

    /* Registers as the routine leaves them, then "ret 4" */
    env->regs[R_EAX] = end1;
    env->regs[R_EDX] = y1;
    env->cc_op = CC_OP_SUBL;
    env->cc_dst = (uint32_t)(edi - end1);
    env->cc_src = end1;
    env->fpstt = 0;
    memset(env->fptags, 0, sizeof(env->fptags));
    for (int i = 0; i < 8; i++) {
        env->fpregs[i].mmx.MMX_Q(0) = y.mm[i];
    }
    env->regs[R_ESP] = esp + 8;
    env->eip = ret - env->segs[R_CS].base;
}
