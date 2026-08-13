/*
 * Copyright 2026 Khronos_AppleICDs contributors
 * SPDX-License-Identifier: MIT
 */

#include "agx_macos_command.h"
#include "agx_macos_queue.h"

#include <stdio.h>
#include <stdlib.h>

int
main(int argc, char **argv)
{
   struct agx_macos_device_session session;
   struct agx_macos_notification_queue queue = {
      .connection = IO_OBJECT_NULL,
      .notification_port = MACH_PORT_NULL,
   };
   struct agx_macos_command_infrastructure infrastructure = {0};
   kern_return_t result;

   if (!getenv("AGX_MACOS_EXPERIMENTAL_COMMAND")) {
      puts("AGX_MACOS_COMMAND_SMOKE skipped; set AGX_MACOS_EXPERIMENTAL_COMMAND=1 to run");
      return 0;
   }

   if (argc != 1 || agx_macos_device_session_open(&session) !=
                        AGX_MACOS_DEVICE_SESSION_READY) {
      fputs("AGX_MACOS_COMMAND_SMOKE failed to open profiled session\n", stderr);
      return 1;
   }

   if (agx_macos_device_session_configure_traced_api(&session, argv[0]) !=
       KERN_SUCCESS) {
      fputs("AGX_MACOS_COMMAND_SMOKE failed to configure profiled session\n", stderr);
      agx_macos_device_session_close(&session);
      return 1;
   }

   result = agx_macos_notification_queue_create(&session, &queue);
   if (result != KERN_SUCCESS) {
      fprintf(stderr, "AGX_MACOS_COMMAND_SMOKE queue setup failed: %#x\n", result);
      agx_macos_device_session_close(&session);
      return 1;
   }

   result = agx_macos_command_infrastructure_init(&session, &infrastructure);
   if (result != KERN_SUCCESS || !infrastructure.initialized ||
       infrastructure.api_generation != session.api_generation ||
       !agx_macos_command_infrastructure_is_current(&session,
                                                     &infrastructure)) {
      fprintf(stderr, "AGX_MACOS_COMMAND_SMOKE initialization failed: %#x\n",
              result);
      (void)agx_macos_notification_queue_release_for_session_close(&queue);
      agx_macos_device_session_close(&session);
      return 1;
   }

   for (unsigned i = 0; i < AGX_MACOS_COMMAND_PAIR_COUNT; ++i) {
      if (!infrastructure.pairs[i].value0 || !infrastructure.pairs[i].value1) {
         fputs("AGX_MACOS_COMMAND_SMOKE received an invalid pair\n", stderr);
         (void)agx_macos_notification_queue_release_for_session_close(&queue);
         agx_macos_device_session_close(&session);
         return 1;
      }
   }

   {
      struct agx_macos_command_infrastructure stale = infrastructure;

      stale.api_generation++;
      if (agx_macos_command_infrastructure_is_current(&session, &stale)) {
         fputs("AGX_MACOS_COMMAND_SMOKE accepted stale infrastructure\n", stderr);
         (void)agx_macos_notification_queue_release_for_session_close(&queue);
         agx_macos_device_session_close(&session);
         return 1;
      }

      stale = infrastructure;
      stale.pairs[AGX_MACOS_COMMAND_PAIR_COUNT - 1].value1 = 0;
      if (agx_macos_command_infrastructure_is_current(&session, &stale)) {
         fputs("AGX_MACOS_COMMAND_SMOKE accepted corrupt infrastructure\n", stderr);
         (void)agx_macos_notification_queue_release_for_session_close(&queue);
         agx_macos_device_session_close(&session);
         return 1;
      }
   }

   if (agx_macos_command_infrastructure_init(&session, &infrastructure) !=
       kIOReturnBadArgument) {
      fputs("AGX_MACOS_COMMAND_SMOKE accepted duplicate initialization\n", stderr);
      (void)agx_macos_notification_queue_release_for_session_close(&queue);
      agx_macos_device_session_close(&session);
      return 1;
   }

   result = agx_macos_notification_queue_release_for_session_close(&queue);
   agx_macos_device_session_close(&session);
   if (result != KERN_SUCCESS) {
      fprintf(stderr, "AGX_MACOS_COMMAND_SMOKE session-close release failed: %#x\n",
              result);
      return 1;
   }

   printf("AGX_MACOS_COMMAND_SMOKE complete generation=%llu\n",
          (unsigned long long)infrastructure.api_generation);
   return 0;
}
