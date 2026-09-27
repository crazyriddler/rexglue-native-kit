#pragma once

namespace conan::native {

// Called once per guest swap; starts the watchdog thread on first use.
void HangWatchdogBeat();

}  // namespace conan::native
