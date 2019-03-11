/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#include <set>
#include <type_traits>
#include <vector>

#include "mongo/client/replica_set_change_notifier.h"
#include "mongo/client/replica_set_monitor.h"
#include "mongo/client/replica_set_monitor_internal.h"
#include "mongo/unittest/unittest.h"

namespace mongo {

// NOTE: Unless stated otherwise, all tests assume exclusive access to state belongs to the
// current (only) thread, so they do not lock SetState::mutex before examining state. This is
// NOT something that non-test code should do.

class ReplicaSetMonitorTest : public unittest::Test {
public:
    // Pull in nested types
    using SetState = ReplicaSetMonitor::SetState;
    using Node = SetState::Node;

    using IsMasterReply = ReplicaSetMonitor::IsMasterReply;

    using Refresher = ReplicaSetMonitor::Refresher;
    using NextStep = Refresher::NextStep;

    static constexpr StringData kSetName = "name"_sd;

    ReplicaSetMonitorTest() = default;
    virtual ~ReplicaSetMonitorTest() = default;

    template <typename... Args>
    using StateIsConstructible =
        std::enable_if_t<std::is_constructible_v<SetState,
                                                 Args...,
                                                 ReplicaSetChangeNotifier* const,
                                                 executor::TaskExecutor* const>>;

    template <typename... Args, typename = StateIsConstructible<Args...>>
    auto makeState(Args&&... args) {
        return std::make_shared<ReplicaSetMonitor::SetState>(
            std::forward<Args>(args)..., &_notifier, nullptr);
    }

    void setUp() override {}
    void tearDown() override {}

    static const std::vector<HostAndPort> basicSeeds;
    static const std::set<HostAndPort> basicSeedsSet;
    static const MongoURI basicUri;

protected:
    ReplicaSetChangeNotifier _notifier;
};

}  // namespace mongo
