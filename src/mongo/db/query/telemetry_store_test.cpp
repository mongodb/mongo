/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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

#include "mongo/bson/simple_bsonobj_comparator.h"
#include "mongo/db/catalog/rename_collection.h"
#include "mongo/db/query/query_feature_flags_gen.h"
#include "mongo/db/query/telemetry.h"
#include "mongo/db/service_context_test_fixture.h"
#include "mongo/idl/server_parameter_test_util.h"
#include "mongo/unittest/unittest.h"

namespace mongo::telemetry {

class TelemetryStoreTest : public ServiceContextTest {};

TEST_F(TelemetryStoreTest, BasicUsage) {
    // Turning on the flag at runtime will crash as telemetry store registerer (which creates the
    // telemetry store) is called at start up and if flag is off, the telemetry store will have
    // never been created. Thus, instead of turning on the flag at runtime and crashing, we skip the
    // test if telemetry feature flag is off.
    if (!feature_flags::gFeatureFlagTelemetry.isEnabledAndIgnoreFCV()) {
        return;
    }
    TelemetryStore telStore{5000000, 1000};

    auto getMetrics = [&](BSONObj& key) {
        auto lookupResult = telStore.lookup(key);
        return *lookupResult.getValue();
    };

    auto collectMetrics = [&](BSONObj& key) {
        TelemetryMetrics* metrics;
        auto lookupResult = telStore.lookup(key);
        if (!lookupResult.isOK()) {
            telStore.put(key, TelemetryMetrics{});
            lookupResult = telStore.lookup(key);
        }
        metrics = lookupResult.getValue();
        metrics->execCount += 1;
        metrics->lastExecutionMicros += 123456;
    };

    auto query1 = BSON("query" << 1 << "xEquals" << 42);
    // same value, different instance (tests hashing & equality)
    auto query1x = BSON("query" << 1 << "xEquals" << 42);
    auto query2 = BSON("query" << 2 << "yEquals" << 43);

    collectMetrics(query1);
    collectMetrics(query1);
    collectMetrics(query1x);
    collectMetrics(query2);

    ASSERT_EQ(getMetrics(query1).execCount, 3);
    ASSERT_EQ(getMetrics(query1x).execCount, 3);
    ASSERT_EQ(getMetrics(query2).execCount, 1);

    auto collectMetricsWithLock = [&](BSONObj& key) {
        auto [lookupResult, lock] = telStore.getWithPartitionLock(key);
        auto metrics = lookupResult.getValue();
        metrics->execCount += 1;
        metrics->lastExecutionMicros += 123456;
    };

    collectMetricsWithLock(query1x);
    collectMetricsWithLock(query2);

    ASSERT_EQ(getMetrics(query1).execCount, 4);
    ASSERT_EQ(getMetrics(query1x).execCount, 4);
    ASSERT_EQ(getMetrics(query2).execCount, 2);

    int numKeys = 0;

    telStore.forEach([&](const BSONObj& key, const TelemetryMetrics& entry) { numKeys++; });

    ASSERT_EQ(numKeys, 2);
}

}  // namespace mongo::telemetry
