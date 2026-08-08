/*
 * Copyright 2026 Khronos_AppleICDs contributors
 * SPDX-License-Identifier: MIT
 */

#import <Metal/Metal.h>

#include "agx_macos_device.h"
#include "agx_macos_iosurface.h"

#include <stdint.h>
#include <stdio.h>

enum {
   trace_width = 64,
   trace_height = 64,
   trace_bytes_per_pixel = 4,
   trace_bytes_per_row = trace_width * trace_bytes_per_pixel,
};

static bool
verify_profiled_device(void)
{
   struct agx_macos_device_session session;

   if (agx_macos_device_session_open(&session) !=
       AGX_MACOS_DEVICE_SESSION_READY) {
      return false;
   }

   agx_macos_device_session_close(&session);
   return true;
}

static bool
is_expected_color(uint8_t actual, uint8_t expected)
{
   return actual >= expected - 1 && actual <= expected + 1;
}

static bool
verify_surface_pixels(struct agx_macos_iosurface *surface)
{
   const uint8_t *bytes;
   uint32_t row_bytes;
   kern_return_t result;

   result = agx_macos_iosurface_map_read(surface, &bytes, &row_bytes);
   if (result != KERN_SUCCESS)
      return false;

   if (!bytes || row_bytes < trace_bytes_per_row) {
      (void)agx_macos_iosurface_unmap_read(surface);
      return false;
   }

   for (unsigned y = 0; y < trace_height; ++y) {
      for (unsigned x = 0; x < trace_width; ++x) {
         const uint8_t *pixel =
            bytes + (y * row_bytes) + (x * trace_bytes_per_pixel);

         if (!is_expected_color(pixel[0], 64) ||
             !is_expected_color(pixel[1], 128) ||
             !is_expected_color(pixel[2], 191) || pixel[3] != 255) {
            (void)agx_macos_iosurface_unmap_read(surface);
            return false;
         }
      }
   }

   return agx_macos_iosurface_unmap_read(surface) == KERN_SUCCESS;
}

int
main(void)
{
   id<MTLDevice> device;
   id<MTLCommandQueue> queue;
   id<MTLCommandBuffer> command_buffer;
   id<MTLTexture> texture;
   MTLTextureDescriptor *descriptor;
   MTLRenderPassDescriptor *pass;
   id<MTLRenderCommandEncoder> encoder;
   struct agx_macos_iosurface drawable = {0};
   IOSurfaceRef surface;

   @autoreleasepool {
      if (!verify_profiled_device()) {
         fputs("AO46_AGX_IOSURFACE profile gate failed\n", stderr);
         return 1;
      }

      if (agx_macos_iosurface_create_rgba8(trace_width, trace_height,
                                           &drawable) != KERN_SUCCESS) {
         fputs("AO46_AGX_IOSURFACE allocation failed\n", stderr);
         return 1;
      }
      surface = drawable.surface;

      device = MTLCreateSystemDefaultDevice();
      queue = device ? [device newCommandQueue] : nil;
      descriptor = [MTLTextureDescriptor
         texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
         width:trace_width
         height:trace_height
         mipmapped:NO];
      descriptor.usage = MTLTextureUsageRenderTarget;
      descriptor.storageMode = MTLStorageModeShared;
      texture = queue ? [device newTextureWithDescriptor:descriptor
                                                iosurface:surface
                                                     plane:0]
                      : nil;
      if (!queue || !texture) {
         fputs("AO46_AGX_IOSURFACE texture import failed\n", stderr);
         if (queue)
            [queue release];
         if (device)
            [device release];
         agx_macos_iosurface_destroy(&drawable);
         return 1;
      }

      printf("AO46_AGX_IOSURFACE drawable id=%u size=%ux%u row-bytes=%u\n",
             IOSurfaceGetID(surface), trace_width, trace_height,
             trace_bytes_per_row);
      command_buffer = [queue commandBuffer];
      pass = [MTLRenderPassDescriptor renderPassDescriptor];
      pass.colorAttachments[0].texture = texture;
      pass.colorAttachments[0].loadAction = MTLLoadActionClear;
      pass.colorAttachments[0].storeAction = MTLStoreActionStore;
      pass.colorAttachments[0].clearColor =
         MTLClearColorMake(0.25, 0.5, 0.75, 1.0);
      encoder = command_buffer
         ? [command_buffer renderCommandEncoderWithDescriptor:pass]
         : nil;
      if (!encoder) {
         fputs("AO46_AGX_IOSURFACE render encoder creation failed\n", stderr);
         [texture release];
         [queue release];
         [device release];
         agx_macos_iosurface_destroy(&drawable);
         return 1;
      }

      /* A clear-only pass isolates IOSurface import, render, and readback. */
      [encoder endEncoding];
      puts("AO46_AGX_IOSURFACE submit clear-render-pass");
      [command_buffer commit];
      [command_buffer waitUntilCompleted];
      if (command_buffer.status != MTLCommandBufferStatusCompleted ||
          !verify_surface_pixels(&drawable)) {
         fputs("AO46_AGX_IOSURFACE render verification failed\n", stderr);
         [texture release];
         [queue release];
         [device release];
         agx_macos_iosurface_destroy(&drawable);
         return 1;
      }

      printf("AO46_AGX_IOSURFACE complete id=%u\n", IOSurfaceGetID(surface));
      [texture release];
      [queue release];
      [device release];
      agx_macos_iosurface_destroy(&drawable);
   }

   return 0;
}
