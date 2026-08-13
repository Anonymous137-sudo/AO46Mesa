/*
 * Copyright 2026 Khronos_AppleICDs contributors
 * SPDX-License-Identifier: MIT
 */

#include "agx_macos_uabi.h"

#include <stdio.h>

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
   struct agx_macos_uabi_contract contract = {0};

   if (!agx_macos_device_session_is_open(&session) ||
       !agx_macos_device_session_is_current(&session) ||
       agx_macos_device_session_is_lost(&session) ||
       agx_macos_uabi_contract_init(&session, &contract) != KERN_SUCCESS) {
      return fail("AGX_MACOS_DEVICE_SESSION_STATE_SMOKE rejected a live session");
   }

   if (!agx_macos_device_session_mark_lost(&session) ||
       agx_macos_device_session_is_open(&session) ||
       agx_macos_device_session_is_current(&session) ||
       !agx_macos_device_session_is_lost(&session) || session.api_configured ||
       session.api_generation != 10 ||
       agx_macos_uabi_contract_is_current(&contract, &session) ||
       agx_macos_uabi_contract_require(
          &contract, &session, AGX_MACOS_UABI_OPERATION_BO_ALLOCATE) !=
          kIOReturnNotReady) {
      return fail("AGX_MACOS_DEVICE_SESSION_STATE_SMOKE admitted work after loss");
   }

   if (agx_macos_device_session_mark_lost(&session)) {
      return fail("AGX_MACOS_DEVICE_SESSION_STATE_SMOKE accepted duplicate loss");
   }

   puts("AGX_MACOS_DEVICE_SESSION_STATE_SMOKE complete");
   return 0;
}
