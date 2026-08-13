/*
 * Copyright 2026 Khronos_AppleICDs contributors
 * SPDX-License-Identifier: MIT
 */

#include "agx_macos_shader_provenance.h"

#include "agx_device.h"

static bool
agx_macos_shader_provenance_has_current_session(
   const struct agx_macos_shader_provenance *provenance,
   const struct agx_macos_device_session *session)
{
   return provenance && session && provenance->session == session &&
          provenance->api_generation != 0 &&
          provenance->api_generation == session->api_generation &&
          agx_macos_device_session_is_current(session);
}

bool
agx_macos_shader_provenance_begin(
   struct agx_macos_shader_provenance *provenance,
   const struct agx_macos_device_session *session, uint64_t compiler_reply_id)
{
   if (!provenance || provenance->state != AGX_MACOS_SHADER_PROVENANCE_EMPTY ||
       !agx_macos_device_session_is_current(session) || compiler_reply_id == 0) {
      return false;
   }

   *provenance = (struct agx_macos_shader_provenance){
      .session = session,
      .api_generation = session->api_generation,
      .compiler_reply_id = compiler_reply_id,
      .state = AGX_MACOS_SHADER_PROVENANCE_COMPILER_REPLY,
   };
   return true;
}

bool
agx_macos_shader_provenance_admit_code_resource(
   struct agx_macos_shader_provenance *provenance, uint64_t code_resource_id,
   uint64_t size)
{
   if (!provenance || provenance->state !=
                          AGX_MACOS_SHADER_PROVENANCE_COMPILER_REPLY ||
       !agx_macos_shader_provenance_has_current_session(provenance,
                                                        provenance->session) ||
       code_resource_id == 0 || size == 0) {
      return false;
   }

   provenance->code_resource_id = code_resource_id;
   provenance->resource_size = size;
   provenance->state = AGX_MACOS_SHADER_PROVENANCE_APPLE_CODE_RESOURCE;
   return true;
}

bool
agx_macos_shader_provenance_publish_relocated_code(
   struct agx_macos_shader_provenance *provenance)
{
   if (!provenance || provenance->state !=
                          AGX_MACOS_SHADER_PROVENANCE_APPLE_CODE_RESOURCE ||
       !agx_macos_shader_provenance_has_current_session(provenance,
                                                        provenance->session)) {
      return false;
   }

   provenance->state = AGX_MACOS_SHADER_PROVENANCE_APPLE_CODE_RELOCATED;
   return true;
}

bool
agx_macos_shader_provenance_bind_low_va_executable(
   struct agx_macos_shader_provenance *provenance, uint64_t gpu_va,
   uint64_t size)
{
   if (!provenance || provenance->state !=
                          AGX_MACOS_SHADER_PROVENANCE_APPLE_CODE_RELOCATED ||
       !agx_macos_shader_provenance_has_current_session(provenance,
                                                        provenance->session) ||
       gpu_va == 0 || size == 0 || size > provenance->resource_size ||
       gpu_va > UINT64_MAX - size ||
       (gpu_va & (AIL_PAGESIZE - 1)) != 0) {
      return false;
   }

   provenance->gpu_va = gpu_va;
   provenance->mapping_size = size;
   provenance->state = AGX_MACOS_SHADER_PROVENANCE_LOW_VA_EXECUTABLE;
   return true;
}

bool
agx_macos_shader_provenance_is_current(
   const struct agx_macos_shader_provenance *provenance,
   const struct agx_macos_device_session *session)
{
   return agx_macos_shader_provenance_has_current_session(provenance, session) &&
          provenance->state >= AGX_MACOS_SHADER_PROVENANCE_COMPILER_REPLY &&
          provenance->state <= AGX_MACOS_SHADER_PROVENANCE_LOW_VA_EXECUTABLE;
}

bool
agx_macos_shader_provenance_is_executable(
   const struct agx_macos_shader_provenance *provenance,
   const struct agx_macos_device_session *session, uint64_t minimum_size)
{
   return agx_macos_shader_provenance_is_current(provenance, session) &&
          provenance->state == AGX_MACOS_SHADER_PROVENANCE_LOW_VA_EXECUTABLE &&
          provenance->compiler_reply_id != 0 &&
          provenance->code_resource_id != 0 && provenance->gpu_va != 0 &&
          provenance->resource_size != 0 && minimum_size != 0 &&
          provenance->mapping_size >= minimum_size;
}

bool
agx_macos_shader_provenance_matches_range(
   const struct agx_macos_shader_provenance *provenance,
   const struct agx_macos_device_session *session, uint64_t gpu_va,
   uint64_t size)
{
   return agx_macos_shader_provenance_is_executable(provenance, session, size) &&
          gpu_va == provenance->gpu_va && size == provenance->mapping_size;
}

void
agx_macos_shader_provenance_retire(
   struct agx_macos_shader_provenance *provenance)
{
   if (!provenance)
      return;

   if (provenance->state != AGX_MACOS_SHADER_PROVENANCE_EMPTY)
      provenance->state = AGX_MACOS_SHADER_PROVENANCE_RETIRED;
}
