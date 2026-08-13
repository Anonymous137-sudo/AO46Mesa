/*
 * Copyright 2026 Khronos_AppleICDs contributors
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <stdint.h>

#include "agx_device.h"
#include "agx_macos_bo.h"
#include "agx_macos_submission_build.h"

struct agx_macos_notification_queue;
struct agx_macos_mesa_submission_package;
struct agx_macos_shader_provenance;

/* A platform provider lets the macOS frontend retain an Apple-owned resource
 * as the backing of a Mesa BO. It is an ownership and mapping boundary only;
 * it does not admit command bytes or submit work to AGX. */
struct agx_macos_mesa_platform_bo {
   void *owner;
   void *cpu;
   uint64_t gpu_va;
   uint64_t size;
   const void *carrier_resource_binding;
   /* Required only for a low-VA executable allocation. It records an
    * Apple-admitted compiler-result lifecycle, never a raw AGX code buffer. */
   const struct agx_macos_shader_provenance *shader_provenance;
};

/* The public-buffer provider is deliberately not a generic AGX allocator.
 * Low VA controls the USC address range consumed by agx_usc_addr(), while
 * executable controls the provenance of bytes the GPU may fetch as code.
 * Those are independent of an ordinary CPU-visible allocation. */
enum agx_macos_mesa_platform_bo_capability {
   AGX_MACOS_MESA_PLATFORM_BO_CAP_CPU_MAPPABLE = 1u << 0,
   AGX_MACOS_MESA_PLATFORM_BO_CAP_LOW_VA = 1u << 1,
   AGX_MACOS_MESA_PLATFORM_BO_CAP_EXECUTABLE = 1u << 2,
};

struct agx_macos_mesa_bo_provider {
   void *context;
   uint32_t capabilities;
   bool (*create)(void *context, uint64_t size, enum agx_bo_flags flags,
                  struct agx_macos_mesa_platform_bo *out_bo);
   bool (*is_current)(const void *context,
                      const struct agx_macos_mesa_platform_bo *bo);
   void (*destroy)(void *context, struct agx_macos_mesa_platform_bo *bo);
};

enum agx_macos_mesa_device_capability {
   AGX_MACOS_MESA_DEVICE_CAP_BO_ALLOC = 1u << 0,
   AGX_MACOS_MESA_DEVICE_CAP_BO_MAP = 1u << 1,
   /* Direct allocations are returned pre-mapped by the AGX user client.
    * This is intentionally narrower than arbitrary VM bind or unbind. */
   AGX_MACOS_MESA_DEVICE_CAP_FIXED_BO_BIND = 1u << 2,
   AGX_MACOS_MESA_DEVICE_CAP_VM_BIND = 1u << 3,
   AGX_MACOS_MESA_DEVICE_CAP_SUBMIT = 1u << 4,
   AGX_MACOS_MESA_DEVICE_CAP_COMPLETION_SYNC = 1u << 5,
   AGX_MACOS_MESA_DEVICE_CAP_OBJECT_BIND = 1u << 6,
   AGX_MACOS_MESA_DEVICE_CAP_LOW_VA_BIND = 1u << 7,
   AGX_MACOS_MESA_DEVICE_CAP_EXECUTABLE_BO = 1u << 8,
   AGX_MACOS_MESA_DEVICE_CAP_SHADER_CODE_ADMISSION = 1u << 9,
};

/* A screen is admitted only when Mesa's required memory, VM, submission, and
 * completion operations are all backed by the active macOS UABI contract. */
#define AGX_MACOS_MESA_DEVICE_CAP_SCREEN_REQUIRED                         \
   (AGX_MACOS_MESA_DEVICE_CAP_BO_ALLOC |                                  \
    AGX_MACOS_MESA_DEVICE_CAP_BO_MAP |                                    \
    AGX_MACOS_MESA_DEVICE_CAP_VM_BIND |                                   \
    AGX_MACOS_MESA_DEVICE_CAP_SUBMIT |                                    \
    AGX_MACOS_MESA_DEVICE_CAP_COMPLETION_SYNC |                           \
    AGX_MACOS_MESA_DEVICE_CAP_OBJECT_BIND |                               \
    AGX_MACOS_MESA_DEVICE_CAP_LOW_VA_BIND |                               \
    AGX_MACOS_MESA_DEVICE_CAP_EXECUTABLE_BO |                             \
    AGX_MACOS_MESA_DEVICE_CAP_SHADER_CODE_ADMISSION)

/* Initializes Mesa's agx_device with only the traced macOS allocation and CPU
 * mapping operations. The caller transfers it to agx_screen_create_macos or
 * destroys it with agx_macos_mesa_device_destroy. */
bool agx_macos_mesa_device_init(
   struct agx_device *out_device,
   const struct agx_macos_device_session *session,
   struct agx_macos_bo_set *bo_set,
   const struct agx_macos_notification_queue *notification_queue);
/* BO creation can precede completion-port creation on the direct AGX profile.
 * Attach the generation-validated notification queue exactly once before sync
 * support is exposed. This does not create or identify an AGX execution queue;
 * queue submission remains unavailable until its own UABI contract exists. */
bool agx_macos_mesa_device_attach_notification_queue(
   struct agx_device *device,
   const struct agx_macos_notification_queue *notification_queue);
/* Installs one Apple-owned BO provider before the first Mesa BO is allocated.
 * The provider declares its CPU-data, USC low-VA, and executable capabilities
 * separately. Direct selector-9 BOs remain available when no provider is
 * attached. */
bool agx_macos_mesa_device_attach_bo_provider(
   struct agx_device *device,
   const struct agx_macos_mesa_bo_provider *provider);
/* Transfers a managed direct BO from the native bootstrap set into Mesa's BO
 * registry without issuing a second raw allocation. On success the caller
 * must clear its copied native handle: Mesa releases the set entry through
 * agx_bo_free after all command and fence references retire. */
struct agx_bo *agx_macos_mesa_device_adopt_bo(
   struct agx_device *device, const struct agx_macos_bo *native,
   enum agx_bo_flags flags);
bool agx_macos_mesa_device_is_current(const struct agx_device *device);
uint32_t agx_macos_mesa_device_capabilities(const struct agx_device *device);
uint32_t agx_macos_mesa_device_missing_screen_capabilities(
   const struct agx_device *device);
/* Reads a provider-owned backing only after revalidating its lifetime and the
 * Mesa BO identity. This is not a generic import API. */
bool agx_macos_mesa_bo_get_platform_backing(
   const struct agx_device *device, const struct agx_bo *bo,
   const struct agx_macos_mesa_bo_provider *expected_provider,
   struct agx_macos_mesa_platform_bo *out_backing);
bool agx_macos_mesa_bo_get_carrier_resource_binding(
   const struct agx_device *device, const struct agx_bo *bo,
   const void **out_binding);
/* Teardown is refused while any native sync handle remains live. In
 * particular, an in-flight package must retire from the notification queue
 * before its device, BO set, or queue can be dismantled. */
bool agx_macos_mesa_device_destroy(struct agx_device *device);

/* A macOS sync owns only an already in-flight package. It receives GPU
 * completion through the bound notification queue and has no carrier creation
 * or submission capability of its own. */
bool agx_macos_mesa_sync_is_supported(const struct agx_device *device);
int agx_macos_mesa_sync_create(struct agx_device *device, uint32_t flags,
                               uint32_t *out_handle);
int agx_macos_mesa_sync_reference(struct agx_device *device, uint32_t handle);
int agx_macos_mesa_sync_destroy(struct agx_device *device, uint32_t handle);
int agx_macos_mesa_sync_wait(struct agx_device *device,
                             const uint32_t *handles, uint32_t handle_count,
                             uint64_t absolute_timeout, uint32_t flags,
                             uint32_t *first_signaled);
/* Transfers an in-flight package to Mesa's real binary/timeline output
 * descriptors after the Apple-owned carrier accepted it. One native
 * completion retires the shared package and advances every output. Binary
 * outputs explicitly rearm reusable batch handles; timeline values must
 * advance monotonically and may retire out of order. Ownership moves only on
 * success. */
int agx_macos_mesa_sync_adopt_submission_outputs(
   struct agx_device *device, const struct agx_submit_sync *outputs,
   uint32_t output_count,
   struct agx_macos_mesa_submission_package *package);

/* Binary-only compatibility wrapper for callers that do not carry Mesa's
 * platform-neutral output descriptors. */
int agx_macos_mesa_sync_adopt_submission_group(
   struct agx_device *device, const uint32_t *handles, uint32_t handle_count,
   struct agx_macos_mesa_submission_package *package);

/* Single-output compatibility wrapper for the grouped ownership API. */
int agx_macos_mesa_sync_adopt_submission(
   struct agx_device *device, uint32_t handle,
   struct agx_macos_mesa_submission_package *package);

/* A Mesa BO subrange is the only input accepted by the macOS submission
 * adapter. Raw Linux drm_asahi_submit buffers are deliberately not accepted:
 * they are Linux transport packets, not Apple command records. */
struct agx_macos_mesa_bo_range {
   struct agx_bo *bo;
   uint64_t offset;
   uint64_t size;
};

/* Asahi batches expose VDM/CDM encoder spans as a mapped BO plus the current
 * CPU write cursor. This helper validates that ownership shape before a future
 * macOS command-record adapter may retain it. It does not treat the stream as
 * an Apple command record or make it submittable. */
struct agx_macos_mesa_encoder_range {
   struct agx_bo *bo;
   const uint8_t *begin;
   const uint8_t *end;
};

kern_return_t agx_macos_mesa_encoder_range_resolve(
   struct agx_device *device,
   const struct agx_macos_mesa_encoder_range *encoder_range,
   struct agx_macos_mesa_bo_range *out_range);

#define AGX_MACOS_MESA_SUBMISSION_MAX_BOS \
   AGX_MACOS_SUBMISSION_LEASE_MAX_RANGES

/* This wrapper owns both sides of the lifetime boundary. native pins keep the
 * direct AGX objects live, while Mesa references prevent agx_bo_free from
 * disposing the corresponding agx_bo before package retirement. */
struct agx_macos_mesa_submission_package {
   struct agx_macos_submission_package native;
   struct agx_device *device;
   struct agx_bo *bo_references[AGX_MACOS_MESA_SUBMISSION_MAX_BOS];
   uint32_t bo_reference_count;
   bool active;
};

/* Admits a real macOS-backed Mesa BO set to the proven command-record binding
 * path. This validates every CPU/GPU subrange and retains each unique Mesa BO,
 * but it neither encodes a Linux submission packet nor dispatches IOKit. */
kern_return_t agx_macos_mesa_submission_package_admit(
   struct agx_macos_mesa_submission_package *out_package,
   struct agx_device *device, uint32_t queue_id,
   const void *descriptor_bytes, size_t descriptor_size,
   const void *auxiliary_bytes, size_t auxiliary_readable_prefix,
   const struct agx_macos_mesa_bo_range *command_ranges,
   uint32_t command_range_count,
   const struct agx_macos_mesa_bo_range *resource_record_range,
   enum agx_macos_resource_record_kind resource_record_kind,
   const struct agx_macos_mesa_bo_range *resource_ranges,
   uint32_t resource_range_count);

/* Resolves every active Asahi VDM/CDM encoder span against its owning agx_bo
 * before admitting one package. This is the native batch boundary: it accepts
 * Mesa command streams and BO ownership, never a Linux drm_asahi_submit
 * packet. A future Apple-owned carrier handoff consumes this package. */
kern_return_t agx_macos_mesa_submission_package_admit_encoders(
   struct agx_macos_mesa_submission_package *out_package,
   struct agx_device *device, uint32_t queue_id,
   const void *descriptor_bytes, size_t descriptor_size,
   const void *auxiliary_bytes, size_t auxiliary_readable_prefix,
   const struct agx_macos_mesa_encoder_range *encoder_ranges,
   uint32_t encoder_range_count,
   const struct agx_macos_mesa_bo_range *resource_record_range,
   enum agx_macos_resource_record_kind resource_record_kind,
   const struct agx_macos_mesa_bo_range *resource_ranges,
   uint32_t resource_range_count);

/* Convenience entry point for a single active Asahi encoder span. */
kern_return_t agx_macos_mesa_submission_package_admit_encoder(
   struct agx_macos_mesa_submission_package *out_package,
   struct agx_device *device, uint32_t queue_id,
   const void *descriptor_bytes, size_t descriptor_size,
   const void *auxiliary_bytes, size_t auxiliary_readable_prefix,
   const struct agx_macos_mesa_encoder_range *encoder_range,
   const struct agx_macos_mesa_bo_range *resource_record_range,
   enum agx_macos_resource_record_kind resource_record_kind,
   const struct agx_macos_mesa_bo_range *resource_ranges,
   uint32_t resource_range_count);
kern_return_t agx_macos_mesa_submission_package_bind_notification_queue(
   const struct agx_macos_device_session *session,
   struct agx_macos_notification_queue *queue,
   struct agx_macos_mesa_submission_package *package);
kern_return_t agx_macos_mesa_submission_package_mark_submitted(
   struct agx_macos_mesa_submission_package *package);
kern_return_t agx_macos_mesa_submission_package_record_completion(
   struct agx_macos_mesa_submission_package *package,
   uint32_t completion_queue_id,
   const struct agx_macos_completion_record_observed *record,
   bool *out_complete);
/* Polls the bound macOS notification queue and retires the Mesa package only
 * after the native lease receives both completion tokens. */
kern_return_t agx_macos_mesa_submission_package_poll_notification_queue(
   const struct agx_macos_device_session *session,
   const struct agx_macos_notification_queue *queue,
   struct agx_macos_mesa_submission_package *package, bool *out_complete);
kern_return_t agx_macos_mesa_submission_package_release(
   struct agx_macos_mesa_submission_package *package);
bool agx_macos_mesa_submission_package_is_intact(
   const struct agx_macos_mesa_submission_package *package);
