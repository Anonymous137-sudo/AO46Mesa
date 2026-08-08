/*
 * Copyright 2026 Khronos_AppleICDs contributors
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <mach/kern_return.h>
#include <pthread.h>

#include "agx_macos_device.h"

#define AGX_MACOS_BO_SHARED_64K_SIZE (64 * 1024)
#define AGX_MACOS_BO_SHARED_128K_SIZE (128 * 1024)
#define AGX_MACOS_BO_SHARED_256K_SIZE (256 * 1024)
#define AGX_MACOS_BO_SHARED_512K_SIZE (512 * 1024)
#define AGX_MACOS_BO_MIN_ALIGNMENT (64 * 1024)
#define AGX_MACOS_BO_SET_CAPACITY 64
#define AGX_MACOS_BO_MAPPING_CAPACITY 256

enum agx_macos_bo_storage {
   AGX_MACOS_BO_STORAGE_SHARED,
   AGX_MACOS_BO_STORAGE_WRITE_COMBINED,
   AGX_MACOS_BO_STORAGE_PRIVATE,
};

/* This structure represents the only direct BO contract currently validated
 * by hardware tracing. It is not a general allocation interface yet. */
struct agx_macos_bo {
   io_connect_t connection;
   uint64_t gpu_va;
   void *cpu;
   uint64_t size;
   uint32_t handle;
   enum agx_macos_bo_storage storage;
   uint32_t in_flight_count;
   uint32_t cpu_map_count;
   bool managed_by_set;
};

/* A managed BO mapping pins the backing allocation until the caller releases
 * it. This prevents resource destruction while Mesa-facing CPU access is live. */
struct agx_macos_bo_mapping {
   uint32_t handle;
   void *cpu;
   uint64_t size;
   uint64_t token;
   bool active;
};

/* Tracks one issued map capability. The public mapping object can be copied,
 * so unmap validates this immutable record before releasing BO ownership. */
struct agx_macos_bo_mapping_lease {
   uint32_t handle;
   void *cpu;
   uint64_t size;
   uint64_t token;
};

/* This is the native resource-ownership layer used before a Mesa agx_bo
 * adapter can take ownership of these direct allocations. */
struct agx_macos_bo_set {
   const struct agx_macos_device_session *session;
   pthread_mutex_t lock;
   struct agx_macos_bo entries[AGX_MACOS_BO_SET_CAPACITY];
   struct agx_macos_bo_mapping_lease
      mappings[AGX_MACOS_BO_MAPPING_CAPACITY];
   uint64_t next_mapping_token;
   bool initialized;
};

/* Creates only a trace-validated direct allocation contract. */
kern_return_t agx_macos_bo_create(const struct agx_macos_device_session *session,
                                  enum agx_macos_bo_storage storage,
                                  uint64_t size, struct agx_macos_bo *bo);

/* Selects the smallest trace-validated root BO that meets the request. */
kern_return_t agx_macos_bo_create_at_least(
   const struct agx_macos_device_session *session,
   enum agx_macos_bo_storage storage, uint64_t minimum_size,
   uint64_t alignment, struct agx_macos_bo *bo);

kern_return_t agx_macos_bo_set_init(
   struct agx_macos_bo_set *set,
   const struct agx_macos_device_session *session);
kern_return_t agx_macos_bo_set_create_at_least(
   struct agx_macos_bo_set *set, enum agx_macos_bo_storage storage,
   uint64_t minimum_size, uint64_t alignment, struct agx_macos_bo *bo);
kern_return_t agx_macos_bo_set_lookup_handle(
   struct agx_macos_bo_set *set, uint32_t handle,
   struct agx_macos_bo *bo);
kern_return_t agx_macos_bo_set_lookup_gpu_va(
   struct agx_macos_bo_set *set, uint64_t gpu_va,
   struct agx_macos_bo *bo);
/* Looks up the BO which completely owns a GPU virtual-address range. This is
 * the lookup required by submission-resource validation; a start-address
 * lookup alone is insufficient when a range crosses a BO boundary. */
kern_return_t agx_macos_bo_set_lookup_gpu_va_range(
   struct agx_macos_bo_set *set, uint64_t gpu_va, uint64_t size,
   struct agx_macos_bo *bo);
/* Retain BO ownership while a future submission may reference it. A matching
 * release is required before destruction or set cleanup can free the handle. */
kern_return_t agx_macos_bo_set_retain_submission(
   struct agx_macos_bo_set *set, uint32_t handle);
/* Retains the BO which completely owns this GPU virtual-address range. This
 * prevents a future submission from pinning only its first byte. */
kern_return_t agx_macos_bo_set_retain_submission_range(
   struct agx_macos_bo_set *set, uint64_t gpu_va, uint64_t size);
kern_return_t agx_macos_bo_set_release_submission(
   struct agx_macos_bo_set *set, uint32_t handle);
/* Releases a previously retained GPU virtual-address range. The range must
 * still be wholly owned by a tracked BO. */
kern_return_t agx_macos_bo_set_release_submission_range(
   struct agx_macos_bo_set *set, uint64_t gpu_va, uint64_t size);
/* Maps a range from a BO owned by the set and retains that BO until unmap.
 * This is the CPU-access contract used by a future macOS agx_bo adapter. */
kern_return_t agx_macos_bo_set_map_range(
   struct agx_macos_bo_set *set, uint32_t handle, uint64_t offset,
   uint64_t size, struct agx_macos_bo_mapping *out_mapping);
kern_return_t agx_macos_bo_set_unmap_range(
   struct agx_macos_bo_set *set, struct agx_macos_bo_mapping *mapping);
kern_return_t agx_macos_bo_set_destroy(struct agx_macos_bo_set *set,
                                       struct agx_macos_bo *bo);
kern_return_t agx_macos_bo_set_cleanup(struct agx_macos_bo_set *set);

static inline kern_return_t
agx_macos_bo_create_shared(const struct agx_macos_device_session *session,
                           uint64_t size, struct agx_macos_bo *bo)
{
   return agx_macos_bo_create(session, AGX_MACOS_BO_STORAGE_SHARED, size, bo);
}

static inline kern_return_t
agx_macos_bo_create_shared_64k(const struct agx_macos_device_session *session,
                               struct agx_macos_bo *bo)
{
   return agx_macos_bo_create_shared(session, AGX_MACOS_BO_SHARED_64K_SIZE, bo);
}

/* Direct mappings last for the BO lifetime; private BOs cannot be mapped. */
kern_return_t agx_macos_bo_map(struct agx_macos_bo *bo, uint64_t offset,
                               uint64_t size, void **cpu);
/* Only standalone BOs may use this release path. BO-set members must go
 * through agx_macos_bo_set_destroy so in-flight ownership is checked. */
kern_return_t agx_macos_bo_destroy(struct agx_macos_bo *bo);
