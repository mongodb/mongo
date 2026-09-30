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

#include "mongo/platform/basic.h"

#include "mongo/crypto/fle_stats.h"

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/json.h"
#include "mongo/bson/unordered_fields_bsonobj_comparator.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context_noop.h"
#include "mongo/idl/server_parameter_test_util.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/testing_options_gen.h"
#include "mongo/util/tick_source_mock.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest

namespace mongo {

class FLEStatsTest : public unittest::Test {
public:
    void setUp() final {
        oldDiagnosticsFlag = gTestingDiagnosticsEnabledAtStartup;
        tickSource = std::make_unique<TickSourceMock<Milliseconds>>();
        instance = std::make_unique<FLEStatusSection>(tickSource.get());
    }

    void tearDown() final {
        gTestingDiagnosticsEnabledAtStartup = oldDiagnosticsFlag;
    }

protected:
    CompactStats zeroStats = CompactStats::parse(
        IDLParserContext("compactStats"),
        BSON("ecoc" << BSON("deleted" << 0 << "read" << 0) << "esc"
                    << BSON("deleted" << 0 << "inserted" << 0 << "read" << 0 << "updated" << 0)));

    CompactStats compactStats = CompactStats::parse(
        IDLParserContext("compactStats"),
        BSON("ecoc" << BSON("deleted" << 1 << "read" << 1) << "esc"
                    << BSON("deleted" << 1 << "inserted" << 1 << "read" << 1 << "updated" << 1)));
    std::unique_ptr<TickSourceMock<Milliseconds>> tickSource;
    std::unique_ptr<FLEStatusSection> instance;
    OperationContextNoop opCtx;
    bool oldDiagnosticsFlag;

    static EncryptedFieldConfig buildTestEncryptedFieldConfig(int64_t contention) {
        EncryptedFieldConfig efc;
        std::vector<EncryptedField> fields;
        const auto keyId = UUID::gen();
        fields.emplace_back(keyId, "path");
        auto qtc = QueryTypeConfig::parse(IDLParserContext("queryTypeConfig"),
                                          fromjson(R"({"queryType": "equality"})"));
        qtc.setContention(contention);
        auto queries = fields.back().getQueries();
        queries = std::move(qtc);
        fields.back().setQueries(queries);
        efc.setFields(std::move(fields));
        return efc;
    }
};

TEST_F(FLEStatsTest, NoopStats) {
    ASSERT_FALSE(instance->includeByDefault());

    auto obj = instance->generateSection(&opCtx, BSONElement());
    ASSERT_TRUE(obj.hasField("compactStats"));
    ASSERT_BSONOBJ_EQ(zeroStats.toBSON(), obj["compactStats"].Obj());
    ASSERT_FALSE(obj.hasField("emuBinaryStats"));
}

TEST_F(FLEStatsTest, CompactStats) {
    instance->updateCompactionStats(compactStats);

    ASSERT_TRUE(instance->includeByDefault());

    auto obj = instance->generateSection(&opCtx, BSONElement());
    ASSERT_TRUE(obj.hasField("compactStats"));
    ASSERT_BSONOBJ_NE(zeroStats.toBSON(), obj["compactStats"].Obj());
    ASSERT_BSONOBJ_EQ(compactStats.toBSON(), obj["compactStats"].Obj());
    ASSERT_FALSE(obj.hasField("emuBinaryStats"));
}

TEST_F(FLEStatsTest, BinaryEmuStatsAreEmptyWithoutTesting) {
    {
        auto tracker = instance->makeEmuBinaryTracker();
        tracker.recordSuboperation();
    }

    ASSERT_FALSE(instance->includeByDefault());

    auto obj = instance->generateSection(&opCtx, BSONElement());
    ASSERT_TRUE(obj.hasField("compactStats"));
    ASSERT_BSONOBJ_EQ(zeroStats.toBSON(), obj["compactStats"].Obj());
    ASSERT_FALSE(obj.hasField("emuBinaryStats"));
}

TEST_F(FLEStatsTest, BinaryEmuStatsArePopulatedWithTesting) {
    RAIIServerParameterControllerForTest controller1(
        "unsupportedDangerousTestingFLEDiagnosticsEnabled", true);
    RAIIServerParameterControllerForTest controller2("testingDiagnosticsEnabled", true);

    {
        auto tracker = instance->makeEmuBinaryTracker();
        tracker.recordSuboperation();
        tickSource->advance(Milliseconds(100));
    }

    ASSERT_TRUE(instance->includeByDefault());

    auto obj = instance->generateSection(&opCtx, BSONElement());
    ASSERT_TRUE(obj.hasField("compactStats"));
    ASSERT_BSONOBJ_EQ(zeroStats.toBSON(), obj["compactStats"].Obj());
    ASSERT_TRUE(obj.hasField("emuBinaryStats"));
    ASSERT_EQ(1, obj["emuBinaryStats"]["calls"].Long());
    ASSERT_EQ(1, obj["emuBinaryStats"]["suboperations"].Long());
    ASSERT_EQ(100, obj["emuBinaryStats"]["totalMillis"].Long());
}

// --- RateLimitedCounter tests ---

TEST_F(FLEStatsTest, RateLimitedCounterZeroCooldown) {
    RateLimitedCounter counter(Milliseconds{0}, tickSource.get());
    ASSERT_TRUE(counter.increment());
    ASSERT_EQ(1, counter.getCount());
    ASSERT_TRUE(counter.increment());
    ASSERT_EQ(2, counter.getCount());
}

TEST_F(FLEStatsTest, RateLimitedCounterNonZeroCooldown) {
    RateLimitedCounter counter(Milliseconds{100}, tickSource.get());
    // First increment always succeeds
    ASSERT_TRUE(counter.increment());
    ASSERT_EQ(1, counter.getCount());

    // Cooldown blocks subsequent increments.
    ASSERT_FALSE(counter.increment());
    ASSERT_EQ(1, counter.getCount());

    // Increment succeeds after cooldown.
    tickSource->advance(Milliseconds{100});
    ASSERT_TRUE(counter.increment());
    ASSERT_EQ(2, counter.getCount());

    // Blocked again immediately after.
    ASSERT_FALSE(counter.increment());
    ASSERT_EQ(2, counter.getCount());
}

TEST_F(FLEStatsTest, RateLimitedCounterForceIncrement) {
    RateLimitedCounter counter(Milliseconds{10000}, tickSource.get());
    // First increment always succeeds
    ASSERT_TRUE(counter.increment());
    ASSERT_EQ(1, counter.getCount());

    // Next increment is blocked by cooldown.
    ASSERT_FALSE(counter.increment());
    ASSERT_EQ(1, counter.getCount());

    // forceIncrement bypasses the cooldown entirely.
    counter.forceIncrement(5);
    ASSERT_EQ(6, counter.getCount());
    counter.forceIncrement();
    ASSERT_EQ(7, counter.getCount());
}

// --- FLEPerCollectionStats tests ---

TEST_F(FLEStatsTest, FLEPerCollectionStatsAccumulate) {
    FLEPerCollectionStats a(Milliseconds{0}, tickSource.get());
    FLEPerCollectionStats b(Milliseconds{0}, tickSource.get());

    a.insertOpCounter.fetchAndAddRelaxed(1);
    a.findOpCounter.forceIncrement(2);
    a.updateOpCounter.forceIncrement(3);
    a.deleteOpCounter.forceIncrement(4);

    b.insertOpCounter.fetchAndAddRelaxed(5);
    b.findOpCounter.forceIncrement(6);
    b.updateOpCounter.forceIncrement(7);
    b.deleteOpCounter.forceIncrement(8);

    a += b;

    ASSERT_EQ(6, a.insertOpCounter.loadRelaxed());
    ASSERT_EQ(8, a.findOpCounter.getCount());
    ASSERT_EQ(10, a.updateOpCounter.getCount());
    ASSERT_EQ(12, a.deleteOpCounter.getCount());
}

TEST_F(FLEStatsTest, FLEPerCollectionStatsSerialize) {
    FLEPerCollectionStats stats(Milliseconds{0}, tickSource.get());
    stats.insertOpCounter.fetchAndAddRelaxed(10);
    stats.findOpCounter.forceIncrement(20);
    stats.updateOpCounter.forceIncrement(30);
    stats.deleteOpCounter.forceIncrement(40);

    BSONObjBuilder builder;
    stats.serialize(&builder);
    auto obj = builder.obj();

    ASSERT_EQ(10, obj["inserts"].Long());
    ASSERT_EQ(20, obj["finds"].Long());
    ASSERT_EQ(30, obj["updates"].Long());
    ASSERT_EQ(40, obj["deletes"].Long());
}

// --- FLEStatusSection operation counter tests ---

TEST_F(FLEStatsTest, OperationCountersSectionIsPresent) {
    auto obj = instance->generateSection(&opCtx, BSONElement());
    ASSERT_TRUE(obj.hasField("operationCounters"));
    auto counters = obj["operationCounters"].Obj();
    ASSERT_EQ(0, counters["inserts"].Long());
    ASSERT_EQ(0, counters["finds"].Long());
    ASSERT_EQ(0, counters["updates"].Long());
    ASSERT_EQ(0, counters["deletes"].Long());
}

TEST_F(FLEStatsTest, IncrementOpCounterOnUnregisteredNamespaceIsNoop) {
    const auto nss = NamespaceString::createNamespaceString_forTest("test.coll");

    // incrementing counters on an unregistered namespace is a no-op
    instance->incrementFindCount(nss);
    instance->incrementInsertCount(nss);
    instance->incrementUpdateCount(nss);
    instance->incrementDeleteCount(nss);

    auto obj = instance->generateSection(&opCtx, BSONElement());
    auto counters = obj["operationCounters"].Obj();
    ASSERT_EQ(0, counters["inserts"].Long());
    ASSERT_EQ(0, counters["finds"].Long());
    ASSERT_EQ(0, counters["updates"].Long());
    ASSERT_EQ(0, counters["deletes"].Long());
}

TEST_F(FLEStatsTest, IncrementInsertCountAlwaysIncrements) {
    const auto nss = NamespaceString::createNamespaceString_forTest("test.coll");
    const auto efc = buildTestEncryptedFieldConfig(1);  // contention doesn't matter

    instance->updateStatsOnRegisterCollection(nss, efc);
    instance->incrementInsertCount(nss);
    instance->incrementInsertCount(nss);
    instance->incrementInsertCount(nss);

    ASSERT_TRUE(instance->includeByDefault());

    auto obj = instance->generateSection(&opCtx, BSONElement());
    ASSERT_EQ(3, obj["operationCounters"].Obj()["inserts"].Long());
}

TEST_F(FLEStatsTest, IncrementFindUpdateDeleteCount) {
    const auto nss = NamespaceString::createNamespaceString_forTest("test.coll");
    const auto efc = buildTestEncryptedFieldConfig(1);

    instance->updateStatsOnRegisterCollection(nss, efc);
    instance->incrementFindCount(nss);
    instance->incrementFindCount(nss);
    instance->incrementUpdateCount(nss);
    instance->incrementDeleteCount(nss);
    instance->incrementDeleteCount(nss);

    auto obj = instance->generateSection(&opCtx, BSONElement());
    auto counters = obj["operationCounters"].Obj();
    ASSERT_EQ(1, counters["finds"].Long());
    ASSERT_EQ(1, counters["updates"].Long());
    ASSERT_EQ(1, counters["deletes"].Long());
}

TEST_F(FLEStatsTest, IncrementFindUpdateDeleteCountRateLimited) {
    const auto nss = NamespaceString::createNamespaceString_forTest("test.coll");
    const auto efc = buildTestEncryptedFieldConfig(1);
    const auto delay = FLEStatusSection::calculateOpCounterIncrementCooldownValue(efc);

    auto validateCounters =
        [this](long long expectedFinds, long long expectedUpdates, long long expectedDeletes) {
            auto obj = instance->generateSection(&opCtx, BSONElement());
            auto counters = obj["operationCounters"].Obj();
            ASSERT_EQ(counters["finds"].Long(), expectedFinds);
            ASSERT_EQ(counters["updates"].Long(), expectedUpdates);
            ASSERT_EQ(counters["deletes"].Long(), expectedDeletes);
        };

    instance->updateStatsOnRegisterCollection(nss, efc);

    // First increments always succeed.
    instance->incrementFindCount(nss);
    instance->incrementUpdateCount(nss);
    instance->incrementDeleteCount(nss);
    validateCounters(1, 1, 1);

    // Increments immediately after don't register.
    instance->incrementFindCount(nss);
    instance->incrementUpdateCount(nss);
    instance->incrementDeleteCount(nss);
    validateCounters(1, 1, 1);

    // Increments just before the period ends don't register
    tickSource->advance(delay - Milliseconds(1));
    instance->incrementFindCount(nss);
    instance->incrementUpdateCount(nss);
    instance->incrementDeleteCount(nss);
    validateCounters(1, 1, 1);

    // Increments at the period end register
    tickSource->advance(Milliseconds(1));
    instance->incrementFindCount(nss);
    instance->incrementUpdateCount(nss);
    instance->incrementDeleteCount(nss);
    validateCounters(2, 2, 2);
}

TEST_F(FLEStatsTest, OperationCountersPersistAfterDeregister) {
    const auto nss = NamespaceString::createNamespaceString_forTest("test.coll");
    const auto efc = buildTestEncryptedFieldConfig(1);

    instance->updateStatsOnRegisterCollection(nss, efc);

    instance->incrementInsertCount(nss);
    instance->incrementInsertCount(nss);
    instance->incrementFindCount(nss);
    instance->incrementUpdateCount(nss);
    instance->incrementDeleteCount(nss);

    // Deregister moves per-collection stats into _deletedCollStats.
    instance->updateStatsOnDeregisterCollection(nss);

    auto obj = instance->generateSection(&opCtx, BSONElement());
    auto counters = obj["operationCounters"].Obj();
    ASSERT_EQ(2, counters["inserts"].Long());
    ASSERT_EQ(1, counters["finds"].Long());
    ASSERT_EQ(1, counters["updates"].Long());
    ASSERT_EQ(1, counters["deletes"].Long());
}

TEST_F(FLEStatsTest, OperationCountersAggregateMultipleCollections) {
    const auto nss1 = NamespaceString::createNamespaceString_forTest("test.coll1");
    const auto nss2 = NamespaceString::createNamespaceString_forTest("test.coll2");
    const auto nss3 = NamespaceString::createNamespaceString_forTest("test.coll3");
    const auto efc = buildTestEncryptedFieldConfig(1);

    instance->updateStatsOnRegisterCollection(nss1, efc);
    instance->updateStatsOnRegisterCollection(nss2, efc);
    instance->updateStatsOnRegisterCollection(nss3, efc);

    instance->incrementInsertCount(nss1);
    instance->incrementInsertCount(nss1);
    instance->incrementUpdateCount(nss1);

    instance->incrementInsertCount(nss2);
    instance->incrementDeleteCount(nss2);
    instance->incrementFindCount(nss2);

    instance->incrementInsertCount(nss3);
    instance->incrementUpdateCount(nss3);
    instance->incrementFindCount(nss3);
    // remove nss3 and merge its stats into _deletedCollStats
    instance->updateStatsOnDeregisterCollection(nss3);

    auto obj = instance->generateSection(&opCtx, BSONElement());
    auto counters = obj["operationCounters"].Obj();
    ASSERT_EQ(4, counters["inserts"].Long());
    ASSERT_EQ(2, counters["finds"].Long());
    ASSERT_EQ(2, counters["updates"].Long());
    ASSERT_EQ(1, counters["deletes"].Long());
}

TEST_F(FLEStatsTest, CalculateOpCounterIncrementCooldownValueTest) {
    // expect the durations returned by calculateOpCounterIncrementCooldownValue()
    // is in the range [106969, 111218] milliseconds for contention factor values 0 through 100
    auto min = FLEStatusSection::calculateOpCounterIncrementCooldownValue(
        buildTestEncryptedFieldConfig(0));
    auto max = FLEStatusSection::calculateOpCounterIncrementCooldownValue(
        buildTestEncryptedFieldConfig(100));
    ASSERT_EQ(Milliseconds(106969), min);
    ASSERT_EQ(Milliseconds(111218), max);

    auto last = min;
    for (int64_t cf = 1; cf < 100; cf++) {
        auto current = FLEStatusSection::calculateOpCounterIncrementCooldownValue(
            buildTestEncryptedFieldConfig(cf));
        ASSERT_GT(current, last);
        last = current;
    }
}

}  // namespace mongo
