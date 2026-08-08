/*
 * Copyright 2026 Khronos_AppleICDs contributors
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Observed in empty Metal submissions on the profiled macOS AGX interface. */
struct agx_macos_submit_descriptor_observed {
   uint32_t header0;
   uint32_t header1;
   uint64_t opaque0;
   uint64_t completion_tokens[2];
   uint64_t opaque1[4];
} __attribute__((packed));

_Static_assert(sizeof(struct agx_macos_submit_descriptor_observed) == 0x40,
               "modern AGX observed submission descriptor size");

/* The first word is the only completion-record field correlated by traces. */
struct agx_macos_completion_record_observed {
   uint64_t token;
   uint8_t opaque[32];
} __attribute__((packed));

_Static_assert(sizeof(struct agx_macos_completion_record_observed) == 0x28,
               "modern AGX observed completion record size");

/* These bounds describe only the outer carrier relationship observed in
 * controlled Metal traces. They do not define or decode the opaque sidecar. */
#define AGX_MACOS_SUBMISSION_CARRIER_AUXILIARY_OFFSET 0x84
#define AGX_MACOS_SUBMISSION_CARRIER_MIN_READABLE_PREFIX 0x100
/* Render and compute traces both expose this much sidecar data. The slot is
 * captured as opaque evidence only; its value is never dereferenced here. */
#define AGX_MACOS_SUBMISSION_CARRIER_EXTENDED_READABLE_PREFIX 0x1000
#define AGX_MACOS_SUBMISSION_CARRIER_OPAQUE_POINTER_SLOT_OFFSET 0x790
#define AGX_MACOS_SUBMISSION_DESCRIPTOR_HEADER0 2u
#define AGX_MACOS_SUBMISSION_DESCRIPTOR_HEADER1 1u

struct agx_macos_submission_observation {
   uint32_t queue_id;
   struct agx_macos_submit_descriptor_observed descriptor;
};

struct agx_macos_submission_carrier_observation {
   struct agx_macos_submission_observation submission;
   size_t auxiliary_offset;
   size_t auxiliary_readable_prefix;
};

/* Captures only the fixed-size sidecar prefix observed by tracing. This
 * immutable snapshot is for offline UABI study, never direct submission. */
struct agx_macos_submission_carrier_snapshot {
   struct agx_macos_submission_carrier_observation observation;
   uint8_t auxiliary_prefix[AGX_MACOS_SUBMISSION_CARRIER_MIN_READABLE_PREFIX];
};

/* This is the complete sidecar evidence currently required before the winsys
 * can retain a submission lease. opaque_pointer_slot is a captured byte value,
 * not a CPU or GPU address exposed to the driver. */
struct agx_macos_submission_carrier_extended_snapshot {
   struct agx_macos_submission_carrier_observation observation;
   uint64_t opaque_pointer_slot;
   uint8_t auxiliary_prefix
      [AGX_MACOS_SUBMISSION_CARRIER_EXTENDED_READABLE_PREFIX];
};

/* A future direct-submit path owns one of these until both observed completion
 * tokens arrive. This is completion bookkeeping only, not a submit ABI. */
struct agx_macos_submission_fence {
   struct agx_macos_submission_observation observation;
   uint8_t completed_token_mask;
};

/* Decode only the shape observed in controlled Metal traces. This is not a
 * command-submission interface and deliberately does not interpret opaque
 * descriptor or completion fields. */
bool agx_macos_submission_observation_decode(
   uint32_t queue_id, const void *bytes, size_t byte_count,
   struct agx_macos_submission_observation *out_observation);

/* Validates the trace-observed relationship between a 64-byte descriptor and
 * its opaque auxiliary carrier. The sidecar remains intentionally opaque and
 * this API must not be used to submit work. */
bool agx_macos_submission_carrier_observation_decode(
   uint32_t queue_id, const void *descriptor_bytes, size_t descriptor_size,
   const void *auxiliary_bytes, size_t auxiliary_readable_prefix,
   struct agx_macos_submission_carrier_observation *out_observation);

bool agx_macos_submission_carrier_snapshot_capture(
   uint32_t queue_id, const void *descriptor_bytes, size_t descriptor_size,
   const void *auxiliary_bytes, size_t auxiliary_readable_prefix,
   struct agx_macos_submission_carrier_snapshot *out_snapshot);

/* Captures the extended trace-observed carrier needed for future direct
 * submission admission. It rejects short or empty pointer-slot evidence and
 * must not be treated as a sidecar decoder or submit interface. */
bool agx_macos_submission_carrier_extended_snapshot_capture(
   uint32_t queue_id, const void *descriptor_bytes, size_t descriptor_size,
   const void *auxiliary_bytes, size_t auxiliary_readable_prefix,
   struct agx_macos_submission_carrier_extended_snapshot *out_snapshot);

/* Returns true only when a 40-byte completion record starts with one of the
 * two descriptor tokens on the same observed queue. out_token_index receives
 * zero or one when non-NULL. */
bool agx_macos_submission_observation_matches_completion(
   const struct agx_macos_submission_observation *observation,
   uint32_t completion_queue_id,
   const struct agx_macos_completion_record_observed *record,
   unsigned *out_token_index);

bool agx_macos_submission_fence_init(
   uint32_t queue_id, const void *descriptor_bytes, size_t descriptor_size,
   struct agx_macos_submission_fence *out_fence);
/* Rejects records for a different queue, unknown tokens, and duplicate token
 * delivery. A completed fence must contain exactly both distinct tokens. */
bool agx_macos_submission_fence_record_completion(
   struct agx_macos_submission_fence *fence, uint32_t completion_queue_id,
   const struct agx_macos_completion_record_observed *record);
bool agx_macos_submission_fence_is_complete(
   const struct agx_macos_submission_fence *fence);
