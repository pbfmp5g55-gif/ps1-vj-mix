// Which monitor the output window goes on. Pure logic, no GLFW, so the
// selection rules can be tested against made-up monitor lists in CI
// (the dev machine has one screen and cannot build C++).
// See design/OUTPUT_WINDOW.md.
#pragma once

#include <string>
#include <vector>

namespace vjmix {

struct MonitorInfo {
    std::string name;
    int  x = 0, y = 0;        // virtual-desktop position (can be negative)
    int  w = 0, h = 0;        // current video mode
    bool primary = false;
};

// requested: 0-based index from --output-monitor / the Controls combo, or -1
// for automatic. Automatic = the first non-primary monitor. Returns -1 when
// the output should stay a normal window (no monitors, or only the primary
// and nothing was asked for, or the requested one does not exist).
int chooseOutputMonitor(const std::vector<MonitorInfo>& mons, int requested);

// After a hot-plug the list is re-enumerated and indices may have shifted.
// Find the monitor we were on by name + position; -1 if it is gone.
int findSameMonitor(const std::vector<MonitorInfo>& mons,
                    const MonitorInfo& previous);

}  // namespace vjmix
