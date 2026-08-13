/*
 * Copyright 2026 Khronos_AppleICDs contributors
 * SPDX-License-Identifier: MIT
 */

#import <Foundation/Foundation.h>

#include "agx_macos_iosurface.h"

#include <limits.h>
#include <pthread.h>

/* Drawable lifecycle calls are infrequent. A single lock keeps the current
 * winsys model safe while the future screen owns higher-level scheduling. */
static pthread_mutex_t agx_macos_iosurface_lock = PTHREAD_MUTEX_INITIALIZER;

static kern_return_t
agx_macos_iosurface_create_rgba8_raw(uint32_t width, uint32_t height,
                                     IOSurfaceRef *out_surface,
                                     uint32_t *out_bytes_per_row)
{
   uint32_t bytes_per_row;
   IOSurfaceRef surface;

   if (!out_surface || !out_bytes_per_row || width == 0 || height == 0 ||
       width > UINT32_MAX / 4) {
      return kIOReturnBadArgument;
   }

   bytes_per_row = width * 4;
   surface = IOSurfaceCreate((CFDictionaryRef)@{
      (NSString *)kIOSurfaceWidth : @(width),
      (NSString *)kIOSurfaceHeight : @(height),
      (NSString *)kIOSurfaceBytesPerElement : @4,
      (NSString *)kIOSurfaceBytesPerRow : @(bytes_per_row),
      (NSString *)kIOSurfacePixelFormat : @(AGX_MACOS_IOSURFACE_PIXEL_FORMAT_RGBA8),
   });
   if (!surface)
      return kIOReturnNoMemory;

   *out_surface = surface;
   *out_bytes_per_row = bytes_per_row;
   return KERN_SUCCESS;
}

static bool
agx_macos_iosurface_capture_token_raw(
   const struct agx_macos_iosurface *surface,
   struct agx_macos_iosurface_token *out_token)
{
   uint32_t id;

   if (!surface || !surface->surface || !surface->generation || !out_token)
      return false;

   id = IOSurfaceGetID(surface->surface);
   if (!id)
      return false;

   *out_token = (struct agx_macos_iosurface_token){
      .id = id,
      .generation = surface->generation,
   };
   return true;
}

static bool
agx_macos_iosurface_token_is_current_raw(
   const struct agx_macos_iosurface *surface,
   const struct agx_macos_iosurface_token *token)
{
   return surface && surface->surface && token && token->id != 0 &&
          token->generation != 0 &&
          token->id == IOSurfaceGetID(surface->surface) &&
          token->generation == surface->generation;
}

static bool
agx_macos_iosurface_token_can_present_raw(
   const struct agx_macos_iosurface *surface,
   const struct agx_macos_iosurface_token *token)
{
   return agx_macos_iosurface_token_is_current_raw(surface, token) &&
          !surface->read_locked && !surface->write_locked;
}

kern_return_t
agx_macos_iosurface_create_rgba8(uint32_t width, uint32_t height,
                                 struct agx_macos_iosurface *out_surface)
{
   IOSurfaceRef surface;
   uint32_t bytes_per_row;
   kern_return_t result;

   if (!out_surface)
      return kIOReturnBadArgument;

   pthread_mutex_lock(&agx_macos_iosurface_lock);
   if (out_surface->surface) {
      pthread_mutex_unlock(&agx_macos_iosurface_lock);
      return kIOReturnBadArgument;
   }

   result = agx_macos_iosurface_create_rgba8_raw(width, height, &surface,
                                                  &bytes_per_row);
   if (result == KERN_SUCCESS) {
      *out_surface = (struct agx_macos_iosurface){
         .surface = surface,
         .width = width,
         .height = height,
         .bytes_per_row = bytes_per_row,
         .pixel_format = AGX_MACOS_IOSURFACE_PIXEL_FORMAT_RGBA8,
         .generation = 1,
      };
   }
   pthread_mutex_unlock(&agx_macos_iosurface_lock);
   return result;
}

uint32_t
agx_macos_iosurface_get_id(const struct agx_macos_iosurface *surface)
{
   uint32_t id = 0;

   pthread_mutex_lock(&agx_macos_iosurface_lock);
   if (surface && surface->surface)
      id = IOSurfaceGetID(surface->surface);
   pthread_mutex_unlock(&agx_macos_iosurface_lock);
   return id;
}

bool
agx_macos_iosurface_capture_token(
   const struct agx_macos_iosurface *surface,
   struct agx_macos_iosurface_token *out_token)
{
   bool captured;

   if (!out_token)
      return false;

   *out_token = (struct agx_macos_iosurface_token){0};
   pthread_mutex_lock(&agx_macos_iosurface_lock);
   captured = agx_macos_iosurface_capture_token_raw(surface, out_token);
   pthread_mutex_unlock(&agx_macos_iosurface_lock);
   return captured;
}

bool
agx_macos_iosurface_token_is_current(
   const struct agx_macos_iosurface *surface,
   const struct agx_macos_iosurface_token *token)
{
   bool current;

   pthread_mutex_lock(&agx_macos_iosurface_lock);
   current = agx_macos_iosurface_token_is_current_raw(surface, token);
   pthread_mutex_unlock(&agx_macos_iosurface_lock);
   return current;
}

bool
agx_macos_iosurface_token_can_present(
   const struct agx_macos_iosurface *surface,
   const struct agx_macos_iosurface_token *token)
{
   bool presentable;

   pthread_mutex_lock(&agx_macos_iosurface_lock);
   presentable = agx_macos_iosurface_token_can_present_raw(surface, token);
   pthread_mutex_unlock(&agx_macos_iosurface_lock);
   return presentable;
}

bool
agx_macos_iosurface_capture_presentable_snapshot(
   const struct agx_macos_iosurface *surface,
   struct agx_macos_iosurface_snapshot *out_snapshot)
{
   struct agx_macos_iosurface_token token;
   bool captured = false;

   if (!out_snapshot)
      return false;

   *out_snapshot = (struct agx_macos_iosurface_snapshot){0};
   pthread_mutex_lock(&agx_macos_iosurface_lock);
   if (agx_macos_iosurface_capture_token_raw(surface, &token) &&
       agx_macos_iosurface_token_can_present_raw(surface, &token)) {
      *out_snapshot = (struct agx_macos_iosurface_snapshot){
         .token = token,
         .width = surface->width,
         .height = surface->height,
         .bytes_per_row = surface->bytes_per_row,
         .pixel_format = surface->pixel_format,
      };
      captured = true;
   }
   pthread_mutex_unlock(&agx_macos_iosurface_lock);
   return captured;
}

kern_return_t
agx_macos_iosurface_acquire_lease(
   const struct agx_macos_iosurface *surface,
   struct agx_macos_iosurface_lease *out_lease)
{
   struct agx_macos_iosurface_token token;

   if (!out_lease || out_lease->active || out_lease->surface)
      return kIOReturnBadArgument;

   *out_lease = (struct agx_macos_iosurface_lease){0};
   pthread_mutex_lock(&agx_macos_iosurface_lock);
   if (!agx_macos_iosurface_capture_token_raw(surface, &token) ||
       !agx_macos_iosurface_token_can_present_raw(surface, &token)) {
      pthread_mutex_unlock(&agx_macos_iosurface_lock);
      return kIOReturnNotReady;
   }

   CFRetain(surface->surface);
   *out_lease = (struct agx_macos_iosurface_lease){
      .surface = surface->surface,
      .token = token,
      .width = surface->width,
      .height = surface->height,
      .bytes_per_row = surface->bytes_per_row,
      .pixel_format = surface->pixel_format,
      .active = true,
   };
   pthread_mutex_unlock(&agx_macos_iosurface_lock);
   return KERN_SUCCESS;
}

bool
agx_macos_iosurface_lease_is_current(
   const struct agx_macos_iosurface *surface,
   const struct agx_macos_iosurface_lease *lease)
{
   bool current;

   pthread_mutex_lock(&agx_macos_iosurface_lock);
   current = surface && lease && lease->active && lease->surface &&
             IOSurfaceGetID(lease->surface) == lease->token.id &&
             agx_macos_iosurface_token_can_present_raw(surface, &lease->token) &&
             lease->width == surface->width && lease->height == surface->height &&
             lease->bytes_per_row == surface->bytes_per_row &&
             lease->pixel_format == surface->pixel_format;
   pthread_mutex_unlock(&agx_macos_iosurface_lock);
   return current;
}

void
agx_macos_iosurface_release_lease(struct agx_macos_iosurface_lease *lease)
{
   if (!lease)
      return;

   if (lease->surface)
      CFRelease(lease->surface);
   *lease = (struct agx_macos_iosurface_lease){0};
}

bool
agx_macos_iosurface_is_idle(const struct agx_macos_iosurface *surface)
{
   bool idle;

   pthread_mutex_lock(&agx_macos_iosurface_lock);
   idle = surface && surface->surface && !surface->read_locked &&
          !surface->write_locked;
   pthread_mutex_unlock(&agx_macos_iosurface_lock);
   return idle;
}

kern_return_t
agx_macos_iosurface_recreate_rgba8(struct agx_macos_iosurface *surface,
                                   uint32_t width, uint32_t height)
{
   IOSurfaceRef replacement;
   IOSurfaceRef old_surface;
   uint32_t bytes_per_row;
   kern_return_t result;
   uint64_t next_generation;

   if (!surface)
      return kIOReturnBadArgument;

   pthread_mutex_lock(&agx_macos_iosurface_lock);
   if (!surface->surface || surface->read_locked || surface->write_locked ||
       surface->generation == UINT64_MAX) {
      pthread_mutex_unlock(&agx_macos_iosurface_lock);
      return kIOReturnBadArgument;
   }

   result = agx_macos_iosurface_create_rgba8_raw(width, height, &replacement,
                                                  &bytes_per_row);
   if (result != KERN_SUCCESS) {
      pthread_mutex_unlock(&agx_macos_iosurface_lock);
      return result;
   }

   old_surface = surface->surface;
   next_generation = surface->generation + 1;
   *surface = (struct agx_macos_iosurface){
      .surface = replacement,
      .width = width,
      .height = height,
      .bytes_per_row = bytes_per_row,
      .pixel_format = AGX_MACOS_IOSURFACE_PIXEL_FORMAT_RGBA8,
      .generation = next_generation,
   };
   CFRelease(old_surface);
   pthread_mutex_unlock(&agx_macos_iosurface_lock);
   return KERN_SUCCESS;
}

kern_return_t
agx_macos_iosurface_destroy(struct agx_macos_iosurface *surface)
{
   if (!surface)
      return kIOReturnBadArgument;

   pthread_mutex_lock(&agx_macos_iosurface_lock);
   if (!surface->surface) {
      pthread_mutex_unlock(&agx_macos_iosurface_lock);
      return kIOReturnBadArgument;
   }
   if (surface->read_locked || surface->write_locked) {
      pthread_mutex_unlock(&agx_macos_iosurface_lock);
      return kIOReturnBusy;
   }

   CFRelease(surface->surface);
   *surface = (struct agx_macos_iosurface){0};
   pthread_mutex_unlock(&agx_macos_iosurface_lock);
   return KERN_SUCCESS;
}

kern_return_t
agx_macos_iosurface_map_read(struct agx_macos_iosurface *surface,
                             const uint8_t **out_bytes,
                             uint32_t *out_bytes_per_row)
{
   kern_return_t result;
   void *bytes;
   size_t row_bytes;

   if (!surface || !out_bytes || !out_bytes_per_row)
      return kIOReturnBadArgument;

   pthread_mutex_lock(&agx_macos_iosurface_lock);
   if (!surface->surface || surface->read_locked || surface->write_locked) {
      pthread_mutex_unlock(&agx_macos_iosurface_lock);
      return kIOReturnBadArgument;
   }

   result = IOSurfaceLock(surface->surface, kIOSurfaceLockReadOnly, NULL);
   if (result != KERN_SUCCESS) {
      pthread_mutex_unlock(&agx_macos_iosurface_lock);
      return result;
   }

   bytes = IOSurfaceGetBaseAddress(surface->surface);
   row_bytes = IOSurfaceGetBytesPerRow(surface->surface);
   if (!bytes || row_bytes != surface->bytes_per_row || row_bytes > UINT32_MAX) {
      (void)IOSurfaceUnlock(surface->surface, kIOSurfaceLockReadOnly, NULL);
      pthread_mutex_unlock(&agx_macos_iosurface_lock);
      return kIOReturnBadArgument;
   }

   *out_bytes = bytes;
   *out_bytes_per_row = row_bytes;
   surface->read_locked = true;
   pthread_mutex_unlock(&agx_macos_iosurface_lock);
   return KERN_SUCCESS;
}

kern_return_t
agx_macos_iosurface_unmap_read(struct agx_macos_iosurface *surface)
{
   kern_return_t result;

   if (!surface)
      return kIOReturnBadArgument;

   pthread_mutex_lock(&agx_macos_iosurface_lock);
   if (!surface->surface || !surface->read_locked) {
      pthread_mutex_unlock(&agx_macos_iosurface_lock);
      return kIOReturnBadArgument;
   }

   result = IOSurfaceUnlock(surface->surface, kIOSurfaceLockReadOnly, NULL);
   if (result == KERN_SUCCESS)
      surface->read_locked = false;
   pthread_mutex_unlock(&agx_macos_iosurface_lock);
   return result;
}

kern_return_t
agx_macos_iosurface_map_write(struct agx_macos_iosurface *surface,
                              uint8_t **out_bytes,
                              uint32_t *out_bytes_per_row)
{
   kern_return_t result;
   void *bytes;
   size_t row_bytes;

   if (!surface || !out_bytes || !out_bytes_per_row)
      return kIOReturnBadArgument;

   pthread_mutex_lock(&agx_macos_iosurface_lock);
   if (!surface->surface || surface->read_locked || surface->write_locked) {
      pthread_mutex_unlock(&agx_macos_iosurface_lock);
      return kIOReturnBadArgument;
   }

   result = IOSurfaceLock(surface->surface, 0, NULL);
   if (result != KERN_SUCCESS) {
      pthread_mutex_unlock(&agx_macos_iosurface_lock);
      return result;
   }

   bytes = IOSurfaceGetBaseAddress(surface->surface);
   row_bytes = IOSurfaceGetBytesPerRow(surface->surface);
   if (!bytes || row_bytes != surface->bytes_per_row || row_bytes > UINT32_MAX) {
      (void)IOSurfaceUnlock(surface->surface, 0, NULL);
      pthread_mutex_unlock(&agx_macos_iosurface_lock);
      return kIOReturnBadArgument;
   }

   *out_bytes = bytes;
   *out_bytes_per_row = row_bytes;
   surface->write_locked = true;
   pthread_mutex_unlock(&agx_macos_iosurface_lock);
   return KERN_SUCCESS;
}

kern_return_t
agx_macos_iosurface_unmap_write(struct agx_macos_iosurface *surface)
{
   kern_return_t result;

   if (!surface)
      return kIOReturnBadArgument;

   pthread_mutex_lock(&agx_macos_iosurface_lock);
   if (!surface->surface || !surface->write_locked) {
      pthread_mutex_unlock(&agx_macos_iosurface_lock);
      return kIOReturnBadArgument;
   }

   result = IOSurfaceUnlock(surface->surface, 0, NULL);
   if (result == KERN_SUCCESS)
      surface->write_locked = false;
   pthread_mutex_unlock(&agx_macos_iosurface_lock);
   return result;
}
