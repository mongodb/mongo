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

#include "mongo/db/query/query_knobs/query_knob.h"
#include "mongo/db/query/query_knobs/query_knob_test_knobs.h"
#include "mongo/db/server_parameter.h"
#include "mongo/unittest/assert.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/framework.h"

#include <tuple>

namespace mongo {

using detail::QueryKnobRegistryBuilder;

namespace {

// Linear scan for the entry whose `knob` refers to `target`. Returns knobCount()
// on miss so callers can assert without risking an out-of-range `entry()` call.
QueryKnobId findEntryIndex(const QueryKnobBase& target) {
    const auto& reg = QueryKnobRegistry::instance();
    for (size_t i = 0; i < reg.knobCount(); ++i) {
        auto id = QueryKnobId(i);
        if (&reg.entry(id).knob == &target) {
            return id;
        }
    }
    return QueryKnobId(reg.knobCount());
}

// The test IDL registers `testIntKnob` as a ServerParameter, which is convenient
// to pass into Builder.addFromServerParameter() when the specific parameter identity doesn't matter
// (invariant tests only exercise the builder's validation logic).
ServerParameter& anyServerParameter() {
    auto* sp = ServerParameterSet::getNodeParameterSet()->getIfExists("testIntKnob");
    invariant(sp);
    return *sp;
}

// -----------------------------------------------------------------------------
// Scenarios against the global registry (populated by QueryKnobRegistryInit).
// -----------------------------------------------------------------------------

TEST(QueryKnobRegistryTest, PqsKnobIsFindableByWireName) {
    const auto& reg = QueryKnobRegistry::instance();
    auto id = reg.getKnobIdForName("testIntKnobWire"_sd);
    ASSERT_TRUE(id.has_value());
    const auto& e = reg.entry(*id);
    ASSERT_EQ(&e.knob, &test_knobs::testIntKnob);
    ASSERT_TRUE(e.pqsSettable);
}

TEST(QueryKnobRegistryTest, NonPqsKnobInvisibleToLookupButCarriesWireName) {
    const auto& reg = QueryKnobRegistry::instance();
    ASSERT_FALSE(reg.getKnobIdForName("testLLKnobWire"_sd).has_value());

    auto idx = findEntryIndex(test_knobs::testLLKnob);
    ASSERT_LT(idx.value, reg.knobCount());
    const auto& e = reg.entry(idx);
    ASSERT_EQ(e.wireName, "testLLKnobWire"_sd);
    ASSERT_FALSE(e.pqsSettable);
}

TEST(QueryKnobRegistryTest, PlainServerParameterNotRegistered) {
    const auto& reg = QueryKnobRegistry::instance();
    ASSERT_FALSE(reg.getKnobIdForName("testPlainParam"_sd).has_value());

    auto* plain = ServerParameterSet::getNodeParameterSet()->getIfExists("testPlainParam");
    ASSERT(plain);
    for (size_t i = 0; i < reg.knobCount(); ++i) {
        auto id = QueryKnobId(i);
        ASSERT_NE(&reg.entry(id).param, plain);
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

TEST(QueryKnobRegistryTest, DenseIndicesWrittenBackToDescriptors) {
    const auto& reg = QueryKnobRegistry::instance();
    for (size_t i = 0; i < reg.knobCount(); ++i) {
        auto id = QueryKnobId(i);
        ASSERT_EQ(reg.entry(id).knob.id, id);
    }
}

TEST(QueryKnobRegistryTest, CountsReflectPqsSplitOverTestSubset) {
    const auto& reg = QueryKnobRegistry::instance();
    size_t testTotal = 0;
    size_t testPqs = 0;
    for (size_t i = 0; i < reg.knobCount(); ++i) {
        auto id = QueryKnobId(i);
        const auto& e = reg.entry(id);
        if (e.wireName.starts_with("test"_sd)) {
            ++testTotal;
            if (e.pqsSettable) {
                ++testPqs;
            }
        }
    }
    ASSERT_EQ(testTotal, 5u);
    ASSERT_EQ(testPqs, 2u);
}

// -----------------------------------------------------------------------------
// Scenarios against in-test QueryKnobRegistryBuilder.
// -----------------------------------------------------------------------------

TEST(QueryKnobRegistryTest, EmptyBuilderBuildsEmptyRegistry) {
    QueryKnobRegistryBuilder b;
    QueryKnobRegistry reg = std::move(b).build();
    ASSERT_EQ(reg.knobCount(), 0u);
    ASSERT_EQ(reg.knobsExposedToQuerySettingsCount(), 0u);
}

// Death tests live in a separate suite because gtest forbids mixing TEST and
// TEST_F (which DEATH_TEST_REGEX expands to) in the same suite.

DEATH_TEST_REGEX(QueryKnobRegistryDeathTest,
                 EntryGetterOutOfRangeTasserts,
                 "QueryKnobRegistry::entry.*out of range") {
    const auto& reg = QueryKnobRegistry::instance();
    auto id = QueryKnobId(reg.knobCount());
    reg.entry(id);
}

DEATH_TEST_REGEX(QueryKnobRegistryDeathTest,
                 DuplicateWireNameInvariant,
                 "duplicate wire name.*dup") {
    QueryKnobRegistryBuilder b;
    auto minFcv = multiversion::parseVersionForFeatureFlags("9.0"_sd);
    b.addFromServerParameter({.knob = test_knobs::testIntKnob,
                              .param = anyServerParameter(),
                              .wireName = "dup"_sd,
                              .pqsSettable = true,
                              .minFcv = minFcv});
    b.addFromServerParameter({.knob = test_knobs::testBoolKnob,
                              .param = anyServerParameter(),
                              .wireName = "dup"_sd,
                              .pqsSettable = true,
                              .minFcv = minFcv});
    std::ignore = std::move(b).build();
}

DEATH_TEST_REGEX(QueryKnobRegistryDeathTest,
                 EmptyWireNameInvariant,
                 "wire name must not be empty") {
    QueryKnobRegistryBuilder b;
    b.addFromServerParameter({.knob = test_knobs::testIntKnob,
                              .param = anyServerParameter(),
                              .wireName = ""_sd,
                              .pqsSettable = true,
                              .minFcv = multiversion::parseVersionForFeatureFlags("9.0"_sd)});
}

DEATH_TEST_REGEX(QueryKnobRegistryDeathTest,
                 PqsWithoutMinFcvInvariant,
                 "PQS knob.*foo.*requires minFcv") {
    QueryKnobRegistryBuilder b;
    b.addFromServerParameter({.knob = test_knobs::testIntKnob,
                              .param = anyServerParameter(),
                              .wireName = "foo"_sd,
                              .pqsSettable = true,
                              .minFcv = boost::none});
}

DEATH_TEST_REGEX(QueryKnobRegistryDeathTest,
                 MissingServerParameterInvariant,
                 "no ServerParameter for QueryKnob.*testIntKnob") {
    QueryKnobRegistryBuilder b;
    b.addFromServerParameter(test_knobs::testIntKnob, nullptr);
}

DEATH_TEST_REGEX(QueryKnobRegistryDeathTest,
                 ServerParameterMissingAnnotationInvariant,
                 "missing the query_knob annotation") {
    auto* plain = ServerParameterSet::getNodeParameterSet()->getIfExists("testPlainParam");
    QueryKnobRegistryBuilder b;
    b.addFromServerParameter(test_knobs::testIntKnob, plain);
}

DEATH_TEST_REGEX(QueryKnobRegistryDeathTest,
                 InvalidFcvMinInvariant,
                 "fcv.min 'nope'.*testIntKnob.*not a valid FCV") {
    QueryKnobRegistryBuilder b;
    b.addFromServerParameter(test_knobs::testIntKnob,
                             anyServerParameter(),
                             BSON("wire_name" << "foo"
                                              << "fcv" << BSON("min" << "nope")));
}

// Ancient FCVs (e.g. 5.0) are no longer in the binary's FCV table, so
// parseVersionForFeatureFlags rejects them and the invariant fires.
DEATH_TEST_REGEX(QueryKnobRegistryDeathTest,
                 AncientFcvMinInvariant,
                 "fcv.min '5.0'.*testIntKnob.*not a valid FCV") {
    QueryKnobRegistryBuilder b;
    b.addFromServerParameter(test_knobs::testIntKnob,
                             anyServerParameter(),
                             BSON("wire_name" << "foo"
                                              << "fcv" << BSON("min" << "5.0")));
}

DEATH_TEST_REGEX(QueryKnobRegistryDeathTest,
                 OrphanAnnotationInvariant,
                 "testPlainParam.*has a query_knob annotation but no matching QueryKnob") {
    auto* params = ServerParameterSet::getNodeParameterSet();
    auto* plain = params->getIfExists("testPlainParam");
    // Turn `plain` into an orphan: annotated as a query knob but no QueryKnob<T>
    // descriptor has ever been declared against its name.
    plain->setAnnotations(BSON("query_knob" << BSON("wire_name" << "orphanWire")));

    QueryKnobRegistryBuilder b;
    // Register all known-good test knobs so that only testPlainParam is
    // orphaned when the detection runs.
    b.addFromServerParameter(test_knobs::testIntKnob, params->getIfExists("testIntKnob"));
    b.addFromServerParameter(test_knobs::testDoubleKnob, params->getIfExists("testDoubleKnob"));
    b.addFromServerParameter(test_knobs::testBoolKnob, params->getIfExists("testBoolKnob"));
    b.addFromServerParameter(test_knobs::testLLKnob, params->getIfExists("testLLKnob"));
    b.addFromServerParameter(test_knobs::testEnumKnob, params->getIfExists("testEnumKnob"));
    b.detectOrphanAnnotations(*params);
}

// TODO SERVER-125395: add a death test for the C++/storage type mismatch case
// once QueryKnobBase gains a ConverterTraits<T>-aware expected-alternative
// check.

}  // namespace
}  // namespace mongo
