/**
 *    Copyright (C) 2026-present MongoDB, Inc.
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

#include "mongo/otel/metrics/instrumentation/system_mount_metrics.h"

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/otel/metrics/metrics_test_util.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

using otel::metrics::DynamicMetricNameMaker;
using otel::metrics::OtelMetricsCapturer;

constexpr StringData kDataMount = "/data"_sd;
constexpr StringData kTmpMount = "/tmp"_sd;

// All tests register SystemMountMetrics with this fixed mountpoint set. The global MetricsService
// retains registrations across tests within the same binary, so every test must use the same set.
const std::vector<std::string> kTestMountpoints = {std::string(kDataMount)};

constexpr StringData kDataCapacity = "systemMetrics.mounts.data.capacity"_sd;
constexpr StringData kDataAvailable = "systemMetrics.mounts.data.available"_sd;
constexpr StringData kDataFree = "systemMetrics.mounts.data.free"_sd;
constexpr StringData kTmpCapacity = "systemMetrics.mounts.tmp.capacity"_sd;

BSONObj makeMountsBson(StringData mountpoint,
                       long long capacity,
                       long long available,
                       long long free) {
    BSONObjBuilder b;
    {
        BSONObjBuilder sub(b.subobjStart(mountpoint));
        sub.appendNumber("capacity", capacity);
        sub.appendNumber("available", available);
        sub.appendNumber("free", free);
    }
    return b.obj();
}

class SystemMountOtelMetricsTest : public unittest::Test {
protected:
    void setUp() override {
        if (!OtelMetricsCapturer::canReadMetrics()) {
            GTEST_SKIP() << "Skipping test: OTel metrics unavailable on this platform";
        }
    }

    OtelMetricsCapturer _capturer;
    SystemMountMetrics _metrics{kTestMountpoints};
};

TEST_F(SystemMountOtelMetricsTest, UpdateSetsGaugeValues) {
    _metrics.update(makeMountsBson(kDataMount, 1000, 400, 500));

    ASSERT_EQ(_capturer.readInt64Gauge(DynamicMetricNameMaker::make(kDataCapacity)), 1000);
    ASSERT_EQ(_capturer.readInt64Gauge(DynamicMetricNameMaker::make(kDataAvailable)), 400);
    ASSERT_EQ(_capturer.readInt64Gauge(DynamicMetricNameMaker::make(kDataFree)), 500);
}

TEST_F(SystemMountOtelMetricsTest, UpdateIgnoresUndeclaredMountpoints) {
    _metrics.update(makeMountsBson(kDataMount, 1000, 400, 500));

    BSONObjBuilder both;
    {
        BSONObjBuilder sub(both.subobjStart(kDataMount));
        sub.appendNumber("capacity", 2000LL);
        sub.appendNumber("available", 800LL);
        sub.appendNumber("free", 1000LL);
    }
    {
        // /tmp was never registered — its data must be ignored.
        BSONObjBuilder sub(both.subobjStart(kTmpMount));
        sub.appendNumber("capacity", 500LL);
        sub.appendNumber("available", 200LL);
        sub.appendNumber("free", 250LL);
    }
    _metrics.update(both.obj());

    ASSERT_EQ(_capturer.readInt64Gauge(DynamicMetricNameMaker::make(kDataCapacity)), 2000);

    // /tmp was never registered, so its metric name does not exist in the service.
    ASSERT_THROWS_CODE(_capturer.readInt64Gauge(DynamicMetricNameMaker::make(kTmpCapacity)),
                       DBException,
                       ErrorCodes::KeyNotFound);
}

TEST_F(SystemMountOtelMetricsTest, UpdateIsIdempotentForSameValues) {
    BSONObj bson = makeMountsBson(kDataMount, 8000, 3000, 4000);
    _metrics.update(bson);
    _metrics.update(bson);

    ASSERT_EQ(_capturer.readInt64Gauge(DynamicMetricNameMaker::make(kDataCapacity)), 8000);
}

}  // namespace
}  // namespace mongo
