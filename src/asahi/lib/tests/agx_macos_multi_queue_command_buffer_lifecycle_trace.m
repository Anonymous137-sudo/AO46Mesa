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
   id<MTLCommandQueue> first_queue;
   id<MTLCommandQueue> second_queue;

   @autoreleasepool {
      puts("AO46_AGX_MULTI_QUEUE_COMMAND_BUFFER_TRACE create-device");
      device = MTLCreateSystemDefaultDevice();
      if (!device) {
         fputs("AO46_AGX_MULTI_QUEUE_COMMAND_BUFFER_TRACE no Metal device\n",
               stderr);
         return 1;
      }

      puts("AO46_AGX_MULTI_QUEUE_COMMAND_BUFFER_TRACE create-first-queue");
      first_queue = [device newCommandQueue];
      puts("AO46_AGX_MULTI_QUEUE_COMMAND_BUFFER_TRACE create-second-queue");
      second_queue = [device newCommandQueue];
      if (!first_queue || !second_queue) {
         fputs("AO46_AGX_MULTI_QUEUE_COMMAND_BUFFER_TRACE queue creation failed\n",
               stderr);
         [second_queue release];
         [first_queue release];
         [device release];
         return 1;
      }

      @autoreleasepool {
         id<MTLCommandBuffer> command_buffer;

         puts("AO46_AGX_MULTI_QUEUE_COMMAND_BUFFER_TRACE create-first-buffer");
         command_buffer = [first_queue commandBuffer];
         if (!command_buffer) {
            fputs("AO46_AGX_MULTI_QUEUE_COMMAND_BUFFER_TRACE first buffer failed\n",
                  stderr);
            [second_queue release];
            [first_queue release];
            [device release];
            return 1;
         }

         /* commandBuffer is autoreleased; this drains it before queue teardown. */
         puts("AO46_AGX_MULTI_QUEUE_COMMAND_BUFFER_TRACE release-first-buffer");
      }

      @autoreleasepool {
         id<MTLCommandBuffer> command_buffer;

         puts("AO46_AGX_MULTI_QUEUE_COMMAND_BUFFER_TRACE create-second-buffer");
         command_buffer = [second_queue commandBuffer];
         if (!command_buffer) {
            fputs("AO46_AGX_MULTI_QUEUE_COMMAND_BUFFER_TRACE second buffer failed\n",
                  stderr);
            [second_queue release];
            [first_queue release];
            [device release];
            return 1;
         }

         puts("AO46_AGX_MULTI_QUEUE_COMMAND_BUFFER_TRACE release-second-buffer");
      }

      /* No command buffer is encoded or committed in this control. */
      puts("AO46_AGX_MULTI_QUEUE_COMMAND_BUFFER_TRACE release-second-queue");
      [second_queue release];
      puts("AO46_AGX_MULTI_QUEUE_COMMAND_BUFFER_TRACE release-first-queue");
      [first_queue release];
      puts("AO46_AGX_MULTI_QUEUE_COMMAND_BUFFER_TRACE release-device");
      [device release];
   }

   puts("AO46_AGX_MULTI_QUEUE_COMMAND_BUFFER_TRACE complete");
   return 0;
}
