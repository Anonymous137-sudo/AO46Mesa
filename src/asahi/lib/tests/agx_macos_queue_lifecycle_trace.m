/*
 * Copyright 2026 Khronos_AppleICDs contributors
 * SPDX-License-Identifier: MIT
 */

#import <Metal/Metal.h>

#include <stdio.h>

int
main(void)
{
   id<MTLDevice> device;
   id<MTLCommandQueue> producer_queue;
   id<MTLCommandQueue> consumer_queue;

   @autoreleasepool {
      puts("AO46_AGX_QUEUE_LIFECYCLE_TRACE create-device");
      device = MTLCreateSystemDefaultDevice();
      if (!device) {
         fputs("AO46_AGX_QUEUE_LIFECYCLE_TRACE no Metal device\n", stderr);
         return 1;
      }

      puts("AO46_AGX_QUEUE_LIFECYCLE_TRACE create-producer-queue");
      producer_queue = [device newCommandQueue];
      puts("AO46_AGX_QUEUE_LIFECYCLE_TRACE create-consumer-queue");
      consumer_queue = [device newCommandQueue];
      if (!producer_queue || !consumer_queue) {
         fputs("AO46_AGX_QUEUE_LIFECYCLE_TRACE queue creation failed\n",
               stderr);
         [consumer_queue release];
         [producer_queue release];
         [device release];
         return 1;
      }

      /* No command buffer, resource allocation, or submission is performed. */
      puts("AO46_AGX_QUEUE_LIFECYCLE_TRACE release-consumer-queue");
      [consumer_queue release];
      puts("AO46_AGX_QUEUE_LIFECYCLE_TRACE release-producer-queue");
      [producer_queue release];
      puts("AO46_AGX_QUEUE_LIFECYCLE_TRACE release-device");
      [device release];
   }

   puts("AO46_AGX_QUEUE_LIFECYCLE_TRACE complete");
   return 0;
}
