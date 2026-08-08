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
   trace_small_buffer_size = 4 * 1024,
   trace_buffer_size = 64 * 1024,
   trace_medium_buffer_size = 128 * 1024,
   trace_large_buffer_size = 256 * 1024,
   trace_extra_large_buffer_size = 512 * 1024,
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

/* Run the profile gate in this process before Metal creates its own client.
 * This uses only the validated capability selector, never allocation UABI. */
static bool
verify_agx_device_reopen(void)
{
   struct agx_macos_device_capabilities first_capabilities;
   struct agx_macos_device_info first_info;
   char first_service_name[sizeof(((struct agx_macos_device *)0)->service_name)] = {0};

   for (unsigned cycle = 0; cycle < 2; ++cycle) {
      struct agx_macos_device_session session;

      if (agx_macos_device_session_open(&session) !=
          AGX_MACOS_DEVICE_SESSION_READY) {
         return false;
      }

      if (cycle == 0) {
         first_info = session.info;
         first_capabilities = session.capabilities;
         snprintf(first_service_name, sizeof(first_service_name), "%s",
                  session.device.service_name);
      } else if (!same_device_info(&first_info, &session.info) ||
                 memcmp(&first_capabilities, &session.capabilities,
                        sizeof(session.capabilities)) != 0 ||
                 strcmp(first_service_name, session.device.service_name) != 0) {
         agx_macos_device_session_close(&session);
         return false;
      }

      printf("AO46_AGX_TRACE_CONTROL device-reopen cycle=%u profile=%s\n",
             cycle + 1, agx_macos_device_profile_name(session.profile));
      agx_macos_device_session_close(&session);
   }

   return true;
}

static bool
submit_copy(id<MTLCommandQueue> queue, id<MTLBuffer> source,
            id<MTLBuffer> destination)
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
                    size:trace_buffer_size];
   [blit endEncoding];
   [command_buffer commit];
   [command_buffer waitUntilCompleted];

   return command_buffer.status == MTLCommandBufferStatusCompleted;
}

static bool
submit_two_step_copy(id<MTLCommandQueue> queue, id<MTLBuffer> source,
                     id<MTLBuffer> staging, id<MTLBuffer> destination)
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
                    size:trace_buffer_size];
   [blit copyFromBuffer:staging
            sourceOffset:0
                toBuffer:destination
       destinationOffset:0
                    size:trace_buffer_size];
   [blit endEncoding];
   [command_buffer commit];
   [command_buffer waitUntilCompleted];

   return command_buffer.status == MTLCommandBufferStatusCompleted;
}

int
main(void)
{
   id<MTLDevice> device;
   id<MTLCommandQueue> producer_queue;
   id<MTLCommandQueue> consumer_queue;
   id<MTLBuffer> shared_small;
   id<MTLBuffer> shared_source;
   id<MTLBuffer> write_combined_staging;
   id<MTLBuffer> private_buffer;
   id<MTLBuffer> shared_destination;
   id<MTLBuffer> shared_medium;
   id<MTLBuffer> shared_large;
   id<MTLBuffer> shared_extra_large;
   id<MTLBuffer> recycled_buffer;
   uint8_t *source_bytes;
   uint8_t *destination_bytes;

   @autoreleasepool {
      if (!verify_agx_device_reopen()) {
         fputs("AO46_AGX_TRACE_CONTROL AGX device reopen failed\n", stderr);
         return 1;
      }

      device = MTLCreateSystemDefaultDevice();
      if (!device) {
         fputs("AO46_AGX_TRACE_CONTROL no Metal device\n", stderr);
         return 1;
      }

      producer_queue = [device newCommandQueue];
      consumer_queue = [device newCommandQueue];
      puts("AO46_AGX_TRACE_CONTROL allocate shared-default-4k");
      shared_small = [device newBufferWithLength:trace_small_buffer_size
                                         options:MTLResourceStorageModeShared];
      puts("AO46_AGX_TRACE_CONTROL allocate shared-default-source");
      shared_source = [device newBufferWithLength:trace_buffer_size
                                           options:MTLResourceStorageModeShared];
      puts("AO46_AGX_TRACE_CONTROL allocate shared-write-combined-staging");
      write_combined_staging =
         [device newBufferWithLength:trace_buffer_size
                              options:MTLResourceStorageModeShared |
                                      MTLResourceCPUCacheModeWriteCombined];
      puts("AO46_AGX_TRACE_CONTROL allocate private-transfer");
      private_buffer = [device newBufferWithLength:trace_buffer_size
                                            options:MTLResourceStorageModePrivate];
      puts("AO46_AGX_TRACE_CONTROL allocate shared-default-destination");
      shared_destination = [device newBufferWithLength:trace_buffer_size
                                                options:MTLResourceStorageModeShared];
      puts("AO46_AGX_TRACE_CONTROL allocate shared-default-128k");
      shared_medium = [device newBufferWithLength:trace_medium_buffer_size
                                          options:MTLResourceStorageModeShared];
      puts("AO46_AGX_TRACE_CONTROL allocate shared-default-256k");
      shared_large = [device newBufferWithLength:trace_large_buffer_size
                                         options:MTLResourceStorageModeShared];
      puts("AO46_AGX_TRACE_CONTROL allocate shared-default-512k");
      shared_extra_large =
         [device newBufferWithLength:trace_extra_large_buffer_size
                              options:MTLResourceStorageModeShared];

      if (!producer_queue || !consumer_queue || !shared_small || !shared_source ||
          !write_combined_staging || !private_buffer || !shared_destination ||
          !shared_medium || !shared_large || !shared_extra_large ||
          !shared_small.contents || !shared_medium.contents ||
          !shared_large.contents || !shared_extra_large.contents ||
          !(source_bytes = shared_source.contents) ||
          !(destination_bytes = shared_destination.contents)) {
         fputs("AO46_AGX_TRACE_CONTROL resource setup failed\n", stderr);
         return 1;
      }

      for (size_t i = 0; i < trace_buffer_size; ++i)
         source_bytes[i] = (uint8_t)((i * 37u) ^ 0xa5u);
      memset(destination_bytes, 0, trace_buffer_size);

      puts("AO46_AGX_TRACE_CONTROL producer-copy shared-to-private");
      if (!submit_copy(producer_queue, shared_source, private_buffer)) {
         fputs("AO46_AGX_TRACE_CONTROL producer copy failed\n", stderr);
         return 1;
      }

      puts("AO46_AGX_TRACE_CONTROL consumer-copy private-via-write-combined-to-shared");
      if (!submit_two_step_copy(consumer_queue, private_buffer,
                                write_combined_staging, shared_destination)) {
         fputs("AO46_AGX_TRACE_CONTROL consumer copy failed\n", stderr);
         return 1;
      }

      if (memcmp(source_bytes, destination_bytes, trace_buffer_size) != 0) {
         fputs("AO46_AGX_TRACE_CONTROL copied bytes differ\n", stderr);
         return 1;
      }

      /* The completed command buffers no longer retain this allocation. The
       * next shared allocation provides a controlled handle/VA reuse sample. */
      puts("AO46_AGX_TRACE_CONTROL release private-transfer");
      [private_buffer release];
      private_buffer = nil;

      puts("AO46_AGX_TRACE_CONTROL allocate shared-default-recycled");
      recycled_buffer = [device newBufferWithLength:trace_buffer_size
                                             options:MTLResourceStorageModeShared];
      if (!recycled_buffer || !recycled_buffer.contents) {
         fputs("AO46_AGX_TRACE_CONTROL recycled allocation failed\n", stderr);
         return 1;
      }

      printf("AO46_AGX_TRACE_CONTROL complete bytes=%u allocations=9\n",
             trace_buffer_size);

      [recycled_buffer release];
      [shared_extra_large release];
      [shared_large release];
      [shared_medium release];
      [shared_destination release];
      [write_combined_staging release];
      [shared_source release];
      [shared_small release];
      [consumer_queue release];
      [producer_queue release];
      [device release];
   }

   return 0;
}
