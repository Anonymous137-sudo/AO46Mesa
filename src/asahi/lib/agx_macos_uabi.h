/*
 * Copyright 2026 Khronos_AppleICDs contributors
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include <IOKit/IOReturn.h>

#include "agx_macos_device.h"

/*
 * The macOS UABI contract is intentionally smaller than an AGX driver. It is
 * the versioned interface between Mesa's platform glue and the macOS-specific
 * implementation, so Gallium does not depend on IOKit selector details.
 */
#define AGX_MACOS_UABI_VERSION_MAJOR 1u
#define AGX_MACOS_UABI_VERSION_MINOR 0u

enum agx_macos_uabi_operation {
   AGX_MACOS_UABI_OPERATION_DEVICE_SESSION = 1u << 0,
   AGX_MACOS_UABI_OPERATION_API_CONFIGURATION = 1u << 1,
   AGX_MACOS_UABI_OPERATION_BO_ALLOCATE = 1u << 2,
   AGX_MACOS_UABI_OPERATION_BO_CPU_MAP = 1u << 3,
   AGX_MACOS_UABI_OPERATION_FIXED_VA_BIND = 1u << 4,
   AGX_MACOS_UABI_OPERATION_COMMAND_INFRASTRUCTURE = 1u << 5,
   AGX_MACOS_UABI_OPERATION_NOTIFICATION_QUEUE = 1u << 6,
   AGX_MACOS_UABI_OPERATION_COMPLETION_POLL = 1u << 7,
   AGX_MACOS_UABI_OPERATION_VM_BIND = 1u << 8,
   AGX_MACOS_UABI_OPERATION_RESOURCE_BIND = 1u << 9,
   AGX_MACOS_UABI_OPERATION_CARRIER_BUILD = 1u << 10,
   AGX_MACOS_UABI_OPERATION_BATCH_SUBMIT = 1u << 11,
   /* Mesa's timestamp and query objects use bo_bind_object. A future screen
    * must not be admitted until this contract is implemented as well. */
   AGX_MACOS_UABI_OPERATION_OBJECT_BIND = 1u << 12,
   /* Asahi shader binaries live in the USC low-VA region. A direct allocator
    * cannot claim a usable Gallium screen until both the allocation class and
    * its low-VA mapping contract are independently available. */
   AGX_MACOS_UABI_OPERATION_LOW_VA_BIND = 1u << 13,
   AGX_MACOS_UABI_OPERATION_EXECUTABLE_BO = 1u << 14,
   /* A restricted code aperture must only receive a resource produced by the
    * Apple compiler-result, relocation, and code-resource lifecycle. Mapping
    * support alone is insufficient to admit Mesa/Asahi executable bytes. */
   AGX_MACOS_UABI_OPERATION_SHADER_CODE_ADMISSION = 1u << 15,
};

#define AGX_MACOS_UABI_OPERATION_ALL                                      \
   (AGX_MACOS_UABI_OPERATION_DEVICE_SESSION |                             \
    AGX_MACOS_UABI_OPERATION_API_CONFIGURATION |                          \
    AGX_MACOS_UABI_OPERATION_BO_ALLOCATE |                                \
    AGX_MACOS_UABI_OPERATION_BO_CPU_MAP |                                 \
    AGX_MACOS_UABI_OPERATION_FIXED_VA_BIND |                              \
    AGX_MACOS_UABI_OPERATION_COMMAND_INFRASTRUCTURE |                     \
    AGX_MACOS_UABI_OPERATION_NOTIFICATION_QUEUE |                         \
    AGX_MACOS_UABI_OPERATION_COMPLETION_POLL |                            \
    AGX_MACOS_UABI_OPERATION_VM_BIND |                                    \
    AGX_MACOS_UABI_OPERATION_RESOURCE_BIND |                              \
    AGX_MACOS_UABI_OPERATION_CARRIER_BUILD |                              \
    AGX_MACOS_UABI_OPERATION_BATCH_SUBMIT |                                \
    AGX_MACOS_UABI_OPERATION_OBJECT_BIND |                                 \
    AGX_MACOS_UABI_OPERATION_LOW_VA_BIND |                                 \
    AGX_MACOS_UABI_OPERATION_EXECUTABLE_BO |                               \
    AGX_MACOS_UABI_OPERATION_SHADER_CODE_ADMISSION)

#define AGX_MACOS_UABI_OPERATION_CURRENT                                  \
   (AGX_MACOS_UABI_OPERATION_DEVICE_SESSION |                             \
    AGX_MACOS_UABI_OPERATION_API_CONFIGURATION |                          \
    AGX_MACOS_UABI_OPERATION_BO_ALLOCATE |                                \
    AGX_MACOS_UABI_OPERATION_BO_CPU_MAP |                                 \
    AGX_MACOS_UABI_OPERATION_FIXED_VA_BIND |                              \
    AGX_MACOS_UABI_OPERATION_COMMAND_INFRASTRUCTURE |                     \
    AGX_MACOS_UABI_OPERATION_NOTIFICATION_QUEUE |                         \
    AGX_MACOS_UABI_OPERATION_COMPLETION_POLL)

#define AGX_MACOS_UABI_OPERATION_SCREEN_REQUIRED                          \
   (AGX_MACOS_UABI_OPERATION_DEVICE_SESSION |                             \
    AGX_MACOS_UABI_OPERATION_API_CONFIGURATION |                          \
    AGX_MACOS_UABI_OPERATION_BO_ALLOCATE |                                \
    AGX_MACOS_UABI_OPERATION_BO_CPU_MAP |                                 \
    AGX_MACOS_UABI_OPERATION_FIXED_VA_BIND |                              \
    AGX_MACOS_UABI_OPERATION_COMMAND_INFRASTRUCTURE |                     \
    AGX_MACOS_UABI_OPERATION_NOTIFICATION_QUEUE |                         \
    AGX_MACOS_UABI_OPERATION_VM_BIND |                                    \
    AGX_MACOS_UABI_OPERATION_RESOURCE_BIND |                              \
    AGX_MACOS_UABI_OPERATION_CARRIER_BUILD |                              \
    AGX_MACOS_UABI_OPERATION_BATCH_SUBMIT |                               \
    AGX_MACOS_UABI_OPERATION_OBJECT_BIND |                                \
    AGX_MACOS_UABI_OPERATION_LOW_VA_BIND |                                \
    AGX_MACOS_UABI_OPERATION_EXECUTABLE_BO |                              \
    AGX_MACOS_UABI_OPERATION_SHADER_CODE_ADMISSION |                      \
    AGX_MACOS_UABI_OPERATION_COMPLETION_POLL)

struct agx_macos_uabi_contract {
   uint16_t version_major;
   uint16_t version_minor;
   enum agx_macos_device_profile profile;
   uint64_t api_generation;
   uint32_t operations;
   uint32_t unavailable_operations;
   bool initialized;
};

/* Initializes the contract only for a configured, profile-gated direct AGX
 * session. It does not make an unavailable operation callable. */
kern_return_t agx_macos_uabi_contract_init(
   const struct agx_macos_device_session *session,
   struct agx_macos_uabi_contract *out_contract);
bool agx_macos_uabi_contract_is_current(
   const struct agx_macos_uabi_contract *contract,
   const struct agx_macos_device_session *session);
bool agx_macos_uabi_contract_supports(
   const struct agx_macos_uabi_contract *contract, uint32_t operations);
/* Returns not-ready for a stale session and unsupported for a declared but
 * unimplemented operation. This is the central guard for future winsys calls. */
kern_return_t agx_macos_uabi_contract_require(
   const struct agx_macos_uabi_contract *contract,
   const struct agx_macos_device_session *session, uint32_t operations);
