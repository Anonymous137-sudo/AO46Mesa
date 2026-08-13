/*
 * Copyright 2026 Khronos_AppleICDs contributors
 * SPDX-License-Identifier: MIT
 */

#include "agx_macos_submission_lease.h"
#include "agx_macos_queue.h"

#include <string.h>

kern_return_t
agx_macos_submission_lease_release(struct agx_macos_submission_lease *lease)
{
   kern_return_t result;

   if (!lease || !lease->active || !lease->bo_set)
      return kIOReturnBadArgument;

   if (lease->state == AGX_MACOS_SUBMISSION_LEASE_IN_FLIGHT)
      return kIOReturnBusy;

   if (lease->state != AGX_MACOS_SUBMISSION_LEASE_ADMITTED &&
        lease->state != AGX_MACOS_SUBMISSION_LEASE_COMPLETE &&
        lease->state != AGX_MACOS_SUBMISSION_LEASE_ABANDONED) {
      return kIOReturnBadArgument;
   }

   while (lease->handle_count > 0) {
      uint32_t handle = lease->handles[lease->handle_count - 1];

      result = agx_macos_bo_set_release_submission(lease->bo_set, handle);
      if (result != KERN_SUCCESS)
         return result;

      --lease->handle_count;
   }

   *lease = (struct agx_macos_submission_lease){0};
   return KERN_SUCCESS;
}

kern_return_t
agx_macos_submission_lease_init(
   struct agx_macos_submission_lease *out_lease,
   struct agx_macos_bo_set *bo_set, uint32_t queue_id,
   const void *descriptor_bytes, size_t descriptor_size,
   const struct agx_macos_submission_range *ranges, uint32_t range_count)
{
   struct agx_macos_submission_fence fence;
   kern_return_t result = KERN_SUCCESS;

   if (!out_lease || out_lease->active || !bo_set ||
       !agx_macos_bo_set_is_current(bo_set, bo_set->session) ||
       range_count > AGX_MACOS_SUBMISSION_LEASE_MAX_RANGES ||
       (range_count && !ranges) ||
       !agx_macos_submission_fence_init(queue_id, descriptor_bytes,
                                        descriptor_size, &fence)) {
      return kIOReturnBadArgument;
   }

   *out_lease = (struct agx_macos_submission_lease){
      .fence = fence,
      .bo_set = bo_set,
      .state = AGX_MACOS_SUBMISSION_LEASE_ADMITTED,
      .active = true,
   };

   for (uint32_t i = 0; i < range_count; ++i) {
      struct agx_macos_bo bo = {.connection = IO_OBJECT_NULL};
      bool already_admitted = false;

      result = agx_macos_bo_set_lookup_gpu_va_range(
         bo_set, ranges[i].gpu_va, ranges[i].size, &bo);
      if (result != KERN_SUCCESS)
         break;

      for (uint32_t j = 0; j < out_lease->handle_count; ++j) {
         if (out_lease->handles[j] == bo.handle) {
            already_admitted = true;
            break;
         }
      }
      if (already_admitted)
         continue;

      result = agx_macos_bo_set_retain_submission(bo_set, bo.handle);
      if (result != KERN_SUCCESS)
         break;

      out_lease->handles[out_lease->handle_count++] = bo.handle;
   }

   if (result == KERN_SUCCESS)
      return KERN_SUCCESS;

   kern_return_t release_result = agx_macos_submission_lease_release(out_lease);
   if (release_result != KERN_SUCCESS)
      return release_result;

   memset(out_lease, 0, sizeof(*out_lease));
   return result;
}

kern_return_t
agx_macos_submission_lease_init_from_carrier(
   struct agx_macos_submission_lease *out_lease,
   struct agx_macos_bo_set *bo_set, uint32_t queue_id,
   const void *descriptor_bytes, size_t descriptor_size,
   const void *auxiliary_bytes, size_t auxiliary_readable_prefix,
   const struct agx_macos_submission_range *ranges, uint32_t range_count)
{
   struct agx_macos_submission_carrier_extended_snapshot snapshot;
   kern_return_t result;

   if (!out_lease || out_lease->active ||
       !agx_macos_submission_carrier_extended_snapshot_capture(
          queue_id, descriptor_bytes, descriptor_size, auxiliary_bytes,
          auxiliary_readable_prefix, &snapshot)) {
      return kIOReturnBadArgument;
   }

   result = agx_macos_submission_lease_init(
      out_lease, bo_set, queue_id, descriptor_bytes, descriptor_size, ranges,
      range_count);
   if (result != KERN_SUCCESS)
      return result;

   out_lease->carrier_snapshot = snapshot;
   out_lease->has_carrier_snapshot = true;
   return KERN_SUCCESS;
}

kern_return_t
agx_macos_submission_lease_mark_submitted(
   struct agx_macos_submission_lease *lease)
{
   kern_return_t result;

   if (!lease || !lease->active || !lease->bo_set ||
       lease->state != AGX_MACOS_SUBMISSION_LEASE_ADMITTED ||
       !agx_macos_bo_set_is_current(lease->bo_set, lease->bo_set->session))
      return kIOReturnBadArgument;

   if (!lease->has_carrier_snapshot || !lease->queue_lease_bound ||
       !lease->bound_queue || lease->queue_submission_serial != 0 ||
       lease->queue_connection == IO_OBJECT_NULL ||
       lease->bound_queue_id != lease->fence.observation.queue_id ||
       lease->bound_queue_api_generation == 0 ||
       lease->bound_queue_api_generation != lease->fence.queue_api_generation ||
       lease->fence.completed_token_mask != 0 ||
      !agx_macos_submission_carrier_extended_snapshot_is_intact(
          &lease->carrier_snapshot))
      return kIOReturnNotPermitted;

   result = agx_macos_notification_queue_admit_lease_submission(
      lease->bound_queue, lease);
   if (result != KERN_SUCCESS)
      return result;

   lease->state = AGX_MACOS_SUBMISSION_LEASE_IN_FLIGHT;
   return KERN_SUCCESS;
}

kern_return_t
agx_macos_submission_lease_abandon_after_device_loss(
   struct agx_macos_submission_lease *lease)
{
   if (!lease || !lease->active || !lease->bo_set ||
       lease->state != AGX_MACOS_SUBMISSION_LEASE_IN_FLIGHT) {
      return kIOReturnBadArgument;
   }

   /* A live BO set means the AGX session can still execute this lease. */
   if (agx_macos_bo_set_is_current(lease->bo_set, lease->bo_set->session))
      return kIOReturnBusy;

   if (agx_macos_notification_queue_abandon_lease_submission(
          lease->bound_queue, lease) != KERN_SUCCESS) {
      return kIOReturnBadArgument;
   }

   lease->state = AGX_MACOS_SUBMISSION_LEASE_ABANDONED;
   return agx_macos_submission_lease_release(lease);
}

kern_return_t
agx_macos_submission_lease_record_completion(
   struct agx_macos_submission_lease *lease, uint32_t completion_queue_id,
   const struct agx_macos_completion_record_observed *record,
   bool *out_complete)
{
   unsigned token_index;
   uint8_t token_bit;
   bool final_completion;

   if (!out_complete)
      return kIOReturnBadArgument;

   *out_complete = false;
   if (!lease || !lease->active || !record ||
       lease->state != AGX_MACOS_SUBMISSION_LEASE_IN_FLIGHT ||
       !lease->bo_set ||
       !agx_macos_bo_set_is_current(lease->bo_set, lease->bo_set->session)) {
      return kIOReturnBadArgument;
   }

   if (!lease->queue_lease_bound ||
       !lease->bound_queue || lease->queue_submission_serial == 0 ||
       lease->queue_connection == IO_OBJECT_NULL ||
       lease->bound_queue_id != completion_queue_id ||
       lease->bound_queue_id != lease->fence.observation.queue_id ||
       lease->bound_queue_api_generation == 0 ||
       lease->bound_queue_api_generation != lease->fence.queue_api_generation) {
      return kIOReturnBadArgument;
   }

   if (!agx_macos_submission_observation_matches_completion(
          &lease->fence.observation, completion_queue_id, record,
          &token_index)) {
      return kIOReturnBadArgument;
   }

   token_bit = 1u << token_index;
   if (lease->fence.completed_token_mask & token_bit)
      return kIOReturnBadArgument;

   final_completion =
      (lease->fence.completed_token_mask | token_bit) == UINT8_C(0x3);
   if (final_completion &&
       agx_macos_notification_queue_retire_lease_submission(
          lease->bound_queue, lease) != KERN_SUCCESS) {
      return kIOReturnNotPermitted;
   }

   if (!agx_macos_submission_fence_record_completion(
          &lease->fence, completion_queue_id, record)) {
      return kIOReturnBadArgument;
   }

   *out_complete = agx_macos_submission_fence_is_complete(&lease->fence);
   if (!*out_complete)
      return KERN_SUCCESS;

   lease->state = AGX_MACOS_SUBMISSION_LEASE_COMPLETE;
   return agx_macos_submission_lease_release(lease);
}
