/**
 * @file        system/graphics_flags.cpp
 * @brief       Guest display pacing flags. Defined in the runtime (not in a
 *              GPU plugin) so they exist whichever graphics system the app
 *              uses; plugins read them through graphics_flags.h.
 */
#include <rex/system/graphics_flags.h>

REXCVAR_DEFINE_BOOL(vsync, true, "GPU", "Enable vertical sync");

REXCVAR_DEFINE_INT32(unlocked_vblank_rate, 240, "GPU",
                     "Guest vblank rate in Hz when vsync is disabled. This is the framerate "
                     "ceiling for titles that pace themselves on vblank; every tick runs the "
                     "guest's vblank interrupt handler, so raising it past what the GPU can "
                     "actually deliver only costs CPU time.")
    .range(30, 1000)
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
