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

   if (!agx_macos_device_session_is_current(session) || !out_bootstrap ||
       out_bootstrap->initialized) {
      return kIOReturnBadArgument;
   }

   result = agx_macos_bo_set_init(&bootstrap.bo_set, session);
   if (result != KERN_SUCCESS)
      return result;
   bootstrap.bo_set_initialized = true;

   result = agx_macos_notification_queue_create(session,
                                                 &bootstrap.notification_queue);
   if (result != KERN_SUCCESS)
      goto fail;
   bootstrap.notification_queue_initialized = true;

   result = agx_macos_command_infrastructure_init(
      session, &bootstrap.command_infrastructure);
   if (result != KERN_SUCCESS)
      goto fail;
   bootstrap.command_infrastructure_initialized = true;

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
   return bootstrap && bootstrap->initialized &&
          agx_macos_device_session_is_current(bootstrap->session) &&
          bootstrap->session->api_generation == bootstrap->api_generation &&
          bootstrap->bo_set_initialized &&
          agx_macos_bo_set_is_current(&bootstrap->bo_set, bootstrap->session) &&
          bootstrap->notification_queue_initialized &&
          agx_macos_notification_queue_is_current(
             bootstrap->session, &bootstrap->notification_queue) &&
          bootstrap->command_infrastructure_initialized &&
          agx_macos_command_infrastructure_is_current(
             bootstrap->session, &bootstrap->command_infrastructure) &&
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
agx_macos_screen_bootstrap_resize_offscreen(
   struct agx_macos_screen_bootstrap *bootstrap, uint32_t width,
   uint32_t height)
{
   if (!agx_macos_screen_bootstrap_is_ready(bootstrap))
      return kIOReturnBadArgument;

   return agx_macos_iosurface_recreate_rgba8(&bootstrap->offscreen, width,
                                              height);
}

kern_return_t
agx_macos_screen_bootstrap_acquire_offscreen_lease(
   const struct agx_macos_screen_bootstrap *bootstrap,
   struct agx_macos_iosurface_lease *out_lease)
{
   if (!agx_macos_screen_bootstrap_is_ready(bootstrap))
      return kIOReturnNotReady;

   return agx_macos_iosurface_acquire_lease(&bootstrap->offscreen, out_lease);
}

bool
agx_macos_screen_bootstrap_offscreen_lease_is_current(
   const struct agx_macos_screen_bootstrap *bootstrap,
   const struct agx_macos_iosurface_lease *lease)
{
   return agx_macos_screen_bootstrap_is_ready(bootstrap) &&
          agx_macos_iosurface_lease_is_current(&bootstrap->offscreen, lease);
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
   if (bootstrap->offscreen_initialized &&
       !agx_macos_iosurface_is_idle(&bootstrap->offscreen)) {
      return kIOReturnBusy;
   }

   if (bootstrap->notification_queue_initialized) {
      result = agx_macos_notification_queue_destroy(&bootstrap->notification_queue);
      if (result != KERN_SUCCESS)
         return result;
      bootstrap->notification_queue_initialized = false;
   }

   /* Command-pair replies do not have a traced destroy selector. They are
    * generation-scoped values, so discard them only after queue teardown. */
   bootstrap->command_infrastructure =
      (struct agx_macos_command_infrastructure){0};
   bootstrap->command_infrastructure_initialized = false;

   if (bootstrap->bo_set_initialized) {
      result = agx_macos_bo_set_cleanup(&bootstrap->bo_set);
      if (result != KERN_SUCCESS)
         return result;
      bootstrap->bo_set_initialized = false;
   }

   if (bootstrap->offscreen_initialized) {
      result = agx_macos_iosurface_destroy(&bootstrap->offscreen);
      if (result != KERN_SUCCESS)
         return result;
      bootstrap->offscreen_initialized = false;
   }

   *bootstrap = (struct agx_macos_screen_bootstrap){0};
   return KERN_SUCCESS;
}
