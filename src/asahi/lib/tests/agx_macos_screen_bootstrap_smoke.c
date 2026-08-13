/*
 * Copyright 2026 Khronos_AppleICDs contributors
 * SPDX-License-Identifier: MIT
 */

#include "agx_macos_screen_bootstrap.h"
#include "agx_macos_submission_lease.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int
main(int argc, char **argv)
{
   struct agx_macos_device_session session;
   struct agx_macos_screen_bootstrap bootstrap = {0};
   struct agx_macos_device_session stale_session;
   struct agx_macos_bo bo = {.connection = IO_OBJECT_NULL};
   struct agx_macos_bo_mapping mapping = {0};
   struct agx_macos_submission_lease lease = {0};
   struct agx_macos_iosurface_lease drawable_lease = {0};
   static const struct agx_macos_submit_descriptor_observed descriptor = {
      .header0 = AGX_MACOS_SUBMISSION_DESCRIPTOR_HEADER0,
      .header1 = AGX_MACOS_SUBMISSION_DESCRIPTOR_HEADER1,
      .completion_tokens = {0x0102030405060708ull, 0x1112131415161718ull},
   };
   uint8_t carrier[AGX_MACOS_SUBMISSION_CARRIER_AUXILIARY_OFFSET +
                   AGX_MACOS_SUBMISSION_CARRIER_EXTENDED_READABLE_PREFIX] = {0};
   const uint64_t opaque_pointer_slot = UINT64_C(0x1ff800000);
   struct agx_macos_submission_range range;
   const uint8_t *pixels;
   uint8_t *write_pixels;
   uint32_t row_bytes;
   struct agx_macos_iosurface_token drawable_token = {0};
   kern_return_t result;

   if (!getenv("AGX_MACOS_EXPERIMENTAL_SCREEN_BOOTSTRAP")) {
      puts("AGX_MACOS_SCREEN_BOOTSTRAP_SMOKE skipped; set "
           "AGX_MACOS_EXPERIMENTAL_SCREEN_BOOTSTRAP=1 to run");
      return 0;
   }

   if (argc != 1 ||
       agx_macos_device_session_open(&session) !=
          AGX_MACOS_DEVICE_SESSION_READY ||
       agx_macos_screen_bootstrap_init(&session, 64, 32, &bootstrap) !=
          kIOReturnBadArgument ||
       agx_macos_device_session_configure_traced_api(&session, argv[0]) !=
          KERN_SUCCESS) {
      fputs("AGX_MACOS_SCREEN_BOOTSTRAP_SMOKE session setup failed\n", stderr);
      agx_macos_device_session_close(&session);
      return 1;
   }

   result = agx_macos_screen_bootstrap_init(&session, 64, 32, &bootstrap);
   if (result != KERN_SUCCESS) {
      fprintf(stderr, "AGX_MACOS_SCREEN_BOOTSTRAP_SMOKE init failed: %#x\n",
              result);
      goto fail;
   }
   if (!agx_macos_screen_bootstrap_is_ready(&bootstrap) ||
       bootstrap.offscreen.width != 64 || bootstrap.offscreen.height != 32 ||
       bootstrap.offscreen.generation != 1 ||
       !agx_macos_bo_set_is_current(&bootstrap.bo_set, &session) ||
       !agx_macos_notification_queue_is_current(&session,
                                                 &bootstrap.notification_queue) ||
       !agx_macos_command_infrastructure_is_current(
          &session, &bootstrap.command_infrastructure)) {
      fputs("AGX_MACOS_SCREEN_BOOTSTRAP_SMOKE readiness validation failed\n",
            stderr);
      goto fail;
   }
   if (!agx_macos_iosurface_capture_token(&bootstrap.offscreen,
                                           &drawable_token) ||
       !agx_macos_iosurface_token_can_present(&bootstrap.offscreen,
                                               &drawable_token)) {
      fputs("AGX_MACOS_SCREEN_BOOTSTRAP_SMOKE drawable token failed\n", stderr);
      goto fail;
   }
   if (agx_macos_screen_bootstrap_acquire_offscreen_lease(
          &bootstrap, &drawable_lease) != KERN_SUCCESS ||
       !agx_macos_screen_bootstrap_offscreen_lease_is_current(
          &bootstrap, &drawable_lease)) {
      fputs("AGX_MACOS_SCREEN_BOOTSTRAP_SMOKE drawable lease failed\n", stderr);
      goto fail;
   }
   result = agx_macos_screen_bootstrap_create_bo(
      &bootstrap, AGX_MACOS_BO_STORAGE_SHARED, 4096, 1, &bo);
   if (result != KERN_SUCCESS) {
      fprintf(stderr, "AGX_MACOS_SCREEN_BOOTSTRAP_SMOKE BO creation failed: %#x\n",
              result);
      goto fail;
   }
   result = agx_macos_iosurface_map_read(&bootstrap.offscreen, &pixels,
                                          &row_bytes);
   if (result != KERN_SUCCESS || !pixels || row_bytes != 256) {
      fprintf(stderr,
              "AGX_MACOS_SCREEN_BOOTSTRAP_SMOKE offscreen readback failed: %#x\n",
              result);
      goto fail;
   }
   result = agx_macos_iosurface_unmap_read(&bootstrap.offscreen);
   if (result != KERN_SUCCESS) {
      fprintf(stderr, "AGX_MACOS_SCREEN_BOOTSTRAP_SMOKE offscreen unlock failed: %#x\n",
              result);
      goto fail;
   }
   result = agx_macos_iosurface_map_read(&bootstrap.offscreen, &pixels,
                                          &row_bytes);
   if (result != KERN_SUCCESS ||
       agx_macos_screen_bootstrap_destroy(&bootstrap) != kIOReturnBusy ||
       !agx_macos_screen_bootstrap_is_ready(&bootstrap) ||
       agx_macos_screen_bootstrap_resize_offscreen(&bootstrap, 128, 64) !=
          kIOReturnBadArgument ||
       agx_macos_iosurface_unmap_read(&bootstrap.offscreen) != KERN_SUCCESS) {
      fputs("AGX_MACOS_SCREEN_BOOTSTRAP_SMOKE locked resize rejection failed\n",
            stderr);
      goto fail;
   }
   result = agx_macos_screen_bootstrap_resize_offscreen(&bootstrap, 128, 64);
   if (result != KERN_SUCCESS || bootstrap.offscreen.width != 128 ||
       bootstrap.offscreen.height != 64 || bootstrap.offscreen.generation != 2 ||
       agx_macos_screen_bootstrap_offscreen_lease_is_current(
          &bootstrap, &drawable_lease) ||
       agx_macos_iosurface_token_is_current(&bootstrap.offscreen,
                                             &drawable_token)) {
      fprintf(stderr, "AGX_MACOS_SCREEN_BOOTSTRAP_SMOKE resize failed: %#x\n",
              result);
      goto fail;
   }
   agx_macos_iosurface_release_lease(&drawable_lease);
   if (agx_macos_screen_bootstrap_acquire_offscreen_lease(
          &bootstrap, &drawable_lease) != KERN_SUCCESS ||
       !agx_macos_screen_bootstrap_offscreen_lease_is_current(
          &bootstrap, &drawable_lease)) {
      fputs("AGX_MACOS_SCREEN_BOOTSTRAP_SMOKE drawable lease rebind failed\n",
            stderr);
      goto fail;
   }
   if (!agx_macos_iosurface_capture_token(&bootstrap.offscreen,
                                           &drawable_token) ||
       !agx_macos_iosurface_token_can_present(&bootstrap.offscreen,
                                               &drawable_token)) {
      fputs("AGX_MACOS_SCREEN_BOOTSTRAP_SMOKE resized drawable token failed\n",
            stderr);
      goto fail;
   }
   result = agx_macos_iosurface_map_write(&bootstrap.offscreen, &write_pixels,
                                           &row_bytes);
   if (result != KERN_SUCCESS || !write_pixels || row_bytes != 512) {
      fprintf(stderr,
              "AGX_MACOS_SCREEN_BOOTSTRAP_SMOKE offscreen write failed: %#x\n",
              result);
      goto fail;
   }
   memset(write_pixels, 0x3c, row_bytes * bootstrap.offscreen.height);
   result = agx_macos_iosurface_unmap_write(&bootstrap.offscreen);
   if (result != KERN_SUCCESS) {
      fprintf(stderr,
              "AGX_MACOS_SCREEN_BOOTSTRAP_SMOKE offscreen write unlock failed: %#x\n",
              result);
      goto fail;
   }
   result = agx_macos_iosurface_map_read(&bootstrap.offscreen, &pixels,
                                          &row_bytes);
   if (result != KERN_SUCCESS || pixels[0] != 0x3c ||
       pixels[row_bytes * bootstrap.offscreen.height - 1] != 0x3c) {
      fprintf(stderr,
              "AGX_MACOS_SCREEN_BOOTSTRAP_SMOKE offscreen round-trip failed: %#x\n",
              result);
      goto fail;
   }
   result = agx_macos_iosurface_unmap_read(&bootstrap.offscreen);
   if (result != KERN_SUCCESS)
      goto fail;
   result = agx_macos_bo_set_map_range(&bootstrap.bo_set, bo.handle, 0, 16,
                                        &mapping);
   if (result != KERN_SUCCESS || !mapping.cpu) {
      fprintf(stderr,
              "AGX_MACOS_SCREEN_BOOTSTRAP_SMOKE managed BO map failed: %#x\n",
              result);
      goto fail;
   }
   memset(mapping.cpu, 0xa5, mapping.size);
   result = agx_macos_screen_bootstrap_destroy(&bootstrap);
   if (result != kIOReturnBusy ||
       !agx_macos_screen_bootstrap_is_ready(&bootstrap)) {
      fprintf(stderr,
              "AGX_MACOS_SCREEN_BOOTSTRAP_SMOKE mapped teardown was %#x\n",
              result);
      goto fail;
   }
   result = agx_macos_bo_set_unmap_range(&bootstrap.bo_set, &mapping);
   if (result != KERN_SUCCESS) {
      fprintf(stderr,
              "AGX_MACOS_SCREEN_BOOTSTRAP_SMOKE managed BO unmap failed: %#x\n",
              result);
      goto fail;
   }
   result = agx_macos_bo_set_retain_submission(&bootstrap.bo_set, bo.handle);
   if (result != KERN_SUCCESS) {
      fprintf(stderr, "AGX_MACOS_SCREEN_BOOTSTRAP_SMOKE BO pin failed: %#x\n",
              result);
      goto fail;
   }
   result = agx_macos_screen_bootstrap_destroy(&bootstrap);
   if (result != kIOReturnBusy) {
      fprintf(stderr,
              "AGX_MACOS_SCREEN_BOOTSTRAP_SMOKE busy teardown was %#x\n",
              result);
      goto fail;
   }
   result = agx_macos_bo_set_release_submission(&bootstrap.bo_set, bo.handle);
   if (result != KERN_SUCCESS) {
      fprintf(stderr, "AGX_MACOS_SCREEN_BOOTSTRAP_SMOKE BO unpin failed: %#x\n",
              result);
      goto fail;
   }
   memcpy(carrier, &descriptor, sizeof(descriptor));
   memcpy(carrier + AGX_MACOS_SUBMISSION_CARRIER_AUXILIARY_OFFSET +
             AGX_MACOS_SUBMISSION_CARRIER_OPAQUE_POINTER_SLOT_OFFSET,
          &opaque_pointer_slot, sizeof(opaque_pointer_slot));
   range = (struct agx_macos_submission_range){.gpu_va = bo.gpu_va, .size = 16};
   result = agx_macos_submission_lease_init_from_carrier(
      &lease, &bootstrap.bo_set, bootstrap.notification_queue.id, carrier,
      sizeof(descriptor),
      carrier + AGX_MACOS_SUBMISSION_CARRIER_AUXILIARY_OFFSET,
      AGX_MACOS_SUBMISSION_CARRIER_EXTENDED_READABLE_PREFIX, &range, 1);
   if (result != KERN_SUCCESS || !lease.has_carrier_snapshot ||
       lease.carrier_snapshot.opaque_pointer_slot != opaque_pointer_slot ||
       lease.handle_count != 1) {
      fprintf(stderr,
              "AGX_MACOS_SCREEN_BOOTSTRAP_SMOKE carrier admission failed: %#x\n",
              result);
      goto fail;
   }
   stale_session = session;
   stale_session.api_generation++;
   if (agx_macos_notification_queue_bind_lease(
          &stale_session, &bootstrap.notification_queue, &lease) !=
          kIOReturnBadArgument ||
       agx_macos_notification_queue_bind_lease(
          &session, &bootstrap.notification_queue, &lease) != KERN_SUCCESS) {
      fputs("AGX_MACOS_SCREEN_BOOTSTRAP_SMOKE queue generation admission failed\n",
            stderr);
      goto fail;
   }
   result = agx_macos_submission_lease_mark_submitted(&lease);
   if (result != KERN_SUCCESS) {
      fprintf(stderr,
              "AGX_MACOS_SCREEN_BOOTSTRAP_SMOKE carrier transition failed: %#x\n",
              result);
      goto fail;
   }
   result = agx_macos_screen_bootstrap_destroy(&bootstrap);
   if (result != kIOReturnBusy || !agx_macos_screen_bootstrap_is_ready(&bootstrap)) {
      fprintf(stderr,
              "AGX_MACOS_SCREEN_BOOTSTRAP_SMOKE in-flight teardown was %#x\n",
              result);
      goto fail;
   }
   result = agx_macos_submission_lease_abandon_after_device_loss(&lease);
   if (result != kIOReturnBusy) {
      fprintf(stderr,
              "AGX_MACOS_SCREEN_BOOTSTRAP_SMOKE abandoned a live lease: %#x\n",
              result);
      goto fail;
   }

   /* An invalidated session is the required precondition for loss retirement. */
   ++session.api_generation;
   result = agx_macos_submission_lease_abandon_after_device_loss(&lease);
   if (result != KERN_SUCCESS) {
      fprintf(stderr,
              "AGX_MACOS_SCREEN_BOOTSTRAP_SMOKE carrier abandonment failed: %#x\n",
              result);
      goto fail;
   }
   result = agx_macos_bo_set_destroy(&bootstrap.bo_set, &bo);
   if (result != KERN_SUCCESS) {
      fprintf(stderr, "AGX_MACOS_SCREEN_BOOTSTRAP_SMOKE BO destruction failed: %#x\n",
              result);
      goto fail;
   }
   result = agx_macos_screen_bootstrap_destroy(&bootstrap);
   if (result != KERN_SUCCESS) {
      fprintf(stderr, "AGX_MACOS_SCREEN_BOOTSTRAP_SMOKE teardown failed: %#x\n",
              result);
      goto fail;
   }

   agx_macos_iosurface_release_lease(&drawable_lease);
   agx_macos_device_session_close(&session);
   puts("AGX_MACOS_SCREEN_BOOTSTRAP_SMOKE complete");
   return 0;

fail:
   agx_macos_iosurface_release_lease(&drawable_lease);
   if (mapping.active && bootstrap.bo_set_initialized)
      (void)agx_macos_bo_set_unmap_range(&bootstrap.bo_set, &mapping);
   if (lease.active) {
      if (lease.state == AGX_MACOS_SUBMISSION_LEASE_IN_FLIGHT)
         (void)agx_macos_submission_lease_abandon_after_device_loss(&lease);
      else
         (void)agx_macos_submission_lease_release(&lease);
   }
   if (bo.connection != IO_OBJECT_NULL && bootstrap.bo_set_initialized)
      (void)agx_macos_bo_set_destroy(&bootstrap.bo_set, &bo);
   if (bootstrap.initialized)
      (void)agx_macos_screen_bootstrap_destroy(&bootstrap);
   agx_macos_device_session_close(&session);
   return 1;
}
