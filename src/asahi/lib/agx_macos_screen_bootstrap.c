/*
 * Copyright 2026 Khronos_AppleICDs contributors
 * SPDX-License-Identifier: MIT
 */

#include "agx_macos_screen_bootstrap.h"

static bool
agx_macos_bo_set_is_idle(struct agx_macos_bo_set *set)
{
   bool idle = true;

   if (!set || !set->initialized)
      return false;

   pthread_mutex_lock(&set->lock);
   for (unsigned i = 0; i < AGX_MACOS_BO_SET_CAPACITY; ++i) {
      if (set->entries[i].in_flight_count != 0 ||
          set->entries[i].cpu_map_count != 0) {
         idle = false;
         break;
      }
   }
   pthread_mutex_unlock(&set->lock);
   return idle;
}

kern_return_t
agx_macos_screen_bootstrap_init(
   const struct agx_macos_device_session *session, uint32_t offscreen_width,
   uint32_t offscreen_height, struct agx_macos_screen_bootstrap *out_bootstrap)
{
   struct agx_macos_screen_bootstrap bootstrap = {0};
   kern_return_t result;

   if (!session || !out_bootstrap || out_bootstrap->initialized ||
       session->profile != AGX_MACOS_DEVICE_PROFILE_T6040_G16S_USC3 ||
       session->device.connection == IO_OBJECT_NULL || !session->api_configured ||
       session->api_generation == 0) {
      return kIOReturnBadArgument;
   }

   result = agx_macos_bo_set_init(&bootstrap.bo_set, session);
   if (result != KERN_SUCCESS)
      return result;
   bootstrap.bo_set_initialized = true;

   result = agx_macos_command_infrastructure_init(
      session, &bootstrap.command_infrastructure);
   if (result != KERN_SUCCESS)
      goto fail;
   bootstrap.command_infrastructure_initialized = true;

   result = agx_macos_notification_queue_create(session,
                                                 &bootstrap.notification_queue);
   if (result != KERN_SUCCESS)
      goto fail;
   bootstrap.notification_queue_initialized = true;

   result = agx_macos_iosurface_create_rgba8(offscreen_width, offscreen_height,
                                              &bootstrap.offscreen);
   if (result != KERN_SUCCESS)
      goto fail;
   bootstrap.offscreen_initialized = true;

   bootstrap.session = session;
   bootstrap.api_generation = session->api_generation;
   bootstrap.initialized = true;
   *out_bootstrap = bootstrap;
   return KERN_SUCCESS;

fail:
   if (bootstrap.notification_queue_initialized)
      (void)agx_macos_notification_queue_destroy(&bootstrap.notification_queue);
   if (bootstrap.bo_set_initialized)
      (void)agx_macos_bo_set_cleanup(&bootstrap.bo_set);
   return result;
}

bool
agx_macos_screen_bootstrap_is_ready(
   const struct agx_macos_screen_bootstrap *bootstrap)
{
   return bootstrap && bootstrap->initialized && bootstrap->session &&
          bootstrap->session->api_configured &&
          bootstrap->session->api_generation == bootstrap->api_generation &&
          bootstrap->bo_set_initialized && bootstrap->bo_set.initialized &&
          bootstrap->command_infrastructure_initialized &&
          bootstrap->command_infrastructure.initialized &&
          bootstrap->command_infrastructure.api_generation ==
             bootstrap->api_generation &&
          bootstrap->notification_queue_initialized &&
          bootstrap->notification_queue.release_state ==
             AGX_MACOS_NOTIFICATION_QUEUE_ACTIVE &&
          bootstrap->offscreen_initialized && bootstrap->offscreen.surface;
}

kern_return_t
agx_macos_screen_bootstrap_create_bo(
   struct agx_macos_screen_bootstrap *bootstrap,
   enum agx_macos_bo_storage storage, uint64_t minimum_size,
   uint64_t alignment, struct agx_macos_bo *out_bo)
{
   if (!agx_macos_screen_bootstrap_is_ready(bootstrap) || !out_bo)
      return kIOReturnBadArgument;

   return agx_macos_bo_set_create_at_least(&bootstrap->bo_set, storage,
                                            minimum_size, alignment, out_bo);
}

kern_return_t
agx_macos_screen_bootstrap_destroy(
   struct agx_macos_screen_bootstrap *bootstrap)
{
   kern_return_t result;

   if (!bootstrap || !bootstrap->initialized)
      return kIOReturnBadArgument;

   if (bootstrap->bo_set_initialized &&
       !agx_macos_bo_set_is_idle(&bootstrap->bo_set)) {
      return kIOReturnBusy;
   }

   if (bootstrap->notification_queue_initialized) {
      result = agx_macos_notification_queue_destroy(&bootstrap->notification_queue);
      if (result != KERN_SUCCESS)
         return result;
      bootstrap->notification_queue_initialized = false;
   }

   if (bootstrap->bo_set_initialized) {
      result = agx_macos_bo_set_cleanup(&bootstrap->bo_set);
      if (result != KERN_SUCCESS)
         return result;
      bootstrap->bo_set_initialized = false;
   }

   if (bootstrap->offscreen_initialized) {
      agx_macos_iosurface_destroy(&bootstrap->offscreen);
      bootstrap->offscreen_initialized = false;
   }

   /* The opaque command-pair setup is scoped to the AGX session. Its observed
    * teardown is session close, after this bootstrap has released its BOs and
    * notification queue. */
   bootstrap->command_infrastructure_initialized = false;

   *bootstrap = (struct agx_macos_screen_bootstrap){0};
   return KERN_SUCCESS;
}
