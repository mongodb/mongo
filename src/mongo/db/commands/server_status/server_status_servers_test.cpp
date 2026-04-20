/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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
#include "mongo/unittest/unittest.h"

#include <cstdint>

#include <fmt/format.h>

namespace mongo {
namespace {

class ServerStatusServersTest : public DBCommandTestFixture {
public:
    // The spill engine is disabled by default in test fixtures to avoid contention from opening
    // the spill WiredTiger instance during concurrent unit test runs. This test exercises the
    // serverStatus command, which includes a SpillWiredTigerServerStatusSection that requires
    // the spill engine to be initialized.
    ServerStatusServersTest() : DBCommandTestFixture(Options{}.enableSpillEngine()) {}

    void setUp() override {
        DBCommandTestFixture::setUp();
    }

    void tearDown() override {
        otel::metrics::MetricsService::instance().clearForTests();
        DBCommandTestFixture::tearDown();
    }
};

TEST_F(ServerStatusServersTest, IncludeUnderMetricsSection) {
    auto& metricsService = otel::metrics::MetricsService::instance();
    otel::metrics::CounterOptions options{
        .serverStatusOptions = otel::metrics::ServerStatusOptions{.dottedPath = "test.metric1",
                                                                  .role = ClusterRole::None}};
    auto& counter = metricsService.createInt64Counter(otel::metrics::MetricNames::kTest1,
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

    auto& counter = metricsService.createInt64Counter(otel::metrics::MetricNames::kTest1,
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

    void tearDown() override {
        otel::metrics::MetricsService::instance().clearForTests();
        ServiceContextTest::tearDown();
    }

protected:
    otel::metrics::Counter<int64_t>& createCounter(otel::metrics::MetricsService& metricsService,
                                                   otel::metrics::MetricName metricName,
                                                   std::string dottedPath,
                                                   ClusterRole role) {
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

    BSONObj getMetricsSection(StringData pathPrefix) {
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
    createCounter(metricsService,
                  otel::metrics::MetricNames::kTestShardMergeNone,
                  "test.noneMetric",
                  ClusterRole::None)
        .add(11);
    createCounter(metricsService,
                  otel::metrics::MetricNames::kTestShardMergeShard,
                  "test.shardMetric",
                  ClusterRole::ShardServer)
        .add(22);
    createCounter(metricsService,
                  otel::metrics::MetricNames::kTestShardMergeRouter,
                  "test.routerMetric",
                  ClusterRole::RouterServer)
        .add(33);

    BSONObj section = getMetricsSection("test");
    EXPECT_EQ(section.getIntField("noneMetric"), 11);
    EXPECT_EQ(section.getIntField("shardMetric"), 22);
    ASSERT_FALSE(section.hasField("routerMetric")) << section.toString();
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
                  otel::metrics::MetricNames::kTestRouterMergeNone,
                  "test.noneMetric",
                  ClusterRole::None)
        .add(11);
    createCounter(metricsService,
                  otel::metrics::MetricNames::kTestRouterMergeShard,
                  "test.shardMetric",
                  ClusterRole::ShardServer)
        .add(22);
    createCounter(metricsService,
                  otel::metrics::MetricNames::kTestRouterMergeRouter,
                  "test.routerMetric",
                  ClusterRole::RouterServer)
        .add(33);

    BSONObj section = getMetricsSection("test");
    EXPECT_EQ(section.getIntField("noneMetric"), 11);
    ASSERT_FALSE(section.hasField("shardMetric")) << section.toString();
    EXPECT_EQ(section.getIntField("routerMetric"), 33);
}

}  // namespace
}  // namespace mongo
