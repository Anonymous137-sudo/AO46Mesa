/*
 * Copyright 2026 Khronos_AppleICDs contributors
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include "agx_macos_device.h"

#define AGX_MACOS_COMMAND_PAIR_COUNT 3

/* The observed command-infrastructure replies are opaque 16-byte pairs. */
struct agx_macos_command_pair {
   uint64_t value0;
   uint64_t value1;
};

_Static_assert(sizeof(struct agx_macos_command_pair) == 16,
               "modern AGX command-infrastructure reply size");

struct agx_macos_command_infrastructure {
   struct agx_macos_command_pair pairs[AGX_MACOS_COMMAND_PAIR_COUNT];
   uint64_t api_generation;
   bool initialized;
};

/* The caller must zero-initialize infrastructure. This replays only the
 * initial sequence observed for the first buffer in the single-queue control.
 * Selector scope after this point remains unknown. */
kern_return_t agx_macos_command_infrastructure_init(
   const struct agx_macos_device_session *session,
   struct agx_macos_command_infrastructure *infrastructure);
