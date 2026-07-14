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

#include "mongo/transport/backpressure_connection_metrics.h"

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/client.h"
#include "mongo/db/service_context.h"
#include "mongo/db/service_context_test_fixture.h"
#include "mongo/stdx/thread.h"
#include "mongo/transport/session.h"
#include "mongo/transport/session_manager.h"
#include "mongo/transport/session_manager_common.h"
#include "mongo/transport/session_manager_common_mock.h"
#include "mongo/transport/transport_layer_manager_impl.h"
#include "mongo/transport/transport_layer_mock.h"
#include "mongo/unittest/barrier.h"
#include "mongo/unittest/unittest.h"

#include <limits>
#include <string>
#include <vector>

namespace mongo::transport {
namespace {

class BackpressureConnectionMetricsTest : public ServiceContextTest {
public:
    void setUp() override {
        ServiceContextTest::setUp();
        auto* svcCtx = getServiceContext();
        setupTransportLayerManager(svcCtx);
        session = transportLayer->createSession();
    }

    void setupTransportLayerManager(ServiceContext* svcCtx) {
        auto tl = std::make_unique<TransportLayerMock>(
            std::make_unique<MockSessionManagerCommon>(getServiceContext()));
        transportLayer = tl.get();

        auto tlm = std::make_unique<TransportLayerManagerImpl>(std::move(tl));
        auto tlmPtr = tlm.get();
        svcCtx->setTransportLayerManager(std::move(tlm));
        ASSERT_OK(tlmPtr->setup());
        ASSERT_OK(tlmPtr->start());
    }

    auto& metrics() {
        return transportLayer->getSessionManager()->backpressureConnectionMetrics;
    }

    BackpressureConnectionMetrics::Version normalizedVersion(const BSONElement& elem) {
        auto sessionForNormalize = transportLayer->createSession();
        BackpressureVersionMetrics::get(sessionForNormalize.get())->setVersionFromHelloField(elem);
        return BackpressureVersionMetrics::get(sessionForNormalize.get())->version();
    }

    TransportLayerMock* transportLayer{nullptr};
    std::shared_ptr<Session> session;
};

TEST_F(BackpressureConnectionMetricsTest, AbsentFieldIsNoBackpressure) {
    BSONObj empty;
    ASSERT_EQ(normalizedVersion(empty["backpressure"]), kNoBackpressureVersion);
}

TEST_F(BackpressureConnectionMetricsTest, BoolTrueIsOne) {
    BSONObj obj = BSON("backpressure" << true);
    ASSERT_EQ(normalizedVersion(obj["backpressure"]), 1);
}

TEST_F(BackpressureConnectionMetricsTest, BoolFalseIsNoBackpressure) {
    BSONObj obj = BSON("backpressure" << false);
    ASSERT_EQ(normalizedVersion(obj["backpressure"]), kNoBackpressureVersion);
}

TEST_F(BackpressureConnectionMetricsTest, PositiveIntIsItself) {
    BSONObj obj = BSON("backpressure" << 2);
    ASSERT_EQ(normalizedVersion(obj["backpressure"]), 2);
}

TEST_F(BackpressureConnectionMetricsTest, MaxExplicitVersionIsItself) {
    BSONObj obj = BSON("backpressure" << kMaxExplicitBackpressureVersion);
    ASSERT_EQ(normalizedVersion(obj["backpressure"]), kMaxExplicitBackpressureVersion);
}

TEST_F(BackpressureConnectionMetricsTest, AboveMaxExplicitBecomesOther) {
    BSONObj obj = BSON("backpressure" << (kMaxExplicitBackpressureVersion + 1));
    ASSERT_EQ(normalizedVersion(obj["backpressure"]), kOtherBackpressureVersion);
}

TEST_F(BackpressureConnectionMetricsTest, LargeIntBecomesOther) {
    BSONObj obj = BSON("backpressure" << 1'000'000);
    ASSERT_EQ(normalizedVersion(obj["backpressure"]), kOtherBackpressureVersion);
}

TEST_F(BackpressureConnectionMetricsTest, ZeroIntIsNoBackpressure) {
    BSONObj obj = BSON("backpressure" << 0);
    ASSERT_EQ(normalizedVersion(obj["backpressure"]), kNoBackpressureVersion);
}

TEST_F(BackpressureConnectionMetricsTest, NegativeIntIsOther) {
    BSONObj obj = BSON("backpressure" << -1);
    ASSERT_EQ(normalizedVersion(obj["backpressure"]), kOtherBackpressureVersion);
}

TEST_F(BackpressureConnectionMetricsTest, NonNumericTypeIsOther) {
    BSONObj obj = BSON("backpressure" << "string");
    ASSERT_EQ(normalizedVersion(obj["backpressure"]), kOtherBackpressureVersion);
}

TEST_F(BackpressureConnectionMetricsTest, DoubleNaNIsOther) {
    BSONObj obj = BSON("backpressure" << std::numeric_limits<double>::quiet_NaN());
    ASSERT_EQ(normalizedVersion(obj["backpressure"]), kOtherBackpressureVersion);
}

TEST_F(BackpressureConnectionMetricsTest, DoubleInfinityIsOther) {
    BSONObj posInf = BSON("backpressure" << std::numeric_limits<double>::infinity());
    ASSERT_EQ(normalizedVersion(posInf["backpressure"]), kOtherBackpressureVersion);
    BSONObj negInf = BSON("backpressure" << -std::numeric_limits<double>::infinity());
    ASSERT_EQ(normalizedVersion(negInf["backpressure"]), kOtherBackpressureVersion);
}

TEST_F(BackpressureConnectionMetricsTest, OutOfRangeDoubleBecomesOther) {
    BSONObj obj = BSON("backpressure" << 1e20);
    ASSERT_EQ(normalizedVersion(obj["backpressure"]), kOtherBackpressureVersion);
}

TEST(BackpressureVersionConstants, OtherSentinelIsWellAboveExplicitVersions) {
    ASSERT_GT(kOtherBackpressureVersion, kMaxExplicitBackpressureVersion);
    ASSERT_NE(kOtherBackpressureVersion, kMaxExplicitBackpressureVersion + 1);
    ASSERT_EQ(kBackpressureVersionBucketCount, kMaxExplicitBackpressureVersion + 2u);
}

TEST_F(BackpressureConnectionMetricsTest, IncrementAndDecrement) {
    metrics().increment(1);
    metrics().increment(1);
    metrics().increment(2);
    ASSERT_EQ(metrics().count(1), 2);
    ASSERT_EQ(metrics().count(2), 1);
    ASSERT_EQ(metrics().count(0), 0);

    metrics().decrement(1);
    ASSERT_EQ(metrics().count(1), 1);
    ASSERT_EQ(metrics().count(2), 1);
}

TEST_F(BackpressureConnectionMetricsTest, DecrementMatchesIncrement) {
    metrics().increment(5);
    metrics().increment(5);
    metrics().decrement(5);
    ASSERT_EQ(metrics().count(5), 1);
    metrics().decrement(5);
    ASSERT_EQ(metrics().count(5), 0);
}

TEST_F(BackpressureConnectionMetricsTest, SerializeProducesActiveAndTotalCountByVersion) {
    metrics().increment(0);
    metrics().increment(1);
    metrics().increment(1);
    metrics().increment(3);

    BSONObjBuilder bob;
    metrics().serialize(&bob);
    auto obj = bob.obj();
    ASSERT_EQ(obj.nFields(), 2);
    ASSERT_EQ(obj["activeCount"].Obj()["NoBackpressure"].numberLong(), 1);
    ASSERT_EQ(obj["activeCount"].Obj()["1"].numberLong(), 2);
    ASSERT_EQ(obj["activeCount"].Obj()["3"].numberLong(), 1);
    auto totalCreated = obj["totalCount"].Obj();
    ASSERT_EQ(totalCreated["NoBackpressure"].numberLong(), 1);
    ASSERT_EQ(totalCreated["1"].numberLong(), 2);
    ASSERT_EQ(totalCreated["3"].numberLong(), 1);
}

TEST_F(BackpressureConnectionMetricsTest, TotalCreatedIsMonotonic) {
    metrics().increment(1);
    metrics().increment(1);
    ASSERT_EQ(metrics().totalCreated(1), 2);
    metrics().decrement(1);
    ASSERT_EQ(metrics().count(1), 1);
    ASSERT_EQ(metrics().totalCreated(1), 2);
}

TEST_F(BackpressureConnectionMetricsTest, VersionsAboveMaxCollapseToOtherBucket) {
    metrics().increment(9);
    metrics().increment(kMaxExplicitBackpressureVersion + 1);
    metrics().increment(kMaxExplicitBackpressureVersion + 2);
    metrics().increment(kMaxExplicitBackpressureVersion + 2);
    metrics().decrement(kMaxExplicitBackpressureVersion + 2);

    ASSERT_EQ(metrics().count(9), 1);
    ASSERT_EQ(metrics().count(kOtherBackpressureVersion), 2);
    ASSERT_EQ(metrics().totalCreated(kOtherBackpressureVersion), 3);
}

TEST_F(BackpressureConnectionMetricsTest, HighVersionsSerializeAsOther) {
    metrics().increment(kMaxExplicitBackpressureVersion + 5);
    metrics().increment(kMaxExplicitBackpressureVersion + 99);
    ASSERT_EQ(metrics().count(kOtherBackpressureVersion), 2);
    ASSERT_EQ(metrics().count(kMaxExplicitBackpressureVersion + 5), 2);
    ASSERT_EQ(metrics().count(kMaxExplicitBackpressureVersion + 99), 2);

    BSONObjBuilder bob;
    metrics().serialize(&bob);
    auto obj = bob.obj();
    ASSERT_EQ(obj["activeCount"].Obj()["Other"].numberLong(), 2);
    ASSERT_EQ(obj["totalCount"].Obj()["Other"].numberLong(), 2);
    ASSERT_FALSE(
        obj["activeCount"].Obj().hasField(std::to_string(kMaxExplicitBackpressureVersion + 99)));
}

TEST_F(BackpressureConnectionMetricsTest, MaxExplicitVersionSerializesAsDecimalString) {
    metrics().increment(kMaxExplicitBackpressureVersion);

    BSONObjBuilder bob;
    metrics().serialize(&bob);
    auto obj = bob.obj();
    ASSERT_EQ(
        obj["activeCount"].Obj()[std::to_string(kMaxExplicitBackpressureVersion)].numberLong(), 1);
    ASSERT_FALSE(obj["activeCount"].Obj().hasField("Other"));
}

TEST_F(BackpressureConnectionMetricsTest, AdditionOperatorSumsCounts) {
    metrics().increment(1);
    metrics().increment(1);
    metrics().increment(2);

    BackpressureConnectionMetrics other;
    other.increment(1);
    other.increment(3);

    metrics() += other;
    ASSERT_EQ(metrics().count(1), 3);
    ASSERT_EQ(metrics().count(2), 1);
    ASSERT_EQ(metrics().count(3), 1);
    ASSERT_EQ(metrics().totalCreated(1), 3);
    ASSERT_EQ(metrics().totalCreated(3), 1);
}

TEST_F(BackpressureConnectionMetricsTest, SessionDecorationIncrementsOnSet) {
    ASSERT_EQ(metrics().count(1), 0);
    BackpressureVersionMetrics::get(session.get())->setVersion(1);
    ASSERT_EQ(BackpressureVersionMetrics::get(session.get())->version(), 1);
    ASSERT_EQ(metrics().count(1), 1);
}

TEST_F(BackpressureConnectionMetricsTest, SessionDecorationIgnoresRepeatedSet) {
    BackpressureVersionMetrics::get(session.get())->setVersion(1);
    BackpressureVersionMetrics::get(session.get())->setVersion(2);
    ASSERT_EQ(BackpressureVersionMetrics::get(session.get())->version(), 1);
    ASSERT_EQ(metrics().count(1), 1);
    ASSERT_EQ(metrics().count(2), 0);
}

TEST_F(BackpressureConnectionMetricsTest, SessionDecorationClampsHighVersions) {
    BackpressureVersionMetrics::get(session.get())->setVersion(kMaxExplicitBackpressureVersion + 1);
    ASSERT_EQ(BackpressureVersionMetrics::get(session.get())->version(), kOtherBackpressureVersion);
    ASSERT_EQ(metrics().count(kOtherBackpressureVersion), 1);
}

TEST_F(BackpressureConnectionMetricsTest, SessionDestructionDecrements) {
    ASSERT_EQ(metrics().count(1), 0);
    {
        auto session2 = transportLayer->createSession();
        BackpressureVersionMetrics::get(session2.get())->setVersion(1);
        ASSERT_EQ(metrics().count(1), 1);
        ASSERT_DOES_NOT_THROW(transportLayer->deleteSession(session2->id()));
    }
    ASSERT_EQ(metrics().count(1), 0);
}

TEST_F(BackpressureConnectionMetricsTest, UnsetSessionDestructionIsNoop) {
    ASSERT_EQ(BackpressureVersionMetrics::get(session.get())->version(),
              BackpressureVersionMetrics::kUnset);
    ASSERT_DOES_NOT_THROW(transportLayer->deleteSession(session->id()));
    BSONObjBuilder bob;
    metrics().serialize(&bob);
    ASSERT_EQ(bob.obj().nFields(), 0);
}

TEST_F(BackpressureConnectionMetricsTest, ConcurrentSetVersionIncrementsOnce) {
    auto* deco = BackpressureVersionMetrics::get(session.get());
    constexpr int kThreads = 8;
    unittest::Barrier start(kThreads);
    std::vector<stdx::thread> threads;
    threads.reserve(kThreads);
    for (int i = 0; i < kThreads; ++i) {
        threads.emplace_back([deco, &start, i] {
            start.countDownAndWait();
            // Distinct versions so a race would show up as multiple buckets.
            deco->setVersion(i % 3);
        });
    }
    for (auto& t : threads) {
        t.join();
    }

    const auto recorded = deco->version();
    ASSERT_NE(recorded, BackpressureVersionMetrics::kUnset);
    ASSERT_EQ(metrics().count(recorded), 1);
    ASSERT_EQ(metrics().totalCreated(recorded), 1);
    // No other version buckets should have been incremented.
    for (int v = 0; v <= kMaxExplicitBackpressureVersion; ++v) {
        if (v == recorded) {
            continue;
        }
        ASSERT_EQ(metrics().count(v), 0);
        ASSERT_EQ(metrics().totalCreated(v), 0);
    }
}

}  // namespace
}  // namespace mongo::transport
