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
   id<MTLCommandBuffer> command_buffer;

   @autoreleasepool {
      puts("AO46_AGX_COMMAND_BUFFER_TRACE create-device");
      device = MTLCreateSystemDefaultDevice();
      if (!device) {
         fputs("AO46_AGX_COMMAND_BUFFER_TRACE no Metal device\n", stderr);
         return 1;
      }

      puts("AO46_AGX_COMMAND_BUFFER_TRACE create-queue");
      queue = [device newCommandQueue];
      if (!queue) {
         fputs("AO46_AGX_COMMAND_BUFFER_TRACE queue creation failed\n", stderr);
         [device release];
         return 1;
      }

      /* This probes command-buffer setup only. It never encodes or commits. */
      @autoreleasepool {
         puts("AO46_AGX_COMMAND_BUFFER_TRACE create-command-buffer");
         command_buffer = [queue commandBuffer];
         if (!command_buffer) {
            fputs("AO46_AGX_COMMAND_BUFFER_TRACE command buffer creation failed\n",
                  stderr);
            [queue release];
            [device release];
            return 1;
         }

         /* commandBuffer is autoreleased; drain this pool before its queue. */
         puts("AO46_AGX_COMMAND_BUFFER_TRACE release-command-buffer");
      }

      puts("AO46_AGX_COMMAND_BUFFER_TRACE release-queue");
      [queue release];
      puts("AO46_AGX_COMMAND_BUFFER_TRACE release-device");
      [device release];
   }

   puts("AO46_AGX_COMMAND_BUFFER_TRACE complete");
   return 0;
}
