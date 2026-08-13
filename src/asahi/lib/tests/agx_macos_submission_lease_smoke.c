/*
 * Copyright 2026 Khronos_AppleICDs contributors
 * SPDX-License-Identifier: MIT
 */

#include "agx_macos_submission_lease.h"
#include "agx_macos_queue.h"

#include <pthread.h>
#include <stdio.h>
#include <string.h>

int
main(void)
{
   static const struct agx_macos_submit_descriptor_observed descriptor = {
      .header0 = 2,
      .header1 = 1,
      .completion_tokens = {0x0102030405060708ull, 0x1112131415161718ull},
   };
   static const struct agx_macos_submit_descriptor_observed queued_descriptor = {
      .header0 = 2,
      .header1 = 1,
      .completion_tokens = {0x2122232425262728ull, 0x3132333435363738ull},
   };
   const struct agx_macos_submission_range ranges[] = {
      {0x1000, 0x80},
      {0x2080, 0x80},
      {0x1080, 0x40},
   };
   const struct agx_macos_submission_range invalid_ranges[] = {
      {0x1000, 0x80},
      {0x21ff, 2},
   };
   uint8_t mapped_bytes[0x100] = {0};
   struct agx_macos_device_session session = {
      .device = {.connection = (io_connect_t)1},
      .profile = AGX_MACOS_DEVICE_PROFILE_T6040_G16S_USC3,
      .state = AGX_MACOS_DEVICE_SESSION_STATE_CONFIGURED,
      .api_configured = true,
      .api_generation = 7,
   };
   struct agx_macos_bo_set set = {
      .session = &session,
      .api_generation = 7,
      .entries = {
         {
            .connection = (io_connect_t)1,
            .gpu_va = 0x1000,
            .cpu = mapped_bytes,
            .size = 0x100,
            .handle = 7,
            .managed_by_set = true,
         },
         {
            .connection = (io_connect_t)1,
            .gpu_va = 0x2000,
            .size = 0x100,
            .handle = 8,
            .managed_by_set = true,
         },
      },
      .initialized = true,
   };
   struct agx_macos_submission_lease lease = {0};
   struct agx_macos_submission_lease ordered_leases[2] = {0};
   struct agx_macos_notification_queue queue = {
      .connection = (io_connect_t)1,
      .data_queue = (IODataQueueMemory *)(uintptr_t)1,
      .notification_port = (mach_port_t)1,
      .id = 4,
      .api_generation = 7,
      .release_state = AGX_MACOS_NOTIFICATION_QUEUE_ACTIVE,
   };
   struct agx_macos_device_session stale_session;
   struct agx_macos_bo_mapping mapping = {0};
   struct agx_macos_bo_mapping copied_mapping = {0};
   uint8_t carrier[AGX_MACOS_SUBMISSION_CARRIER_AUXILIARY_OFFSET +
                   AGX_MACOS_SUBMISSION_CARRIER_EXTENDED_READABLE_PREFIX] = {0};
   uint8_t queued_carrier[AGX_MACOS_SUBMISSION_CARRIER_AUXILIARY_OFFSET +
                          AGX_MACOS_SUBMISSION_CARRIER_EXTENDED_READABLE_PREFIX] = {0};
   const uint64_t opaque_pointer_slot = UINT64_C(0x1ff800000);
   struct agx_macos_completion_record_observed completion = {
      .token = descriptor.completion_tokens[1],
   };
   bool complete = false;

   if (pthread_mutex_init(&set.lock, NULL) != 0) {
      fputs("AGX_MACOS_SUBMISSION_LEASE_SMOKE lock initialization failed\n",
            stderr);
      return 1;
   }

   stale_session = session;
   stale_session.api_generation++;

   ++set.api_generation;
   if (agx_macos_submission_lease_init(
          &lease, &set, 4, &descriptor, sizeof(descriptor), NULL, 0) !=
       kIOReturnBadArgument) {
      fputs("AGX_MACOS_SUBMISSION_LEASE_SMOKE admitted a stale empty lease\n",
            stderr);
      pthread_mutex_destroy(&set.lock);
      return 1;
   }
   --set.api_generation;

   if (agx_macos_bo_set_map_range(&set, 7, 0x10, 0x20, &mapping) !=
          KERN_SUCCESS ||
       mapping.token == 0 || set.entries[0].cpu_map_count != 1) {
      fputs("AGX_MACOS_SUBMISSION_LEASE_SMOKE map admission failed\n", stderr);
      pthread_mutex_destroy(&set.lock);
      return 1;
   }
   copied_mapping = mapping;
   copied_mapping.size--;
   if (agx_macos_bo_set_unmap_range(&set, &copied_mapping) !=
          kIOReturnBadArgument ||
       !mapping.active || set.entries[0].cpu_map_count != 1 ||
       agx_macos_bo_set_unmap_range(&set, &mapping) != KERN_SUCCESS ||
       agx_macos_bo_set_unmap_range(&set, &copied_mapping) !=
          kIOReturnBadArgument ||
       set.entries[0].cpu_map_count != 0) {
      fputs("AGX_MACOS_SUBMISSION_LEASE_SMOKE map capability rejection failed\n",
            stderr);
      pthread_mutex_destroy(&set.lock);
      return 1;
   }

   completion.token = 0;
   memcpy(carrier, &descriptor, sizeof(descriptor));
   memcpy(carrier + AGX_MACOS_SUBMISSION_CARRIER_AUXILIARY_OFFSET +
             AGX_MACOS_SUBMISSION_CARRIER_OPAQUE_POINTER_SLOT_OFFSET,
          &opaque_pointer_slot, sizeof(opaque_pointer_slot));
   memcpy(queued_carrier, &queued_descriptor, sizeof(queued_descriptor));
   memcpy(queued_carrier + AGX_MACOS_SUBMISSION_CARRIER_AUXILIARY_OFFSET +
             AGX_MACOS_SUBMISSION_CARRIER_OPAQUE_POINTER_SLOT_OFFSET,
          &opaque_pointer_slot, sizeof(opaque_pointer_slot));

   if (agx_macos_submission_lease_init_from_carrier(
          &lease, &set, 4, carrier, sizeof(descriptor),
          carrier + AGX_MACOS_SUBMISSION_CARRIER_AUXILIARY_OFFSET - 1,
          AGX_MACOS_SUBMISSION_CARRIER_MIN_READABLE_PREFIX, ranges,
          sizeof(ranges) / sizeof(ranges[0])) != kIOReturnBadArgument ||
       lease.active || set.entries[0].in_flight_count != 0 ||
       set.entries[1].in_flight_count != 0 ||
       agx_macos_submission_lease_init(
          &lease, &set, 4, &descriptor, sizeof(descriptor), ranges,
          sizeof(ranges) / sizeof(ranges[0])) != KERN_SUCCESS ||
       set.entries[0].in_flight_count != 1 ||
       set.entries[1].in_flight_count != 1 ||
       lease.has_carrier_snapshot ||
       agx_macos_submission_lease_mark_submitted(&lease) !=
          kIOReturnNotPermitted ||
       agx_macos_submission_lease_release(&lease) != KERN_SUCCESS ||
       set.entries[0].in_flight_count != 0 ||
       set.entries[1].in_flight_count != 0 ||
       agx_macos_submission_lease_init_from_carrier(
          &lease, &set, 4, carrier, sizeof(descriptor),
          carrier + AGX_MACOS_SUBMISSION_CARRIER_AUXILIARY_OFFSET,
          AGX_MACOS_SUBMISSION_CARRIER_EXTENDED_READABLE_PREFIX - 1, ranges,
          sizeof(ranges) / sizeof(ranges[0])) != kIOReturnBadArgument ||
       agx_macos_submission_lease_init_from_carrier(
          &lease, &set, 4, carrier, sizeof(descriptor),
          carrier + AGX_MACOS_SUBMISSION_CARRIER_AUXILIARY_OFFSET,
          AGX_MACOS_SUBMISSION_CARRIER_EXTENDED_READABLE_PREFIX, ranges,
          sizeof(ranges) / sizeof(ranges[0])) != KERN_SUCCESS ||
       !lease.has_carrier_snapshot ||
       lease.carrier_snapshot.observation.submission.queue_id != 4 ||
       lease.carrier_snapshot.opaque_pointer_slot != opaque_pointer_slot ||
       set.entries[0].in_flight_count != 1 ||
       set.entries[1].in_flight_count != 1 ||
       agx_macos_submission_lease_init(
          &lease, &set, 4, &descriptor, sizeof(descriptor), ranges,
          sizeof(ranges) / sizeof(ranges[0])) != kIOReturnBadArgument ||
       agx_macos_notification_queue_bind_lease(&stale_session, &queue, &lease) !=
          kIOReturnBadArgument ||
       agx_macos_notification_queue_bind_lease(&session, &queue, &lease) !=
          KERN_SUCCESS ||
       !lease.queue_lease_bound ||
       lease.queue_connection != queue.connection ||
       lease.bound_queue != &queue || lease.queue_submission_serial != 0 ||
       lease.bound_queue_id != queue.id ||
       lease.bound_queue_api_generation != queue.api_generation ||
       lease.fence.queue_api_generation != queue.api_generation ||
       queue.next_submission_serial != 0 ||
       queue.retired_submission_serial != 0 ||
       queue.in_flight_submission_count != 0 ||
       agx_macos_notification_queue_bind_lease(&session, &queue, &lease) !=
          kIOReturnBadArgument) {
      fputs("AGX_MACOS_SUBMISSION_LEASE_SMOKE queue bind failed\n", stderr);
      (void)agx_macos_submission_lease_release(&lease);
      pthread_mutex_destroy(&set.lock);
      return 1;
   }

   lease.fence.completed_token_mask = 1;
   if (agx_macos_submission_lease_mark_submitted(&lease) !=
       kIOReturnNotPermitted) {
      fputs("AGX_MACOS_SUBMISSION_LEASE_SMOKE admitted a partial fence\n",
            stderr);
      lease.fence.completed_token_mask = 0;
      (void)agx_macos_submission_lease_release(&lease);
      pthread_mutex_destroy(&set.lock);
      return 1;
   }
   lease.fence.completed_token_mask = 0;

   if (agx_macos_submission_lease_mark_submitted(&lease) != KERN_SUCCESS ||
       lease.queue_submission_serial != 1 ||
       queue.next_submission_serial != 1 ||
       queue.in_flight_submission_count != 1 ||
       agx_macos_submission_lease_mark_submitted(&lease) !=
          kIOReturnBadArgument ||
       agx_macos_submission_lease_release(&lease) != kIOReturnBusy ||
       agx_macos_submission_lease_record_completion(
          &lease, 4, &completion, &complete) != kIOReturnBadArgument ||
       complete || set.entries[0].in_flight_count != 1 ||
       set.entries[1].in_flight_count != 1) {
      fputs("AGX_MACOS_SUBMISSION_LEASE_SMOKE invalid-token rejection failed\n",
            stderr);
      (void)agx_macos_submission_lease_release(&lease);
      pthread_mutex_destroy(&set.lock);
      return 1;
   }

   completion.token = descriptor.completion_tokens[0];
   if (agx_macos_submission_lease_abandon_after_device_loss(&lease) !=
       kIOReturnBusy) {
      fputs("AGX_MACOS_SUBMISSION_LEASE_SMOKE abandoned a live session\n",
            stderr);
      ++set.api_generation;
      (void)agx_macos_submission_lease_abandon_after_device_loss(&lease);
      --set.api_generation;
      pthread_mutex_destroy(&set.lock);
      return 1;
   }

   ++set.api_generation;
   if (agx_macos_submission_lease_abandon_after_device_loss(&lease) !=
          KERN_SUCCESS ||
       lease.active || set.entries[0].in_flight_count != 0 ||
       set.entries[1].in_flight_count != 0 ||
       queue.retired_submission_serial != 1 ||
       queue.in_flight_submission_count != 0) {
      fputs("AGX_MACOS_SUBMISSION_LEASE_SMOKE device-loss retirement failed\n",
            stderr);
      --set.api_generation;
      pthread_mutex_destroy(&set.lock);
      return 1;
   }
   --set.api_generation;

   if (agx_macos_submission_lease_record_completion(
          &lease, 4, &completion, &complete) != kIOReturnBadArgument ||
       agx_macos_submission_lease_init_from_carrier(
          &lease, &set, 4, carrier, sizeof(descriptor),
          carrier + AGX_MACOS_SUBMISSION_CARRIER_AUXILIARY_OFFSET,
          AGX_MACOS_SUBMISSION_CARRIER_EXTENDED_READABLE_PREFIX, ranges,
          sizeof(ranges) / sizeof(ranges[0])) != KERN_SUCCESS ||
       agx_macos_notification_queue_bind_lease(&session, &queue, &lease) !=
          KERN_SUCCESS ||
       agx_macos_submission_lease_mark_submitted(&lease) != KERN_SUCCESS) {
      fputs("AGX_MACOS_SUBMISSION_LEASE_SMOKE post-loss admission failed\n",
            stderr);
      pthread_mutex_destroy(&set.lock);
      return 1;
   }

   completion.token = descriptor.completion_tokens[1];

   ++set.api_generation;
   if (agx_macos_submission_lease_record_completion(
          &lease, 4, &completion, &complete) != kIOReturnBadArgument) {
      fputs("AGX_MACOS_SUBMISSION_LEASE_SMOKE completed a stale lease\n",
            stderr);
      (void)agx_macos_submission_lease_abandon_after_device_loss(&lease);
      --set.api_generation;
      pthread_mutex_destroy(&set.lock);
      return 1;
   }
   --set.api_generation;

   if (agx_macos_submission_lease_record_completion(
          &lease, 5, &completion, &complete) != kIOReturnBadArgument ||
       complete ||
       agx_macos_submission_lease_record_completion(
          &lease, 4, &completion, &complete) != KERN_SUCCESS ||
       complete || set.entries[0].in_flight_count != 1 ||
       set.entries[1].in_flight_count != 1) {
      fputs("AGX_MACOS_SUBMISSION_LEASE_SMOKE admission failed\n", stderr);
      pthread_mutex_destroy(&set.lock);
      return 1;
   }

   completion.token = descriptor.completion_tokens[0];
   if (agx_macos_submission_lease_record_completion(
          &lease, 4, &completion, &complete) != KERN_SUCCESS ||
       !complete || lease.active || lease.has_carrier_snapshot ||
       lease.queue_lease_bound ||
       set.entries[0].in_flight_count != 0 ||
       set.entries[1].in_flight_count != 0 ||
       queue.retired_submission_serial != 2 ||
       queue.in_flight_submission_count != 0 ||
       agx_macos_submission_lease_init_from_carrier(
          &lease, &set, 4, carrier, sizeof(descriptor),
          carrier + AGX_MACOS_SUBMISSION_CARRIER_AUXILIARY_OFFSET,
          AGX_MACOS_SUBMISSION_CARRIER_EXTENDED_READABLE_PREFIX, invalid_ranges,
          sizeof(invalid_ranges) / sizeof(invalid_ranges[0])) !=
          kIOReturnNotFound ||
       set.entries[0].in_flight_count != 0 ||
       set.entries[1].in_flight_count != 0) {
      fputs("AGX_MACOS_SUBMISSION_LEASE_SMOKE completion or rollback failed\n",
            stderr);
      pthread_mutex_destroy(&set.lock);
      return 1;
   }

   if (agx_macos_submission_lease_init_from_carrier(
          &ordered_leases[0], &set, 4, carrier, sizeof(descriptor),
          carrier + AGX_MACOS_SUBMISSION_CARRIER_AUXILIARY_OFFSET,
          AGX_MACOS_SUBMISSION_CARRIER_EXTENDED_READABLE_PREFIX, ranges,
          sizeof(ranges) / sizeof(ranges[0])) != KERN_SUCCESS ||
       agx_macos_submission_lease_init_from_carrier(
          &ordered_leases[1], &set, 4, queued_carrier,
          sizeof(queued_descriptor),
          queued_carrier + AGX_MACOS_SUBMISSION_CARRIER_AUXILIARY_OFFSET,
          AGX_MACOS_SUBMISSION_CARRIER_EXTENDED_READABLE_PREFIX, ranges,
          sizeof(ranges) / sizeof(ranges[0])) != KERN_SUCCESS ||
       agx_macos_notification_queue_bind_lease(
          &session, &queue, &ordered_leases[0]) != KERN_SUCCESS ||
       agx_macos_notification_queue_bind_lease(
          &session, &queue, &ordered_leases[1]) != KERN_SUCCESS ||
       agx_macos_submission_lease_mark_submitted(&ordered_leases[0]) !=
          KERN_SUCCESS ||
       agx_macos_submission_lease_mark_submitted(&ordered_leases[1]) !=
          KERN_SUCCESS ||
       ordered_leases[0].queue_submission_serial != 3 ||
       ordered_leases[1].queue_submission_serial != 4 ||
       queue.next_submission_serial != 4 ||
       queue.in_flight_submission_count != 2) {
      fputs("AGX_MACOS_SUBMISSION_LEASE_SMOKE queue serial admission failed\n",
            stderr);
      pthread_mutex_destroy(&set.lock);
      return 1;
   }

   completion.token = queued_descriptor.completion_tokens[0];
   if (agx_macos_submission_lease_record_completion(
          &ordered_leases[1], 4, &completion, &complete) != KERN_SUCCESS ||
       complete) {
      fputs("AGX_MACOS_SUBMISSION_LEASE_SMOKE second queue lease did not start\n",
            stderr);
      pthread_mutex_destroy(&set.lock);
      return 1;
   }

   completion.token = queued_descriptor.completion_tokens[1];
   if (agx_macos_submission_lease_record_completion(
          &ordered_leases[1], 4, &completion, &complete) !=
          kIOReturnNotPermitted ||
       complete || !ordered_leases[1].active ||
       queue.retired_submission_serial != 2 ||
       queue.in_flight_submission_count != 2) {
      fputs("AGX_MACOS_SUBMISSION_LEASE_SMOKE accepted out-of-order retirement\n",
            stderr);
      pthread_mutex_destroy(&set.lock);
      return 1;
   }

   completion.token = descriptor.completion_tokens[0];
   if (agx_macos_submission_lease_record_completion(
          &ordered_leases[0], 4, &completion, &complete) != KERN_SUCCESS ||
       complete) {
      fputs("AGX_MACOS_SUBMISSION_LEASE_SMOKE first queue lease did not start\n",
            stderr);
      pthread_mutex_destroy(&set.lock);
      return 1;
   }

   completion.token = descriptor.completion_tokens[1];
   if (agx_macos_submission_lease_record_completion(
          &ordered_leases[0], 4, &completion, &complete) != KERN_SUCCESS ||
       !complete || ordered_leases[0].active ||
       queue.retired_submission_serial != 3 ||
       queue.in_flight_submission_count != 1) {
      fputs("AGX_MACOS_SUBMISSION_LEASE_SMOKE first queue retirement failed\n",
            stderr);
      pthread_mutex_destroy(&set.lock);
      return 1;
   }

   completion.token = queued_descriptor.completion_tokens[1];
   if (agx_macos_submission_lease_record_completion(
          &ordered_leases[1], 4, &completion, &complete) != KERN_SUCCESS ||
       !complete || ordered_leases[1].active ||
       queue.retired_submission_serial != 4 ||
       queue.in_flight_submission_count != 0) {
      fputs("AGX_MACOS_SUBMISSION_LEASE_SMOKE ordered queue retirement failed\n",
            stderr);
      pthread_mutex_destroy(&set.lock);
      return 1;
   }

   if (agx_macos_submission_lease_init_from_carrier(
          &lease, &set, 4, carrier, sizeof(descriptor),
          carrier + AGX_MACOS_SUBMISSION_CARRIER_AUXILIARY_OFFSET,
          AGX_MACOS_SUBMISSION_CARRIER_EXTENDED_READABLE_PREFIX, ranges,
          sizeof(ranges) / sizeof(ranges[0])) != KERN_SUCCESS ||
       agx_macos_notification_queue_bind_fence(&session, &queue, &lease.fence) !=
          KERN_SUCCESS ||
       lease.queue_lease_bound ||
       agx_macos_submission_lease_mark_submitted(&lease) !=
          kIOReturnNotPermitted ||
       agx_macos_submission_lease_release(&lease) != KERN_SUCCESS) {
      fputs("AGX_MACOS_SUBMISSION_LEASE_SMOKE fence-only bind bypassed lease gate\n",
            stderr);
      pthread_mutex_destroy(&set.lock);
      return 1;
   }

   if (agx_macos_submission_lease_init_from_carrier(
          &lease, &set, 4, carrier, sizeof(descriptor),
          carrier + AGX_MACOS_SUBMISSION_CARRIER_AUXILIARY_OFFSET,
          AGX_MACOS_SUBMISSION_CARRIER_EXTENDED_READABLE_PREFIX, ranges,
          sizeof(ranges) / sizeof(ranges[0])) != KERN_SUCCESS ||
       agx_macos_notification_queue_bind_lease(&session, &queue, &lease) !=
          KERN_SUCCESS) {
      fputs("AGX_MACOS_SUBMISSION_LEASE_SMOKE stale-submit setup failed\n",
            stderr);
      (void)agx_macos_submission_lease_release(&lease);
      pthread_mutex_destroy(&set.lock);
      return 1;
   }

   lease.carrier_snapshot.auxiliary_prefix[0] ^= 1;
   if (agx_macos_submission_lease_mark_submitted(&lease) !=
       kIOReturnNotPermitted) {
      fputs("AGX_MACOS_SUBMISSION_LEASE_SMOKE submitted mutated evidence\n",
            stderr);
      lease.carrier_snapshot.auxiliary_prefix[0] ^= 1;
      (void)agx_macos_submission_lease_abandon_after_device_loss(&lease);
      pthread_mutex_destroy(&set.lock);
      return 1;
   }
   lease.carrier_snapshot.auxiliary_prefix[0] ^= 1;

   ++set.api_generation;
   if (agx_macos_submission_lease_mark_submitted(&lease) !=
       kIOReturnBadArgument) {
      fputs("AGX_MACOS_SUBMISSION_LEASE_SMOKE submitted a stale lease\n",
            stderr);
      --set.api_generation;
      (void)agx_macos_submission_lease_abandon_after_device_loss(&lease);
      pthread_mutex_destroy(&set.lock);
      return 1;
   }
   --set.api_generation;

   if (agx_macos_submission_lease_release(&lease) != KERN_SUCCESS) {
      fputs("AGX_MACOS_SUBMISSION_LEASE_SMOKE stale-submit cleanup failed\n",
            stderr);
      pthread_mutex_destroy(&set.lock);
      return 1;
   }

   pthread_mutex_destroy(&set.lock);
   puts("AGX_MACOS_SUBMISSION_LEASE_SMOKE complete");
   return 0;
}
