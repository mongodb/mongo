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

    CompactStats zeroStats = CompactStats::parse(
        BSON("ecoc" << BSON("deleted" << 0 << "read" << 0) << "esc"
                    << BSON("deleted" << 0 << "inserted" << 0 << "read" << 0 << "updated" << 0)),
        IDLParserContext("compactStats"));

    CompactStats compactStats = CompactStats::parse(
        BSON("ecoc" << BSON("deleted" << 1 << "read" << 1) << "esc"
                    << BSON("deleted" << 1 << "inserted" << 1 << "read" << 1 << "updated" << 1)),
        IDLParserContext("compactStats"));

    CleanupStats cleanupStats = CleanupStats::parse(
        BSON("ecoc" << BSON("deleted" << 1 << "read" << 1) << "esc"
                    << BSON("deleted" << 1 << "inserted" << 1 << "read" << 1 << "updated" << 1)),
        IDLParserContext("cleanupStats"));

    std::unique_ptr<TickSourceMock<Milliseconds>> tickSource;
    std::unique_ptr<FLEStatusSection> instance;

    bool oldDiagnosticsFlag;
};

TEST_F(FLEStatsTest, NoopStats) {
    ASSERT_TRUE(instance->includeByDefault());

    auto obj = instance->generateSection(opCtx, BSONElement());
    ASSERT_TRUE(obj.hasField("compactStats"));
    ASSERT_BSONOBJ_EQ(zeroStats.toBSON(), obj["compactStats"].Obj());
    ASSERT_TRUE(obj.hasField("cleanupStats"));
    ASSERT_BSONOBJ_EQ(zeroStats.toBSON(), obj["cleanupStats"].Obj());
    ASSERT_FALSE(obj.hasField("emuBinaryStats"));
}

TEST_F(FLEStatsTest, CompactStats) {
    instance->updateCompactionStats(compactStats);

    ASSERT_TRUE(instance->includeByDefault());

    auto obj = instance->generateSection(opCtx, BSONElement());
    ASSERT_TRUE(obj.hasField("compactStats"));
    ASSERT_BSONOBJ_NE(zeroStats.toBSON(), obj["compactStats"].Obj());
    ASSERT_BSONOBJ_EQ(compactStats.toBSON(), obj["compactStats"].Obj());
    ASSERT_TRUE(obj.hasField("cleanupStats"));
    ASSERT_BSONOBJ_EQ(zeroStats.toBSON(), obj["cleanupStats"].Obj());
    ASSERT_FALSE(obj.hasField("emuBinaryStats"));
}

TEST_F(FLEStatsTest, CleanupStats) {
    instance->updateCleanupStats(cleanupStats);

    ASSERT_TRUE(instance->includeByDefault());

    auto obj = instance->generateSection(opCtx, BSONElement());
    ASSERT_TRUE(obj.hasField("compactStats"));
    ASSERT_BSONOBJ_EQ(zeroStats.toBSON(), obj["compactStats"].Obj());
    ASSERT_TRUE(obj.hasField("cleanupStats"));
    ASSERT_BSONOBJ_EQ(cleanupStats.toBSON(), obj["cleanupStats"].Obj());
    ASSERT_FALSE(obj.hasField("emuBinaryStats"));
}

TEST_F(FLEStatsTest, BinaryEmuStatsAreEmptyWithoutTesting) {
    {
        auto tracker = instance->makeEmuBinaryTracker();
        tracker.recordSuboperation();
    }

    ASSERT_TRUE(instance->includeByDefault());

    auto obj = instance->generateSection(opCtx, BSONElement());
    ASSERT_TRUE(obj.hasField("compactStats"));
    ASSERT_BSONOBJ_EQ(zeroStats.toBSON(), obj["compactStats"].Obj());
    ASSERT_TRUE(obj.hasField("cleanupStats"));
    ASSERT_BSONOBJ_EQ(zeroStats.toBSON(), obj["cleanupStats"].Obj());
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
    ASSERT_TRUE(obj.hasField("compactStats"));
    ASSERT_BSONOBJ_EQ(zeroStats.toBSON(), obj["compactStats"].Obj());
    ASSERT_TRUE(obj.hasField("cleanupStats"));
    ASSERT_BSONOBJ_EQ(zeroStats.toBSON(), obj["cleanupStats"].Obj());
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
            } else if (indexType == "multi") {
                for (auto i = count; i > 0; i--) {
                    fields.emplace_back(keyId, indexType);

                    std::vector<QueryTypeConfig> queries;
                    queries.push_back(
                        QueryTypeConfig::parse(fromjson(R"({"queryType": "suffixPreview"})")));
                    queries.push_back(
                        QueryTypeConfig::parse(fromjson(R"({"queryType": "prefixPreview"})")));

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

    auto efc1 = buildConfig({{"unindexed", 4}, {"equality", 2}, {"multi", 3}});
    instance->updateIndexTypeStatsOnRegisterCollection(efc1);
    expected.unindexed++;
    expected.equality++;
    expected.suffixPreview++;
    expected.prefixPreview++;
    assertCounters(expected);

    auto efc2 = buildConfig({{"range", 3}, {"rangePreview", 1}, {"substringPreview", 2}});
    instance->updateIndexTypeStatsOnRegisterCollection(efc2);
    expected.range++;
    expected.rangePreview++;
    expected.substringPreview++;
    assertCounters(expected);

    auto efc3 = buildConfig({{"suffixPreview", 1}, {"multi", 1}});
    instance->updateIndexTypeStatsOnRegisterCollection(efc3);
    expected.suffixPreview++;
    expected.prefixPreview++;
    assertCounters(expected);

    instance->updateIndexTypeStatsOnDeregisterCollection(efc2);
    expected.range--;
    expected.rangePreview--;
    expected.substringPreview--;
    assertCounters(expected);

    instance->updateIndexTypeStatsOnRegisterCollection(efc3);
    expected.suffixPreview++;
    expected.prefixPreview++;
    assertCounters(expected);

    instance->updateIndexTypeStatsOnDeregisterCollection(efc3);
    expected.suffixPreview--;
    expected.prefixPreview--;
    assertCounters(expected);

    instance->updateIndexTypeStatsOnDeregisterCollection(efc1);
    expected.unindexed--;
    expected.equality--;
    expected.suffixPreview--;
    expected.prefixPreview--;
    assertCounters(expected);

    instance->updateIndexTypeStatsOnDeregisterCollection(efc3);
    expected.suffixPreview--;
    expected.prefixPreview--;
    assertCounters(expected);
}


}  // namespace
}  // namespace mongo
