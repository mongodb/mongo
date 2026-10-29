// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/server_lifecycle_monitor.h"

#include "mongo/unittest/unittest.h"

#include <vector>

namespace mongo {
namespace {

TEST(ServerLifecycleMonitorTest, FinalizeStartup) {
    ServerLifecycleMonitor monitor;
    bool ran{};
    monitor.addFinishingStartupCallback([&] { ran = true; });
    ASSERT_FALSE(ran);
    monitor.onFinishingStartup();
    ASSERT(ran);
}

TEST(ServerLifecycleMonitorTest, FinalizeStartupMultipleClients) {
    ServerLifecycleMonitor monitor;

    const size_t nClients = 10;
    std::vector<size_t> ran;
    for (size_t i = 0; i < nClients; ++i)
        monitor.addFinishingStartupCallback([&, i] { ran.push_back(i); });

    ASSERT(ran.empty());
    monitor.onFinishingStartup();

    std::vector<size_t> exp;
    for (size_t i = 0; i < nClients; ++i)
        exp.push_back(i);
    ASSERT_EQ(ran, exp) << "All callbacks should run, in order of registration.";
}

}  // namespace
}  // namespace mongo
