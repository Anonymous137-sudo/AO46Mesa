/*
 * Copyright 2026 Khronos_AppleICDs contributors
 * SPDX-License-Identifier: MIT
 */

#include "agx_macos_submission_build.h"
#include "agx_macos_queue.h"

#include <IOKit/IOReturn.h>

#include <string.h>

static const struct agx_macos_resource_record_layout resource_layouts[] = {
   {
      .kind = AGX_MACOS_RESOURCE_RECORD_BLIT_PRODUCER,
      .binding_count = 2,
      .minimum_record_size = 0x10,
      .binding_offsets = {0x0, 0x8},
   },
   {
      .kind = AGX_MACOS_RESOURCE_RECORD_BLIT_CONSUMER,
      .binding_count = 4,
      .minimum_record_size = 0x30,
      .binding_offsets = {0x0, 0x8, 0x20, 0x28},
   },
   {
      .kind = AGX_MACOS_RESOURCE_RECORD_COMPUTE,
      .binding_count = 2,
      .minimum_record_size = 0x1bb0,
      .binding_offsets = {0x1ba0, 0x1ba8},
   },
};

static uint64_t
agx_macos_preview_fingerprint(const struct agx_macos_trap4_submission_preview *preview)
{
   uint64_t fingerprint = UINT64_C(0xcbf29ce484222325);

   for (size_t i = 0; i < sizeof(preview->carrier); ++i) {
      fingerprint ^= preview->carrier[i];
      fingerprint *= UINT64_C(0x100000001b3);
   }

   return fingerprint;
}

bool
agx_macos_resource_record_layout_get(
   enum agx_macos_resource_record_kind kind,
   struct agx_macos_resource_record_layout *out_layout)
{
   if (!out_layout)
      return false;

   for (unsigned i = 0; i < sizeof(resource_layouts) / sizeof(resource_layouts[0]);
        ++i) {
      if (resource_layouts[i].kind == kind) {
         *out_layout = resource_layouts[i];
         return true;
      }
   }

   return false;
}

static bool
agx_macos_resource_range_is_valid(const struct agx_macos_resource_binding *binding,
                                  agx_macos_resource_range_is_owned_fn range_is_owned,
                                  void *range_context)
{
   uint64_t end;

   if (!binding || !range_is_owned || binding->gpu_va == 0 ||
       binding->byte_size == 0)
      return false;

   end = binding->gpu_va + binding->byte_size;
   if (end < binding->gpu_va)
      return false;

   return range_is_owned(range_context, binding->gpu_va, binding->byte_size);
}

kern_return_t
agx_macos_resource_record_encode(
   uint8_t *record, size_t record_size,
   enum agx_macos_resource_record_kind kind,
   const struct agx_macos_resource_binding *bindings, uint32_t binding_count,
   agx_macos_resource_range_is_owned_fn range_is_owned, void *range_context)
{
   struct agx_macos_resource_record_layout layout;

   if (!record || !bindings || !agx_macos_resource_record_layout_get(kind, &layout) ||
       binding_count != layout.binding_count || record_size < layout.minimum_record_size) {
      return kIOReturnBadArgument;
   }

   /* Validate every address before mutating the command record. This keeps an
    * invalid resource table from becoming a partially encoded submission. */
   for (uint32_t i = 0; i < binding_count; ++i) {
      if (!agx_macos_resource_range_is_valid(&bindings[i], range_is_owned,
                                             range_context)) {
         return kIOReturnBadArgument;
      }
   }

   for (uint32_t i = 0; i < binding_count; ++i)
      memcpy(record + layout.binding_offsets[i], &bindings[i].gpu_va,
             sizeof(bindings[i].gpu_va));

   return KERN_SUCCESS;
}

static bool
agx_macos_resource_range_is_owned_by_bo_set(void *context, uint64_t gpu_va,
                                             uint64_t byte_size)
{
   struct agx_macos_bo unused_bo = {0};

   return agx_macos_bo_set_lookup_gpu_va_range(context, gpu_va, byte_size,
                                                &unused_bo) == KERN_SUCCESS;
}

kern_return_t
agx_macos_resource_record_encode_bo_set(
   struct agx_macos_bo_set *bo_set, uint8_t *record, size_t record_size,
   enum agx_macos_resource_record_kind kind,
   const struct agx_macos_resource_binding *bindings, uint32_t binding_count)
{
   if (!bo_set)
      return kIOReturnBadArgument;

   return agx_macos_resource_record_encode(
      record, record_size, kind, bindings, binding_count,
      agx_macos_resource_range_is_owned_by_bo_set, bo_set);
}

bool
agx_macos_trap4_submission_preview_build(
   const struct agx_macos_submission_carrier_extended_snapshot *snapshot,
   struct agx_macos_trap4_submission_preview *out_preview)
{
   struct agx_macos_submission_trap_observation observation;
   uintptr_t descriptor_address;
   uintptr_t auxiliary_address;

   if (!snapshot || !out_preview ||
       !agx_macos_submission_carrier_extended_snapshot_is_intact(snapshot)) {
      return false;
   }

   *out_preview = (struct agx_macos_trap4_submission_preview){0};
   memcpy(out_preview->carrier, &snapshot->observation.submission.descriptor,
          sizeof(snapshot->observation.submission.descriptor));
   memcpy(out_preview->carrier + AGX_MACOS_SUBMISSION_CARRIER_AUXILIARY_OFFSET,
          snapshot->auxiliary_prefix, sizeof(snapshot->auxiliary_prefix));

   descriptor_address = (uintptr_t)out_preview->carrier;
   auxiliary_address = descriptor_address +
                       AGX_MACOS_SUBMISSION_CARRIER_AUXILIARY_OFFSET;
   if (!agx_macos_submission_trap_observation_decode(
          0, snapshot->observation.submission.queue_id,
          sizeof(struct agx_macos_submit_descriptor_observed), descriptor_address,
          auxiliary_address,
          AGX_MACOS_SUBMISSION_CARRIER_EXTENDED_READABLE_PREFIX, &observation)) {
      *out_preview = (struct agx_macos_trap4_submission_preview){0};
      return false;
   }

   out_preview->observation = observation;
   out_preview->arguments[0] = snapshot->observation.submission.queue_id;
   out_preview->arguments[1] = sizeof(struct agx_macos_submit_descriptor_observed);
   out_preview->arguments[2] = descriptor_address;
   out_preview->arguments[3] = auxiliary_address;
   out_preview->integrity_fingerprint = agx_macos_preview_fingerprint(out_preview);
   return true;
}

bool
agx_macos_trap4_submission_preview_is_intact(
   const struct agx_macos_trap4_submission_preview *preview)
{
   struct agx_macos_submission_trap_observation observation;

   return preview && preview->arguments[2] == (uintptr_t)preview->carrier &&
          preview->arguments[3] == (uintptr_t)preview->carrier +
                                      AGX_MACOS_SUBMISSION_CARRIER_AUXILIARY_OFFSET &&
          preview->integrity_fingerprint == agx_macos_preview_fingerprint(preview) &&
          agx_macos_submission_trap_observation_decode(
             0, preview->arguments[0], preview->arguments[1], preview->arguments[2],
             preview->arguments[3],
             AGX_MACOS_SUBMISSION_CARRIER_EXTENDED_READABLE_PREFIX,
             &observation) &&
          observation.carrier.submission.queue_id == preview->observation.carrier.submission.queue_id;
}

bool
agx_macos_trap4_submission_preview_can_submit(
   const struct agx_macos_trap4_submission_preview *preview)
{
   (void)preview;
   return false;
}

static bool
agx_macos_submission_package_matches_snapshot(
   const struct agx_macos_submission_package *package)
{
   const struct agx_macos_submission_carrier_extended_snapshot *snapshot =
      &package->lease.carrier_snapshot;

   return memcmp(package->preview.carrier,
                 &snapshot->observation.submission.descriptor,
                 sizeof(snapshot->observation.submission.descriptor)) == 0 &&
          memcmp(package->preview.carrier +
                    AGX_MACOS_SUBMISSION_CARRIER_AUXILIARY_OFFSET,
                 snapshot->auxiliary_prefix,
                 sizeof(snapshot->auxiliary_prefix)) == 0 &&
          package->preview.observation.carrier.submission.queue_id ==
             snapshot->observation.submission.queue_id;
}

bool
agx_macos_submission_package_is_intact(
   const struct agx_macos_submission_package *package)
{
   return package && package->active && package->lease.active &&
          (package->lease.state == AGX_MACOS_SUBMISSION_LEASE_ADMITTED ||
           package->lease.state == AGX_MACOS_SUBMISSION_LEASE_IN_FLIGHT) &&
          package->lease.has_carrier_snapshot &&
          agx_macos_submission_carrier_extended_snapshot_is_intact(
             &package->lease.carrier_snapshot) &&
          agx_macos_trap4_submission_preview_is_intact(&package->preview) &&
          agx_macos_submission_package_matches_snapshot(package);
}

kern_return_t
agx_macos_submission_package_bind_notification_queue(
   const struct agx_macos_device_session *session,
   struct agx_macos_notification_queue *queue,
   struct agx_macos_submission_package *package)
{
   if (!package || !agx_macos_submission_package_is_intact(package) ||
       package->lease.state != AGX_MACOS_SUBMISSION_LEASE_ADMITTED) {
      return kIOReturnBadArgument;
   }

   return agx_macos_notification_queue_bind_lease(session, queue,
                                                   &package->lease);
}

kern_return_t
agx_macos_submission_package_mark_submitted(
   struct agx_macos_submission_package *package)
{
   if (!package || !agx_macos_submission_package_is_intact(package) ||
       package->lease.state != AGX_MACOS_SUBMISSION_LEASE_ADMITTED) {
      return kIOReturnBadArgument;
   }

   return agx_macos_submission_lease_mark_submitted(&package->lease);
}

kern_return_t
agx_macos_submission_package_record_completion(
   struct agx_macos_submission_package *package, uint32_t completion_queue_id,
   const struct agx_macos_completion_record_observed *record,
   bool *out_complete)
{
   kern_return_t result;

   if (!package || !out_complete || !agx_macos_submission_package_is_intact(package) ||
       package->lease.state != AGX_MACOS_SUBMISSION_LEASE_IN_FLIGHT) {
      return kIOReturnBadArgument;
   }

   result = agx_macos_submission_lease_record_completion(
      &package->lease, completion_queue_id, record, out_complete);
   if (result != KERN_SUCCESS || !*out_complete)
      return result;

   /* The final completion already retired every pin. Remove the carrier and
    * resource metadata so an old package cannot be reused as a submission. */
   *package = (struct agx_macos_submission_package){0};
   return KERN_SUCCESS;
}

kern_return_t
agx_macos_submission_package_poll_notification_queue(
   const struct agx_macos_device_session *session,
   const struct agx_macos_notification_queue *queue,
   struct agx_macos_submission_package *package, bool *out_complete)
{
   kern_return_t result;

   if (!package || !out_complete ||
       !agx_macos_submission_package_is_intact(package) ||
       package->lease.state != AGX_MACOS_SUBMISSION_LEASE_IN_FLIGHT) {
      return kIOReturnBadArgument;
   }

   result = agx_macos_notification_queue_poll_lease(
      session, queue, &package->lease, out_complete);
   if (result != KERN_SUCCESS || !*out_complete)
      return result;

   /* The second queue token has retired every native BO pin. Do not retain
    * stale carrier or resource-record metadata after that lifetime boundary. */
   *package = (struct agx_macos_submission_package){0};
   return KERN_SUCCESS;
}

kern_return_t
agx_macos_submission_package_admit(
   struct agx_macos_submission_package *out_package,
   struct agx_macos_bo_set *bo_set, uint32_t queue_id,
   const void *descriptor_bytes, size_t descriptor_size,
   const void *auxiliary_bytes, size_t auxiliary_readable_prefix,
   const struct agx_macos_submission_range *ranges, uint32_t range_count,
   uint8_t *resource_record, size_t resource_record_size,
   enum agx_macos_resource_record_kind resource_record_kind,
   const struct agx_macos_resource_binding *resource_bindings,
   uint32_t resource_binding_count)
{
   struct agx_macos_resource_record_layout layout;
   struct agx_macos_submission_range admitted_ranges
      [AGX_MACOS_SUBMISSION_LEASE_MAX_RANGES];
   kern_return_t result;

   if (!out_package || out_package->active || !bo_set || !ranges ||
       range_count == 0 || !resource_record || !resource_bindings ||
       !agx_macos_resource_record_layout_get(resource_record_kind, &layout) ||
       resource_binding_count != layout.binding_count ||
       range_count > AGX_MACOS_SUBMISSION_LEASE_MAX_RANGES -
                        resource_binding_count) {
      return kIOReturnBadArgument;
   }

   /* Include the exact address ranges written into the record. Retaining only
    * a caller-provided command range would leave resource backing freeable. */
   for (uint32_t i = 0; i < range_count; ++i)
      admitted_ranges[i] = ranges[i];
   for (uint32_t i = 0; i < resource_binding_count; ++i) {
      admitted_ranges[range_count + i] =
         (struct agx_macos_submission_range){
            .gpu_va = resource_bindings[i].gpu_va,
            .size = resource_bindings[i].byte_size,
         };
   }

   *out_package = (struct agx_macos_submission_package){0};
   result = agx_macos_submission_lease_init_from_carrier(
      &out_package->lease, bo_set, queue_id, descriptor_bytes, descriptor_size,
      auxiliary_bytes, auxiliary_readable_prefix, admitted_ranges,
      range_count + resource_binding_count);
   if (result != KERN_SUCCESS)
      return result;

   if (!agx_macos_trap4_submission_preview_build(
          &out_package->lease.carrier_snapshot, &out_package->preview)) {
      result = kIOReturnBadArgument;
      goto fail_release;
   }

   result = agx_macos_resource_record_encode_bo_set(
      bo_set, resource_record, resource_record_size, resource_record_kind,
      resource_bindings, resource_binding_count);
   if (result != KERN_SUCCESS)
      goto fail_release;

   out_package->resource_record_kind = resource_record_kind;
   out_package->resource_binding_count = resource_binding_count;
   out_package->active = true;
   return KERN_SUCCESS;

fail_release:
   (void)agx_macos_submission_lease_release(&out_package->lease);
   *out_package = (struct agx_macos_submission_package){0};
   return result;
}

kern_return_t
agx_macos_submission_package_release(
   struct agx_macos_submission_package *package)
{
   kern_return_t result;

   if (!package || !package->active)
      return kIOReturnBadArgument;

   result = agx_macos_submission_lease_release(&package->lease);
   if (result != KERN_SUCCESS)
      return result;

   *package = (struct agx_macos_submission_package){0};
   return KERN_SUCCESS;
}
