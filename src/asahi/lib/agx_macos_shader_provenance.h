/*
 * Copyright 2026 Khronos_AppleICDs contributors
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "agx_macos_device.h"

/* A shader BO is executable only after Apple has admitted compiler-produced
 * code into its own resource lifecycle. AO46 tracks that contract explicitly
 * instead of treating an arbitrary mapped buffer as executable. */
enum agx_macos_shader_provenance_state {
   AGX_MACOS_SHADER_PROVENANCE_EMPTY = 0,
   AGX_MACOS_SHADER_PROVENANCE_COMPILER_REPLY,
   AGX_MACOS_SHADER_PROVENANCE_APPLE_CODE_RESOURCE,
   AGX_MACOS_SHADER_PROVENANCE_APPLE_CODE_RELOCATED,
   AGX_MACOS_SHADER_PROVENANCE_LOW_VA_EXECUTABLE,
   AGX_MACOS_SHADER_PROVENANCE_RETIRED,
};

struct agx_macos_shader_provenance {
   const struct agx_macos_device_session *session;
   uint64_t api_generation;
   uint64_t compiler_reply_id;
   uint64_t code_resource_id;
   uint64_t gpu_va;
   uint64_t resource_size;
   uint64_t mapping_size;
   enum agx_macos_shader_provenance_state state;
};

/* Records an opaque compiler-result identity observed for the active device
 * generation. The identity is not compiler bytes and is never an import ABI. */
bool agx_macos_shader_provenance_begin(
   struct agx_macos_shader_provenance *provenance,
   const struct agx_macos_device_session *session, uint64_t compiler_reply_id);

/* The future Apple-owned code-resource handoff calls this only after it has
 * created and retained the resource. code_resource_id is an opaque lifetime
 * identity, never a reconstructed private Apple object. */
bool agx_macos_shader_provenance_admit_code_resource(
   struct agx_macos_shader_provenance *provenance, uint64_t code_resource_id,
   uint64_t size);

/* Records the Apple-owned copy and relocation completion for the admitted
 * resource. It validates ordering only and does not receive or mutate code. */
bool agx_macos_shader_provenance_publish_relocated_code(
   struct agx_macos_shader_provenance *provenance);

/* Records the already-authorized low-VA mapping returned by the Apple-owned
 * lifecycle. This function performs validation only; it does not map memory. */
bool agx_macos_shader_provenance_bind_low_va_executable(
   struct agx_macos_shader_provenance *provenance, uint64_t gpu_va,
   uint64_t size);

bool agx_macos_shader_provenance_is_current(
   const struct agx_macos_shader_provenance *provenance,
   const struct agx_macos_device_session *session);
bool agx_macos_shader_provenance_is_executable(
   const struct agx_macos_shader_provenance *provenance,
   const struct agx_macos_device_session *session, uint64_t minimum_size);
/* Checks that a Mesa BO range is exactly rooted in the Apple-authorized
 * executable mapping. It does not create an import or a new mapping. */
bool agx_macos_shader_provenance_matches_range(
   const struct agx_macos_shader_provenance *provenance,
   const struct agx_macos_device_session *session, uint64_t gpu_va,
   uint64_t size);
void agx_macos_shader_provenance_retire(
   struct agx_macos_shader_provenance *provenance);
