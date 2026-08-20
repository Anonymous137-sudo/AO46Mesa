/*
 * SPDX-License-Identifier: MIT
 *
 * Mesa's native AO46 target intentionally owns no GL semantics. It exposes
 * the existing AO46 Metal Gallium screen to Mesa's normal EGL/state-tracker
 * frontend without involving the legacy CGL or NSOpenGL product.
 */

#include "AO46MesaMetalBackend.h"
#include "ao46mtl_screen.h"

#include "pipe/p_screen.h"

struct pipe_screen *
ao46mtl_screen_create(void)
{
   return AO46MesaMetalBackendCreateScreen();
}
