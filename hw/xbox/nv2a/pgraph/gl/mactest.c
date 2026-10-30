/*
 * Mac test: looks for why menu text and some overlays do not show on Apple
 * Silicon Macs. Writes "mactest:" lines to xemu.log:
 *
 *  - at start, a self-test of every texture format the game can use: each is
 *    uploaded the way xemu uploads it, read in a shader, and compared with
 *    what the format should give (catches a driver that ignores the channel
 *    swizzle or mishandles a format)
 *  - the first time each texture format is bound, with its size
 *
 * While "Save Mac test pictures" is on (perf.mac_test_pictures, off at each
 * start), also:
 *  - the first time each combination of bound formats + blend + alpha test
 *    is drawn, so the menu screens' draws can be told apart
 *  - any OpenGL error after a draw
 *  - every 30 s, how many draws were made
 *  - after the first draw of each new kind, the textures used and the render
 *    target as pictures in ~/Downloads/BlackArrow-mactest-<time>
 *
 * DXT textures are tested too, through xemu's CPU decoder.
 *
 * On by default on Macs only. XEMU_MACTEST=0 turns it off, =1 turns it on
 * elsewhere.
 */

#include "qemu/osdep.h"
#include "qemu/timer.h"
#include "hw/xbox/nv2a/nv2a_int.h"
#include "hw/xbox/nv2a/pgraph/texture.h"
#include "hw/xbox/nv2a/pgraph/s3tc.h"
#include "ui/xemu-settings.h"
#include "renderer.h"

#define MACTEST_SIZE 4
#define MACTEST_MAX_FORMAT_LINES 200
#define MACTEST_MAX_DRAWSET_LINES 400
#define MACTEST_MAX_ERROR_LINES 100

static int mactest_enabled = -1;

static bool mactest_on(void)
{
    if (mactest_enabled < 0) {
        const char *e = getenv("XEMU_MACTEST");
#ifdef __APPLE__
        mactest_enabled = !(e && e[0] == '0');
#else
        mactest_enabled = e && e[0] == '1';
#endif
    }
    return mactest_enabled;
}

static bool format_is_compressed(GLint internal_format)
{
    return internal_format == GL_COMPRESSED_RGBA_S3TC_DXT1_EXT ||
           internal_format == GL_COMPRESSED_RGBA_S3TC_DXT3_EXT ||
           internal_format == GL_COMPRESSED_RGBA_S3TC_DXT5_EXT;
}

/* Formats xemu converts on the CPU before upload are tested by the game run */
static bool format_is_converted(unsigned int color_format)
{
    return color_format == NV097_SET_TEXTURE_FORMAT_COLOR_SZ_I8_A8R8G8B8 ||
           color_format == NV097_SET_TEXTURE_FORMAT_COLOR_LC_IMAGE_CR8YB8CB8YA8 ||
           color_format == NV097_SET_TEXTURE_FORMAT_COLOR_LC_IMAGE_YB8CR8YA8CB8 ||
           color_format == NV097_SET_TEXTURE_FORMAT_COLOR_SZ_R6G5B5;
}

static GLuint mactest_compile(GLenum type, const char *src)
{
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, NULL);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[1024] = "";
        glGetShaderInfoLog(s, sizeof(log), NULL, log);
        fprintf(stderr, "mactest: self-test shader failed: %s\n", log);
        glDeleteShader(s);
        return 0;
    }
    return s;
}

static GLuint mactest_program(bool fetch)
{
    static const char *vs =
        "#version 330\n"
        "const vec2 p[3] = vec2[](vec2(-1.0, -1.0), vec2(3.0, -1.0),"
        " vec2(-1.0, 3.0));\n"
        "void main() { gl_Position = vec4(p[gl_VertexID], 0.0, 1.0); }\n";
    /* texelFetch, and texture() with normalized coordinates as the game's
     * shaders sample */
    static const char *fs_fetch =
        "#version 330\n"
        "uniform sampler2D t;\n"
        "out vec4 o;\n"
        "void main() { o = texelFetch(t, ivec2(gl_FragCoord.xy), 0); }\n";
    static const char *fs_sample =
        "#version 330\n"
        "uniform sampler2D t;\n"
        "out vec4 o;\n"
        "void main() { o = texture(t, gl_FragCoord.xy / 4.0); }\n";

    GLuint v = mactest_compile(GL_VERTEX_SHADER, vs);
    GLuint f = mactest_compile(GL_FRAGMENT_SHADER, fetch ? fs_fetch : fs_sample);
    if (!v || !f) {
        return 0;
    }
    GLuint prog = glCreateProgram();
    glAttachShader(prog, v);
    glAttachShader(prog, f);
    glBindFragDataLocation(prog, 0, "o");
    glLinkProgram(prog);
    glDeleteShader(v);
    glDeleteShader(f);
    GLint ok = 0;
    glGetProgramiv(prog, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[1024] = "";
        glGetProgramInfoLog(prog, sizeof(log), NULL, log);
        fprintf(stderr, "mactest: self-test link failed: %s\n", log);
        glDeleteProgram(prog);
        return 0;
    }
    return prog;
}

static uint8_t swizzle_pick(GLenum sel, const uint8_t raw[4])
{
    switch (sel) {
    case GL_RED: return raw[0];
    case GL_GREEN: return raw[1];
    case GL_BLUE: return raw[2];
    case GL_ALPHA: return raw[3];
    case GL_ZERO: return 0;
    case GL_ONE: return 255;
    default: return 0x55;
    }
}

static const char *swizzle_name(GLenum sel)
{
    switch (sel) {
    case GL_RED: return "R";
    case GL_GREEN: return "G";
    case GL_BLUE: return "B";
    case GL_ALPHA: return "A";
    case GL_ZERO: return "0";
    case GL_ONE: return "1";
    default: return "-";
    }
}

/*
 * Draws the texture bound to unit 0 into the 4x4 target with texelFetch and
 * with texture(), and compares both with want. Returns the number of bad
 * pixels; the first one is described in detail.
 */
static int mactest_draw_compare(GLuint prog_fetch, GLuint prog_sample,
                                GLuint fbo, const uint8_t *want,
                                const uint8_t *raw, char *detail,
                                size_t detail_size)
{
    const int n = MACTEST_SIZE * MACTEST_SIZE;
    int bad_total = 0;
    for (int mode = 0; mode < 2; mode++) {
        GLuint prog = mode == 0 ? prog_fetch : prog_sample;
        uint8_t got[MACTEST_SIZE * MACTEST_SIZE * 4];
        memset(got, 0, sizeof(got));
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        glViewport(0, 0, MACTEST_SIZE, MACTEST_SIZE);
        glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        glUseProgram(prog);
        glUniform1i(glGetUniformLocation(prog, "t"), 0);
        glDrawArrays(GL_TRIANGLES, 0, 3);
        glReadPixels(0, 0, MACTEST_SIZE, MACTEST_SIZE, GL_RGBA,
                     GL_UNSIGNED_BYTE, got);

        int bad = 0;
        int first_bad = -1;
        for (int p = 0; p < n; p++) {
            for (int c = 0; c < 4; c++) {
                if (abs((int)got[p * 4 + c] - (int)want[p * 4 + c]) > 2) {
                    bad++;
                    if (first_bad < 0) {
                        first_bad = p;
                    }
                    break;
                }
            }
        }
        if (bad && !detail[0]) {
            const uint8_t *g = &got[first_bad * 4];
            const uint8_t *w = &want[first_bad * 4];
            const uint8_t *r = &raw[first_bad * 4];
            snprintf(detail, detail_size,
                     " (%s: pixel %d got %d,%d,%d,%d want %d,%d,%d,%d"
                     " raw %d,%d,%d,%d)",
                     mode == 0 ? "fetch" : "sample", first_bad,
                     g[0], g[1], g[2], g[3], w[0], w[1], w[2], w[3],
                     r[0], r[1], r[2], r[3]);
        }
        bad_total += bad;
    }
    return bad_total;
}

/* Tests one format with both shaders; returns the number of bad pixels */
static int mactest_one_format(unsigned int fmt, GLuint prog_fetch,
                              GLuint prog_sample, GLuint fbo)
{
    const ColorFormatInfo *f = &kelvin_color_format_gl_map[fmt];
    const int n = MACTEST_SIZE * MACTEST_SIZE;
    uint8_t src[MACTEST_SIZE * MACTEST_SIZE * 4];
    for (int i = 0; i < (int)sizeof(src); i++) {
        src[i] = (uint8_t)(i * 37 + 11);
    }

    while (glGetError() != GL_NO_ERROR) {
    }

    GLuint tex;
    glGenTextures(1, &tex);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, 0);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
    glTexImage2D(GL_TEXTURE_2D, 0, f->gl_internal_format, MACTEST_SIZE,
                 MACTEST_SIZE, 0, f->gl_format, f->gl_type, src);
    GLenum upload_err = glGetError();

    bool has_swizzle = f->gl_swizzle_mask[0] || f->gl_swizzle_mask[1] ||
                       f->gl_swizzle_mask[2] || f->gl_swizzle_mask[3];
    if (has_swizzle) {
        glTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_RGBA,
                         (const GLint *)f->gl_swizzle_mask);
    }
    GLenum swizzle_err = glGetError();

    /* What the texture holds, before the swizzle (swizzles only apply to
     * shader reads) */
    uint8_t raw[MACTEST_SIZE * MACTEST_SIZE * 4];
    memset(raw, 0, sizeof(raw));
    glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, raw);

    GLint swz_read[4] = { 0, 0, 0, 0 };
    glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_RGBA, swz_read);

    uint8_t want[MACTEST_SIZE * MACTEST_SIZE * 4];
    for (int p = 0; p < n; p++) {
        for (int c = 0; c < 4; c++) {
            want[p * 4 + c] = has_swizzle ?
                swizzle_pick(f->gl_swizzle_mask[c], &raw[p * 4]) :
                raw[p * 4 + c];
        }
    }

    char detail[256] = "";
    int bad_total = mactest_draw_compare(prog_fetch, prog_sample, fbo, want,
                                         raw, detail, sizeof(detail));
    GLenum draw_err = glGetError();

    char swz[64] = "none";
    if (has_swizzle) {
        snprintf(swz, sizeof(swz), "%s%s%s%s read back %s%s%s%s",
                 swizzle_name(f->gl_swizzle_mask[0]),
                 swizzle_name(f->gl_swizzle_mask[1]),
                 swizzle_name(f->gl_swizzle_mask[2]),
                 swizzle_name(f->gl_swizzle_mask[3]),
                 swizzle_name(swz_read[0]), swizzle_name(swz_read[1]),
                 swizzle_name(swz_read[2]), swizzle_name(swz_read[3]));
    }
    fprintf(stderr,
            "mactest: format 0x%02x %s internal 0x%04x swizzle %s: %s%s"
            " (gl errors upload 0x%x swizzle 0x%x draw 0x%x)\n",
            fmt, f->linear ? "linear" : "swizzled",
            (unsigned)f->gl_internal_format, swz,
            bad_total ? "BAD" : "ok", detail,
            upload_err, swizzle_err, draw_err);

    glBindTexture(GL_TEXTURE_2D, 0);
    glDeleteTextures(1, &tex);
    return bad_total;
}

/*
 * DXT textures (the menus use them) are decoded on the CPU and uploaded as
 * RGBA8. Tests that path with one 4x4 block of each kind whose texels are
 * known: red and blue end colours, every colour index and alpha step.
 */
static int mactest_dxt(enum S3TC_DECOMPRESS_FORMAT kind, GLuint prog_fetch,
                       GLuint prog_sample, GLuint fbo)
{
    static const char *names[] = { "DXT1", "DXT3", "DXT5" };
    const int n = MACTEST_SIZE * MACTEST_SIZE;
    const uint16_t c0 = 0xF800, c1 = 0x001F; /* red, blue; c0 > c1 */
    const uint8_t pal_r[4] = { 255, 0, 170, 85 };
    const uint8_t pal_b[4] = { 0, 255, 85, 170 };
    const uint8_t a5[8] = { 255, 0, 218, 182, 145, 109, 72, 36 };

    uint32_t indices = 0;
    for (int p = 0; p < n; p++) {
        indices |= (uint32_t)(p % 4) << (2 * p);
    }
    uint8_t block[16];
    uint8_t *color = block;
    memset(block, 0, sizeof(block));
    if (kind == S3TC_DECOMPRESS_FORMAT_DXT3) {
        uint64_t alpha = 0;
        for (int p = 0; p < n; p++) {
            alpha |= (uint64_t)p << (4 * p);
        }
        for (int b = 0; b < 8; b++) {
            block[b] = (uint8_t)(alpha >> (8 * b));
        }
        color = block + 8;
    } else if (kind == S3TC_DECOMPRESS_FORMAT_DXT5) {
        uint64_t alpha = 255 | (0 << 8);
        for (int p = 0; p < n; p++) {
            alpha |= (uint64_t)(p % 8) << (16 + 3 * p);
        }
        for (int b = 0; b < 8; b++) {
            block[b] = (uint8_t)(alpha >> (8 * b));
        }
        color = block + 8;
    }
    color[0] = c0 & 0xff;
    color[1] = c0 >> 8;
    color[2] = c1 & 0xff;
    color[3] = c1 >> 8;
    for (int b = 0; b < 4; b++) {
        color[4 + b] = (uint8_t)(indices >> (8 * b));
    }

    uint8_t want[MACTEST_SIZE * MACTEST_SIZE * 4];
    for (int p = 0; p < n; p++) {
        int idx = p % 4;
        want[p * 4 + 0] = pal_r[idx];
        want[p * 4 + 1] = 0;
        want[p * 4 + 2] = pal_b[idx];
        want[p * 4 + 3] = kind == S3TC_DECOMPRESS_FORMAT_DXT1 ? 255 :
                          kind == S3TC_DECOMPRESS_FORMAT_DXT3 ? p * 17 :
                          a5[p % 8];
    }

    while (glGetError() != GL_NO_ERROR) {
    }

    /* The CPU decode, as texture.c does it */
    uint8_t *converted = s3tc_decompress_2d(kind, block, MACTEST_SIZE,
                                            MACTEST_SIZE);
    int cpu_bad = 0;
    for (int i = 0; i < n * 4; i++) {
        if (abs((int)converted[i] - (int)want[i]) > 2) {
            cpu_bad++;
        }
    }

    GLuint tex;
    glGenTextures(1, &tex);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, 0);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, MACTEST_SIZE, MACTEST_SIZE, 0,
                 GL_RGBA, GL_UNSIGNED_INT_8_8_8_8_REV, converted);
    GLenum upload_err = glGetError();
    g_free(converted);

    uint8_t raw[MACTEST_SIZE * MACTEST_SIZE * 4];
    memset(raw, 0, sizeof(raw));
    glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, raw);

    char detail[256] = "";
    int bad = mactest_draw_compare(prog_fetch, prog_sample, fbo, want, raw,
                                   detail, sizeof(detail));
    GLenum draw_err = glGetError();

    fprintf(stderr,
            "mactest: %s decode %s, drawn %s%s (gl errors upload 0x%x"
            " draw 0x%x)\n",
            names[kind], cpu_bad ? "BAD" : "ok", bad ? "BAD" : "ok", detail,
            upload_err, draw_err);

    glBindTexture(GL_TEXTURE_2D, 0);
    glDeleteTextures(1, &tex);
    return cpu_bad + bad;
}

void pgraph_gl_mactest_selftest(void)
{
    /* Pictures fill ~/Downloads, so they are only for the session they were
     * turned on in */
    g_config.perf.mac_test_pictures = false;

    if (!mactest_on()) {
        return;
    }

    GLint old_fbo, old_prog, old_vao, old_tex, old_active, old_unpack,
        old_pack;
    GLint old_viewport[4];
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &old_fbo);
    glGetIntegerv(GL_CURRENT_PROGRAM, &old_prog);
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &old_vao);
    glGetIntegerv(GL_ACTIVE_TEXTURE, &old_active);
    glActiveTexture(GL_TEXTURE0);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &old_tex);
    glGetIntegerv(GL_UNPACK_ALIGNMENT, &old_unpack);
    glGetIntegerv(GL_PACK_ALIGNMENT, &old_pack);
    glGetIntegerv(GL_VIEWPORT, old_viewport);
    GLboolean old_blend = glIsEnabled(GL_BLEND);
    GLboolean old_depth = glIsEnabled(GL_DEPTH_TEST);
    GLboolean old_scissor = glIsEnabled(GL_SCISSOR_TEST);
    GLboolean old_cull = glIsEnabled(GL_CULL_FACE);
    GLboolean old_stencil = glIsEnabled(GL_STENCIL_TEST);
    glDisable(GL_BLEND);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_STENCIL_TEST);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);

    fprintf(stderr, "mactest: renderer %s, %s\n",
            (const char *)glGetString(GL_RENDERER),
            (const char *)glGetString(GL_VERSION));

    GLuint prog_fetch = mactest_program(true);
    GLuint prog_sample = mactest_program(false);
    GLuint vao, fbo, rb;
    glGenVertexArrays(1, &vao);
    glBindVertexArray(vao);
    glGenRenderbuffers(1, &rb);
    glBindRenderbuffer(GL_RENDERBUFFER, rb);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, MACTEST_SIZE,
                          MACTEST_SIZE);
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                              GL_RENDERBUFFER, rb);
    GLenum fbo_status = glCheckFramebufferStatus(GL_FRAMEBUFFER);

    if (!prog_fetch || !prog_sample ||
        fbo_status != GL_FRAMEBUFFER_COMPLETE) {
        fprintf(stderr, "mactest: self-test could not start (fbo 0x%x)\n",
                fbo_status);
    } else {
        int tested = 0, failed = 0;
        for (unsigned int fmt = 0;
             fmt < ARRAY_SIZE(kelvin_color_format_gl_map); fmt++) {
            const ColorFormatInfo *f = &kelvin_color_format_gl_map[fmt];
            if (!f->bytes_per_pixel || f->depth ||
                format_is_compressed(f->gl_internal_format) ||
                format_is_converted(fmt)) {
                continue;
            }
            tested++;
            if (mactest_one_format(fmt, prog_fetch, prog_sample, fbo)) {
                failed++;
            }
        }
        for (int kind = S3TC_DECOMPRESS_FORMAT_DXT1;
             kind <= S3TC_DECOMPRESS_FORMAT_DXT5; kind++) {
            tested++;
            if (mactest_dxt(kind, prog_fetch, prog_sample, fbo)) {
                failed++;
            }
        }
        fprintf(stderr, "mactest: self-test done: %d formats, %d BAD\n",
                tested, failed);
    }

    glBindFramebuffer(GL_FRAMEBUFFER, old_fbo);
    glDeleteFramebuffers(1, &fbo);
    glDeleteRenderbuffers(1, &rb);
    glBindVertexArray(old_vao);
    glDeleteVertexArrays(1, &vao);
    glUseProgram(old_prog);
    if (prog_fetch) {
        glDeleteProgram(prog_fetch);
    }
    if (prog_sample) {
        glDeleteProgram(prog_sample);
    }
    glBindTexture(GL_TEXTURE_2D, old_tex);
    glActiveTexture(old_active);
    glPixelStorei(GL_UNPACK_ALIGNMENT, old_unpack);
    glPixelStorei(GL_PACK_ALIGNMENT, old_pack);
    glViewport(old_viewport[0], old_viewport[1], old_viewport[2],
               old_viewport[3]);
    if (old_blend) glEnable(GL_BLEND);
    if (old_depth) glEnable(GL_DEPTH_TEST);
    if (old_scissor) glEnable(GL_SCISSOR_TEST);
    if (old_cull) glEnable(GL_CULL_FACE);
    if (old_stencil) glEnable(GL_STENCIL_TEST);
    while (glGetError() != GL_NO_ERROR) {
    }
}

/* First use of each texture format (with its kind of texture) */
void pgraph_gl_mactest_texture(PGRAPHState *pg, int stage,
                               const TextureShape *s)
{
    static uint8_t seen[66][8];
    static int lines;

    if (!mactest_on() || s->color_format >= 66 ||
        lines >= MACTEST_MAX_FORMAT_LINES) {
        return;
    }
    int kind = (s->cubemap ? 4 : 0) | (s->border ? 2 : 0) |
               (s->dimensionality == 3 ? 1 : 0);
    if (seen[s->color_format][kind]) {
        return;
    }
    seen[s->color_format][kind] = 1;
    lines++;
    fprintf(stderr,
            "mactest: frame %d first texture format 0x%02x on stage %d:"
            " %ux%u levels %u%s%s%s\n",
            pg->frame_time, s->color_format, stage, s->width, s->height,
            s->levels, s->cubemap ? " cube" : "",
            s->border ? " border" : "",
            s->dimensionality == 3 ? " 3D" : "");
}

/*
 * Pictures: after the first draw of each new kind, the textures it used and
 * the render target it drew into, as TGA files in
 * ~/Downloads/BlackArrow-mactest-<time>. Capped so the folder stays small
 * enough to zip and send.
 */
#define MACTEST_MAX_DUMP_KINDS 60
#define MACTEST_MAX_DUMP_BYTES (80 * 1024 * 1024)
#define MACTEST_DUMP_UNIT 15

static char *dump_dir;
static size_t dump_bytes;

static bool mactest_dump_dir_ready(void)
{
    if (dump_dir) {
        return true;
    }
    GDateTime *now = g_date_time_new_now_local();
    gchar *stamp = g_date_time_format(now, "%Y-%m-%d_%H%M%S");
    g_date_time_unref(now);
    gchar *name = g_strdup_printf("BlackArrow-mactest-%s", stamp);
    g_free(stamp);
    char *dir = g_build_filename(g_get_home_dir(), "Downloads", name, NULL);
    g_free(name);
    if (g_mkdir_with_parents(dir, 0755) != 0) {
        fprintf(stderr, "mactest: could not make %s\n", dir);
        g_free(dir);
        return false;
    }
    fprintf(stderr, "mactest: pictures go to %s\n", dir);
    dump_dir = dir;
    return true;
}

/* Writes RGBA rows as TGA (first row at the top, as xemu keeps render
 * targets), keeping at most max_w pixels across */
static void mactest_write_tga(const char *name, const uint8_t *rgba, int w,
                              int h, int max_w)
{
    int step = 1;
    while (w / step > max_w) {
        step++;
    }
    int ow = w / step, oh = h / step;
    size_t size = 18 + (size_t)ow * oh * 4;
    if (dump_bytes + size > MACTEST_MAX_DUMP_BYTES) {
        return;
    }
    uint8_t *out = g_malloc(size);
    memset(out, 0, 18);
    out[2] = 2;
    out[12] = ow & 0xff;
    out[13] = ow >> 8;
    out[14] = oh & 0xff;
    out[15] = oh >> 8;
    out[16] = 32;
    out[17] = 8 | 0x20; /* 8 alpha bits, first row at the top */
    uint8_t *o = out + 18;
    for (int y = 0; y < oh; y++) {
        for (int x = 0; x < ow; x++) {
            const uint8_t *p = &rgba[((size_t)y * step * w + x * step) * 4];
            *o++ = p[2];
            *o++ = p[1];
            *o++ = p[0];
            *o++ = p[3];
        }
    }
    char *path = g_build_filename(dump_dir, name, NULL);
    if (g_file_set_contents(path, (const char *)out, size, NULL)) {
        dump_bytes += size;
    }
    g_free(path);
    g_free(out);
}

/* Reads level 0 of a 2D texture and writes it */
static void mactest_dump_texture(GLuint tex, const char *name, int max_w)
{
    GLint w = 0, h = 0;
    glBindTexture(GL_TEXTURE_2D, tex);
    glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &w);
    glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &h);
    if (w <= 0 || h <= 0 || w > 4096 || h > 4096) {
        return;
    }
    uint8_t *rgba = g_malloc((size_t)w * h * 4);
    glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    mactest_write_tga(name, rgba, w, h, max_w);
    g_free(rgba);
}

static void mactest_dump_draw(NV2AState *d, uint64_t draw, const uint32_t *key)
{
    static GHashTable *dumped_textures;
    static int kinds;
    PGRAPHState *pg = &d->pgraph;
    PGRAPHGLState *r = pg->gl_renderer_state;

    if (kinds >= MACTEST_MAX_DUMP_KINDS ||
        dump_bytes >= MACTEST_MAX_DUMP_BYTES || !mactest_dump_dir_ready()) {
        return;
    }
    kinds++;
    if (!dumped_textures) {
        dumped_textures = g_hash_table_new(NULL, NULL);
    }

    GLint old_active, old_tex, old_pack;
    glGetIntegerv(GL_ACTIVE_TEXTURE, &old_active);
    glActiveTexture(GL_TEXTURE0 + MACTEST_DUMP_UNIT);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &old_tex);
    glGetIntegerv(GL_PACK_ALIGNMENT, &old_pack);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);

    for (int i = 0; i < NV2A_MAX_TEXTURES; i++) {
        TextureBinding *b = r->texture_binding[i];
        if (key[i] == 0xff || key[i] >= 66 ||
            kelvin_color_format_gl_map[key[i]].depth ||
            !b || b->gl_target != GL_TEXTURE_2D ||
            g_hash_table_contains(dumped_textures,
                                  GUINT_TO_POINTER(b->gl_texture))) {
            continue;
        }
        g_hash_table_add(dumped_textures, GUINT_TO_POINTER(b->gl_texture));
        char name[96];
        snprintf(name, sizeof(name), "f%06d-d%07" PRIu64 "-tex%d-fmt%02x.tga",
                 pg->frame_time, draw, i, key[i]);
        mactest_dump_texture(b->gl_texture, name, 512);
    }

    if (r->color_binding) {
        char name[96];
        snprintf(name, sizeof(name), "f%06d-d%07" PRIu64 "-target.tga",
                 pg->frame_time, draw);
        mactest_dump_texture(r->color_binding->gl_buffer, name, 640);
    }

    glBindTexture(GL_TEXTURE_2D, old_tex);
    glActiveTexture(old_active);
    glPixelStorei(GL_PACK_ALIGNMENT, old_pack);

    /* xemu asserts no GL error later on: report and clear ours here */
    GLenum err;
    while ((err = glGetError()) != GL_NO_ERROR) {
        fprintf(stderr, "mactest: GL error 0x%x while saving pictures\n", err);
    }
}

/*
 * Every 15 s, at the first draw of a new frame: every colour render target
 * xemu holds (up to 16), so off-screen pictures that are combined into the
 * screen (menus, HUD) are caught as well. The name says when each was last
 * drawn into.
 */
#define MACTEST_FRAME_SHOT_MS 15000
#define MACTEST_MAX_FRAME_SHOTS 24

static void mactest_frame_shots(NV2AState *d)
{
    static int last_frame = -1;
    static int64_t last_shot;
    static int shots;
    PGRAPHState *pg = &d->pgraph;
    PGRAPHGLState *r = pg->gl_renderer_state;

    if (pg->frame_time == last_frame) {
        return;
    }
    int drawn_frame = last_frame;
    last_frame = pg->frame_time;

    int64_t now = qemu_clock_get_ms(QEMU_CLOCK_REALTIME);
    if (drawn_frame < 0 || shots >= MACTEST_MAX_FRAME_SHOTS ||
        now - last_shot < MACTEST_FRAME_SHOT_MS ||
        dump_bytes >= MACTEST_MAX_DUMP_BYTES || !mactest_dump_dir_ready()) {
        return;
    }
    last_shot = now;
    shots++;

    GLint old_active, old_tex, old_pack;
    glGetIntegerv(GL_ACTIVE_TEXTURE, &old_active);
    glActiveTexture(GL_TEXTURE0 + MACTEST_DUMP_UNIT);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &old_tex);
    glGetIntegerv(GL_PACK_ALIGNMENT, &old_pack);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);

    int n = 0;
    SurfaceBinding *s;
    QTAILQ_FOREACH(s, &r->surfaces, entry) {
        if (!s->color || n >= 16) {
            continue;
        }
        char name[96];
        snprintf(name, sizeof(name),
                 "shot%02d-f%06d-surface%08x-%ux%u-drawn%06d.tga", shots,
                 drawn_frame, (unsigned)s->vram_addr, s->width, s->height,
                 s->frame_time);
        mactest_dump_texture(s->gl_buffer, name, 640);
        n++;
    }
    fprintf(stderr, "mactest: shot %d of frame %d: %d render targets\n",
            shots, drawn_frame, n);

    glBindTexture(GL_TEXTURE_2D, old_tex);
    glActiveTexture(old_active);
    glPixelStorei(GL_PACK_ALIGNMENT, old_pack);
    GLenum err;
    while ((err = glGetError()) != GL_NO_ERROR) {
        fprintf(stderr, "mactest: GL error 0x%x while saving pictures\n", err);
    }
}

/* After each draw: OpenGL errors, the first draw of each kind, and a count */
void pgraph_gl_mactest_after_draw(NV2AState *d)
{
    static GHashTable *seen;
    static int drawset_lines, error_lines;
    static uint64_t draws;
    static int64_t window_start;
    static uint64_t window_draws;

    if (!mactest_on() || !g_config.perf.mac_test_pictures) {
        return;
    }
    PGRAPHState *pg = &d->pgraph;

    draws++;
    window_draws++;
    int64_t now = qemu_clock_get_ms(QEMU_CLOCK_REALTIME);
    if (!window_start) {
        window_start = now;
    } else if (now - window_start >= 30000) {
        fprintf(stderr, "mactest: %" PRIu64 " draws in %.0f s (frame %d)\n",
                window_draws, (now - window_start) / 1000.0, pg->frame_time);
        window_start = now;
        window_draws = 0;
    }

    uint32_t control_0 = pgraph_reg_r(pg, NV_PGRAPH_CONTROL_0);
    uint32_t blend = pgraph_reg_r(pg, NV_PGRAPH_BLEND);
    uint32_t key[NV2A_MAX_TEXTURES + 2];
    for (int i = 0; i < NV2A_MAX_TEXTURES; i++) {
        key[i] = 0xff;
        if (pgraph_is_texture_enabled(pg, i)) {
            TextureShape s = pgraph_get_texture_shape(pg, i);
            key[i] = s.color_format;
        }
    }
    key[NV2A_MAX_TEXTURES] =
        (blend & (NV_PGRAPH_BLEND_EN | NV_PGRAPH_BLEND_SFACTOR |
                  NV_PGRAPH_BLEND_DFACTOR));
    key[NV2A_MAX_TEXTURES + 1] =
        control_0 & (NV_PGRAPH_CONTROL_0_ALPHATESTENABLE |
                     NV_PGRAPH_CONTROL_0_ALPHAFUNC |
                     NV_PGRAPH_CONTROL_0_ALPHA_WRITE_ENABLE |
                     NV_PGRAPH_CONTROL_0_RED_WRITE_ENABLE |
                     NV_PGRAPH_CONTROL_0_GREEN_WRITE_ENABLE |
                     NV_PGRAPH_CONTROL_0_BLUE_WRITE_ENABLE |
                     NV_PGRAPH_CONTROL_0_ZENABLE);

    GLenum err = glGetError();
    if (err != GL_NO_ERROR && error_lines < MACTEST_MAX_ERROR_LINES) {
        error_lines++;
        fprintf(stderr,
                "mactest: GL error 0x%x after draw %" PRIu64 " (frame %d)"
                " textures %02x %02x %02x %02x blend %03x control %05x\n",
                err, draws, pg->frame_time, key[0], key[1], key[2], key[3],
                key[4], key[5]);
        while (glGetError() != GL_NO_ERROR) {
        }
    }

    mactest_frame_shots(d);

    if (drawset_lines >= MACTEST_MAX_DRAWSET_LINES) {
        return;
    }
    if (!seen) {
        seen = g_hash_table_new_full(g_bytes_hash, g_bytes_equal,
                                     (GDestroyNotify)g_bytes_unref, NULL);
    }
    GBytes *b = g_bytes_new(key, sizeof(key));
    if (g_hash_table_contains(seen, b)) {
        g_bytes_unref(b);
        return;
    }
    g_hash_table_add(seen, b);
    drawset_lines++;

    uint32_t alpha_ref = GET_MASK(control_0, NV_PGRAPH_CONTROL_0_ALPHAREF);
    fprintf(stderr,
            "mactest: frame %d draw %" PRIu64 " new kind: textures"
            " %02x %02x %02x %02x blend %s src %u dst %u alphatest %s"
            " func %u ref %u writes %c%c%c%c depth %s primitive %u\n",
            pg->frame_time, draws, key[0], key[1], key[2], key[3],
            (blend & NV_PGRAPH_BLEND_EN) ? "on" : "off",
            GET_MASK(blend, NV_PGRAPH_BLEND_SFACTOR),
            GET_MASK(blend, NV_PGRAPH_BLEND_DFACTOR),
            (control_0 & NV_PGRAPH_CONTROL_0_ALPHATESTENABLE) ? "on" : "off",
            GET_MASK(control_0, NV_PGRAPH_CONTROL_0_ALPHAFUNC), alpha_ref,
            (control_0 & NV_PGRAPH_CONTROL_0_RED_WRITE_ENABLE) ? 'R' : '-',
            (control_0 & NV_PGRAPH_CONTROL_0_GREEN_WRITE_ENABLE) ? 'G' : '-',
            (control_0 & NV_PGRAPH_CONTROL_0_BLUE_WRITE_ENABLE) ? 'B' : '-',
            (control_0 & NV_PGRAPH_CONTROL_0_ALPHA_WRITE_ENABLE) ? 'A' : '-',
            (control_0 & NV_PGRAPH_CONTROL_0_ZENABLE) ? "on" : "off",
            pg->primitive_mode);

    mactest_dump_draw(d, draws, key);
}
