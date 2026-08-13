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
      .api_generation = 7,
   };
   struct agx_macos_uabi_contract contract = {0};
   const uint32_t supported = AGX_MACOS_UABI_OPERATION_DEVICE_SESSION |
                              AGX_MACOS_UABI_OPERATION_API_CONFIGURATION |
                              AGX_MACOS_UABI_OPERATION_BO_ALLOCATE |
                              AGX_MACOS_UABI_OPERATION_BO_CPU_MAP |
                              AGX_MACOS_UABI_OPERATION_FIXED_VA_BIND |
                              AGX_MACOS_UABI_OPERATION_COMMAND_INFRASTRUCTURE |
                              AGX_MACOS_UABI_OPERATION_NOTIFICATION_QUEUE |
                              AGX_MACOS_UABI_OPERATION_COMPLETION_POLL;

   if (agx_macos_uabi_contract_init(&session, &contract) != KERN_SUCCESS ||
       !agx_macos_uabi_contract_is_current(&contract, &session) ||
       contract.version_major != AGX_MACOS_UABI_VERSION_MAJOR ||
       contract.version_minor != AGX_MACOS_UABI_VERSION_MINOR ||
       !agx_macos_uabi_contract_supports(&contract, supported) ||
       agx_macos_uabi_contract_require(&contract, &session, supported) !=
          KERN_SUCCESS) {
      return fail("AGX_MACOS_UABI_CONTRACT_SMOKE rejected current operations");
   }

   if (agx_macos_uabi_contract_supports(
          &contract, AGX_MACOS_UABI_OPERATION_VM_BIND) ||
       agx_macos_uabi_contract_supports(
          &contract, AGX_MACOS_UABI_OPERATION_RESOURCE_BIND) ||
       agx_macos_uabi_contract_supports(
          &contract, AGX_MACOS_UABI_OPERATION_CARRIER_BUILD) ||
       agx_macos_uabi_contract_supports(
          &contract, AGX_MACOS_UABI_OPERATION_BATCH_SUBMIT) ||
       agx_macos_uabi_contract_supports(
          &contract, AGX_MACOS_UABI_OPERATION_LOW_VA_BIND) ||
       agx_macos_uabi_contract_supports(
          &contract, AGX_MACOS_UABI_OPERATION_EXECUTABLE_BO) ||
       agx_macos_uabi_contract_supports(
          &contract, AGX_MACOS_UABI_OPERATION_SHADER_CODE_ADMISSION) ||
       agx_macos_uabi_contract_require(
          &contract, &session, AGX_MACOS_UABI_OPERATION_SCREEN_REQUIRED) !=
          kIOReturnUnsupported) {
      return fail("AGX_MACOS_UABI_CONTRACT_SMOKE exposed unimplemented submit");
   }

   ++session.api_generation;
   if (agx_macos_uabi_contract_is_current(&contract, &session) ||
       agx_macos_uabi_contract_require(
          &contract, &session, AGX_MACOS_UABI_OPERATION_BO_ALLOCATE) !=
          kIOReturnNotReady) {
      return fail("AGX_MACOS_UABI_CONTRACT_SMOKE accepted a stale generation");
   }

   session.api_configured = false;
   if (agx_macos_uabi_contract_init(&session, &contract) != kIOReturnNotReady) {
      return fail("AGX_MACOS_UABI_CONTRACT_SMOKE accepted an unconfigured API");
   }

   session.api_configured = true;
   session.profile = AGX_MACOS_DEVICE_PROFILE_UNSUPPORTED;
   if (agx_macos_uabi_contract_init(&session, &contract) !=
       kIOReturnUnsupported) {
      return fail("AGX_MACOS_UABI_CONTRACT_SMOKE accepted a foreign profile");
   }

   puts("AGX_MACOS_UABI_CONTRACT_SMOKE complete");
   return 0;
}
