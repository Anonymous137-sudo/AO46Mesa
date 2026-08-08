/*
 * Copyright 2026 Khronos_AppleICDs contributors
 * SPDX-License-Identifier: MIT
 */

#include "agx_macos_device.h"

#include <CoreFoundation/CoreFoundation.h>
#include <errno.h>
#include <mach/mach.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define AGX_MACOS_SELECTOR_DEVICE_CAPABILITIES 0
#define AGX_MACOS_SELECTOR_SET_API 7
#define AGX_MACOS_SET_API_PATH_END 0x400

static kern_return_t agx_macos_device_call(
   struct agx_macos_device *device, uint32_t selector,
   const uint64_t *input, uint32_t input_count, const void *input_struct,
   size_t input_struct_count, uint64_t *output, uint32_t *output_count,
   void *output_struct, size_t *output_struct_count);

bool
agx_macos_device_open(struct agx_macos_device *device)
{
   io_iterator_t iterator = IO_OBJECT_NULL;
   CFMutableDictionaryRef matching;
   kern_return_t result;

   if (!device)
      return false;

   *device = (struct agx_macos_device){.connection = IO_OBJECT_NULL};
   matching = IOServiceMatching("AGXAccelerator");
   if (!matching)
      return false;

   result = IOServiceGetMatchingServices(kIOMainPortDefault, matching, &iterator);
   if (result != KERN_SUCCESS)
      return false;

   for (io_registry_entry_t entry = IOIteratorNext(iterator);
        entry != IO_OBJECT_NULL; entry = IOIteratorNext(iterator)) {
      io_name_t name = {0};
      io_connect_t connection = IO_OBJECT_NULL;

      result = IOServiceOpen(entry, mach_task_self(), AGX_MACOS_SERVICE_TYPE,
                             &connection);
      if (result == KERN_SUCCESS) {
         device->connection = connection;
         if (IORegistryEntryGetName(entry, name) == KERN_SUCCESS)
            snprintf(device->service_name, sizeof(device->service_name), "%s",
                     name);

         device->service = entry;
         IOObjectRelease(iterator);
         return true;
      }

      IOObjectRelease(entry);
   }

   IOObjectRelease(iterator);
   return false;
}

static bool
agx_macos_read_u32(CFDictionaryRef dictionary, CFStringRef key, uint32_t *value)
{
   CFTypeRef property = CFDictionaryGetValue(dictionary, key);

   return property && CFGetTypeID(property) == CFNumberGetTypeID() &&
          CFNumberGetValue(property, kCFNumberSInt32Type, value);
}

static bool
agx_macos_read_service_u64(io_registry_entry_t service, CFStringRef key,
                           uint64_t *value)
{
   CFTypeRef property =
      IORegistryEntryCreateCFProperty(service, key, kCFAllocatorDefault, 0);
   bool valid = property && CFGetTypeID(property) == CFNumberGetTypeID() &&
                CFNumberGetValue(property, kCFNumberSInt64Type, value);

   if (property)
      CFRelease(property);

   return valid;
}

static bool
agx_macos_read_core_masks(CFDictionaryRef dictionary,
                          struct agx_macos_device_info *info)
{
   CFTypeRef property =
      CFDictionaryGetValue(dictionary, CFSTR("core_mask_list"));
   CFIndex count;

   if (!property || CFGetTypeID(property) != CFArrayGetTypeID())
      return false;

   count = CFArrayGetCount(property);
   if (count <= 0 || count > AGX_MACOS_MAX_CLUSTERS)
      return false;

   for (CFIndex i = 0; i < count; ++i) {
      CFTypeRef mask = CFArrayGetValueAtIndex(property, i);

      if (!mask || CFGetTypeID(mask) != CFNumberGetTypeID() ||
          !CFNumberGetValue(mask, kCFNumberSInt64Type, &info->core_masks[i]))
         return false;
   }

   info->cluster_count = count;
   for (unsigned i = 0; i < info->cluster_count; ++i) {
      uint32_t width = 0;
      uint64_t mask = info->core_masks[i];

      while (mask) {
         ++width;
         mask >>= 1;
      }

      if (width > info->cores_per_cluster)
         info->cores_per_cluster = width;
   }

   return info->cores_per_cluster != 0;
}

static bool
agx_macos_validate_topology(const struct agx_macos_device_info *info)
{
   uint32_t active_core_count = 0;

   for (unsigned i = 0; i < info->cluster_count; ++i) {
      uint64_t mask = info->core_masks[i];

      if (!mask)
         return false;

      while (mask) {
         active_core_count += mask & 1;
         mask >>= 1;
      }
   }

   return active_core_count == info->core_count;
}

static bool
agx_macos_read_chip_id(io_registry_entry_t service, uint32_t *chip_id)
{
   CFTypeRef property;
   char name[128] = {0};
   char *end;
   unsigned long value;

   property = IORegistryEntryCreateCFProperty(
      service, CFSTR("IONameMatched"), kCFAllocatorDefault, 0);
   if (!property || CFGetTypeID(property) != CFStringGetTypeID())
      goto out;

   if (!CFStringGetCString(property, name, sizeof(name), kCFStringEncodingUTF8) ||
       strncmp(name, "gpu,t", strlen("gpu,t")) != 0)
      goto out;

   errno = 0;
   value = strtoul(name + strlen("gpu,t"), &end, 16);
   if (errno != 0 || *end != '\0' || value > UINT32_MAX)
      goto out;

   *chip_id = value;
   CFRelease(property);
   return true;

out:
   if (property)
      CFRelease(property);
   return false;
}

bool
agx_macos_device_query_info(const struct agx_macos_device *device,
                            struct agx_macos_device_info *info)
{
   CFTypeRef configuration;
   CFTypeRef variant;

   if (!device || device->service == IO_OBJECT_NULL || !info)
      return false;

   *info = (struct agx_macos_device_info){};
   if (!agx_macos_read_chip_id(device->service, &info->chip_id))
      return false;

   configuration = IORegistryEntryCreateCFProperty(
      device->service, CFSTR("GPUConfigurationVariable"), kCFAllocatorDefault,
      0);
   if (!configuration || CFGetTypeID(configuration) != CFDictionaryGetTypeID()) {
      if (configuration)
         CFRelease(configuration);
      return false;
   }

   bool valid =
      agx_macos_read_u32(configuration, CFSTR("gpu_gen"),
                         &info->gpu_generation) &&
      agx_macos_read_u32(configuration, CFSTR("num_cores"),
                         &info->core_count) &&
      agx_macos_read_core_masks(configuration, info);
   agx_macos_read_u32(configuration, CFSTR("num_mgpus"),
                      &info->gpu_partition_count);
   agx_macos_read_u32(configuration, CFSTR("num_frags"),
                      &info->fragment_core_count);
   agx_macos_read_u32(configuration, CFSTR("usc_gen"), &info->usc_generation);
   valid &= agx_macos_read_u32(configuration, CFSTR("kickid_qid_shift"),
                               &info->kickid_queue_shift) &&
            agx_macos_read_u32(configuration, CFSTR("kickid_qid_mask"),
                               &info->kickid_queue_mask) &&
            agx_macos_read_service_u64(
               device->service, CFSTR("AGXParameterBufferMaxSize"),
               &info->parameter_buffer_max_size) &&
            agx_macos_validate_topology(info);
   variant = CFDictionaryGetValue(configuration, CFSTR("gpu_var"));
   if (variant && CFGetTypeID(variant) == CFStringGetTypeID()) {
      valid &= CFStringGetCString(variant, info->variant, sizeof(info->variant),
                                  kCFStringEncodingUTF8);
   }

   CFRelease(configuration);
   return valid;
}

bool
agx_macos_device_query_capabilities(
   struct agx_macos_device *device,
   struct agx_macos_device_capabilities *capabilities)
{
   size_t size = sizeof(*capabilities);

   if (!capabilities)
      return false;

   memset(capabilities, 0, sizeof(*capabilities));
   return agx_macos_device_call(
             device, AGX_MACOS_SELECTOR_DEVICE_CAPABILITIES, NULL, 0, NULL, 0,
             NULL, NULL, capabilities, &size) == KERN_SUCCESS &&
          size == sizeof(*capabilities);
}

enum agx_macos_device_profile
agx_macos_device_detect_profile(const struct agx_macos_device_info *info)
{
   if (!info)
      return AGX_MACOS_DEVICE_PROFILE_UNSUPPORTED;

   if (info->chip_id == 0x6040 && info->gpu_generation == 16 &&
       strcmp(info->variant, "S") == 0 && info->usc_generation == 3 &&
       info->kickid_queue_shift == 40 && info->kickid_queue_mask == 0x7f &&
       info->core_count > 0 && info->cluster_count > 0 &&
       info->parameter_buffer_max_size > 0) {
      return AGX_MACOS_DEVICE_PROFILE_T6040_G16S_USC3;
   }

   return AGX_MACOS_DEVICE_PROFILE_UNSUPPORTED;
}

const char *
agx_macos_device_profile_name(enum agx_macos_device_profile profile)
{
   switch (profile) {
   case AGX_MACOS_DEVICE_PROFILE_T6040_G16S_USC3:
      return "t6040-g16s-usc3";
   case AGX_MACOS_DEVICE_PROFILE_UNSUPPORTED:
      return "unsupported";
   }

   return "invalid";
}

void
agx_macos_device_close(struct agx_macos_device *device)
{
   if (!device || device->connection == IO_OBJECT_NULL)
      return;

   IOServiceClose(device->connection);
   IOObjectRelease(device->service);
   *device = (struct agx_macos_device){.connection = IO_OBJECT_NULL};
}

enum agx_macos_device_session_status
agx_macos_device_session_open(struct agx_macos_device_session *session)
{
   if (!session)
      return AGX_MACOS_DEVICE_SESSION_UNSUPPORTED;

   *session = (struct agx_macos_device_session){
      .device = {.connection = IO_OBJECT_NULL},
      .profile = AGX_MACOS_DEVICE_PROFILE_UNSUPPORTED,
   };

   if (!agx_macos_device_open(&session->device))
      return AGX_MACOS_DEVICE_SESSION_NO_DEVICE;

   if (!agx_macos_device_query_info(&session->device, &session->info) ||
       !agx_macos_device_query_capabilities(&session->device,
                                             &session->capabilities)) {
      agx_macos_device_session_close(session);
      return AGX_MACOS_DEVICE_SESSION_UNSUPPORTED;
   }

   session->profile = agx_macos_device_detect_profile(&session->info);
   if (session->profile == AGX_MACOS_DEVICE_PROFILE_UNSUPPORTED) {
      agx_macos_device_session_close(session);
      return AGX_MACOS_DEVICE_SESSION_UNSUPPORTED;
   }

   return AGX_MACOS_DEVICE_SESSION_READY;
}

void
agx_macos_device_session_close(struct agx_macos_device_session *session)
{
   if (!session)
      return;

   agx_macos_device_close(&session->device);
   *session = (struct agx_macos_device_session){
      .device = {.connection = IO_OBJECT_NULL},
      .profile = AGX_MACOS_DEVICE_PROFILE_UNSUPPORTED,
   };
}

static kern_return_t
agx_macos_device_call(struct agx_macos_device *device, uint32_t selector,
                      const uint64_t *input, uint32_t input_count,
                      const void *input_struct, size_t input_struct_count,
                      uint64_t *output, uint32_t *output_count,
                      void *output_struct, size_t *output_struct_count)
{
   if (!device || device->connection == IO_OBJECT_NULL)
      return kIOReturnNotOpen;

   return IOConnectCallMethod(
      device->connection, selector, input, input_count, input_struct,
      input_struct_count, output, output_count, output_struct,
      output_struct_count);
}

kern_return_t
agx_macos_device_session_configure_traced_api(
   struct agx_macos_device_session *session, const char *client_path)
{
   struct {
      uint64_t configured;
      uint64_t opaque;
   } response = {0};
   uint8_t request[AGX_MACOS_SET_API_PAYLOAD_SIZE] = {0};
   size_t response_size = sizeof(response);
   size_t path_length;
   uint64_t api_version = 2;
   int32_t route = -1;
   uint32_t enabled = 1;
   kern_return_t result;

   if (!session || !client_path ||
       session->profile != AGX_MACOS_DEVICE_PROFILE_T6040_G16S_USC3 ||
       session->device.connection == IO_OBJECT_NULL) {
      return kIOReturnBadArgument;
   }

   path_length = strnlen(client_path, AGX_MACOS_SET_API_PATH_END);
   if (path_length == 0 || path_length == AGX_MACOS_SET_API_PATH_END)
      return kIOReturnBadArgument;

   /* The observed payload stores the NUL-terminated client path twice, with
    * the second copy ending exactly at the stable 16-byte trailer. */
   memcpy(request, client_path, path_length + 1);
   memcpy(request + AGX_MACOS_SET_API_PATH_END - (path_length + 1), client_path,
          path_length + 1);
   memcpy(request + AGX_MACOS_SET_API_PATH_END, &api_version,
          sizeof(api_version));
   memcpy(request + AGX_MACOS_SET_API_PATH_END + sizeof(api_version), &route,
          sizeof(route));
   memcpy(request + AGX_MACOS_SET_API_PATH_END + sizeof(api_version) +
             sizeof(route),
          &enabled, sizeof(enabled));

   result = agx_macos_device_call(&session->device, AGX_MACOS_SELECTOR_SET_API,
                                  NULL, 0, request, sizeof(request), NULL, NULL,
                                  &response, &response_size);
   if (result != KERN_SUCCESS)
      return result;
   if (response_size != sizeof(response) || response.configured == 0 ||
       response.configured <= session->api_generation) {
      return kIOReturnBadArgument;
   }

   session->api_configured = true;
   session->api_generation = response.configured;
   return KERN_SUCCESS;
}
