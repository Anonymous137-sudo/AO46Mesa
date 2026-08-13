/*
 * Copyright 2026 Khronos_AppleICDs contributors
 * SPDX-License-Identifier: MIT
 */

#import <Metal/Metal.h>
#import <objc/runtime.h>

#include "agx_macos_device.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define AO46_G16X_RESOURCE_REF_ENCODING \
   "^{__IOGPUResource={__CFRuntimeBase=QAQ}^{__IOGPUDevice}^vQQIIQQ" \
   "^{IOGPUClientSharedRO}QQQQ^v[0Q]}16@0:8"

enum { trace_size = 4096 };

typedef const void *(*trace_resource_ref_fn)(id resource, SEL selector);

static bool
trace_resource_ref(id<MTLBuffer> buffer, const void **out_ref)
{
   Class buffer_class = objc_lookUpClass("IOGPUMetalBuffer");
   Class resource_class = objc_lookUpClass("IOGPUMetalResource");
   SEL selector = sel_registerName("resourceRef");
   Method method = resource_class
      ? class_getInstanceMethod(resource_class, selector)
      : NULL;
   const char *encoding = method ? method_getTypeEncoding(method) : NULL;
   trace_resource_ref_fn resource_ref;

   if (!buffer || !out_ref || !buffer_class || !resource_class ||
       ![(id)buffer isKindOfClass:buffer_class] || !encoding ||
       strcmp(encoding, AO46_G16X_RESOURCE_REF_ENCODING) != 0) {
      return false;
   }

   resource_ref = (trace_resource_ref_fn)[(id)buffer methodForSelector:selector];
   *out_ref = resource_ref ? resource_ref((id)buffer, selector) : NULL;
   return *out_ref != NULL;
}

static bool
trace_blit(id<MTLCommandQueue> queue, id<MTLBuffer> source,
           id<MTLBuffer> destination)
{
   id<MTLCommandBuffer> command_buffer = [queue commandBuffer];
   id<MTLBlitCommandEncoder> blit;

   if (!command_buffer)
      return false;

   blit = [command_buffer blitCommandEncoder];
   if (!blit)
      return false;

   [blit copyFromBuffer:source sourceOffset:0 toBuffer:destination
      destinationOffset:0 size:trace_size];
   [blit endEncoding];
   [command_buffer commit];
   [command_buffer waitUntilCompleted];
   return command_buffer.status == MTLCommandBufferStatusCompleted;
}

int
main(void)
{
   struct agx_macos_device_session session = {0};
   id<MTLDevice> device;
   id<MTLCommandQueue> queue;
   id<MTLBuffer> source;
   id<MTLBuffer> destination;
   const void *source_ref = NULL;
   const void *destination_ref = NULL;
   uint8_t *source_data;
   uint8_t *destination_data;
   bool passed = false;

   if (agx_macos_device_session_open(&session) !=
       AGX_MACOS_DEVICE_SESSION_READY) {
      return 77;
   }

   @autoreleasepool {
      device = MTLCreateSystemDefaultDevice();
      queue = [device newCommandQueue];
      source = [device newBufferWithLength:trace_size
                                   options:MTLResourceStorageModeShared];
      destination = [device newBufferWithLength:trace_size
                                        options:MTLResourceStorageModeShared];
      source_data = source ? source.contents : NULL;
      destination_data = destination ? destination.contents : NULL;
      if (!device || !queue || !source || !destination || !source_data ||
          !destination_data || !trace_resource_ref(source, &source_ref) ||
          !trace_resource_ref(destination, &destination_ref)) {
         goto out;
      }

      for (unsigned i = 0; i < trace_size; ++i)
         source_data[i] = (uint8_t)(i * 31u + 7u);
      memset(destination_data, 0, trace_size);

      printf("AO46_AGX_BUFFER_BINDING public-resource name=source object=%p "
             "resource_ref=%p resource_ref_plus_0x40=%p buffer_plus_0x40=%p "
             "gpu_va=%#llx bytes=%u\n",
             source, source_ref, (const uint8_t *)source_ref + 0x40,
             (const uint8_t *)source + 0x40,
             (unsigned long long)source.gpuAddress, trace_size);
      printf("AO46_AGX_BUFFER_BINDING public-resource name=destination object=%p "
             "resource_ref=%p resource_ref_plus_0x40=%p buffer_plus_0x40=%p "
             "gpu_va=%#llx bytes=%u\n",
             destination, destination_ref, (const uint8_t *)destination_ref + 0x40,
             (const uint8_t *)destination + 0x40,
             (unsigned long long)destination.gpuAddress, trace_size);

      passed = trace_blit(queue, source, destination) &&
         memcmp(source_data, destination_data, trace_size) == 0;

out:
      [destination release];
      [source release];
      [queue release];
      [device release];
   }

   agx_macos_device_session_close(&session);
   if (!passed) {
      fputs("AO46_AGX_BUFFER_BINDING blit failed\n", stderr);
      return 1;
   }

   puts("AO46_AGX_BUFFER_BINDING complete");
   return 0;
}
