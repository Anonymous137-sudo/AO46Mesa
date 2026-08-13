/*
 * Copyright 2026 Khronos_AppleICDs contributors
 * SPDX-License-Identifier: MIT
 */

#import <Metal/Metal.h>

#include "agx_macos_device.h"

#include <stdio.h>

static void
trace_marker(const char *marker)
{
   puts(marker);
   fflush(stdout);
}

/* Keep the control bound to the same known UABI profile as the runtime, but
 * close the direct session before public Metal creates its own client. */
static bool
verify_agx_device_profile(const char *client_path)
{
   struct agx_macos_device_session session = {0};
   bool ready = agx_macos_device_session_open(&session) ==
      AGX_MACOS_DEVICE_SESSION_READY &&
      agx_macos_device_session_configure_traced_api(&session, client_path) ==
         KERN_SUCCESS;

   agx_macos_device_session_close(&session);
   return ready;
}

int
main(int argc, char **argv)
{
   static NSString *const source =
      @"#include <metal_stdlib>\n"
       "using namespace metal;\n"
       "kernel void ao46_shader_allocation_probe(device uint *output "
       "[[buffer(0)]], uint index [[thread_position_in_grid]])\n"
       "{ output[index] = index * 7u; }\n";
   id<MTLDevice> device;
   id<MTLLibrary> library;
   id<MTLFunction> function;
   id<MTLComputePipelineState> pipeline;
   NSError *error = nil;

   if (argc != 1 || !verify_agx_device_profile(argv[0])) {
      fputs("AO46_AGX_SHADER_ALLOCATION profile gate failed\n", stderr);
      return 1;
   }

   @autoreleasepool {
      device = MTLCreateSystemDefaultDevice();
      if (!device) {
         fputs("AO46_AGX_SHADER_ALLOCATION no Metal device\n", stderr);
         return 1;
      }

      trace_marker("AO46_AGX_SHADER_ALLOCATION source-compile-begin");
      library = [device newLibraryWithSource:source options:nil error:&error];
      if (!library) {
         fprintf(stderr, "AO46_AGX_SHADER_ALLOCATION compile failed: %s\n",
                 error ? error.localizedDescription.UTF8String : "unknown error");
         [device release];
         return 1;
      }
      trace_marker("AO46_AGX_SHADER_ALLOCATION source-compile-ready");

      function = [library newFunctionWithName:@"ao46_shader_allocation_probe"];
      if (!function) {
         fputs("AO46_AGX_SHADER_ALLOCATION entry point missing\n", stderr);
         [library release];
         [device release];
         return 1;
      }

      trace_marker("AO46_AGX_SHADER_ALLOCATION pipeline-create-begin");
      pipeline = [device newComputePipelineStateWithFunction:function error:&error];
      if (!pipeline) {
         fprintf(stderr, "AO46_AGX_SHADER_ALLOCATION pipeline failed: %s\n",
                 error ? error.localizedDescription.UTF8String : "unknown error");
         [function release];
         [library release];
         [device release];
         return 1;
      }
      printf("AO46_AGX_SHADER_ALLOCATION pipeline-create-ready max_threads=%lu\n",
             (unsigned long)pipeline.maxTotalThreadsPerThreadgroup);
      fflush(stdout);

      [pipeline release];
      [function release];
      [library release];
      [device release];
   }

   trace_marker("AO46_AGX_SHADER_ALLOCATION complete no-submit=1");
   return 0;
}
