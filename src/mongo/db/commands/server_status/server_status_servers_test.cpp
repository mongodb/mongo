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
#include "mongo/otel/metrics/metrics_service.h"
#include "mongo/otel/metrics/metrics_test_util.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {
class ServerStatusServersTest : public DBCommandTestFixture {};

TEST_F(ServerStatusServersTest, IncludesOtelMetrics) {
    otel::metrics::OtelMetricsCapturer capturer;
    auto& metricsService = otel::metrics::MetricsService::instance();
    auto& counter = metricsService.createInt64Counter(otel::metrics::MetricNames::kTest1,
                                                      "description",
                                                      otel::metrics::MetricUnit::kSeconds,
                                                      {.inServerStatus = true});
    counter.add(11);

    BSONObj result = runCommand(BSON("serverStatus" << 1 << "otelMetrics" << 1));
    ASSERT_TRUE(result.hasField("otelMetrics"));

    BSONObj otelMetrics = result.getObjectField("otelMetrics");
    ASSERT_TRUE(otelMetrics.hasField("test_only.metric1_seconds"));
    EXPECT_EQ(otelMetrics.getIntField("test_only.metric1_seconds"), 11);
}

}  // namespace
}  // namespace mongo
