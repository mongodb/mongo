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


#include "mongo/db/repl/hello/topology_version_observer.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/client.h"
#include "mongo/db/repl/hello/hello_response.h"
#include "mongo/db/repl/member_state.h"
#include "mongo/db/repl/optime.h"
#include "mongo/db/repl/repl_set_config.h"
#include "mongo/db/repl/replication_coordinator_impl.h"
#include "mongo/db/repl/replication_coordinator_test_fixture.h"
#include "mongo/executor/network_interface_mock.h"
#include "mongo/unittest/barrier.h"
#include "mongo/unittest/log_test.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/future.h"
#include "mongo/util/net/hostandport.h"
#include "mongo/util/time_support.h"

#include <iostream>
#include <memory>

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest


namespace mongo {
namespace repl {
namespace {

/**
 * Sets up and tears down the test environment for `TopologyVersionObserver`
 */
class TopologyVersionObserverTest : public ReplCoordTest {
protected:
    BSONObj getConfigObj() {
        BSONObjBuilder configBuilder;
        configBuilder << "_id"
                      << "mySet";
        configBuilder << "version" << 1;
        configBuilder << "members"
                      << BSON_ARRAY(BSON("_id" << 1 << "host"
                                               << "node1:12345")
                                    << BSON("_id" << 2 << "host"
                                                  << "node2:12345"));
        configBuilder << "protocolVersion" << 1;
        return configBuilder.obj();
    }

public:
    void setUp() override {
        auto configObj = getConfigObj();
        assertStartSuccess(configObj, HostAndPort("node1", 12345));
        ReplSetConfig config = assertMakeRSConfig(configObj);
        replCoord = getReplCoord();

        ASSERT_OK(replCoord->setFollowerMode(MemberState::RS_SECONDARY));
        replCoordSetMyLastWrittenAndAppliedAndDurableOpTime(OpTime(Timestamp(100, 1), 1),
                                                            Date_t() + Seconds(100));
        simulateSuccessfulV1Election();
        ASSERT(replCoord->getMemberState().primary());

        getNet()->enterNetwork();
        getNet()->advanceTime(Date_t::now() + sleepTime);
        getNet()->exitNetwork();

        auto serviceContext = getServiceContext();
        observer = std::make_unique<TopologyVersionObserver>();
        observer->init(serviceContext, replCoord);
    }

    void tearDown() override {
        observer->shutdown();
        ASSERT(observer->isShutdown());
        observer.reset();
    }

    auto getObserverCache() {
        // Wait for observer to initialize its cache. Due to the unpredictable nature of thread
        // scheduling, do not change the following to a fixed-wait.
        auto cache = observer->getCached();
        while (!cache) {
            sleepFor(sleepTime);
            cache = observer->getCached();
        }

        return cache;
    }

    void forceElectionAndWaitForCacheUpdate(std::shared_ptr<const HelloResponse> cachedResponse) {
        // Force an election to advance topology version
        auto opCtx = makeOperationContext();
        auto electionTimeoutWhen = getReplCoord()->getElectionTimeout_forTest();
        simulateSuccessfulV1ElectionWithoutExitingDrainMode(electionTimeoutWhen, opCtx.get());

        int sleepCounter = 0;
        // Wait for the observer to update its cache
        while (observer->getCached()->getTopologyVersion()->getCounter() ==
               cachedResponse->getTopologyVersion()->getCounter()) {
            sleepFor(sleepTime);
            // Make sure the test doesn't wait here for longer than 15 seconds.
            ASSERT_LTE(sleepCounter++, 150);
        }
        LOGV2(9326401, "Observer topology incremented after successful election");
    }

protected:
    ReplicationCoordinatorImpl* replCoord;

    const Milliseconds sleepTime = Milliseconds(100);

    std::unique_ptr<TopologyVersionObserver> observer;

    unittest::MinimumLoggedSeverityGuard severityGuard{logv2::LogComponent::kDefault,
                                                       logv2::LogSeverity::Debug(4)};
};


TEST_F(TopologyVersionObserverTest, PopulateCache) {
    auto cachedResponse = getObserverCache();
    ASSERT(cachedResponse);

    auto opCtx = makeOperationContext();
    auto expectedResponse =
        replCoord->awaitHelloResponse(opCtx.get(), {}, boost::none, boost::none);
    ASSERT_EQ(cachedResponse->toBSON().toString(), expectedResponse->toBSON().toString());
}

TEST_F(TopologyVersionObserverTest, UpdateCache) {
    auto cachedResponse = getObserverCache();
    ASSERT(cachedResponse);

    forceElectionAndWaitForCacheUpdate(cachedResponse);

    auto newResponse = observer->getCached();
    ASSERT(newResponse && newResponse->getTopologyVersion());
    ASSERT(newResponse->getTopologyVersion()->getCounter() >
           cachedResponse->getTopologyVersion()->getCounter());

    auto opCtx = makeOperationContext();
    auto expectedResponse =
        replCoord->awaitHelloResponse(opCtx.get(), {}, boost::none, boost::none);
    ASSERT(expectedResponse && expectedResponse->getTopologyVersion());

    ASSERT_EQ(newResponse->getTopologyVersion()->getCounter(),
              expectedResponse->getTopologyVersion()->getCounter());
}

TEST_F(TopologyVersionObserverTest, CallbackExecutedOnTopologyChange) {
    // Wait for the observer to see the initial replSetConfig
    auto cachedResponse = getObserverCache();
    ASSERT(cachedResponse);

    // Set up a notification to learn when the callback has been called
    Notification<ReplSetConfig> receivedConfig;
    observer->registerTopologyChangeObserver(
        [&](const ReplSetConfig& config) { receivedConfig.set(config); });

    ASSERT(!receivedConfig);

    // Force an election
    forceElectionAndWaitForCacheUpdate(cachedResponse);

    // Verify callback was executed successfully after the topology change.
    ASSERT_EQ(receivedConfig.get().getConfigVersion(), replCoord->getConfig().getConfigVersion());
}

TEST_F(TopologyVersionObserverTest, HandleDBException) {
    auto cachedResponse = getObserverCache();
    ASSERT(cachedResponse);

    Client* observerClient = nullptr;
    {
        auto cur = ServiceContext::LockedClientsCursor(getGlobalServiceContext());
        while (auto client = cur.next()) {
            if (client->desc() == kTopologyVersionObserverName) {
                observerClient = client;
                break;
            }
        }
    }
    // The client should not go out-of-scope as it is attached to the observer thread.
    ASSERT(observerClient);

    auto tryKillOperation = [&] {
        ClientLock clientLock(observerClient);

        if (auto opCtx = observerClient->getOperationContext()) {
            observerClient->getServiceContext()->killOperation(clientLock, opCtx);
            return true;
        }

        return false;
    };

    {
        // Set the failpoint here so that if there is no opCtx we catch the next one.
        FailPointEnableBlock failBlock("topologyVersionObserverExpectsInterruption");

        // Kill the operation waiting on the `isMaster` future to make it throw
        if (!tryKillOperation()) {
            // If we weren't able to kill, then block until there is an opCtx again.
            failBlock->waitForTimesEntered(failBlock.initialTimesEntered() + 1);

            // Try again to kill now that we've waited for the failpoint.
            ASSERT(tryKillOperation()) << "Unable to acquire and kill observer OpCtx";
        }
    }

    // Observer thread must handle the exception and fetch the most recent IMR
    auto newResponse = getObserverCache();
    ASSERT(newResponse->getTopologyVersion()->getCounter() ==
           cachedResponse->getTopologyVersion()->getCounter());
}

TEST_F(TopologyVersionObserverTest, HandleQuiesceMode) {
    // Start out as a secondary to transition to quiesce mode easily.
    ASSERT_OK(getReplCoord()->setFollowerMode(MemberState::RS_SECONDARY));

    auto cachedResponse = getObserverCache();
    ASSERT(cachedResponse);

    // Set a failpoint so we can observe the background thread shutting down.
    FailPointEnableBlock failBlock("topologyVersionObserverExpectsShutdown");

    {
        // Enter quiesce mode in the replication coordinator to make shutdown errors come from
        // awaitHelloResponseFuture()/getHelloResponseFuture().
        auto opCtx = makeOperationContext();
        getReplCoord()->enterQuiesceModeIfSecondary(Milliseconds(0));

        getNet()->enterNetwork();
        getNet()->advanceTime(getNet()->now() + sleepTime);
        getNet()->exitNetwork();

        ASSERT_THROWS_CODE(replCoord->getHelloResponseFuture({}, boost::none).get(opCtx.get()),
                           AssertionException,
                           ErrorCodes::ShutdownInProgress);
    }

    // Wait for the background thread to fully shutdown.
    failBlock->waitForTimesEntered(failBlock.initialTimesEntered() + 1);

    // In quiescence, the observer should be shutdown and have nothing in cache.
    ASSERT(!observer->getCached());
    ASSERT(observer->isShutdown());
}

class TopologyVersionObserverInterruptedTest : public TopologyVersionObserverTest {
public:
    void setUp() override {
        auto configObj = getConfigObj();
        assertStartSuccess(configObj, HostAndPort("node1", 12345));
    }

    void tearDown() override {}
};

TEST_F(TopologyVersionObserverInterruptedTest, ShutdownAlwaysInterruptsWorkerOperation) {

    std::unique_ptr<TopologyVersionObserver> observer;
    unittest::Barrier b1(2), b2(2);
    boost::optional<stdx::thread> observerThread;
    boost::optional<stdx::thread> blockerThread;
    {
        FailPointEnableBlock workerFailBlock("topologyVersionObserverBeforeCheckingForShutdown");

        observer = std::make_unique<TopologyVersionObserver>();
        observer->init(getServiceContext(), getReplCoord());

        workerFailBlock->waitForTimesEntered(workerFailBlock.initialTimesEntered() + 1);
        blockerThread = stdx::thread([&] {
            FailPointEnableBlock requestFailBlock("topologyVersionObserverExpectsInterruption");
            b1.countDownAndWait();
            // Keeps the failpoint enabled until it receives a signal from themain thread.
            b2.countDownAndWait();
        });
        b1.countDownAndWait();  // Wait for blocker thread to enable thefailpoint
        {
            FailPointEnableBlock shutdownFailBlock("topologyVersionObserverShutdownShouldWait");
            observerThread = stdx::thread([&] { observer->shutdown(); });

            shutdownFailBlock->waitForTimesEntered(shutdownFailBlock.initialTimesEntered() + 1);
        }
    }
    observerThread->join();
    b2.countDownAndWait();  // Unblock the blocker thread so that it can join
    blockerThread->join();

    ASSERT(observer->isShutdown());
}

}  // namespace
}  // namespace repl
}  // namespace mongo
