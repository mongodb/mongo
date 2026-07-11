// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/util/modules.h"
#include "mongo/util/synchronized_value.h"

#include <functional>

#include <boost/optional.hpp>

[[MONGO_MOD_PUBLIC]];

namespace mongo {

/** Observes server process lifecycle milestones. */
class ServerLifecycleMonitor {
public:
    /**
     * Add `cb` as a function to be invoked when startup is finishing.
     * Cannot be called after the call to `onFinishingStartup`.
     */
    void addFinishingStartupCallback(std::function<void()> cb);

    /**
     * Invoked as the final stage of server startup (e.g. by main).
     * Cannot be called more than once.
     */
    void onFinishingStartup();

private:
    using TaskVec = std::vector<std::function<void()>>;

    /**
     * Initialized to an empty vector, but reset by `onFinishingStartup`. This
     * behavior detects call order violations.
     */
    synchronized_value<boost::optional<TaskVec>> _finishingStartupCallbacks{TaskVec{}};
};

ServerLifecycleMonitor& globalServerLifecycleMonitor();

}  // namespace mongo
