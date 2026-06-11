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

#include "mongo/db/query/query_knobs/query_knob_registry.h"

#include "mongo/bson/bsonobj.h"
#include "mongo/db/query/query_knobs/query_knob.h"
#include "mongo/db/query/query_knobs/query_knob_test_knobs.h"
#include "mongo/db/server_parameter.h"
#include "mongo/unittest/assert.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/framework.h"

#include <memory>
#include <tuple>

namespace mongo {

namespace {
const QueryKnobRegistry& registry() {
    return QueryKnobRegistry::instance();
}

struct QueryKnobAnnotationArgs {
    boost::optional<StringData> wireName;
    boost::optional<bool> pqsSettable;
    boost::optional<StringData> minFcv;
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
auto createDummyServerParameter(StringData paramName, BSONObj annotations) {
    using Storage = AtomicWord<T>;
    using SPT = IDLServerParameterWithStorage<ServerParameterType::kStartupAndRuntime, Storage>;
    static Storage atomic;
    auto param = std::make_unique<SPT>(paramName, atomic);
    param->setAnnotations(annotations);
    return param;
}

QueryKnobRegistry::Entry createDummyKnobEntry(ServerParameter* sp, QueryKnobId id = {}) {
    return QueryKnobRegistry::Entry(id, sp, nullptr, nullptr, nullptr);
}

// -----------------------------------------------------------------------------
// Scenarios against the global registry (populated by QueryKnobRegistryInit).
// -----------------------------------------------------------------------------

TEST(QueryKnobRegistryTest, PqsKnobIsFindableByWireName) {
    auto id = registry().getKnobIdForName("testIntKnobWire"_sd);
    ASSERT_TRUE(id.has_value());
    ASSERT_EQ(*id, test_knobs::testIntKnob.id);
    const auto& entry = registry().entry(*id);
    ASSERT_TRUE(entry.pqsSettable);
}

TEST(QueryKnobRegistryTest, NonPqsKnobInvisibleToLookupButCarriesWireName) {
    ASSERT_EQ(registry().getKnobIdForName("testDoubleKnobWire"_sd), boost::none);
    const auto& entry = registry().entry(test_knobs::testDoubleKnob.id);
    ASSERT_EQ(entry.wireName, "testDoubleKnobWire"_sd);
    ASSERT_FALSE(entry.pqsSettable);
}

TEST(QueryKnobRegistryTest, PlainServerParameterNotRegistered) {
    ASSERT_FALSE(registry().getKnobIdForName("testPlainParam"_sd).has_value());
    auto* plain = ServerParameterSet::getNodeParameterSet()->getIfExists("testPlainParam");
    ASSERT(plain);
    for (auto&& entry : registry().entries()) {
        ASSERT_NE(entry.param->name(), "testPlainParam");
    }
}

TEST(QueryKnobRegistryTest, MinFcvRoundTrip) {
    const auto& reg = QueryKnobRegistry::instance();
    auto id = reg.getKnobIdForName("testIntKnobWire"_sd);
    ASSERT_TRUE(id.has_value());
    const auto& e = reg.entry(*id);
    ASSERT_TRUE(e.minFcv.has_value());
    ASSERT_EQ(*e.minFcv, multiversion::parseVersionForFeatureFlags("9.0"_sd));
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
    ASSERT_EQ(testTotal, 5u);
    ASSERT_EQ(testPqs, 2u);
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
                                                       .wireName = "dup"_sd,
                                                       .pqsSettable = true,
                                                       .minFcv = "9.0"_sd,
                                                   }));
    auto paramB = createDummyServerParameter<bool>("dupParamB",
                                                   annotations({
                                                       .wireName = "dup"_sd,
                                                       .pqsSettable = true,
                                                       .minFcv = "9.0"_sd,
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
                                                      .wireName = ""_sd,
                                                  }));
    std::ignore = createDummyKnobEntry(param.get());
}

DEATH_TEST_REGEX(QueryKnobRegistryDeathTest,
                 PqsWithoutMinFcvInvariant,
                 "PQS knob.*pqsKnobWithNoFcv.*requires minFcv") {
    auto param = createDummyServerParameter<bool>("pqsKnobWithNoFcv",
                                                  annotations({
                                                      .wireName = "foo"_sd,
                                                      .pqsSettable = true,
                                                  }));
    std::ignore = createDummyKnobEntry(param.get());
    QueryKnobRegistry::Entry{QueryKnobId(0), param.get(), nullptr, nullptr, nullptr};
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
                                                      .wireName = "foo"_sd,
                                                      .pqsSettable = true,
                                                      .minFcv = "nope"_sd,
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
                                                      .wireName = "foo"_sd,
                                                      .pqsSettable = true,
                                                      .minFcv = "5.0"_sd,
                                                  }));
    std::ignore = createDummyKnobEntry(param.get());
}

DEATH_TEST_REGEX(QueryKnobRegistryDeathTest,
                 OrphanAnnotationInvariant,
                 "paramC.*has a query_knob annotation but no matching QueryKnob") {
    auto paramA = createDummyServerParameter<bool>("paramA",
                                                   annotations({
                                                       .wireName = "paramA"_sd,
                                                   }));
    auto paramB = createDummyServerParameter<bool>("paramB",
                                                   annotations({
                                                       .wireName = "paramB"_sd,
                                                   }));
    auto paramC = createDummyServerParameter<bool>("paramC",
                                                   annotations({
                                                       .wireName = "paramC"_sd,
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

}  // namespace
}  // namespace mongo
