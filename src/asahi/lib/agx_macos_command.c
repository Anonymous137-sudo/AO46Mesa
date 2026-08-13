/*
 * Copyright 2026 Khronos_AppleICDs contributors
 * SPDX-License-Identifier: MIT
 */

#include "agx_macos_command.h"

#define AGX_MACOS_SELECTOR_COMMAND_PAIR_NO_INPUT 0x6
#define AGX_MACOS_SELECTOR_COMMAND_PAIR_CONFIGURED 0x0e
#define AGX_MACOS_COMMAND_CONFIGURATION_SIZE 0x4000

static bool
agx_macos_command_pair_is_valid(const struct agx_macos_command_pair *pair)
{
   return pair->value0 != 0 && pair->value1 != 0;
}

bool
agx_macos_command_infrastructure_is_current(
   const struct agx_macos_device_session *session,
   const struct agx_macos_command_infrastructure *infrastructure)
{
   if (!agx_macos_device_session_is_current(session) || !infrastructure ||
       !infrastructure->initialized ||
       infrastructure->api_generation != session->api_generation) {
      return false;
   }

   for (unsigned i = 0; i < AGX_MACOS_COMMAND_PAIR_COUNT; ++i) {
      if (!agx_macos_command_pair_is_valid(&infrastructure->pairs[i]))
         return false;
   }

   return true;
}

static kern_return_t
agx_macos_command_pair_call(io_connect_t connection, uint32_t selector,
                            const uint64_t *input, uint32_t input_count,
                            struct agx_macos_command_pair *pair)
{
   size_t pair_size = sizeof(*pair);
   kern_return_t result = IOConnectCallMethod(
      connection, selector, input, input_count, NULL, 0, NULL, NULL, pair,
      &pair_size);

   if (result != KERN_SUCCESS)
      return result;
   if (pair_size != sizeof(*pair) || !agx_macos_command_pair_is_valid(pair))
      return kIOReturnBadArgument;

   return KERN_SUCCESS;
}

kern_return_t
agx_macos_command_infrastructure_init(
   const struct agx_macos_device_session *session,
   struct agx_macos_command_infrastructure *infrastructure)
{
   static const uint64_t configured_inputs[][2] = {
      {AGX_MACOS_COMMAND_CONFIGURATION_SIZE, 0},
      {AGX_MACOS_COMMAND_CONFIGURATION_SIZE, 1},
   };
   struct agx_macos_command_infrastructure configured = {0};
   kern_return_t result;

   if (!agx_macos_device_session_is_current(session) || !infrastructure ||
       infrastructure->initialized) {
      return kIOReturnBadArgument;
   }

   result = agx_macos_command_pair_call(
      session->device.connection, AGX_MACOS_SELECTOR_COMMAND_PAIR_NO_INPUT,
      NULL, 0, &configured.pairs[0]);
   if (result != KERN_SUCCESS)
      return result;

   for (unsigned i = 0; i < sizeof(configured_inputs) / sizeof(configured_inputs[0]);
        ++i) {
      result = agx_macos_command_pair_call(
         session->device.connection, AGX_MACOS_SELECTOR_COMMAND_PAIR_CONFIGURED,
         configured_inputs[i], 2, &configured.pairs[i + 1]);
      if (result != KERN_SUCCESS)
         return result;
   }

   configured.api_generation = session->api_generation;
   configured.initialized = true;
   if (!agx_macos_command_infrastructure_is_current(session, &configured))
      return kIOReturnBadArgument;

   *infrastructure = configured;
   return KERN_SUCCESS;
}
