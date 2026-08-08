/*
 * Copyright 2026 Khronos_AppleICDs contributors
 * SPDX-License-Identifier: MIT
 */

#include "agx_macos_device.h"

#include <stdio.h>
#include <string.h>

static bool
same_device_info(const struct agx_macos_device_info *a,
                 const struct agx_macos_device_info *b)
{
   return a->chip_id == b->chip_id &&
          a->gpu_generation == b->gpu_generation &&
          a->core_count == b->core_count &&
          a->cluster_count == b->cluster_count &&
          a->cores_per_cluster == b->cores_per_cluster &&
          a->gpu_partition_count == b->gpu_partition_count &&
          a->fragment_core_count == b->fragment_core_count &&
          a->usc_generation == b->usc_generation &&
          a->kickid_queue_shift == b->kickid_queue_shift &&
          a->kickid_queue_mask == b->kickid_queue_mask &&
          a->parameter_buffer_max_size == b->parameter_buffer_max_size &&
          memcmp(a->core_masks, b->core_masks, sizeof(a->core_masks)) == 0 &&
          strcmp(a->variant, b->variant) == 0;
}

int
main(void)
{
   struct agx_macos_device_capabilities first_capabilities;
   struct agx_macos_device_info first_info;
   struct agx_macos_device_session session;
   enum agx_macos_device_profile first_profile;
   char first_service_name[sizeof(session.device.service_name)] = {0};
   struct agx_macos_device_info known_profile = {
      .chip_id = 0x6040,
      .gpu_generation = 16,
      .core_count = 20,
      .cluster_count = 2,
      .usc_generation = 3,
      .kickid_queue_shift = 40,
      .kickid_queue_mask = 0x7f,
      .parameter_buffer_max_size = 1,
      .variant = "S",
   };

   if (agx_macos_device_detect_profile(&known_profile) !=
          AGX_MACOS_DEVICE_PROFILE_T6040_G16S_USC3 ||
       agx_macos_device_detect_profile(NULL) !=
          AGX_MACOS_DEVICE_PROFILE_UNSUPPORTED) {
      fputs("AGX profile gate does not classify known inputs\n", stderr);
      return 1;
   }

   for (unsigned cycle = 0; cycle < 2; ++cycle) {
      if (agx_macos_device_session_open(&session) !=
          AGX_MACOS_DEVICE_SESSION_READY) {
         fputs("failed to open a profiled AGX IOKit user client\n", stderr);
         return 1;
      }

      if (cycle == 0) {
         first_info = session.info;
         first_capabilities = session.capabilities;
         first_profile = session.profile;
         snprintf(first_service_name, sizeof(first_service_name), "%s",
                  session.device.service_name);
      } else if (!same_device_info(&first_info, &session.info) ||
                 memcmp(&first_capabilities, &session.capabilities,
                        sizeof(session.capabilities)) != 0 ||
                 strcmp(first_service_name, session.device.service_name) != 0 ||
                 first_profile != session.profile) {
         fputs("AGX device re-open changed profile or capabilities\n", stderr);
         agx_macos_device_session_close(&session);
         return 1;
      }

      printf("AGX_MACOS_DEVICE_REOPEN cycle=%u service=%s profile=%s "
             "chip=t%04x G%u%s cores=%u clusters=%u capabilities=%u\n",
             cycle + 1, session.device.service_name,
             agx_macos_device_profile_name(session.profile),
             session.info.chip_id, session.info.gpu_generation,
             session.info.variant, session.info.core_count,
             session.info.cluster_count, AGX_MACOS_DEVICE_CAPABILITIES_SIZE);
      agx_macos_device_session_close(&session);
   }

   puts("AGX_MACOS_DEVICE_REOPEN complete cycles=2");
   return 0;
}
