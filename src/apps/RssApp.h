#pragma once

// Included by path so this header resolves without the build system's library
// include paths (the same reason the host test fakes do).
#include "../../lib/MiniApps/src/MiniApps.h"

namespace papyrix {
struct Core;
struct Event;
}  // namespace papyrix

namespace papyrix::rss_app {

void enter(Core& core);
bool update(Core& core);
miniapps::Action handleEvent(Core& core, const Event& event, bool menuOpen);
miniapps::Frame render(Core& core, bool menuOpen);
void exitApp(Core& core);
miniapps::ExitResult prepareExit(Core& core, miniapps::ExitTarget target);

}  // namespace papyrix::rss_app
