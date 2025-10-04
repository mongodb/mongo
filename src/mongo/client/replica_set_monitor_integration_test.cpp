/**
 *    Copyright (C) 2020-present MongoDB, Inc.
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
// IWYU pragma: no_include "cxxabi.h"
#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/client/connection_string.h"
#include "mongo/client/mongo_uri.h"
#include "mongo/client/read_preference.h"
#include "mongo/client/replica_set_change_notifier.h"
#include "mongo/client/replica_set_monitor.h"
#include "mongo/client/replica_set_monitor_manager.h"
#include "mongo/db/service_context.h"
#include "mongo/db/wire_version.h"
#include "mongo/executor/network_interface.h"
#include "mongo/executor/network_interface_factory.h"
#include "mongo/executor/network_interface_thread_pool.h"
#include "mongo/executor/remote_command_request.h"
#include "mongo/executor/remote_command_response.h"
#include "mongo/executor/task_executor.h"
#include "mongo/executor/thread_pool_task_executor.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/stdx/condition_variable.h"
#include "mongo/stdx/mutex.h"
#include "mongo/unittest/integration_test.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/cancellation.h"
#include "mongo/util/duration.h"
#include "mongo/util/future.h"
#include "mongo/util/hierarchical_acquisition.h"

#include <algorithm>
#include <cstddef>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include <boost/move/utility_core.hpp>
#include <boost/smart_ptr.hpp>

namespace mongo {
namespace executor {
namespace {

/**
 * A mock class mimicking TaskExecutor::CallbackState, does nothing.
 */
class MockCallbackState final : public TaskExecutor::CallbackState {
public:
    MockCallbackState() = default;
    void cancel() override {}
    void waitForCompletion() override {}
    bool isCanceled() const override {
        return false;
    }
};

inline TaskExecutor::CallbackHandle makeCallbackHandle() {
    return TaskExecutor::CallbackHandle(std::make_shared<MockCallbackState>());
}


class ReplicaSetMonitorFixture : public mongo::unittest::Test {
public:
    constexpr static Milliseconds kTimeout{5000};

    void resetIsInternalClient(bool isInternalClient) {
        WireSpec::Specification newSpec = *WireSpec::getWireSpec(getGlobalServiceContext()).get();
        newSpec.isInternalClient = isInternalClient;
        WireSpec::getWireSpec(getGlobalServiceContext()).reset(std::move(newSpec));
    }

    void setUp() override {
        resetIsInternalClient(true);

        net = makeNetworkInterface("ReplicaSetMonintorTest");

        auto tp = std::make_unique<NetworkInterfaceThreadPool>(net.get());
        executor = ThreadPoolTaskExecutor::create(std::move(tp), net);
        executor->startup();

        connectionManager = std::make_unique<ReplicaSetMonitorConnectionManager>(net);

        replSetUri = uassertStatusOK(MongoURI::parse(getReplSetConnectionString()));
        numNodes = replSetUri.getServers().size();
    };

    void tearDown() override {
        shutdownExecutor();
        resetIsInternalClient(false);
    };

    void shutdownExecutor() {
        if (executor) {
            executor->shutdown();
            executor.reset();
        }
    }

    /**
     * Runs the given remote command request, and returns the response.
     */
    RemoteCommandResponse runCommand(RemoteCommandRequest request) {
        return net->startCommand(makeCallbackHandle(), request).get();
    }


    ConnectionString getFixtureConnectionString() {
        return unittest::getFixtureConnectionString();
    }

    /**
     * If this test is running on a mongos, returns the connection string for one of its replica
     * set shards. Otherwise, assumes that the test is running on a replica set, and returns the
     * fixture connection string.
     */
    std::string getReplSetConnectionString() {
        // Run the listShards command to get shards. Expect it to fail with CommandNotFound if
        // this test is not running on mongos.
        const auto cmdObj = BSON("listShards" << 1);
        RemoteCommandRequest request{getFixtureConnectionString().getServers().front(),
                                     DatabaseName::kAdmin,
                                     cmdObj,
                                     BSONObj(),
                                     nullptr,
                                     kTimeout};
        auto res = runCommand(request);
        ASSERT_OK(res.status);
        auto cmdStatus = getStatusFromCommandResult(res.data);

        if (cmdStatus == ErrorCodes::CommandNotFound) {
            // This test is not running on a mongos.
            return getFixtureConnectionString().toString();
        }

        ASSERT_OK(cmdStatus);
        const auto shards = res.data["shards"].Array();
        ASSERT_FALSE(shards.empty());
        return std::string{shards.front().embeddedObject().getStringField("host")};
    }

protected:
    std::shared_ptr<NetworkInterface> net;
    std::shared_ptr<ThreadPoolTaskExecutor> executor;
    std::shared_ptr<ReplicaSetMonitorConnectionManager> connectionManager;
    ReplicaSetChangeNotifier notifier;

    MongoURI replSetUri;
    size_t numNodes;
};

TEST_F(ReplicaSetMonitorFixture, StreamableRSMWireVersion) {
    auto rsm = ReplicaSetMonitorManager::get()->getOrCreateMonitor(replSetUri, nullptr);

    auto primaryFuture =
        rsm->getHostOrRefresh(ReadPreferenceSetting(mongo::ReadPreference::PrimaryOnly),
                              CancellationToken::uncancelable());
    primaryFuture.get();

    ASSERT_EQ(rsm->getMinWireVersion(), WireVersion::LATEST_WIRE_VERSION);
    ASSERT_EQ(rsm->getMaxWireVersion(), WireVersion::LATEST_WIRE_VERSION);
}

TEST_F(ReplicaSetMonitorFixture, ReplicaSetMonitorCleanup) {
    const auto& setName = replSetUri.getSetName();
    ReplicaSetMonitor::cleanup();
    auto sets = ReplicaSetMonitorManager::get()->getAllSetNames();
    ASSERT_TRUE(std::find(sets.begin(), sets.end(), setName) == sets.end());

    stdx::mutex mutex;
    stdx::condition_variable cv;
    bool cleanupInvoked = false;
    auto rsm = ReplicaSetMonitorManager::get()->getOrCreateMonitor(replSetUri,
                                                                   [&cleanupInvoked, &mutex, &cv] {
                                                                       stdx::unique_lock lk(mutex);
                                                                       cleanupInvoked = true;
                                                                       cv.notify_one();
                                                                   });

    sets = ReplicaSetMonitorManager::get()->getAllSetNames();
    ASSERT_TRUE(std::find(sets.begin(), sets.end(), setName) != sets.end());

    shutdownExecutor();
    rsm.reset();

    stdx::unique_lock lk(mutex);
    cv.wait(lk, [&cleanupInvoked] { return cleanupInvoked; });
    ASSERT_TRUE(cleanupInvoked);
}

// Tests that RSM Manager registerForGarbageCollection() could be invoked while holding a
// lvl 2 mutex. Tests that GC is invoked before creating a new monitor.
TEST_F(ReplicaSetMonitorFixture, LockOrderingAndGC) {
    ReplicaSetMonitor::cleanup();
    ASSERT_EQ(0, ReplicaSetMonitorManager::get()->getAllSetNames().size());
    auto monitor = ReplicaSetMonitor::createIfNeeded(replSetUri);
    ASSERT_EQ(1, ReplicaSetMonitorManager::get()->getAllSetNames().size());
    const auto previousGCCount =
        ReplicaSetMonitorManager::get()->getGarbageCollectedMonitorsCount();

    {
        stdx::mutex lvl2mutex;
        stdx::unique_lock lk(lvl2mutex);
        // This invokes delayed GC that locks only lvl 1 mutex.
        monitor.reset();
        // Tests that GC was not run yet.
        ASSERT_EQ(previousGCCount,
                  ReplicaSetMonitorManager::get()->getGarbageCollectedMonitorsCount());
    }

    // Tests that GC was not invoked yet and the empty pointer is still in the cache.
    ASSERT_EQ(1, ReplicaSetMonitorManager::get()->getNumMonitors());
    // getAllSetNames() checks that each pointer is valid, thus it returns 0.
    ASSERT_EQ(0, ReplicaSetMonitorManager::get()->getAllSetNames().size());

    ReplicaSetMonitor::createIfNeeded(replSetUri);
    // Tests that GC was run.
    ASSERT_EQ(previousGCCount + 1,
              ReplicaSetMonitorManager::get()->getGarbageCollectedMonitorsCount());
    ASSERT_EQ(0, ReplicaSetMonitorManager::get()->getAllSetNames().size());
}

// Tests 1) that boost::none gets returned if you try to get pingTime for some random host and port
// that isn't part of the RSM and 2) that the pingTime can be collected for an RSM.
TEST_F(ReplicaSetMonitorFixture, PingTime) {
    ReplicaSetMonitor::cleanup();
    auto rsm = ReplicaSetMonitor::createIfNeeded(replSetUri);

    rsm->getHostOrRefresh(ReadPreferenceSetting(ReadPreference::PrimaryOnly),
                          CancellationToken::uncancelable())
        .get();

    bool foundPing = false;
    for (const auto& server : replSetUri.getServers()) {
        if (rsm->pingTime(server) != boost::none) {
            foundPing = true;
        }
    }
    ASSERT_TRUE(foundPing);
    ASSERT_EQ(rsm->pingTime(HostAndPort{"not-member-host", 1234}), boost::none);
}

}  // namespace
}  // namespace executor
}  // namespace mongo
