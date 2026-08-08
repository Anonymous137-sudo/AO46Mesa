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

   @autoreleasepool {
      puts("AO46_AGX_EMPTY_SUBMISSION_TRACE create-device");
      device = MTLCreateSystemDefaultDevice();
      if (!device) {
         fputs("AO46_AGX_EMPTY_SUBMISSION_TRACE no Metal device\n", stderr);
         return 1;
      }

      puts("AO46_AGX_EMPTY_SUBMISSION_TRACE create-queue");
      queue = [device newCommandQueue];
      if (!queue) {
         fputs("AO46_AGX_EMPTY_SUBMISSION_TRACE queue creation failed\n",
               stderr);
         [device release];
         return 1;
      }

      @autoreleasepool {
         id<MTLCommandBuffer> command_buffer;

         puts("AO46_AGX_EMPTY_SUBMISSION_TRACE create-command-buffer");
         command_buffer = [queue commandBuffer];
         if (!command_buffer) {
            fputs("AO46_AGX_EMPTY_SUBMISSION_TRACE command buffer creation failed\n",
                  stderr);
            [queue release];
            [device release];
            return 1;
         }

         /* Deliberately submit no encoders or resources. */
         puts("AO46_AGX_EMPTY_SUBMISSION_TRACE commit-empty");
         [command_buffer commit];
         [command_buffer waitUntilCompleted];
         if (command_buffer.status != MTLCommandBufferStatusCompleted) {
            fprintf(stderr,
                    "AO46_AGX_EMPTY_SUBMISSION_TRACE status=%ld\n",
                    (long)command_buffer.status);
            [queue release];
            [device release];
            return 1;
         }

         /* commandBuffer is autoreleased; drain it before the queue. */
         puts("AO46_AGX_EMPTY_SUBMISSION_TRACE release-command-buffer");
      }

      puts("AO46_AGX_EMPTY_SUBMISSION_TRACE release-queue");
      [queue release];
      puts("AO46_AGX_EMPTY_SUBMISSION_TRACE release-device");
      [device release];
   }

   puts("AO46_AGX_EMPTY_SUBMISSION_TRACE complete");
   return 0;
}
