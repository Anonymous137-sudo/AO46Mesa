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
   trace_element_count = 1024,
   trace_buffer_size = 64 * 1024,
   trace_input_offset = 4 * 1024,
   trace_output_offset = 12 * 1024,
   trace_salt = 0x39c5u,
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
submit_compute(id<MTLCommandQueue> queue, id<MTLComputePipelineState> pipeline,
               id<MTLBuffer> input, id<MTLBuffer> output)
{
   id<MTLCommandBuffer> command_buffer = [queue commandBuffer];
   id<MTLComputeCommandEncoder> compute;
   uint32_t salt = trace_salt;

   if (!command_buffer)
      return false;

   compute = [command_buffer computeCommandEncoder];
   if (!compute)
      return false;

   [compute setComputePipelineState:pipeline];
   [compute setBuffer:input offset:trace_input_offset atIndex:0];
   [compute setBuffer:output offset:trace_output_offset atIndex:1];
   [compute setBytes:&salt length:sizeof(salt) atIndex:2];
   [compute dispatchThreads:MTLSizeMake(trace_element_count, 1, 1)
       threadsPerThreadgroup:MTLSizeMake(32, 1, 1)];
   [compute endEncoding];
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
   static NSString *const source =
      @"#include <metal_stdlib>\n"
       "using namespace metal;\n"
       "kernel void ao46_transform(device const uint *input [[buffer(0)]],\n"
       "                           device uint *output [[buffer(1)]],\n"
       "                           constant uint &salt [[buffer(2)]],\n"
       "                           uint index [[thread_position_in_grid]])\n"
       "{\n"
       "   output[index] = input[index] ^ (salt + index * 19u);\n"
       "}\n";
   id<MTLDevice> device;
   id<MTLCommandQueue> queue;
   id<MTLLibrary> library;
   id<MTLFunction> function;
   id<MTLComputePipelineState> pipeline;
   id<MTLBuffer> input;
   id<MTLBuffer> output;
   NSError *error = nil;
   uint32_t *input_values;
   uint32_t *output_values;
   const size_t range_size = trace_element_count * sizeof(uint32_t);
   const size_t input_index = trace_input_offset / sizeof(uint32_t);
   const size_t output_index = trace_output_offset / sizeof(uint32_t);

   @autoreleasepool {
      if (!verify_agx_device_reopen()) {
         fputs("AO46_AGX_COMPUTE_RANGE device profile gate failed\n", stderr);
         return 1;
      }

      device = MTLCreateSystemDefaultDevice();
      if (!device) {
         fputs("AO46_AGX_COMPUTE_RANGE no Metal device\n", stderr);
         return 1;
      }

      library = [device newLibraryWithSource:source options:nil error:&error];
      if (!library) {
         fprintf(stderr, "AO46_AGX_COMPUTE_RANGE shader compile failed: %s\n",
                 error ? error.localizedDescription.UTF8String : "unknown error");
         return 1;
      }

      function = [library newFunctionWithName:@"ao46_transform"];
      if (!function) {
         fputs("AO46_AGX_COMPUTE_RANGE shader entry point missing\n", stderr);
         return 1;
      }

      pipeline = [device newComputePipelineStateWithFunction:function error:&error];
      if (!pipeline) {
         fprintf(stderr, "AO46_AGX_COMPUTE_RANGE pipeline creation failed: %s\n",
                 error ? error.localizedDescription.UTF8String : "unknown error");
         return 1;
      }

      queue = [device newCommandQueue];
      puts("AO46_AGX_COMPUTE_RANGE allocate input-64k-shared");
      input = [device newBufferWithLength:trace_buffer_size
                                   options:MTLResourceStorageModeShared];
      puts("AO46_AGX_COMPUTE_RANGE allocate output-64k-shared");
      output = [device newBufferWithLength:trace_buffer_size
                                    options:MTLResourceStorageModeShared];
      if (!queue || !input || !output ||
          !(input_values = input.contents) || !(output_values = output.contents)) {
         fputs("AO46_AGX_COMPUTE_RANGE resource setup failed\n", stderr);
         return 1;
      }

      memset(input_values, 0, trace_buffer_size);
      memset(output_values, 0, trace_buffer_size);
      for (unsigned i = 0; i < trace_element_count; ++i)
         input_values[input_index + i] = i * 0x81d1u;

      puts("AO46_AGX_COMPUTE_RANGE dispatch input-0x1000-to-output-0x3000");
      if (!submit_compute(queue, pipeline, input, output)) {
         fputs("AO46_AGX_COMPUTE_RANGE compute dispatch failed\n", stderr);
         return 1;
      }

      for (unsigned i = 0; i < trace_element_count; ++i) {
         if (output_values[output_index + i] !=
             (input_values[input_index + i] ^ (trace_salt + i * 19u))) {
            fputs("AO46_AGX_COMPUTE_RANGE output verification failed\n", stderr);
            return 1;
         }
      }

      if (!range_is_zero((const uint8_t *)output_values, trace_output_offset,
                         range_size)) {
         fputs("AO46_AGX_COMPUTE_RANGE output range verification failed\n", stderr);
         return 1;
      }

      printf("AO46_AGX_COMPUTE_RANGE complete elements=%u bytes=%zu input_offset=%u output_offset=%u\n",
             trace_element_count, range_size, trace_input_offset,
             trace_output_offset);

      [output release];
      [input release];
      [queue release];
      [pipeline release];
      [function release];
      [library release];
      [device release];
   }

   return 0;
}
