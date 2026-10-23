/*
 * Geforce NV2A PGRAPH OpenGL Renderer
 *
 * Copyright (c) 2015 espes
 * Copyright (c) 2015 Jannik Vogel
 * Copyright (c) 2020-2025 Matt Borgerson
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

#include "qemu/osdep.h"
#include <glib/gstdio.h>
#include "qemu/fast-hash.h"
#include "qemu/xemu-perf.h"
#include "qemu/mstring.h"
#include "qemu/bswap.h"
#include "qemu/timer.h"
#include "hw/xbox/nv2a/shader-precache.h"
#include "ui/xemu-diag.h"

#include "xemu-version.h"
#include "ui/xemu-settings.h"
#include "hw/xbox/nv2a/debug_gl.h"
#include "hw/xbox/nv2a/pgraph/util.h"
#include "debug.h"
#include "renderer.h"
#include "shader-state-v1.h"

static GLenum get_gl_primitive_mode(enum ShaderPolygonMode polygon_mode, enum ShaderPrimitiveMode primitive_mode)
{
    switch (primitive_mode) {
    case PRIM_TYPE_POINTS: return GL_POINTS;
    case PRIM_TYPE_LINES: return GL_LINES;
    case PRIM_TYPE_LINE_LOOP: return GL_LINE_LOOP;
    case PRIM_TYPE_LINE_STRIP: return GL_LINE_STRIP;
    case PRIM_TYPE_TRIANGLES: return GL_TRIANGLES;
    case PRIM_TYPE_TRIANGLE_STRIP: return GL_TRIANGLE_STRIP;
    case PRIM_TYPE_TRIANGLE_FAN: return GL_TRIANGLE_FAN;
    case PRIM_TYPE_QUADS: return GL_LINES_ADJACENCY;
    case PRIM_TYPE_QUAD_STRIP: return GL_LINE_STRIP_ADJACENCY;
    case PRIM_TYPE_POLYGON:
        if (polygon_mode == POLY_MODE_LINE) {
            return GL_LINE_LOOP;
        } else if (polygon_mode == POLY_MODE_FILL) {
            return GL_TRIANGLE_FAN;
        }

        assert(!"PRIM_TYPE_POLYGON with invalid polygon_mode");
        return 0;
    default:
        assert(!"Invalid primitive_mode");
        return 0;
    }
}

static GLuint create_gl_shader(GLenum gl_shader_type,
                               const char *code,
                               const char *name)
{
    GLint compiled = 0;

    NV2A_GL_DGROUP_BEGIN("Creating new %s", name);

    NV2A_DPRINTF("compile new %s, code:\n%s\n", name, code);

    GLuint shader = glCreateShader(gl_shader_type);
    glShaderSource(shader, 1, &code, 0);
    glCompileShader(shader);

    /* Check it compiled */
    compiled = 0;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
    if (!compiled) {
        GLchar* log;
        GLint log_length;
        glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &log_length);
        log = g_malloc(log_length * sizeof(GLchar));
        glGetShaderInfoLog(shader, log_length, NULL, log);
        fprintf(stderr, "%s\n\n" "nv2a: %s compilation failed: %s\n", code, name, log);
        g_free(log);

        NV2A_GL_DGROUP_END();
        abort();
    }

    NV2A_GL_DGROUP_END();

    return shader;
}

static void set_texture_sampler_uniforms(ShaderBinding *binding)
{
    for (int i = 0; i < NV2A_MAX_TEXTURES; i++) {
        char samplerName[16];
        snprintf(samplerName, sizeof(samplerName), "texSamp%d", i);
        GLint texSampLoc =
            glGetUniformLocation(binding->gl_program, samplerName);
        if (texSampLoc >= 0) {
            glUniform1i(texSampLoc, i);
        }
    }
}

static void update_shader_uniform_locs(ShaderBinding *binding)
{
    char tmp[64];

    for (int i = 0; i < ARRAY_SIZE(binding->uniform_locs.vsh); i++) {
        const char *name = VshUniformInfo[i].name;
        if (VshUniformInfo[i].count > 1) {
            snprintf(tmp, sizeof(tmp), "%s[0]", name);
            name = tmp;
        }
        binding->uniform_locs.vsh[i] = glGetUniformLocation(binding->gl_program, name);
    }

    for (int i = 0; i < ARRAY_SIZE(binding->uniform_locs.psh); i++) {
        const char *name = PshUniformInfo[i].name;
        if (PshUniformInfo[i].count > 1) {
            snprintf(tmp, sizeof(tmp), "%s[0]", name);
            name = tmp;
        }
        binding->uniform_locs.psh[i] = glGetUniformLocation(binding->gl_program, name);
    }
}

static void shader_module_cache_entry_init(Lru *lru, LruNode *node,
                                           const void *key)
{
    ShaderModuleCacheEntry *module =
        container_of(node, ShaderModuleCacheEntry, node);
    memcpy(&module->key, key, sizeof(ShaderModuleCacheKey));

    const char *kind_str;
    MString *code;

    switch (module->key.kind) {
    case GL_VERTEX_SHADER:
        kind_str = "vertex shader";
        code = pgraph_glsl_gen_vsh(&module->key.vsh.state,
                                   module->key.vsh.glsl_opts);
        break;
    case GL_GEOMETRY_SHADER:
        kind_str = "geometry shader";
        code = pgraph_glsl_gen_geom(&module->key.geom.state,
                                    module->key.geom.glsl_opts);
        break;
    case GL_FRAGMENT_SHADER:
        kind_str = "fragment shader";
        code = pgraph_glsl_gen_psh(&module->key.psh.state,
                                   module->key.psh.glsl_opts);
        break;
    default:
        assert(!"Invalid shader module kind");
        kind_str = "unknown";
        code = NULL;
    }

    module->gl_shader =
        create_gl_shader(module->key.kind, mstring_get_str(code), kind_str);
    mstring_unref(code);
}

static void shader_module_cache_entry_post_evict(Lru *lru, LruNode *node)
{
    ShaderModuleCacheEntry *module =
        container_of(node, ShaderModuleCacheEntry, node);
    glDeleteShader(module->gl_shader);
}

static bool shader_module_cache_entry_compare(Lru *lru, LruNode *node,
                                              const void *key)
{
    ShaderModuleCacheEntry *module =
        container_of(node, ShaderModuleCacheEntry, node);
    return memcmp(&module->key, key, sizeof(ShaderModuleCacheKey));
}

static GLuint get_shader_module_for_key(PGRAPHGLState *r,
                                        const ShaderModuleCacheKey *key)
{
    uint64_t hash = fast_hash((void *)key, sizeof(ShaderModuleCacheKey));
    LruNode *node = lru_lookup(&r->shader_module_cache, hash, key);
    ShaderModuleCacheEntry *module =
        container_of(node, ShaderModuleCacheEntry, node);
    return module->gl_shader;
}

static void generate_shaders(PGRAPHGLState *r, ShaderBinding *binding)
{
    GLuint program = glCreateProgram();

    ShaderState *state = &binding->state;
    ShaderModuleCacheKey key;

    bool need_geometry_shader = pgraph_glsl_need_geom(&state->geom);
    if (need_geometry_shader) {
        memset(&key, 0, sizeof(key));
        key.kind = GL_GEOMETRY_SHADER;
        key.geom.state = state->geom;
        glAttachShader(program, get_shader_module_for_key(r, &key));
    }

    /* create the vertex shader */
    memset(&key, 0, sizeof(key));
    key.kind = GL_VERTEX_SHADER;
    key.vsh.state = state->vsh;
    key.vsh.glsl_opts.prefix_outputs = need_geometry_shader;
    glAttachShader(program, get_shader_module_for_key(r, &key));

    /* generate a fragment shader from register combiners */
    memset(&key, 0, sizeof(key));
    key.kind = GL_FRAGMENT_SHADER;
    key.psh.state = state->psh;
    glAttachShader(program, get_shader_module_for_key(r, &key));

    /* link the program */
    glLinkProgram(program);
    GLint linked = 0;
    glGetProgramiv(program, GL_LINK_STATUS, &linked);
    if(!linked) {
        GLchar log[2048];
        glGetProgramInfoLog(program, 2048, NULL, log);
        fprintf(stderr, "nv2a: shader linking failed: %s\n", log);
        abort();
    }

    glUseProgram(program);

    binding->gl_program = program;
    binding->gl_primitive_mode = get_gl_primitive_mode(
        state->geom.polygon_front_mode, state->geom.primitive_mode);
    binding->initialized = true;

    set_texture_sampler_uniforms(binding);

    /* validate the program */
    GLint valid = 0;
    glValidateProgram(program);
    glGetProgramiv(program, GL_VALIDATE_STATUS, &valid);
    if (!valid) {
        GLchar log[1024];
        glGetProgramInfoLog(program, 1024, NULL, log);
        fprintf(stderr, "nv2a: shader validation failed: %s\n", log);
        abort();
    }

    update_shader_uniform_locs(binding);
}

/*
 * Background shader building (BA Online > Build shaders in the
 * background). With the driver's parallel shader compiler, a new shader is
 * compiled and linked on the driver's own threads. Until it is ready, draws
 * that need it use a program that draws nothing, so an effect is missing
 * for a few frames instead of the game freezing. A shader still not ready
 * after ASYNC_MAX_FRAMES frames is waited for, so nothing stays missing.
 */

#define ASYNC_MAX_FRAMES 3

static bool async_supported;
static ShaderBinding async_null_binding;

static void async_init(void)
{
    if (glo_check_extension("GL_KHR_parallel_shader_compile")) {
        glMaxShaderCompilerThreadsKHR(0xFFFFFFFF);
        async_supported = true;
    } else if (glo_check_extension("GL_ARB_parallel_shader_compile")) {
        glMaxShaderCompilerThreadsARB(0xFFFFFFFF);
        async_supported = true;
    }
    fprintf(stderr, "nv2a: background shader building %s\n",
            async_supported ? "available" : "not supported by this driver");

    /* Every vertex lands outside the screen, so nothing is drawn */
    static const char *vs =
        "#version 330\n"
        "void main() { gl_Position = vec4(2.0, 2.0, 0.0, 1.0); }\n";
    static const char *fs =
        "#version 330\n"
        "out vec4 color;\n"
        "void main() { color = vec4(0.0); }\n";
    ShaderBinding *null = &async_null_binding;
    null->gl_program = pgraph_gl_compile_shader(vs, fs);
    null->initialized = true;
    for (int i = 0; i < ARRAY_SIZE(null->uniform_locs.vsh); i++) {
        null->uniform_locs.vsh[i] = -1;
    }
    for (int i = 0; i < ARRAY_SIZE(null->uniform_locs.psh); i++) {
        null->uniform_locs.psh[i] = -1;
    }
}

/* Compile one stage without waiting for it; the link reports failures */
static void async_attach(GLuint program, GLenum kind, MString *code)
{
    const char *src = mstring_get_str(code);
    GLuint shader = glCreateShader(kind);

    glShaderSource(shader, 1, &src, 0);
    glCompileShader(shader);
    glAttachShader(program, shader);
    /* Freed with the program */
    glDeleteShader(shader);
    mstring_unref(code);
}

/* Start building the program for a binding, the way generate_shaders() does */
static void async_start(ShaderBinding *binding)
{
    ShaderState *state = &binding->state;
    GLuint program = glCreateProgram();

    bool need_geometry_shader = pgraph_glsl_need_geom(&state->geom);
    if (need_geometry_shader) {
        GenGeomGlslOptions geom_opts;
        memset(&geom_opts, 0, sizeof(geom_opts));
        async_attach(program, GL_GEOMETRY_SHADER,
                     pgraph_glsl_gen_geom(&state->geom, geom_opts));
    }
    GenVshGlslOptions vsh_opts;
    memset(&vsh_opts, 0, sizeof(vsh_opts));
    vsh_opts.prefix_outputs = need_geometry_shader;
    async_attach(program, GL_VERTEX_SHADER,
                 pgraph_glsl_gen_vsh(&state->vsh, vsh_opts));
    GenPshGlslOptions psh_opts;
    memset(&psh_opts, 0, sizeof(psh_opts));
    async_attach(program, GL_FRAGMENT_SHADER,
                 pgraph_glsl_gen_psh(&state->psh, psh_opts));

    glLinkProgram(program);
    binding->gl_program = program;
    binding->async_pending = true;
    binding->async_frame = g_nv2a_stats.frame_count;
}

static bool async_ready(ShaderBinding *binding)
{
    GLint done = GL_FALSE;
    glGetProgramiv(binding->gl_program, GL_COMPLETION_STATUS_KHR, &done);
    return done ||
           g_nv2a_stats.frame_count - binding->async_frame >= ASYNC_MAX_FRAMES;
}

/*
 * Finish a binding whose program was linking in the background (waiting for
 * it if needed). False if the link failed; the program is then deleted.
 */
static bool async_finish(ShaderBinding *binding)
{
    GLuint program = binding->gl_program;
    GLint linked = 0;

    binding->async_pending = false;
    glGetProgramiv(program, GL_LINK_STATUS, &linked);
    if (!linked) {
        GLchar log[2048];
        glGetProgramInfoLog(program, sizeof(log), NULL, log);
        fprintf(stderr, "nv2a: background shader link failed: %s\n", log);
        glDeleteProgram(program);
        binding->gl_program = 0;
        return false;
    }

    glUseProgram(program);
    binding->gl_primitive_mode =
        get_gl_primitive_mode(binding->state.geom.polygon_front_mode,
                              binding->state.geom.primitive_mode);
    binding->initialized = true;
    set_texture_sampler_uniforms(binding);
    update_shader_uniform_locs(binding);
    return true;
}

/* The binding that draws nothing, standing in for one still building */
static ShaderBinding *async_stand_in(const ShaderState *state)
{
    ShaderBinding *null = &async_null_binding;
    memcpy(&null->state, state, sizeof(*state));
    null->gl_primitive_mode = get_gl_primitive_mode(
        state->geom.polygon_front_mode, state->geom.primitive_mode);
    return null;
}

static void precache_attach(PGRAPHState *pg);
static void precache_detach(void);
static void learned_add(const ShaderState *state);

static const char *shader_gl_vendor = NULL;
static const char *shader_gl_version = NULL;
static const char *shader_gl_renderer = NULL;

/*
 * Bump when this fork changes how shaders are generated, so old cached
 * programs are thrown away.
 */
#define SHADER_CACHE_REV 1

/*
 * What a cached shader must match besides the GPU and its driver: the
 * upstream xemu version (not the build number, so an update of this fork
 * keeps the cache), the shader state layout and SHADER_CACHE_REV.
 */
static const char *shader_cache_tag(void)
{
    static char tag[64];
    if (!tag[0]) {
        size_t upstream = strcspn(xemu_version, "-");
        snprintf(tag, sizeof(tag), "%.*s-s%zu-r%d", (int)upstream,
                 xemu_version, sizeof(ShaderState), SHADER_CACHE_REV);
    }
    return tag;
}

/*
 * The cache folder of this tag (shaders-0.8.138-s2756-r1), with its own
 * shader_cache_list inside. Builds up to 20 use "shaders" and a
 * shader_cache_list beside it; each build deletes cache files with another
 * tag when it opens them, so sharing one folder made an update or a
 * rollback throw away the other build's shaders.
 */
static const char *shader_cache_dir(void)
{
    static char *dir;
    if (!dir) {
        dir = g_strdup_printf("%sshaders-%s", xemu_settings_get_base_path(),
                              shader_cache_tag());
    }
    return dir;
}

#define SHADER_CACHE_LIST "shader_cache_list"

static void shader_create_cache_folder(void)
{
    qemu_mkdir(shader_cache_dir());
}

static char *shader_get_lru_cache_path(void)
{
    return g_build_filename(shader_cache_dir(), SHADER_CACHE_LIST, NULL);
}

/* Delete a cache folder: its xxxx folders, their files and its own files */
static void shader_remove_cache_folder(const char *root)
{
    GDir *dir = g_dir_open(root, 0, NULL);
    const char *name;
    while (dir && (name = g_dir_read_name(dir))) {
        char *sub_path = g_build_filename(root, name, NULL);
        GDir *sub = g_dir_open(sub_path, 0, NULL);
        const char *file;
        while (sub && (file = g_dir_read_name(sub))) {
            char *file_path = g_build_filename(sub_path, file, NULL);
            g_remove(file_path);
            g_free(file_path);
        }
        if (sub) {
            g_dir_close(sub);
            g_rmdir(sub_path);
        } else {
            g_remove(sub_path);
        }
        g_free(sub_path);
    }
    if (dir) {
        g_dir_close(dir);
    }
    g_rmdir(root);
}

/*
 * When a cache folder was last used: its list is written at a clean exit,
 * and an xxxx folder changes when a shader is saved in it (a crash or a
 * killed xemu leaves the list old).
 */
static gint64 shader_cache_folder_time(const char *root, const char *list)
{
    GStatBuf st;
    gint64 newest = g_stat(list, &st) == 0 ? (gint64)st.st_mtime : 0;
    GDir *dir = g_dir_open(root, 0, NULL);
    const char *name;
    while (dir && (name = g_dir_read_name(dir))) {
        char *sub_path = g_build_filename(root, name, NULL);
        if (g_stat(sub_path, &st) == 0 && (gint64)st.st_mtime > newest) {
            newest = st.st_mtime;
        }
        g_free(sub_path);
    }
    if (dir) {
        g_dir_close(dir);
    }
    return newest;
}

/*
 * Other builds' cache folders: the newest one is kept, so going back to that
 * build finds its shaders built; older ones are deleted. With every_one, all
 * of them go (Clear Cache).
 */
static void shader_cache_prune_others(bool every_one)
{
    const char *base = xemu_settings_get_base_path();
    const char *mine = shader_cache_dir();
    GPtrArray *roots = g_ptr_array_new_with_free_func(g_free);
    GArray *times = g_array_new(FALSE, FALSE, sizeof(gint64));

    /* Builds up to 20: "shaders" with shader_cache_list beside it */
    char *legacy = g_strdup_printf("%sshaders", base);
    char *legacy_list = g_strdup_printf("%s%s", base, SHADER_CACHE_LIST);
    if (g_file_test(legacy, G_FILE_TEST_IS_DIR)) {
        gint64 t = shader_cache_folder_time(legacy, legacy_list);
        g_ptr_array_add(roots, g_strdup(legacy));
        g_array_append_val(times, t);
    }

    GDir *dir = g_dir_open(base, 0, NULL);
    const char *name;
    while (dir && (name = g_dir_read_name(dir))) {
        if (strncmp(name, "shaders-", 8) || !g_ascii_isdigit(name[8])) {
            continue;
        }
        char *root = g_strdup_printf("%s%s", base, name);
        if (!strcmp(root, mine) || !g_file_test(root, G_FILE_TEST_IS_DIR)) {
            g_free(root);
            continue;
        }
        char *list = g_build_filename(root, SHADER_CACHE_LIST, NULL);
        gint64 t = shader_cache_folder_time(root, list);
        g_free(list);
        g_ptr_array_add(roots, root);
        g_array_append_val(times, t);
    }
    if (dir) {
        g_dir_close(dir);
    }

    int keep = -1;
    for (guint i = 0; i < roots->len && !every_one; i++) {
        if (keep < 0 || g_array_index(times, gint64, i) >
                        g_array_index(times, gint64, keep)) {
            keep = i;
        }
    }
    for (guint i = 0; i < roots->len; i++) {
        if ((int)i == keep) {
            continue;
        }
        const char *root = g_ptr_array_index(roots, i);
        fprintf(stderr, "nv2a: removing an older shader cache: %s\n", root);
        shader_remove_cache_folder(root);
        if (!strcmp(root, legacy)) {
            g_remove(legacy_list);
        }
    }
    g_free(legacy);
    g_free(legacy_list);
    g_ptr_array_free(roots, TRUE);
    g_array_free(times, TRUE);
}

static void shader_write_lru_list_entry_to_disk(Lru *lru, LruNode *node, void *opaque)
{
    FILE *lru_list_file = (FILE*) opaque;
    size_t written = fwrite(&node->hash, sizeof(uint64_t), 1, lru_list_file);
    if (written != 1) {
        fprintf(stderr, "nv2a: Failed to write shader list entry %llx to disk\n",
                (unsigned long long) node->hash);
    }
}

void pgraph_gl_shader_write_cache_reload_list(PGRAPHState *pg)
{
    PGRAPHGLState *r = pg->gl_renderer_state;

    if (!g_config.perf.cache_shaders) {
        qatomic_set(&r->shader_cache_writeback_pending, false);
        qemu_event_set(&r->shader_cache_writeback_complete);
        return;
    }

    char *shader_lru_path = shader_get_lru_cache_path();
    qemu_thread_join(&r->shader_disk_thread);

    FILE *lru_list = qemu_fopen(shader_lru_path, "wb");
    g_free(shader_lru_path);
    if (!lru_list) {
        fprintf(stderr, "nv2a: Failed to open shader LRU cache for writing\n");
        return;
    }

    lru_visit_active(&r->shader_cache, shader_write_lru_list_entry_to_disk, lru_list);
    fclose(lru_list);

    lru_flush(&r->shader_cache);

    qatomic_set(&r->shader_cache_writeback_pending, false);
    qemu_event_set(&r->shader_cache_writeback_complete);
}

bool pgraph_gl_shader_load_from_memory(ShaderBinding *binding)
{
    ASSERT_NO_GL_ERROR();

    if (!binding->program) {
        return false;
    }

    GLuint gl_program = glCreateProgram();
    glProgramBinary(gl_program, binding->program_format, binding->program,
                    binding->program_size);
    GLint gl_error = glGetError();
    if (gl_error != GL_NO_ERROR) {
        NV2A_DPRINTF(
            "failed to load shader binary from disk: GL error code %d\n",
            gl_error);
        glDeleteProgram(gl_program);
        return false;
    }

    GLint link_status = GL_FALSE;
    glGetProgramiv(gl_program, GL_LINK_STATUS, &link_status);
    if (!link_status) {
        NV2A_DPRINTF(
            "failed to load shader binary from disk: link status is FALSE\n");
        glDeleteProgram(gl_program);
        return false;
    }

    glUseProgram(gl_program);

    g_free(binding->program);

    binding->program = NULL;
    binding->gl_program = gl_program;
    binding->gl_primitive_mode =
        get_gl_primitive_mode(binding->state.geom.polygon_front_mode,
                              binding->state.geom.primitive_mode);
    binding->initialized = true;

    set_texture_sampler_uniforms(binding);

    glValidateProgram(gl_program);
    GLint valid = 0;
    glGetProgramiv(gl_program, GL_VALIDATE_STATUS, &valid);
    if (!valid) {
        GLchar log[1024];
        glGetProgramInfoLog(gl_program, 1024, NULL, log);
        NV2A_DPRINTF("failed to load shader binary from disk: %s\n", log);
        glDeleteProgram(gl_program);
        return false;
    }

    update_shader_uniform_locs(binding);

    return true;
}

static char *shader_get_bin_directory(uint64_t hash)
{
    char *shader_bin_dir =
        g_strdup_printf("%s/%04x", shader_cache_dir(), (uint32_t)(hash >> 48));
    return shader_bin_dir;
}

static char *shader_get_binary_path(const char *shader_bin_dir, uint64_t hash)
{
    uint64_t bin_mask = (uint64_t)0xffff << 48;
    return g_strdup_printf("%s/%012" PRIx64, shader_bin_dir, hash & ~bin_mask);
}

static void shader_load_from_disk(PGRAPHState *pg, uint64_t hash)
{
    PGRAPHGLState *r = pg->gl_renderer_state;

    char *shader_bin_dir = shader_get_bin_directory(hash);
    char *shader_path = shader_get_binary_path(shader_bin_dir, hash);
    char *cached_xemu_version = NULL;
    char *cached_gl_vendor = NULL;
    char *cached_gl_version = NULL;
    void *program_buffer = NULL;

    uint64_t cached_xemu_version_len;
    uint64_t gl_vendor_len;
    uint64_t gl_version_len;
    GLenum program_binary_format;
    ShaderState state;
    size_t shader_size;
    bool keep_file = false;

    g_free(shader_bin_dir);

    qemu_mutex_lock(&r->shader_cache_lock);
    if (lru_contains_hash(&r->shader_cache, hash)) {
        qemu_mutex_unlock(&r->shader_cache_lock);
        return;
    }
    qemu_mutex_unlock(&r->shader_cache_lock);

    FILE *shader_file = qemu_fopen(shader_path, "rb");
    if (!shader_file) {
        goto error;
    }

    size_t nread;
    #define READ_OR_ERR(data, data_len) \
        do { \
            nread = fread(data, data_len, 1, shader_file); \
            if (nread != 1) { \
                fclose(shader_file); \
                goto error; \
            } \
        } while (0)

    READ_OR_ERR(&cached_xemu_version_len, sizeof(cached_xemu_version_len));

    cached_xemu_version = g_malloc(cached_xemu_version_len +1);
    READ_OR_ERR(cached_xemu_version, cached_xemu_version_len);
    if (strcmp(cached_xemu_version, shader_cache_tag()) != 0) {
        fclose(shader_file);
        goto error;
    }

    READ_OR_ERR(&gl_vendor_len, sizeof(gl_vendor_len));

    cached_gl_vendor = g_malloc(gl_vendor_len);
    READ_OR_ERR(cached_gl_vendor, gl_vendor_len);
    if (strcmp(cached_gl_vendor, shader_gl_vendor) != 0) {
        fclose(shader_file);
        keep_file = true;
        goto error;
    }

    READ_OR_ERR(&gl_version_len, sizeof(gl_version_len));

    cached_gl_version = g_malloc(gl_version_len);
    READ_OR_ERR(cached_gl_version, gl_version_len);
    if (strcmp(cached_gl_version, shader_gl_version) != 0) {
        fclose(shader_file);
        keep_file = true;
        goto error;
    }

    READ_OR_ERR(&program_binary_format, sizeof(program_binary_format));
    READ_OR_ERR(&state, sizeof(state));
    READ_OR_ERR(&shader_size, sizeof(shader_size));

    program_buffer = g_malloc(shader_size);
    READ_OR_ERR(program_buffer, shader_size);

    #undef READ_OR_ERR

    fclose(shader_file);
    g_free(shader_path);
    g_free(cached_xemu_version);
    g_free(cached_gl_vendor);
    g_free(cached_gl_version);

    qemu_mutex_lock(&r->shader_cache_lock);
    LruNode *node = lru_lookup(&r->shader_cache, hash, &state);
    ShaderBinding *binding = container_of(node, ShaderBinding, node);

    /* If we happened to regenerate this shader already, then we may as well use the new one */
    if (binding->initialized) {
        qemu_mutex_unlock(&r->shader_cache_lock);
        return;
    }

    binding->program_format = program_binary_format;
    binding->program_size = shader_size;
    binding->program = program_buffer;
    binding->cached = true;
    qemu_mutex_unlock(&r->shader_cache_lock);
    return;

error:
    /*
     * Delete the shader so it won't be loaded again, unless it was only built
     * by another graphics driver: the precache can rebuild it from its state
     */
    if (!keep_file) {
        qemu_unlink(shader_path);
    }
    g_free(shader_path);
    g_free(program_buffer);
    g_free(cached_xemu_version);
    g_free(cached_gl_vendor);
    g_free(cached_gl_version);
}

/*
 * Fill an entry the game asks for from its file on disk, if this PC built it
 * before (in an earlier session, at another resolution scale, or in the
 * precache). Reading the file takes a moment; compiling takes tens of ms.
 * Called with shader_cache_lock held. Nothing is deleted here: the precache
 * may still be writing the file.
 */
static bool shader_fill_from_disk(ShaderBinding *binding)
{
    uint64_t hash = binding->node.hash;
    char *dir = shader_get_bin_directory(hash);
    char *path = shader_get_binary_path(dir, hash);
    FILE *f = qemu_fopen(path, "rb");
    g_free(dir);
    g_free(path);
    if (!f) {
        return false;
    }

    /* The same three strings shader_load_from_disk() checks */
    const char *want[3] = { shader_cache_tag(), shader_gl_vendor,
                            shader_gl_version };
    bool ok = true;
    for (int i = 0; i < 3 && ok; i++) {
        uint64_t len;
        char buf[256];
        ok = fread(&len, sizeof(len), 1, f) == 1 &&
             len == strlen(want[i]) + 1 && len <= sizeof(buf) &&
             fread(buf, len, 1, f) == 1 && !memcmp(buf, want[i], len);
    }

    GLenum format;
    ShaderState *state = g_new(ShaderState, 1);
    size_t size = 0;
    void *program = NULL;
    ok = ok && fread(&format, sizeof(format), 1, f) == 1 &&
         fread(state, sizeof(*state), 1, f) == 1 &&
         !memcmp(state, &binding->state, sizeof(*state)) &&
         fread(&size, sizeof(size), 1, f) == 1 && size > 0 &&
         size < 64 * 1024 * 1024;
    if (ok) {
        program = g_malloc(size);
        ok = fread(program, size, 1, f) == 1;
    }
    fclose(f);
    g_free(state);
    if (!ok) {
        g_free(program);
        return false;
    }

    g_free(binding->program);
    binding->program = program;
    binding->program_format = format;
    binding->program_size = size;
    binding->cached = true;
    return true;
}

static void *shader_reload_lru_from_disk(void *arg)
{
    if (!g_config.perf.cache_shaders) {
        return NULL;
    }

    PGRAPHState *pg = (PGRAPHState*) arg;
    char *shader_lru_path = shader_get_lru_cache_path();

    FILE *lru_shaders_list = qemu_fopen(shader_lru_path, "rb");
    g_free(shader_lru_path);
    if (!lru_shaders_list) {
        return NULL;
    }

    uint64_t hash;
    while (fread(&hash, sizeof(uint64_t), 1, lru_shaders_list) == 1) {
        shader_load_from_disk(pg, hash);
    }

    return NULL;
}

static void shader_cache_entry_init(Lru *lru, LruNode *node, const void *state)
{
    ShaderBinding *binding = container_of(node, ShaderBinding, node);
    memcpy(&binding->state, state, sizeof(ShaderState));
    binding->initialized = false;
    binding->cached = false;
    binding->program = NULL;
    binding->save_thread = NULL;
    binding->gl_program = 0;
    binding->async_pending = false;
}

static void shader_cache_entry_post_evict(Lru *lru, LruNode *node)
{
    ShaderBinding *binding = container_of(node, ShaderBinding, node);

    if (binding->save_thread) {
        qemu_thread_join(binding->save_thread);
        g_free(binding->save_thread);
    }

    glDeleteProgram(binding->gl_program);
    if (binding->program) {
        g_free(binding->program);
    }

    binding->cached = false;
    binding->save_thread = NULL;
    binding->program = NULL;
    memset(&binding->state, 0, sizeof(ShaderState));
}

static bool shader_cache_entry_compare(Lru *lru, LruNode *node, const void *key)
{
    ShaderBinding *binding = container_of(node, ShaderBinding, node);
    return memcmp(&binding->state, key, sizeof(ShaderState));
}

void pgraph_gl_init_shaders(PGRAPHState *pg)
{
    PGRAPHGLState *r = pg->gl_renderer_state;

    qemu_mutex_init(&r->shader_cache_lock);
    qemu_event_init(&r->shader_cache_writeback_complete, false);

    if (!shader_gl_vendor) {
        shader_gl_vendor = (const char *) glGetString(GL_VENDOR);
    }
    if (!shader_gl_version) {
        shader_gl_version = (const char *) glGetString(GL_VERSION);
    }
    if (!shader_gl_renderer) {
        shader_gl_renderer = (const char *) glGetString(GL_RENDERER);
    }

    shader_create_cache_folder();

    /* FIXME: Make this configurable */
    const size_t shader_cache_size = 50*1024;
    lru_init(&r->shader_cache);
    r->shader_cache_entries = malloc(shader_cache_size * sizeof(ShaderBinding));
    assert(r->shader_cache_entries != NULL);
    for (int i = 0; i < shader_cache_size; i++) {
        lru_add_free(&r->shader_cache, &r->shader_cache_entries[i].node);
    }

    r->shader_cache.init_node = shader_cache_entry_init;
    r->shader_cache.compare_nodes = shader_cache_entry_compare;
    r->shader_cache.post_node_evict = shader_cache_entry_post_evict;

    qemu_thread_create(&r->shader_disk_thread, "pgraph.renderer_state->shader_cache",
                       shader_reload_lru_from_disk, pg, QEMU_THREAD_JOINABLE);

    /* FIXME: Make this configurable */
    const size_t shader_module_cache_size = 50*1024;
    lru_init(&r->shader_module_cache);
    r->shader_module_cache_entries =
        g_malloc_n(shader_module_cache_size, sizeof(ShaderModuleCacheEntry));
    assert(r->shader_module_cache_entries != NULL);
    for (int i = 0; i < shader_module_cache_size; i++) {
        lru_add_free(&r->shader_module_cache, &r->shader_module_cache_entries[i].node);
    }

    precache_attach(pg);
    async_init();

    r->shader_module_cache.init_node = shader_module_cache_entry_init;
    r->shader_module_cache.compare_nodes = shader_module_cache_entry_compare;
    r->shader_module_cache.post_node_evict = shader_module_cache_entry_post_evict;
}

void pgraph_gl_finalize_shaders(PGRAPHState *pg)
{
    PGRAPHGLState *r = pg->gl_renderer_state;

    precache_detach();

    // Clear out shader cache
    pgraph_gl_shader_write_cache_reload_list(pg); // FIXME: also flushes, rename for clarity
    free(r->shader_cache_entries);
    r->shader_cache_entries = NULL;

    lru_flush(&r->shader_module_cache);
    g_free(r->shader_module_cache_entries);
    r->shader_module_cache_entries = NULL;

    qemu_mutex_destroy(&r->shader_cache_lock);
}

static void *shader_write_to_disk(void *arg)
{
    ShaderBinding *binding = (ShaderBinding*) arg;

    char *shader_bin = shader_get_bin_directory(binding->node.hash);
    char *shader_path = shader_get_binary_path(shader_bin, binding->node.hash);

    static uint64_t gl_vendor_len;
    if (gl_vendor_len == 0) {
        gl_vendor_len = (uint64_t) (strlen(shader_gl_vendor) + 1);
    }

    static uint64_t gl_version_len;
    if (gl_version_len == 0) {
        gl_version_len = (uint64_t) (strlen(shader_gl_version) + 1);
    }

    static uint64_t xemu_version_len = 0;
    if (xemu_version_len == 0) {
        xemu_version_len = (uint64_t) (strlen(shader_cache_tag()) + 1);
    }

    qemu_mkdir(shader_bin);
    g_free(shader_bin);

    FILE *shader_file = qemu_fopen(shader_path, "wb");
    if (!shader_file) {
        goto error;
    }

    size_t written;
    #define WRITE_OR_ERR(data, data_size) \
        do { \
            written = fwrite(data, data_size, 1, shader_file); \
            if (written != 1) { \
                fclose(shader_file); \
                goto error; \
            } \
        } while (0)

    WRITE_OR_ERR(&xemu_version_len, sizeof(xemu_version_len));
    WRITE_OR_ERR(shader_cache_tag(), xemu_version_len);

    WRITE_OR_ERR(&gl_vendor_len, sizeof(gl_vendor_len));
    WRITE_OR_ERR(shader_gl_vendor, gl_vendor_len);

    WRITE_OR_ERR(&gl_version_len, sizeof(gl_version_len));
    WRITE_OR_ERR(shader_gl_version, gl_version_len);

    WRITE_OR_ERR(&binding->program_format, sizeof(binding->program_format));
    WRITE_OR_ERR(&binding->state, sizeof(binding->state));

    WRITE_OR_ERR(&binding->program_size, sizeof(binding->program_size));
    WRITE_OR_ERR(binding->program, binding->program_size);

    #undef WRITE_OR_ERR

    fclose(shader_file);

    g_free(shader_path);
    g_free(binding->program);
    binding->program = NULL;

    return NULL;

error:
    fprintf(stderr, "nv2a: Failed to write shader binary file to %s\n", shader_path);
    qemu_unlink(shader_path);
    g_free(shader_path);
    g_free(binding->program);
    binding->program = NULL;
    return NULL;
}

void pgraph_gl_shader_cache_to_disk(ShaderBinding *binding)
{
    if (binding->cached) {
        return;
    }

    GLint program_size;
    glGetProgramiv(binding->gl_program, GL_PROGRAM_BINARY_LENGTH, &program_size);

    if (binding->program) {
        g_free(binding->program);
        binding->program = NULL;
    }

    /* program_size might be zero on some systems, if no binary formats are supported */
    if (program_size == 0) {
        return;
    }

    binding->program = g_malloc(program_size);
    GLsizei program_size_copied;
    glGetProgramBinary(binding->gl_program, program_size, &program_size_copied,
                       &binding->program_format, binding->program);
    ASSERT_NO_GL_ERROR();

    binding->program_size = program_size_copied;
    binding->cached = true;

    char name[24];
    snprintf(name, sizeof(name), "scache-%llx", (unsigned long long) binding->node.hash);
    binding->save_thread = g_malloc0(sizeof(QemuThread));
    qemu_thread_create(binding->save_thread, name, shader_write_to_disk, binding, QEMU_THREAD_JOINABLE);
}

static void apply_uniform_updates(const UniformInfo *info, int *locs,
                                  void *values, size_t count)
{
    for (int i = 0; i < count; i++) {
        if (locs[i] == -1) {
            continue;
        }

        void *value = (char*)values + info[i].val_offs;

        switch (info[i].type) {
        case UniformElementType_uint:
            glUniform1uiv(locs[i], info[i].count, value);
            break;
        case UniformElementType_int:
            glUniform1iv(locs[i], info[i].count, value);
            break;
        case UniformElementType_ivec2:
            glUniform2iv(locs[i], info[i].count, value);
            break;
        case UniformElementType_ivec4:
            glUniform4iv(locs[i], info[i].count, value);
            break;
        case UniformElementType_float:
            glUniform1fv(locs[i], info[i].count, value);
            break;
        case UniformElementType_vec2:
            glUniform2fv(locs[i], info[i].count, value);
            break;
        case UniformElementType_vec3:
            glUniform3fv(locs[i], info[i].count, value);
            break;
        case UniformElementType_vec4:
            glUniform4fv(locs[i], info[i].count, value);
            break;
        case UniformElementType_mat2:
            glUniformMatrix2fv(locs[i], info[i].count, GL_FALSE, value);
            break;
        default:
            g_assert_not_reached();
        }
    }

    ASSERT_NO_GL_ERROR();
}

// FIXME: Dirty tracking
// FIXME: Consider UBO to align with VK renderer
static void update_shader_uniforms(PGRAPHState *pg, ShaderBinding *binding)
{
    PGRAPHGLState *r = pg->gl_renderer_state;

    VshUniformValues vsh_values;
    pgraph_glsl_set_vsh_uniform_values(pg, &binding->state.vsh,
                                  binding->uniform_locs.vsh, &vsh_values);
    apply_uniform_updates(VshUniformInfo, binding->uniform_locs.vsh,
                          &vsh_values, VshUniform__COUNT);

    PshUniformValues psh_values;
    pgraph_glsl_set_psh_uniform_values(pg, binding->uniform_locs.psh, &psh_values);

    for (int i = 0; i < 4; i++) {
        if (r->texture_binding[i] != NULL) {
            float scale = r->texture_binding[i]->scale;
            psh_values.texScale[i] = scale;
        }
    }
    apply_uniform_updates(PshUniformInfo, binding->uniform_locs.psh,
                          &psh_values, PshUniform__COUNT);
}

void pgraph_gl_bind_shaders(PGRAPHState *pg)
{
    PGRAPHGLState *r = pg->gl_renderer_state;

    bool binding_changed = false;
    /* A stand-in is never kept: look again whether the shader is ready */
    if (r->shader_binding && r->shader_binding != &async_null_binding &&
        !pgraph_glsl_check_shader_state_dirty(pg, &r->shader_binding->state)) {
        nv2a_profile_inc_counter(NV2A_PROF_SHADER_BIND_NOTDIRTY);
        goto update_uniforms;
    }

    ShaderBinding *old_binding = r->shader_binding;
    ShaderState state = pgraph_glsl_get_shader_state(pg);

    NV2A_GL_DGROUP_BEGIN("%s (%s)", __func__,
                         state.vsh.is_fixed_function ? "FF" : "PROG");

    qemu_mutex_lock(&r->shader_cache_lock);

    uint64_t shader_state_hash =
        fast_hash((uint8_t *)&state, sizeof(ShaderState));

    LruNode *node = lru_lookup(&r->shader_cache, shader_state_hash, &state);
    ShaderBinding *binding = container_of(node, ShaderBinding, node);

    bool stand_in = false;
    if (binding->async_pending) {
        int64_t perf_start = xemu_perf_start();
        GLuint pending = binding->gl_program;
        if (binding->program && pgraph_gl_shader_load_from_memory(binding)) {
            /* The cache on disk delivered it first */
            binding->async_pending = false;
            glDeleteProgram(pending);
            xemu_perf_end(XEMU_PERF_SHADER_LOAD, perf_start);
        } else if (!async_ready(binding)) {
            stand_in = true;
        } else {
            /* A shader still not ready after a few frames is waited for */
            bool late = g_nv2a_stats.frame_count - binding->async_frame >=
                        ASYNC_MAX_FRAMES;
            if (!async_finish(binding)) {
                generate_shaders(r, binding);
            }
            if (g_config.perf.cache_shaders) {
                pgraph_gl_shader_cache_to_disk(binding);
            }
            xemu_perf_end(late ? XEMU_PERF_SHADER_GEN : XEMU_PERF_SHADER_ASYNC,
                          perf_start);
        }
    } else if (!binding->initialized) {
        int64_t perf_start = xemu_perf_start();
        if (!binding->program && g_config.perf.cache_shaders) {
            shader_fill_from_disk(binding);
        }
        bool loaded = pgraph_gl_shader_load_from_memory(binding);
        if (!loaded && binding->program) {
            /* A binary the driver rejects: build it and write it again */
            g_free(binding->program);
            binding->program = NULL;
            binding->cached = false;
        }
        if (loaded) {
            xemu_perf_end(XEMU_PERF_SHADER_LOAD, perf_start);
        } else if (async_supported && g_config.perf.shader_async) {
            nv2a_profile_inc_counter(NV2A_PROF_SHADER_GEN);
            learned_add(&state);
            async_start(binding);
            stand_in = true;
            xemu_perf_end(XEMU_PERF_SHADER_ASYNC, perf_start);
        } else {
            nv2a_profile_inc_counter(NV2A_PROF_SHADER_GEN);
            learned_add(&state);
            generate_shaders(r, binding);
            if (g_config.perf.cache_shaders) {
                pgraph_gl_shader_cache_to_disk(binding);
            }
            xemu_perf_end(XEMU_PERF_SHADER_GEN, perf_start);
        }
    }
    if (stand_in) {
        binding = async_stand_in(&state);
        xemu_perf_end(XEMU_PERF_DRAW_SKIPPED, xemu_perf_start());
    }

    assert(binding->initialized);
    r->shader_binding = binding;
    pg->program_data_dirty = false;

    qemu_mutex_unlock(&r->shader_cache_lock);

    binding_changed = (r->shader_binding != old_binding);
    if (binding_changed) {
        nv2a_profile_inc_counter(NV2A_PROF_SHADER_BIND);
        glUseProgram(r->shader_binding->gl_program);
    }

    // FIXME: Clear only the registers that are consumed by this binding.
    pgraph_clear_dirty_reg_map(pg);

    NV2A_GL_DGROUP_END();

update_uniforms:
    assert(r->shader_binding);
    assert(r->shader_binding->initialized);
    update_shader_uniforms(pg, r->shader_binding);
}

GLuint pgraph_gl_compile_shader(const char *vs_src, const char *fs_src)
{
    GLint status;
    char err_buf[512];

    // Compile vertex shader
    GLuint vs = glCreateShader(GL_VERTEX_SHADER);
    glShaderSource(vs, 1, &vs_src, NULL);
    glCompileShader(vs);
    glGetShaderiv(vs, GL_COMPILE_STATUS, &status);
    if (status != GL_TRUE) {
        glGetShaderInfoLog(vs, sizeof(err_buf), NULL, err_buf);
        err_buf[sizeof(err_buf)-1] = '\0';
        fprintf(stderr, "Vertex shader compilation failed: %s\n", err_buf);
        exit(1);
    }

    // Compile fragment shader
    GLuint fs = glCreateShader(GL_FRAGMENT_SHADER);
    glShaderSource(fs, 1, &fs_src, NULL);
    glCompileShader(fs);
    glGetShaderiv(fs, GL_COMPILE_STATUS, &status);
    if (status != GL_TRUE) {
        glGetShaderInfoLog(fs, sizeof(err_buf), NULL, err_buf);
        err_buf[sizeof(err_buf)-1] = '\0';
        fprintf(stderr, "Fragment shader compilation failed: %s\n", err_buf);
        exit(1);
    }

    // Link vertex and fragment shaders
    GLuint prog = glCreateProgram();
    glAttachShader(prog, vs);
    glAttachShader(prog, fs);
    glLinkProgram(prog);
    glUseProgram(prog);

    // Flag shaders for deletion (will still be retained for lifetime of prog)
    glDeleteShader(vs);
    glDeleteShader(fs);

    return prog;
}

/*
 * Shader precache: build the shaders of the shipped list in the background,
 * on a GL context of their own, and put them in the cache on disk and in
 * memory. The game then finds them ready instead of compiling them.
 */

#define SHADER_LIST_MAGIC "XBLSHL01"

static struct {
    QemuMutex lock;          /* guards status and pg */
    PGRAPHState *pg;         /* set while the OpenGL renderer runs */
    QemuThread thread;
    bool thread_started;
    bool build;              /* the thread builds, not only counts */
    bool stop;
    ShaderPrecacheStatus status;
} precache;

QEMU_BUILD_BUG_ON(sizeof(ShaderStateV1) == sizeof(ShaderState));

/*
 * Turn a state saved by xemu 0.8.137 or earlier into the state the current
 * shader generator makes for it.
 * - specular_power(_back) are uniforms now, so they are dropped.
 * - texgen_infinite_viewer is new; Black Arrow never sets it (coverage run
 *   on Agora and Presidio, 2026-10-20: 363 fixed-function states, all off).
 * - point_sprite is now off unless points are drawn, and the game draws
 *   none with it, so it is cleared.
 */
static void shader_state_from_v1(const ShaderStateV1 *o, ShaderState *out)
{
    ShaderState n;
    memset(&n, 0, sizeof(n));

    const VshStateV1 *ov = &o->vsh;
    VshState *v = &n.vsh;
    v->surface_scale_factor = ov->surface_scale_factor;
    v->compressed_attrs = ov->compressed_attrs;
    v->uniform_attrs = ov->uniform_attrs;
    v->swizzle_attrs = ov->swizzle_attrs;
    v->fog_enable = ov->fog_enable;
    v->fog_mode = ov->fog_mode;
    v->specular_enable = ov->specular_enable;
    v->separate_specular = ov->separate_specular;
    v->ignore_specular_alpha = ov->ignore_specular_alpha;
    v->point_params_enable = ov->point_params_enable;
    v->point_size = ov->point_size;
    memcpy(v->point_params, ov->point_params, sizeof(v->point_params));
    v->smooth_shading = ov->smooth_shading;
    v->z_perspective = ov->z_perspective;
    v->is_fixed_function = ov->is_fixed_function;

    const FixedFunctionVshStateV1 *of = &ov->fixed_function;
    FixedFunctionVshState *f = &v->fixed_function;
    f->normalization = of->normalization;
    memcpy(f->texture_matrix_enable, of->texture_matrix_enable,
           sizeof(f->texture_matrix_enable));
    memcpy(f->texgen, of->texgen, sizeof(f->texgen));
    f->foggen = of->foggen;
    f->skinning = of->skinning;
    f->lighting = of->lighting;
    memcpy(f->light, of->light, sizeof(f->light));
    f->emission_src = of->emission_src;
    f->ambient_src = of->ambient_src;
    f->diffuse_src = of->diffuse_src;
    f->specular_src = of->specular_src;
    f->local_eye = of->local_eye;
    f->texgen_infinite_viewer = false;

    QEMU_BUILD_BUG_ON(sizeof(v->programmable) != sizeof(ov->programmable));
    memcpy(&v->programmable, &ov->programmable, sizeof(v->programmable));
    n.geom = o->geom;
    n.psh = o->psh;
    n.psh.point_sprite = false;

    *out = n;
}

/*
 * The shaders of a list file, or NULL. *count is -1 when there is no file.
 * *converted is set when the file had the 0.8.137 layout.
 */
static ShaderState *shader_list_load(const char *file, int *count,
                                     bool *converted)
{
    char *path = g_strdup_printf("%s%s", xemu_settings_get_base_path(), file);
    gchar *data = NULL;
    gsize len = 0;
    ShaderState *states = NULL;

    *count = -1;
    *converted = false;
    if (g_file_get_contents(path, &data, &len, NULL)) {
        *count = 0;
        uint32_t size = len >= 16 ? ldl_le_p(data + 8) : 0;
        uint32_t n = len >= 16 ? ldl_le_p(data + 12) : 0;
        bool magic = len >= 16 && !memcmp(data, SHADER_LIST_MAGIC, 8);
        if (magic && size == sizeof(ShaderState) &&
            len == 16 + (gsize)n * sizeof(ShaderState)) {
            *count = n;
            states = g_memdup2(data + 16, *count * sizeof(ShaderState));
        } else if (magic && size == sizeof(ShaderStateV1) &&
                   len == 16 + (gsize)n * sizeof(ShaderStateV1)) {
            states = g_new(ShaderState, MAX(n, 1));
            for (uint32_t i = 0; i < n; i++) {
                ShaderStateV1 old;
                memcpy(&old, data + 16 + i * sizeof(old), sizeof(old));
                shader_state_from_v1(&old, states + i);
            }
            *count = n;
            *converted = true;
            fprintf(stderr, "nv2a: %s converted from the 0.8.137 layout, "
                    "%u -> %d shaders\n", path, n, *count);
            xemu_diag_event("%s converted from the old shader layout, "
                            "%u -> %d", file, n, *count);
        } else {
            fprintf(stderr, "nv2a: %s does not match this emulator\n", path);
        }
    }
    g_free(data);
    g_free(path);
    return states;
}

/* Whether a usable compiled shader for the hash is on disk */
static bool shader_on_disk(uint64_t hash)
{
    char *dir = shader_get_bin_directory(hash);
    char *path = shader_get_binary_path(dir, hash);
    FILE *f = qemu_fopen(path, "rb");
    bool ok;

    g_free(dir);
    g_free(path);
    if (!f) {
        return false;
    }

    /* The same three strings shader_load_from_disk() checks */
    const char *want[3] = { shader_cache_tag(), shader_gl_vendor,
                            shader_gl_version };
    ok = true;
    for (int i = 0; i < 3 && ok; i++) {
        uint64_t len;
        char buf[256];
        ok = fread(&len, sizeof(len), 1, f) == 1 &&
             len == strlen(want[i]) + 1 && len <= sizeof(buf) &&
             fread(buf, len, 1, f) == 1 && !memcmp(buf, want[i], len);
    }
    fclose(f);
    return ok;
}

/* Count the files and bytes of the cache on disk */
static void shader_disk_usage(int *count, uint64_t *bytes)
{
    char *root = g_strdup(shader_cache_dir());
    GDir *dir = g_dir_open(root, 0, NULL);
    const char *name;

    *count = 0;
    *bytes = 0;
    while (dir && (name = g_dir_read_name(dir))) {
        char *sub_path = g_build_filename(root, name, NULL);
        GDir *sub = g_dir_open(sub_path, 0, NULL);
        const char *file;
        while (sub && (file = g_dir_read_name(sub))) {
            char *file_path = g_build_filename(sub_path, file, NULL);
            GStatBuf st;
            if (g_stat(file_path, &st) == 0) {
                (*count)++;
                *bytes += st.st_size;
            }
            g_free(file_path);
        }
        if (sub) {
            g_dir_close(sub);
        }
        g_free(sub_path);
    }
    if (dir) {
        g_dir_close(dir);
    }
    g_free(root);
}

/* Compile one stage without aborting on failure; 0 if it fails */
static GLuint precache_compile(GLenum kind, MString *code)
{
    const char *src = mstring_get_str(code);
    GLuint shader = glCreateShader(kind);
    GLint compiled = 0;

    glShaderSource(shader, 1, &src, 0);
    glCompileShader(shader);
    glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
    mstring_unref(code);
    if (!compiled) {
        glDeleteShader(shader);
        return 0;
    }
    return shader;
}

/*
 * Build the program for a state the way generate_shaders() does and save it
 * the way pgraph_gl_shader_cache_to_disk() does. Returns false if it failed,
 * and sets *unsupported when the driver cannot hand out program binaries.
 */
static bool precache_build(const ShaderState *state, uint64_t hash,
                           bool *unsupported)
{
    GLuint program = glCreateProgram();
    GLuint stages[3];
    int n = 0;
    bool ok = true;

    bool need_geometry_shader = pgraph_glsl_need_geom(&state->geom);
    if (need_geometry_shader) {
        GenGeomGlslOptions geom_opts;
        memset(&geom_opts, 0, sizeof(geom_opts));
        stages[n++] = precache_compile(
            GL_GEOMETRY_SHADER, pgraph_glsl_gen_geom(&state->geom, geom_opts));
    }
    GenVshGlslOptions vsh_opts;
    memset(&vsh_opts, 0, sizeof(vsh_opts));
    vsh_opts.prefix_outputs = need_geometry_shader;
    stages[n++] = precache_compile(GL_VERTEX_SHADER,
                                   pgraph_glsl_gen_vsh(&state->vsh, vsh_opts));
    GenPshGlslOptions psh_opts;
    memset(&psh_opts, 0, sizeof(psh_opts));
    stages[n++] = precache_compile(GL_FRAGMENT_SHADER,
                                   pgraph_glsl_gen_psh(&state->psh, psh_opts));

    for (int i = 0; i < n; i++) {
        ok &= stages[i] != 0;
        if (stages[i]) {
            glAttachShader(program, stages[i]);
        }
    }

    GLint linked = 0;
    if (ok) {
        glLinkProgram(program);
        glGetProgramiv(program, GL_LINK_STATUS, &linked);
        ok = linked;
    }

    GLint size = 0;
    if (ok) {
        glGetProgramiv(program, GL_PROGRAM_BINARY_LENGTH, &size);
        if (size <= 0) {
            *unsupported = true;
            ok = false;
        }
    }
    if (ok) {
        ShaderBinding *binding = g_malloc0(sizeof(*binding));
        GLsizei copied = 0;
        binding->node.hash = hash;
        binding->state = *state;
        binding->program = g_malloc(size);
        glGetProgramBinary(program, size, &copied, &binding->program_format,
                           binding->program);
        binding->program_size = copied;
        ok = glGetError() == GL_NO_ERROR && copied > 0;
        if (ok) {
            /* Writes the file and frees binding->program */
            shader_write_to_disk(binding);
        } else {
            g_free(binding->program);
        }
        g_free(binding);
    }

    for (int i = 0; i < n; i++) {
        if (stages[i]) {
            glDetachShader(program, stages[i]);
            glDeleteShader(stages[i]);
        }
    }
    glDeleteProgram(program);
    return ok;
}

/* The state stored in one of this PC's cache files, whatever its driver */
static bool shader_file_state(const char *path, ShaderState *state)
{
    FILE *f = qemu_fopen(path, "rb");
    const char *tag = shader_cache_tag();
    GLenum format;
    bool ok = f != NULL;

    /* Cache tag, GL vendor, GL version; only the tag has to match */
    for (int i = 0; i < 3 && ok; i++) {
        uint64_t len;
        char buf[256];
        ok = fread(&len, sizeof(len), 1, f) == 1 && len <= sizeof(buf) &&
             fread(buf, len, 1, f) == 1;
        if (ok && i == 0) {
            ok = len == strlen(tag) + 1 && !memcmp(buf, tag, len);
        }
    }
    ok = ok && fread(&format, sizeof(format), 1, f) == 1 &&
         fread(state, sizeof(*state), 1, f) == 1;
    if (f) {
        fclose(f);
    }
    return ok;
}

/* Add the state of every shader in this PC's cache */
static void precache_add_own(GArray *states)
{
    char *root = g_strdup(shader_cache_dir());
    GDir *dir = g_dir_open(root, 0, NULL);
    const char *name;

    while (dir && (name = g_dir_read_name(dir))) {
        char *sub_path = g_build_filename(root, name, NULL);
        GDir *sub = g_dir_open(sub_path, 0, NULL);
        const char *file;
        while (sub && (file = g_dir_read_name(sub))) {
            char *file_path = g_build_filename(sub_path, file, NULL);
            ShaderState state;
            if (shader_file_state(file_path, &state)) {
                g_array_append_val(states, state);
            }
            g_free(file_path);
        }
        if (sub) {
            g_dir_close(sub);
        }
        g_free(sub_path);
    }
    if (dir) {
        g_dir_close(dir);
    }
    g_free(root);
}

/*
 * This PC's own list (shader-list-mine.bin): every shader the game had to
 * build here, plus the states in this PC's cache files. It holds no compiled
 * code, so it works with any driver, and it survives Clear Cache and our
 * updates. Kept in memory and written in the background a few seconds after
 * it grew, through a temporary file so a crash cannot damage it.
 */

#define LEARNED_SAVE_DELAY_MS 5000

static struct {
    QemuMutex lock;          /* guards everything below but save_lock */
    QemuMutex save_lock;     /* held while the file is written */
    bool ready;
    GArray *states;          /* ShaderState, scale set to 1 */
    GHashTable *hashes;      /* their hashes */
    bool dirty;
    bool saving;             /* a background save is running */
    int64_t dirty_since;
} learned;

/* The state as kept in a list: the scale is set again when it is built */
static uint64_t learned_key(const ShaderState *state, ShaderState *out)
{
    *out = *state;
    out->vsh.surface_scale_factor = 1;
    return fast_hash((uint8_t *)out, sizeof(*out));
}

static void learned_insert_locked(const ShaderState *state)
{
    ShaderState s;
    uint64_t hash = learned_key(state, &s);
    if (g_hash_table_contains(learned.hashes, &hash)) {
        return;
    }
    uint64_t *key = g_new(uint64_t, 1);
    *key = hash;
    g_hash_table_add(learned.hashes, key);
    g_array_append_val(learned.states, s);
    if (!learned.dirty) {
        learned.dirty = true;
        learned.dirty_since = qemu_clock_get_ms(QEMU_CLOCK_REALTIME);
    }
}

static void learned_init(void)
{
    if (learned.ready) {
        return;
    }
    qemu_mutex_init(&learned.lock);
    qemu_mutex_init(&learned.save_lock);
    learned.states = g_array_new(FALSE, FALSE, sizeof(ShaderState));
    learned.hashes = g_hash_table_new_full(g_int64_hash, g_int64_equal,
                                           g_free, NULL);

    int count;
    bool converted;
    ShaderState *states = shader_list_load(NV2A_SHADER_LEARNED_FILE, &count,
                                           &converted);
    if (count == 0 && !states) {
        /* A file that does not match: keep it aside, start a new one */
        char *path = g_strdup_printf("%s%s", xemu_settings_get_base_path(),
                                     NV2A_SHADER_LEARNED_FILE);
        if (g_file_test(path, G_FILE_TEST_EXISTS)) {
            char *bad = g_strdup_printf("%s.bad", path);
            g_remove(bad);
            g_rename(path, bad);
            xemu_diag_event("%s unreadable, set aside", NV2A_SHADER_LEARNED_FILE);
            g_free(bad);
        }
        g_free(path);
    }
    for (int i = 0; i < count; i++) {
        learned_insert_locked(&states[i]);
    }
    g_free(states);
    learned.dirty = false;

    /*
     * The list of builds up to 20, read only: the first start after the
     * update takes all of it, later starts what was played after a rollback.
     */
    int old_count;
    states = shader_list_load(NV2A_SHADER_LEARNED_V1_FILE, &old_count,
                              &converted);
    guint before = learned.states->len;
    for (int i = 0; i < old_count; i++) {
        learned_insert_locked(&states[i]);
    }
    g_free(states);
    if (learned.states->len > before) {
        fprintf(stderr, "nv2a: %u shaders added from %s\n",
                learned.states->len - before, NV2A_SHADER_LEARNED_V1_FILE);
        learned.dirty = true;
        learned.dirty_since = qemu_clock_get_ms(QEMU_CLOCK_REALTIME);
    }
    learned.ready = true;
    fprintf(stderr, "nv2a: %d shaders in this PC's own list\n",
            (int)learned.states->len);
}

static void learned_add(const ShaderState *state)
{
    if (!learned.ready) {
        return;
    }
    qemu_mutex_lock(&learned.lock);
    learned_insert_locked(state);
    qemu_mutex_unlock(&learned.lock);
}

/* Write the list if it grew; waits for a background save still running */
static void learned_save(void)
{
    if (!learned.ready) {
        return;
    }
    qemu_mutex_lock(&learned.save_lock);
    qemu_mutex_lock(&learned.lock);
    if (!learned.dirty) {
        qemu_mutex_unlock(&learned.lock);
        qemu_mutex_unlock(&learned.save_lock);
        return;
    }
    guint count = learned.states->len;
    ShaderState *copy = g_memdup2(learned.states->data,
                                  count * sizeof(ShaderState));
    learned.dirty = false;
    qemu_mutex_unlock(&learned.lock);

    char *path = g_strdup_printf("%s%s", xemu_settings_get_base_path(),
                                 NV2A_SHADER_LEARNED_FILE);
    char *tmp = g_strdup_printf("%s.tmp", path);
    FILE *f = qemu_fopen(tmp, "wb");
    uint8_t header[16];
    memcpy(header, SHADER_LIST_MAGIC, 8);
    stl_le_p(header + 8, sizeof(ShaderState));
    stl_le_p(header + 12, count);
    bool ok = f && fwrite(header, sizeof(header), 1, f) == 1 &&
              (count == 0 ||
               fwrite(copy, sizeof(ShaderState), count, f) == count);
    if (f) {
        ok = fclose(f) == 0 && ok;
    }
    /* g_rename replaces the old file, on Windows too */
    ok = ok && g_rename(tmp, path) == 0;
    if (!ok) {
        fprintf(stderr, "nv2a: could not save %s\n", path);
        g_remove(tmp);
        qemu_mutex_lock(&learned.lock);
        learned.dirty = true;
        learned.dirty_since = qemu_clock_get_ms(QEMU_CLOCK_REALTIME);
        qemu_mutex_unlock(&learned.lock);
    }
    g_free(tmp);
    g_free(path);
    g_free(copy);
    qemu_mutex_unlock(&learned.save_lock);
}

static void *learned_save_thread(void *arg)
{
    learned_save();
    qemu_mutex_lock(&learned.lock);
    learned.saving = false;
    qemu_mutex_unlock(&learned.lock);
    return NULL;
}

/* Called often (each UI frame): save in the background once it settled */
static void learned_tick(void)
{
    if (!learned.ready) {
        return;
    }
    qemu_mutex_lock(&learned.lock);
    bool go = learned.dirty && !learned.saving &&
              qemu_clock_get_ms(QEMU_CLOCK_REALTIME) - learned.dirty_since >=
                  LEARNED_SAVE_DELAY_MS;
    if (go) {
        learned.saving = true;
    }
    qemu_mutex_unlock(&learned.lock);
    if (go) {
        QemuThread thread;
        qemu_thread_create(&thread, "nv2a.learned", learned_save_thread, NULL,
                           QEMU_THREAD_DETACHED);
    }
}

static int learned_count(void)
{
    if (!learned.ready) {
        return 0;
    }
    qemu_mutex_lock(&learned.lock);
    int n = learned.states->len;
    qemu_mutex_unlock(&learned.lock);
    return n;
}

/*
 * Crash guard. Before each shader is precached its hash is written to the
 * guard file, which is removed when the run ends. If the guard file is there
 * at the next start, that shader took the emulator down (a driver crash), so
 * it goes in the skip file and is never precached again.
 */

static GHashTable *precache_skip;  /* hashes never to precache */

static char *base_file(const char *name)
{
    return g_strdup_printf("%s%s", xemu_settings_get_base_path(), name);
}

static void guard_check(void)
{
    if (precache_skip) {
        return;
    }
    precache_skip = g_hash_table_new_full(g_int64_hash, g_int64_equal, g_free,
                                          NULL);

    char *guard = base_file(NV2A_SHADER_GUARD_FILE);
    char *skip = base_file(NV2A_SHADER_SKIP_FILE);
    gchar *text = NULL;
    if (g_file_get_contents(guard, &text, NULL, NULL)) {
        uint64_t hash = g_ascii_strtoull(text, NULL, 16);
        if (hash) {
            FILE *f = qemu_fopen(skip, "a");
            if (f) {
                fprintf(f, "%016" PRIx64 "\n", hash);
                fclose(f);
            }
            fprintf(stderr, "nv2a: the last precache stopped on shader "
                    "%016" PRIx64 "; it will be skipped\n", hash);
            xemu_diag_event("shader precache crashed on %016" PRIx64
                            ", skipped from now on", hash);
        }
        g_free(text);
        g_remove(guard);
    }

    text = NULL;
    if (g_file_get_contents(skip, &text, NULL, NULL)) {
        gchar **lines = g_strsplit(text, "\n", -1);
        for (int i = 0; lines[i]; i++) {
            uint64_t hash = g_ascii_strtoull(g_strstrip(lines[i]), NULL, 16);
            if (hash) {
                uint64_t *key = g_new(uint64_t, 1);
                *key = hash;
                g_hash_table_add(precache_skip, key);
            }
        }
        g_strfreev(lines);
        g_free(text);
    }
    g_free(guard);
    g_free(skip);
}

static void guard_set(uint64_t hash)
{
    char *guard = base_file(NV2A_SHADER_GUARD_FILE);
    FILE *f = qemu_fopen(guard, "wb");
    if (f) {
        fprintf(f, "%016" PRIx64 "\n", hash);
        fclose(f);
    }
    g_free(guard);
}

static void guard_clear(void)
{
    char *guard = base_file(NV2A_SHADER_GUARD_FILE);
    g_remove(guard);
    g_free(guard);
}

/*
 * Resolution scales to build for: the ones the player has used (remembered
 * in the settings) plus 2x and 3x by default, so a switch in a match finds
 * them ready. Each scale is about 125 MB and 2-3 minutes for the full list.
 */
#define PRECACHE_MAX_SCALE 10
#define PRECACHE_BIT(s) (1u << ((s) - 1))

static unsigned int precache_targets(unsigned int now)
{
    unsigned int mask = (unsigned int)g_config.perf.shader_precache_scales;
    if (now >= 1 && now <= PRECACHE_MAX_SCALE) {
        mask |= PRECACHE_BIT(now);
    }
    mask &= PRECACHE_BIT(PRECACHE_MAX_SCALE + 1) - 1;
    g_config.perf.shader_precache_scales = (int)mask;
    return mask;
}

/* The game's scale first, then 3x, 2x, 1x, then the higher ones; 0 if done */
static unsigned int precache_next_scale(unsigned int targets,
                                        unsigned int finished,
                                        unsigned int now)
{
    static const unsigned int order[] = { 3, 2, 1, 4, 5, 6, 7, 8, 9, 10 };
    unsigned int todo = targets & ~finished;
    if (now >= 1 && now <= PRECACHE_MAX_SCALE && (todo & PRECACHE_BIT(now))) {
        return now;
    }
    for (size_t i = 0; i < ARRAY_SIZE(order); i++) {
        if (todo & PRECACHE_BIT(order[i])) {
            return order[i];
        }
    }
    return 0;
}

/* How many of the list's states are not on disk yet at this scale */
static int precache_missing(const ShaderState *states, int count,
                            unsigned int scale, int *skipped)
{
    int missing = 0;
    for (int i = 0; i < count; i++) {
        ShaderState state = states[i];
        state.vsh.surface_scale_factor = scale;
        uint64_t hash = fast_hash((uint8_t *)&state, sizeof(state));
        if (g_hash_table_contains(precache_skip, &hash)) {
            if (skipped) {
                (*skipped)++;
            }
        } else if (!shader_on_disk(hash)) {
            missing++;
        }
    }
    return missing;
}

static void *precache_thread(void *arg)
{
    PGRAPHState *pg = arg;
    static bool pruned;
    if (!pruned) {
        /* Here, off the render thread: it can delete thousands of files */
        pruned = true;
        shader_cache_prune_others(false);
    }
    int list_count;
    bool list_converted;
    ShaderState *list = shader_list_load(NV2A_SHADER_LIST_FILE, &list_count,
                                         &list_converted);
    GArray *all = g_array_new(FALSE, FALSE, sizeof(ShaderState));

    if (list) {
        g_array_append_vals(all, list, list_count);
    }
    g_free(list);
    if (g_config.perf.shader_precache_own) {
        /* The cache files' states join this PC's own list too */
        guint from = all->len;
        precache_add_own(all);
        qemu_mutex_lock(&learned.lock);
        for (guint i = from; i < all->len; i++) {
            learned_insert_locked(&g_array_index(all, ShaderState, i));
        }
        g_array_append_vals(all, learned.states->data, learned.states->len);
        qemu_mutex_unlock(&learned.lock);
    }

    /*
     * The resolution scale is part of every vertex shader's state. The list
     * is kept at scale 1 and built at every scale the player uses (see
     * precache_targets), the one the game runs at now first, so switching
     * the scale in a match finds its shaders ready too.
     */
    ShaderState *states = g_new(ShaderState, MAX(all->len, 1));
    uint64_t *hashes = g_new(uint64_t, MAX(all->len, 1));
    GHashTable *seen = g_hash_table_new(g_int64_hash, g_int64_equal);
    int count = 0;

    for (guint i = 0; i < all->len; i++) {
        states[count] = g_array_index(all, ShaderState, i);
        states[count].vsh.surface_scale_factor = 1;
        hashes[count] =
            fast_hash((uint8_t *)&states[count], sizeof(ShaderState));
        if (!g_hash_table_contains(seen, &hashes[count])) {
            g_hash_table_add(seen, &hashes[count]);
            count++;
        }
    }
    g_hash_table_destroy(seen);
    g_array_free(all, TRUE);
    g_free(hashes);

    unsigned int now = qatomic_read(&pg->surface_scale_factor);
    unsigned int targets = precache_targets(now);
    int skipped = 0, n_missing = 0, n_scales = 0;
    for (unsigned int s = 1; s <= PRECACHE_MAX_SCALE; s++) {
        if (targets & PRECACHE_BIT(s)) {
            n_scales++;
            n_missing += precache_missing(states, count, s, &skipped);
        }
    }

    int disk_count;
    uint64_t disk_bytes;
    shader_disk_usage(&disk_count, &disk_bytes);

    qemu_mutex_lock(&precache.lock);
    precache.status.list_total =
        list_count < 0 && count == 0 ? -1 : count * n_scales;
    precache.status.list_missing = n_missing;
    precache.status.scales = targets;
    precache.status.disk_count = disk_count;
    precache.status.disk_bytes = disk_bytes;
    precache.status.skipped_count = skipped;
    precache.status.scanned = true;
    bool build = precache.build && n_missing > 0;
    qemu_mutex_unlock(&precache.lock);

    if (build) {
        bool unsupported = false;
        int64_t start = qemu_clock_get_ms(QEMU_CLOCK_REALTIME);
        xemu_diag_event("shader precache started: %d to build", n_missing);
        glo_set_current(g_nv2a_context_precache);
        unsigned int finished = 0;   /* scales gone through to the end */
        while (!qatomic_read(&precache.stop) && !unsupported) {
            now = qatomic_read(&pg->surface_scale_factor);
            if (!(targets & PRECACHE_BIT(now))) {
                /* A scale this player has not used before */
                targets = precache_targets(now);
                int more = precache_missing(states, count, now, NULL);
                qemu_mutex_lock(&precache.lock);
                precache.status.list_total += count;
                precache.status.list_missing += more;
                precache.status.scales = targets;
                qemu_mutex_unlock(&precache.lock);
            }
            unsigned int scale = precache_next_scale(targets, finished, now);
            if (!scale) {
                break;
            }
            bool moved = false;
            for (int i = 0; i < count && !qatomic_read(&precache.stop); i++) {
                unsigned int game = qatomic_read(&pg->surface_scale_factor);
                if (game != scale && !(finished & PRECACHE_BIT(game))) {
                    /* The player switched: that scale comes first */
                    moved = true;
                    break;
                }
                ShaderState state = states[i];
                state.vsh.surface_scale_factor = scale;
                uint64_t hash = fast_hash((uint8_t *)&state, sizeof(state));
                if (g_hash_table_contains(precache_skip, &hash) ||
                    shader_on_disk(hash)) {
                    continue;
                }
                guard_set(hash);
                bool ok = precache_build(&state, hash, &unsupported);
                if (ok && scale == game) {
                    /* Use it in this session too */
                    shader_load_from_disk(pg, hash);
                }
                qemu_mutex_lock(&precache.lock);
                if (ok) {
                    precache.status.done++;
                } else {
                    precache.status.failed++;
                }
                qemu_mutex_unlock(&precache.lock);
                if (unsupported) {
                    break;
                }
            }
            if (!moved && !qatomic_read(&precache.stop)) {
                finished |= PRECACHE_BIT(scale);
            }
        }
        glFinish();
        glo_set_current(NULL);
        guard_clear();

        shader_disk_usage(&disk_count, &disk_bytes);
        qemu_mutex_lock(&precache.lock);
        precache.status.disk_count = disk_count;
        precache.status.disk_bytes = disk_bytes;
        precache.status.list_missing -= precache.status.done;
        if (unsupported) {
            snprintf(precache.status.error, sizeof(precache.status.error),
                     "This graphics driver cannot save compiled shaders.");
        }
        int done = precache.status.done, failed = precache.status.failed;
        qemu_mutex_unlock(&precache.lock);
        int ms = qemu_clock_get_ms(QEMU_CLOCK_REALTIME) - start;
        fprintf(stderr, "nv2a: precache built %d shaders (%d failed) in %d ms\n",
                done, failed, ms);
        xemu_diag_event("shader precache finished: %d built, %d failed, "
                        "%d ms%s", done, failed, ms,
                        unsupported ? ", driver cannot save shaders" : "");
    }

    qemu_mutex_lock(&precache.lock);
    precache.status.running = false;
    precache.status.building = false;
    qemu_mutex_unlock(&precache.lock);

    g_free(states);
    return NULL;
}

static bool precache_launch(bool build)
{
    qemu_mutex_lock(&precache.lock);
    if (!precache.pg || precache.status.running) {
        qemu_mutex_unlock(&precache.lock);
        return false;
    }
    if (precache.thread_started) {
        /* The previous run has finished; collect its thread */
        qemu_thread_join(&precache.thread);
    }
    precache.build = build;
    precache.status.running = true;
    precache.status.building = build;
    if (build) {
        precache.status.done = 0;
        precache.status.failed = 0;
        precache.status.error[0] = 0;
    }
    qatomic_set(&precache.stop, false);
    precache.thread_started = true;
    qemu_thread_create(&precache.thread, "nv2a.precache", precache_thread,
                       precache.pg, QEMU_THREAD_JOINABLE);
    qemu_mutex_unlock(&precache.lock);
    return true;
}

static void precache_attach(PGRAPHState *pg)
{
    static bool lock_ready;
    if (!lock_ready) {
        qemu_mutex_init(&precache.lock);
        lock_ready = true;
    }
    learned_init();
    guard_check();
    qemu_mutex_lock(&precache.lock);
    precache.pg = pg;
    qemu_mutex_unlock(&precache.lock);
}

static void precache_detach(void)
{
    qatomic_set(&precache.stop, true);
    if (precache.thread_started) {
        qemu_thread_join(&precache.thread);
        precache.thread_started = false;
    }
    qemu_mutex_lock(&precache.lock);
    precache.pg = NULL;
    precache.status.running = false;
    qemu_mutex_unlock(&precache.lock);
    learned_save();
}

void nv2a_shader_precache_scan(void)
{
    precache_launch(false);
}

bool nv2a_shader_precache_start(void)
{
    return precache_launch(true);
}

void nv2a_shader_precache_stop(void)
{
    qatomic_set(&precache.stop, true);
}

void nv2a_shader_precache_get_status(ShaderPrecacheStatus *status)
{
    memset(status, 0, sizeof(*status));
    if (!precache.pg) {
        return;
    }
    learned_tick();
    int learned_n = learned_count();
    qemu_mutex_lock(&precache.lock);
    *status = precache.status;
    status->learned_count = learned_n;
    status->supported = precache.pg != NULL;
    snprintf(status->gpu, sizeof(status->gpu), "%s (%s)",
             shader_gl_renderer ? shader_gl_renderer : "?",
             shader_gl_vendor ? shader_gl_vendor : "?");
    snprintf(status->driver, sizeof(status->driver), "%s",
             shader_gl_version ? shader_gl_version : "?");
    qemu_mutex_unlock(&precache.lock);
}

bool nv2a_shader_cache_clear(bool forget_learned)
{
    if (!precache.pg) {
        return false;
    }
    qemu_mutex_lock(&precache.lock);
    if (precache.status.running) {
        qemu_mutex_unlock(&precache.lock);
        return false;
    }

    if (!forget_learned) {
        /* The cache files' states move into the own list before they go */
        GArray *own = g_array_new(FALSE, FALSE, sizeof(ShaderState));
        precache_add_own(own);
        qemu_mutex_lock(&learned.lock);
        for (guint i = 0; i < own->len; i++) {
            learned_insert_locked(&g_array_index(own, ShaderState, i));
        }
        qemu_mutex_unlock(&learned.lock);
        g_array_free(own, TRUE);
    }

    /* This build's folder (its list included) and other builds' folders */
    shader_remove_cache_folder(shader_cache_dir());
    shader_create_cache_folder();
    shader_cache_prune_others(true);

    if (forget_learned) {
        /* A save still running would write the list back */
        qemu_mutex_lock(&learned.save_lock);
        qemu_mutex_lock(&learned.lock);
        g_array_set_size(learned.states, 0);
        g_hash_table_remove_all(learned.hashes);
        learned.dirty = false;
        qemu_mutex_unlock(&learned.lock);
        char *path = base_file(NV2A_SHADER_LEARNED_FILE);
        g_remove(path);
        g_free(path);
        /* Or the next start would merge the old builds' list back in */
        path = base_file(NV2A_SHADER_LEARNED_V1_FILE);
        g_remove(path);
        g_free(path);
        qemu_mutex_unlock(&learned.save_lock);

        path = base_file(NV2A_SHADER_SKIP_FILE);
        g_remove(path);
        g_free(path);
        g_hash_table_remove_all(precache_skip);
    } else {
        /* Kept: what the list learned this run is written now */
        learned_save();
    }

    precache.status.disk_count = 0;
    precache.status.disk_bytes = 0;
    precache.status.scanned = false;
    qemu_mutex_unlock(&precache.lock);
    return true;
}
