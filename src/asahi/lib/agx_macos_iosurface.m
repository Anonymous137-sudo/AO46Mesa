/*
 * Copyright 2026 Khronos_AppleICDs contributors
 * SPDX-License-Identifier: MIT
 */

#import <Foundation/Foundation.h>

#include "agx_macos_iosurface.h"

#include <limits.h>

kern_return_t
agx_macos_iosurface_create_rgba8(uint32_t width, uint32_t height,
                                 struct agx_macos_iosurface *out_surface)
{
   uint32_t bytes_per_row;
   IOSurfaceRef surface;

   if (!out_surface || out_surface->surface || width == 0 || height == 0 ||
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

   *out_surface = (struct agx_macos_iosurface){
      .surface = surface,
      .width = width,
      .height = height,
      .bytes_per_row = bytes_per_row,
      .pixel_format = AGX_MACOS_IOSURFACE_PIXEL_FORMAT_RGBA8,
      .generation = 1,
   };
   return KERN_SUCCESS;
}

uint32_t
agx_macos_iosurface_get_id(const struct agx_macos_iosurface *surface)
{
   return surface && surface->surface ? IOSurfaceGetID(surface->surface) : 0;
}

bool
agx_macos_iosurface_capture_token(
   const struct agx_macos_iosurface *surface,
   struct agx_macos_iosurface_token *out_token)
{
   uint32_t id = agx_macos_iosurface_get_id(surface);

   if (!out_token)
      return false;

   *out_token = (struct agx_macos_iosurface_token){0};
   if (!surface || !id || !surface->generation)
      return false;

   *out_token = (struct agx_macos_iosurface_token){
      .id = id,
      .generation = surface->generation,
   };
   return true;
}

bool
agx_macos_iosurface_token_is_current(
   const struct agx_macos_iosurface *surface,
   const struct agx_macos_iosurface_token *token)
{
   return surface && token && token->id != 0 && token->generation != 0 &&
          token->id == agx_macos_iosurface_get_id(surface) &&
          token->generation == surface->generation;
}

bool
agx_macos_iosurface_token_can_present(
   const struct agx_macos_iosurface *surface,
   const struct agx_macos_iosurface_token *token)
{
   return agx_macos_iosurface_token_is_current(surface, token) &&
          !surface->read_locked && !surface->write_locked;
}

kern_return_t
agx_macos_iosurface_recreate_rgba8(struct agx_macos_iosurface *surface,
                                   uint32_t width, uint32_t height)
{
   struct agx_macos_iosurface replacement = {0};
   uint64_t next_generation;
   kern_return_t result;

   if (!surface || !surface->surface || surface->read_locked ||
       surface->write_locked ||
       surface->generation == UINT64_MAX)
      return kIOReturnBadArgument;

   next_generation = surface->generation + 1;
   result = agx_macos_iosurface_create_rgba8(width, height, &replacement);
   if (result != KERN_SUCCESS)
      return result;

   agx_macos_iosurface_destroy(surface);
   replacement.generation = next_generation;
   *surface = replacement;
   return KERN_SUCCESS;
}

void
agx_macos_iosurface_destroy(struct agx_macos_iosurface *surface)
{
   if (!surface)
      return;

   if (surface->surface && surface->read_locked)
      (void)IOSurfaceUnlock(surface->surface, kIOSurfaceLockReadOnly, NULL);
   if (surface->surface && surface->write_locked)
      (void)IOSurfaceUnlock(surface->surface, 0, NULL);
   if (surface->surface)
      CFRelease(surface->surface);

   *surface = (struct agx_macos_iosurface){0};
}

kern_return_t
agx_macos_iosurface_map_read(struct agx_macos_iosurface *surface,
                             const uint8_t **out_bytes,
                             uint32_t *out_bytes_per_row)
{
   kern_return_t result;
   void *bytes;
   size_t row_bytes;

   if (!surface || !surface->surface || surface->read_locked ||
       surface->write_locked || !out_bytes || !out_bytes_per_row) {
      return kIOReturnBadArgument;
   }

   result = IOSurfaceLock(surface->surface, kIOSurfaceLockReadOnly, NULL);
   if (result != KERN_SUCCESS)
      return result;

   bytes = IOSurfaceGetBaseAddress(surface->surface);
   row_bytes = IOSurfaceGetBytesPerRow(surface->surface);
   if (!bytes || row_bytes != surface->bytes_per_row || row_bytes > UINT32_MAX) {
      (void)IOSurfaceUnlock(surface->surface, kIOSurfaceLockReadOnly, NULL);
      return kIOReturnBadArgument;
   }

   *out_bytes = bytes;
   *out_bytes_per_row = row_bytes;
   surface->read_locked = true;
   return KERN_SUCCESS;
}

kern_return_t
agx_macos_iosurface_unmap_read(struct agx_macos_iosurface *surface)
{
   kern_return_t result;

   if (!surface || !surface->surface || !surface->read_locked)
      return kIOReturnBadArgument;

   result = IOSurfaceUnlock(surface->surface, kIOSurfaceLockReadOnly, NULL);
   if (result == KERN_SUCCESS)
      surface->read_locked = false;
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

   if (!surface || !surface->surface || surface->read_locked ||
       surface->write_locked || !out_bytes || !out_bytes_per_row) {
      return kIOReturnBadArgument;
   }

   result = IOSurfaceLock(surface->surface, 0, NULL);
   if (result != KERN_SUCCESS)
      return result;

   bytes = IOSurfaceGetBaseAddress(surface->surface);
   row_bytes = IOSurfaceGetBytesPerRow(surface->surface);
   if (!bytes || row_bytes != surface->bytes_per_row || row_bytes > UINT32_MAX) {
      (void)IOSurfaceUnlock(surface->surface, 0, NULL);
      return kIOReturnBadArgument;
   }

   *out_bytes = bytes;
   *out_bytes_per_row = row_bytes;
   surface->write_locked = true;
   return KERN_SUCCESS;
}

kern_return_t
agx_macos_iosurface_unmap_write(struct agx_macos_iosurface *surface)
{
   kern_return_t result;

   if (!surface || !surface->surface || !surface->write_locked)
      return kIOReturnBadArgument;

   result = IOSurfaceUnlock(surface->surface, 0, NULL);
   if (result == KERN_SUCCESS)
      surface->write_locked = false;
   return result;
}
