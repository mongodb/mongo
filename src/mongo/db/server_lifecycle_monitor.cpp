// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/server_lifecycle_monitor.h"

#include "mongo/util/static_immortal.h"

namespace mongo {

void ServerLifecycleMonitor::onFinishingStartup() {
    // Synchronously consume and reset the optional.
    auto tasks = std::exchange(**_finishingStartupCallbacks, {});
    for (auto& cb : tasks.value())
        std::exchange(cb, {})();  // Execute and destroy
}

void ServerLifecycleMonitor::addFinishingStartupCallback(std::function<void()> cb) {
    (**_finishingStartupCallbacks).value().push_back(std::move(cb));
}

ServerLifecycleMonitor& globalServerLifecycleMonitor() {
    static StaticImmortal<ServerLifecycleMonitor> obj;
    return *obj;
}

}  // namespace mongo
