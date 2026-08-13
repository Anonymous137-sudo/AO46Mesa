/*
 * Copyright 2026 Khronos_AppleICDs contributors
 * SPDX-License-Identifier: MIT
 */

#include "agx_macos_bo.h"

#include <stddef.h>
#include <string.h>

#define AGX_MACOS_SELECTOR_ALLOCATE_MEM 0x9
#define AGX_MACOS_TRAP_FREE_MEM 0x1
#define AGX_MACOS_ALLOCATE_ATTR_WRITE_COMBINED_BYTE 0x05
#define AGX_MACOS_ALLOCATE_ATTR_WRITE_COMBINED_VALUE 0x04
#define AGX_MACOS_ALLOCATE_STORAGE_FLAGS_BYTE 0x15
#define AGX_MACOS_ALLOCATE_STORAGE_PRIVATE_VALUE 0x20

/* This is the complete 104-byte request captured for a direct 64 KiB shared
 * Metal buffer on the profiled host. Unnamed bytes are intentionally replayed
 * verbatim; no general allocation parameters are inferred from them. */
struct agx_macos_allocate_shared_64k_request {
   uint8_t bytes[0x68];
} __attribute__((packed));

_Static_assert(sizeof(struct agx_macos_allocate_shared_64k_request) == 0x68,
               "modern AGX allocation request size");

struct agx_macos_allocate_response {
   uint64_t gpu_va;
   uint64_t cpu;
   uint32_t unknown0[5];
   uint32_t handle;
   uint64_t root_size;
   uint32_t guid;
   uint32_t unknown1[7];
   uint64_t sub_size;
} __attribute__((packed));

_Static_assert(sizeof(struct agx_macos_allocate_response) == 0x58,
               "modern AGX allocation response size");

static const struct agx_macos_allocate_shared_64k_request
   agx_macos_shared_64k_request = {
      .bytes = {
         [0x08] = 0x01,
         [0x0a] = 0x01,
         [0x0c] = 0x01,
         [0x10] = 0x01,
         [0x11] = 0x01,
         [0x13] = 0x01,
         [0x14] = 0x70,
         [0x15] = 0x04,
         [0x30] = 0x01,
         [0x4a] = 0x01,
         [0x60] = 0x58,
         [0x61] = 0x80,
         [0x62] = 0x7b,
         [0x63] = 0xef,
         [0x64] = 0x01,
      },
   };

static kern_return_t agx_macos_bo_destroy_direct(struct agx_macos_bo *bo);

static bool
agx_macos_bo_is_current(const struct agx_macos_device_session *session,
                        const struct agx_macos_bo *bo)
{
   return agx_macos_device_session_is_current(session) && bo &&
          bo->connection == session->device.connection &&
          bo->api_generation == session->api_generation;
}

static uint16_t
agx_macos_bo_shared_size_units(uint64_t size)
{
   switch (size) {
   case AGX_MACOS_BO_SHARED_64K_SIZE:
      return 1;
   case AGX_MACOS_BO_SHARED_128K_SIZE:
      return 2;
   case AGX_MACOS_BO_SHARED_256K_SIZE:
      return 4;
   case AGX_MACOS_BO_SHARED_512K_SIZE:
      return 8;
   default:
      return 0;
   }
}

static bool
agx_macos_bo_storage_supports_size(enum agx_macos_bo_storage storage,
                                   uint64_t size)
{
   switch (storage) {
   case AGX_MACOS_BO_STORAGE_SHARED:
      return agx_macos_bo_shared_size_units(size) != 0;
   case AGX_MACOS_BO_STORAGE_WRITE_COMBINED:
   case AGX_MACOS_BO_STORAGE_PRIVATE:
      return size == AGX_MACOS_BO_SHARED_64K_SIZE;
   }

   return false;
}

static uint64_t
agx_macos_bo_choose_size(enum agx_macos_bo_storage storage, uint64_t minimum_size)
{
   static const uint64_t shared_sizes[] = {
      AGX_MACOS_BO_SHARED_64K_SIZE,
      AGX_MACOS_BO_SHARED_128K_SIZE,
      AGX_MACOS_BO_SHARED_256K_SIZE,
      AGX_MACOS_BO_SHARED_512K_SIZE,
   };

   if (storage == AGX_MACOS_BO_STORAGE_SHARED) {
      for (unsigned i = 0; i < sizeof(shared_sizes) / sizeof(shared_sizes[0]);
           ++i) {
         if (minimum_size <= shared_sizes[i])
            return shared_sizes[i];
      }
   } else if ((storage == AGX_MACOS_BO_STORAGE_WRITE_COMBINED ||
               storage == AGX_MACOS_BO_STORAGE_PRIVATE) &&
              minimum_size <= AGX_MACOS_BO_SHARED_64K_SIZE) {
      return AGX_MACOS_BO_SHARED_64K_SIZE;
   }

   return 0;
}

static bool
agx_macos_bo_response_is_valid(const struct agx_macos_allocate_response *response,
                               enum agx_macos_bo_storage storage,
                               uint64_t size)
{
   bool expects_cpu_mapping = storage != AGX_MACOS_BO_STORAGE_PRIVATE;

   return response->handle != 0 && response->gpu_va != 0 &&
          (!!response->cpu == expects_cpu_mapping) && response->sub_size == size &&
          response->root_size >= response->sub_size;
}

kern_return_t
agx_macos_bo_create(const struct agx_macos_device_session *session,
                    enum agx_macos_bo_storage storage, uint64_t size,
                    struct agx_macos_bo *bo)
{
   struct agx_macos_allocate_shared_64k_request request =
      agx_macos_shared_64k_request;
   struct agx_macos_allocate_response response = {0};
   size_t response_size = sizeof(response);
   uint16_t size_units = agx_macos_bo_shared_size_units(size);
   kern_return_t result;

   if (!agx_macos_device_session_is_current(session) || !bo ||
       bo->connection != IO_OBJECT_NULL ||
       !agx_macos_bo_storage_supports_size(storage, size)) {
      return kIOReturnBadArgument;
   }

   if (storage == AGX_MACOS_BO_STORAGE_SHARED) {
      /* The trace identifies this as the direct-allocation size-unit field. */
      request.bytes[0x4a] = size_units & 0xff;
      request.bytes[0x4b] = size_units >> 8;
   } else if (storage == AGX_MACOS_BO_STORAGE_WRITE_COMBINED) {
      request.bytes[AGX_MACOS_ALLOCATE_ATTR_WRITE_COMBINED_BYTE] |=
         AGX_MACOS_ALLOCATE_ATTR_WRITE_COMBINED_VALUE;
   } else {
      request.bytes[AGX_MACOS_ALLOCATE_STORAGE_FLAGS_BYTE] |=
         AGX_MACOS_ALLOCATE_STORAGE_PRIVATE_VALUE;
   }

   result = IOConnectCallMethod(
      session->device.connection, AGX_MACOS_SELECTOR_ALLOCATE_MEM, NULL, 0,
      &request, sizeof(request),
      NULL, NULL, &response, &response_size);
   if (result != KERN_SUCCESS)
      return result;

   if (response_size != sizeof(response) ||
       !agx_macos_bo_response_is_valid(&response, storage, size)) {
      if (response.handle)
         (void)IOConnectTrap1(session->device.connection, AGX_MACOS_TRAP_FREE_MEM,
                              response.handle);
      return kIOReturnBadArgument;
   }

   *bo = (struct agx_macos_bo){
      .connection = session->device.connection,
      .gpu_va = response.gpu_va,
      .cpu = (void *)(uintptr_t)response.cpu,
      .size = response.sub_size,
      .handle = response.handle,
      .api_generation = session->api_generation,
      .storage = storage,
   };
   return KERN_SUCCESS;
}

kern_return_t
agx_macos_bo_create_at_least(const struct agx_macos_device_session *session,
                             enum agx_macos_bo_storage storage,
                             uint64_t minimum_size, uint64_t alignment,
                             struct agx_macos_bo *bo)
{
   uint64_t size = agx_macos_bo_choose_size(storage, minimum_size);
   kern_return_t result;

   if (!bo || minimum_size == 0 || alignment == 0 ||
       (alignment & (alignment - 1)) != 0 ||
       alignment > AGX_MACOS_BO_MIN_ALIGNMENT || size == 0) {
      return kIOReturnBadArgument;
   }

   result = agx_macos_bo_create(session, storage, size, bo);
   if (result != KERN_SUCCESS)
      return result;

   if (bo->size < minimum_size || (bo->gpu_va & (alignment - 1)) != 0) {
      (void)agx_macos_bo_destroy(bo);
      return kIOReturnBadArgument;
   }

   return KERN_SUCCESS;
}

static bool
agx_macos_bo_set_conflicts(const struct agx_macos_bo_set *set,
                           const struct agx_macos_bo *bo)
{
   uint64_t bo_end = bo->gpu_va + bo->size;

   if (bo_end < bo->gpu_va)
      return true;

   for (unsigned i = 0; i < AGX_MACOS_BO_SET_CAPACITY; ++i) {
      const struct agx_macos_bo *entry = &set->entries[i];
      uint64_t entry_end;

      if (entry->connection == IO_OBJECT_NULL)
         continue;
      if (entry->handle == bo->handle || entry->gpu_va == bo->gpu_va)
         return true;

      entry_end = entry->gpu_va + entry->size;
      if (entry_end < entry->gpu_va ||
          (entry->gpu_va < bo_end && bo->gpu_va < entry_end)) {
         return true;
      }
   }

   return false;
}

static bool
agx_macos_bo_set_has_identity_conflict(const struct agx_macos_bo_set *set,
                                       const struct agx_macos_bo *bo)
{
   for (unsigned i = 0; i < AGX_MACOS_BO_SET_CAPACITY; ++i) {
      const struct agx_macos_bo *entry = &set->entries[i];

      if (entry->connection != IO_OBJECT_NULL &&
          (entry->handle == bo->handle || entry->gpu_va == bo->gpu_va)) {
         return true;
      }
   }

   return false;
}

kern_return_t
agx_macos_bo_set_init(struct agx_macos_bo_set *set,
                      const struct agx_macos_device_session *session)
{
   if (!set || !agx_macos_device_session_is_current(session)) {
      return kIOReturnBadArgument;
   }

   *set = (struct agx_macos_bo_set){
      .session = session,
      .next_mapping_token = 1,
      .api_generation = session->api_generation,
   };
   if (pthread_mutex_init(&set->lock, NULL) != 0)
      return kIOReturnNoResources;

   set->initialized = true;
   return KERN_SUCCESS;
}

bool
agx_macos_bo_set_is_current(const struct agx_macos_bo_set *set,
                            const struct agx_macos_device_session *session)
{
   return set && agx_macos_device_session_is_current(session) &&
          set->initialized && set->session == session &&
          set->api_generation == session->api_generation;
}

kern_return_t
agx_macos_bo_set_create_at_least(struct agx_macos_bo_set *set,
                                 enum agx_macos_bo_storage storage,
                                 uint64_t minimum_size, uint64_t alignment,
                                 struct agx_macos_bo *bo)
{
   struct agx_macos_bo allocated = {.connection = IO_OBJECT_NULL};
   unsigned free_index = AGX_MACOS_BO_SET_CAPACITY;
   kern_return_t result;

   if (!set || !bo || !agx_macos_bo_set_is_current(set, set->session) ||
       bo->connection != IO_OBJECT_NULL)
      return kIOReturnBadArgument;

   pthread_mutex_lock(&set->lock);
   for (unsigned i = 0; i < AGX_MACOS_BO_SET_CAPACITY; ++i) {
      if (set->entries[i].connection == IO_OBJECT_NULL) {
         free_index = i;
         break;
      }
   }

   if (free_index == AGX_MACOS_BO_SET_CAPACITY) {
      pthread_mutex_unlock(&set->lock);
      return kIOReturnNoResources;
   }

   result = agx_macos_bo_create_at_least(set->session, storage, minimum_size,
                                         alignment, &allocated);
   if (result == KERN_SUCCESS && agx_macos_bo_set_conflicts(set, &allocated)) {
      /* A repeated raw selector-9 call may report the identity of an existing
       * allocation. Never free that ambiguous reply: doing so could release a
       * live BO already tracked by this set. A distinct overlapping reply can
       * be safely retired, but identity reuse stays fail-closed. */
      if (!agx_macos_bo_set_has_identity_conflict(set, &allocated))
         (void)agx_macos_bo_destroy(&allocated);
      result = kIOReturnBadArgument;
   }

   if (result == KERN_SUCCESS) {
      allocated.managed_by_set = true;
      set->entries[free_index] = allocated;
      *bo = allocated;
   }

   pthread_mutex_unlock(&set->lock);
   return result;
}

kern_return_t
agx_macos_bo_set_lookup_handle(struct agx_macos_bo_set *set,
                               uint32_t handle, struct agx_macos_bo *bo)
{
   kern_return_t result = kIOReturnNotFound;

   if (!set || !bo || handle == 0 ||
       !agx_macos_bo_set_is_current(set, set->session))
      return kIOReturnBadArgument;

   pthread_mutex_lock(&set->lock);
   for (unsigned i = 0; i < AGX_MACOS_BO_SET_CAPACITY; ++i) {
      const struct agx_macos_bo *entry = &set->entries[i];

      if (entry->connection != IO_OBJECT_NULL && entry->handle == handle) {
         *bo = *entry;
         result = KERN_SUCCESS;
         break;
      }
   }
   pthread_mutex_unlock(&set->lock);
   return result;
}

kern_return_t
agx_macos_bo_set_lookup_gpu_va(struct agx_macos_bo_set *set,
                               uint64_t gpu_va, struct agx_macos_bo *bo)
{
   kern_return_t result = kIOReturnNotFound;

   if (!set || !bo || !agx_macos_bo_set_is_current(set, set->session))
      return kIOReturnBadArgument;

   pthread_mutex_lock(&set->lock);
   for (unsigned i = 0; i < AGX_MACOS_BO_SET_CAPACITY; ++i) {
      const struct agx_macos_bo *entry = &set->entries[i];

      if (entry->connection != IO_OBJECT_NULL && gpu_va >= entry->gpu_va &&
          gpu_va - entry->gpu_va < entry->size) {
         *bo = *entry;
         result = KERN_SUCCESS;
         break;
      }
   }
   pthread_mutex_unlock(&set->lock);
   return result;
}

kern_return_t
agx_macos_bo_set_lookup_gpu_va_range(struct agx_macos_bo_set *set,
                                     uint64_t gpu_va, uint64_t size,
                                     struct agx_macos_bo *bo)
{
   kern_return_t result = kIOReturnNotFound;

   if (!set || !bo || size == 0 ||
       !agx_macos_bo_set_is_current(set, set->session) ||
       gpu_va > UINT64_MAX - size) {
      return kIOReturnBadArgument;
   }

   pthread_mutex_lock(&set->lock);
   for (unsigned i = 0; i < AGX_MACOS_BO_SET_CAPACITY; ++i) {
      const struct agx_macos_bo *entry = &set->entries[i];
      uint64_t offset;

      if (entry->connection == IO_OBJECT_NULL || gpu_va < entry->gpu_va)
         continue;

      offset = gpu_va - entry->gpu_va;
      if (offset > entry->size || size > entry->size - offset)
         continue;

      *bo = *entry;
      result = KERN_SUCCESS;
      break;
   }
   pthread_mutex_unlock(&set->lock);
   return result;
}

static struct agx_macos_bo *
agx_macos_bo_set_find_handle(struct agx_macos_bo_set *set, uint32_t handle)
{
   for (unsigned i = 0; i < AGX_MACOS_BO_SET_CAPACITY; ++i) {
      struct agx_macos_bo *entry = &set->entries[i];

      if (entry->connection != IO_OBJECT_NULL && entry->handle == handle)
         return entry;
   }

   return NULL;
}

static struct agx_macos_bo_mapping_lease *
agx_macos_bo_set_find_mapping(struct agx_macos_bo_set *set, uint64_t token)
{
   for (unsigned i = 0; i < AGX_MACOS_BO_MAPPING_CAPACITY; ++i) {
      struct agx_macos_bo_mapping_lease *mapping = &set->mappings[i];

      if (mapping->token == token)
         return mapping;
   }

   return NULL;
}

static struct agx_macos_bo_mapping_lease *
agx_macos_bo_set_allocate_mapping(struct agx_macos_bo_set *set)
{
   for (unsigned i = 0; i < AGX_MACOS_BO_MAPPING_CAPACITY; ++i) {
      if (set->mappings[i].token == 0)
         return &set->mappings[i];
   }

   return NULL;
}

static uint64_t
agx_macos_bo_set_next_mapping_token(struct agx_macos_bo_set *set)
{
   for (unsigned i = 0; i < AGX_MACOS_BO_MAPPING_CAPACITY; ++i) {
      uint64_t token = set->next_mapping_token++;

      if (token != 0 && !agx_macos_bo_set_find_mapping(set, token))
         return token;
   }

   return 0;
}

kern_return_t
agx_macos_bo_set_retain_submission(struct agx_macos_bo_set *set,
                                   uint32_t handle)
{
   struct agx_macos_bo *entry;
   kern_return_t result = kIOReturnNotFound;

   if (!set || handle == 0 ||
       !agx_macos_bo_set_is_current(set, set->session))
      return kIOReturnBadArgument;

   pthread_mutex_lock(&set->lock);
   entry = agx_macos_bo_set_find_handle(set, handle);
   if (entry) {
      if (entry->in_flight_count == UINT32_MAX)
         result = kIOReturnNoResources;
      else {
         ++entry->in_flight_count;
         result = KERN_SUCCESS;
      }
   }
   pthread_mutex_unlock(&set->lock);
   return result;
}

kern_return_t
agx_macos_bo_set_retain_submission_range(struct agx_macos_bo_set *set,
                                         uint64_t gpu_va, uint64_t size)
{
   struct agx_macos_bo *match = NULL;
   kern_return_t result = kIOReturnNotFound;

   if (!set || size == 0 ||
       !agx_macos_bo_set_is_current(set, set->session) ||
       gpu_va > UINT64_MAX - size) {
      return kIOReturnBadArgument;
   }

   pthread_mutex_lock(&set->lock);
   for (unsigned i = 0; i < AGX_MACOS_BO_SET_CAPACITY; ++i) {
      struct agx_macos_bo *entry = &set->entries[i];
      uint64_t offset;

      if (entry->connection == IO_OBJECT_NULL || gpu_va < entry->gpu_va)
         continue;

      offset = gpu_va - entry->gpu_va;
      if (offset > entry->size || size > entry->size - offset)
         continue;

      match = entry;
      break;
   }

   if (match) {
      if (match->in_flight_count == UINT32_MAX)
         result = kIOReturnNoResources;
      else {
         ++match->in_flight_count;
         result = KERN_SUCCESS;
      }
   }
   pthread_mutex_unlock(&set->lock);
   return result;
}

kern_return_t
agx_macos_bo_set_release_submission(struct agx_macos_bo_set *set,
                                    uint32_t handle)
{
   struct agx_macos_bo *entry;
   kern_return_t result = kIOReturnNotFound;

   if (!set || !set->initialized || handle == 0)
      return kIOReturnBadArgument;

   pthread_mutex_lock(&set->lock);
   entry = agx_macos_bo_set_find_handle(set, handle);
   if (entry) {
      if (entry->in_flight_count == 0)
         result = kIOReturnBadArgument;
      else {
         --entry->in_flight_count;
         result = KERN_SUCCESS;
      }
   }
   pthread_mutex_unlock(&set->lock);
   return result;
}

kern_return_t
agx_macos_bo_set_release_submission_range(struct agx_macos_bo_set *set,
                                          uint64_t gpu_va, uint64_t size)
{
   struct agx_macos_bo *match = NULL;
   kern_return_t result = kIOReturnNotFound;

   if (!set || !set->initialized || size == 0 ||
       gpu_va > UINT64_MAX - size) {
      return kIOReturnBadArgument;
   }

   pthread_mutex_lock(&set->lock);
   for (unsigned i = 0; i < AGX_MACOS_BO_SET_CAPACITY; ++i) {
      struct agx_macos_bo *entry = &set->entries[i];
      uint64_t offset;

      if (entry->connection == IO_OBJECT_NULL || gpu_va < entry->gpu_va)
         continue;

      offset = gpu_va - entry->gpu_va;
      if (offset > entry->size || size > entry->size - offset)
         continue;

      match = entry;
      break;
   }

   if (match) {
      if (match->in_flight_count == 0)
         result = kIOReturnBadArgument;
      else {
         --match->in_flight_count;
         result = KERN_SUCCESS;
      }
   }
   pthread_mutex_unlock(&set->lock);
   return result;
}

kern_return_t
agx_macos_bo_set_map_range(struct agx_macos_bo_set *set, uint32_t handle,
                           uint64_t offset, uint64_t size,
                           struct agx_macos_bo_mapping *out_mapping)
{
   struct agx_macos_bo *entry;
   struct agx_macos_bo_mapping_lease *mapping;
   kern_return_t result = kIOReturnNotFound;

   if (!set || !out_mapping || out_mapping->active || handle == 0 ||
       size == 0 || !agx_macos_bo_set_is_current(set, set->session)) {
      return kIOReturnBadArgument;
   }

   pthread_mutex_lock(&set->lock);
   entry = agx_macos_bo_set_find_handle(set, handle);
   if (entry && entry->storage != AGX_MACOS_BO_STORAGE_PRIVATE && entry->cpu &&
       offset <= entry->size && size <= entry->size - offset) {
      mapping = agx_macos_bo_set_allocate_mapping(set);
      if (entry->cpu_map_count == UINT32_MAX || !mapping) {
         result = kIOReturnNoResources;
      } else {
         uint64_t token = agx_macos_bo_set_next_mapping_token(set);

         if (token == 0) {
            result = kIOReturnNoResources;
         } else {
            ++entry->cpu_map_count;
            *mapping = (struct agx_macos_bo_mapping_lease){
               .handle = handle,
               .cpu = (uint8_t *)entry->cpu + offset,
               .size = size,
               .token = token,
            };
            *out_mapping = (struct agx_macos_bo_mapping){
               .handle = handle,
               .cpu = mapping->cpu,
               .size = size,
               .token = token,
               .active = true,
            };
            result = KERN_SUCCESS;
         }
      }
   }
   pthread_mutex_unlock(&set->lock);
   return result;
}

kern_return_t
agx_macos_bo_set_unmap_range(struct agx_macos_bo_set *set,
                             struct agx_macos_bo_mapping *mapping)
{
   struct agx_macos_bo *entry;
   struct agx_macos_bo_mapping_lease *lease;
   kern_return_t result = kIOReturnNotFound;

   if (!set || !set->initialized || !mapping || !mapping->active ||
       mapping->handle == 0 || !mapping->cpu || mapping->size == 0 ||
       mapping->token == 0) {
      return kIOReturnBadArgument;
   }

   pthread_mutex_lock(&set->lock);
   entry = agx_macos_bo_set_find_handle(set, mapping->handle);
   lease = agx_macos_bo_set_find_mapping(set, mapping->token);
   if (!entry || !lease || entry->cpu_map_count == 0 ||
       lease->handle != mapping->handle || lease->cpu != mapping->cpu ||
       lease->size != mapping->size) {
      result = kIOReturnBadArgument;
   } else {
      --entry->cpu_map_count;
      *lease = (struct agx_macos_bo_mapping_lease){0};
      *mapping = (struct agx_macos_bo_mapping){0};
      result = KERN_SUCCESS;
   }
   pthread_mutex_unlock(&set->lock);
   return result;
}

kern_return_t
agx_macos_bo_set_destroy(struct agx_macos_bo_set *set,
                         struct agx_macos_bo *bo)
{
   kern_return_t result = kIOReturnBadArgument;

   if (!set || !bo || !set->initialized || !bo->managed_by_set ||
       bo->connection == IO_OBJECT_NULL || bo->handle == 0) {
      return kIOReturnBadArgument;
   }

   pthread_mutex_lock(&set->lock);
   for (unsigned i = 0; i < AGX_MACOS_BO_SET_CAPACITY; ++i) {
      struct agx_macos_bo *entry = &set->entries[i];

      if (entry->connection == bo->connection && entry->handle == bo->handle &&
          entry->gpu_va == bo->gpu_va) {
         if (entry->in_flight_count != 0 || entry->cpu_map_count != 0) {
            result = kIOReturnBusy;
         } else {
            result = agx_macos_bo_destroy_direct(entry);
            if (result == KERN_SUCCESS)
               *bo = (struct agx_macos_bo){.connection = IO_OBJECT_NULL};
         }
         break;
      }
   }
   pthread_mutex_unlock(&set->lock);
   return result;
}

kern_return_t
agx_macos_bo_set_cleanup(struct agx_macos_bo_set *set)
{
   kern_return_t first_error = KERN_SUCCESS;
   bool empty = true;

   if (!set || !set->initialized)
      return kIOReturnBadArgument;

   pthread_mutex_lock(&set->lock);
   for (unsigned i = 0; i < AGX_MACOS_BO_SET_CAPACITY; ++i) {
      struct agx_macos_bo *entry = &set->entries[i];

      if (entry->connection == IO_OBJECT_NULL)
         continue;

      kern_return_t result =
         (entry->in_flight_count != 0 || entry->cpu_map_count != 0)
                               ? kIOReturnBusy
                               : agx_macos_bo_destroy_direct(entry);
      if (result != KERN_SUCCESS) {
         if (first_error == KERN_SUCCESS)
            first_error = result;
         empty = false;
      }
   }
   pthread_mutex_unlock(&set->lock);

   if (!empty)
      return first_error;

   pthread_mutex_destroy(&set->lock);
   *set = (struct agx_macos_bo_set){0};
   return KERN_SUCCESS;
}

kern_return_t
agx_macos_bo_map(const struct agx_macos_device_session *session,
                 struct agx_macos_bo *bo, uint64_t offset, uint64_t size,
                 void **cpu)
{
   if (!cpu)
      return kIOReturnBadArgument;

   *cpu = NULL;
   if (!agx_macos_bo_is_current(session, bo) ||
       bo->storage == AGX_MACOS_BO_STORAGE_PRIVATE || !bo->cpu || size == 0 ||
       offset > bo->size || size > bo->size - offset) {
      return kIOReturnBadArgument;
   }

   *cpu = (uint8_t *)bo->cpu + offset;
   return KERN_SUCCESS;
}

kern_return_t
agx_macos_bo_destroy(struct agx_macos_bo *bo)
{
   if (!bo || bo->managed_by_set || bo->connection == IO_OBJECT_NULL ||
       bo->handle == 0)
      return kIOReturnBadArgument;

   return agx_macos_bo_destroy_direct(bo);
}

static kern_return_t
agx_macos_bo_destroy_direct(struct agx_macos_bo *bo)
{
   kern_return_t result;

   result = IOConnectTrap1(bo->connection, AGX_MACOS_TRAP_FREE_MEM, bo->handle);
   if (result == KERN_SUCCESS)
      *bo = (struct agx_macos_bo){.connection = IO_OBJECT_NULL};

   return result;
}
