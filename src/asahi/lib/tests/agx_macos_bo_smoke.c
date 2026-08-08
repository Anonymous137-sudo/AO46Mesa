/*
 * Copyright 2026 Khronos_AppleICDs contributors
 * SPDX-License-Identifier: MIT
 */

#include "agx_macos_bo.h"
#include "agx_macos_submission_lease.h"

#include <inttypes.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

static bool
agx_macos_bo_ranges_overlap(const struct agx_macos_bo *a,
                            const struct agx_macos_bo *b)
{
   return a->gpu_va < b->gpu_va + b->size && b->gpu_va < a->gpu_va + a->size;
}

int
main(int argc, char **argv)
{
   static const struct {
      enum agx_macos_bo_storage storage;
      uint64_t size;
      const char *name;
   } allocations[] = {
      {AGX_MACOS_BO_STORAGE_SHARED, AGX_MACOS_BO_SHARED_64K_SIZE, "shared"},
      {AGX_MACOS_BO_STORAGE_SHARED, AGX_MACOS_BO_SHARED_128K_SIZE, "shared"},
      {AGX_MACOS_BO_STORAGE_SHARED, AGX_MACOS_BO_SHARED_256K_SIZE, "shared"},
      {AGX_MACOS_BO_STORAGE_SHARED, AGX_MACOS_BO_SHARED_512K_SIZE, "shared"},
      {AGX_MACOS_BO_STORAGE_WRITE_COMBINED, AGX_MACOS_BO_SHARED_64K_SIZE,
       "write-combined"},
      {AGX_MACOS_BO_STORAGE_PRIVATE, AGX_MACOS_BO_SHARED_64K_SIZE, "private"},
   };
   static const struct {
      enum agx_macos_bo_storage storage;
      uint64_t minimum_size;
      uint64_t alignment;
      uint64_t expected_size;
      const char *name;
   } policy_allocations[] = {
      {AGX_MACOS_BO_STORAGE_SHARED, 1, 1, AGX_MACOS_BO_SHARED_64K_SIZE,
       "shared-small"},
      {AGX_MACOS_BO_STORAGE_SHARED, AGX_MACOS_BO_SHARED_64K_SIZE + 1,
       AGX_MACOS_BO_MIN_ALIGNMENT, AGX_MACOS_BO_SHARED_128K_SIZE,
       "shared-medium"},
      {AGX_MACOS_BO_STORAGE_WRITE_COMBINED, 1, 1,
       AGX_MACOS_BO_SHARED_64K_SIZE, "write-combined-small"},
      {AGX_MACOS_BO_STORAGE_PRIVATE, 1, 1, AGX_MACOS_BO_SHARED_64K_SIZE,
       "private-small"},
   };
   struct agx_macos_device_session session;

   if (!getenv("AGX_MACOS_EXPERIMENTAL_BO")) {
      puts("AGX_MACOS_BO_SMOKE skipped; set AGX_MACOS_EXPERIMENTAL_BO=1 to run");
      return 0;
   }

   if (agx_macos_device_session_open(&session) !=
       AGX_MACOS_DEVICE_SESSION_READY) {
      fputs("AGX_MACOS_BO_SMOKE failed to open profiled session\n", stderr);
      return 1;
   }

   if (argc != 1 ||
       agx_macos_device_session_configure_traced_api(&session, argv[0]) !=
          KERN_SUCCESS) {
      fputs("AGX_MACOS_BO_SMOKE failed to configure traced API\n", stderr);
      agx_macos_device_session_close(&session);
      return 1;
   }

   {
      struct agx_macos_bo_set set;
      struct agx_macos_bo resources[3] = {
         {.connection = IO_OBJECT_NULL},
         {.connection = IO_OBJECT_NULL},
         {.connection = IO_OBJECT_NULL},
      };
      static const enum agx_macos_bo_storage storage[] = {
         AGX_MACOS_BO_STORAGE_SHARED,
         AGX_MACOS_BO_STORAGE_SHARED,
         AGX_MACOS_BO_STORAGE_SHARED,
      };

      if (agx_macos_bo_set_init(&set, &session) != KERN_SUCCESS) {
         fputs("AGX_MACOS_BO_SMOKE could not initialize the live BO set\n", stderr);
         agx_macos_device_session_close(&session);
         return 1;
      }

      for (unsigned i = 0; i < sizeof(resources) / sizeof(resources[0]); ++i) {
         kern_return_t result = agx_macos_bo_set_create_at_least(
            &set, storage[i], AGX_MACOS_BO_SHARED_64K_SIZE,
            1, &resources[i]);

         if (result != KERN_SUCCESS ||
             !resources[i].cpu) {
            fprintf(stderr, "AGX_MACOS_BO_SMOKE live BO allocation %u failed: %#x\n",
                    i, result);
            (void)agx_macos_bo_set_cleanup(&set);
            agx_macos_device_session_close(&session);
            return 1;
         }
      }

      if (resources[0].handle == resources[1].handle ||
          resources[0].handle == resources[2].handle ||
          resources[1].handle == resources[2].handle ||
          agx_macos_bo_ranges_overlap(&resources[0], &resources[1]) ||
          agx_macos_bo_ranges_overlap(&resources[0], &resources[2]) ||
          agx_macos_bo_ranges_overlap(&resources[1], &resources[2])) {
         fputs("AGX_MACOS_BO_SMOKE live BO ownership was not distinct\n", stderr);
         (void)agx_macos_bo_set_cleanup(&set);
         agx_macos_device_session_close(&session);
         return 1;
      }

      if (agx_macos_bo_set_retain_submission(&set, resources[0].handle) !=
             KERN_SUCCESS ||
          agx_macos_bo_set_retain_submission_range(
             &set, resources[0].gpu_va + 64, 128) !=
             KERN_SUCCESS ||
          agx_macos_bo_set_retain_submission_range(
             &set, resources[0].gpu_va + resources[0].size - 1, 2) !=
             kIOReturnNotFound ||
          agx_macos_bo_destroy(&resources[0]) != kIOReturnBadArgument ||
          agx_macos_bo_set_destroy(&set, &resources[0]) != kIOReturnBusy ||
          agx_macos_bo_set_release_submission_range(
             &set, resources[0].gpu_va + 64, 128) !=
             KERN_SUCCESS ||
          agx_macos_bo_set_release_submission(&set, resources[0].handle) !=
             KERN_SUCCESS ||
          agx_macos_bo_set_release_submission_range(
             &set, resources[0].gpu_va + resources[0].size - 1, 2) !=
             kIOReturnNotFound ||
          agx_macos_bo_set_release_submission(&set, resources[0].handle) !=
             kIOReturnBadArgument) {
         fputs("AGX_MACOS_BO_SMOKE in-flight BO lifetime gate failed\n", stderr);
         (void)agx_macos_bo_set_cleanup(&set);
         agx_macos_device_session_close(&session);
         return 1;
      }

      {
         static const struct agx_macos_submit_descriptor_observed descriptor = {
            .header0 = 2,
            .header1 = 1,
            .completion_tokens = {
               0x0102030405060708ull,
               0x1112131415161718ull,
            },
         };
         const struct agx_macos_submission_range ranges[] = {
            {resources[0].gpu_va + 64, 128},
            {resources[1].gpu_va + 128, 256},
            {resources[0].gpu_va + 256, 128},
         };
         const struct agx_macos_submission_range invalid_ranges[] = {
            {resources[2].gpu_va + 64, 128},
            {resources[2].gpu_va + resources[2].size - 1, 2},
         };
         struct agx_macos_submission_lease lease = {0};
         uint8_t carrier[AGX_MACOS_SUBMISSION_CARRIER_AUXILIARY_OFFSET +
                         AGX_MACOS_SUBMISSION_CARRIER_EXTENDED_READABLE_PREFIX] = {0};
         const uint64_t opaque_pointer_slot = UINT64_C(0x1ff800000);
         struct agx_macos_completion_record_observed completion = {
            .token = descriptor.completion_tokens[1],
         };
         bool complete = false;

         memcpy(carrier, &descriptor, sizeof(descriptor));
         memcpy(carrier + AGX_MACOS_SUBMISSION_CARRIER_AUXILIARY_OFFSET +
                   AGX_MACOS_SUBMISSION_CARRIER_OPAQUE_POINTER_SLOT_OFFSET,
                &opaque_pointer_slot, sizeof(opaque_pointer_slot));

         if (agx_macos_submission_lease_init_from_carrier(
                &lease, &set, 7, carrier, sizeof(descriptor),
                carrier + AGX_MACOS_SUBMISSION_CARRIER_AUXILIARY_OFFSET,
                AGX_MACOS_SUBMISSION_CARRIER_EXTENDED_READABLE_PREFIX, ranges,
                sizeof(ranges) / sizeof(ranges[0])) != KERN_SUCCESS ||
             agx_macos_submission_lease_mark_submitted(&lease) != KERN_SUCCESS ||
             agx_macos_bo_set_destroy(&set, &resources[0]) != kIOReturnBusy ||
             agx_macos_bo_set_destroy(&set, &resources[1]) != kIOReturnBusy ||
             agx_macos_submission_lease_record_completion(
                &lease, 8, &completion, &complete) != kIOReturnBadArgument ||
             complete ||
             agx_macos_submission_lease_record_completion(
                &lease, 7, &completion, &complete) != KERN_SUCCESS ||
             complete ||
             agx_macos_bo_set_destroy(&set, &resources[0]) != kIOReturnBusy) {
            fputs("AGX_MACOS_BO_SMOKE submission lease admission failed\n", stderr);
            (void)agx_macos_submission_lease_release(&lease);
            (void)agx_macos_bo_set_cleanup(&set);
            agx_macos_device_session_close(&session);
            return 1;
         }

         completion.token = descriptor.completion_tokens[0];
         if (agx_macos_submission_lease_record_completion(
                &lease, 7, &completion, &complete) != KERN_SUCCESS ||
             !complete || lease.active ||
             agx_macos_submission_lease_release(&lease) != kIOReturnBadArgument ||
             agx_macos_submission_lease_init_from_carrier(
                &lease, &set, 7, carrier, sizeof(descriptor),
                carrier + AGX_MACOS_SUBMISSION_CARRIER_AUXILIARY_OFFSET,
                AGX_MACOS_SUBMISSION_CARRIER_EXTENDED_READABLE_PREFIX,
                invalid_ranges,
                sizeof(invalid_ranges) / sizeof(invalid_ranges[0])) !=
                kIOReturnNotFound ||
             agx_macos_bo_set_retain_submission(&set, resources[2].handle) !=
                KERN_SUCCESS ||
             agx_macos_bo_set_release_submission(&set, resources[2].handle) !=
                KERN_SUCCESS) {
            fputs("AGX_MACOS_BO_SMOKE submission lease completion failed\n",
                  stderr);
            (void)agx_macos_bo_set_cleanup(&set);
            agx_macos_device_session_close(&session);
            return 1;
         }
      }

      for (unsigned i = 0; i < sizeof(resources) / sizeof(resources[0]); ++i) {
         struct agx_macos_bo by_handle = {.connection = IO_OBJECT_NULL};
         struct agx_macos_bo by_gpu_va = {.connection = IO_OBJECT_NULL};
         struct agx_macos_bo by_gpu_va_range = {.connection = IO_OBJECT_NULL};
         struct agx_macos_bo cross_boundary = {.connection = IO_OBJECT_NULL};

         if (agx_macos_bo_set_lookup_handle(&set, resources[i].handle,
                                            &by_handle) != KERN_SUCCESS ||
             agx_macos_bo_set_lookup_gpu_va(
                &set, resources[i].gpu_va + resources[i].size - 1,
                &by_gpu_va) != KERN_SUCCESS ||
             agx_macos_bo_set_lookup_gpu_va_range(
                &set, resources[i].gpu_va + resources[i].size - 128, 128,
                &by_gpu_va_range) != KERN_SUCCESS ||
             agx_macos_bo_set_lookup_gpu_va_range(
                &set, resources[i].gpu_va + resources[i].size - 1, 2,
                &cross_boundary) != kIOReturnNotFound ||
             by_handle.handle != resources[i].handle ||
             by_gpu_va.handle != resources[i].handle ||
             by_gpu_va_range.handle != resources[i].handle ||
             cross_boundary.connection != IO_OBJECT_NULL) {
            fputs("AGX_MACOS_BO_SMOKE live BO lookup failed\n", stderr);
            (void)agx_macos_bo_set_cleanup(&set);
            agx_macos_device_session_close(&session);
            return 1;
         }
      }

      {
         struct agx_macos_bo missing = {.connection = IO_OBJECT_NULL};

         if (agx_macos_bo_set_lookup_handle(&set, 0, &missing) !=
                kIOReturnBadArgument ||
             agx_macos_bo_set_lookup_gpu_va(&set, 0, &missing) !=
                kIOReturnNotFound ||
             agx_macos_bo_set_lookup_gpu_va_range(&set, 0, 0, &missing) !=
                kIOReturnBadArgument) {
            fputs("AGX_MACOS_BO_SMOKE live BO lookup accepted an invalid key\n",
                  stderr);
            (void)agx_macos_bo_set_cleanup(&set);
            agx_macos_device_session_close(&session);
            return 1;
         }
      }

      printf("AGX_MACOS_BO_SMOKE live-set handles=%u/%u/%u\n",
             resources[0].handle, resources[1].handle, resources[2].handle);

      for (unsigned i = sizeof(resources) / sizeof(resources[0]); i-- > 0;) {
         if (agx_macos_bo_set_destroy(&set, &resources[i]) != KERN_SUCCESS) {
            fputs("AGX_MACOS_BO_SMOKE live BO release failed\n", stderr);
            (void)agx_macos_bo_set_cleanup(&set);
            agx_macos_device_session_close(&session);
            return 1;
         }
      }

      if (agx_macos_bo_set_cleanup(&set) != KERN_SUCCESS) {
         fputs("AGX_MACOS_BO_SMOKE live BO set cleanup failed\n", stderr);
         agx_macos_device_session_close(&session);
         return 1;
      }
   }

   for (unsigned i = 0;
        i < sizeof(policy_allocations) / sizeof(policy_allocations[0]); ++i) {
      struct agx_macos_bo bo = {.connection = IO_OBJECT_NULL};
      kern_return_t result = agx_macos_bo_create_at_least(
         &session, policy_allocations[i].storage,
         policy_allocations[i].minimum_size, policy_allocations[i].alignment,
         &bo);

      if (result != KERN_SUCCESS || bo.size != policy_allocations[i].expected_size ||
          (bo.gpu_va & (policy_allocations[i].alignment - 1)) != 0 ||
          agx_macos_bo_destroy(&bo) != KERN_SUCCESS) {
         fprintf(stderr, "AGX_MACOS_BO_SMOKE policy allocation %s failed: %#x\n",
                 policy_allocations[i].name, result);
         agx_macos_device_session_close(&session);
         return 1;
      }
   }

   {
      struct agx_macos_bo unsupported = {.connection = IO_OBJECT_NULL};

      if (agx_macos_bo_create_at_least(
             &session, AGX_MACOS_BO_STORAGE_SHARED,
             AGX_MACOS_BO_SHARED_512K_SIZE + 1, 1, &unsupported) !=
             kIOReturnBadArgument ||
          agx_macos_bo_create_at_least(
             &session, AGX_MACOS_BO_STORAGE_SHARED, 1,
             AGX_MACOS_BO_MIN_ALIGNMENT * 2, &unsupported) !=
             kIOReturnBadArgument) {
         fputs("AGX_MACOS_BO_SMOKE accepted unsupported policy request\n", stderr);
         agx_macos_device_session_close(&session);
         return 1;
      }
   }

   {
      struct agx_macos_bo unsupported = {.connection = IO_OBJECT_NULL};

      if (agx_macos_bo_create(&session, AGX_MACOS_BO_STORAGE_SHARED, 4 * 1024,
                              &unsupported) != kIOReturnBadArgument) {
         fputs("AGX_MACOS_BO_SMOKE accepted unsupported direct size\n", stderr);
         agx_macos_device_session_close(&session);
         return 1;
      }
   }

   for (unsigned i = 0; i < sizeof(allocations) / sizeof(allocations[0]); ++i) {
      struct agx_macos_bo bo = {.connection = IO_OBJECT_NULL};
      kern_return_t result = agx_macos_bo_create(
         &session, allocations[i].storage, allocations[i].size, &bo);

      if (result != KERN_SUCCESS) {
         fprintf(stderr,
                 "AGX_MACOS_BO_SMOKE allocation %s size=%" PRIu64
                 " failed: %#x\n",
                 allocations[i].name, allocations[i].size, result);
         agx_macos_device_session_close(&session);
         return 1;
      }

      printf("AGX_MACOS_BO_SMOKE allocated %s handle=%u gpu=%" PRIx64
             " cpu=%p size=%" PRIu64 "\n",
             allocations[i].name, bo.handle, bo.gpu_va, bo.cpu, bo.size);

      if (bo.storage == AGX_MACOS_BO_STORAGE_PRIVATE) {
         void *cpu;

         if (agx_macos_bo_map(&bo, 0, 1, &cpu) != kIOReturnBadArgument) {
            fputs("AGX_MACOS_BO_SMOKE private BO unexpectedly mapped\n", stderr);
            agx_macos_bo_destroy(&bo);
            agx_macos_device_session_close(&session);
            return 1;
         }
      } else {
         static const uint8_t pattern[] = {
            0x46, 0x4f, 0x34, 0x36, 0x41, 0x47, 0x58, 0x00,
         };
         void *mapped;
         uint8_t *first;
         uint8_t *last;

         if (agx_macos_bo_map(&bo, 0, sizeof(pattern), &mapped) != KERN_SUCCESS) {
            fputs("AGX_MACOS_BO_SMOKE CPU map validation failed\n", stderr);
            agx_macos_bo_destroy(&bo);
            agx_macos_device_session_close(&session);
            return 1;
         }
         first = mapped;

         if (agx_macos_bo_map(&bo, bo.size - sizeof(pattern), sizeof(pattern),
                              &mapped) != KERN_SUCCESS ||
             agx_macos_bo_map(&bo, bo.size, 1, &mapped) != kIOReturnBadArgument) {
            fputs("AGX_MACOS_BO_SMOKE CPU map validation failed\n", stderr);
            agx_macos_bo_destroy(&bo);
            agx_macos_device_session_close(&session);
            return 1;
         }
         last = (uint8_t *)bo.cpu + bo.size - sizeof(pattern);

         memcpy(first, pattern, sizeof(pattern));
         memcpy(last, pattern, sizeof(pattern));
         if (memcmp(first, pattern, sizeof(pattern)) != 0 ||
             memcmp(last, pattern, sizeof(pattern)) != 0) {
            fputs("AGX_MACOS_BO_SMOKE CPU mapping data mismatch\n", stderr);
            agx_macos_bo_destroy(&bo);
            agx_macos_device_session_close(&session);
            return 1;
         }
      }

      result = agx_macos_bo_destroy(&bo);
      if (result != KERN_SUCCESS) {
         fprintf(stderr, "AGX_MACOS_BO_SMOKE release %s size=%" PRIu64
                         " failed: %#x\n",
                 allocations[i].name, allocations[i].size, result);
         agx_macos_device_session_close(&session);
         return 1;
      }
   }

   agx_macos_device_session_close(&session);

   puts("AGX_MACOS_BO_SMOKE complete");
   return 0;
}
