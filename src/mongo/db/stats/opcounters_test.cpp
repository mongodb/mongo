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

#include "mongo/db/stats/opcounters.h"

#include "mongo/otel/metrics/metric_names.h"
#include "mongo/otel/metrics/metrics_test_util.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

// ---------------------------------------------------------------------------
// OTel export tests
// These verify that gotFoo() calls are reflected in the OTel instrumentation.
// OtelMetricsCapturer must be constructed before OpCounters so MetricsService
// is initialized with the in-memory exporter before the counters are registered.
// ---------------------------------------------------------------------------

#if MONGO_CONFIG_OTEL
TEST(OpCountersOtelTest, MainCountersAreExported) {
    otel::metrics::OtelMetricsCapturer capturer;
    if (!otel::metrics::OtelMetricsCapturer::canReadMetrics()) {
        return;
    }

    OpCounters counters(otel::metrics::MetricNames::kInsertOpCount,
                        otel::metrics::MetricNames::kQueryOpCount,
                        otel::metrics::MetricNames::kUpdateOpCount,
                        otel::metrics::MetricNames::kDeleteOpCount,
                        otel::metrics::MetricNames::kGetMoreOpCount,
                        otel::metrics::MetricNames::kCommandOpCount,
                        otel::metrics::MetricNames::kAggregateOpCount);

    counters.gotInserts(3);
    counters.gotInsert();
    EXPECT_EQ(4, capturer.readInt64Counter(otel::metrics::MetricNames::kInsertOpCount));

    counters.gotQuery();
    EXPECT_EQ(1, capturer.readInt64Counter(otel::metrics::MetricNames::kQueryOpCount));

    counters.gotUpdate();
    EXPECT_EQ(1, capturer.readInt64Counter(otel::metrics::MetricNames::kUpdateOpCount));

    counters.gotDelete();
    EXPECT_EQ(1, capturer.readInt64Counter(otel::metrics::MetricNames::kDeleteOpCount));

    counters.gotGetMore();
    EXPECT_EQ(1, capturer.readInt64Counter(otel::metrics::MetricNames::kGetMoreOpCount));

    counters.gotCommand();
    EXPECT_EQ(1, capturer.readInt64Counter(otel::metrics::MetricNames::kCommandOpCount));

    counters.gotAggregate();
    EXPECT_EQ(1, capturer.readInt64Counter(otel::metrics::MetricNames::kAggregateOpCount));
}
#endif  // MONGO_CONFIG_OTEL

// ---------------------------------------------------------------------------
// serverStatus (getObj) tests
// These use the default constructor so they work on all platforms without MetricsService.
// ---------------------------------------------------------------------------

TEST(OpCountersGetObjTest, ReflectsCounterValues) {
    OpCounters counters;

    counters.gotInserts(2);
    counters.gotInsert();
    counters.gotQuery();
    counters.gotUpdate();
    counters.gotDelete();
    counters.gotGetMore();
    counters.gotCommand();
    counters.gotAggregate();

    BSONObj obj = counters.getObj();
    EXPECT_EQ(3, obj["insert"].numberLong());
    EXPECT_EQ(1, obj["query"].numberLong());
    EXPECT_EQ(1, obj["update"].numberLong());
    EXPECT_EQ(1, obj["delete"].numberLong());
    EXPECT_EQ(1, obj["getmore"].numberLong());
    EXPECT_EQ(1, obj["command"].numberLong());
    EXPECT_EQ(1, obj["aggregate"].numberLong());
}

TEST(OpCountersGetObjTest, OmitsDeprecatedAndConstraintsRelaxedWhenZero) {
    OpCounters counters;
    BSONObj obj = counters.getObj();
    EXPECT_FALSE(obj.hasField("deprecated"));
    EXPECT_FALSE(obj.hasField("constraintsRelaxed"));
}

TEST(OpCountersGetObjTest, IncludesDeprecatedSectionWhenNonzero) {
    OpCounters counters;
    counters.gotQueryDeprecated();
    BSONObj obj = counters.getObj();
    ASSERT_TRUE(obj.hasField("deprecated"));
    EXPECT_EQ(1, obj["deprecated"].Obj()["query"].numberLong());
}

TEST(OpCountersGetObjTest, IncludesConstraintsRelaxedSectionWhenNonzero) {
    OpCounters counters;
    counters.gotInsertOnExistingDoc();
    counters.gotUpdateOnMissingDoc();
    counters.gotDeleteWasEmpty();
    counters.gotDeleteFromMissingNamespace();
    counters.gotAcceptableErrorInCommand();
    counters.gotRecordIdsReplicatedDocIdMismatch();
    BSONObj obj = counters.getObj();
    ASSERT_TRUE(obj.hasField("constraintsRelaxed"));
    BSONObj relaxed = obj["constraintsRelaxed"].Obj();
    EXPECT_EQ(1, relaxed["insertOnExistingDoc"].numberLong());
    EXPECT_EQ(1, relaxed["updateOnMissingDoc"].numberLong());
    EXPECT_EQ(1, relaxed["deleteWasEmpty"].numberLong());
    EXPECT_EQ(1, relaxed["deleteFromMissingNamespace"].numberLong());
    EXPECT_EQ(1, relaxed["acceptableErrorInCommand"].numberLong());
    EXPECT_EQ(1, relaxed["recordIdsReplicatedDocIdMismatch"].numberLong());
}

}  // namespace
}  // namespace mongo
