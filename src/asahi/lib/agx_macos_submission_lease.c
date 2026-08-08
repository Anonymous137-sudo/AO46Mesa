/*
 * Copyright 2026 Khronos_AppleICDs contributors
 * SPDX-License-Identifier: MIT
 */

#include "agx_macos_submission_lease.h"

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

   lease->bo_set = NULL;
   lease->carrier_snapshot =
      (struct agx_macos_submission_carrier_extended_snapshot){0};
   lease->has_carrier_snapshot = false;
   lease->active = false;
   lease->state = AGX_MACOS_SUBMISSION_LEASE_NONE;
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

   if (!out_lease || out_lease->active || !bo_set || !bo_set->initialized ||
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
   if (!lease || !lease->active || !lease->bo_set ||
       lease->state != AGX_MACOS_SUBMISSION_LEASE_ADMITTED)
      return kIOReturnBadArgument;

   if (!lease->has_carrier_snapshot)
      return kIOReturnNotPermitted;

   lease->state = AGX_MACOS_SUBMISSION_LEASE_IN_FLIGHT;
   return KERN_SUCCESS;
}

kern_return_t
agx_macos_submission_lease_abandon_after_device_loss(
   struct agx_macos_submission_lease *lease)
{
   if (!lease || !lease->active ||
       lease->state != AGX_MACOS_SUBMISSION_LEASE_IN_FLIGHT) {
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
   if (!lease || !lease->active || !record || !out_complete ||
       lease->state != AGX_MACOS_SUBMISSION_LEASE_IN_FLIGHT) {
      return kIOReturnBadArgument;
   }

   *out_complete = false;
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
