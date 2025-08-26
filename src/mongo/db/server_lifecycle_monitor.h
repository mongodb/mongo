/**
 *    Copyright (C) 2024-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#pragma once

#include "mongo/util/synchronized_value.h"

#include <functional>

#include <boost/optional.hpp>

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
