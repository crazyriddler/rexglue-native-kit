// Startup launcher: the settings window shown before the game starts
// (display mode, window size, VSync, frame rate limit, internal resolution,
// shadow quality, anisotropic filtering, MSAA, language). Reads and writes
// conan.cfg through conan::Settings.
//
// Implemented as a plain native Win32 window (not an SDK ImGuiDialog) -
// ImGui popups crashed (STATUS_STACK_BUFFER_OVERRUN) when opened this early
// in ReXApp::OnInitialize, before the render pipeline has completed its
// first real frame; see docs/error_log.md for the isolation tests. A native
// window has no dependency on ImGui/D3D12/window_ timing at all, so it's
// safe to show synchronously from ConanApp::OnPreSetup (called after config
// is loaded, before window/GPU are created).

#pragma once

#include "settings.h"

// Blocking: runs its own Win32 message loop. Returns true when the user
// pressed Play (`settings` then holds the choices), false when the launcher
// was closed / Exit was pressed (the game should quit).
bool ShowConanLauncher(conan::Settings& settings);
