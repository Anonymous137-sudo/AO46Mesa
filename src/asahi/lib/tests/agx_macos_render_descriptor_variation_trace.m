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
   first_width = 64,
   first_height = 64,
   second_width = 128,
   second_height = 32,
   trace_bytes_per_pixel = 4,
   trace_readback_size = first_width * first_height * trace_bytes_per_pixel,
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
submit_render(id<MTLCommandQueue> queue, id<MTLRenderPipelineState> pipeline,
              id<MTLTexture> color_target)
{
   id<MTLCommandBuffer> command_buffer = [queue commandBuffer];
   MTLRenderPassDescriptor *pass;
   id<MTLRenderCommandEncoder> render;

   if (!command_buffer)
      return false;

   pass = [MTLRenderPassDescriptor renderPassDescriptor];
   pass.colorAttachments[0].texture = color_target;
   pass.colorAttachments[0].loadAction = MTLLoadActionClear;
   pass.colorAttachments[0].clearColor = MTLClearColorMake(0.0, 0.0, 0.0, 1.0);
   pass.colorAttachments[0].storeAction = MTLStoreActionStore;
   render = [command_buffer renderCommandEncoderWithDescriptor:pass];
   if (!render)
      return false;

   [render setRenderPipelineState:pipeline];
   [render drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
   [render endEncoding];
   [command_buffer commit];
   [command_buffer waitUntilCompleted];
   return command_buffer.status == MTLCommandBufferStatusCompleted;
}

static bool
is_expected_color(uint8_t actual, uint8_t expected)
{
   return actual >= expected - 1 && actual <= expected + 1;
}

static bool
verify_color_target(id<MTLTexture> color_target, unsigned width, unsigned height)
{
   uint8_t readback[trace_readback_size];
   size_t bytes_per_row = width * trace_bytes_per_pixel;
   size_t byte_count = bytes_per_row * height;

   if (byte_count != sizeof(readback))
      return false;

   [color_target getBytes:readback
               bytesPerRow:bytes_per_row
                fromRegion:MTLRegionMake2D(0, 0, width, height)
               mipmapLevel:0];
   for (size_t offset = 0; offset < byte_count; offset += trace_bytes_per_pixel) {
      if (!is_expected_color(readback[offset], 64) ||
          !is_expected_color(readback[offset + 1], 128) ||
          !is_expected_color(readback[offset + 2], 191) ||
          readback[offset + 3] != 255) {
         return false;
      }
   }

   return true;
}

static id<MTLTexture>
new_color_target(id<MTLDevice> device, unsigned width, unsigned height)
{
   MTLTextureDescriptor *descriptor =
      [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                           width:width
                                                          height:height
                                                       mipmapped:NO];

   descriptor.usage = MTLTextureUsageRenderTarget;
   descriptor.storageMode = MTLStorageModeShared;
   return [device newTextureWithDescriptor:descriptor];
}

int
main(void)
{
   static NSString *const source =
      @"#include <metal_stdlib>\n"
       "using namespace metal;\n"
       "struct AO46VertexOut { float4 position [[position]]; };\n"
       "vertex AO46VertexOut ao46_vertex(uint id [[vertex_id]])\n"
       "{\n"
       "   const float2 positions[3] = { float2(-1.0, -1.0),\n"
       "                                 float2(3.0, -1.0),\n"
       "                                 float2(-1.0, 3.0) };\n"
       "   AO46VertexOut out;\n"
       "   out.position = float4(positions[id], 0.0, 1.0);\n"
       "   return out;\n"
       "}\n"
       "fragment float4 ao46_fragment()\n"
       "{\n"
       "   return float4(0.25, 0.5, 0.75, 1.0);\n"
       "}\n";
   id<MTLDevice> device;
   id<MTLCommandQueue> queue;
   id<MTLLibrary> library;
   id<MTLFunction> vertex;
   id<MTLFunction> fragment;
   id<MTLRenderPipelineState> pipeline;
   id<MTLTexture> first_target;
   id<MTLTexture> second_target;
   MTLRenderPipelineDescriptor *pipeline_descriptor;
   NSError *error = nil;

   @autoreleasepool {
      if (!verify_agx_device_reopen()) {
         fputs("AO46_AGX_RENDER_VARIATION device profile gate failed\n", stderr);
         return 1;
      }

      device = MTLCreateSystemDefaultDevice();
      if (!device) {
         fputs("AO46_AGX_RENDER_VARIATION no Metal device\n", stderr);
         return 1;
      }

      library = [device newLibraryWithSource:source options:nil error:&error];
      if (!library) {
         fprintf(stderr, "AO46_AGX_RENDER_VARIATION shader compile failed: %s\n",
                 error ? error.localizedDescription.UTF8String : "unknown error");
         return 1;
      }

      vertex = [library newFunctionWithName:@"ao46_vertex"];
      fragment = [library newFunctionWithName:@"ao46_fragment"];
      if (!vertex || !fragment) {
         fputs("AO46_AGX_RENDER_VARIATION shader entry point missing\n", stderr);
         return 1;
      }

      pipeline_descriptor = [MTLRenderPipelineDescriptor new];
      pipeline_descriptor.vertexFunction = vertex;
      pipeline_descriptor.fragmentFunction = fragment;
      pipeline_descriptor.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA8Unorm;
      pipeline = [device newRenderPipelineStateWithDescriptor:pipeline_descriptor
                                                          error:&error];
      [pipeline_descriptor release];
      if (!pipeline) {
         fprintf(stderr, "AO46_AGX_RENDER_VARIATION pipeline creation failed: %s\n",
                 error ? error.localizedDescription.UTF8String : "unknown error");
         return 1;
      }

      queue = [device newCommandQueue];
      puts("AO46_AGX_RENDER_VARIATION allocate target-a-64x64-rgba8");
      first_target = new_color_target(device, first_width, first_height);
      if (!queue || !first_target) {
         fputs("AO46_AGX_RENDER_VARIATION first resource setup failed\n", stderr);
         return 1;
      }

      puts("AO46_AGX_RENDER_VARIATION render target-a-first");
      if (!submit_render(queue, pipeline, first_target) ||
          !verify_color_target(first_target, first_width, first_height)) {
         fputs("AO46_AGX_RENDER_VARIATION first render verification failed\n", stderr);
         return 1;
      }

      puts("AO46_AGX_RENDER_VARIATION render target-a-repeat");
      if (!submit_render(queue, pipeline, first_target) ||
          !verify_color_target(first_target, first_width, first_height)) {
         fputs("AO46_AGX_RENDER_VARIATION repeat render verification failed\n", stderr);
         return 1;
      }

      puts("AO46_AGX_RENDER_VARIATION allocate target-b-128x32-rgba8");
      second_target = new_color_target(device, second_width, second_height);
      if (!second_target) {
         fputs("AO46_AGX_RENDER_VARIATION second resource setup failed\n", stderr);
         return 1;
      }

      puts("AO46_AGX_RENDER_VARIATION render target-b-128x32");
      if (!submit_render(queue, pipeline, second_target) ||
          !verify_color_target(second_target, second_width, second_height)) {
         fputs("AO46_AGX_RENDER_VARIATION variation render verification failed\n", stderr);
         return 1;
      }

      printf("AO46_AGX_RENDER_VARIATION complete submissions=3 bytes=%u\n",
             trace_readback_size);

      [second_target release];
      [first_target release];
      [queue release];
      [pipeline release];
      [fragment release];
      [vertex release];
      [library release];
      [device release];
   }

   return 0;
}
