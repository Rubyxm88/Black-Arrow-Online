//
// xemu User Interface
//
// Copyright (C) 2020-2022 Matt Borgerson
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <http://www.gnu.org/licenses/>.
//
#include "ui/xemu-widescreen.h"
#include "gl-helpers.hh"
#include "common.hh"
#include "data/controller_mask.png.h"
#include "data/controller_mask_s.png.h"
#include "data/logo_sdf.png.h"
#include "data/xemu_64x64.png.h"
#include "data/xmu_mask.png.h"
#include "notifications.hh"
#include "stb_image.h"
#include <fpng.h>
#include <math.h>
#include <stdio.h>
#include <vector>

#include "ui/shader/xemu-logo-frag.h"

Fbo *controller_fbo, *xmu_fbo, *logo_fbo;
GLuint g_controller_duke_tex, g_controller_s_tex, g_logo_tex, g_icon_tex, g_xmu_tex;

enum class ShaderType {
    Blit,
    BlitGamma, // FIMXE: Move to nv2a_get_framebuffer_surface
    Mask,
    Logo,
};

typedef struct DecalShader_
{
    int flip;
    float scale;
    uint32_t time;
    GLuint prog, vao, vbo, ebo;
    GLint flipy_loc;
    GLint tex_loc;
    GLint scale_offset_loc;
    GLint tex_scale_offset_loc;
    GLint color_primary_loc;
    GLint color_secondary_loc;
    GLint color_fill_loc;
    GLint time_loc;
    GLint scale_loc;
    GLint palette_loc[256];
    GLint filter_mode_loc;
    GLint fxaa_loc;
    GLint fxaa_preserve_text_loc;
    GLint cas_enable_loc;
    GLint cas_sharpness_loc;
    GLint text_sharpen_loc;
    GLint surface_scale_loc;
    GLint viewport_size_loc;
    uint8_t last_raw_palette[256 * 3];
    bool palette_uploaded;
} DecalShader;

static DecalShader *g_decal_shader,
                   *g_logo_shader,
                   *g_framebuffer_shader;

GLint Fbo::vp[4];
GLint Fbo::original_fbo;
bool Fbo::blend;

DecalShader *NewDecalShader(enum ShaderType type);
void DeleteDecalShader(DecalShader *s);

static GLint GetCurrentFbo()
{
    GLint fbo;
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, (GLint*)&fbo);
    return fbo;
}

Fbo::Fbo(int width, int height)
{
    w = width;
    h = height;

    // Allocate the texture
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_BORDER);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_BORDER);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA,
                 GL_UNSIGNED_BYTE, NULL);

    GLint original = GetCurrentFbo();

    // Allocate the framebuffer object
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                           tex, 0);
    GLenum DrawBuffers[1] = { GL_COLOR_ATTACHMENT0 };
    glDrawBuffers(1, DrawBuffers);

    glBindFramebuffer(GL_FRAMEBUFFER, original);
}

Fbo::~Fbo()
{
    glDeleteTextures(1, &tex);
    glDeleteFramebuffers(1, &fbo);
}

void Fbo::Target()
{
    GLint vp[4];
    glGetIntegerv(GL_VIEWPORT, vp);

    original_fbo = GetCurrentFbo();
    blend = glIsEnabled(GL_BLEND);
    if (!blend) {
        glEnable(GL_BLEND);
    }
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glViewport(0, 0, w, h);
    glClearColor(0, 0, 0, 0);
    glClear(GL_COLOR_BUFFER_BIT);
}

void Fbo::Restore()
{
    if (!blend) {
        glDisable(GL_BLEND);
    }

    // Restore default framebuffer, viewport, blending function
    glBindFramebuffer(GL_FRAMEBUFFER, original_fbo);
    glViewport(vp[0], vp[1], vp[2], vp[3]);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
}

static GLuint InitTexture(unsigned char *data, int width, int height,
                          int channels)
{
    GLuint tex;
    glGenTextures(1, &tex);
    assert(tex != 0);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL,  0);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S,     GL_CLAMP_TO_BORDER);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T,     GL_CLAMP_TO_BORDER);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, data);
    return tex;
}

static GLuint LoadTextureFromMemory(const unsigned char *buf, unsigned int size, bool flip=true)
{
    // Flip vertically so textures are loaded according to GL convention.
    stbi_set_flip_vertically_on_load(flip);

    int width, height, channels = 0;
    unsigned char *data = stbi_load_from_memory(buf, size, &width, &height, &channels, 4);
    assert(data != NULL);

    GLuint tex = InitTexture(data, width, height, channels);
    stbi_image_free(data);

    return tex;
}

static GLuint TryShader(GLenum type, const char *src)
{
    char err_buf[512];
    GLuint shader = glCreateShader(type);
    if (!shader) {
        return 0;
    }

    glShaderSource(shader, 1, &src, NULL);
    glCompileShader(shader);

    GLint status = GL_FALSE;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &status);
    if (status != GL_TRUE) {
        glGetShaderInfoLog(shader, sizeof(err_buf), NULL, err_buf);
        fprintf(stderr,
                "Shader compilation warning (using fallback if available): %s\n",
                err_buf);
        glDeleteShader(shader);
        return 0;
    }

    return shader;
}

static GLuint Shader(GLenum type, const char *src)
{
    GLuint shader = TryShader(type, src);
    assert(shader != 0);
    return shader;
}

DecalShader *NewDecalShader(enum ShaderType type)
{
    // Allocate shader wrapper object
    DecalShader *s = new DecalShader;
    assert(s != NULL);
    s->flip = 0;
    s->scale = 1.4;
    s->time = 0;

    const char *vert_src = R"(
#version 150 core
uniform bool in_FlipY;
uniform vec4 in_ScaleOffset;
uniform vec4 in_TexScaleOffset;
in vec2 in_Position;
in vec2 in_Texcoord;
out vec2 Texcoord;
void main() {
    vec2 t = in_Texcoord;
    if (in_FlipY) t.y = 1-t.y;
    Texcoord = t*in_TexScaleOffset.xy + in_TexScaleOffset.zw;
    gl_Position = vec4(in_Position*in_ScaleOffset.xy+in_ScaleOffset.zw, 0.0, 1.0);
}
)";
    GLuint vert = Shader(GL_VERTEX_SHADER, vert_src);
    assert(vert != 0);

    //     const char *image_frag_src = R"(
    // #version 150 core
    // uniform sampler2D tex;
    // in  vec2 Texcoord;
    // out vec4 out_Color;
    // void main() {
    //     out_Color.rgba = texture(tex, Texcoord);
    // }
    // )";

    const char *image_gamma_frag_src = R"(
#version 400 core
uniform sampler2D tex;
uniform uint palette[256];
uniform int in_FilterMode;
uniform bool in_Fxaa;
uniform bool in_FxaaPreserveText;
uniform bool in_CasEnable;
uniform float in_CasSharpness;
uniform bool in_TextSharpen;
uniform float in_SurfaceScale;
uniform vec2 in_ViewportSize;

float gamma_ch(int ch, float col)
{
    uint idx = clamp(uint(col * 255.0 + 0.5), 0u, 255u);
    return float(bitfieldExtract(palette[idx], ch*8, 8)) / 255.0;
}

vec4 gamma(vec4 col)
{
    return vec4(gamma_ch(0, col.r), gamma_ch(1, col.g), gamma_ch(2, col.b), col.a);
}

float luma(vec3 rgb)
{
    return dot(rgb, vec3(0.299, 0.587, 0.114));
}

vec4 sample_filtered_raw(vec2 uv, vec2 texSize, vec2 invTexSize)
{
    if (in_FilterMode == 2) {
        // Sharp Bilinear: pre-scales UV to nearest texel center with anti-aliased 1-screen-pixel transition
        vec2 texel = uv * texSize - 0.5;
        vec2 fl = floor(texel);
        vec2 fr = fract(texel);
        vec2 scale = max(in_ViewportSize * invTexSize, vec2(1.0));
        vec2 region = 0.5 - 0.5 / scale;
        vec2 dist = fr - 0.5;
        vec2 f = (dist - clamp(dist, -region, region)) * scale + 0.5;
        return texture(tex, (fl + f + 0.5) * invTexSize);
    } else if (in_FilterMode == 3) {
        // 4-tap Fast Catmull-Rom Bicubic
        vec2 pos = uv * texSize - 0.5;
        vec2 f = fract(pos);
        vec2 c = pos - f;
        vec2 f2 = f * f;
        vec2 f3 = f2 * f;
        vec2 w0 = -0.5 * f3 + f2 - 0.5 * f;
        vec2 w1 =  1.5 * f3 - 2.5 * f2 + 1.0;
        vec2 w2 = -1.5 * f3 + 2.0 * f2 + 0.5 * f;
        vec2 w3 =  0.5 * f3 - 0.5 * f2;
        vec2 w12 = w1 + w2;
        vec2 tc0 = (c - 0.5) * invTexSize;
        vec2 tc3 = (c + 2.5) * invTexSize;
        vec2 tc12 = (c + 0.5 + w2 / w12) * invTexSize;
        vec4 s =
            texture(tex, vec2(tc12.x, tc0.y))  * (w12.x * w0.y) +
            texture(tex, vec2(tc0.x,  tc12.y)) * (w0.x  * w12.y) +
            texture(tex, vec2(tc12.x, tc12.y)) * (w12.x * w12.y) +
            texture(tex, vec2(tc3.x,  tc12.y)) * (w3.x  * w12.y) +
            texture(tex, vec2(tc12.x, tc3.y))  * (w12.x * w3.y);
        float total_w = (w12.x * w0.y) + (w0.x * w12.y) + (w12.x * w12.y) + (w3.x * w12.y) + (w12.x * w3.y);
        return clamp(s / total_w, 0.0, 1.0);
    }
    return texture(tex, uv);
}

// Official 9-tap AMD FidelityFX Contrast Adaptive Sharpening (CAS)
// Includes full 9-tap soft min/max in [0, 2] so white text/highlights are protected by (2.0 - mxRGB)
vec3 compute_cas_9tap(
    vec3 a, vec3 b, vec3 c,
    vec3 d, vec3 e, vec3 f,
    vec3 g, vec3 h, vec3 i,
    float sharpness)
{
    vec3 mnRGB  = min(min(min(d, e), min(f, b)), h);
    vec3 mnRGB2 = min(mnRGB, min(min(a, c), min(g, i)));
    mnRGB += mnRGB2;

    vec3 mxRGB  = max(max(max(d, e), max(f, b)), h);
    vec3 mxRGB2 = max(mxRGB, max(max(a, c), max(g, i)));
    mxRGB += mxRGB2;

    vec3 ampRGB = clamp(min(mnRGB, 2.0 - mxRGB) / max(mxRGB, vec3(1e-5)), 0.0, 1.0);
    ampRGB = sqrt(ampRGB);

    float peak = -1.0 / mix(8.0, 5.0, clamp(sharpness, 0.0, 1.0));
    vec3 wRGB = ampRGB * peak;
    vec3 rcpWeightRGB = 1.0 / (1.0 + 4.0 * wRGB);

    vec3 window = b + d + f + h;
    return clamp((window * wRGB + e) * rcpWeightRGB, 0.0, 1.0);
}

// FXAA 3.11 edge-walk anti-aliasing with Text/Ridge & Corner protection.
// Returns vec4(antiAliasedRgb, blendWeight) where blendWeight in [0,1] indicates how much AA was applied.
vec4 apply_fxaa_311(
    vec2 uv, vec2 texSize, vec2 invTexSize,
    vec3 cNW, vec3 cN, vec3 cNE,
    vec3 cW,  vec3 cM, vec3 cE,
    vec3 cSW, vec3 cS, vec3 cSE)
{
    float lM  = luma(cM);
    float lN  = luma(cN);
    float lS  = luma(cS);
    float lW  = luma(cW);
    float lE  = luma(cE);

    // 5-tap cross range check (ignores diagonal-only corner touches so corners don't shoot streaks)
    float lMin = min(lM, min(min(lN, lS), min(lW, lE)));
    float lMax = max(lM, max(max(lN, lS), max(lW, lE)));
    float lRange = lMax - lMin;
    if (lRange < max(0.0625, lMax * 0.166)) {
        return vec4(cM, 0.0);
    }

    float lNW = luma(cNW);
    float lNE = luma(cNE);
    float lSW = luma(cSW);
    float lSE = luma(cSE);

    float edgeHorz =
        abs(-2.0 * lW + lNW + lSW) +
        abs(-2.0 * lM + lN  + lS ) * 2.0 +
        abs(-2.0 * lE + lNE + lSE);
    float edgeVert =
        abs(-2.0 * lN + lNW + lNE) +
        abs(-2.0 * lM + lW  + lE ) * 2.0 +
        abs(-2.0 * lS + lSW + lSE);

    if (in_FxaaPreserveText) {
        // 1) Thin bright-stroke protection: if M is a 1px bright peak between N&S or W&E,
        //    it's a bright font stroke or UI line on a darker background, not a 3D step edge.
        //    (Dark troughs are NOT excluded so dark grooves on corrugated roofs/grates are
        //    smoothly anti-aliased instead of flickering black when moving.)
        bool ridgeNS = (lM > lN && lM > lS) &&
                       (min(lM - lN, lM - lS) > 0.20 * lRange);
        bool ridgeWE = (lM > lW && lM > lE) &&
                       (min(lM - lW, lM - lE) > 0.20 * lRange);
        if (ridgeNS || ridgeWE) {
            return vec4(cM, 0.0);
        }

        // 2) Sharp 2D corner protection: when both horizontal and vertical curvature are
        //    simultaneously high (e.g. 90-degree corners/serifs of R, E, D, N, T, W),
        //    do not round off or streak the corner.
        if (min(edgeHorz, edgeVert) > 0.60 * max(edgeHorz, edgeVert)) {
            return vec4(cM, 0.0);
        }
    }

    bool isHorz = (edgeHorz >= edgeVert);
    float l1 = isHorz ? lN : lW;
    float l2 = isHorz ? lS : lE;
    float grad1 = abs(l1 - lM);
    float grad2 = abs(l2 - lM);
    bool is1Steepest = (grad1 >= grad2);
    float gradScaled = 0.25 * max(grad1, grad2);

    float stepLen = isHorz ? invTexSize.y : invTexSize.x;
    if (is1Steepest) {
        stepLen = -stepLen;
    }
    float lLocalAvg = 0.5 * ((is1Steepest ? l1 : l2) + lM);

    vec2 curUv = uv;
    if (isHorz) {
        curUv.y += stepLen * 0.5;
    } else {
        curUv.x += stepLen * 0.5;
    }

    vec2 offset = isHorz ? vec2(invTexSize.x, 0.0) : vec2(0.0, invTexSize.y);
    vec2 uv1 = curUv - offset;
    vec2 uv2 = curUv + offset;

    float lEnd1 = luma(sample_filtered_raw(uv1, texSize, invTexSize).rgb) - lLocalAvg;
    float lEnd2 = luma(sample_filtered_raw(uv2, texSize, invTexSize).rgb) - lLocalAvg;
    bool reached1 = (abs(lEnd1) >= gradScaled);
    bool reached2 = (abs(lEnd2) >= gradScaled);
    bool reachedBoth = reached1 && reached2;

    if (!reached1) uv1 -= offset;
    if (!reached2) uv2 += offset;

    if (!reachedBoth) {
        const float QUALITY[5] = float[5](1.0, 1.0, 1.5, 2.0, 2.0);
        for (int i = 0; i < 5; i++) {
            if (!reached1) {
                lEnd1 = luma(sample_filtered_raw(uv1, texSize, invTexSize).rgb) - lLocalAvg;
            }
            if (!reached2) {
                lEnd2 = luma(sample_filtered_raw(uv2, texSize, invTexSize).rgb) - lLocalAvg;
            }
            reached1 = (abs(lEnd1) >= gradScaled);
            reached2 = (abs(lEnd2) >= gradScaled);
            reachedBoth = reached1 && reached2;
            if (!reached1) uv1 -= offset * QUALITY[i];
            if (!reached2) uv2 += offset * QUALITY[i];
            if (reachedBoth) break;
        }
    }

    float dist1 = isHorz ? (uv.x - uv1.x) : (uv.y - uv1.y);
    float dist2 = isHorz ? (uv2.x - uv.x) : (uv2.y - uv.y);
    bool isDir1 = (dist1 < dist2);
    float distFinal = min(dist1, dist2);
    float edgeLen = dist1 + dist2;
    float pixelOffset = -distFinal / max(edgeLen, 1e-5) + 0.5;

    bool isLumaMSmaller = (lM < lLocalAvg);
    bool correctVariation = (((isDir1 ? lEnd1 : lEnd2) < 0.0) != isLumaMSmaller);
    float finalOffset = correctVariation ? pixelOffset : 0.0;

    if (in_FxaaPreserveText) {
        // 3) True staircase polarity check: on a tilted 3D edge staircase, one end of the span
        //    steps into foreground (+) and the other end steps into background (-).
        //    On a finite horizontal/vertical font bar or cap, both ends terminate with the SAME
        //    polarity (both into background or both into stems). Reject non-staircase bars!
        if (reached1 && reached2 && ((lEnd1 < 0.0) == (lEnd2 < 0.0))) {
            finalOffset = 0.0;
        }
    } else {
        // Standard subpixel AA when text preservation is turned off
        float lAvg = (1.0 / 12.0) * (2.0 * (lN + lS + lW + lE) + lNW + lNE + lSW + lSE);
        float subpix = clamp(abs(lAvg - lM) / max(lRange, 1e-5), 0.0, 1.0);
        float subpixB = (-2.0 * subpix + 3.0) * subpix * subpix;
        float subpixC = subpixB * subpixB * 0.5;
        finalOffset = max(finalOffset, subpixC);
    }

    if (finalOffset <= 0.001) {
        return vec4(cM, 0.0);
    }

    vec2 finalUv = uv;
    if (isHorz) {
        finalUv.y += finalOffset * stepLen;
    } else {
        finalUv.x += finalOffset * stepLen;
    }

    vec3 aaRgb = sample_filtered_raw(finalUv, texSize, invTexSize).rgb;
    float blendWeight = clamp(finalOffset * 2.0, 0.0, 1.0);
    return vec4(aaRgb, blendWeight);
}

in  vec2 Texcoord;
out vec4 out_Color;
void main() {
    vec2 texSize = vec2(textureSize(tex, 0));
    vec2 invTexSize = 1.0 / max(texSize, vec2(1.0));

    vec4 colM = sample_filtered_raw(Texcoord, texSize, invTexSize);
    vec3 cM = colM.rgb;

    if (!in_Fxaa && (!in_CasEnable || in_CasSharpness <= 0.0) && !in_TextSharpen) {
        out_Color = gamma(colM);
        return;
    }

    // Shared 9-tap neighborhood at render-target pixel stride
    vec3 cNW = texture(tex, Texcoord + vec2(-1.0, -1.0) * invTexSize).rgb;
    vec3 cN  = texture(tex, Texcoord + vec2( 0.0, -1.0) * invTexSize).rgb;
    vec3 cNE = texture(tex, Texcoord + vec2( 1.0, -1.0) * invTexSize).rgb;
    vec3 cW  = texture(tex, Texcoord + vec2(-1.0,  0.0) * invTexSize).rgb;
    vec3 cE  = texture(tex, Texcoord + vec2( 1.0,  0.0) * invTexSize).rgb;
    vec3 cSW = texture(tex, Texcoord + vec2(-1.0,  1.0) * invTexSize).rgb;
    vec3 cS  = texture(tex, Texcoord + vec2( 0.0,  1.0) * invTexSize).rgb;
    vec3 cSE = texture(tex, Texcoord + vec2( 1.0,  1.0) * invTexSize).rgb;

    vec4 fxaaRes = vec4(cM, 0.0);
    if (in_Fxaa) {
        fxaaRes = apply_fxaa_311(
            Texcoord, texSize, invTexSize,
            cNW, cN, cNE,
            cW,  cM, cE,
            cSW, cS, cSE);
    }

    // Compute CAS sharpening delta strictly from the consistent pre-FXAA neighborhood
    // so center and neighbors never mismatch or invert font strokes.
    vec3 casDelta = vec3(0.0);
    if (in_CasEnable && in_CasSharpness > 0.0) {
        vec3 casFine = compute_cas_9tap(
            cNW, cN, cNE,
            cW,  cM, cE,
            cSW, cS, cSE,
            in_CasSharpness);
        casDelta += (casFine - cM);
    }

    if (in_TextSharpen) {
        // Xbox 2D menu fonts & HUD textures are 640x480 bitmaps magnified with GL_LINEAR.
        // At >1x surface scale, their bilinear ramp spans in_SurfaceScale texels; at 1x scale on
        // a high-res monitor, a 0.65-texel stride tightens the bilinear ramp across screen pixels.
        float uiStride = (in_SurfaceScale > 1.0) ? (in_SurfaceScale * 0.75) : 0.65;
        vec2 uiStep = invTexSize * uiStride;
        vec3 uNW = texture(tex, Texcoord + vec2(-1.0, -1.0) * uiStep).rgb;
        vec3 uN  = texture(tex, Texcoord + vec2( 0.0, -1.0) * uiStep).rgb;
        vec3 uNE = texture(tex, Texcoord + vec2( 1.0, -1.0) * uiStep).rgb;
        vec3 uW  = texture(tex, Texcoord + vec2(-1.0,  0.0) * uiStep).rgb;
        vec3 uE  = texture(tex, Texcoord + vec2( 1.0,  0.0) * uiStep).rgb;
        vec3 uSW = texture(tex, Texcoord + vec2(-1.0,  1.0) * uiStep).rgb;
        vec3 uS  = texture(tex, Texcoord + vec2( 0.0,  1.0) * uiStep).rgb;
        vec3 uSE = texture(tex, Texcoord + vec2( 1.0,  1.0) * uiStep).rgb;

        float uiSharp = in_CasEnable ? max(in_CasSharpness, 0.65) : 0.65;
        vec3 casUi = compute_cas_9tap(
            uNW, uN, uNE,
            uW,  cM, uE,
            uSW, uS, uSE,
            uiSharp);
        // Cap negative darkening from UI sharpener so dark grooves on 3D roofs aren't crushed
        vec3 uiDelta = (casUi - cM) * 0.85;
        casDelta += max(uiDelta, vec3(-0.015));
    }

    // Damping negative (darkening) overshoot prevents dark grooves on corrugated roofs
    // from strobing black when moving while keeping bright text and highlights crisp.
    casDelta = mix(casDelta, max(casDelta, vec3(0.0)), 0.55);

    // Avoid over-sharpening monochrome green Night Vision static grain while
    // keeping 100% of native texture detail (zero blur).
    float rbMax = max(cM.r, cM.b);
    float nvgGreen = clamp((cM.g - rbMax * 2.0 - 0.04) * 8.0, 0.0, 1.0);
    casDelta *= (1.0 - nvgGreen * 0.75);

    // Apply sharpening where FXAA did not smooth a 3D staircase edge
    vec3 finalRgb = clamp(fxaaRes.rgb + casDelta * (1.0 - fxaaRes.a), 0.0, 1.0);
    out_Color = gamma(vec4(finalRgb, colM.a));
}
)";

    // Simple 2-color decal shader
    // - in_ColorFill is first pass
    // - Red channel of the texture is used as primary color, mixed with 1-Red for
    //   secondary color.
    // - Blue is a lazy alpha removal for now
    // - Alpha channel passed through
    const char *mask_frag_src = R"(
#version 150 core
uniform sampler2D tex;
uniform vec4 in_ColorPrimary;
uniform vec4 in_ColorSecondary;
uniform vec4 in_ColorFill;
in  vec2 Texcoord;
out vec4 out_Color;
void main() {
    vec4 t = texture(tex, Texcoord);
    out_Color.rgba = in_ColorFill.rgba;
    out_Color.rgb += mix(in_ColorSecondary.rgb, in_ColorPrimary.rgb, t.r);
    out_Color.a += t.a - t.b;
}
)";

    const char *image_gamma_fallback_frag_src = R"(
#version 400 core
uniform sampler2D tex;
uniform uint palette[256];
float gamma_ch(int ch, float col)
{
    uint idx = clamp(uint(col * 255.0 + 0.5), 0u, 255u);
    return float(bitfieldExtract(palette[idx], ch*8, 8)) / 255.0;
}
vec4 gamma(vec4 col)
{
    return vec4(gamma_ch(0, col.r), gamma_ch(1, col.g), gamma_ch(2, col.b), col.a);
}
in  vec2 Texcoord;
out vec4 out_Color;
void main() {
    out_Color.rgba = gamma(texture(tex, Texcoord));
}
)";

    const char *frag_src = NULL;
    switch (type) {
    case ShaderType::Mask: frag_src = mask_frag_src; break;
    // case ShaderType::Blit: frag_src = image_frag_src; break;
    case ShaderType::BlitGamma: frag_src = image_gamma_frag_src; break;
    case ShaderType::Logo: frag_src = xemu_logo_frag_src; break;
    default: assert(0);
    }
    GLuint frag = (type == ShaderType::BlitGamma)
                      ? TryShader(GL_FRAGMENT_SHADER, frag_src)
                      : Shader(GL_FRAGMENT_SHADER, frag_src);
    if (frag == 0 && type == ShaderType::BlitGamma) {
        frag = Shader(GL_FRAGMENT_SHADER, image_gamma_fallback_frag_src);
    }
    assert(frag != 0);

    // Link vertex and fragment shaders
    s->prog = glCreateProgram();
    glAttachShader(s->prog, vert);
    glAttachShader(s->prog, frag);
    glBindFragDataLocation(s->prog, 0, "out_Color");
    glLinkProgram(s->prog);

    GLint linked = GL_FALSE;
    glGetProgramiv(s->prog, GL_LINK_STATUS, &linked);
    if (linked != GL_TRUE && type == ShaderType::BlitGamma) {
        glDetachShader(s->prog, frag);
        glDeleteShader(frag);
        frag = Shader(GL_FRAGMENT_SHADER, image_gamma_fallback_frag_src);
        glAttachShader(s->prog, frag);
        glLinkProgram(s->prog);
    }
    glUseProgram(s->prog);

    // Flag shaders for deletion when program is deleted
    glDeleteShader(vert);
    glDeleteShader(frag);

    s->flipy_loc = glGetUniformLocation(s->prog, "in_FlipY");
    s->scale_offset_loc = glGetUniformLocation(s->prog, "in_ScaleOffset");
    s->tex_scale_offset_loc =
        glGetUniformLocation(s->prog, "in_TexScaleOffset");
    s->tex_loc = glGetUniformLocation(s->prog, "tex");
    s->color_primary_loc = glGetUniformLocation(s->prog, "in_ColorPrimary");
    s->color_secondary_loc = glGetUniformLocation(s->prog, "in_ColorSecondary");
    s->color_fill_loc = glGetUniformLocation(s->prog, "in_ColorFill");
    s->time_loc = glGetUniformLocation(s->prog, "iTime");
    s->scale_loc = glGetUniformLocation(s->prog, "scale");
    s->filter_mode_loc = glGetUniformLocation(s->prog, "in_FilterMode");
    s->fxaa_loc = glGetUniformLocation(s->prog, "in_Fxaa");
    s->fxaa_preserve_text_loc = glGetUniformLocation(s->prog, "in_FxaaPreserveText");
    s->cas_enable_loc = glGetUniformLocation(s->prog, "in_CasEnable");
    s->cas_sharpness_loc = glGetUniformLocation(s->prog, "in_CasSharpness");
    s->text_sharpen_loc = glGetUniformLocation(s->prog, "in_TextSharpen");
    s->surface_scale_loc = glGetUniformLocation(s->prog, "in_SurfaceScale");
    s->viewport_size_loc = glGetUniformLocation(s->prog, "in_ViewportSize");
    s->palette_uploaded = false;
    memset(s->last_raw_palette, 0, sizeof(s->last_raw_palette));
    for (int i = 0; i < 256; i++) {
        char name[64];
        snprintf(name, sizeof(name), "palette[%d]", i);
        s->palette_loc[i] = glGetUniformLocation(s->prog, name);
    }

    const GLfloat verts[6][4] = {
        //  x      y      s      t
        { -1.0f, -1.0f,  0.0f,  0.0f }, // BL
        { -1.0f,  1.0f,  0.0f,  1.0f }, // TL
        {  1.0f,  1.0f,  1.0f,  1.0f }, // TR
        {  1.0f, -1.0f,  1.0f,  0.0f }, // BR
    };
    const GLint indicies[] = { 0, 1, 2, 3 };

    glGenVertexArrays(1, &s->vao);
    glBindVertexArray(s->vao);

    glGenBuffers(1, &s->vbo);
    glBindBuffer(GL_ARRAY_BUFFER, s->vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(verts), verts,  GL_STATIC_COPY);

    glGenBuffers(1, &s->ebo);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, s->ebo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(indicies), indicies, GL_STATIC_DRAW);

    GLint loc = glGetAttribLocation(s->prog, "in_Position");
    if (loc >= 0) {
        glVertexAttribPointer(loc, 2, GL_FLOAT, GL_FALSE, 4*sizeof(GLfloat), (void*)0);
        glEnableVertexAttribArray(loc);
    }

    loc = glGetAttribLocation(s->prog, "in_Texcoord");
    if (loc >= 0) {
        glVertexAttribPointer(loc, 2, GL_FLOAT, GL_FALSE, 4*sizeof(GLfloat), (void*)(2*sizeof(GLfloat)));
        glEnableVertexAttribArray(loc);
    }

    return s;
}

static void RenderDecal(DecalShader *s, float x, float y, float w, float h,
                        float tex_x, float tex_y, float tex_w, float tex_h,
                        uint32_t primary, uint32_t secondary, uint32_t fill)
{
    GLint vp[4];
    glGetIntegerv(GL_VIEWPORT, vp);
    float ww = vp[2], wh = vp[3];

    x = (int)x;
    y = (int)y;
    w = (int)w;
    h = (int)h;
    tex_x = (int)tex_x;
    tex_y = (int)tex_y;
    tex_w = (int)tex_w;
    tex_h = (int)tex_h;

    int tw_i, th_i;
    glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH,  &tw_i);
    glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &th_i);
    float tw = tw_i, th = th_i;

#define COL(color, c) (float)(((color) >> ((c)*8)) & 0xff) / 255.0
    if (s->flipy_loc >= 0) {
        glUniform1i(s->flipy_loc, s->flip);
    }
    if (s->scale_offset_loc >= 0) {
        glUniform4f(s->scale_offset_loc, w / ww, h / wh, -1 + ((2 * x + w) / ww),
                    -1 + ((2 * y + h) / wh));
    }
    if (s->tex_scale_offset_loc >= 0) {
        glUniform4f(s->tex_scale_offset_loc, tex_w / tw, tex_h / th, tex_x / tw,
                    tex_y / th);
    }
    if (s->tex_loc >= 0) {
        glUniform1i(s->tex_loc, 0);
    }
    if (s->color_primary_loc >= 0) {
        glUniform4f(s->color_primary_loc, COL(primary, 3), COL(primary, 2),
                    COL(primary, 1), COL(primary, 0));
    }
    if (s->color_secondary_loc >= 0) {
        glUniform4f(s->color_secondary_loc, COL(secondary, 3), COL(secondary, 2),
                    COL(secondary, 1), COL(secondary, 0));
    }
    if (s->color_fill_loc >= 0) {
        glUniform4f(s->color_fill_loc, COL(fill, 3), COL(fill, 2), COL(fill, 1),
                    COL(fill, 0));
    }
    if (s->time_loc >= 0) {
        glUniform1f(s->time_loc, s->time/1000.0f);
    }
    if (s->scale_loc >= 0) {
        glUniform1f(s->scale_loc, s->scale);
    }
#undef COL
    glDrawElements(GL_TRIANGLE_FAN, 4, GL_UNSIGNED_INT, NULL);
}

struct rect {
    int x, y, w, h;
};

static const struct rect tex_items[] = {
    { 0, 148, 467, 364 }, // obj_controller
    { 0, 81, 67, 67 }, // obj_lstick
    { 0, 14, 67, 67 }, // obj_rstick
    { 67, 104, 68, 44 }, // obj_port_socket
    { 67, 76, 28, 28 }, // obj_port_lbl_1
    { 67, 48, 28, 28 }, // obj_port_lbl_2
    { 67, 20, 28, 28 }, // obj_port_lbl_3
    { 95, 76, 28, 28 }, // obj_port_lbl_4
    { 0, 0, 512, 512 } // obj_xmu
};

enum tex_item_names {
    obj_controller,
    obj_lstick,
    obj_rstick,
    obj_port_socket,
    obj_port_lbl_1,
    obj_port_lbl_2,
    obj_port_lbl_3,
    obj_port_lbl_4,
    obj_xmu
};

void InitCustomRendering(void)
{
    glActiveTexture(GL_TEXTURE0);
    g_controller_duke_tex =
        LoadTextureFromMemory(controller_mask_data, controller_mask_size);
    g_controller_s_tex =
        LoadTextureFromMemory(controller_mask_s_data, controller_mask_s_size);
    g_decal_shader = NewDecalShader(ShaderType::Mask);
    controller_fbo = new Fbo(512, 512);

    g_xmu_tex = LoadTextureFromMemory(xmu_mask_data, xmu_mask_size);
    xmu_fbo = new Fbo(512, 256);

    g_logo_tex = LoadTextureFromMemory(logo_sdf_data, logo_sdf_size);
    g_logo_shader = NewDecalShader(ShaderType::Logo);
    logo_fbo = new Fbo(512, 512);

    g_icon_tex = LoadTextureFromMemory(xemu_64x64_data, xemu_64x64_size, false);

    g_framebuffer_shader = NewDecalShader(ShaderType::BlitGamma);
}

static void RenderMeter(DecalShader *s, float x, float y, float width,
                        float height, float p, uint32_t color_bg,
                        uint32_t color_fg)
{
    RenderDecal(s, x, y, width, height, 0, 0, 1, 1, 0, 0, color_bg);
    RenderDecal(s, x, y, width * p, height, 0, 0, 1, 1, 0, 0, color_fg);
}

static void RenderDukeController(float frame_x, float frame_y, uint32_t primary_color,
                      uint32_t secondary_color, ControllerState *state)
{
    // Location within the controller texture of masked button locations,
    // relative to the origin of the controller
    const struct rect jewel      = { 177, 172, 113, 118 };
    const struct rect lstick_ctr = {  93, 246,   0,   0 };
    const struct rect rstick_ctr = { 342, 148,   0,   0 };
    const struct rect buttons[12] = {
        { 367, 187, 30, 38 }, // A
        { 368, 229, 30, 38 }, // B
        { 330, 204, 30, 38 }, // X
        { 331, 247, 30, 38 }, // Y
        {  82, 121, 31, 47 }, // D-Left
        { 104, 160, 44, 25 }, // D-Up
        { 141, 121, 31, 47 }, // D-Right
        { 104, 105, 44, 25 }, // D-Down
        { 187,  94, 34, 24 }, // Back
        { 246,  94, 36, 26 }, // Start
        { 348, 288, 30, 38 }, // White
        { 386, 268, 30, 38 }, // Black
    };

    uint8_t alpha = 0;
    uint32_t now = SDL_GetTicks();
    float t;

    glUseProgram(g_decal_shader->prog);
    glBindVertexArray(g_decal_shader->vao);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, g_controller_duke_tex);

    // Add a 5 pixel space around the controller so we can wiggle the controller
    // around to visualize rumble in action
    frame_x += 5;
    frame_y += 5;
    float original_frame_x = frame_x;
    float original_frame_y = frame_y;

    // Floating point versions that will get scaled
    float rumble_l = 0;
    float rumble_r = 0;

    glBlendEquation(GL_FUNC_ADD);
    glBlendFunc(GL_ONE, GL_ZERO);

    uint32_t jewel_color = secondary_color;

    // Check to see if the guide button is pressed
    const uint32_t animate_guide_button_duration = 2000;
    if (state->buttons & CONTROLLER_BUTTON_GUIDE) {
        state->animate_guide_button_end = now + animate_guide_button_duration;
    }

    if (now < state->animate_guide_button_end) {
        t = 1.0f - (float)(state->animate_guide_button_end-now)/(float)animate_guide_button_duration;
        float sin_wav = (1-sin(M_PI * t / 2.0f));

        // Animate guide button by highlighting logo jewel and fading out over time
        alpha = sin_wav * 255.0f;
        jewel_color = primary_color + alpha;

        // Add a little extra flare: wiggle the frame around while we rumble
        frame_x += ((float)(rand() % 5)-2.5) * (1-t);
        frame_y += ((float)(rand() % 5)-2.5) * (1-t);
        rumble_l = rumble_r = sin_wav;
    }

    // Render controller texture
    RenderDecal(g_decal_shader, frame_x + 0, frame_y + 0,
                tex_items[obj_controller].w, tex_items[obj_controller].h,
                tex_items[obj_controller].x, tex_items[obj_controller].y,
                tex_items[obj_controller].w, tex_items[obj_controller].h,
                primary_color, secondary_color, 0);

    glBlendFunc(GL_ONE_MINUS_DST_ALPHA, GL_ONE); // Blend with controller cutouts
    RenderDecal(g_decal_shader, frame_x + jewel.x, frame_y + jewel.y, jewel.w,
                jewel.h, 0, 0, 1, 1, 0, 0, jewel_color);

    // The controller has alpha cutouts where the buttons are. Draw a surface
    // behind the buttons if they are activated
    for (int i = 0; i < 12; i++) {
        if (state->buttons & (1 << i)) {
            RenderDecal(g_decal_shader, frame_x + buttons[i].x,
                        frame_y + buttons[i].y, buttons[i].w, buttons[i].h, 0,
                        0, 1, 1, 0, 0, primary_color + 0xff);
        }
    }

    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA); // Blend with controller

    // Render left thumbstick
    float w = tex_items[obj_lstick].w;
    float h = tex_items[obj_lstick].h;
    float c_x = frame_x+lstick_ctr.x;
    float c_y = frame_y+lstick_ctr.y;
    float lstick_x = (float)state->axis[CONTROLLER_AXIS_LSTICK_X]/32768.0;
    float lstick_y = (float)state->axis[CONTROLLER_AXIS_LSTICK_Y]/32768.0;
    RenderDecal(g_decal_shader, (int)(c_x - w / 2.0f + 10.0f * lstick_x),
                (int)(c_y - h / 2.0f + 10.0f * lstick_y), w, h,
                tex_items[obj_lstick].x, tex_items[obj_lstick].y, w, h,
                (state->buttons & CONTROLLER_BUTTON_LSTICK) ? secondary_color :
                                                              primary_color,
                (state->buttons & CONTROLLER_BUTTON_LSTICK) ? primary_color :
                                                              secondary_color,
                0);

    // Render right thumbstick
    w = tex_items[obj_rstick].w;
    h = tex_items[obj_rstick].h;
    c_x = frame_x+rstick_ctr.x;
    c_y = frame_y+rstick_ctr.y;
    float rstick_x = (float)state->axis[CONTROLLER_AXIS_RSTICK_X]/32768.0;
    float rstick_y = (float)state->axis[CONTROLLER_AXIS_RSTICK_Y]/32768.0;
    RenderDecal(g_decal_shader, (int)(c_x - w / 2.0f + 10.0f * rstick_x),
                (int)(c_y - h / 2.0f + 10.0f * rstick_y), w, h,
                tex_items[obj_rstick].x, tex_items[obj_rstick].y, w, h,
                (state->buttons & CONTROLLER_BUTTON_RSTICK) ? secondary_color :
                                                              primary_color,
                (state->buttons & CONTROLLER_BUTTON_RSTICK) ? primary_color :
                                                              secondary_color,
                0);

    glBlendFunc(GL_ONE, GL_ZERO); // Don't blend, just overwrite values in buffer

    // Render trigger bars
    float ltrig = state->axis[CONTROLLER_AXIS_LTRIG] / 32767.0;
    float rtrig = state->axis[CONTROLLER_AXIS_RTRIG] / 32767.0;
    const uint32_t animate_trigger_duration = 1000;
    if ((ltrig > 0) || (rtrig > 0)) {
        state->animate_trigger_end = now + animate_trigger_duration;
        rumble_l = fmax(rumble_l, ltrig);
        rumble_r = fmax(rumble_r, rtrig);
    }

    // Animate trigger alpha down after a period of inactivity
    alpha = 0x80;
    if (state->animate_trigger_end > now) {
        t = 1.0f - (float)(state->animate_trigger_end-now)/(float)animate_trigger_duration;
        float sin_wav = (1-sin(M_PI * t / 2.0f));
        alpha += fmin(sin_wav * 0x40, 0x80);
    }

    RenderMeter(g_decal_shader, original_frame_x + 10,
                original_frame_y + tex_items[obj_controller].h + 20, 150, 5,
                ltrig, primary_color + alpha, primary_color + 0xff);
    RenderMeter(g_decal_shader,
                original_frame_x + tex_items[obj_controller].w - 160,
                original_frame_y + tex_items[obj_controller].h + 20, 150, 5,
                rtrig, primary_color + alpha, primary_color + 0xff);

    // Apply rumble updates
    state->rumble_l = (int)(rumble_l * (float)0xffff);
    state->rumble_r = (int)(rumble_r * (float)0xffff);

    glBindVertexArray(0);
    glUseProgram(0);
}

static void RenderControllerS(float frame_x, float frame_y, uint32_t primary_color,
                        uint32_t secondary_color, ControllerState *state)
{
    // Location within the controller texture of masked button locations,
    // relative to the origin of the controller
    const struct rect jewel = { 194, 213, 84, 84 };
    const struct rect lstick_ctr = { 103, 254, 0, 0 };
    const struct rect rstick_ctr = { 295, 176, 0, 0 };
    const struct rect buttons[12] = {
        { 347, 200, 34, 34 }, // A
        { 381, 235, 34, 34 }, // B
        { 313, 235, 34, 34 }, // X
        { 347, 270, 34, 34 }, // Y
        { 123, 165, 31, 26 }, // D-Left
        { 150, 187, 26, 31 }, // D-Up
        { 173, 165, 31, 26 }, // D-Right
        { 150, 135, 26, 31 }, // D-Down
        { 45, 195, 20, 24 }, // Back
        { 70, 163, 26, 26 }, // Start
        { 352, 145, 30, 30 }, // White
        { 388, 172, 30, 30 }, // Black
    };

    uint8_t alpha = 0;
    uint32_t now = SDL_GetTicks();
    float t;

    glUseProgram(g_decal_shader->prog);
    glBindVertexArray(g_decal_shader->vao);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, g_controller_s_tex);

    // Add a 5 pixel space around the controller so we can wiggle the controller
    // around to visualize rumble in action
    frame_x += 5;
    frame_y += 5;
    float original_frame_x = frame_x;
    float original_frame_y = frame_y;

    // Floating point versions that will get scaled
    float rumble_l = 0;
    float rumble_r = 0;

    glBlendEquation(GL_FUNC_ADD);
    glBlendFunc(GL_ONE, GL_ZERO);

    uint32_t jewel_color = secondary_color;

    // Check to see if the guide button is pressed
    const uint32_t animate_guide_button_duration = 2000;
    if (state->buttons & CONTROLLER_BUTTON_GUIDE) {
        state->animate_guide_button_end =
            now + animate_guide_button_duration;
    }

    if (now < state->animate_guide_button_end) {
        t = 1.0f - (float)(state->animate_guide_button_end - now) /
                       (float)animate_guide_button_duration;
        float sin_wav = (1 - sin(M_PI * t / 2.0f));

        // Animate guide button by highlighting logo jewel and fading out over
        // time
        alpha = sin_wav * 255.0f;
        jewel_color = primary_color + alpha;

        // Add a little extra flare: wiggle the frame around while we rumble
        frame_x += ((float)(rand() % 5) - 2.5) * (1 - t);
        frame_y += ((float)(rand() % 5) - 2.5) * (1 - t);
        rumble_l = rumble_r = sin_wav;
    }

    // Render controller texture
    RenderDecal(g_decal_shader, frame_x + 0, frame_y + 0,
                tex_items[obj_controller].w, tex_items[obj_controller].h,
                tex_items[obj_controller].x, tex_items[obj_controller].y,
                tex_items[obj_controller].w, tex_items[obj_controller].h,
                primary_color, secondary_color, 0);

    glBlendFunc(GL_ONE_MINUS_DST_ALPHA,
                GL_ONE); // Blend with controller cutouts
    RenderDecal(g_decal_shader, frame_x + jewel.x, frame_y + jewel.y, jewel.w,
                jewel.h, 0, 0, 1, 1, 0, 0, jewel_color);

    // The controller has alpha cutouts where the buttons are. Draw a surface
    // behind the buttons if they are activated
    for (int i = 0; i < 12; i++) {
        if (state->buttons & (1 << i)) {
            RenderDecal(g_decal_shader, frame_x + buttons[i].x,
                        frame_y + buttons[i].y, buttons[i].w, buttons[i].h, 0,
                        0, 1, 1, 0, 0, primary_color + 0xff);
        }
    }

    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA); // Blend with controller

    // Render left thumbstick
    float w = tex_items[obj_lstick].w;
    float h = tex_items[obj_lstick].h;
    float c_x = frame_x + lstick_ctr.x;
    float c_y = frame_y + lstick_ctr.y;
    float lstick_x = (float)state->axis[CONTROLLER_AXIS_LSTICK_X] / 32768.0;
    float lstick_y = (float)state->axis[CONTROLLER_AXIS_LSTICK_Y] / 32768.0;
    RenderDecal(
        g_decal_shader, (int)(c_x - w / 2.0f + 10.0f * lstick_x),
        (int)(c_y - h / 2.0f + 10.0f * lstick_y), w, h, tex_items[obj_lstick].x,
        tex_items[obj_lstick].y, w, h,
        (state->buttons & CONTROLLER_BUTTON_LSTICK) ? secondary_color :
                                                         primary_color,
        (state->buttons & CONTROLLER_BUTTON_LSTICK) ? primary_color :
                                                         secondary_color,
        0);

    // Render right thumbstick
    w = tex_items[obj_rstick].w;
    h = tex_items[obj_rstick].h;
    c_x = frame_x + rstick_ctr.x;
    c_y = frame_y + rstick_ctr.y;
    float rstick_x = (float)state->axis[CONTROLLER_AXIS_RSTICK_X] / 32768.0;
    float rstick_y = (float)state->axis[CONTROLLER_AXIS_RSTICK_Y] / 32768.0;
    RenderDecal(
        g_decal_shader, (int)(c_x - w / 2.0f + 10.0f * rstick_x),
        (int)(c_y - h / 2.0f + 10.0f * rstick_y), w, h, tex_items[obj_rstick].x,
        tex_items[obj_rstick].y, w, h,
        (state->buttons & CONTROLLER_BUTTON_RSTICK) ? secondary_color :
                                                         primary_color,
        (state->buttons & CONTROLLER_BUTTON_RSTICK) ? primary_color :
                                                         secondary_color,
        0);

    glBlendFunc(GL_ONE,
                GL_ZERO); // Don't blend, just overwrite values in buffer

    // Render trigger bars
    float ltrig = state->axis[CONTROLLER_AXIS_LTRIG] / 32767.0;
    float rtrig = state->axis[CONTROLLER_AXIS_RTRIG] / 32767.0;
    const uint32_t animate_trigger_duration = 1000;
    if ((ltrig > 0) || (rtrig > 0)) {
        state->animate_trigger_end = now + animate_trigger_duration;
        rumble_l = fmax(rumble_l, ltrig);
        rumble_r = fmax(rumble_r, rtrig);
    }

    // Animate trigger alpha down after a period of inactivity
    alpha = 0x80;
    if (state->animate_trigger_end > now) {
        t = 1.0f - (float)(state->animate_trigger_end - now) /
                       (float)animate_trigger_duration;
        float sin_wav = (1 - sin(M_PI * t / 2.0f));
        alpha += fmin(sin_wav * 0x40, 0x80);
    }

    RenderMeter(g_decal_shader, original_frame_x + 10,
                original_frame_y + tex_items[obj_controller].h + 20, 150, 5,
                ltrig, primary_color + alpha, primary_color + 0xff);
    RenderMeter(g_decal_shader,
                original_frame_x + tex_items[obj_controller].w - 160,
                original_frame_y + tex_items[obj_controller].h + 20, 150, 5,
                rtrig, primary_color + alpha, primary_color + 0xff);

    // Apply rumble updates
    state->rumble_l = (int)(rumble_l * (float)0xffff);
    state->rumble_r = (int)(rumble_r * (float)0xffff);

    glBindVertexArray(0);
    glUseProgram(0);
}

void RenderController(float frame_x, float frame_y, uint32_t primary_color,
                      uint32_t secondary_color, ControllerState *state)
{
    if (strcmp(bound_drivers[state->bound], DRIVER_S) == 0)
        RenderControllerS(frame_x, frame_y, primary_color, secondary_color,
                          state);
    else if (strcmp(bound_drivers[state->bound], DRIVER_DUKE) == 0)
        RenderDukeController(frame_x, frame_y, primary_color, secondary_color,
                             state);
}

void RenderControllerPort(float frame_x, float frame_y, int i,
                          uint32_t port_color)
{
    glUseProgram(g_decal_shader->prog);
    glBindVertexArray(g_decal_shader->vao);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, g_controller_duke_tex);
    glBlendFunc(GL_ONE, GL_ZERO);

    // Render port socket
    RenderDecal(g_decal_shader, frame_x, frame_y, tex_items[obj_port_socket].w,
                tex_items[obj_port_socket].h, tex_items[obj_port_socket].x,
                tex_items[obj_port_socket].y, tex_items[obj_port_socket].w,
                tex_items[obj_port_socket].h, port_color, port_color, 0);

    frame_x += (tex_items[obj_port_socket].w-tex_items[obj_port_lbl_1].w)/2;
    frame_y += tex_items[obj_port_socket].h + 8;

    // Render port label
    RenderDecal(
        g_decal_shader, frame_x, frame_y, tex_items[obj_port_lbl_1 + i].w,
        tex_items[obj_port_lbl_1 + i].h, tex_items[obj_port_lbl_1 + i].x,
        tex_items[obj_port_lbl_1 + i].y, tex_items[obj_port_lbl_1 + i].w,
        tex_items[obj_port_lbl_1 + i].h, port_color, port_color, 0);

    glBindVertexArray(0);
    glUseProgram(0);
}

void RenderXmu(float frame_x, float frame_y, uint32_t primary_color,
               uint32_t secondary_color)
{
    glUseProgram(g_decal_shader->prog);
    glBindVertexArray(g_decal_shader->vao);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, g_xmu_tex);
    glBlendEquation(GL_FUNC_ADD);
    glBlendFunc(GL_ONE, GL_ZERO);

    // Render xmu
    RenderDecal(g_decal_shader, frame_x, frame_y, 256, 256,
                tex_items[obj_xmu].x, tex_items[obj_xmu].y,
                tex_items[obj_xmu].w, tex_items[obj_xmu].h, primary_color,
                secondary_color, 0);

    glBindVertexArray(0);
    glUseProgram(0);
}

void RenderLogo(uint32_t time)
{
    uint32_t color = 0x62ca13ff;

    g_logo_shader->time = time;
    glUseProgram(g_logo_shader->prog);
    glBindVertexArray(g_decal_shader->vao);
    glBlendFunc(GL_ONE, GL_ZERO);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, g_logo_tex);
    RenderDecal(g_logo_shader, 0, 0, 512, 512, 0, 0, 128, 128, color,
        color, 0x00000000);
    glBindVertexArray(0);
    glUseProgram(0);
}

// Scale <src> proportionally to fit in <max>
void ScaleDimensions(int src_width, int src_height, int max_width, int max_height, int *out_width, int *out_height)
{
    float w_ratio = (float)max_width/(float)max_height;
    float t_ratio = (float)src_width/(float)src_height;

    if (w_ratio >= t_ratio) {
        *out_width = (float)max_width * t_ratio/w_ratio;
        *out_height = max_height;
    } else {
        *out_width = max_width;
        *out_height = (float)max_height * w_ratio/t_ratio;
    }
}

void RenderFramebuffer(GLint tex, int width, int height, bool flip, float scale[2])
{
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, tex);
    
    switch (g_config.display.filtering) {
    case CONFIG_DISPLAY_FILTERING_NEAREST:
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        break;
    case CONFIG_DISPLAY_FILTERING_LINEAR:
    default:
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        break;
    }

    bool enhanced = g_config.display.quality.enhanced;
    int filter_mode = (g_config.display.filtering == CONFIG_DISPLAY_FILTERING_NEAREST)
                          ? 1
                          : (enhanced ? 3 : 0);

    DecalShader *s = g_framebuffer_shader;
    s->flip = flip;
    glViewport(0, 0, width, height);
    glUseProgram(s->prog);
    glBindVertexArray(s->vao);
    glUniform1i(s->flipy_loc, s->flip);
    glUniform4f(s->scale_offset_loc, scale[0], scale[1], 0, 0);
    glUniform4f(s->tex_scale_offset_loc, 1.0, 1.0, 0, 0);
    glUniform1i(s->tex_loc, 0);
    if (s->filter_mode_loc >= 0) {
        glUniform1i(s->filter_mode_loc, filter_mode);
    }
    if (s->fxaa_loc >= 0) {
        glUniform1i(s->fxaa_loc, enhanced ? 1 : 0);
    }
    if (s->fxaa_preserve_text_loc >= 0) {
        glUniform1i(s->fxaa_preserve_text_loc, 1);
    }
    if (s->cas_enable_loc >= 0) {
        glUniform1i(s->cas_enable_loc, enhanced ? 1 : 0);
    }
    if (s->cas_sharpness_loc >= 0) {
        glUniform1f(s->cas_sharpness_loc, 0.50f);
    }
    if (s->text_sharpen_loc >= 0) {
        glUniform1i(s->text_sharpen_loc, enhanced ? 1 : 0);
    }
    if (s->surface_scale_loc >= 0) {
        int surf_scale = nv2a_get_surface_scale_factor();
        if (surf_scale < 1) surf_scale = 1;
        glUniform1f(s->surface_scale_loc, (float)surf_scale);
    }
    if (s->viewport_size_loc >= 0) {
        glUniform2f(s->viewport_size_loc,
                    (float)width * fabsf(scale[0]),
                    (float)height * fabsf(scale[1]));
    }

    const uint8_t *palette = nv2a_get_dac_palette();
    if (!s->palette_uploaded ||
        memcmp(palette, s->last_raw_palette, sizeof(s->last_raw_palette)) != 0) {
        for (int i = 0; i < 256; i++) {
            if (s->palette_loc[i] >= 0) {
                uint32_t val = (palette[i * 3 + 2] << 16) |
                               (palette[i * 3 + 1] << 8) |
                               palette[i * 3];
                glUniform1ui(s->palette_loc[i], val);
            }
        }
        memcpy(s->last_raw_palette, palette, sizeof(s->last_raw_palette));
        s->palette_uploaded = true;
    }

    glClearColor(0, 0, 0, 0);
    glClear(GL_COLOR_BUFFER_BIT);

    if (!nv2a_get_screen_off()) {
        glDrawElements(GL_TRIANGLE_FAN, 4, GL_UNSIGNED_INT, NULL);
    }
}

static float GetDisplayAspectRatio(int width, int height)
{
    switch (g_config.display.ui.aspect_ratio) {
    case CONFIG_DISPLAY_UI_ASPECT_RATIO_NATIVE:
        return (float)width/(float)height;
    case CONFIG_DISPLAY_UI_ASPECT_RATIO_16X9:
        return 16.0f/9.0f;
    case CONFIG_DISPLAY_UI_ASPECT_RATIO_4X3:
        return 4.0f/3.0f;
    case CONFIG_DISPLAY_UI_ASPECT_RATIO_AUTO:
    default:
        return xemu_get_widescreen() ? 16.0f/9.0f : 4.0f/3.0f;
    }
}

void RenderFramebuffer(GLint tex, int width, int height, bool flip)
{
    int tw, th;
    float scale[2];

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, tex);
    glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &tw);
    glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &th);

    // Calculate scaling factors
    if (g_config.display.ui.fit == CONFIG_DISPLAY_UI_FIT_STRETCH) {
        // Stretch to fit
        scale[0] = 1.0;
        scale[1] = 1.0;
    } else if (g_config.display.ui.fit == CONFIG_DISPLAY_UI_FIT_CENTER) {
        // Centered
        float t_ratio = GetDisplayAspectRatio(tw, th);
        scale[0] = t_ratio*(float)th/(float)width;
        scale[1] = (float)th/(float)height;
    } else {
        float t_ratio = GetDisplayAspectRatio(tw, th);
        float w_ratio = (float)width/(float)height;
        if (w_ratio >= t_ratio) {
            scale[0] = t_ratio/w_ratio;
            scale[1] = 1.0;
        } else {
            scale[0] = 1.0;
            scale[1] = w_ratio/t_ratio;
        }
    }

    RenderFramebuffer(tex, width, height, flip, scale);
}

bool RenderFramebufferToPng(GLuint tex, bool flip, std::vector<uint8_t> &png, int max_width, int max_height)
{
    int width, height;

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, tex);
    glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &width);
    glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &height);

    width = height * GetDisplayAspectRatio(width, height);

    if (!max_width) max_width = width;
    if (!max_height) max_height = height;
    ScaleDimensions(width, height, max_width, max_height, &width, &height);

    std::vector<uint8_t> pixels;
    pixels.resize(width * height * 3);

    Fbo fbo(width, height);
    fbo.Target();
    bool blend = glIsEnabled(GL_BLEND);
    if (blend) glDisable(GL_BLEND);
    float scale[2] = {1.0, 1.0};
    RenderFramebuffer(tex, width, height, !flip, scale);
    if (blend) glEnable(GL_BLEND);
    glPixelStorei(GL_PACK_ROW_LENGTH, width);
    glPixelStorei(GL_PACK_IMAGE_HEIGHT, height);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, width, height, GL_RGB, GL_UNSIGNED_BYTE, pixels.data());
    fbo.Restore();

    return fpng::fpng_encode_image_to_memory(pixels.data(), width, height, 3, png);
}

void SaveScreenshot(GLuint tex, bool flip)
{
    Error *err = NULL;
    char fname[128];
    std::vector<uint8_t> png;

    if (RenderFramebufferToPng(tex, flip, png)) {
        time_t t = time(NULL);
        struct tm *tmp = localtime(&t);
        if (tmp) {
            strftime(fname, sizeof(fname), "xemu-%Y-%m-%d-%H-%M-%S.png", tmp);
        } else {
            strcpy(fname, "xemu.png");
        }

        const char *output_dir = g_config.general.screenshot_dir;
        if (!strlen(output_dir)) {
            output_dir = ".";
        }
        // FIXME: Check for existing path
        char *path = g_strdup_printf("%s/%s", output_dir, fname);
        FILE *fd = qemu_fopen(path, "wb");
        if (fd) {
            int s = fwrite(png.data(), png.size(), 1, fd);
            if (s != 1) {
                error_setg(&err, "Failed to write %s", path);
            }
            fclose(fd);
        } else {
            error_setg(&err, "Failed to open %s for writing", path);
        }
        g_free(path);
    } else {
        error_setg(&err, "Failed to encode PNG image");
    }

    if (err) {
        xemu_queue_error_message(error_get_pretty(err));
        error_report_err(err);
    } else {
        char *msg = g_strdup_printf("Screenshot Saved: %s", fname);
        xemu_queue_notification(msg);
        free(msg);
    }
}
