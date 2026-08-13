/*
 * Copyright 2026 Khronos_AppleICDs contributors
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <IOKit/IOKitLib.h>

/* The public IOKit user-client type observed by Mesa's macOS AGX tracer. */
#define AGX_MACOS_SERVICE_TYPE 0x100005
#define AGX_MACOS_MAX_CLUSTERS 64
#define AGX_MACOS_DEVICE_CAPABILITIES_SIZE 64
#define AGX_MACOS_SET_API_PAYLOAD_SIZE 0x410

enum agx_macos_device_profile {
   AGX_MACOS_DEVICE_PROFILE_UNSUPPORTED = 0,
   AGX_MACOS_DEVICE_PROFILE_T6040_G16S_USC3,
};

/* Session state is separate from the IOKit connection so a device-loss event
 * can reject new work while resource and queue teardown retain their handles. */
enum agx_macos_device_session_state {
   AGX_MACOS_DEVICE_SESSION_STATE_CLOSED = 0,
   AGX_MACOS_DEVICE_SESSION_STATE_OPEN,
   AGX_MACOS_DEVICE_SESSION_STATE_CONFIGURED,
   AGX_MACOS_DEVICE_SESSION_STATE_LOST,
};

struct agx_macos_device_capabilities {
   uint8_t data[AGX_MACOS_DEVICE_CAPABILITIES_SIZE];
};

struct agx_macos_device_info {
   uint32_t chip_id;
   uint32_t gpu_generation;
   uint32_t core_count;
   uint32_t cluster_count;
   uint32_t cores_per_cluster;
   uint32_t gpu_partition_count;
   uint32_t fragment_core_count;
   uint32_t usc_generation;
   uint32_t kickid_queue_shift;
   uint32_t kickid_queue_mask;
   uint64_t parameter_buffer_max_size;
   uint64_t core_masks[AGX_MACOS_MAX_CLUSTERS];
   char variant[16];
};

struct agx_macos_device {
   io_connect_t connection;
   io_registry_entry_t service;
   char service_name[128];
};

/* A profiled session owns one direct AGX user-client connection and the
 * immutable hardware/capability record validated when it was opened. */
struct agx_macos_device_session {
   struct agx_macos_device device;
   struct agx_macos_device_info info;
   struct agx_macos_device_capabilities capabilities;
   enum agx_macos_device_profile profile;
   enum agx_macos_device_session_state state;
   bool api_configured;
   uint64_t api_generation;
};

enum agx_macos_device_session_status {
   AGX_MACOS_DEVICE_SESSION_NO_DEVICE = 0,
   AGX_MACOS_DEVICE_SESSION_UNSUPPORTED,
   AGX_MACOS_DEVICE_SESSION_READY,
};

bool agx_macos_device_open(struct agx_macos_device *device);
bool agx_macos_device_query_info(const struct agx_macos_device *device,
                                 struct agx_macos_device_info *info);
bool agx_macos_device_query_capabilities(
   struct agx_macos_device *device,
   struct agx_macos_device_capabilities *capabilities);
enum agx_macos_device_profile agx_macos_device_detect_profile(
   const struct agx_macos_device_info *info);
const char *agx_macos_device_profile_name(enum agx_macos_device_profile profile);
void agx_macos_device_close(struct agx_macos_device *device);
enum agx_macos_device_session_status agx_macos_device_session_open(
   struct agx_macos_device_session *session);
void agx_macos_device_session_close(struct agx_macos_device_session *session);
bool agx_macos_device_session_is_open(
   const struct agx_macos_device_session *session);
bool agx_macos_device_session_is_current(
   const struct agx_macos_device_session *session);
bool agx_macos_device_session_is_lost(
   const struct agx_macos_device_session *session);
bool agx_macos_device_session_mark_lost(struct agx_macos_device_session *session);
kern_return_t agx_macos_device_session_configure_traced_api(
   struct agx_macos_device_session *session, const char *client_path);
