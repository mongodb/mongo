// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/topology/cluster_parameters/cluster_server_parameter_refresher.h"

#include "mongo/base/error_codes.h"
#include "mongo/db/client.h"
#include "mongo/db/service_context.h"
#include "mongo/db/service_context_test_fixture.h"
#include "mongo/db/sharding_environment/sharding_mongos_test_fixture.h"
#include "mongo/stdx/thread.h"
#include "mongo/stdx/unordered_map.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/fail_point.h"

#include <string_view>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest

namespace mongo {
namespace {

class ClusterServerParameterRefresherTest : public virtual service_context_test::RouterRoleOverride,
                                            public ShardingTestFixture {
protected:
    void setUp() override {
        ShardingTestFixture::setUp();
        _refresher = std::make_unique<ClusterServerParameterRefresher>();

        // RouterServer role is required to run the refresher.
        auto targetService = operationContext()->getServiceContext()->getService();
        operationContext()->getClient()->setService(targetService);

        const HostAndPort kTestConfigShardHost = HostAndPort("FakeConfigHost", 12345);
        configTargeter()->setFindHostReturnValue(kTestConfigShardHost);

        // (Generic FCV reference): Used to provide a valid FCV version response to the
        // ClusterServerParameterRefresher. This FCV reference should exist across binary versions.
        _fcv = multiversion::GenericFCV::kLatest;
        _clusterParameterDocs.insert({"maxIncomingConnections",
                                      BSON("_id" << "maxIncomingConnections"
                                                 << "value" << 3000 << "type"
                                                 << "int")});
    }

    void mockFCVAndClusterParametersResponses() {
        auto atClusterTime = Timestamp(10, 3);

        // Respond to FCV find.
        onCommand([&](const executor::RemoteCommandRequest& request) {
            return BSON(
                "ok" << 1 << "cursor"
                     << BSON("firstBatch"
                             << BSON_ARRAY(BSON("_id" << "featureCompatibilityVersion"
                                                      << "version" << multiversion::toString(_fcv)))
                             << "atClusterTime" << atClusterTime << "id" << 0LL << "ns"
                             << NamespaceString::kServerConfigurationNamespace.toString_forTest()));
        });

        // Respond to find for clusterParameters.
        onCommand([&](const executor::RemoteCommandRequest& request) {
            BSONArrayBuilder clusterParameterDocs;
            for (const auto& [_, paramDoc] : _clusterParameterDocs) {
                clusterParameterDocs.append(paramDoc);
            }

            // atClusterTime in readConcern should be the same as the time returned by the FCV find.
            ASSERT_EQ(request.cmdObj["readConcern"].Obj()["atClusterTime"].timestamp(),
                      atClusterTime);

            return BSON(
                "ok" << 1 << "cursor"
                     << BSON("firstBatch"
                             << clusterParameterDocs.obj() << "id" << 0LL << "ns"
                             << NamespaceString::kClusterParametersNamespace.toString_forTest()));
        });
    }

    void assertFCVAndClusterParamsMatchExpected(multiversion::FeatureCompatibilityVersion fcv,
                                                StringMap<BSONObj> clusterParameterDocs) {

        ASSERT_EQ(fcv, _fcv);

        for (const auto& [clusterParameterName, _] : _clusterParameterDocs) {
            ASSERT_NE(clusterParameterDocs.find(clusterParameterName), clusterParameterDocs.end());
        }
    }

    SharedPromise<void>* refreshPromise() {
        return _refresher->getRefreshPromise_forTest();
    }

    ServiceContext::UniqueOperationContext _opCtx;
    std::unique_ptr<ClusterServerParameterRefresher> _refresher;

    multiversion::FeatureCompatibilityVersion _fcv;
    stdx::unordered_map<std::string, BSONObj> _clusterParameterDocs;
};

TEST_F(ClusterServerParameterRefresherTest, RefresherConcurrency) {
    auto runRefreshTestIteration = [&](std::string_view blockingFPName, Status expectedStatus) {
        ASSERT_EQ(refreshPromise(), nullptr);
        // Set up failpoints and get their initial times entered.
        auto blockFp = globalFailPointRegistry().find(blockingFPName);
        auto waiterCountFp =
            globalFailPointRegistry().find("countPromiseWaitersClusterParameterRefresh");
        auto initBlockFpTE = blockFp->setMode(FailPoint::alwaysOn);
        auto initWaiterFpTE = waiterCountFp->setMode(FailPoint::alwaysOn);

        // Since there should be no active job at this point, we expect thread 1 to create the
        // promise, run _refreshParameters, and block on the failpoint.
        stdx::thread firstRun([&]() {
            Status status = _refresher->refreshParameters(operationContext());
            ASSERT_EQ(status, expectedStatus);
        });

        // Wait for thread 1 to reach the blocking failpoint. Note that each time we enter the
        // blocking failpoint, we increment "timesEntered" by 2, because we first check for
        // shouldFail and then call pauseWhileSet.
        blockFp->waitForTimesEntered(initBlockFpTE + 2);
        ASSERT(refreshPromise() && !refreshPromise()->getFuture().isReady());

        // Toggle the countPromiseWaiters failpoint to get the times entered. This count should not
        // have changed from the initial count as we have no futures waiting on the promise yet.
        waiterCountFp->setMode(FailPoint::off);
        auto waiterCountAfterBlock = waiterCountFp->setMode(FailPoint::alwaysOn);
        ASSERT_EQ(waiterCountAfterBlock, initWaiterFpTE);

        // Threads 2 and 3 should both see that there is an active promise and take out a future on
        // it, not entering _refreshParameters themselves.
        stdx::thread secondRun([&]() {
            Status status = _refresher->refreshParameters(operationContext());
            ASSERT_EQ(status, expectedStatus);
        });
        stdx::thread thirdRun([&]() {
            Status status = _refresher->refreshParameters(operationContext());
            ASSERT_EQ(status, expectedStatus);
        });

        // Allow both new threads to hit the future wait before unblocking the first thread
        waiterCountFp->waitForTimesEntered(initWaiterFpTE + 2);
        waiterCountFp->setMode(FailPoint::off);

        // We expect that neither of threads 2 and 3 entered _refreshParameters, so neither should
        // have hit the blocking failpoint; assert its count is the same as before.
        auto afterSleepTE = blockFp->setMode(FailPoint::off);
        ASSERT_EQ(afterSleepTE, initBlockFpTE + 2);

        // The first thread should now finish, setting the job to ready and notifying threads 2 and
        // 3, which should finish.
        firstRun.join();
        ASSERT_EQ(refreshPromise(), nullptr);

        secondRun.join();
        thirdRun.join();
    };

    const int major_iters = 3;
    const int minor_iters = 2;
    for (int i = 0; i < major_iters; i++) {
        // Interlace testing of the OK and failure cases to ensure that we are never getting a stale
        // status.
        for (int j = 0; j < minor_iters; j++) {
            runRefreshTestIteration("blockAndSucceedClusterParameterRefresh", Status::OK());
        }
        for (int j = 0; j < minor_iters; j++) {
            // Note that status comparison only cares about error code.
            runRefreshTestIteration("blockAndFailClusterParameterRefresh",
                                    Status(ErrorCodes::FailPointEnabled, "..."));
        }
    }
}

// Verifies that an invocation of 'refreshParameters()' with 'ensureReadYourWritesConsistency'
// parameter set to 'true' while another cluster-wide parameters refresh request is in progress
// waits for completion of that request and then initiates a new request.
TEST_F(ClusterServerParameterRefresherTest, RefresherConcurrencyReadYourWritesConsistency) {
    ASSERT_EQ(refreshPromise(), nullptr);

    // Lookup fail-points.
    auto blockAndSucceedClusterParameterRefreshFailPoint =
        globalFailPointRegistry().find("blockAndSucceedClusterParameterRefresh");
    auto blockAndFailClusterParameterRefreshFailPoint =
        globalFailPointRegistry().find("blockAndFailClusterParameterRefresh");
    auto countPromiseWaitersClusterParameterRefreshFailPoint =
        globalFailPointRegistry().find("countPromiseWaitersClusterParameterRefresh");

    // Program the fail-points.
    const auto blockAndSucceedClusterParameterRefreshFailPointEntryCount =
        blockAndSucceedClusterParameterRefreshFailPoint->setMode(FailPoint::alwaysOn);
    const auto countPromiseWaitersClusterParameterRefreshFailPointEntryCount =
        countPromiseWaitersClusterParameterRefreshFailPoint->setMode(FailPoint::alwaysOn);

    // Create a thread that creates the in-progress cluster-wide parameters refresh request in the
    // test scenario.
    stdx::thread backgroundThread(
        [&]() { ASSERT_OK(_refresher->refreshParameters(operationContext())); });

    // Wait for 'backgroundThread' to block on a fail-point. Note that each time we enter the
    // blocking failpoint, we increment "timesEntered" by 2, because we first check for
    // 'shouldFail()' and then call 'pauseWhileSet()'.
    blockAndSucceedClusterParameterRefreshFailPoint->waitForTimesEntered(
        blockAndSucceedClusterParameterRefreshFailPointEntryCount + 2);

    // Create a thread which generates a concurrent cluster-wide parameters refresh request with
    // Read Your Writes consistency.
    stdx::thread concurrentRequestThread([&]() {
        const bool kEnsureReadYourWritesConsistency = true;
        Status status =
            _refresher->refreshParameters(operationContext(), kEnsureReadYourWritesConsistency);

        // Verify that "blockAndFailClusterParameterRefresh" fail-point was hit since that implies
        // that a new request to refresh cluster-wide parameters was generated.
        ASSERT_EQ(status, Status(ErrorCodes::FailPointEnabled, "..."));
    });

    // Wait until 'concurrentRequestThread' passes the fail-point and blocks on future wait.
    countPromiseWaitersClusterParameterRefreshFailPoint->waitForTimesEntered(
        countPromiseWaitersClusterParameterRefreshFailPointEntryCount + 1);

    // Program the failpoint so the 'concurrentRequestThread' fails the cluster-wide parameter
    // refresh process.
    const auto blockAndFailClusterParameterRefreshFailPointEntryCount =
        blockAndFailClusterParameterRefreshFailPoint->setMode(FailPoint::alwaysOn);

    // Unblock 'backgroundThread'.
    blockAndSucceedClusterParameterRefreshFailPoint->setMode(FailPoint::off);

    backgroundThread.join();

    // Wait until 'concurrentRequestThread' blocks on "blockAndFailClusterParameterRefresh"
    // fail-point.
    blockAndFailClusterParameterRefreshFailPoint->waitForTimesEntered(
        blockAndFailClusterParameterRefreshFailPointEntryCount + 2);

    // Unblock 'concurrentRequestThread'.
    blockAndFailClusterParameterRefreshFailPoint->setMode(FailPoint::off);

    concurrentRequestThread.join();
}

TEST_F(ClusterServerParameterRefresherTest, GetFCVAndClusterParamsFunctionRetriesOnSnapshotError) {
    // gMultitenancySupport is set to false so that the function to get FCV and cluster parameters
    // skips making a request to get the list of tenants.
    gMultitenancySupport = false;
    auto originalMultitenancySupport = gMultitenancySupport;
    ON_BLOCK_EXIT([&] { gMultitenancySupport = originalMultitenancySupport; });

    auto future = launchAsync([&] {
        multiversion::FeatureCompatibilityVersion fcv;
        TenantIdMap<StringMap<BSONObj>> tenantParameterDocs;

        std::tie(fcv, tenantParameterDocs) = getFCVAndClusterParametersFromConfigServer();

        assertFCVAndClusterParamsMatchExpected(fcv, tenantParameterDocs.find(boost::none)->second);
    });

    // The function should retry when the config server responds with a SnapshotTooOld error.
    onCommand([&](const executor::RemoteCommandRequest& request) {
        return Status(ErrorCodes::SnapshotTooOld, "Snapshot too old");
    });

    // The function should succeed on retry when successful responses are provided by the config
    // server.
    mockFCVAndClusterParametersResponses();

    future.default_timed_get();
}

}  // namespace
}  // namespace mongo
