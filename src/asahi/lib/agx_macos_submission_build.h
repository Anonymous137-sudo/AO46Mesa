/*
 * Copyright 2026 Khronos_AppleICDs contributors
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <mach/kern_return.h>

#include "agx_macos_bo.h"
#include "agx_macos_submission_lease.h"
#include "agx_macos_submission_observation.h"

#define AGX_MACOS_RESOURCE_RECORD_MAX_BINDINGS 4
#define AGX_MACOS_TRAP4_SUBMISSION_CARRIER_BYTES \
   (AGX_MACOS_SUBMISSION_CARRIER_AUXILIARY_OFFSET + \
    AGX_MACOS_SUBMISSION_CARRIER_EXTENDED_READABLE_PREFIX)

/* These layouts are resource-address records observed in CPU-mapped command
 * allocations. The profiled sidecar is outer transport metadata, not a
 * resource table, so resource binding must use these records. */
enum agx_macos_resource_record_kind {
   AGX_MACOS_RESOURCE_RECORD_BLIT_PRODUCER,
   AGX_MACOS_RESOURCE_RECORD_BLIT_CONSUMER,
   AGX_MACOS_RESOURCE_RECORD_COMPUTE,
};

struct agx_macos_resource_record_layout {
   enum agx_macos_resource_record_kind kind;
   uint32_t binding_count;
   size_t minimum_record_size;
   size_t binding_offsets[AGX_MACOS_RESOURCE_RECORD_MAX_BINDINGS];
};

/* gpu_va may include a traced per-resource byte offset. byte_size is used to
 * prove that every encoded GPU address lies entirely within its owner. */
struct agx_macos_resource_binding {
   uint64_t gpu_va;
   uint64_t byte_size;
};

typedef bool (*agx_macos_resource_range_is_owned_fn)(void *context,
                                                      uint64_t gpu_va,
                                                      uint64_t byte_size);

/* Exposes the three record shapes whose address slots were varied and
 * independently verified by controlled blit/compute workloads. */
bool agx_macos_resource_record_layout_get(
   enum agx_macos_resource_record_kind kind,
   struct agx_macos_resource_record_layout *out_layout);

/* Encodes only proven resource-address slots after every range has been
 * authorized. The caller owns record lifetime and must pin resources before
 * any future real submission; this API does not interpret a command stream. */
kern_return_t agx_macos_resource_record_encode(
   uint8_t *record, size_t record_size,
   enum agx_macos_resource_record_kind kind,
   const struct agx_macos_resource_binding *bindings, uint32_t binding_count,
   agx_macos_resource_range_is_owned_fn range_is_owned, void *range_context);

/* Mesa-facing convenience admission check against the native BO tracker. */
kern_return_t agx_macos_resource_record_encode_bo_set(
   struct agx_macos_bo_set *bo_set, uint8_t *record, size_t record_size,
   enum agx_macos_resource_record_kind kind,
   const struct agx_macos_resource_binding *bindings, uint32_t binding_count);

/* This is an exact in-memory representation of the observed outer Trap4
 * arguments. Its carrier is copied from immutable trace evidence and it has
 * no direct IOConnectTrap4 dispatch API. */
struct agx_macos_trap4_submission_preview {
   struct agx_macos_submission_trap_observation observation;
   uintptr_t arguments[4];
   uint64_t integrity_fingerprint;
   uint8_t carrier[AGX_MACOS_TRAP4_SUBMISSION_CARRIER_BYTES];
};

/* Builds descriptor/sidecar placement and the four observed Trap4 arguments
 * from intact captured evidence. The result remains non-submittable until the
 * full outer transport schema and a real command-record carrier are validated.
 * Resource addresses are deliberately supplied through the separate record. */
bool agx_macos_trap4_submission_preview_build(
   const struct agx_macos_submission_carrier_extended_snapshot *snapshot,
   struct agx_macos_trap4_submission_preview *out_preview);
bool agx_macos_trap4_submission_preview_is_intact(
   const struct agx_macos_trap4_submission_preview *preview);
/* Always false today. Keeping this admission point explicit prevents a new
 * caller from turning captured, process-local pointer graphs into a replay. */
bool agx_macos_trap4_submission_preview_can_submit(
   const struct agx_macos_trap4_submission_preview *preview);

/* This is the native admission unit connecting a CPU-mapped resource record
 * to its BO lifetime and an immutable outer Trap4 carrier preview. It does
 * not decode sidecar pointers or provide an IOConnectTrap4 dispatch path. */
struct agx_macos_submission_package {
   struct agx_macos_submission_lease lease;
   struct agx_macos_trap4_submission_preview preview;
   enum agx_macos_resource_record_kind resource_record_kind;
   uint32_t resource_binding_count;
   bool active;
};

struct agx_macos_device_session;
struct agx_macos_notification_queue;

/* These methods join the immutable carrier package to the existing
 * notification/fence lifecycle. They neither encode an AGX command payload
 * nor call a submission entry point. */
kern_return_t agx_macos_submission_package_bind_notification_queue(
   const struct agx_macos_device_session *session,
   struct agx_macos_notification_queue *queue,
   struct agx_macos_submission_package *package);
kern_return_t agx_macos_submission_package_mark_submitted(
   struct agx_macos_submission_package *package);
kern_return_t agx_macos_submission_package_record_completion(
   struct agx_macos_submission_package *package, uint32_t completion_queue_id,
   const struct agx_macos_completion_record_observed *record,
   bool *out_complete);
/* Polls exactly one matching completion from the bound notification queue.
 * It keeps foreign or malformed records queued and destroys this package only
 * after the second expected token retires the native BO lease. */
kern_return_t agx_macos_submission_package_poll_notification_queue(
   const struct agx_macos_device_session *session,
   const struct agx_macos_notification_queue *queue,
   struct agx_macos_submission_package *package, bool *out_complete);

/* Retains every caller range and every encoded resource range before changing
 * a record. ranges must include the CPU-mapped record's backing BO. The result
 * is an integrity-checked, explicitly non-submittable package; a future UABI
 * implementation must not bypass this admission. */
kern_return_t agx_macos_submission_package_admit(
   struct agx_macos_submission_package *out_package,
   struct agx_macos_bo_set *bo_set, uint32_t queue_id,
   const void *descriptor_bytes, size_t descriptor_size,
   const void *auxiliary_bytes, size_t auxiliary_readable_prefix,
   const struct agx_macos_submission_range *ranges, uint32_t range_count,
   uint8_t *resource_record, size_t resource_record_size,
   enum agx_macos_resource_record_kind resource_record_kind,
   const struct agx_macos_resource_binding *resource_bindings,
   uint32_t resource_binding_count);
kern_return_t agx_macos_submission_package_release(
   struct agx_macos_submission_package *package);
bool agx_macos_submission_package_is_intact(
   const struct agx_macos_submission_package *package);
