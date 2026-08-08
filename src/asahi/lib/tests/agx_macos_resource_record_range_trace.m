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
   trace_buffer_size = 64 * 1024,
   trace_copy_size = 8 * 1024,
   trace_source_offset = 4 * 1024,
   trace_private_offset = 12 * 1024,
   trace_staging_offset = 20 * 1024,
   trace_destination_offset = 28 * 1024,
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
            NSUInteger source_offset, id<MTLBuffer> destination,
            NSUInteger destination_offset)
{
   id<MTLCommandBuffer> command_buffer = [queue commandBuffer];
   id<MTLBlitCommandEncoder> blit;

   if (!command_buffer)
      return false;

   blit = [command_buffer blitCommandEncoder];
   if (!blit)
      return false;

   [blit copyFromBuffer:source
            sourceOffset:source_offset
                toBuffer:destination
       destinationOffset:destination_offset
                    size:trace_copy_size];
   [blit endEncoding];
   [command_buffer commit];
   [command_buffer waitUntilCompleted];
   return command_buffer.status == MTLCommandBufferStatusCompleted;
}

static bool
submit_two_step_copy(id<MTLCommandQueue> queue, id<MTLBuffer> source,
                     NSUInteger source_offset, id<MTLBuffer> staging,
                     NSUInteger staging_offset, id<MTLBuffer> destination,
                     NSUInteger destination_offset)
{
   id<MTLCommandBuffer> command_buffer = [queue commandBuffer];
   id<MTLBlitCommandEncoder> blit;

   if (!command_buffer)
      return false;

   blit = [command_buffer blitCommandEncoder];
   if (!blit)
      return false;

   [blit copyFromBuffer:source
            sourceOffset:source_offset
                toBuffer:staging
       destinationOffset:staging_offset
                    size:trace_copy_size];
   [blit copyFromBuffer:staging
            sourceOffset:staging_offset
                toBuffer:destination
       destinationOffset:destination_offset
                    size:trace_copy_size];
   [blit endEncoding];
   [command_buffer commit];
   [command_buffer waitUntilCompleted];
   return command_buffer.status == MTLCommandBufferStatusCompleted;
}

static bool
range_is_zero(const uint8_t *bytes, size_t offset, size_t length)
{
   for (size_t i = 0; i < trace_buffer_size; ++i) {
      if (i >= offset && i < offset + length)
         continue;
      if (bytes[i] != 0)
         return false;
   }

   return true;
}

int
main(void)
{
   id<MTLDevice> device;
   id<MTLCommandQueue> producer_queue;
   id<MTLCommandQueue> consumer_queue;
   id<MTLBuffer> source;
   id<MTLBuffer> private_buffer;
   id<MTLBuffer> staging;
   id<MTLBuffer> destination;
   uint8_t *source_bytes;
   uint8_t *destination_bytes;

   @autoreleasepool {
      if (!verify_agx_device_reopen()) {
         fputs("AO46_AGX_RESOURCE_RANGE device profile gate failed\n", stderr);
         return 1;
      }

      device = MTLCreateSystemDefaultDevice();
      if (!device) {
         fputs("AO46_AGX_RESOURCE_RANGE no Metal device\n", stderr);
         return 1;
      }

      producer_queue = [device newCommandQueue];
      consumer_queue = [device newCommandQueue];
      puts("AO46_AGX_RESOURCE_RANGE allocate source-shared");
      source = [device newBufferWithLength:trace_buffer_size
                                    options:MTLResourceStorageModeShared];
      puts("AO46_AGX_RESOURCE_RANGE allocate private-transfer");
      private_buffer = [device newBufferWithLength:trace_buffer_size
                                            options:MTLResourceStorageModePrivate];
      puts("AO46_AGX_RESOURCE_RANGE allocate staging-write-combined");
      staging = [device newBufferWithLength:trace_buffer_size
                                     options:MTLResourceStorageModeShared |
                                             MTLResourceCPUCacheModeWriteCombined];
      puts("AO46_AGX_RESOURCE_RANGE allocate destination-shared");
      destination = [device newBufferWithLength:trace_buffer_size
                                         options:MTLResourceStorageModeShared];

      if (!producer_queue || !consumer_queue || !source || !private_buffer ||
          !staging || !destination || !(source_bytes = source.contents) ||
          !(destination_bytes = destination.contents)) {
         fputs("AO46_AGX_RESOURCE_RANGE resource setup failed\n", stderr);
         return 1;
      }

      memset(source_bytes, 0, trace_buffer_size);
      memset(destination_bytes, 0, trace_buffer_size);
      for (size_t i = 0; i < trace_copy_size; ++i)
         source_bytes[trace_source_offset + i] = (uint8_t)((i * 29u) ^ 0x6b);

      puts("AO46_AGX_RESOURCE_RANGE producer source-0x1000-to-private-0x3000");
      if (!submit_copy(producer_queue, source, trace_source_offset,
                       private_buffer, trace_private_offset)) {
         fputs("AO46_AGX_RESOURCE_RANGE producer copy failed\n", stderr);
         return 1;
      }

      puts("AO46_AGX_RESOURCE_RANGE consumer private-0x3000-via-staging-0x5000-to-destination-0x7000");
      if (!submit_two_step_copy(consumer_queue, private_buffer,
                                trace_private_offset, staging,
                                trace_staging_offset, destination,
                                trace_destination_offset) ||
          memcmp(source_bytes + trace_source_offset,
                 destination_bytes + trace_destination_offset,
                 trace_copy_size) != 0 ||
          !range_is_zero(destination_bytes, trace_destination_offset,
                         trace_copy_size)) {
         fputs("AO46_AGX_RESOURCE_RANGE data range verification failed\n", stderr);
         return 1;
      }

      printf("AO46_AGX_RESOURCE_RANGE complete copy_size=%u source_offset=%u private_offset=%u staging_offset=%u destination_offset=%u\n",
             trace_copy_size, trace_source_offset, trace_private_offset,
             trace_staging_offset, trace_destination_offset);

      [destination release];
      [staging release];
      [private_buffer release];
      [source release];
      [consumer_queue release];
      [producer_queue release];
      [device release];
   }

   return 0;
}
