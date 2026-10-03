#include "mixer/output/output_monitor.h"

namespace vjmix {

int chooseOutputMonitor(const std::vector<MonitorInfo>& mons, int requested) {
    const int n = static_cast<int>(mons.size());
    if (n == 0) return -1;
    if (requested >= 0 && requested < n) return requested;
    for (int i = 0; i < n; ++i) {
        if (!mons[static_cast<size_t>(i)].primary) return i;
    }
    return -1;
}

int findSameMonitor(const std::vector<MonitorInfo>& mons,
                    const MonitorInfo& previous) {
    // Name alone is not enough (two identical projectors), position alone
    // is not enough (a different screen plugged into the same port).
    for (size_t i = 0; i < mons.size(); ++i) {
        const MonitorInfo& m = mons[i];
        if (m.name == previous.name && m.x == previous.x && m.y == previous.y) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

}  // namespace vjmix
