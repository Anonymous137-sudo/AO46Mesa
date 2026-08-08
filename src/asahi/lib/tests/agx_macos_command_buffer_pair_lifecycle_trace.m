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
   id<MTLCommandQueue> queue;
   id<MTLCommandBuffer> first;
   id<MTLCommandBuffer> second;

   @autoreleasepool {
      puts("AO46_AGX_COMMAND_BUFFER_PAIR_TRACE create-device");
      device = MTLCreateSystemDefaultDevice();
      if (!device) {
         fputs("AO46_AGX_COMMAND_BUFFER_PAIR_TRACE no Metal device\n", stderr);
         return 1;
      }

      puts("AO46_AGX_COMMAND_BUFFER_PAIR_TRACE create-queue");
      queue = [device newCommandQueue];
      if (!queue) {
         fputs("AO46_AGX_COMMAND_BUFFER_PAIR_TRACE queue creation failed\n",
               stderr);
         [device release];
         return 1;
      }

      /* Neither command buffer encodes or commits work. */
      @autoreleasepool {
         puts("AO46_AGX_COMMAND_BUFFER_PAIR_TRACE create-first");
         first = [queue commandBuffer];
         puts("AO46_AGX_COMMAND_BUFFER_PAIR_TRACE create-second");
         second = [queue commandBuffer];
         if (!first || !second) {
            fputs("AO46_AGX_COMMAND_BUFFER_PAIR_TRACE command buffer creation failed\n",
                  stderr);
            [queue release];
            [device release];
            return 1;
         }

         /* commandBuffer is autoreleased; drain both before the queue. */
         puts("AO46_AGX_COMMAND_BUFFER_PAIR_TRACE release-pair");
      }

      puts("AO46_AGX_COMMAND_BUFFER_PAIR_TRACE release-queue");
      [queue release];
      puts("AO46_AGX_COMMAND_BUFFER_PAIR_TRACE release-device");
      [device release];
   }

   puts("AO46_AGX_COMMAND_BUFFER_PAIR_TRACE complete");
   return 0;
}
