/*
 * Copyright 2026 Khronos_AppleICDs contributors
 * SPDX-License-Identifier: MIT
 */

#include "agx_macos_uabi.h"

static bool
agx_macos_uabi_session_is_ready(const struct agx_macos_device_session *session)
{
   return agx_macos_device_session_is_current(session);
}

kern_return_t
agx_macos_uabi_contract_init(
   const struct agx_macos_device_session *session,
   struct agx_macos_uabi_contract *out_contract)
{
   if (!out_contract)
      return kIOReturnBadArgument;

   *out_contract = (struct agx_macos_uabi_contract){0};
   if (!session || session->device.connection == IO_OBJECT_NULL)
      return kIOReturnNotOpen;
   if (session->profile != AGX_MACOS_DEVICE_PROFILE_T6040_G16S_USC3)
      return kIOReturnUnsupported;
   if (!agx_macos_uabi_session_is_ready(session))
      return kIOReturnNotReady;

   *out_contract = (struct agx_macos_uabi_contract){
      .version_major = AGX_MACOS_UABI_VERSION_MAJOR,
      .version_minor = AGX_MACOS_UABI_VERSION_MINOR,
      .profile = session->profile,
      .api_generation = session->api_generation,
      .operations = AGX_MACOS_UABI_OPERATION_CURRENT,
      .unavailable_operations = AGX_MACOS_UABI_OPERATION_ALL &
                              ~AGX_MACOS_UABI_OPERATION_CURRENT,
      .initialized = true,
   };
   return KERN_SUCCESS;
}

bool
agx_macos_uabi_contract_is_current(
   const struct agx_macos_uabi_contract *contract,
   const struct agx_macos_device_session *session)
{
   return contract && contract->initialized &&
          contract->version_major == AGX_MACOS_UABI_VERSION_MAJOR &&
          contract->version_minor == AGX_MACOS_UABI_VERSION_MINOR &&
          contract->profile == AGX_MACOS_DEVICE_PROFILE_T6040_G16S_USC3 &&
          contract->operations == AGX_MACOS_UABI_OPERATION_CURRENT &&
          contract->unavailable_operations ==
             (AGX_MACOS_UABI_OPERATION_ALL &
              ~AGX_MACOS_UABI_OPERATION_CURRENT) &&
          agx_macos_uabi_session_is_ready(session) &&
          contract->profile == session->profile &&
          contract->api_generation == session->api_generation;
}

bool
agx_macos_uabi_contract_supports(
   const struct agx_macos_uabi_contract *contract, uint32_t operations)
{
   return contract && contract->initialized && operations != 0 &&
          (operations & ~AGX_MACOS_UABI_OPERATION_ALL) == 0 &&
          (contract->operations & operations) == operations;
}

kern_return_t
agx_macos_uabi_contract_require(
   const struct agx_macos_uabi_contract *contract,
   const struct agx_macos_device_session *session, uint32_t operations)
{
   if (operations == 0 || (operations & ~AGX_MACOS_UABI_OPERATION_ALL) != 0)
      return kIOReturnBadArgument;
   if (!agx_macos_uabi_contract_is_current(contract, session))
      return kIOReturnNotReady;
   if (!agx_macos_uabi_contract_supports(contract, operations))
      return kIOReturnUnsupported;

   return KERN_SUCCESS;
}
