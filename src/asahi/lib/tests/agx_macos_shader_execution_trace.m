/*
 * Copyright 2026 Khronos_AppleICDs contributors
 * SPDX-License-Identifier: MIT
 */

#import <Metal/Metal.h>

#include "agx_macos_device.h"

#include <dlfcn.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
   trace_output_value = 0x6a46u,
};

enum trace_workload {
   TRACE_WORKLOAD_BASELINE,
   TRACE_WORKLOAD_THREADGROUP,
   TRACE_WORKLOAD_TWO_BUFFERS,
   /* Uses a bounded 32 KiB threadgroup allocation to force the Apple USC
    * residency path during a read-only profiling control. */
   TRACE_WORKLOAD_USC_STRESS,
   /* Requests a public ICB-compatible pipeline. The AGX compute-program
    * factory uses this descriptor feature when choosing its profile path. */
   TRACE_WORKLOAD_INDIRECT_COMMAND,
   /* Three submissions with shared process, queue, and buffers. This removes
    * address-space churn from carrier and pipeline differential captures. */
   TRACE_WORKLOAD_PAIRED,
};

static void
trace_marker(const char *marker)
{
   puts(marker);
   fflush(stdout);
}

/* Configure the direct-session profile first, then close it before the public
 * control creates its independent Apple client. */
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

static void
wait_for_contract_debugger(void)
{
   if (getenv("AO46_AGX_SHADER_CONTRACT_DEBUG_STOP")) {
      trace_marker("AO46_AGX_SHADER_EXECUTION debugger-ready");
      raise(SIGSTOP);
   }
}

static enum trace_workload
selected_workload(void)
{
   const char *workload = getenv("AO46_AGX_SHADER_CONTRACT_WORKLOAD");
   if (workload && strcmp(workload, "threadgroup") == 0)
      return TRACE_WORKLOAD_THREADGROUP;
   if (workload && strcmp(workload, "two-buffers") == 0)
      return TRACE_WORKLOAD_TWO_BUFFERS;
   if (workload && strcmp(workload, "usc-stress") == 0)
      return TRACE_WORKLOAD_USC_STRESS;
   if (workload && strcmp(workload, "indirect-command") == 0)
      return TRACE_WORKLOAD_INDIRECT_COMMAND;
   if (workload && strcmp(workload, "paired") == 0)
      return TRACE_WORKLOAD_PAIRED;

   return TRACE_WORKLOAD_BASELINE;
}

static void *
preload_agx_profile_bundle(void)
{
   const char *path = getenv("AO46_AGX_SHADER_CONTRACT_PRELOAD_BUNDLE");

   return path && path[0] ? dlopen(path, RTLD_LAZY | RTLD_LOCAL) : NULL;
}

static bool
submit_control(id<MTLCommandQueue> queue,
               id<MTLComputePipelineState> pipeline,
               id<MTLBuffer> output,
               id<MTLBuffer> input,
               NSUInteger thread_count,
               const char *phase)
{
   id<MTLCommandBuffer> command_buffer = [queue commandBuffer];
   id<MTLComputeCommandEncoder> encoder = nil;
   bool completed = false;

   /* Keep every observed private transition associated with this one control. */
   trace_marker(phase);
   if (!command_buffer) {
      fputs("AO46_AGX_SHADER_EXECUTION paired command-buffer creation failed\n",
            stderr);
      return false;
   }
   trace_marker("AO46_AGX_SHADER_EXECUTION command-buffer-create-ready");

   encoder = [command_buffer computeCommandEncoder];
   if (!encoder) {
      fputs("AO46_AGX_SHADER_EXECUTION paired encoder creation failed\n", stderr);
      goto cleanup;
   }
   trace_marker("AO46_AGX_SHADER_EXECUTION encoder-create-ready");

   [encoder setComputePipelineState:pipeline];
   trace_marker("AO46_AGX_SHADER_EXECUTION pipeline-bind-ready");

   [encoder setBuffer:output offset:0 atIndex:0];
   if (input)
      [encoder setBuffer:input offset:0 atIndex:1];
   trace_marker("AO46_AGX_SHADER_EXECUTION resource-bind-ready");

   [encoder dispatchThreads:MTLSizeMake(thread_count, 1, 1)
       threadsPerThreadgroup:MTLSizeMake(thread_count, 1, 1)];
   [encoder endEncoding];
   encoder = nil;
   trace_marker("AO46_AGX_SHADER_EXECUTION dispatch-ready");

   trace_marker("AO46_AGX_SHADER_EXECUTION commit-begin");
   [command_buffer commit];
   trace_marker("AO46_AGX_SHADER_EXECUTION commit-ready");

   trace_marker("AO46_AGX_SHADER_EXECUTION completion-wait-begin");
   [command_buffer waitUntilCompleted];
   completed = command_buffer.status == MTLCommandBufferStatusCompleted &&
      *(const uint32_t *)output.contents == trace_output_value;
   if (!completed)
      fputs("AO46_AGX_SHADER_EXECUTION paired output verification failed\n",
            stderr);
   else
      trace_marker("AO46_AGX_SHADER_EXECUTION completion-wait-ready");

cleanup:
   [encoder release];
   return completed;
}

int
main(int argc, char **argv)
{
   static NSString *const source =
      @"#include <metal_stdlib>\n"
       "using namespace metal;\n"
       "kernel void ao46_shader_execution_probe(device uint *output "
       "[[buffer(0)]]) { output[0] = 0x6a46u; }\n"
       "kernel void ao46_shader_execution_probe_variant(device uint *output "
       "[[buffer(0)]], uint tid [[thread_position_in_threadgroup]]) { "
       "threadgroup uint scratch[256]; scratch[tid] = 0x6a46u; "
       "threadgroup_barrier(mem_flags::mem_threadgroup); "
       "if (tid == 0) output[0] = scratch[0]; }\n"
       "kernel void ao46_shader_execution_probe_usc_stress(device uint *output "
       "[[buffer(0)]], uint tid [[thread_position_in_threadgroup]]) { "
       "threadgroup volatile uint scratch[8192]; uint base = tid * 256u; "
       "for (uint i = 0; i < 256; ++i) scratch[base + i] = tid ^ i; "
       "threadgroup_barrier(mem_flags::mem_threadgroup); "
       "if (tid == 0) { uint observed = 0; "
       "for (uint i = 0; i < 8192; ++i) observed ^= scratch[i]; "
       "output[0] = 0x6a46u ^ observed ^ observed; } }\n"
       "kernel void ao46_shader_execution_probe_two_buffers(device uint *output "
       "[[buffer(0)]], device const uint *input [[buffer(1)]]) { "
       "output[0] = input[0]; }\n";
   id<MTLDevice> device = nil;
   id<MTLLibrary> library = nil;
   id<MTLFunction> function = nil;
   id<MTLFunction> variant_function = nil;
   id<MTLFunction> two_buffer_function = nil;
   id<MTLFunction> usc_stress_function = nil;
   id<MTLComputePipelineState> pipeline = nil;
   id<MTLComputePipelineState> variant_pipeline = nil;
   id<MTLComputePipelineState> two_buffer_pipeline = nil;
   id<MTLComputePipelineState> usc_stress_pipeline = nil;
   id<MTLComputePipelineState> indirect_command_pipeline = nil;
   id<MTLCommandQueue> queue = nil;
   id<MTLBuffer> output = nil;
   id<MTLBuffer> input = nil;
   id<MTLCommandBuffer> command_buffer = nil;
   id<MTLComputeCommandEncoder> encoder = nil;
   NSError *error = nil;
   enum trace_workload workload = selected_workload();
   void *agx_profile_bundle = NULL;
   int status = 1;

   if (argc != 1 || !verify_agx_device_profile(argv[0])) {
      fputs("AO46_AGX_SHADER_EXECUTION profile gate failed\n", stderr);
      return 1;
   }

   /* The profiler needs the exact AGX image loaded before its pre-device
    * breakpoints are installed. Loading alone does not create a device or
    * invoke an Apple private API. */
   agx_profile_bundle = preload_agx_profile_bundle();
   if (!agx_profile_bundle) {
      fputs("AO46_AGX_SHADER_EXECUTION AGX profile preload failed\n", stderr);
      return 1;
   }
   wait_for_contract_debugger();

   @autoreleasepool {
      device = MTLCreateSystemDefaultDevice();
      if (!device) {
         fputs("AO46_AGX_SHADER_EXECUTION no Metal device\n", stderr);
         goto cleanup;
      }

      trace_marker("AO46_AGX_SHADER_EXECUTION source-compile-begin");
      library = [device newLibraryWithSource:source options:nil error:&error];
      if (!library) {
         fprintf(stderr, "AO46_AGX_SHADER_EXECUTION compile failed: %s\n",
                 error ? error.localizedDescription.UTF8String : "unknown error");
         goto cleanup;
      }
      trace_marker("AO46_AGX_SHADER_EXECUTION source-compile-ready");
      switch (workload) {
      case TRACE_WORKLOAD_THREADGROUP:
         trace_marker("AO46_AGX_SHADER_EXECUTION workload=threadgroup");
         break;
      case TRACE_WORKLOAD_TWO_BUFFERS:
         trace_marker("AO46_AGX_SHADER_EXECUTION workload=two-buffers");
         break;
      case TRACE_WORKLOAD_USC_STRESS:
         trace_marker("AO46_AGX_SHADER_EXECUTION workload=usc-stress");
         break;
      case TRACE_WORKLOAD_INDIRECT_COMMAND:
         trace_marker("AO46_AGX_SHADER_EXECUTION workload=indirect-command");
         break;
      case TRACE_WORKLOAD_PAIRED:
         trace_marker("AO46_AGX_SHADER_EXECUTION workload=paired");
         break;
      case TRACE_WORKLOAD_BASELINE:
         trace_marker("AO46_AGX_SHADER_EXECUTION workload=baseline");
         break;
      }

      function = [library newFunctionWithName:@"ao46_shader_execution_probe"];
      if (!function) {
         fputs("AO46_AGX_SHADER_EXECUTION entry point missing\n", stderr);
         goto cleanup;
      }
      trace_marker("AO46_AGX_SHADER_EXECUTION function-ready");

      trace_marker("AO46_AGX_SHADER_EXECUTION queue-create-begin");
      queue = [device newCommandQueue];
      if (!queue) {
         fputs("AO46_AGX_SHADER_EXECUTION queue creation failed\n", stderr);
         goto cleanup;
      }
      trace_marker("AO46_AGX_SHADER_EXECUTION queue-create-ready");

      trace_marker("AO46_AGX_SHADER_EXECUTION pipeline-create-begin");
      if (workload == TRACE_WORKLOAD_INDIRECT_COMMAND) {
         MTLComputePipelineDescriptor *indirect_descriptor =
            [[MTLComputePipelineDescriptor alloc] init];

         indirect_descriptor.computeFunction = function;
         indirect_descriptor.supportIndirectCommandBuffers = YES;
         trace_marker("AO46_AGX_SHADER_EXECUTION pipeline-slot=indirect-command");
         indirect_command_pipeline =
            [device newComputePipelineStateWithDescriptor:indirect_descriptor
                                                   options:MTLPipelineOptionNone
                                                reflection:nil
                                                     error:&error];
         [indirect_descriptor release];
         if (!indirect_command_pipeline) {
            fprintf(stderr,
                    "AO46_AGX_SHADER_EXECUTION indirect-command pipeline failed: %s\n",
                    error ? error.localizedDescription.UTF8String : "unknown error");
            goto cleanup;
         }
         if (!indirect_command_pipeline.supportIndirectCommandBuffers) {
            fputs("AO46_AGX_SHADER_EXECUTION indirect-command capability rejected\n",
                  stderr);
            goto cleanup;
         }
      } else {
         trace_marker("AO46_AGX_SHADER_EXECUTION pipeline-slot=baseline");
         pipeline = [device newComputePipelineStateWithFunction:function error:&error];
         if (!pipeline) {
            fprintf(stderr, "AO46_AGX_SHADER_EXECUTION pipeline failed: %s\n",
                    error ? error.localizedDescription.UTF8String : "unknown error");
            goto cleanup;
         }
      }
      if (workload != TRACE_WORKLOAD_INDIRECT_COMMAND) {
         variant_function = [library newFunctionWithName:
             @"ao46_shader_execution_probe_variant"];
         if (!variant_function) {
            fputs("AO46_AGX_SHADER_EXECUTION variant entry point missing\n", stderr);
            goto cleanup;
         }
         trace_marker("AO46_AGX_SHADER_EXECUTION pipeline-slot=threadgroup");
         variant_pipeline = [device newComputePipelineStateWithFunction:variant_function
                                                                  error:&error];
         if (!variant_pipeline) {
           fprintf(stderr, "AO46_AGX_SHADER_EXECUTION variant pipeline failed: %s\n",
                   error ? error.localizedDescription.UTF8String : "unknown error");
           goto cleanup;
         }
         if (workload == TRACE_WORKLOAD_USC_STRESS) {
            if (device.maxThreadgroupMemoryLength < (8192 * sizeof(uint32_t))) {
               fputs("AO46_AGX_SHADER_EXECUTION insufficient threadgroup memory\n",
                     stderr);
               goto cleanup;
            }
            usc_stress_function = [library newFunctionWithName:
                @"ao46_shader_execution_probe_usc_stress"];
            if (!usc_stress_function) {
               fputs("AO46_AGX_SHADER_EXECUTION USC stress entry point missing\n",
                     stderr);
               goto cleanup;
            }
            trace_marker("AO46_AGX_SHADER_EXECUTION pipeline-slot=usc-stress");
            usc_stress_pipeline = [device newComputePipelineStateWithFunction:
                usc_stress_function error:&error];
            if (!usc_stress_pipeline) {
               fprintf(stderr,
                       "AO46_AGX_SHADER_EXECUTION USC stress pipeline failed: %s\n",
                       error ? error.localizedDescription.UTF8String : "unknown error");
               goto cleanup;
            }
         }
         two_buffer_function = [library newFunctionWithName:
             @"ao46_shader_execution_probe_two_buffers"];
         if (!two_buffer_function) {
            fputs("AO46_AGX_SHADER_EXECUTION two-buffer entry point missing\n", stderr);
            goto cleanup;
         }
         trace_marker("AO46_AGX_SHADER_EXECUTION pipeline-slot=two-buffers");
         two_buffer_pipeline = [device newComputePipelineStateWithFunction:two_buffer_function
                                                                       error:&error];
         if (!two_buffer_pipeline) {
            fprintf(stderr,
                    "AO46_AGX_SHADER_EXECUTION two-buffer pipeline failed: %s\n",
                    error ? error.localizedDescription.UTF8String : "unknown error");
            goto cleanup;
         }
      }
      trace_marker("AO46_AGX_SHADER_EXECUTION pipeline-create-ready");

      trace_marker("AO46_AGX_SHADER_EXECUTION buffer-create-begin");
      output = [device newBufferWithLength:sizeof(uint32_t)
                                   options:MTLResourceStorageModeShared];
      if (!output || !output.contents) {
         fputs("AO46_AGX_SHADER_EXECUTION output allocation failed\n", stderr);
         goto cleanup;
      }
      *(uint32_t *)output.contents = 0;
      if (workload == TRACE_WORKLOAD_TWO_BUFFERS ||
          workload == TRACE_WORKLOAD_PAIRED) {
         input = [device newBufferWithLength:sizeof(uint32_t)
                                    options:MTLResourceStorageModeShared];
         if (!input || !input.contents) {
            fputs("AO46_AGX_SHADER_EXECUTION input allocation failed\n", stderr);
            goto cleanup;
         }
         *(uint32_t *)input.contents = trace_output_value;
      }
      trace_marker("AO46_AGX_SHADER_EXECUTION buffer-create-ready");

      if (workload == TRACE_WORKLOAD_PAIRED) {
         trace_marker("AO46_AGX_SHADER_EXECUTION paired-begin");
         if (!submit_control(queue, pipeline, output, nil, 1,
                             "AO46_AGX_SHADER_EXECUTION paired-phase=warmup"))
            goto cleanup;

         *(uint32_t *)output.contents = 0;
         if (!submit_control(queue, pipeline, output, nil, 1,
                             "AO46_AGX_SHADER_EXECUTION paired-phase=baseline-a"))
            goto cleanup;

         *(uint32_t *)output.contents = 0;
         if (!submit_control(queue, pipeline, output, nil, 1,
                             "AO46_AGX_SHADER_EXECUTION paired-phase=baseline-b"))
            goto cleanup;

         *(uint32_t *)output.contents = 0;
         if (!submit_control(queue, two_buffer_pipeline, output, input, 1,
                             "AO46_AGX_SHADER_EXECUTION paired-phase=two-buffers-a"))
            goto cleanup;

         *(uint32_t *)output.contents = 0;
         if (!submit_control(queue, two_buffer_pipeline, output, input, 1,
                             "AO46_AGX_SHADER_EXECUTION paired-phase=two-buffers-b"))
            goto cleanup;

         *(uint32_t *)output.contents = 0;
         if (!submit_control(queue, variant_pipeline, output, nil, 32,
                             "AO46_AGX_SHADER_EXECUTION paired-phase=threadgroup-a"))
            goto cleanup;

         *(uint32_t *)output.contents = 0;
         if (!submit_control(queue, variant_pipeline, output, nil, 32,
                             "AO46_AGX_SHADER_EXECUTION paired-phase=threadgroup-b"))
            goto cleanup;

         trace_marker("AO46_AGX_SHADER_EXECUTION paired-ready");
         trace_marker("AO46_AGX_SHADER_EXECUTION complete result=0x6a46");
         status = 0;
         goto cleanup;
      }

      trace_marker("AO46_AGX_SHADER_EXECUTION command-buffer-create-begin");
      command_buffer = [queue commandBuffer];
      if (!command_buffer) {
         fputs("AO46_AGX_SHADER_EXECUTION command-buffer creation failed\n", stderr);
         goto cleanup;
      }
      trace_marker("AO46_AGX_SHADER_EXECUTION command-buffer-create-ready");

      trace_marker("AO46_AGX_SHADER_EXECUTION encoder-create-begin");
      encoder = [command_buffer computeCommandEncoder];
      if (!encoder) {
         fputs("AO46_AGX_SHADER_EXECUTION encoder creation failed\n", stderr);
         goto cleanup;
      }
      trace_marker("AO46_AGX_SHADER_EXECUTION encoder-create-ready");

      trace_marker("AO46_AGX_SHADER_EXECUTION pipeline-bind-begin");
      [encoder setComputePipelineState:
          workload == TRACE_WORKLOAD_USC_STRESS ? usc_stress_pipeline :
          workload == TRACE_WORKLOAD_INDIRECT_COMMAND ? indirect_command_pipeline :
          workload == TRACE_WORKLOAD_THREADGROUP ? variant_pipeline :
          workload == TRACE_WORKLOAD_TWO_BUFFERS ? two_buffer_pipeline : pipeline];
      trace_marker("AO46_AGX_SHADER_EXECUTION pipeline-bind-ready");

      trace_marker("AO46_AGX_SHADER_EXECUTION resource-bind-begin");
      [encoder setBuffer:output offset:0 atIndex:0];
      if (workload == TRACE_WORKLOAD_TWO_BUFFERS)
         [encoder setBuffer:input offset:0 atIndex:1];
      trace_marker("AO46_AGX_SHADER_EXECUTION resource-bind-ready");

      trace_marker("AO46_AGX_SHADER_EXECUTION dispatch-begin");
      [encoder dispatchThreads:MTLSizeMake(
          (workload == TRACE_WORKLOAD_THREADGROUP ||
           workload == TRACE_WORKLOAD_USC_STRESS) ? 32 : 1, 1, 1)
          threadsPerThreadgroup:MTLSizeMake(
              (workload == TRACE_WORKLOAD_THREADGROUP ||
               workload == TRACE_WORKLOAD_USC_STRESS) ? 32 : 1, 1, 1)];
      [encoder endEncoding];
      encoder = nil;
      trace_marker("AO46_AGX_SHADER_EXECUTION dispatch-ready");

      trace_marker("AO46_AGX_SHADER_EXECUTION commit-begin");
      [command_buffer commit];
      trace_marker("AO46_AGX_SHADER_EXECUTION commit-ready");

      trace_marker("AO46_AGX_SHADER_EXECUTION completion-wait-begin");
      [command_buffer waitUntilCompleted];
      if (command_buffer.status != MTLCommandBufferStatusCompleted ||
          *(const uint32_t *)output.contents != trace_output_value) {
         fputs("AO46_AGX_SHADER_EXECUTION output verification failed\n", stderr);
         goto cleanup;
      }
      trace_marker("AO46_AGX_SHADER_EXECUTION completion-wait-ready");
      trace_marker("AO46_AGX_SHADER_EXECUTION complete result=0x6a46");
      status = 0;

cleanup:
      [encoder release];
      [input release];
      [output release];
      [queue release];
      [variant_pipeline release];
      [usc_stress_pipeline release];
      [indirect_command_pipeline release];
      [pipeline release];
      [two_buffer_pipeline release];
      [variant_function release];
      [function release];
      [two_buffer_function release];
      [usc_stress_function release];
      [library release];
      [device release];
   }

   dlclose(agx_profile_bundle);
   return status;
}
