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

#include "mongo/crypto/fle_stats.h"

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/json.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/service_context.h"
#include "mongo/db/service_context_test_fixture.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/idl/server_parameter_test_controller.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/duration.h"
#include "mongo/util/testing_options_gen.h"
#include "mongo/util/tick_source_mock.h"

#include <memory>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest

namespace mongo {
namespace {

class FLEStatsTest : public ServiceContextTest {
public:
    FLEStatsTest() {
        opCtxPtr = makeOperationContext();
        opCtx = opCtxPtr.get();
    }

    void setUp() final {
        ServiceContextTest::setUp();
        oldDiagnosticsFlag = gTestingDiagnosticsEnabledAtStartup;
        tickSource = std::make_unique<TickSourceMock<Milliseconds>>();
        instance =
            std::make_unique<FLEStatusSection>("test section", ClusterRole::None, tickSource.get());
    }

    void tearDown() final {
        gTestingDiagnosticsEnabledAtStartup = oldDiagnosticsFlag;
        ServiceContextTest::tearDown();
    }

    ServiceContext::UniqueOperationContext opCtxPtr;
    OperationContext* opCtx;

    std::unique_ptr<TickSourceMock<Milliseconds>> tickSource;
    std::unique_ptr<FLEStatusSection> instance;

    bool oldDiagnosticsFlag;

    static EncryptedFieldConfig buildTestEncryptedFieldConfig(int64_t contention) {
        EncryptedFieldConfig efc;
        std::vector<EncryptedField> fields;
        const auto keyId = UUID::gen();
        fields.emplace_back(keyId, "path");
        auto qtc = QueryTypeConfig::parse(fromjson(R"({"queryType": "equality"})"));
        qtc.setContention(contention);
        fields.back().setQueries(
            std::variant<std::vector<QueryTypeConfig>, QueryTypeConfig>(std::move(qtc)));
        efc.setFields(std::move(fields));
        return efc;
    }
};

TEST_F(FLEStatsTest, NoopStats) {
    ASSERT_TRUE(instance->includeByDefault());

    auto obj = instance->generateSection(opCtx, BSONElement());
    ASSERT_FALSE(obj.hasField("compactStats"));
    ASSERT_FALSE(obj.hasField("cleanupStats"));
    ASSERT_FALSE(obj.hasField("emuBinaryStats"));
}

TEST_F(FLEStatsTest, BinaryEmuStatsAreEmptyWithoutTesting) {
    {
        auto tracker = instance->makeEmuBinaryTracker();
        tracker.recordSuboperation();
    }

    ASSERT_TRUE(instance->includeByDefault());

    auto obj = instance->generateSection(opCtx, BSONElement());
    ASSERT_FALSE(obj.hasField("compactStats"));
    ASSERT_FALSE(obj.hasField("cleanupStats"));
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

    auto obj = instance->generateSection(opCtx, BSONElement());
    ASSERT_FALSE(obj.hasField("compactStats"));
    ASSERT_FALSE(obj.hasField("cleanupStats"));
    ASSERT_TRUE(obj.hasField("emuBinaryStats"));
    ASSERT_EQ(1, obj["emuBinaryStats"]["calls"].Long());
    ASSERT_EQ(1, obj["emuBinaryStats"]["suboperations"].Long());
    ASSERT_EQ(100, obj["emuBinaryStats"]["totalMillis"].Long());
}

TEST_F(FLEStatsTest, IndexTypeStats) {
    struct IndexTypeCounters {
        int64_t equality = 0;
        int64_t unindexed = 0;
        int64_t range = 0;
        int64_t suffix = 0;
        int64_t prefix = 0;
        int64_t rangePreview = 0;
        int64_t substringPreview = 0;
        int64_t suffixPreview = 0;
        int64_t prefixPreview = 0;
    };

    auto assertCounters = [this](const IndexTypeCounters& expected) {
        auto obj = instance->generateSection(opCtx, BSONElement());
        auto actual = FLEIndexTypeStats::parse(obj["indexTypeStats"].Obj());
        ASSERT_EQ(actual.getEquality(), expected.equality);
        ASSERT_EQ(actual.getUnindexed(), expected.unindexed);
        ASSERT_EQ(actual.getRange(), expected.range);
        ASSERT_EQ(actual.getSuffix(), expected.suffix);
        ASSERT_EQ(actual.getPrefix(), expected.prefix);
        ASSERT_EQ(actual.getRangePreview(), expected.rangePreview);
        ASSERT_EQ(actual.getSubstringPreview(), expected.substringPreview);
        ASSERT_EQ(actual.getSuffixPreview(), expected.suffixPreview);
        ASSERT_EQ(actual.getPrefixPreview(), expected.prefixPreview);
    };

    const auto buildConfig = [](const StringMap<int64_t>& spec) {
        EncryptedFieldConfig efc;
        std::vector<EncryptedField> fields;
        const auto keyId = UUID::gen();

        for (auto& [indexType, count] : spec) {
            if (indexType == "unindexed") {
                for (auto i = count; i > 0; i--) {
                    fields.emplace_back(keyId, indexType);
                }
            } else if (indexType == "multi" || indexType == "multiPreview") {
                auto suffixType = (indexType == "multi") ? R"({"queryType": "suffix"})"
                                                         : R"({"queryType": "suffixPreview"})";
                auto prefixType = (indexType == "multi") ? R"({"queryType": "prefix"})"
                                                         : R"({"queryType": "prefixPreview"})";

                for (auto i = count; i > 0; i--) {
                    fields.emplace_back(keyId, indexType);

                    std::vector<QueryTypeConfig> queries;
                    queries.push_back(QueryTypeConfig::parse(fromjson(suffixType)));
                    queries.push_back(QueryTypeConfig::parse(fromjson(prefixType)));

                    fields.back().setQueries(
                        std::variant<std::vector<QueryTypeConfig>, QueryTypeConfig>(
                            std::move(queries)));
                }
            } else {
                for (auto i = count; i > 0; i--) {
                    fields.emplace_back(keyId, indexType);
                    auto qtc =
                        QueryTypeConfig::parse(fromjson("{\"queryType\": \"" + indexType + "\"}"));
                    fields.back().setQueries(
                        std::variant<std::vector<QueryTypeConfig>, QueryTypeConfig>(
                            std::move(qtc)));
                }
            }
        }
        efc.setFields(std::move(fields));
        return efc;
    };

    ASSERT_TRUE(instance->includeByDefault());

    IndexTypeCounters expected;
    assertCounters(expected);

    // Use a single NSS; index type stats are EFC-driven and NSS is irrelevant here.
    const auto nss = NamespaceString::createNamespaceString_forTest("test.coll");

    auto efc1 = buildConfig({{"unindexed", 4}, {"equality", 2}, {"multiPreview", 3}});
    instance->updateStatsOnRegisterCollection(nss, efc1);
    expected.unindexed++;
    expected.equality++;
    expected.suffixPreview++;
    expected.prefixPreview++;
    assertCounters(expected);

    auto efc2 = buildConfig({{"range", 3}, {"rangePreview", 1}, {"substringPreview", 2}});
    instance->updateStatsOnRegisterCollection(nss, efc2);
    expected.range++;
    expected.rangePreview++;
    expected.substringPreview++;
    assertCounters(expected);

    auto efc3 = buildConfig({{"suffixPreview", 1}, {"multi", 1}});
    instance->updateStatsOnRegisterCollection(nss, efc3);
    expected.suffixPreview++;
    expected.suffix++;
    expected.prefix++;
    assertCounters(expected);

    instance->updateStatsOnDeregisterCollection(nss, efc2);
    expected.range--;
    expected.rangePreview--;
    expected.substringPreview--;
    assertCounters(expected);

    instance->updateStatsOnRegisterCollection(nss, efc3);
    expected.suffixPreview++;
    expected.suffix++;
    expected.prefix++;
    assertCounters(expected);

    instance->updateStatsOnDeregisterCollection(nss, efc3);
    expected.suffixPreview--;
    expected.suffix--;
    expected.prefix--;
    assertCounters(expected);

    instance->updateStatsOnDeregisterCollection(nss, efc1);
    expected.unindexed--;
    expected.equality--;
    expected.suffixPreview--;
    expected.prefixPreview--;
    assertCounters(expected);

    instance->updateStatsOnDeregisterCollection(nss, efc3);
    expected.suffixPreview--;
    expected.suffix--;
    expected.prefix--;
    assertCounters(expected);
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
    auto obj = instance->generateSection(opCtx, BSONElement());
    ASSERT_TRUE(obj.hasField("operationCounters"));
    auto counters = obj["operationCounters"].Obj();
    ASSERT_EQ(0, counters["inserts"].Long());
    ASSERT_EQ(0, counters["finds"].Long());
    ASSERT_EQ(0, counters["updates"].Long());
    ASSERT_EQ(0, counters["deletes"].Long());
}

TEST_F(FLEStatsTest, IncrementInsertCountAlwaysIncrements) {
    const auto nss = NamespaceString::createNamespaceString_forTest("test.coll");
    const auto efc = buildTestEncryptedFieldConfig(1);  // contention doesn't matter

    instance->incrementInsertCount(nss, efc);
    instance->incrementInsertCount(nss, efc);
    instance->incrementInsertCount(nss, efc);

    auto obj = instance->generateSection(opCtx, BSONElement());
    ASSERT_EQ(3, obj["operationCounters"].Obj()["inserts"].Long());
}

TEST_F(FLEStatsTest, IncrementFindUpdateDeleteCount) {
    const auto nss = NamespaceString::createNamespaceString_forTest("test.coll");
    const auto efc = buildTestEncryptedFieldConfig(1);

    instance->incrementFindCount(nss, efc);
    instance->incrementFindCount(nss, efc);
    instance->incrementUpdateCount(nss, efc);
    instance->incrementDeleteCount(nss, efc);
    instance->incrementDeleteCount(nss, efc);

    auto obj = instance->generateSection(opCtx, BSONElement());
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
            auto obj = instance->generateSection(opCtx, BSONElement());
            auto counters = obj["operationCounters"].Obj();
            ASSERT_EQ(counters["finds"].Long(), expectedFinds);
            ASSERT_EQ(counters["updates"].Long(), expectedUpdates);
            ASSERT_EQ(counters["deletes"].Long(), expectedDeletes);
        };

    // First increments always succeed.
    instance->incrementFindCount(nss, efc);
    instance->incrementUpdateCount(nss, efc);
    instance->incrementDeleteCount(nss, efc);
    validateCounters(1, 1, 1);

    // Increments immediately after don't register.
    instance->incrementFindCount(nss, efc);
    instance->incrementUpdateCount(nss, efc);
    instance->incrementDeleteCount(nss, efc);
    validateCounters(1, 1, 1);

    // Increments just before the period ends don't register
    tickSource->advance(delay - Milliseconds(1));
    instance->incrementFindCount(nss, efc);
    instance->incrementUpdateCount(nss, efc);
    instance->incrementDeleteCount(nss, efc);
    validateCounters(1, 1, 1);

    // Increments at the period end register
    tickSource->advance(Milliseconds(1));
    instance->incrementFindCount(nss, efc);
    instance->incrementUpdateCount(nss, efc);
    instance->incrementDeleteCount(nss, efc);
    validateCounters(2, 2, 2);
}

TEST_F(FLEStatsTest, OperationCountersPersistAfterDeregister) {
    const auto nss = NamespaceString::createNamespaceString_forTest("test.coll");
    const auto efc = buildTestEncryptedFieldConfig(1);

    instance->incrementInsertCount(nss, efc);
    instance->incrementInsertCount(nss, efc);
    instance->incrementFindCount(nss, efc);
    instance->incrementUpdateCount(nss, efc);
    instance->incrementDeleteCount(nss, efc);

    // Deregister moves per-collection stats into _deletedCollStats.
    instance->updateStatsOnDeregisterCollection(nss, efc);

    auto obj = instance->generateSection(opCtx, BSONElement());
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

    instance->incrementInsertCount(nss1, efc);
    instance->incrementInsertCount(nss1, efc);
    instance->incrementUpdateCount(nss1, efc);

    instance->incrementInsertCount(nss2, efc);
    instance->incrementDeleteCount(nss2, efc);
    instance->incrementFindCount(nss2, efc);

    instance->incrementInsertCount(nss3, efc);
    instance->incrementUpdateCount(nss3, efc);
    instance->incrementFindCount(nss3, efc);
    // remove nss3 and merge its stats into _deletedCollStats
    instance->updateStatsOnDeregisterCollection(nss3, efc);

    auto obj = instance->generateSection(opCtx, BSONElement());
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

}  // namespace
}  // namespace mongo
