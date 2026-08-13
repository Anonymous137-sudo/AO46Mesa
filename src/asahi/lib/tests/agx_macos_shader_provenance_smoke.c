/*
 * Copyright 2026 Khronos_AppleICDs contributors
 * SPDX-License-Identifier: MIT
 */

#include "agx_macos_shader_provenance.h"

#include <stdio.h>

#include "agx_device.h"

static int
fail(const char *message)
{
   fputs(message, stderr);
   fputc('\n', stderr);
   return 1;
}

int
main(void)
{
   struct agx_macos_device_session session = {
      .device = {.connection = 1},
      .profile = AGX_MACOS_DEVICE_PROFILE_T6040_G16S_USC3,
      .state = AGX_MACOS_DEVICE_SESSION_STATE_CONFIGURED,
      .api_configured = true,
      .api_generation = 9,
   };
   struct agx_macos_shader_provenance provenance = {0};
   const uint64_t code_va = UINT64_C(0x1000000000);

   if (agx_macos_shader_provenance_begin(&provenance, &session, 0) ||
       !agx_macos_shader_provenance_begin(&provenance, &session,
                                           UINT64_C(0xfeed))) {
      return fail("AGX_MACOS_SHADER_PROVENANCE_SMOKE reply admission mismatch");
   }

   if (agx_macos_shader_provenance_is_executable(&provenance, &session,
                                                  AIL_PAGESIZE) ||
       agx_macos_shader_provenance_admit_code_resource(&provenance, 0,
                                                        AIL_PAGESIZE) ||
       !agx_macos_shader_provenance_admit_code_resource(
          &provenance, UINT64_C(0xbeef), AIL_PAGESIZE * 2) ||
       agx_macos_shader_provenance_bind_low_va_executable(
          &provenance, code_va, AIL_PAGESIZE) ||
       !agx_macos_shader_provenance_publish_relocated_code(&provenance) ||
       agx_macos_shader_provenance_publish_relocated_code(&provenance)) {
      return fail("AGX_MACOS_SHADER_PROVENANCE_SMOKE code resource mismatch");
   }

   if (agx_macos_shader_provenance_bind_low_va_executable(
          &provenance, code_va + 1, AIL_PAGESIZE) ||
       !agx_macos_shader_provenance_bind_low_va_executable(
          &provenance, code_va, AIL_PAGESIZE) ||
       !agx_macos_shader_provenance_is_executable(&provenance, &session,
                                                   AIL_PAGESIZE) ||
       agx_macos_shader_provenance_is_executable(&provenance, &session,
                                                  AIL_PAGESIZE * 2) ||
       !agx_macos_shader_provenance_matches_range(&provenance, &session,
                                                   code_va, AIL_PAGESIZE) ||
       agx_macos_shader_provenance_matches_range(&provenance, &session,
                                                  code_va + AIL_PAGESIZE,
                                                  AIL_PAGESIZE) ||
       agx_macos_shader_provenance_matches_range(&provenance, &session,
                                                  code_va, AIL_PAGESIZE * 2)) {
      return fail("AGX_MACOS_SHADER_PROVENANCE_SMOKE low-VA gate mismatch");
   }

   ++session.api_generation;
   if (agx_macos_shader_provenance_is_current(&provenance, &session) ||
       agx_macos_shader_provenance_is_executable(&provenance, &session,
                                                  AIL_PAGESIZE)) {
      return fail("AGX_MACOS_SHADER_PROVENANCE_SMOKE accepted stale generation");
   }

   agx_macos_shader_provenance_retire(&provenance);
   if (provenance.state != AGX_MACOS_SHADER_PROVENANCE_RETIRED ||
       agx_macos_shader_provenance_is_current(&provenance, &session)) {
      return fail("AGX_MACOS_SHADER_PROVENANCE_SMOKE retirement mismatch");
   }

   puts("AGX_MACOS_SHADER_PROVENANCE_SMOKE complete");
   return 0;
}
