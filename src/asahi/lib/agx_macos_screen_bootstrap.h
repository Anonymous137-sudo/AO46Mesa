/*
 * Copyright 2026 Khronos_AppleICDs contributors
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include "agx_macos_bo.h"
#include "agx_macos_command.h"
#include "agx_macos_iosurface.h"
#include "agx_macos_queue.h"

/* This owns the macOS-native prerequisites of a future Mesa Asahi screen.
 * It deliberately does not create a pipe_screen until the direct submission
 * ABI is validated. */
struct agx_macos_screen_bootstrap {
   const struct agx_macos_device_session *session;
   struct agx_macos_bo_set bo_set;
   struct agx_macos_notification_queue notification_queue;
   struct agx_macos_command_infrastructure command_infrastructure;
   struct agx_macos_iosurface offscreen;
   uint64_t api_generation;
   bool bo_set_initialized;
   bool notification_queue_initialized;
   bool command_infrastructure_initialized;
   bool offscreen_initialized;
   bool initialized;
};

kern_return_t agx_macos_screen_bootstrap_init(
   const struct agx_macos_device_session *session, uint32_t offscreen_width,
   uint32_t offscreen_height, struct agx_macos_screen_bootstrap *out_bootstrap);

bool agx_macos_screen_bootstrap_is_ready(
   const struct agx_macos_screen_bootstrap *bootstrap);

kern_return_t agx_macos_screen_bootstrap_create_bo(
   struct agx_macos_screen_bootstrap *bootstrap,
   enum agx_macos_bo_storage storage, uint64_t minimum_size,
   uint64_t alignment, struct agx_macos_bo *out_bo);

/* Replaces the owned offscreen drawable transactionally. Consumers holding an
 * old IOSurface token must rebind before presentation can proceed. */
kern_return_t agx_macos_screen_bootstrap_resize_offscreen(
   struct agx_macos_screen_bootstrap *bootstrap, uint32_t width,
   uint32_t height);

/* The future native Mesa screen uses this to own an IOSurface without taking
 * a borrowed bootstrap pointer across asynchronous rendering or resize. */
kern_return_t agx_macos_screen_bootstrap_acquire_offscreen_lease(
   const struct agx_macos_screen_bootstrap *bootstrap,
   struct agx_macos_iosurface_lease *out_lease);
bool agx_macos_screen_bootstrap_offscreen_lease_is_current(
   const struct agx_macos_screen_bootstrap *bootstrap,
   const struct agx_macos_iosurface_lease *lease);

/* Teardown is resumable: on an error, successfully destroyed components stay
 * cleared and the caller can retry or close the owning session. */
kern_return_t agx_macos_screen_bootstrap_destroy(
   struct agx_macos_screen_bootstrap *bootstrap);
