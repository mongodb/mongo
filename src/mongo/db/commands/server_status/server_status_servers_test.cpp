// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/bson/bsonobj.h"
#include "mongo/db/commands/db_command_test_fixture.h"
#include "mongo/db/commands/server_status/server_status_metric.h"
#include "mongo/db/database_name.h"
#include "mongo/db/read_write_concern_defaults_cache_lookup_mock.h"
#include "mongo/db/service_context_test_fixture.h"
#include "mongo/db/service_entry_point_shard_role.h"
#include "mongo/db/topology/cluster_role.h"
#include "mongo/otel/metrics/metric_names.h"
#include "mongo/otel/metrics/metrics_service.h"
#include "mongo/otel/metrics/metrics_test_util.h"
#include "mongo/s/service_entry_point_router_role.h"
#include "mongo/stdx/unordered_set.h"
#include "mongo/unittest/unittest.h"

#include <cstdint>
#include <string>
#include <string_view>

#include <fmt/format.h>

namespace mongo {
namespace {

using otel::metrics::MetricName;
using otel::metrics::MetricNames;

/**
 * Verifies that the provided values have not been used in this test file. Since this test is
 * testing static objects, reusing values will result in cross-test contamination. Resetting those
 * static objects is non-trivial as other parts of the test fixtures may rely on them being static
 * to work correctly.
 *
 * TODO SERVER-125804: Remove these and make these tests simpler.
 */
MetricName verifyNotUsedInTest(MetricName name) {
    static stdx::unordered_set<std::string_view> usedNames;
    invariant(usedNames.insert(name.getName()).second);
    return name;
}
std::string verifyPathNotUsedInTest(std::string path) {
    static stdx::unordered_set<std::string> usedPaths;
    invariant(usedPaths.insert(path).second);
    return path;
}

class ServerStatusServersTest : public DBCommandTestFixture {
public:
    // The spill engine is disabled by default in test fixtures to avoid contention from opening
    // the spill WiredTiger instance during concurrent unit test runs. This test exercises the
    // serverStatus command, which includes a SpillWiredTigerServerStatusSection that requires
    // the spill engine to be initialized.
    ServerStatusServersTest() : DBCommandTestFixture(Options{}.enableSpillEngine()) {}
};

TEST_F(ServerStatusServersTest, IncludeUnderMetricsSection) {
    auto& metricsService = otel::metrics::MetricsService::instance();
    otel::metrics::CounterOptions options{
        .serverStatusOptions = otel::metrics::ServerStatusOptions{
            .dottedPath = verifyPathNotUsedInTest("test.metric1"), .role = ClusterRole::None}};
    auto& counter = metricsService.createInt64Counter(verifyNotUsedInTest(MetricNames::kTest1),
                                                      "description",
                                                      otel::metrics::MetricUnit::kSeconds,
                                                      options);
    counter.add(11);

    BSONObj resultObj = runCommand(BSON("serverStatus" << 1 << "metrics" << 1));
    ASSERT_TRUE(resultObj.hasField("metrics"));
    BSONObj metricsObj = resultObj.getObjectField("metrics");
    ASSERT_EQ(metricsObj["test"]["metric1"].Long(), 11);
}

TEST_F(ServerStatusServersTest, ExcludeWhenServerStatusOptionsNotSet) {
    auto& metricsService = otel::metrics::MetricsService::instance();
    otel::metrics::CounterOptions options{};
    ASSERT_FALSE(options.serverStatusOptions.has_value());

    auto& counter = metricsService.createInt64Counter(verifyNotUsedInTest(MetricNames::kTest2),
                                                      "description",
                                                      otel::metrics::MetricUnit::kSeconds,
                                                      options);
    counter.add(11);

    BSONObj resultObj = runCommand(BSON("serverStatus" << 1 << "metrics" << 1));
    // The "metrics" section may still exist because of non-otel serverStatus metrics.
    if (resultObj.hasField("metrics")) {
        BSONObj metricsObj = resultObj.getObjectField("metrics");
        ASSERT_FALSE(metricsObj.hasField("test_only")) << metricsObj.toString();
    }
}

class ServerStatusServersRoleTestFixture : public ServiceContextTest {
public:
    void setUp() override {
        ServiceContextTest::setUp();
        ReadWriteConcernDefaults::create(getService(), _lookupMock.getFetchDefaultsFn());
    }

protected:
    otel::metrics::Counter<int64_t>& createCounter(otel::metrics::MetricsService& metricsService,
                                                   otel::metrics::MetricName metricName,
                                                   std::string dottedPath,
                                                   ClusterRole role) {
        verifyNotUsedInTest(metricName);
        verifyPathNotUsedInTest(dottedPath);
        return metricsService.createInt64Counter(metricName,
                                                 "description",
                                                 otel::metrics::MetricUnit::kSeconds,
                                                 otel::metrics::CounterOptions{
                                                     .serverStatusOptions =
                                                         otel::metrics::ServerStatusOptions{
                                                             .dottedPath = std::move(dottedPath),
                                                             .role = role,
                                                         },
                                                 });
    }

    BSONObj getMetricsSection(std::string_view pathPrefix) {
        Service* const service = getServiceContext()->getService();
        ServiceContext::UniqueClient client =
            service->makeClient("ServerStatusServersRoleTestFixture");
        AlternativeClientRegion acr(client);
        auto opCtx = cc().makeOperationContext();
        DBDirectClient dbclient(opCtx.get());

        BSONObj resultObj;
        // Specify none: 1 to exclude all other sections.
        dbclient.runCommand(DatabaseName::kAdmin,
                            BSON("serverStatus" << 1 << "none" << 1 << "metrics" << 1),
                            resultObj);
        ASSERT_OK(getStatusFromWriteCommandReply(resultObj));

        ASSERT_TRUE(resultObj.hasField("metrics"));
        BSONObj metricsObj = resultObj.getObjectField("metrics");
        return metricsObj.getObjectField(pathPrefix).getOwned();
    }

private:
    // Allows for commands to not specify a default read/write concern.
    ReadWriteConcernDefaultsLookupMock _lookupMock;
};

class ServerStatusServersRoleShardTest : public virtual service_context_test::ShardRoleOverride,
                                         public ServerStatusServersRoleTestFixture {

    void setUp() override {
        ServerStatusServersRoleTestFixture::setUp();
        // Initialize the serviceEntryPoint so that DBDirectClient can function.
        getService()->setServiceEntryPoint(std::make_unique<ServiceEntryPointShardRole>());

        const auto service = getServiceContext();
        auto replCoord =
            std::make_unique<repl::ReplicationCoordinatorMock>(service, repl::ReplSettings{});
        ASSERT_OK(replCoord->setFollowerMode(repl::MemberState::RS_PRIMARY));
        repl::ReplicationCoordinator::set(service, std::move(replCoord));
    }
};

TEST_F(ServerStatusServersRoleShardTest, MergesNoneAndShardMetricTreesExcludesRouter) {
    auto& metricsService = otel::metrics::MetricsService::instance();
    createCounter(
        metricsService, MetricNames::kTestShardMergeNone, "test.noneMetricShard", ClusterRole::None)
        .add(11);
    createCounter(metricsService,
                  MetricNames::kTestShardMergeShard,
                  "test.shardMetricShard",
                  ClusterRole::ShardServer)
        .add(22);
    createCounter(metricsService,
                  MetricNames::kTestShardMergeRouter,
                  "test.routerMetricShard",
                  ClusterRole::RouterServer)
        .add(33);

    BSONObj section = getMetricsSection("test");
    EXPECT_EQ(section.getIntField("noneMetricShard"), 11);
    EXPECT_EQ(section.getIntField("shardMetricShard"), 22);
    ASSERT_FALSE(section.hasField("routerMetricShard")) << section.toString();
}

class ServerStatusServersRoleRouterTest : public virtual service_context_test::RouterRoleOverride,
                                          public ServerStatusServersRoleTestFixture {
    void setUp() override {
        ServerStatusServersRoleTestFixture::setUp();
        // Initialize the serviceEntryPoint so that DBDirectClient can function.
        getService()->setServiceEntryPoint(std::make_unique<ServiceEntryPointRouterRole>());
    }
};

TEST_F(ServerStatusServersRoleRouterTest, MergesNoneAndRouterMetricTreesExcludesShard) {
    auto& metricsService = otel::metrics::MetricsService::instance();
    createCounter(metricsService,
                  MetricNames::kTestRouterMergeNone,
                  "test.noneMetricRouter",
                  ClusterRole::None)
        .add(11);
    createCounter(metricsService,
                  MetricNames::kTestRouterMergeShard,
                  "test.shardMetricRouter",
                  ClusterRole::ShardServer)
        .add(22);
    createCounter(metricsService,
                  MetricNames::kTestRouterMergeRouter,
                  "test.routerMetricRouter",
                  ClusterRole::RouterServer)
        .add(33);

    BSONObj section = getMetricsSection("test");
    EXPECT_EQ(section.getIntField("noneMetricRouter"), 11);
    ASSERT_FALSE(section.hasField("shardMetricRouter")) << section.toString();
    EXPECT_EQ(section.getIntField("routerMetricRouter"), 33);
}

}  // namespace
}  // namespace mongo
