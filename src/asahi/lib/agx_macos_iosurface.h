/*
 * Copyright 2026 Khronos_AppleICDs contributors
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <IOSurface/IOSurface.h>
#include <mach/kern_return.h>

#include <stdbool.h>
#include <stdint.h>

#define AGX_MACOS_IOSURFACE_PIXEL_FORMAT_RGBA8 0x52474241u

/* An IOSurface is the macOS shareable-drawable primitive. It intentionally
 * contains no Metal or AGX command-submission state. */
struct agx_macos_iosurface {
   IOSurfaceRef surface;
   uint32_t width;
   uint32_t height;
   uint32_t bytes_per_row;
   uint32_t pixel_format;
   uint64_t generation;
   bool read_locked;
   bool write_locked;
};

/* A presentation consumer captures this before handing the drawable to a
 * later stage. Recreate invalidates the token even if IOSurface reuses an ID. */
struct agx_macos_iosurface_token {
   uint32_t id;
   uint64_t generation;
};

kern_return_t agx_macos_iosurface_create_rgba8(
   uint32_t width, uint32_t height, struct agx_macos_iosurface *out_surface);
/* Returns the shareable IOSurface identity used by a future presentation or
 * AGX import path, or zero for an invalid drawable. */
uint32_t agx_macos_iosurface_get_id(
   const struct agx_macos_iosurface *surface);
bool agx_macos_iosurface_capture_token(
   const struct agx_macos_iosurface *surface,
   struct agx_macos_iosurface_token *out_token);
bool agx_macos_iosurface_token_is_current(
   const struct agx_macos_iosurface *surface,
   const struct agx_macos_iosurface_token *token);
/* A future present path may hand off only the current, unlocked drawable. */
bool agx_macos_iosurface_token_can_present(
   const struct agx_macos_iosurface *surface,
   const struct agx_macos_iosurface_token *token);
/* Replaces an unlocked drawable only after its replacement has been created.
 * This is the resize primitive used before CAMetalLayer presentation exists. */
kern_return_t agx_macos_iosurface_recreate_rgba8(
   struct agx_macos_iosurface *surface, uint32_t width, uint32_t height);
void agx_macos_iosurface_destroy(struct agx_macos_iosurface *surface);
kern_return_t agx_macos_iosurface_map_read(
   struct agx_macos_iosurface *surface, const uint8_t **out_bytes,
   uint32_t *out_bytes_per_row);
kern_return_t agx_macos_iosurface_unmap_read(
   struct agx_macos_iosurface *surface);
/* Acquires mutable IOSurface storage. A drawable may have at most one read or
 * write mapping at a time, keeping resize and presentation handoff explicit. */
kern_return_t agx_macos_iosurface_map_write(
   struct agx_macos_iosurface *surface, uint8_t **out_bytes,
   uint32_t *out_bytes_per_row);
kern_return_t agx_macos_iosurface_unmap_write(
   struct agx_macos_iosurface *surface);
