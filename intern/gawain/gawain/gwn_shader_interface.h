
// Gawain shader interface (C --> GLSL)
//
// This code is part of the Gawain library, with modifications
// specific to integration with Blender.
//
// Copyright 2017 Mike Erwin
//
// This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0. If a copy of
// the MPL was not distributed with this file, You can obtain one at https://mozilla.org/MPL/2.0/.

#pragma once

#include "gwn_common.h"

typedef enum {
	GWN_UNIFORM_NONE = 0, // uninitialized/unknown

	GWN_UNIFORM_MODELVIEW,  // mat4 ModelViewMatrix
	GWN_UNIFORM_PROJECTION, // mat4 ProjectionMatrix
	GWN_UNIFORM_MVP,        // mat4 ModelViewProjectionMatrix

	GWN_UNIFORM_MODELVIEW_INV,  // mat4 ModelViewInverseMatrix
	GWN_UNIFORM_PROJECTION_INV, // mat4 ProjectionInverseMatrix

	GWN_UNIFORM_NORMAL,     // mat3 NormalMatrix

	GWN_UNIFORM_COLOR, // vec4 color

	GWN_UNIFORM_CUSTOM, // custom uniform, not one of the above built-ins

	GWN_NUM_UNIFORMS, // Special value, denotes number of builtin uniforms.
} Gwn_UniformBuiltin;

typedef struct Gwn_ShaderInput {
	const char* name;
	unsigned name_hash;
	GLenum gl_type;
	Gwn_UniformBuiltin builtin_type; // only for uniform inputs
	GLint size;
	GLint location;
} Gwn_ShaderInput;

typedef struct Gwn_ShaderInput_Entry {
	struct Gwn_ShaderInput_Entry* next;
	Gwn_ShaderInput* shader_input;
} Gwn_ShaderInput_Entry;

#define GWN_NUM_SHADERINTERFACE_BUCKETS 1009

typedef struct Gwn_ShaderInterface {
	uint16_t uniform_ct;
	uint16_t attrib_ct;
	Gwn_ShaderInput_Entry* uniform_buckets[GWN_NUM_SHADERINTERFACE_BUCKETS];
	Gwn_ShaderInput_Entry* attrib_buckets[GWN_NUM_SHADERINTERFACE_BUCKETS];
	Gwn_ShaderInput* builtin_uniforms[GWN_NUM_UNIFORMS];
	Gwn_ShaderInput inputs[0]; // dynamic size, uniforms followed by attribs
} Gwn_ShaderInterface;

Gwn_ShaderInterface* GWN_shaderinterface_create(GLint program_id);
void GWN_shaderinterface_discard(Gwn_ShaderInterface*);

const Gwn_ShaderInput* GWN_shaderinterface_uniform(const Gwn_ShaderInterface*, const char* name);
const Gwn_ShaderInput* GWN_shaderinterface_uniform_builtin(const Gwn_ShaderInterface*, Gwn_UniformBuiltin);
const Gwn_ShaderInput* GWN_shaderinterface_attr(const Gwn_ShaderInterface*, const char* name);
