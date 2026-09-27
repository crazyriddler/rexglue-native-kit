/**
 * @file        system/graphics_flags.h
 * @brief       Guest display pacing flags shared by every graphics system
 *              (GPU plugins and application-provided ones).
 */
#pragma once

#include <cstdint>

#include <rex/cvar.h>

REXCVAR_DECLARE(bool, vsync);
REXCVAR_DECLARE(int32_t, unlocked_vblank_rate);
