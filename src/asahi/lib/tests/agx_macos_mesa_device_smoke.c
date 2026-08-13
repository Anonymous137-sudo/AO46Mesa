/*
 * Copyright 2026 Khronos_AppleICDs contributors
 * SPDX-License-Identifier: MIT
 */

#include "agx_macos_mesa_device.h"
#include "agx_macos_queue.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "agx_bo.h"
#include "agx_submit.h"

int
main(int argc, char **argv)
{
   struct agx_device device = {0};

   if (agx_macos_mesa_device_init(&device, NULL, NULL, NULL) ||
       agx_macos_mesa_device_is_current(&device) ||
       agx_macos_mesa_device_capabilities(&device) != 0) {
      fputs("AGX_MACOS_MESA_DEVICE_SMOKE accepted incomplete ownership\n", stderr);
      return 1;
   }

   if (!agx_macos_mesa_device_destroy(&device))
      return 1;

   if (!getenv("AGX_MACOS_EXPERIMENTAL_MESA_DEVICE")) {
      puts("AGX_MACOS_MESA_DEVICE_SMOKE skipped; set "
           "AGX_MACOS_EXPERIMENTAL_MESA_DEVICE=1 to run");
      return 0;
   }

   if (argc != 1) {
      fputs("AGX_MACOS_MESA_DEVICE_SMOKE received invalid argv\n", stderr);
      return 1;
   }

   struct agx_macos_device_session session = {0};
   struct agx_macos_bo_set bo_set = {0};
   struct agx_macos_notification_queue notification_queue = {
      .connection = IO_OBJECT_NULL,
      .notification_port = MACH_PORT_NULL,
   };
   struct agx_macos_bo native_bos[2] = {
      {.connection = IO_OBJECT_NULL},
      {.connection = IO_OBJECT_NULL},
   };
   struct agx_bo *bo = NULL;
   struct agx_bo *repeated_bo = NULL;
   struct agx_bo *dynamic_bo = NULL;
   struct agx_bo *mesa_lookup = NULL;
   struct agx_macos_mesa_submission_package package = {0};
   unsigned repeated_allocation_count = 1;
   int first_adoption_errno = 0;
   int repeated_adoption_errno = 0;
   int status = 1;

   if (agx_macos_device_session_open(&session) !=
          AGX_MACOS_DEVICE_SESSION_READY ||
       agx_macos_device_session_configure_traced_api(&session, argv[0]) !=
          KERN_SUCCESS ||
       agx_macos_bo_set_init(&bo_set, &session) != KERN_SUCCESS) {
      fputs("AGX_MACOS_MESA_DEVICE_SMOKE failed to prepare direct ownership\n",
            stderr);
      goto out;
   }

   if (agx_macos_bo_set_create_at_least(
          &bo_set, AGX_MACOS_BO_STORAGE_SHARED,
          AGX_MACOS_BO_SHARED_64K_SIZE, 1,
          &native_bos[0]) != KERN_SUCCESS ||
       agx_macos_bo_set_create_at_least(
          &bo_set, AGX_MACOS_BO_STORAGE_SHARED,
          AGX_MACOS_BO_SHARED_64K_SIZE, 1,
          &native_bos[1]) != KERN_SUCCESS) {
      fputs("AGX_MACOS_MESA_DEVICE_SMOKE direct multi-BO bootstrap failed\n",
            stderr);
      goto out;
   }

   /* Direct allocations are established before Mesa takes ownership. This is
    * the same dependency order used by the native BO-set probe. */
   if (!agx_macos_mesa_device_init(&device, &session, &bo_set, NULL)) {
      fputs("AGX_MACOS_MESA_DEVICE_SMOKE failed to adopt direct ownership\n",
            stderr);
      goto out;
   }

   if (agx_macos_mesa_device_capabilities(&device) !=
           (AGX_MACOS_MESA_DEVICE_CAP_BO_ALLOC |
            AGX_MACOS_MESA_DEVICE_CAP_BO_MAP |
            AGX_MACOS_MESA_DEVICE_CAP_FIXED_BO_BIND)) {
      fputs("AGX_MACOS_MESA_DEVICE_SMOKE reported unvalidated capabilities\n",
            stderr);
      goto out;
   }

   if (device.ops.submit || !device.ops.submit_info) {
      fputs("AGX_MACOS_MESA_DEVICE_SMOKE exposed a Linux submit path\n",
            stderr);
      goto out;
   }

   if (!device.ops.is_screen_ready || device.ops.is_screen_ready(&device)) {
      fputs("AGX_MACOS_MESA_DEVICE_SMOKE admitted an incomplete screen\n",
            stderr);
      goto out;
   }

   /* The Asahi linker allocates every shader as EXEC|LOW_VA. This must fail
    * at the macOS UABI gate, before an unsupported selector-9 allocation can
    * claim a misleading GPU VA or Mesa creates a partial screen. */
   if (device.ops.bo_alloc(&device, AGX_MACOS_BO_SHARED_64K_SIZE, 0,
                           AGX_BO_EXEC | AGX_BO_LOW_VA) != NULL ||
       device.ops.bo_alloc(&device, AGX_MACOS_BO_SHARED_64K_SIZE, 0,
                           AGX_BO_LOW_VA) != NULL) {
      fputs("AGX_MACOS_MESA_DEVICE_SMOKE admitted unsupported low-VA BOs\n",
            stderr);
      goto out;
   }

   /* Adopt the deliberately non-64 KiB-aligned VA first. This proves Mesa
    * ownership does not accidentally depend on allocator order. */
   errno = 0;
   bo = agx_macos_mesa_device_adopt_bo(&device, &native_bos[1],
                                        AGX_BO_WRITEBACK);
   first_adoption_errno = errno;
   if (bo)
      native_bos[1] = (struct agx_macos_bo){.connection = IO_OBJECT_NULL};
   errno = 0;
   repeated_bo = agx_macos_mesa_device_adopt_bo(&device, &native_bos[0],
                                                 AGX_BO_WRITEBACK);
   repeated_adoption_errno = errno;
   if (repeated_bo)
      native_bos[0] =
         (struct agx_macos_bo){.connection = IO_OBJECT_NULL};
   if (bo)
      mesa_lookup = agx_lookup_bo(&device, bo->handle);
   if (!bo || bo->dev != &device || !bo->va || !bo->_map ||
       bo->size != AGX_MACOS_BO_SHARED_64K_SIZE || bo->va->addr == 0 ||
       mesa_lookup != bo) {
      fprintf(stderr,
              "AGX_MACOS_MESA_DEVICE_SMOKE Mesa BO allocation mismatch "
              "bo=%p lookup=%p ref=%d handle=%u native=%u errno=%d/%d\n",
              (void *)bo, (void *)mesa_lookup,
              bo ? p_atomic_read(&bo->refcnt) : -1, bo ? bo->handle : 0,
              bo ? bo->uapi_handle : 0, first_adoption_errno,
              repeated_adoption_errno);
      goto out;
   }

   if (!repeated_bo || repeated_bo == bo ||
       repeated_bo->handle == bo->handle || !repeated_bo->va ||
       repeated_bo->va->addr == bo->va->addr ||
       repeated_bo->uapi_handle == bo->uapi_handle) {
      fprintf(stderr,
              "AGX_MACOS_MESA_DEVICE_SMOKE native adoption identities first="
              "%u/%u/%#llx second=%u/%u/%#llx native-second=%u/%#llx errno=%d\n",
              bo ? bo->handle : 0, bo ? bo->uapi_handle : 0,
              bo && bo->va ? (unsigned long long)bo->va->addr : 0,
              repeated_bo ? repeated_bo->handle : 0,
              repeated_bo ? repeated_bo->uapi_handle : 0,
              repeated_bo && repeated_bo->va
                 ? (unsigned long long)repeated_bo->va->addr
                 : 0,
              native_bos[0].handle,
              (unsigned long long)native_bos[0].gpu_va,
              repeated_adoption_errno);
      if (repeated_bo == bo)
         repeated_bo = NULL;
      goto out;
   }
   repeated_allocation_count = 2;

   /* The native screen factory needs to grow past its bootstrap pair before
    * it attaches queue/completion state. Exercise that Mesa-requested third
    * BO here, while the direct allocator's multi-BO contract is live. */
   dynamic_bo = device.ops.bo_alloc(&device, AGX_MACOS_BO_SHARED_64K_SIZE,
                                    AIL_PAGESIZE, AGX_BO_WRITEBACK);
   if (!dynamic_bo || dynamic_bo == bo || dynamic_bo == repeated_bo ||
       !dynamic_bo->va || dynamic_bo->uapi_handle == bo->uapi_handle ||
       dynamic_bo->uapi_handle == repeated_bo->uapi_handle ||
       dynamic_bo->va->addr == bo->va->addr ||
       dynamic_bo->va->addr == repeated_bo->va->addr ||
       p_atomic_read(&dynamic_bo->refcnt) != 1) {
      fputs("AGX_MACOS_MESA_DEVICE_SMOKE dynamic multi-BO growth failed\n",
            stderr);
      goto out;
   }
   repeated_allocation_count = 3;

   if (agx_macos_notification_queue_create(&session, &notification_queue) !=
          KERN_SUCCESS ||
       !agx_macos_mesa_device_attach_notification_queue(
          &device, &notification_queue) ||
       device.queue_id != 0 ||
       agx_macos_mesa_device_capabilities(&device) !=
          (AGX_MACOS_MESA_DEVICE_CAP_BO_ALLOC |
           AGX_MACOS_MESA_DEVICE_CAP_BO_MAP |
           AGX_MACOS_MESA_DEVICE_CAP_FIXED_BO_BIND |
           AGX_MACOS_MESA_DEVICE_CAP_COMPLETION_SYNC) ||
       agx_macos_mesa_device_missing_screen_capabilities(&device) !=
          (AGX_MACOS_MESA_DEVICE_CAP_VM_BIND |
           AGX_MACOS_MESA_DEVICE_CAP_SUBMIT |
           AGX_MACOS_MESA_DEVICE_CAP_OBJECT_BIND |
           AGX_MACOS_MESA_DEVICE_CAP_LOW_VA_BIND |
           AGX_MACOS_MESA_DEVICE_CAP_EXECUTABLE_BO |
           AGX_MACOS_MESA_DEVICE_CAP_SHADER_CODE_ADMISSION)) {
      fputs("AGX_MACOS_MESA_DEVICE_SMOKE notification queue attachment failed\n",
            stderr);
      goto out;
   }

   {
      uint32_t sync = 0;

      if (agx_macos_mesa_sync_create(&device, 1u, &sync) != 0 || sync == 0 ||
          agx_macos_mesa_sync_wait(&device, &sync, 1, 0, 1u, NULL) != 0 ||
          agx_macos_mesa_sync_destroy(&device, sync) != 0) {
         fputs("AGX_MACOS_MESA_DEVICE_SMOKE notification-backed sync failed\n",
               stderr);
         goto out;
      }
   }

   {
      struct drm_asahi_gem_bind_op bind = {
         .flags = DRM_ASAHI_BIND_READ | DRM_ASAHI_BIND_WRITE,
         .handle = bo->uapi_handle,
         .range = bo->size,
         .addr = bo->va->addr,
      };

      if (agx_bo_bind(&device, bo, bind.addr, bind.range, bind.offset,
                      bind.flags) != 0) {
         fputs("AGX_MACOS_MESA_DEVICE_SMOKE generic bind rejected fixed native mapping\n",
               stderr);
         goto out;
      }

      bind.range -= AIL_PAGESIZE;
      if (agx_bo_bind(&device, bo, bind.addr, bind.range, bind.offset,
                      bind.flags) != -EINVAL) {
         fputs("AGX_MACOS_MESA_DEVICE_SMOKE generic bind accepted partial native mapping\n",
               stderr);
         goto out;
      }

      if (agx_bo_bind(&device, bo, bo->va->addr, bo->size, 0,
                      DRM_ASAHI_BIND_UNBIND) != -ENOTSUP) {
         fputs("AGX_MACOS_MESA_DEVICE_SMOKE generic bind exposed unverified unbind\n",
               stderr);
         goto out;
      }
   }

   {
      static const unsigned char pattern[] = {
         0x4b, 0x68, 0x72, 0x6f, 0x6e, 0x6f, 0x73, 0x00,
      };
      unsigned char *cpu = agx_bo_map(bo);

      if (cpu != bo->_map) {
         fputs("AGX_MACOS_MESA_DEVICE_SMOKE Mesa BO map mismatch\n", stderr);
         goto out;
      }

      memcpy(cpu, pattern, sizeof(pattern));
      if (memcmp(cpu, pattern, sizeof(pattern)) != 0) {
         fputs("AGX_MACOS_MESA_DEVICE_SMOKE Mesa BO CPU write mismatch\n",
               stderr);
         goto out;
      }
   }

   {
      const struct agx_macos_submit_descriptor_observed descriptor = {
         .header0 = AGX_MACOS_SUBMISSION_DESCRIPTOR_HEADER0,
         .header1 = AGX_MACOS_SUBMISSION_DESCRIPTOR_HEADER1,
         .completion_tokens = {
            UINT64_C(0x1111111122222222), UINT64_C(0x3333333344444444),
         },
      };
      const uint64_t opaque_pointer_slot = UINT64_C(0x1ff800000);
      struct agx_macos_mesa_encoder_range encoder_range = {
         .bo = bo,
         .begin = agx_bo_map(bo),
         .end = (uint8_t *)agx_bo_map(bo) + 0x10,
      };
      struct agx_macos_mesa_encoder_range encoder_ranges[2] = {
         {
            .bo = bo,
            .begin = agx_bo_map(bo),
            .end = (uint8_t *)agx_bo_map(bo) + 0x10,
         },
         {
            .bo = bo,
            .begin = (uint8_t *)agx_bo_map(bo) + 0x20,
            .end = (uint8_t *)agx_bo_map(bo) + 0x30,
         },
      };
      struct agx_macos_mesa_bo_range command_range = {0};
      const struct agx_macos_mesa_bo_range resource_ranges[] = {
         {.bo = repeated_bo, .offset = 0x1000, .size = 0x1000},
         {.bo = dynamic_bo, .offset = 0x2000, .size = 0x1000},
      };
      struct agx_macos_mesa_bo_range invalid_record_range = {
         .bo = bo,
         .offset = bo->size,
         .size = 0x10,
      };
      uint8_t carrier[AGX_MACOS_TRAP4_SUBMISSION_CARRIER_BYTES] = {0};
      uint64_t encoded_source;
      uint64_t encoded_destination;

      memset(agx_bo_map(bo), 0xcd, 0x10);
      if (agx_macos_mesa_encoder_range_resolve(&device, &encoder_range,
                                                &command_range) != KERN_SUCCESS ||
          command_range.bo != bo || command_range.offset != 0 ||
          command_range.size != 0x10) {
         fputs("AGX_MACOS_MESA_DEVICE_SMOKE encoder range resolution failed\n",
               stderr);
         goto out;
      }

      encoder_range.end = encoder_range.begin;
      if (agx_macos_mesa_encoder_range_resolve(&device, &encoder_range,
                                                &command_range) !=
          kIOReturnBadArgument) {
         fputs("AGX_MACOS_MESA_DEVICE_SMOKE accepted an empty encoder range\n",
               stderr);
         goto out;
      }
      encoder_range.end = (uint8_t *)agx_bo_map(bo) + 0x10;

      memcpy(carrier, &descriptor, sizeof(descriptor));
      memset(carrier + AGX_MACOS_SUBMISSION_CARRIER_AUXILIARY_OFFSET, 0xa5,
             AGX_MACOS_SUBMISSION_CARRIER_EXTENDED_READABLE_PREFIX);
      memcpy(carrier + AGX_MACOS_SUBMISSION_CARRIER_AUXILIARY_OFFSET +
                AGX_MACOS_SUBMISSION_CARRIER_OPAQUE_POINTER_SLOT_OFFSET,
             &opaque_pointer_slot, sizeof(opaque_pointer_slot));

      if (agx_macos_mesa_submission_package_admit_encoders(
             &package, &device, 7, carrier, sizeof(descriptor),
             carrier + AGX_MACOS_SUBMISSION_CARRIER_AUXILIARY_OFFSET,
             AGX_MACOS_SUBMISSION_CARRIER_EXTENDED_READABLE_PREFIX,
             encoder_ranges, 2, &invalid_record_range,
             AGX_MACOS_RESOURCE_RECORD_BLIT_PRODUCER, resource_ranges, 2) !=
             kIOReturnBadArgument ||
          package.active || ((uint8_t *)agx_bo_map(bo))[0] != 0xcd) {
         fputs("AGX_MACOS_MESA_DEVICE_SMOKE accepted an unpinned multi-stream record\n",
               stderr);
         goto out;
      }

      encoder_ranges[1].end = encoder_ranges[1].begin;
      if (agx_macos_mesa_submission_package_admit_encoders(
             &package, &device, 7, carrier, sizeof(descriptor),
             carrier + AGX_MACOS_SUBMISSION_CARRIER_AUXILIARY_OFFSET,
             AGX_MACOS_SUBMISSION_CARRIER_EXTENDED_READABLE_PREFIX,
             encoder_ranges, 2, &command_range,
             AGX_MACOS_RESOURCE_RECORD_BLIT_PRODUCER, resource_ranges, 2) !=
             kIOReturnBadArgument ||
          package.active || ((uint8_t *)agx_bo_map(bo))[0] != 0xcd) {
         fputs("AGX_MACOS_MESA_DEVICE_SMOKE accepted an invalid encoder in a multi-stream package\n",
               stderr);
         goto out;
      }
      encoder_ranges[1].end = (uint8_t *)agx_bo_map(bo) + 0x30;

      if (agx_macos_mesa_submission_package_admit_encoders(
             &package, &device, 7, carrier, sizeof(descriptor),
             carrier + AGX_MACOS_SUBMISSION_CARRIER_AUXILIARY_OFFSET,
             AGX_MACOS_SUBMISSION_CARRIER_EXTENDED_READABLE_PREFIX,
             encoder_ranges, 2, &command_range,
             AGX_MACOS_RESOURCE_RECORD_BLIT_PRODUCER, resource_ranges, 2) !=
             KERN_SUCCESS ||
          !agx_macos_mesa_submission_package_is_intact(&package) ||
          package.bo_reference_count != 3 ||
          p_atomic_read(&bo->refcnt) != 2 ||
          p_atomic_read(&repeated_bo->refcnt) != 2 ||
          p_atomic_read(&dynamic_bo->refcnt) != 2) {
         fputs("AGX_MACOS_MESA_DEVICE_SMOKE Mesa multi-stream admission failed\n",
               stderr);
         goto out;
      }

      memcpy(&encoded_source, agx_bo_map(bo), sizeof(encoded_source));
      memcpy(&encoded_destination, (uint8_t *)agx_bo_map(bo) + 0x8,
             sizeof(encoded_destination));
      if (encoded_source != repeated_bo->va->addr + resource_ranges[0].offset ||
          encoded_destination != dynamic_bo->va->addr + resource_ranges[1].offset ||
          agx_macos_mesa_submission_package_release(&package) != KERN_SUCCESS ||
          package.active || p_atomic_read(&bo->refcnt) != 1 ||
          p_atomic_read(&repeated_bo->refcnt) != 1 ||
          p_atomic_read(&dynamic_bo->refcnt) != 1) {
         fputs("AGX_MACOS_MESA_DEVICE_SMOKE Mesa submission ownership failed\n",
               stderr);
         goto out;
      }
   }

   {
      struct agx_submit_attachment attachments[] = {
         {.gpu_va = bo->va->addr + 0x1800, .size_B = 0x100},
      };
      uint32_t batch_sync = 0;
      uint32_t timeline_sync = 0;
      struct agx_submit_sync syncs[] = {
         {
            .type = AGX_SUBMIT_SYNC_BINARY,
         },
         {
            .type = AGX_SUBMIT_SYNC_TIMELINE,
            .timeline_value = 1,
         },
      };
      struct agx_submit_encoder encoders[] = {
         {
            .type = AGX_SUBMIT_ENCODER_COMPUTE,
            .bo = bo,
            .gpu_va = bo->va->addr + 0x1000,
            .size_B = 0x100,
         },
         {
            .type = AGX_SUBMIT_ENCODER_RENDER,
            .bo = bo,
            .gpu_va = bo->va->addr + 0x1400,
         },
      };
      struct agx_submit_object objects[] = {
         {
            .type = AGX_SUBMIT_OBJECT_TIMESTAMPS,
            .handle = 1,
            .bo = bo,
         },
      };
      struct agx_submit_resource resources[] = {
         {
            .bo = bo,
            .gpu_va = bo->va->addr,
            .size_B = bo->size,
         },
      };
      struct agx_submit_command commands[] = {
         {
            .type = AGX_SUBMIT_COMMAND_COMPUTE,
            .compute = {
               .cdm_ctrl_stream_base = bo->va->addr + 0x1000,
               .cdm_ctrl_stream_end = bo->va->addr + 0x1100,
               .sampler_heap = bo->va->addr + 0x1200,
               .helper = {.data = bo->va->addr + 0x1300},
               .timestamps = {
                  .start = {.object = 1},
                  .end = {.object = 1},
               },
            },
         },
         {
            .type = AGX_SUBMIT_COMMAND_RENDER,
            .render = {
               .vdm_ctrl_stream_base = bo->va->addr + 0x1400,
               .sampler_heap = bo->va->addr + 0x1500,
               .vertex_helper = {.data = bo->va->addr + 0x1600},
               .fragment_helper = {.data = bo->va->addr + 0x1700},
               .attachments = attachments,
               .attachment_count = 1,
            },
         },
      };
      struct agx_submit_info info = {
         .queue_id = notification_queue.id,
         .syncs = syncs,
         .out_sync_count = 2,
         .commands = commands,
         .command_count = 2,
         .encoders = encoders,
         .encoder_count = 2,
         .objects = objects,
         .object_count = 1,
         .resources = resources,
         .resource_count = 1,
      };

      if (agx_macos_mesa_sync_create(&device, 0, &batch_sync) != 0 ||
          agx_macos_mesa_sync_create(&device, 0, &timeline_sync) != 0) {
         fputs("AGX_MACOS_MESA_DEVICE_SMOKE sync creation failed\n", stderr);
         goto out;
      }
      syncs[0].handle = batch_sync;
      syncs[1].handle = timeline_sync;

      if (device.ops.submit_info(&device, NULL, NULL) != -EINVAL) {
         fputs("AGX_MACOS_MESA_DEVICE_SMOKE accepted missing Asahi work\n",
               stderr);
         goto out;
      }

      if (device.ops.submit_info(&device, &info, NULL) != -ENOTSUP) {
         fputs("AGX_MACOS_MESA_DEVICE_SMOKE rejected owned Asahi work\n",
               stderr);
         goto out;
      }

      commands[0].compute.timestamps.start.offset_B = bo->size;
      if (device.ops.submit_info(&device, &info, NULL) != -EINVAL) {
         fputs("AGX_MACOS_MESA_DEVICE_SMOKE accepted an out-of-range timestamp\n",
               stderr);
         goto out;
      }
      commands[0].compute.timestamps.start.offset_B = 0;

      info.queue_id++;
      if (device.ops.submit_info(&device, &info, NULL) != -ENOTSUP) {
         fputs("AGX_MACOS_MESA_DEVICE_SMOKE interpreted an unavailable "
               "execution queue\n",
               stderr);
         goto out;
      }
      --info.queue_id;

      commands[1].render.depth.base = bo->va->addr + bo->size;
      if (device.ops.submit_info(&device, &info, NULL) != -EINVAL) {
         fputs("AGX_MACOS_MESA_DEVICE_SMOKE accepted a foreign depth resource\n",
               stderr);
         goto out;
      }
      commands[1].render.depth.base = bo->va->addr + 0x1900;

      commands[1].render.isp_scissor_base = bo->va->addr + bo->size;
      if (device.ops.submit_info(&device, &info, NULL) != -EINVAL) {
         fputs("AGX_MACOS_MESA_DEVICE_SMOKE accepted a foreign ISP resource\n",
               stderr);
         goto out;
      }
      commands[1].render.isp_scissor_base = bo->va->addr + 0x1a00;

      syncs[1].handle = batch_sync;
      if (device.ops.submit_info(&device, &info, NULL) != -EINVAL) {
         fputs("AGX_MACOS_MESA_DEVICE_SMOKE accepted duplicate output syncs\n",
               stderr);
         goto out;
      }
      syncs[1].handle = timeline_sync;

      attachments[0].gpu_va = bo->va->addr + bo->size;
      if (device.ops.submit_info(&device, &info, NULL) != -EINVAL) {
         fputs("AGX_MACOS_MESA_DEVICE_SMOKE accepted foreign Asahi resource\n",
               stderr);
         goto out;
      }

      attachments[0].gpu_va = bo->va->addr + 0x1800;
      encoders[0].bo = NULL;
      if (device.ops.submit_info(&device, &info, NULL) != -EINVAL) {
         fputs("AGX_MACOS_MESA_DEVICE_SMOKE accepted foreign Asahi encoder\n",
               stderr);
         goto out;
      }
      encoders[0].bo = bo;

      syncs[0].handle = UINT32_MAX;
      if (device.ops.submit_info(&device, &info, NULL) != -EINVAL) {
         fputs("AGX_MACOS_MESA_DEVICE_SMOKE accepted a foreign Asahi sync\n",
               stderr);
         goto out;
      }
      syncs[0].handle = batch_sync;
      syncs[0].timeline_value = 1;
      if (device.ops.submit_info(&device, &info, NULL) != -EINVAL) {
         fputs("AGX_MACOS_MESA_DEVICE_SMOKE accepted a binary timeline value\n",
               stderr);
         goto out;
      }
      syncs[0].timeline_value = 0;

      objects[0].bo = NULL;
      if (device.ops.submit_info(&device, &info, NULL) != -EINVAL) {
         fputs("AGX_MACOS_MESA_DEVICE_SMOKE accepted a foreign timestamp BO\n",
               stderr);
         goto out;
      }
      objects[0].bo = bo;

      resources[0].bo = NULL;
      if (device.ops.submit_info(&device, &info, NULL) != -EINVAL) {
         fputs("AGX_MACOS_MESA_DEVICE_SMOKE accepted a foreign batch BO\n",
               stderr);
         goto out;
      }
      resources[0].bo = bo;

      if (agx_macos_mesa_sync_destroy(&device, timeline_sync) != 0 ||
          agx_macos_mesa_sync_destroy(&device, batch_sync) != 0) {
         fputs("AGX_MACOS_MESA_DEVICE_SMOKE sync cleanup failed\n", stderr);
         goto out;
      }
   }

   printf("AGX_MACOS_MESA_DEVICE_SMOKE record=%u resource_ranges=2 repeated=%u gpu=%#llx size=%zu\n",
          bo->handle, repeated_allocation_count,
          (unsigned long long)bo->va->addr, bo->size);
   status = 0;

out:
   if (package.active)
      (void)agx_macos_mesa_submission_package_release(&package);

   if (dynamic_bo)
      agx_bo_unreference(&device, dynamic_bo);
   if (repeated_bo)
      agx_bo_unreference(&device, repeated_bo);
   if (bo)
      agx_bo_unreference(&device, bo);

   if (!agx_macos_mesa_device_destroy(&device)) {
      fputs("AGX_MACOS_MESA_DEVICE_SMOKE direct device remained busy\n", stderr);
      status = 1;
   }
   if (bo_set.initialized && agx_macos_bo_set_cleanup(&bo_set) != KERN_SUCCESS) {
      fputs("AGX_MACOS_MESA_DEVICE_SMOKE direct BO cleanup failed\n", stderr);
      status = 1;
   }
   if (notification_queue.connection != IO_OBJECT_NULL &&
       agx_macos_notification_queue_destroy(&notification_queue) !=
          KERN_SUCCESS) {
      fputs("AGX_MACOS_MESA_DEVICE_SMOKE notification queue cleanup failed\n",
            stderr);
      status = 1;
   }
   agx_macos_device_session_close(&session);

   if (status == 0)
      puts("AGX_MACOS_MESA_DEVICE_SMOKE complete");

   return status;
}
