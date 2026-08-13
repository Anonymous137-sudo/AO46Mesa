/*
 * Copyright 2026 Khronos_AppleICDs contributors
 * SPDX-License-Identifier: MIT
 */

#include "agx_macos_submission_build.h"
#include "agx_macos_queue.h"

#include <pthread.h>
#include <stdio.h>
#include <string.h>

struct owned_range {
   uint64_t gpu_va;
   uint64_t byte_size;
};

static bool
owns_range(void *context, uint64_t gpu_va, uint64_t byte_size)
{
   const struct owned_range *ranges = context;

   for (unsigned i = 0; i < 4; ++i) {
      uint64_t end = ranges[i].gpu_va + ranges[i].byte_size;

      if (gpu_va >= ranges[i].gpu_va && gpu_va + byte_size >= gpu_va &&
          gpu_va + byte_size <= end) {
         return true;
      }
   }

   return false;
}

static uint64_t
read_u64(const uint8_t *bytes, size_t offset)
{
   uint64_t value;

   memcpy(&value, bytes + offset, sizeof(value));
   return value;
}

int
main(void)
{
   const struct owned_range ranges[] = {
      {0x100000000ull, 0x10000},
      {0x200000000ull, 0x10000},
      {0x300000000ull, 0x10000},
      {0x400000000ull, 0x10000},
   };
   const struct agx_macos_resource_binding consumer_bindings[] = {
      {0x100003000ull, 0x1000},
      {0x200005000ull, 0x1000},
      {0x300005000ull, 0x1000},
      {0x400007000ull, 0x1000},
   };
   const struct agx_macos_resource_binding compute_bindings[] = {
      {0x100001000ull, 0x1000},
      {0x200003000ull, 0x1000},
   };
   const struct agx_macos_submit_descriptor_observed descriptor = {
      .header0 = AGX_MACOS_SUBMISSION_DESCRIPTOR_HEADER0,
      .header1 = AGX_MACOS_SUBMISSION_DESCRIPTOR_HEADER1,
      .completion_tokens = {0x1111111122222222ull, 0x3333333344444444ull},
   };
   struct agx_macos_resource_record_layout layout = {0};
   struct agx_macos_submission_carrier_extended_snapshot snapshot = {0};
   struct agx_macos_trap4_submission_preview preview = {0};
   struct agx_macos_device_session session = {
      .device = {.connection = (io_connect_t)1},
      .profile = AGX_MACOS_DEVICE_PROFILE_T6040_G16S_USC3,
      .state = AGX_MACOS_DEVICE_SESSION_STATE_CONFIGURED,
      .api_configured = true,
      .api_generation = 9,
   };
   struct agx_macos_bo_set bo_set = {
      .session = &session,
      .api_generation = 9,
      .entries = {
         {
            .connection = (io_connect_t)1,
            .gpu_va = 0x100000000ull,
            .size = 0x10000,
            .handle = 1,
            .managed_by_set = true,
         },
         {
            .connection = (io_connect_t)1,
            .gpu_va = 0x200000000ull,
            .size = 0x10000,
            .handle = 2,
            .managed_by_set = true,
         },
         {
            .connection = (io_connect_t)1,
            .gpu_va = 0x300000000ull,
            .size = 0x10000,
            .handle = 3,
            .managed_by_set = true,
         },
      },
      .initialized = true,
   };
   struct agx_macos_notification_queue notification_queue = {
      .connection = (io_connect_t)1,
      .data_queue = (IODataQueueMemory *)(uintptr_t)1,
      .notification_port = (mach_port_t)1,
      .id = 7,
      .api_generation = 9,
      .release_state = AGX_MACOS_NOTIFICATION_QUEUE_ACTIVE,
   };
   struct agx_macos_submission_package package = {0};
   struct agx_macos_completion_record_observed completion = {0};
   bool complete;
   const struct agx_macos_submission_range package_ranges[] = {
      {0x300000800ull, 0x1bb0},
   };
   uint8_t record[0x1bb0];
   uint8_t package_record[0x1bb0];
   uint8_t carrier[AGX_MACOS_TRAP4_SUBMISSION_CARRIER_BYTES] = {0};
   const uint64_t opaque_pointer_slot = UINT64_C(0x1ff800000);

   memset(record, 0xcc, sizeof(record));
   if (!agx_macos_resource_record_layout_get(
          AGX_MACOS_RESOURCE_RECORD_BLIT_CONSUMER, &layout) ||
       layout.binding_count != 4 || layout.minimum_record_size != 0x30 ||
       layout.binding_offsets[2] != 0x20 ||
       agx_macos_resource_record_encode(
          record, sizeof(record), AGX_MACOS_RESOURCE_RECORD_BLIT_CONSUMER,
          consumer_bindings, 4, owns_range, (void *)ranges) != KERN_SUCCESS ||
       read_u64(record, 0x0) != consumer_bindings[0].gpu_va ||
       read_u64(record, 0x8) != consumer_bindings[1].gpu_va ||
       read_u64(record, 0x20) != consumer_bindings[2].gpu_va ||
       read_u64(record, 0x28) != consumer_bindings[3].gpu_va ||
       record[0x10] != 0xcc) {
      fputs("AGX_MACOS_SUBMISSION_BUILD_SMOKE blit resource table failed\n", stderr);
      return 1;
   }

   if (!agx_macos_resource_record_layout_get(AGX_MACOS_RESOURCE_RECORD_COMPUTE,
                                               &layout) ||
       layout.minimum_record_size != 0x1bb0 || layout.binding_offsets[0] != 0x1ba0 ||
       agx_macos_resource_record_encode(record, sizeof(record),
                                        AGX_MACOS_RESOURCE_RECORD_COMPUTE,
                                        compute_bindings, 2, owns_range,
                                        (void *)ranges) != KERN_SUCCESS ||
       read_u64(record, 0x1ba0) != compute_bindings[0].gpu_va ||
       read_u64(record, 0x1ba8) != compute_bindings[1].gpu_va) {
      fputs("AGX_MACOS_SUBMISSION_BUILD_SMOKE compute resource table failed\n", stderr);
      return 1;
   }

   record[0] = 0x5a;
   if (agx_macos_resource_record_encode(record, sizeof(record),
                                        AGX_MACOS_RESOURCE_RECORD_BLIT_PRODUCER,
                                        consumer_bindings, 2, NULL,
                                        (void *)ranges) == KERN_SUCCESS ||
       record[0] != 0x5a) {
      fputs("AGX_MACOS_SUBMISSION_BUILD_SMOKE accepted unowned table\n", stderr);
      return 1;
   }

   memcpy(carrier, &descriptor, sizeof(descriptor));
   memset(carrier + AGX_MACOS_SUBMISSION_CARRIER_AUXILIARY_OFFSET, 0xa5,
          AGX_MACOS_SUBMISSION_CARRIER_EXTENDED_READABLE_PREFIX);
   memcpy(carrier + AGX_MACOS_SUBMISSION_CARRIER_AUXILIARY_OFFSET +
             AGX_MACOS_SUBMISSION_CARRIER_OPAQUE_POINTER_SLOT_OFFSET,
          &opaque_pointer_slot, sizeof(opaque_pointer_slot));
   if (!agx_macos_submission_carrier_extended_snapshot_capture(
          7, carrier, sizeof(descriptor),
          carrier + AGX_MACOS_SUBMISSION_CARRIER_AUXILIARY_OFFSET,
          AGX_MACOS_SUBMISSION_CARRIER_EXTENDED_READABLE_PREFIX, &snapshot) ||
       !agx_macos_trap4_submission_preview_build(&snapshot, &preview) ||
       !agx_macos_trap4_submission_preview_is_intact(&preview) ||
       agx_macos_trap4_submission_preview_can_submit(&preview) ||
       preview.arguments[0] != 7 || preview.arguments[1] != sizeof(descriptor) ||
       preview.arguments[3] - preview.arguments[2] !=
          AGX_MACOS_SUBMISSION_CARRIER_AUXILIARY_OFFSET) {
      fputs("AGX_MACOS_SUBMISSION_BUILD_SMOKE Trap4 preview failed\n", stderr);
      return 1;
   }

   preview.carrier[AGX_MACOS_SUBMISSION_CARRIER_AUXILIARY_OFFSET] ^= 1;
   if (agx_macos_trap4_submission_preview_is_intact(&preview)) {
      fputs("AGX_MACOS_SUBMISSION_BUILD_SMOKE accepted mutated carrier\n", stderr);
      return 1;
   }

   if (pthread_mutex_init(&bo_set.lock, NULL) != 0) {
      fputs("AGX_MACOS_SUBMISSION_BUILD_SMOKE package lock initialization failed\n",
            stderr);
      return 1;
   }

   memset(package_record, 0xcd, sizeof(package_record));
   if (agx_macos_submission_package_admit(
          &package, &bo_set, 7, carrier, sizeof(descriptor),
          carrier + AGX_MACOS_SUBMISSION_CARRIER_AUXILIARY_OFFSET,
          AGX_MACOS_SUBMISSION_CARRIER_EXTENDED_READABLE_PREFIX, NULL, 0,
          package_record, sizeof(package_record), AGX_MACOS_RESOURCE_RECORD_COMPUTE,
          compute_bindings, 2) != kIOReturnBadArgument || package.active ||
       package_record[0] != 0xcd) {
      fputs("AGX_MACOS_SUBMISSION_BUILD_SMOKE accepted an unpinned record\n",
            stderr);
      pthread_mutex_destroy(&bo_set.lock);
      return 1;
   }

   if (agx_macos_submission_package_admit(
          &package, &bo_set, 7, carrier, sizeof(descriptor),
          carrier + AGX_MACOS_SUBMISSION_CARRIER_AUXILIARY_OFFSET,
          AGX_MACOS_SUBMISSION_CARRIER_EXTENDED_READABLE_PREFIX,
          package_ranges, 1,
          package_record, sizeof(package_record), AGX_MACOS_RESOURCE_RECORD_COMPUTE,
          compute_bindings, 2) != KERN_SUCCESS ||
       !package.active || !agx_macos_submission_package_is_intact(&package) ||
       agx_macos_trap4_submission_preview_can_submit(&package.preview) ||
       bo_set.entries[0].in_flight_count != 1 ||
       bo_set.entries[1].in_flight_count != 1 ||
       bo_set.entries[2].in_flight_count != 1 ||
       read_u64(package_record, 0x1ba0) != compute_bindings[0].gpu_va ||
       read_u64(package_record, 0x1ba8) != compute_bindings[1].gpu_va ||
       package_record[0] != 0xcd) {
      fputs("AGX_MACOS_SUBMISSION_BUILD_SMOKE package admission failed\n", stderr);
      (void)agx_macos_submission_package_release(&package);
      pthread_mutex_destroy(&bo_set.lock);
      return 1;
   }

   package.preview.carrier[AGX_MACOS_SUBMISSION_CARRIER_AUXILIARY_OFFSET] ^= 1;
   if (agx_macos_submission_package_is_intact(&package)) {
      fputs("AGX_MACOS_SUBMISSION_BUILD_SMOKE accepted a mutated package\n",
            stderr);
      package.preview.carrier[AGX_MACOS_SUBMISSION_CARRIER_AUXILIARY_OFFSET] ^= 1;
      (void)agx_macos_submission_package_release(&package);
      pthread_mutex_destroy(&bo_set.lock);
      return 1;
   }
   package.preview.carrier[AGX_MACOS_SUBMISSION_CARRIER_AUXILIARY_OFFSET] ^= 1;

   /* This simulates only the already validated notification/fence handoff.
    * It does not submit the captured carrier or call an IOKit method. */
   if (agx_macos_submission_package_bind_notification_queue(
          &session, &notification_queue, &package) != KERN_SUCCESS ||
       !package.lease.queue_lease_bound ||
       agx_macos_submission_package_bind_notification_queue(
          &session, &notification_queue, &package) != kIOReturnBadArgument ||
       agx_macos_submission_package_mark_submitted(&package) != KERN_SUCCESS ||
       !agx_macos_submission_package_is_intact(&package) ||
       agx_macos_submission_package_release(&package) != kIOReturnBusy) {
      fputs("AGX_MACOS_SUBMISSION_BUILD_SMOKE package submission gate failed\n",
            stderr);
      pthread_mutex_destroy(&bo_set.lock);
      return 1;
   }

   completion.token = UINT64_C(0x9999999900000000);
   complete = true;
   if (agx_macos_submission_package_record_completion(
          &package, 7, &completion, &complete) != kIOReturnBadArgument ||
       complete ||
       !package.active || bo_set.entries[0].in_flight_count != 1) {
      fputs("AGX_MACOS_SUBMISSION_BUILD_SMOKE package accepted foreign completion\n",
            stderr);
      pthread_mutex_destroy(&bo_set.lock);
      return 1;
   }

   completion.token = descriptor.completion_tokens[1];
   complete = true;
   if (agx_macos_submission_package_record_completion(
          &package, 7, &completion, &complete) != KERN_SUCCESS || complete ||
       !package.active || bo_set.entries[0].in_flight_count != 1 ||
       bo_set.entries[1].in_flight_count != 1 ||
       bo_set.entries[2].in_flight_count != 1) {
      fputs("AGX_MACOS_SUBMISSION_BUILD_SMOKE first completion retired package\n",
            stderr);
      pthread_mutex_destroy(&bo_set.lock);
      return 1;
   }

   completion.token = descriptor.completion_tokens[1];
   complete = true;
   if (agx_macos_submission_package_record_completion(
          &package, 7, &completion, &complete) != kIOReturnBadArgument ||
       complete || !package.active || bo_set.entries[0].in_flight_count != 1 ||
       bo_set.entries[1].in_flight_count != 1 ||
       bo_set.entries[2].in_flight_count != 1) {
      fputs("AGX_MACOS_SUBMISSION_BUILD_SMOKE accepted duplicate completion\n",
            stderr);
      pthread_mutex_destroy(&bo_set.lock);
      return 1;
   }

   completion.token = descriptor.completion_tokens[0];
   complete = false;
   if (agx_macos_submission_package_record_completion(
          &package, 7, &completion, &complete) != KERN_SUCCESS || !complete ||
       package.active || bo_set.entries[0].in_flight_count != 0 ||
       bo_set.entries[1].in_flight_count != 0 ||
       bo_set.entries[2].in_flight_count != 0) {
      fputs("AGX_MACOS_SUBMISSION_BUILD_SMOKE package completion retirement failed\n",
            stderr);
      pthread_mutex_destroy(&bo_set.lock);
      return 1;
   }

   pthread_mutex_destroy(&bo_set.lock);

   puts("AGX_MACOS_SUBMISSION_BUILD_SMOKE complete");
   return 0;
}
