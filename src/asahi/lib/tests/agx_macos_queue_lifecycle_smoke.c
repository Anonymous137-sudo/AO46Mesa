/*
 * Copyright 2026 Khronos_AppleICDs contributors
 * SPDX-License-Identifier: MIT
 */

#include "agx_macos_queue.h"

#include <stdio.h>
#include <stdlib.h>

static void
release_for_session_close(struct agx_macos_notification_queue *queue)
{
   if (queue->connection != IO_OBJECT_NULL &&
       queue->notification_port != MACH_PORT_NULL)
      (void)agx_macos_notification_queue_release_for_session_close(queue);
}

int
main(int argc, char **argv)
{
   struct agx_macos_notification_queue queues[2] = {
      {.connection = IO_OBJECT_NULL, .notification_port = MACH_PORT_NULL},
      {.connection = IO_OBJECT_NULL, .notification_port = MACH_PORT_NULL},
   };
   struct agx_macos_device_session session;
   kern_return_t result;

   if (!getenv("AGX_MACOS_EXPERIMENTAL_QUEUE_LIFECYCLE")) {
      puts("AGX_MACOS_QUEUE_LIFECYCLE_SMOKE skipped; set AGX_MACOS_EXPERIMENTAL_QUEUE_LIFECYCLE=1 to run");
      return 0;
   }

   if (argc != 1 || agx_macos_device_session_open(&session) !=
                        AGX_MACOS_DEVICE_SESSION_READY) {
      fputs("AGX_MACOS_QUEUE_LIFECYCLE_SMOKE failed to configure profiled session\n",
            stderr);
      return 1;
   }

   if (agx_macos_device_session_configure_traced_api(&session, argv[0]) !=
       KERN_SUCCESS) {
      fputs("AGX_MACOS_QUEUE_LIFECYCLE_SMOKE failed to configure profiled session\n",
            stderr);
      agx_macos_device_session_close(&session);
      return 1;
   }

   for (unsigned i = 0; i < 2; ++i) {
      if (i > 0 &&
         agx_macos_device_session_configure_traced_api(&session, argv[0]) !=
             KERN_SUCCESS) {
         fputs("AGX_MACOS_QUEUE_LIFECYCLE_SMOKE failed to reconfigure API\n",
               stderr);
         for (unsigned j = 0; j < i; ++j)
            release_for_session_close(&queues[j]);
         agx_macos_device_session_close(&session);
         return 1;
      }

      result = agx_macos_notification_queue_create(&session, &queues[i]);
      if (result != KERN_SUCCESS) {
         fprintf(stderr, "AGX_MACOS_QUEUE_LIFECYCLE_SMOKE create %u failed: %#x\n",
                 i, result);
         for (unsigned j = 0; j < i; ++j)
            release_for_session_close(&queues[j]);
         agx_macos_device_session_close(&session);
         return 1;
      }

      if (!agx_macos_notification_queue_is_current(&session, &queues[i]) ||
          (i > 0 &&
           agx_macos_notification_queue_is_current(&session, &queues[0]))) {
         fputs("AGX_MACOS_QUEUE_LIFECYCLE_SMOKE queue generation ownership failed\n",
               stderr);
         for (unsigned j = 0; j <= i; ++j)
            release_for_session_close(&queues[j]);
         agx_macos_device_session_close(&session);
         return 1;
      }
   }

   for (unsigned i = 2; i-- > 0;) {
      result = agx_macos_notification_queue_begin_destroy(&queues[i]);
      if (result != KERN_SUCCESS) {
         fprintf(stderr, "AGX_MACOS_QUEUE_LIFECYCLE_SMOKE begin %u failed: %#x\n",
                 i, result);
         goto fail;
      }

      result = agx_macos_notification_queue_release_port(&queues[i]);
      if (result != KERN_SUCCESS) {
         fprintf(stderr,
                 "AGX_MACOS_QUEUE_LIFECYCLE_SMOKE release port %u failed: %#x\n",
                 i, result);
         goto fail;
      }

      result = agx_macos_notification_queue_finish_destroy(&queues[i]);
      if (result != KERN_SUCCESS || queues[i].connection != IO_OBJECT_NULL ||
          queues[i].notification_port != MACH_PORT_NULL || queues[i].id != 0) {
         fprintf(stderr,
                 "AGX_MACOS_QUEUE_LIFECYCLE_SMOKE finish %u failed: %#x\n", i,
                 result);
         goto fail;
      }
   }

   agx_macos_device_session_close(&session);
   puts("AGX_MACOS_QUEUE_LIFECYCLE_SMOKE complete");
   return 0;

fail:
   for (unsigned i = 0; i < 2; ++i)
      release_for_session_close(&queues[i]);
   agx_macos_device_session_close(&session);
   return 1;
}
