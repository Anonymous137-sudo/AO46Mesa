/*
 * Copyright 2026 Khronos_AppleICDs contributors
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include "agx_macos_bo.h"
#include "agx_macos_submission_observation.h"

#define AGX_MACOS_SUBMISSION_LEASE_MAX_RANGES 64

struct agx_macos_submission_range {
   uint64_t gpu_va;
   uint64_t size;
};

enum agx_macos_submission_lease_state {
   AGX_MACOS_SUBMISSION_LEASE_NONE,
   AGX_MACOS_SUBMISSION_LEASE_ADMITTED,
   AGX_MACOS_SUBMISSION_LEASE_IN_FLIGHT,
   AGX_MACOS_SUBMISSION_LEASE_COMPLETE,
   AGX_MACOS_SUBMISSION_LEASE_ABANDONED,
};

/* Owns BO pins from submit admission until both observed completion tokens
 * arrive. It validates lifetime only; it does not construct or submit AGX ABI. */
struct agx_macos_submission_lease {
   struct agx_macos_submission_fence fence;
   /* Direct submission must retain the observed outer carrier, not merely a
    * copied descriptor, until the sidecar ABI is fully implemented. */
   struct agx_macos_submission_carrier_extended_snapshot carrier_snapshot;
   struct agx_macos_bo_set *bo_set;
   /* One pin per backing BO, even when multiple Mesa resource ranges alias
    * the same allocation in a batch. */
   uint32_t handles[AGX_MACOS_SUBMISSION_LEASE_MAX_RANGES];
   uint32_t handle_count;
   enum agx_macos_submission_lease_state state;
   bool has_carrier_snapshot;
   bool active;
};

kern_return_t agx_macos_submission_lease_init(
   struct agx_macos_submission_lease *out_lease,
   struct agx_macos_bo_set *bo_set, uint32_t queue_id,
   const void *descriptor_bytes, size_t descriptor_size,
   const struct agx_macos_submission_range *ranges, uint32_t range_count);
/* Admits a future direct submission only after capturing the 4 KiB outer
 * carrier evidence established by live macOS AGX render and compute traces. */
kern_return_t agx_macos_submission_lease_init_from_carrier(
   struct agx_macos_submission_lease *out_lease,
   struct agx_macos_bo_set *bo_set, uint32_t queue_id,
   const void *descriptor_bytes, size_t descriptor_size,
   const void *auxiliary_bytes, size_t auxiliary_readable_prefix,
   const struct agx_macos_submission_range *ranges, uint32_t range_count);

/* Marks a lease as in flight after a direct submission has been accepted. A
 * bare descriptor lease is intentionally not eligible for this transition. */
kern_return_t agx_macos_submission_lease_mark_submitted(
   struct agx_macos_submission_lease *lease);

/* Releases all held BO pins for an admitted, completed, or device-lost lease.
 * An in-flight lease must instead retire through completions or abandonment. */
kern_return_t agx_macos_submission_lease_release(
   struct agx_macos_submission_lease *lease);

/* Releases an in-flight lease only after the owning device/session is known
 * lost. This is a teardown path, not a timeout or normal cancellation path. */
kern_return_t agx_macos_submission_lease_abandon_after_device_loss(
   struct agx_macos_submission_lease *lease);

/* Returns a completion error without consuming ownership for a wrong token or
 * queue. On the final token, all BO pins are released before success. */
kern_return_t agx_macos_submission_lease_record_completion(
   struct agx_macos_submission_lease *lease, uint32_t completion_queue_id,
   const struct agx_macos_completion_record_observed *record,
   bool *out_complete);
