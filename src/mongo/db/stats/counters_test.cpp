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

#include "mongo/db/stats/counters.h"

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/otel/metrics/metric_names.h"
#include "mongo/otel/metrics/metrics_test_util.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

using ::mongo::otel::metrics::MetricNames;
using ::mongo::otel::metrics::OtelMetricsCapturer;

BSONObj getNetworkBson(NetworkCounter& nc) {
    BSONObjBuilder bob;
    nc.append(bob);
    return bob.obj();
}

TEST(NetworkCounterOtelTest, IngressCountersAreExported) {
    OtelMetricsCapturer capturer;
    if (!capturer.canReadMetrics()) {
        return;
    }

    NetworkCounter nc;

    nc.hitLogicalIn(NetworkCounter::ConnectionType::kIngress, 100);
    nc.hitLogicalIn(NetworkCounter::ConnectionType::kIngress, 50);
    nc.hitLogicalOut(NetworkCounter::ConnectionType::kIngress, 25);

    EXPECT_EQ(150, capturer.readInt64Counter(MetricNames::kNetworkIngressBytesIn));
    EXPECT_EQ(25, capturer.readInt64Counter(MetricNames::kNetworkIngressBytesOut));
    EXPECT_EQ(2, capturer.readInt64Counter(MetricNames::kNetworkIngressNumRequests));
}

TEST(NetworkCounterOtelTest, EgressCountersAreExported) {
    OtelMetricsCapturer capturer;
    if (!capturer.canReadMetrics()) {
        return;
    }

    NetworkCounter nc;

    nc.hitLogicalIn(NetworkCounter::ConnectionType::kEgress, 200);
    nc.hitLogicalOut(NetworkCounter::ConnectionType::kEgress, 80);

    EXPECT_EQ(200, capturer.readInt64Counter(MetricNames::kNetworkEgressBytesIn));
    EXPECT_EQ(80, capturer.readInt64Counter(MetricNames::kNetworkEgressBytesOut));
    EXPECT_EQ(1, capturer.readInt64Counter(MetricNames::kNetworkEgressNumRequests));
}

TEST(NetworkCounterOtelTest, SlowDNSOperationsAreExported) {
    OtelMetricsCapturer capturer;
    if (!capturer.canReadMetrics()) {
        return;
    }

    NetworkCounter nc;

    nc.incrementNumSlowDNSOperations();
    nc.incrementNumSlowDNSOperations();
    nc.incrementNumSlowDNSOperations();

    EXPECT_EQ(3, capturer.readInt64Counter(MetricNames::kNetworkNumSlowDNSOperations));
}

TEST(NetworkCounterOtelTest, SlowSSLOperationsAreExported) {
    OtelMetricsCapturer capturer;
    if (!capturer.canReadMetrics()) {
        return;
    }

    NetworkCounter nc;

    nc.incrementNumSlowSSLOperations();
    nc.incrementNumSlowSSLOperations();

    EXPECT_EQ(2, capturer.readInt64Counter(MetricNames::kNetworkNumSlowSSLOperations));
}

TEST(NetworkCounterBsonTest, IngressBytesInAndNumRequests) {
    NetworkCounter nc;

    nc.hitLogicalIn(NetworkCounter::ConnectionType::kIngress, 300);
    nc.hitLogicalIn(NetworkCounter::ConnectionType::kIngress, 700);

    BSONObj obj = getNetworkBson(nc);
    EXPECT_EQ(1000, obj["bytesIn"].numberLong());
    EXPECT_EQ(2, obj["numRequests"].numberLong());
}

TEST(NetworkCounterBsonTest, IngressBytesOut) {
    NetworkCounter nc;

    nc.hitLogicalOut(NetworkCounter::ConnectionType::kIngress, 512);

    BSONObj obj = getNetworkBson(nc);
    EXPECT_EQ(512, obj["bytesOut"].numberLong());
}

TEST(NetworkCounterBsonTest, EgressSubObject) {
    NetworkCounter nc;

    nc.hitLogicalIn(NetworkCounter::ConnectionType::kEgress, 400);
    nc.hitLogicalOut(NetworkCounter::ConnectionType::kEgress, 100);

    BSONObj obj = getNetworkBson(nc);
    ASSERT_TRUE(obj.hasField("egress"));
    BSONObj egress = obj["egress"].Obj();
    EXPECT_EQ(400, egress["bytesIn"].numberLong());
    EXPECT_EQ(100, egress["bytesOut"].numberLong());
    EXPECT_EQ(1, egress["numRequests"].numberLong());
}

TEST(NetworkCounterBsonTest, NumSlowDNSOperations) {
    NetworkCounter nc;

    nc.incrementNumSlowDNSOperations();
    nc.incrementNumSlowDNSOperations();

    BSONObj obj = getNetworkBson(nc);
    EXPECT_EQ(2, obj["numSlowDNSOperations"].numberLong());
}

TEST(NetworkCounterBsonTest, NumSlowSSLOperations) {
    NetworkCounter nc;

    nc.incrementNumSlowSSLOperations();

    BSONObj obj = getNetworkBson(nc);
    EXPECT_EQ(1, obj["numSlowSSLOperations"].numberLong());
}

}  // namespace
}  // namespace mongo
