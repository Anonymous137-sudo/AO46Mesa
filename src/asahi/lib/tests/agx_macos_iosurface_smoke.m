/*
 * Copyright 2026 Khronos_AppleICDs contributors
 * SPDX-License-Identifier: MIT
 */

#include "agx_macos_iosurface.h"

#include <stdio.h>
#include <string.h>

int
main(void)
{
   struct agx_macos_iosurface surface = {0};
   const uint8_t *bytes;
   uint8_t *write_bytes;
   uint32_t row_bytes;
   struct agx_macos_iosurface_token original_token = {0};
   struct agx_macos_iosurface_token resized_token = {0};
   struct agx_macos_iosurface_snapshot original_snapshot = {0};
   struct agx_macos_iosurface_snapshot resized_snapshot = {0};
   struct agx_macos_iosurface_lease lease = {0};

   if (agx_macos_iosurface_create_rgba8(64, 64, &surface) != KERN_SUCCESS ||
       !surface.surface || surface.width != 64 || surface.height != 64 ||
       surface.bytes_per_row != 256 ||
       surface.generation != 1 ||
       surface.pixel_format != AGX_MACOS_IOSURFACE_PIXEL_FORMAT_RGBA8 ||
       agx_macos_iosurface_get_id(&surface) == 0 ||
       !agx_macos_iosurface_capture_token(&surface, &original_token) ||
       !agx_macos_iosurface_capture_presentable_snapshot(
          &surface, &original_snapshot) ||
       original_snapshot.token.id != original_token.id ||
       original_snapshot.token.generation != original_token.generation ||
       original_snapshot.width != 64 || original_snapshot.height != 64 ||
       original_snapshot.bytes_per_row != 256 ||
       original_snapshot.pixel_format != AGX_MACOS_IOSURFACE_PIXEL_FORMAT_RGBA8 ||
       !agx_macos_iosurface_token_is_current(&surface, &original_token) ||
       !agx_macos_iosurface_token_can_present(&surface, &original_token) ||
       agx_macos_iosurface_acquire_lease(&surface, &lease) != KERN_SUCCESS ||
       !agx_macos_iosurface_lease_is_current(&surface, &lease) ||
       agx_macos_iosurface_create_rgba8(64, 64, &surface) !=
          kIOReturnBadArgument ||
       agx_macos_iosurface_map_write(&surface, &write_bytes, &row_bytes) !=
       KERN_SUCCESS ||
       !write_bytes || row_bytes != 256 ||
       agx_macos_iosurface_lease_is_current(&surface, &lease) ||
       agx_macos_iosurface_token_can_present(&surface, &original_token) ||
       agx_macos_iosurface_capture_presentable_snapshot(
          &surface, &original_snapshot) ||
       agx_macos_iosurface_destroy(&surface) != kIOReturnBusy ||
       agx_macos_iosurface_map_read(&surface, &bytes, &row_bytes) !=
          kIOReturnBadArgument ||
       agx_macos_iosurface_recreate_rgba8(&surface, 128, 32) !=
          kIOReturnBadArgument) {
      fputs("AGX_MACOS_IOSURFACE_SMOKE write lifecycle failed\n", stderr);
      agx_macos_iosurface_release_lease(&lease);
      agx_macos_iosurface_destroy(&surface);
      return 1;
   }

   memset(write_bytes, 0x5a, 64 * 64 * 4);
   if (agx_macos_iosurface_unmap_write(&surface) != KERN_SUCCESS ||
       agx_macos_iosurface_map_read(&surface, &bytes, &row_bytes) !=
          KERN_SUCCESS ||
       agx_macos_iosurface_map_read(&surface, &bytes, &row_bytes) !=
          kIOReturnBadArgument ||
       agx_macos_iosurface_map_write(&surface, &write_bytes, &row_bytes) !=
          kIOReturnBadArgument ||
       !bytes || row_bytes != 256 || bytes[0] != 0x5a ||
       agx_macos_iosurface_token_can_present(&surface, &original_token) ||
       agx_macos_iosurface_destroy(&surface) != kIOReturnBusy ||
       agx_macos_iosurface_recreate_rgba8(&surface, 128, 32) !=
          kIOReturnBadArgument ||
       agx_macos_iosurface_unmap_read(&surface) != KERN_SUCCESS) {
      fputs("AGX_MACOS_IOSURFACE_SMOKE basic lifecycle failed\n", stderr);
      agx_macos_iosurface_release_lease(&lease);
      agx_macos_iosurface_destroy(&surface);
      return 1;
   }

   if (agx_macos_iosurface_recreate_rgba8(&surface, 128, 32) != KERN_SUCCESS ||
       !surface.surface || surface.width != 128 || surface.height != 32 ||
       surface.bytes_per_row != 512 || surface.generation != 2 ||
       agx_macos_iosurface_get_id(&surface) == 0 ||
       agx_macos_iosurface_lease_is_current(&surface, &lease) ||
       agx_macos_iosurface_token_is_current(&surface, &original_token) ||
       !agx_macos_iosurface_capture_token(&surface, &resized_token) ||
       !agx_macos_iosurface_capture_presentable_snapshot(
          &surface, &resized_snapshot) ||
       resized_snapshot.token.id != resized_token.id ||
       resized_snapshot.token.generation != resized_token.generation ||
       resized_snapshot.width != 128 || resized_snapshot.height != 32 ||
       resized_snapshot.bytes_per_row != 512 ||
       !agx_macos_iosurface_token_is_current(&surface, &resized_token) ||
       !agx_macos_iosurface_token_can_present(&surface, &resized_token) ||
       agx_macos_iosurface_recreate_rgba8(&surface, 0, 32) !=
          kIOReturnBadArgument ||
       surface.width != 128 || surface.height != 32 ||
       surface.bytes_per_row != 512 || surface.generation != 2) {
      fputs("AGX_MACOS_IOSURFACE_SMOKE resize lifecycle failed\n", stderr);
      agx_macos_iosurface_release_lease(&lease);
      agx_macos_iosurface_destroy(&surface);
      return 1;
   }

   agx_macos_iosurface_release_lease(&lease);
   if (agx_macos_iosurface_acquire_lease(&surface, &lease) != KERN_SUCCESS ||
       !agx_macos_iosurface_lease_is_current(&surface, &lease)) {
      fputs("AGX_MACOS_IOSURFACE_SMOKE lease rebind failed\n", stderr);
      agx_macos_iosurface_release_lease(&lease);
      agx_macos_iosurface_destroy(&surface);
      return 1;
   }

   if (agx_macos_iosurface_destroy(&surface) != KERN_SUCCESS || surface.surface ||
       surface.width || surface.height ||
       agx_macos_iosurface_get_id(&surface) != 0 ||
       agx_macos_iosurface_capture_token(&surface, &original_token) ||
       agx_macos_iosurface_capture_presentable_snapshot(&surface,
                                                         &original_snapshot) ||
       agx_macos_iosurface_token_is_current(&surface, &resized_token) ||
       agx_macos_iosurface_token_can_present(&surface, &resized_token) ||
       agx_macos_iosurface_lease_is_current(&surface, &lease) ||
       agx_macos_iosurface_unmap_read(&surface) != kIOReturnBadArgument ||
       agx_macos_iosurface_unmap_write(&surface) != kIOReturnBadArgument ||
       agx_macos_iosurface_create_rgba8(0, 64, &surface) !=
          kIOReturnBadArgument ||
       agx_macos_iosurface_create_rgba8(UINT32_MAX, 1, &surface) !=
          kIOReturnBadArgument) {
      fputs("AGX_MACOS_IOSURFACE_SMOKE validation failed\n", stderr);
      agx_macos_iosurface_release_lease(&lease);
      agx_macos_iosurface_destroy(&surface);
      return 1;
   }

   agx_macos_iosurface_release_lease(&lease);

   puts("AGX_MACOS_IOSURFACE_SMOKE complete");
   return 0;
}
