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
#include "mongo/db/commands/server_status/server_status_metric.h"
#include "mongo/otel/metrics/metric_names.h"
#include "mongo/otel/metrics/metrics_test_util.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

using ::mongo::otel::metrics::MetricNames;
using ::mongo::otel::metrics::OtelMetricsCapturer;

// Read a counter from the global serverStatus tree at `dottedPath` (rooted under "metrics.").
long long readServerStatusMetric(StringData dottedPath) {
    BSONObjBuilder bob;
    globalMetricTreeSet()[ClusterRole::None].appendTo(bob);
    BSONObj root = bob.obj();
    BSONObj cur = root["metrics"].Obj();
    std::string path(dottedPath);
    size_t pos = 0;
    while (true) {
        size_t dot = path.find('.', pos);
        if (dot == std::string::npos)
            return cur[path.substr(pos)].Long();
        cur = cur[path.substr(pos, dot - pos)].Obj();
        pos = dot + 1;
    }
}

BSONObj getNetworkBson(NetworkCounter& nc) {
    BSONObjBuilder bob;
    nc.append(bob);
    return bob.obj();
}

TEST(NetworkCounterOtelTest, IngressCountersAreExported) {
    OtelMetricsCapturer capturer;
    if (!capturer.canReadMetrics()) {
        GTEST_SKIP() << "OTel not available";
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
        GTEST_SKIP() << "OTel not available";
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
        GTEST_SKIP() << "OTel not available";
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
        GTEST_SKIP() << "OTel not available";
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

TEST(PlanCacheCountersOtelTest, ClassicCountersAreExported) {
    OtelMetricsCapturer capturer;
    if (!capturer.canReadMetrics()) {
        GTEST_SKIP() << "OTel not available";
    }

    PlanCacheCounters counters;
    int64_t hitsBase = capturer.readInt64Counter(MetricNames::kPlanCacheClassicHits);
    int64_t missesBase = capturer.readInt64Counter(MetricNames::kPlanCacheClassicMisses);
    int64_t skippedBase = capturer.readInt64Counter(MetricNames::kPlanCacheClassicSkipped);
    int64_t replannedBase = capturer.readInt64Counter(MetricNames::kPlanCacheClassicReplanned);
    int64_t replannedIsCachedBase =
        capturer.readInt64Counter(MetricNames::kPlanCacheClassicReplannedPlanIsCachedPlan);
    int64_t evictedBase =
        capturer.readInt64Counter(MetricNames::kPlanCacheClassicCachedPlansEvicted);
    int64_t inactiveReplacedBase =
        capturer.readInt64Counter(MetricNames::kPlanCacheClassicInactiveCachedPlansReplaced);

    counters.incrementClassicHitsCounter();
    counters.incrementClassicHitsCounter();
    counters.incrementClassicMissesCounter();
    counters.incrementClassicSkippedCounter();
    counters.incrementClassicReplannedCounter();
    counters.incrementClassicReplannedPlanIsCachedPlanCounter();
    counters.incrementClassicCachedPlansEvictedCounter(5);
    counters.incrementClassicInactiveCachedPlansReplacedCounter();

    EXPECT_EQ(hitsBase + 2, capturer.readInt64Counter(MetricNames::kPlanCacheClassicHits));
    EXPECT_EQ(missesBase + 1, capturer.readInt64Counter(MetricNames::kPlanCacheClassicMisses));
    EXPECT_EQ(skippedBase + 1, capturer.readInt64Counter(MetricNames::kPlanCacheClassicSkipped));
    EXPECT_EQ(replannedBase + 1,
              capturer.readInt64Counter(MetricNames::kPlanCacheClassicReplanned));
    EXPECT_EQ(replannedIsCachedBase + 1,
              capturer.readInt64Counter(MetricNames::kPlanCacheClassicReplannedPlanIsCachedPlan));
    EXPECT_EQ(evictedBase + 5,
              capturer.readInt64Counter(MetricNames::kPlanCacheClassicCachedPlansEvicted));
    EXPECT_EQ(inactiveReplacedBase + 1,
              capturer.readInt64Counter(MetricNames::kPlanCacheClassicInactiveCachedPlansReplaced));
}

TEST(PlanCacheCountersOtelTest, SbeCountersAreExported) {
    OtelMetricsCapturer capturer;
    if (!capturer.canReadMetrics()) {
        GTEST_SKIP() << "OTel not available";
    }

    PlanCacheCounters counters;
    int64_t hitsBase = capturer.readInt64Counter(MetricNames::kPlanCacheSbeHits);
    int64_t missesBase = capturer.readInt64Counter(MetricNames::kPlanCacheSbeMisses);
    int64_t skippedBase = capturer.readInt64Counter(MetricNames::kPlanCacheSbeSkipped);
    int64_t replannedBase = capturer.readInt64Counter(MetricNames::kPlanCacheSbeReplanned);
    int64_t replannedIsCachedBase =
        capturer.readInt64Counter(MetricNames::kPlanCacheSbeReplannedPlanIsCachedPlan);
    int64_t evictedBase = capturer.readInt64Counter(MetricNames::kPlanCacheSbeCachedPlansEvicted);
    int64_t inactiveReplacedBase =
        capturer.readInt64Counter(MetricNames::kPlanCacheSbeInactiveCachedPlansReplaced);

    counters.incrementSbeHitsCounter();
    counters.incrementSbeHitsCounter();
    counters.incrementSbeMissesCounter();
    counters.incrementSbeSkippedCounter();
    counters.incrementSbeReplannedCounter();
    counters.incrementSbeReplannedPlanIsCachedPlanCounter();
    counters.incrementSbeCachedPlansEvictedCounter(3);
    counters.incrementSbeInactiveCachedPlansReplacedCounter();

    EXPECT_EQ(hitsBase + 2, capturer.readInt64Counter(MetricNames::kPlanCacheSbeHits));
    EXPECT_EQ(missesBase + 1, capturer.readInt64Counter(MetricNames::kPlanCacheSbeMisses));
    EXPECT_EQ(skippedBase + 1, capturer.readInt64Counter(MetricNames::kPlanCacheSbeSkipped));
    EXPECT_EQ(replannedBase + 1, capturer.readInt64Counter(MetricNames::kPlanCacheSbeReplanned));
    EXPECT_EQ(replannedIsCachedBase + 1,
              capturer.readInt64Counter(MetricNames::kPlanCacheSbeReplannedPlanIsCachedPlan));
    EXPECT_EQ(evictedBase + 3,
              capturer.readInt64Counter(MetricNames::kPlanCacheSbeCachedPlansEvicted));
    EXPECT_EQ(inactiveReplacedBase + 1,
              capturer.readInt64Counter(MetricNames::kPlanCacheSbeInactiveCachedPlansReplaced));
}

TEST(QueryFrameworkCountersOtelTest, CountersAreExported) {
    OtelMetricsCapturer capturer;
    if (!capturer.canReadMetrics()) {
        GTEST_SKIP() << "OTel not available";
    }

    QueryFrameworkCounters counters;
    int64_t findSbeBase = capturer.readInt64Counter(MetricNames::kQueryFrameworkFindSbe);
    int64_t findClassicBase = capturer.readInt64Counter(MetricNames::kQueryFrameworkFindClassic);
    int64_t aggSbeOnlyBase =
        capturer.readInt64Counter(MetricNames::kQueryFrameworkAggregateSbeOnly);
    int64_t aggClassicOnlyBase =
        capturer.readInt64Counter(MetricNames::kQueryFrameworkAggregateClassicOnly);
    int64_t aggSbeHybridBase =
        capturer.readInt64Counter(MetricNames::kQueryFrameworkAggregateSbeHybrid);
    int64_t aggClassicHybridBase =
        capturer.readInt64Counter(MetricNames::kQueryFrameworkAggregateClassicHybrid);

    counters.incrementFindSbeCounter();
    counters.incrementFindClassicCounter();
    counters.incrementFindClassicCounter();
    counters.incrementAggregateSbeOnlyCounter();
    counters.incrementAggregateClassicOnlyCounter();
    counters.incrementAggregateSbeHybridCounter();
    counters.incrementAggregateClassicHybridCounter();

    EXPECT_EQ(findSbeBase + 1, capturer.readInt64Counter(MetricNames::kQueryFrameworkFindSbe));
    EXPECT_EQ(findClassicBase + 2,
              capturer.readInt64Counter(MetricNames::kQueryFrameworkFindClassic));
    EXPECT_EQ(aggSbeOnlyBase + 1,
              capturer.readInt64Counter(MetricNames::kQueryFrameworkAggregateSbeOnly));
    EXPECT_EQ(aggClassicOnlyBase + 1,
              capturer.readInt64Counter(MetricNames::kQueryFrameworkAggregateClassicOnly));
    EXPECT_EQ(aggSbeHybridBase + 1,
              capturer.readInt64Counter(MetricNames::kQueryFrameworkAggregateSbeHybrid));
    EXPECT_EQ(aggClassicHybridBase + 1,
              capturer.readInt64Counter(MetricNames::kQueryFrameworkAggregateClassicHybrid));
}

TEST(FastPathQueryCountersOtelTest, CountersAreExported) {
    OtelMetricsCapturer capturer;
    if (!capturer.canReadMetrics()) {
        GTEST_SKIP() << "OTel not available";
    }

    FastPathQueryCounters counters;
    int64_t idHackBase = capturer.readInt64Counter(MetricNames::kFastPathIdHack);
    int64_t expressBase = capturer.readInt64Counter(MetricNames::kFastPathExpress);

    counters.incrementIdHackQueryCounter();
    counters.incrementIdHackQueryCounter();
    counters.incrementExpressQueryCounter();

    EXPECT_EQ(idHackBase + 2, capturer.readInt64Counter(MetricNames::kFastPathIdHack));
    EXPECT_EQ(expressBase + 1, capturer.readInt64Counter(MetricNames::kFastPathExpress));
}

TEST(PlanCacheCountersServerStatusTest, ClassicCountersAreExportedToServerStatus) {
    PlanCacheCounters counters;
    long long hitsBase = readServerStatusMetric("query.planCache.classic.hits");
    long long missesBase = readServerStatusMetric("query.planCache.classic.misses");
    long long skippedBase = readServerStatusMetric("query.planCache.classic.skipped");
    long long replannedBase = readServerStatusMetric("query.planCache.classic.replanned");
    long long replannedIsCachedBase =
        readServerStatusMetric("query.planCache.classic.replanned_plan_is_cached_plan");
    long long evictedBase = readServerStatusMetric("query.planCache.classic.cached_plans_evicted");
    long long inactiveReplacedBase =
        readServerStatusMetric("query.planCache.classic.inactive_cached_plans_replaced");

    counters.incrementClassicHitsCounter();
    counters.incrementClassicHitsCounter();
    counters.incrementClassicMissesCounter();
    counters.incrementClassicSkippedCounter();
    counters.incrementClassicReplannedCounter();
    counters.incrementClassicReplannedPlanIsCachedPlanCounter();
    counters.incrementClassicCachedPlansEvictedCounter(5);
    counters.incrementClassicInactiveCachedPlansReplacedCounter();

    EXPECT_EQ(hitsBase + 2, readServerStatusMetric("query.planCache.classic.hits"));
    EXPECT_EQ(missesBase + 1, readServerStatusMetric("query.planCache.classic.misses"));
    EXPECT_EQ(skippedBase + 1, readServerStatusMetric("query.planCache.classic.skipped"));
    EXPECT_EQ(replannedBase + 1, readServerStatusMetric("query.planCache.classic.replanned"));
    EXPECT_EQ(replannedIsCachedBase + 1,
              readServerStatusMetric("query.planCache.classic.replanned_plan_is_cached_plan"));
    EXPECT_EQ(evictedBase + 5,
              readServerStatusMetric("query.planCache.classic.cached_plans_evicted"));
    EXPECT_EQ(inactiveReplacedBase + 1,
              readServerStatusMetric("query.planCache.classic.inactive_cached_plans_replaced"));
}

TEST(PlanCacheCountersServerStatusTest, SbeCountersAreExportedToServerStatus) {
    PlanCacheCounters counters;
    long long hitsBase = readServerStatusMetric("query.planCache.sbe.hits");
    long long missesBase = readServerStatusMetric("query.planCache.sbe.misses");
    long long skippedBase = readServerStatusMetric("query.planCache.sbe.skipped");
    long long replannedBase = readServerStatusMetric("query.planCache.sbe.replanned");
    long long replannedIsCachedBase =
        readServerStatusMetric("query.planCache.sbe.replanned_plan_is_cached_plan");
    long long evictedBase = readServerStatusMetric("query.planCache.sbe.cached_plans_evicted");
    long long inactiveReplacedBase =
        readServerStatusMetric("query.planCache.sbe.inactive_cached_plans_replaced");

    counters.incrementSbeHitsCounter();
    counters.incrementSbeHitsCounter();
    counters.incrementSbeMissesCounter();
    counters.incrementSbeSkippedCounter();
    counters.incrementSbeReplannedCounter();
    counters.incrementSbeReplannedPlanIsCachedPlanCounter();
    counters.incrementSbeCachedPlansEvictedCounter(3);
    counters.incrementSbeInactiveCachedPlansReplacedCounter();

    EXPECT_EQ(hitsBase + 2, readServerStatusMetric("query.planCache.sbe.hits"));
    EXPECT_EQ(missesBase + 1, readServerStatusMetric("query.planCache.sbe.misses"));
    EXPECT_EQ(skippedBase + 1, readServerStatusMetric("query.planCache.sbe.skipped"));
    EXPECT_EQ(replannedBase + 1, readServerStatusMetric("query.planCache.sbe.replanned"));
    EXPECT_EQ(replannedIsCachedBase + 1,
              readServerStatusMetric("query.planCache.sbe.replanned_plan_is_cached_plan"));
    EXPECT_EQ(evictedBase + 3, readServerStatusMetric("query.planCache.sbe.cached_plans_evicted"));
    EXPECT_EQ(inactiveReplacedBase + 1,
              readServerStatusMetric("query.planCache.sbe.inactive_cached_plans_replaced"));
}

TEST(QueryFrameworkCountersServerStatusTest, CountersAreExportedToServerStatus) {
    QueryFrameworkCounters counters;
    long long findSbeBase = readServerStatusMetric("query.queryFramework.find.sbe");
    long long findClassicBase = readServerStatusMetric("query.queryFramework.find.classic");
    long long aggSbeOnlyBase = readServerStatusMetric("query.queryFramework.aggregate.sbeOnly");
    long long aggClassicOnlyBase =
        readServerStatusMetric("query.queryFramework.aggregate.classicOnly");
    long long aggSbeHybridBase = readServerStatusMetric("query.queryFramework.aggregate.sbeHybrid");
    long long aggClassicHybridBase =
        readServerStatusMetric("query.queryFramework.aggregate.classicHybrid");

    counters.incrementFindSbeCounter();
    counters.incrementFindClassicCounter();
    counters.incrementFindClassicCounter();
    counters.incrementAggregateSbeOnlyCounter();
    counters.incrementAggregateClassicOnlyCounter();
    counters.incrementAggregateSbeHybridCounter();
    counters.incrementAggregateClassicHybridCounter();

    EXPECT_EQ(findSbeBase + 1, readServerStatusMetric("query.queryFramework.find.sbe"));
    EXPECT_EQ(findClassicBase + 2, readServerStatusMetric("query.queryFramework.find.classic"));
    EXPECT_EQ(aggSbeOnlyBase + 1, readServerStatusMetric("query.queryFramework.aggregate.sbeOnly"));
    EXPECT_EQ(aggClassicOnlyBase + 1,
              readServerStatusMetric("query.queryFramework.aggregate.classicOnly"));
    EXPECT_EQ(aggSbeHybridBase + 1,
              readServerStatusMetric("query.queryFramework.aggregate.sbeHybrid"));
    EXPECT_EQ(aggClassicHybridBase + 1,
              readServerStatusMetric("query.queryFramework.aggregate.classicHybrid"));
}

TEST(FastPathQueryCountersServerStatusTest, CountersAreExportedToServerStatus) {
    FastPathQueryCounters counters;
    long long idHackBase = readServerStatusMetric("query.planning.fastPath.idHack");
    long long expressBase = readServerStatusMetric("query.planning.fastPath.express");

    counters.incrementIdHackQueryCounter();
    counters.incrementIdHackQueryCounter();
    counters.incrementExpressQueryCounter();

    EXPECT_EQ(idHackBase + 2, readServerStatusMetric("query.planning.fastPath.idHack"));
    EXPECT_EQ(expressBase + 1, readServerStatusMetric("query.planning.fastPath.express"));
}

}  // namespace
}  // namespace mongo
