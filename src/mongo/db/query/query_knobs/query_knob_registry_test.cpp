// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/query_knobs/query_knob_registry.h"

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/json.h"
#include "mongo/db/query/query_knobs/query_knob.h"
#include "mongo/db/query/query_knobs/query_knob_test_knobs.h"
#include "mongo/db/server_parameter.h"
#include "mongo/unittest/assert.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/framework.h"

#include <cmath>
#include <memory>
#include <string_view>
#include <tuple>

namespace mongo {

namespace {
using namespace std::literals::string_view_literals;

const QueryKnobRegistry& registry() {
    return QueryKnobRegistry::instance();
}

struct QueryKnobAnnotationArgs {
    boost::optional<std::string_view> wireName;
    boost::optional<bool> pqsSettable;
    boost::optional<std::string_view> minFcv;
};
BSONObj annotations(QueryKnobAnnotationArgs args) {
    BSONObjBuilder builder;
    BSONObjBuilder queryKnobBuilder(builder.subobjStart("query_knob"));
    if (args.wireName) {
        queryKnobBuilder.append("wire_name", *args.wireName);
    }
    if (args.pqsSettable) {
        queryKnobBuilder.append("pqs_settable", *args.pqsSettable);
    }
    if (args.minFcv) {
        BSONObjBuilder fcvBuilder(queryKnobBuilder.subobjStart("fcv"));
        fcvBuilder.append("min", *args.minFcv);
        fcvBuilder.doneFast();
    }
    queryKnobBuilder.doneFast();
    return builder.obj();
}

template <typename T>
auto createDummyServerParameter(std::string_view paramName, BSONObj annotations) {
    using Storage = Atomic<T>;
    using SPT = IDLServerParameterWithStorage<ServerParameterType::kStartupAndRuntime, Storage>;
    static Storage atomic;
    auto param = std::make_unique<SPT>(paramName, atomic);
    param->setAnnotations(annotations);
    return param;
}

QueryKnobRegistry::Entry createDummyKnobEntry(ServerParameter* sp, QueryKnobId id = {}) {
    return QueryKnobRegistry::Entry(id, sp, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr);
}

// -----------------------------------------------------------------------------
// Scenarios against the global registry (populated by QueryKnobRegistryInit).
// -----------------------------------------------------------------------------

TEST(QueryKnobRegistryTest, PqsKnobIsFindableByWireName) {
    auto id = registry().getKnobIdForName("testIntKnobWire"sv);
    ASSERT_TRUE(id.has_value());
    ASSERT_EQ(*id, test_knobs::testIntKnob.id);
    const auto& entry = registry().entry(*id);
    ASSERT_TRUE(entry.pqsSettable);
}

TEST(QueryKnobRegistryTest, NonPqsKnobInvisibleToLookupButCarriesWireName) {
    ASSERT_EQ(registry().getKnobIdForName("testLLKnobWire"sv), boost::none);
    const auto& entry = registry().entry(test_knobs::testLLKnob.id);
    ASSERT_EQ(entry.wireName, "testLLKnobWire"sv);
    ASSERT_FALSE(entry.pqsSettable);
}

// Long long knob values must be accepted from any BSON numeric type, matching setParameter's
// coercion semantics: doubles are truncated toward zero.
TEST(QueryKnobRegistryTest, LongLongKnobFromBSONCoercesNumericTypes) {
    const auto& entry = registry().entry(test_knobs::testLLKnob.id);
    ASSERT_EQ(std::get<long long>(entry.fromBSON(BSON("v" << 0.0).firstElement())), 0LL);
    ASSERT_EQ(std::get<long long>(entry.fromBSON(BSON("v" << 5).firstElement())), 5LL);
    ASSERT_EQ(std::get<long long>(entry.fromBSON(BSON("v" << 5.5).firstElement())), 5LL);
    ASSERT_EQ(std::get<long long>(entry.fromBSON(BSON("v" << -5.5).firstElement())), -5LL);
    ASSERT_THROWS(entry.fromBSON(BSON("v" << std::nan("")).firstElement()), DBException);
}

TEST(QueryKnobRegistryTest, PlainServerParameterNotRegistered) {
    ASSERT_FALSE(registry().getKnobIdForName("testPlainParam"sv).has_value());
    auto* plain = ServerParameterSet::getNodeParameterSet()->getIfExists("testPlainParam");
    ASSERT(plain);
    for (auto&& entry : registry().entries()) {
        ASSERT_NE(entry.param->name(), "testPlainParam");
    }
}

TEST(QueryKnobRegistryTest, MinFcvRoundTrip) {
    const auto& reg = QueryKnobRegistry::instance();
    auto id = reg.getKnobIdForName("testIntKnobWire"sv);
    ASSERT_TRUE(id.has_value());
    const auto& e = reg.entry(*id);
    ASSERT_TRUE(e.minFcv.has_value());
    ASSERT_EQ(*e.minFcv, multiversion::parseVersionForFeatureFlags("9.0"sv));
}

TEST(QueryKnobRegistryTest, DenseIndicesKnobs) {
    for (size_t i = 0; i < registry().knobCount(); ++i) {
        auto id = QueryKnobId(i);
        auto&& entry = registry().entry(id);
        ASSERT_EQ(entry.id, id);
    }
}

TEST(QueryKnobRegistryTest, CountsReflectPqsSplitOverTestSubset) {
    size_t testTotal = 0;
    size_t testPqs = 0;
    for (auto&& entry : registry().entries()) {
        ++testTotal;
        if (entry.pqsSettable) {
            ++testPqs;
        }
    }
    ASSERT_EQ(testTotal, 6u);
    ASSERT_EQ(testPqs, 4u);
}

// -----------------------------------------------------------------------------
// Scenarios against in-test QueryKnobRegistryBuilder.
// -----------------------------------------------------------------------------

TEST(QueryKnobRegistryTest, EmptyBuilderBuildsEmptyRegistry) {
    QueryKnobRegistry reg;
    ASSERT_EQ(reg.knobCount(), 0u);
    ASSERT_EQ(reg.knobsExposedToQuerySettingsCount(), 0u);
    ASSERT_EQ(reg.entries().size(), 0u);
}

// Death tests live in a separate suite because gtest forbids mixing TEST and
// TEST_F (which DEATH_TEST_REGEX expands to) in the same suite.

DEATH_TEST_REGEX(QueryKnobRegistryDeathTest,
                 EntryGetterOutOfRangeTasserts,
                 "QueryKnobRegistry::entry.*out of range") {
    auto outOfBoundsId = QueryKnobId(registry().knobCount());
    registry().entry(outOfBoundsId);
}

DEATH_TEST_REGEX(QueryKnobRegistryDeathTest,
                 DuplicateWireNameInvariant,
                 "duplicate wire name.*dup") {
    auto paramA = createDummyServerParameter<bool>("dupParamA",
                                                   annotations({
                                                       .wireName = "dup"sv,
                                                       .pqsSettable = true,
                                                       .minFcv = "9.0"sv,
                                                   }));
    auto paramB = createDummyServerParameter<bool>("dupParamB",
                                                   annotations({
                                                       .wireName = "dup"sv,
                                                       .pqsSettable = true,
                                                       .minFcv = "9.0"sv,
                                                   }));
    QueryKnobRegistry registry{{
        createDummyKnobEntry(paramA.get(), QueryKnobId(0)),
        createDummyKnobEntry(paramB.get(), QueryKnobId(1)),
    }};
}

DEATH_TEST_REGEX(QueryKnobRegistryDeathTest,
                 EmptyWireNameInvariant,
                 "wire name must not be empty") {
    auto param = createDummyServerParameter<bool>("emptyWireNameParam",
                                                  annotations({
                                                      .wireName = ""sv,
                                                  }));
    std::ignore = createDummyKnobEntry(param.get());
}

DEATH_TEST_REGEX(QueryKnobRegistryDeathTest,
                 PqsWithoutMinFcvInvariant,
                 "PQS knob.*pqsKnobWithNoFcv.*requires minFcv") {
    auto param = createDummyServerParameter<bool>("pqsKnobWithNoFcv",
                                                  annotations({
                                                      .wireName = "foo"sv,
                                                      .pqsSettable = true,
                                                  }));
    std::ignore = createDummyKnobEntry(param.get());
    QueryKnobRegistry::Entry{
        QueryKnobId(0), param.get(), nullptr, nullptr, nullptr, nullptr, nullptr, nullptr};
}

DEATH_TEST_REGEX(QueryKnobRegistryDeathTest,
                 MissingServerParameterInvariant,
                 "No server parameter found for query knob") {
    std::ignore = createDummyKnobEntry(nullptr);
}

DEATH_TEST_REGEX(QueryKnobRegistryDeathTest,
                 ServerParameterMissingAnnotationInvariant,
                 "missing the query_knob annotation") {
    auto param = createDummyServerParameter<bool>("noAnnotationKnob", BSONObj());
    std::ignore = createDummyKnobEntry(param.get());
}

DEATH_TEST_REGEX(QueryKnobRegistryDeathTest,
                 InvalidFcvMinInvariant,
                 "fcv.min 'nope'.*foo.*not a valid FCV") {
    auto param = createDummyServerParameter<bool>("pqsKnobWithNoFcv",
                                                  annotations({
                                                      .wireName = "foo"sv,
                                                      .pqsSettable = true,
                                                      .minFcv = "nope"sv,
                                                  }));
    std::ignore = createDummyKnobEntry(param.get());
}

// Ancient FCVs (e.g. 5.0) are no longer in the binary's FCV table, so
// parseVersionForFeatureFlags rejects them and the invariant fires.
DEATH_TEST_REGEX(QueryKnobRegistryDeathTest,
                 AncientFcvMinInvariant,
                 "fcv.min '5.0'.*foo.*not a valid FCV") {
    auto param = createDummyServerParameter<bool>("oldFcvKnob",
                                                  annotations({
                                                      .wireName = "foo"sv,
                                                      .pqsSettable = true,
                                                      .minFcv = "5.0"sv,
                                                  }));
    std::ignore = createDummyKnobEntry(param.get());
}

DEATH_TEST_REGEX(QueryKnobRegistryDeathTest,
                 OrphanAnnotationInvariant,
                 "paramC.*has a query_knob annotation but no matching QueryKnob") {
    auto paramA = createDummyServerParameter<bool>("paramA",
                                                   annotations({
                                                       .wireName = "paramA"sv,
                                                   }));
    auto paramB = createDummyServerParameter<bool>("paramB",
                                                   annotations({
                                                       .wireName = "paramB"sv,
                                                   }));
    auto paramC = createDummyServerParameter<bool>("paramC",
                                                   annotations({
                                                       .wireName = "paramC"sv,
                                                   }));
    std::vector entries = {
        createDummyKnobEntry(paramA.get(), QueryKnobId(0)),
        createDummyKnobEntry(paramB.get(), QueryKnobId(1)),
    };

    ServerParameterSet params;
    params.add(std::move(paramA));
    params.add(std::move(paramB));
    params.add(std::move(paramC));
    detail::detectOrphanAnnotations(entries, params);
}

DEATH_TEST_REGEX(QueryKnobRegistryDeathTest,
                 MissmatchedServerParameterTypeInvariant,
                 ".*No server parameter found.*") {
    std::ignore =
        QueryKnobRegistry::Entry::create<gTestBoolKnob>(QueryKnob<bool>(), kTestIntKnobName);
}

// -----------------------------------------------------------------------------
// appendType
// -----------------------------------------------------------------------------

BSONObj typeInfoFor(QueryKnobId id) {
    BSONObjBuilder bob;
    registry().entry(id).appendType(&bob);
    return bob.obj();
}

TEST(QueryKnobRegistryTest, AppendTypeIntKnob) {
    ASSERT_BSONOBJ_EQ(typeInfoFor(test_knobs::testIntKnob.id), fromjson(R"({"type": "int"})"));
}

TEST(QueryKnobRegistryTest, AppendTypeDoubleKnob) {
    ASSERT_BSONOBJ_EQ(typeInfoFor(test_knobs::testDoubleKnob.id),
                      fromjson(R"({"type": "double"})"));
}

TEST(QueryKnobRegistryTest, AppendTypeBoolKnob) {
    ASSERT_BSONOBJ_EQ(typeInfoFor(test_knobs::testBoolKnob.id), fromjson(R"({"type": "bool"})"));
}

TEST(QueryKnobRegistryTest, AppendTypeLongLongKnob) {
    ASSERT_BSONOBJ_EQ(typeInfoFor(test_knobs::testLLKnob.id), fromjson(R"({"type": "long long"})"));
}

TEST(QueryKnobRegistryTest, AppendTypeEnumKnob) {
    ASSERT_BSONOBJ_EQ(typeInfoFor(test_knobs::testEnumKnob.id),
                      fromjson(R"({"type": "enum", "allowedValues": ["alpha", "beta"]})"));
}

}  // namespace
}  // namespace mongo
