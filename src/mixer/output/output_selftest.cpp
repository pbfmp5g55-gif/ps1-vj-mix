// CI selftest for the output-monitor selection rules. Exit 0 = all pass.
#include <cstdio>

#include "mixer/output/output_monitor.h"

using vjmix::MonitorInfo;

static int g_fail = 0;
static void check(bool ok, const char* what) {
    std::printf("%s  %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) ++g_fail;
}

static MonitorInfo mon(const char* name, int x, int y, bool primary) {
    MonitorInfo m;
    m.name = name; m.x = x; m.y = y; m.w = 1920; m.h = 1080; m.primary = primary;
    return m;
}

int main() {
    const std::vector<MonitorInfo> none;
    const std::vector<MonitorInfo> one   = {mon("Laptop", 0, 0, true)};
    const std::vector<MonitorInfo> two   = {mon("Laptop", 0, 0, true),
                                            mon("Projector", 1920, 0, false)};
    // Projector listed first and to the left (negative x), laptop primary.
    const std::vector<MonitorInfo> twoRev = {mon("Projector", -1920, 0, false),
                                             mon("Laptop", 0, 0, true)};
    // Venue made the projector the primary.
    const std::vector<MonitorInfo> projPrimary = {mon("Projector", 0, 0, true),
                                                  mon("Laptop", 1920, 0, false)};

    check(vjmix::chooseOutputMonitor(none, -1) == -1, "no monitors -> windowed");
    check(vjmix::chooseOutputMonitor(none, 0) == -1, "no monitors + request -> windowed");
    check(vjmix::chooseOutputMonitor(one, -1) == -1, "one screen, auto -> windowed");
    check(vjmix::chooseOutputMonitor(one, 0) == 0, "one screen, explicit 0 -> 0");
    check(vjmix::chooseOutputMonitor(two, -1) == 1, "two screens, auto -> non-primary");
    check(vjmix::chooseOutputMonitor(twoRev, -1) == 0, "non-primary listed first, negative x");
    check(vjmix::chooseOutputMonitor(two, 0) == 0, "explicit request wins");
    check(vjmix::chooseOutputMonitor(two, 7) == -1, "out-of-range request -> windowed, not a guess");
    check(vjmix::chooseOutputMonitor(projPrimary, -1) == 1,
          "projector is primary -> auto picks laptop (needs Identify / combo)");

    const MonitorInfo proj = mon("Projector", 1920, 0, false);
    check(vjmix::findSameMonitor(two, proj) == 1, "same monitor found");
    check(vjmix::findSameMonitor(twoRev, mon("Projector", -1920, 0, false)) == 0,
          "same monitor found after reorder");
    check(vjmix::findSameMonitor(one, proj) == -1, "unplugged -> -1");
    check(vjmix::findSameMonitor(two, mon("Projector", 0, 1080, false)) == -1,
          "same name, moved -> treated as gone");

    std::printf("%s (%d failed)\n", g_fail ? "FAILED" : "PASSED", g_fail);
    return g_fail ? 1 : 0;
}
