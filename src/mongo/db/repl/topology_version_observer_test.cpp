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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest

#include "mongo/platform/basic.h"

#include <functional>
#include <iostream>
#include <memory>

#include "mongo/bson/timestamp.h"
#include "mongo/bson/util/bson_extract.h"
#include "mongo/db/repl/is_master_response.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/repl/replication_coordinator_impl.h"
#include "mongo/db/repl/replication_coordinator_test_fixture.h"
#include "mongo/db/repl/topology_version_observer.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/clock_source.h"
#include "mongo/util/time_support.h"

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
    TopologyVersionObserverTest() {
        auto configObj = getConfigObj();
        assertStartSuccess(configObj, HostAndPort("node1", 12345));
        ReplSetConfig config = assertMakeRSConfig(configObj);
        replCoord = getReplCoord();

        ASSERT_OK(replCoord->setFollowerMode(MemberState::RS_SECONDARY));
        replCoordSetMyLastAppliedOpTime(OpTime(Timestamp(100, 1), 1), Date_t() + Seconds(100));
        replCoordSetMyLastDurableOpTime(OpTime(Timestamp(100, 1), 1), Date_t() + Seconds(100));
        simulateSuccessfulV1Election();
        ASSERT(replCoord->getMemberState().primary());

        getNet()->enterNetwork();
        getNet()->advanceTime(Date_t::now() + sleepTime);
        getNet()->exitNetwork();

        auto serviceContext = getServiceContext();
        observer = std::make_unique<TopologyVersionObserver>();
        observer->init(serviceContext, replCoord);
    }

    ~TopologyVersionObserverTest() {
        observer->shutdown();
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

protected:
    ReplicationCoordinatorImpl* replCoord;

    const Milliseconds sleepTime = Milliseconds(100);

    std::unique_ptr<TopologyVersionObserver> observer;
};


TEST_F(TopologyVersionObserverTest, PopulateCache) {
    auto cachedResponse = getObserverCache();
    ASSERT(cachedResponse);

    auto opCtx = makeOperationContext();
    auto expectedResponse =
        replCoord->awaitIsMasterResponse(opCtx.get(), {}, boost::none, boost::none);
    ASSERT_EQ(cachedResponse->toBSON().toString(), expectedResponse->toBSON().toString());
}

TEST_F(TopologyVersionObserverTest, UpdateCache) {
    auto cachedResponse = getObserverCache();
    ASSERT(cachedResponse);

    // Force an election to advance topology version
    auto opCtx = makeOperationContext();
    auto electionTimeoutWhen = getReplCoord()->getElectionTimeout_forTest();
    simulateSuccessfulV1ElectionWithoutExitingDrainMode(electionTimeoutWhen, opCtx.get());

    // Wait for the observer to update its cache
    while (observer->getCached()->getTopologyVersion()->getCounter() ==
           cachedResponse->getTopologyVersion()->getCounter()) {
        sleepFor(sleepTime);
    }

    auto newResponse = observer->getCached();
    ASSERT(newResponse && newResponse->getTopologyVersion());
    ASSERT(newResponse->getTopologyVersion()->getCounter() >
           cachedResponse->getTopologyVersion()->getCounter());

    auto expectedResponse =
        replCoord->awaitIsMasterResponse(opCtx.get(), {}, boost::none, boost::none);
    ASSERT(expectedResponse && expectedResponse->getTopologyVersion());

    ASSERT_EQ(newResponse->getTopologyVersion()->getCounter(),
              expectedResponse->getTopologyVersion()->getCounter());
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

    ClockSource::StopWatch timer;
    constexpr auto maxWait = Seconds(10);

    // Kill the operation waiting on the `isMaster` future to make it throw
    bool wasAbleToKillOpCtx = false;
    while (!wasAbleToKillOpCtx) {
        if (timer.elapsed() > maxWait) {
            FAIL(str::stream() << "Timed out while waiting for the observer to create OpCtx.");
        }

        stdx::lock_guard clientLock(*observerClient);
        if (auto opCtx = observerClient->getOperationContext()) {
            observerClient->getServiceContext()->killOperation(clientLock, opCtx);
            wasAbleToKillOpCtx = true;
            continue;
        }

        sleepFor(sleepTime);
    }

    // Observer thread must handle the exception and fetch the most recent IMR
    auto newResponse = getObserverCache();
    ASSERT(newResponse->getTopologyVersion()->getCounter() ==
           cachedResponse->getTopologyVersion()->getCounter());
}

}  // namespace
}  // namespace repl
}  // namespace mongo
