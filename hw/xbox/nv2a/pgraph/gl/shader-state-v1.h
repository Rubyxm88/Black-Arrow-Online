/*
 * The shader state layout of xemu 0.8.137 and earlier (Black Arrow Online
 * builds 9 to 20), as stored in shader-list.bin and shader-list-mine.bin.
 * Kept only so those lists can be converted to the current ShaderState.
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

#ifndef HW_XBOX_NV2A_PGRAPH_GL_SHADER_STATE_V1_H
#define HW_XBOX_NV2A_PGRAPH_GL_SHADER_STATE_V1_H

#include "hw/xbox/nv2a/pgraph/glsl/shaders.h"

/* FixedFunctionVshState before texgen_infinite_viewer */
typedef struct FixedFunctionVshStateV1 {
    bool normalization;
    bool texture_matrix_enable[4];
    enum VshTexgen texgen[4][4];
    enum VshFoggen foggen;
    enum VshSkinning skinning;
    bool lighting;
    enum VshLight light[NV2A_MAX_LIGHTS];
    enum MaterialColorSource emission_src;
    enum MaterialColorSource ambient_src;
    enum MaterialColorSource diffuse_src;
    enum MaterialColorSource specular_src;
    bool local_eye;
} FixedFunctionVshStateV1;

/* VshState with specular_power and specular_power_back */
typedef struct {
    unsigned int surface_scale_factor;

    uint16_t compressed_attrs;
    uint16_t uniform_attrs;
    uint16_t swizzle_attrs;

    bool fog_enable;
    enum VshFogMode fog_mode;

    bool specular_enable;
    bool separate_specular;
    bool ignore_specular_alpha;
    float specular_power;
    float specular_power_back;

    bool point_params_enable;
    float point_size;
    float point_params[8];

    bool smooth_shading;
    bool z_perspective;

    bool is_fixed_function;
    FixedFunctionVshStateV1 fixed_function;
    ProgrammableVshState programmable;
} VshStateV1;

typedef struct ShaderStateV1 {
    VshStateV1 vsh;
    GeomState geom;
    PshState psh;
} ShaderStateV1;

#endif
