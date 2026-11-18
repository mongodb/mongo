// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/executor/task_executor_pool.h"

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/commands/server_status/server_status_metric.h"
#include "mongo/db/topology/cluster_role.h"
#include "mongo/executor/network_interface_mock.h"
#include "mongo/executor/task_executor.h"
#include "mongo/executor/thread_pool_task_executor_test_fixture.h"
#include "mongo/otel/metrics/metric_names.h"
#include "mongo/otel/metrics/metrics_test_util.h"
#include "mongo/unittest/unittest.h"

#include <cstddef>
#include <memory>
#include <vector>

namespace mongo::executor {
namespace {

class TaskExecutorPoolSizeMetricTest : public unittest::Test {
protected:
    void setUp() override {
        if (!_capturer.canReadMetrics())
            GTEST_SKIP() << "OTel metrics reader unavailable";
    }

    static std::shared_ptr<TaskExecutor> makeExecutor() {
        return makeThreadPoolTestExecutor(std::make_unique<NetworkInterfaceMock>());
    }

    /** Populates a pool with 'numExecutors' pooled executors plus the fixed executor. */
    static void populate(TaskExecutorPool& pool, size_t numExecutors) {
        std::vector<std::shared_ptr<TaskExecutor>> executors;
        for (size_t i = 0; i < numExecutors; ++i) {
            executors.push_back(makeExecutor());
        }
        pool.addExecutors(std::move(executors), makeExecutor());
    }

    int64_t readPoolSizeMetric() {
        return _capturer.readInt64Gauge(
            otel::metrics::MetricNames::kNetworkEgressTaskExecutorPoolSize);
    }

private:
    otel::metrics::OtelMetricsCapturer _capturer;
};

TEST_F(TaskExecutorPoolSizeMetricTest, ReportsNumberOfPooledExecutors) {
    // The fixed executor is built by populate() but is not part of the count.
    for (const auto numExecutors : {1, 4}) {
        TaskExecutorPool pool;
        populate(pool, numExecutors);
        ASSERT_EQ(readPoolSizeMetric(), numExecutors);
    }
}

TEST_F(TaskExecutorPoolSizeMetricTest, IsRegisteredInServerStatusMetricsTree) {
    TaskExecutorPool pool;
    populate(pool, 3);

    // The metric is registered for ClusterRole::None, so it must be exposed on every node type.
    BSONObjBuilder builder;
    globalMetricTreeSet()[ClusterRole::None].appendTo(builder);
    const BSONObj obj = builder.obj();
    ASSERT_EQ(obj["metrics"]["taskExecutorPool"]["size"].safeNumberLong(), 3) << obj;
}

}  // namespace
}  // namespace mongo::executor
