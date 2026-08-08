/*
 * Copyright 2026 Khronos_AppleICDs contributors
 * SPDX-License-Identifier: MIT
 */

#import <Metal/Metal.h>

#include <stdbool.h>
#include <stdio.h>

static bool
submit_empty(id<MTLCommandQueue> queue, const char *create_marker,
             const char *commit_marker, const char *release_marker)
{
   @autoreleasepool {
      id<MTLCommandBuffer> command_buffer;

      puts(create_marker);
      command_buffer = [queue commandBuffer];
      if (!command_buffer)
         return false;

      puts(commit_marker);
      [command_buffer commit];
      [command_buffer waitUntilCompleted];
      if (command_buffer.status != MTLCommandBufferStatusCompleted)
         return false;

      puts(release_marker);
   }

   return true;
}

int
main(void)
{
   id<MTLDevice> device;
   id<MTLCommandQueue> first_queue;
   id<MTLCommandQueue> second_queue;

   @autoreleasepool {
      puts("AO46_AGX_TWO_QUEUE_EMPTY_SUBMISSION_TRACE create-device");
      device = MTLCreateSystemDefaultDevice();
      if (!device) {
         fputs("AO46_AGX_TWO_QUEUE_EMPTY_SUBMISSION_TRACE no Metal device\n",
               stderr);
         return 1;
      }

      puts("AO46_AGX_TWO_QUEUE_EMPTY_SUBMISSION_TRACE create-first-queue");
      first_queue = [device newCommandQueue];
      puts("AO46_AGX_TWO_QUEUE_EMPTY_SUBMISSION_TRACE create-second-queue");
      second_queue = [device newCommandQueue];
      if (!first_queue || !second_queue) {
         fputs("AO46_AGX_TWO_QUEUE_EMPTY_SUBMISSION_TRACE queue creation failed\n",
               stderr);
         [second_queue release];
         [first_queue release];
         [device release];
         return 1;
      }

      if (!submit_empty(first_queue,
                        "AO46_AGX_TWO_QUEUE_EMPTY_SUBMISSION_TRACE create-first-buffer",
                        "AO46_AGX_TWO_QUEUE_EMPTY_SUBMISSION_TRACE commit-first-empty",
                        "AO46_AGX_TWO_QUEUE_EMPTY_SUBMISSION_TRACE release-first-buffer") ||
          !submit_empty(second_queue,
                        "AO46_AGX_TWO_QUEUE_EMPTY_SUBMISSION_TRACE create-second-buffer",
                        "AO46_AGX_TWO_QUEUE_EMPTY_SUBMISSION_TRACE commit-second-empty",
                        "AO46_AGX_TWO_QUEUE_EMPTY_SUBMISSION_TRACE release-second-buffer")) {
         fputs("AO46_AGX_TWO_QUEUE_EMPTY_SUBMISSION_TRACE submission failed\n",
               stderr);
         [second_queue release];
         [first_queue release];
         [device release];
         return 1;
      }

      puts("AO46_AGX_TWO_QUEUE_EMPTY_SUBMISSION_TRACE release-second-queue");
      [second_queue release];
      puts("AO46_AGX_TWO_QUEUE_EMPTY_SUBMISSION_TRACE release-first-queue");
      [first_queue release];
      puts("AO46_AGX_TWO_QUEUE_EMPTY_SUBMISSION_TRACE release-device");
      [device release];
   }

   puts("AO46_AGX_TWO_QUEUE_EMPTY_SUBMISSION_TRACE complete");
   return 0;
}
