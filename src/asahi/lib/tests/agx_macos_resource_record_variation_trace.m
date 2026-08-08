/*
 * Copyright 2026 Khronos_AppleICDs contributors
 * SPDX-License-Identifier: MIT
 */

#import <Metal/Metal.h>

#include "agx_macos_device.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

enum {
   trace_small_size = 4 * 1024,
   trace_large_size = 128 * 1024,
};

static bool
same_device_info(const struct agx_macos_device_info *a,
                 const struct agx_macos_device_info *b)
{
   return a->chip_id == b->chip_id &&
          a->gpu_generation == b->gpu_generation &&
          a->core_count == b->core_count &&
          a->cluster_count == b->cluster_count &&
          a->cores_per_cluster == b->cores_per_cluster &&
          a->gpu_partition_count == b->gpu_partition_count &&
          a->fragment_core_count == b->fragment_core_count &&
          a->usc_generation == b->usc_generation &&
          a->kickid_queue_shift == b->kickid_queue_shift &&
          a->kickid_queue_mask == b->kickid_queue_mask &&
          a->parameter_buffer_max_size == b->parameter_buffer_max_size &&
          memcmp(a->core_masks, b->core_masks, sizeof(a->core_masks)) == 0 &&
          strcmp(a->variant, b->variant) == 0;
}

/* Validate the direct profile gate before Metal opens its independent client. */
static bool
verify_agx_device_reopen(void)
{
   struct agx_macos_device_capabilities first_capabilities;
   struct agx_macos_device_info first_info;

   for (unsigned cycle = 0; cycle < 2; ++cycle) {
      struct agx_macos_device_session session;

      if (agx_macos_device_session_open(&session) !=
          AGX_MACOS_DEVICE_SESSION_READY) {
         return false;
      }

      if (cycle == 0) {
         first_info = session.info;
         first_capabilities = session.capabilities;
      } else if (!same_device_info(&first_info, &session.info) ||
                 memcmp(&first_capabilities, &session.capabilities,
                        sizeof(session.capabilities)) != 0) {
         agx_macos_device_session_close(&session);
         return false;
      }

      agx_macos_device_session_close(&session);
   }

   return true;
}

static bool
submit_copy(id<MTLCommandQueue> queue, id<MTLBuffer> source,
            id<MTLBuffer> destination, NSUInteger length)
{
   id<MTLCommandBuffer> command_buffer = [queue commandBuffer];
   id<MTLBlitCommandEncoder> blit;

   if (!command_buffer)
      return false;

   blit = [command_buffer blitCommandEncoder];
   if (!blit)
      return false;

   [blit copyFromBuffer:source
            sourceOffset:0
                toBuffer:destination
       destinationOffset:0
                    size:length];
   [blit endEncoding];
   [command_buffer commit];
   [command_buffer waitUntilCompleted];
   return command_buffer.status == MTLCommandBufferStatusCompleted;
}

static bool
submit_two_step_copy(id<MTLCommandQueue> queue, id<MTLBuffer> source,
                     id<MTLBuffer> staging, id<MTLBuffer> destination,
                     NSUInteger length)
{
   id<MTLCommandBuffer> command_buffer = [queue commandBuffer];
   id<MTLBlitCommandEncoder> blit;

   if (!command_buffer)
      return false;

   blit = [command_buffer blitCommandEncoder];
   if (!blit)
      return false;

   [blit copyFromBuffer:source
            sourceOffset:0
                toBuffer:staging
       destinationOffset:0
                    size:length];
   [blit copyFromBuffer:staging
            sourceOffset:0
                toBuffer:destination
       destinationOffset:0
                    size:length];
   [blit endEncoding];
   [command_buffer commit];
   [command_buffer waitUntilCompleted];
   return command_buffer.status == MTLCommandBufferStatusCompleted;
}

static void
write_pattern(uint8_t *bytes, size_t length, uint8_t salt)
{
   for (size_t i = 0; i < length; ++i)
      bytes[i] = (uint8_t)((i * 37u) ^ salt);
}

int
main(void)
{
   id<MTLDevice> device;
   id<MTLCommandQueue> producer_queue;
   id<MTLCommandQueue> consumer_queue;
   id<MTLBuffer> source_small;
   id<MTLBuffer> private_small;
   id<MTLBuffer> destination_small;
   id<MTLBuffer> source_large;
   id<MTLBuffer> private_large;
   id<MTLBuffer> staging_large;
   id<MTLBuffer> destination_large;
   uint8_t *small_source_bytes;
   uint8_t *small_destination_bytes;
   uint8_t *large_source_bytes;
   uint8_t *large_destination_bytes;

   @autoreleasepool {
      if (!verify_agx_device_reopen()) {
         fputs("AO46_AGX_RESOURCE_VARIATION device profile gate failed\n", stderr);
         return 1;
      }

      device = MTLCreateSystemDefaultDevice();
      if (!device) {
         fputs("AO46_AGX_RESOURCE_VARIATION no Metal device\n", stderr);
         return 1;
      }

      producer_queue = [device newCommandQueue];
      consumer_queue = [device newCommandQueue];

      puts("AO46_AGX_RESOURCE_VARIATION allocate source-4k-shared");
      source_small = [device newBufferWithLength:trace_small_size
                                          options:MTLResourceStorageModeShared];
      puts("AO46_AGX_RESOURCE_VARIATION allocate private-4k");
      private_small = [device newBufferWithLength:trace_small_size
                                           options:MTLResourceStorageModePrivate];
      puts("AO46_AGX_RESOURCE_VARIATION allocate destination-4k-shared");
      destination_small =
         [device newBufferWithLength:trace_small_size
                              options:MTLResourceStorageModeShared];

      puts("AO46_AGX_RESOURCE_VARIATION allocate source-128k-shared");
      source_large = [device newBufferWithLength:trace_large_size
                                          options:MTLResourceStorageModeShared];
      puts("AO46_AGX_RESOURCE_VARIATION allocate private-128k");
      private_large = [device newBufferWithLength:trace_large_size
                                           options:MTLResourceStorageModePrivate];
      puts("AO46_AGX_RESOURCE_VARIATION allocate staging-128k-write-combined");
      staging_large =
         [device newBufferWithLength:trace_large_size
                              options:MTLResourceStorageModeShared |
                                      MTLResourceCPUCacheModeWriteCombined];
      puts("AO46_AGX_RESOURCE_VARIATION allocate destination-128k-shared");
      destination_large =
         [device newBufferWithLength:trace_large_size
                              options:MTLResourceStorageModeShared];

      if (!producer_queue || !consumer_queue || !source_small || !private_small ||
          !destination_small || !source_large || !private_large || !staging_large ||
          !destination_large ||
          !(small_source_bytes = source_small.contents) ||
          !(small_destination_bytes = destination_small.contents) ||
          !(large_source_bytes = source_large.contents) ||
          !(large_destination_bytes = destination_large.contents)) {
         fputs("AO46_AGX_RESOURCE_VARIATION resource setup failed\n", stderr);
         return 1;
      }

      write_pattern(small_source_bytes, trace_small_size, 0x3d);
      memset(small_destination_bytes, 0, trace_small_size);
      puts("AO46_AGX_RESOURCE_VARIATION producer-4k shared-to-private");
      if (!submit_copy(producer_queue, source_small, private_small,
                       trace_small_size)) {
         fputs("AO46_AGX_RESOURCE_VARIATION producer-4k failed\n", stderr);
         return 1;
      }

      puts("AO46_AGX_RESOURCE_VARIATION consumer-4k private-to-shared");
      if (!submit_copy(consumer_queue, private_small, destination_small,
                       trace_small_size) ||
          memcmp(small_source_bytes, small_destination_bytes, trace_small_size) != 0) {
         fputs("AO46_AGX_RESOURCE_VARIATION 4k data verification failed\n", stderr);
         return 1;
      }

      write_pattern(large_source_bytes, trace_large_size, 0xa6);
      memset(large_destination_bytes, 0, trace_large_size);
      puts("AO46_AGX_RESOURCE_VARIATION producer-128k shared-to-private");
      if (!submit_copy(producer_queue, source_large, private_large,
                       trace_large_size)) {
         fputs("AO46_AGX_RESOURCE_VARIATION producer-128k failed\n", stderr);
         return 1;
      }

      puts("AO46_AGX_RESOURCE_VARIATION consumer-128k private-via-write-combined-to-shared");
      if (!submit_two_step_copy(consumer_queue, private_large, staging_large,
                                destination_large, trace_large_size) ||
          memcmp(large_source_bytes, large_destination_bytes, trace_large_size) != 0) {
         fputs("AO46_AGX_RESOURCE_VARIATION 128k data verification failed\n", stderr);
         return 1;
      }

      printf("AO46_AGX_RESOURCE_VARIATION complete bytes_4k=%u bytes_128k=%u\n",
             trace_small_size, trace_large_size);

      [destination_large release];
      [staging_large release];
      [private_large release];
      [source_large release];
      [destination_small release];
      [private_small release];
      [source_small release];
      [consumer_queue release];
      [producer_queue release];
      [device release];
   }

   return 0;
}
