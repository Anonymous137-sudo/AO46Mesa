/*
 * Copyright 2026 Khronos_AppleICDs contributors
 * SPDX-License-Identifier: MIT
 */

#include "agx_macos_queue.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>

int
main(int argc, char **argv)
{
   struct agx_macos_notification_queue queues[2] = {
      {
         .connection = IO_OBJECT_NULL,
         .notification_port = MACH_PORT_NULL,
      },
      {
         .connection = IO_OBJECT_NULL,
         .notification_port = MACH_PORT_NULL,
      },
   };
   struct agx_macos_device_session session;
   unsigned created_queues = 0;
   kern_return_t result;

   if (!getenv("AGX_MACOS_EXPERIMENTAL_QUEUE")) {
      puts("AGX_MACOS_QUEUE_SMOKE skipped; set AGX_MACOS_EXPERIMENTAL_QUEUE=1 to run");
      return 0;
   }

   if (agx_macos_device_session_open(&session) !=
       AGX_MACOS_DEVICE_SESSION_READY) {
      fputs("AGX_MACOS_QUEUE_SMOKE failed to open profiled session\n", stderr);
      return 1;
   }

   if (argc != 1 ||
       agx_macos_device_session_configure_traced_api(&session, argv[0]) !=
          KERN_SUCCESS) {
      fputs("AGX_MACOS_QUEUE_SMOKE failed to configure traced API\n", stderr);
      agx_macos_device_session_close(&session);
      return 1;
   }

   for (unsigned i = 0; i < sizeof(queues) / sizeof(queues[0]); ++i) {
      struct agx_macos_notification_queue_state state;
      struct agx_macos_completion_record completion = {0};
      struct agx_macos_completion_record completions[2] = {0};
      uint32_t completion_count = 0;

      if (i > 0 &&
          agx_macos_device_session_configure_traced_api(&session, argv[0]) !=
             KERN_SUCCESS) {
         fputs("AGX_MACOS_QUEUE_SMOKE failed to reconfigure traced API\n", stderr);
         goto fail;
      }

      result = agx_macos_notification_queue_create(&session, &queues[i]);
      if (result != KERN_SUCCESS) {
         fprintf(stderr, "AGX_MACOS_QUEUE_SMOKE create %u failed: %#x\n", i,
                 result);
         goto fail;
      }
      ++created_queues;

      printf("AGX_MACOS_QUEUE_SMOKE created id=%u data_queue=%p port=%u\n",
             queues[i].id, (void *)queues[i].data_queue,
             queues[i].notification_port);

      result = agx_macos_notification_queue_get_state(&queues[i], &state);
      if (result != KERN_SUCCESS || state.head != state.tail) {
         fprintf(stderr, "AGX_MACOS_QUEUE_SMOKE unexpected queue %u state: %#x\n",
                 i, result);
         goto fail;
      }

      printf("AGX_MACOS_QUEUE_SMOKE empty id=%u capacity=%u head=%u tail=%u\n",
             queues[i].id, state.capacity, state.head, state.tail);

      if (agx_macos_notification_queue_poll_completion(&queues[i],
                                                        &completion) !=
          kIOReturnUnderrun) {
         fprintf(stderr,
                 "AGX_MACOS_QUEUE_SMOKE typed empty poll for queue %u did not underrun\n",
                 i);
         goto fail;
      }

      if (agx_macos_notification_queue_peek_completion(&queues[i],
                                                        &completion) !=
             kIOReturnUnderrun ||
          agx_macos_notification_queue_drain_completions(
             &queues[i], completions,
             sizeof(completions) / sizeof(completions[0]),
             &completion_count) != KERN_SUCCESS ||
          completion_count != 0) {
         fprintf(stderr,
                 "AGX_MACOS_QUEUE_SMOKE empty completion drain for queue %u failed\n",
                 i);
         goto fail;
      }
   }

   if (queues[0].id == queues[1].id || queues[0].data_queue == queues[1].data_queue ||
       queues[0].notification_port == queues[1].notification_port) {
      fputs("AGX_MACOS_QUEUE_SMOKE queue identities were not distinct\n", stderr);
      goto fail;
   }

   for (unsigned i = 0; i < created_queues; ++i) {
      result = agx_macos_notification_queue_release_for_session_close(&queues[i]);
      if (result != KERN_SUCCESS) {
         fprintf(stderr,
                 "AGX_MACOS_QUEUE_SMOKE session-close release %u failed: %#x\n",
                 i, result);
         agx_macos_device_session_close(&session);
         return 1;
      }
   }
   agx_macos_device_session_close(&session);

   puts("AGX_MACOS_QUEUE_SMOKE complete");
   return 0;

fail:
   for (unsigned i = 0; i < created_queues; ++i)
      (void)agx_macos_notification_queue_release_for_session_close(&queues[i]);
   agx_macos_device_session_close(&session);
   return 1;
}
