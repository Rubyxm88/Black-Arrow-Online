/*
 * SSE2 version of the YUV row loop, one 4-pixel group per step. The MMX
 * ops map 1:1 onto SSE2 ops on the low 64 bits; the two unpack/shift/or
 * halves of each output are done together in one 128-bit register.
 * The caller runs the exact C loop for the last group, which leaves the
 * MMX registers as the game's code would.
 */
#if defined(__SSE2__)
#include <emmintrin.h>

static inline const uint8_t *span_src(const Span *s, uint32_t addr, int n,
                                      uint8_t *tmp)
{
    if ((addr & ~TARGET_PAGE_MASK) + n <= TARGET_PAGE_SIZE) {
        return span_at(s, addr);
    }
    span_read(s, addr, tmp, n);
    return tmp;
}

static inline __m128i tab_x(const Yuv *y, uint32_t addr)
{
    return _mm_cvtsi32_si128((int)tab(y, addr));
}

/* Does groups [0, count) of a row; returns nothing, pointers advance */
static void yuv_row_fast(const Yuv *y, const Span *dst, const Span *ys,
                         const Span *us, const Span *vs, uint32_t *edi,
                         uint32_t count, uint32_t *edx, uint32_t *ebp,
                         uint32_t *ebx)
{
    const __m128i zero = _mm_setzero_si128();
    const __m128i k_sub = _mm_loadl_epi64((const __m128i *)&y->k_sub);
    const __m128i k_mul = _mm_loadl_epi64((const __m128i *)&y->k_mul);
    const __m128i k_clamp = _mm_loadl_epi64((const __m128i *)&y->k_clamp);
    uint8_t ty[4], tu[2], tv[2];

    for (uint32_t i = 0; i < count; i++) {
        const uint8_t *py = span_src(ys, *edx, 4, ty);
        const uint8_t *pu = span_src(us, *ebp, 2, tu);
        const uint8_t *pv = span_src(vs, *ebx, 2, tv);
        uint32_t yd;
        memcpy(&yd, py, 4);
        uint8_t u0 = pu[0], u1 = pu[1], v0 = pv[0], v1 = pv[1];
        *edx += 4;
        *ebp += 2;
        *ebx += 2;

        __m128i m4 = _mm_unpacklo_epi8(_mm_cvtsi32_si128((int)yd), zero);
        m4 = _mm_subs_epu16(m4, k_sub);
        m4 = _mm_slli_epi16(m4, 2);
        m4 = _mm_mulhi_epi16(m4, k_mul);

        __m128i m1 = _mm_unpacklo_epi32(tab_x(y, 0x652310 + u0 * 4),
                                        tab_x(y, 0x652310 + u1 * 4));
        __m128i m2 = _mm_add_epi16(tab_x(y, 0x652b10 + u0 * 4),
                                   tab_x(y, 0x652710 + v0 * 4));
        __m128i m6 = _mm_add_epi16(tab_x(y, 0x652b10 + u1 * 4),
                                   tab_x(y, 0x652710 + v1 * 4));
        m2 = _mm_unpacklo_epi32(m2, m6);
        __m128i m3 = _mm_unpacklo_epi32(tab_x(y, 0x652f10 + v0 * 4),
                                        tab_x(y, 0x652f10 + v1 * 4));

        m1 = _mm_subs_epu16(_mm_adds_epi16(_mm_add_epi16(m1, m4), k_clamp),
                            k_clamp);
        m2 = _mm_subs_epu16(_mm_adds_epi16(_mm_add_epi16(m2, m4), k_clamp),
                            k_clamp);
        m3 = _mm_subs_epu16(_mm_adds_epi16(_mm_add_epi16(m3, m4), k_clamp),
                            k_clamp);

        /* Low 64 bits = the first 8 bytes written, high 64 = the second */
        __m128i out = _mm_or_si128(
            _mm_or_si128(_mm_unpacklo_epi16(m1, zero),
                         _mm_slli_epi32(_mm_unpacklo_epi16(m2, zero), 8)),
            _mm_slli_epi32(_mm_unpacklo_epi16(m3, zero), 16));

        if ((*edi & ~TARGET_PAGE_MASK) + 16 <= TARGET_PAGE_SIZE) {
            _mm_storeu_si128((__m128i *)span_at(dst, *edi), out);
        } else {
            uint8_t t[16];
            _mm_storeu_si128((__m128i *)t, out);
            span_write(dst, *edi, t, 8);
            span_write(dst, *edi + 8, t + 8, 8);
        }
        *edi += 16;
    }
}
#define HAVE_YUV_ROW_FAST 1
#endif
