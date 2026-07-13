// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/query_settings/query_knob_overrides.h"

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/query/query_fcv_environment_for_test.h"
#include "mongo/unittest/assert.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/ensure_fcv.h"
#include "mongo/unittest/framework.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/version/releases.h"

namespace mongo::query_settings {
namespace {

// Uses the PQS-settable test knobs from query_knob_test.idl:
//   testIntKnob (testIntKnobWire)       — validator: {callback: rejects 13, gt: 0, lt: 1000}
//   testDoubleKnob (testDoubleKnobWire) — validator: {gte: 1.0, lte: 10.0}
//   testBoolKnob (testBoolKnobWire)
// TODO SERVER-125993: Cover enum knob.

TEST(QuerySettingsKnobOverridesTest, EmptyBSONYieldsEmptyOverrides) {
    auto overrides = QuerySettingsKnobOverrides::fromBSON(BSONObj{});
    ASSERT_TRUE(overrides.empty());
    ASSERT_EQ(overrides.entries().size(), 0u);
}

TEST(QuerySettingsKnobOverridesTest, RoundTripInt) {
    auto bson = BSON("testIntKnobWire" << 99);
    auto overrides = QuerySettingsKnobOverrides::fromBSON(bson);

    ASSERT_FALSE(overrides.empty());
    ASSERT_EQ(overrides.entries().size(), 1u);
    ASSERT_TRUE(std::holds_alternative<int>(overrides.entries()[0].value));
    ASSERT_EQ(std::get<int>(overrides.entries()[0].value), 99);
    ASSERT_BSONOBJ_EQ(overrides.toBSON(), bson);
}

TEST(QuerySettingsKnobOverridesTest, RoundTripBool) {
    auto bson = BSON("testBoolKnobWire" << true);
    auto overrides = QuerySettingsKnobOverrides::fromBSON(bson);

    ASSERT_EQ(overrides.entries().size(), 1u);
    ASSERT_TRUE(std::holds_alternative<bool>(overrides.entries()[0].value));
    ASSERT_TRUE(std::get<bool>(overrides.entries()[0].value));
    ASSERT_BSONOBJ_EQ(overrides.toBSON(), bson);
}

TEST(QuerySettingsKnobOverridesTest, RoundTripMultipleKnobs) {
    auto bson = BSON("testIntKnobWire" << 7 << "testBoolKnobWire" << false);
    auto overrides = QuerySettingsKnobOverrides::fromBSON(bson);

    ASSERT_EQ(overrides.entries().size(), 2u);
    ASSERT_BSONOBJ_EQ(overrides.toBSON(), bson);
}

TEST(QuerySettingsKnobOverridesTest, NullElementStoresDeleteQueryKnobOverride) {
    auto bson = BSON("testIntKnobWire" << BSONNULL);
    auto overrides = QuerySettingsKnobOverrides::fromBSON(bson);

    ASSERT_EQ(overrides.entries().size(), 1u);
    ASSERT_TRUE(std::holds_alternative<DeleteQueryKnobOverride>(overrides.entries()[0].value));
}

TEST(QuerySettingsKnobOverridesTest, DuplicateKnob) {
    auto bson = BSON("testIntKnobWire" << 5 << "testIntKnobWire" << 10);
    auto overrides = QuerySettingsKnobOverrides::fromBSON(bson);

    ASSERT_EQ(overrides.entries().size(), 2u);
    ASSERT_EQ(std::get<int>(overrides.entries()[0].value), 5);
    ASSERT_EQ(std::get<int>(overrides.entries()[1].value), 10);
    ASSERT_BSONOBJ_EQ(overrides.toBSON(), bson);
}

// Numeric knob values must be accepted regardless of the BSON numeric type they arrive as, as
// long as the value is exactly representable, matching setParameter's coercion semantics.
TEST(QuerySettingsKnobOverridesTest, IntKnobAcceptsIntegralDouble) {
    auto overrides = QuerySettingsKnobOverrides::fromBSON(BSON("testIntKnobWire" << 5.0));
    ASSERT_EQ(std::get<int>(overrides.entries()[0].value), 5);
}

TEST(QuerySettingsKnobOverridesTest, IntKnobAcceptsNumberLong) {
    auto overrides = QuerySettingsKnobOverrides::fromBSON(BSON("testIntKnobWire" << 5LL));
    ASSERT_EQ(std::get<int>(overrides.entries()[0].value), 5);
}

// Non-integral doubles are truncated toward zero, matching setParameter's coercion semantics.
TEST(QuerySettingsKnobOverridesTest, IntKnobTruncatesNonIntegralDouble) {
    auto overrides = QuerySettingsKnobOverrides::fromBSON(BSON("testIntKnobWire" << 5.5));
    ASSERT_EQ(std::get<int>(overrides.entries()[0].value), 5);
}

TEST(QuerySettingsKnobOverridesTest, DoubleKnobAcceptsInt) {
    auto overrides = QuerySettingsKnobOverrides::fromBSON(BSON("testDoubleKnobWire" << 5));
    ASSERT_EQ(std::get<double>(overrides.entries()[0].value), 5.0);
}

TEST(QuerySettingsKnobOverridesTest, DoubleKnobAcceptsNumberLong) {
    auto overrides = QuerySettingsKnobOverrides::fromBSON(BSON("testDoubleKnobWire" << 5LL));
    ASSERT_EQ(std::get<double>(overrides.entries()[0].value), 5.0);
}

TEST(QuerySettingsKnobOverridesTest, IntKnobRejectsInt32Overflow) {
    ASSERT_THROWS_CODE(QuerySettingsKnobOverrides::fromBSON(BSON("testIntKnobWire" << (1LL << 40)))
                           .uassertNoErrors(),
                       DBException,
                       12194501);
}

TEST(QuerySettingsKnobOverridesTest, DoubleKnobRejectsNonNumeric) {
    ASSERT_THROWS_CODE(
        QuerySettingsKnobOverrides::fromBSON(BSON("testDoubleKnobWire" << "5.0")).uassertNoErrors(),
        DBException,
        12194501);
}

TEST(QuerySettingsKnobOverridesTest, WrongTypeThrows) {
    ASSERT_THROWS_CODE(QuerySettingsKnobOverrides::fromBSON(BSON("testIntKnobWire" << "notAnInt"))
                           .uassertNoErrors(),
                       DBException,
                       12194501);
}

TEST(QuerySettingsKnobOverridesTest, IntValidatorRejectsOutOfRangeValue) {
    // testIntKnob has validator: {gt: 0}; 0 must be rejected.
    ASSERT_THROWS_CODE(
        QuerySettingsKnobOverrides::fromBSON(BSON("testIntKnobWire" << 0)).uassertNoErrors(),
        DBException,
        12194501);
}

TEST(QuerySettingsKnobOverridesTest, IntValidatorAcceptsInRangeValue) {
    auto overrides = QuerySettingsKnobOverrides::fromBSON(BSON("testIntKnobWire" << 1));
    ASSERT_EQ(overrides.entries().size(), 1u);
    ASSERT_EQ(std::get<int>(overrides.entries()[0].value), 1);
}

// testIntKnob has validator: {gt: 0, lt: 1000} — covers the exclusive upper bound (lt).
TEST(QuerySettingsKnobOverridesTest, IntValidatorRejectsAtExclusiveUpperBound) {
    ASSERT_THROWS_CODE(
        QuerySettingsKnobOverrides::fromBSON(BSON("testIntKnobWire" << 1000)).uassertNoErrors(),
        DBException,
        12194501);
}

TEST(QuerySettingsKnobOverridesTest, IntValidatorAcceptsBelowExclusiveUpperBound) {
    auto overrides = QuerySettingsKnobOverrides::fromBSON(BSON("testIntKnobWire" << 999));
    ASSERT_EQ(std::get<int>(overrides.entries()[0].value), 999);
}

// testIntKnob also has a callback validator that rejects the sentinel value 13.
TEST(QuerySettingsKnobOverridesTest, IntCallbackValidatorRejectsForbiddenSentinel) {
    ASSERT_THROWS_CODE(
        QuerySettingsKnobOverrides::fromBSON(BSON("testIntKnobWire" << 13)).uassertNoErrors(),
        DBException,
        12194501);
}

TEST(QuerySettingsKnobOverridesTest, IntCallbackValidatorAcceptsNonSentinel) {
    auto overrides = QuerySettingsKnobOverrides::fromBSON(BSON("testIntKnobWire" << 14));
    ASSERT_EQ(std::get<int>(overrides.entries()[0].value), 14);
}

// testDoubleKnob has validator: {gte: 1.0, lte: 10.0} — covers inclusive bounds (gte, lte).
TEST(QuerySettingsKnobOverridesTest, DoubleValidatorRejectsBelowMin) {
    ASSERT_THROWS_CODE(
        QuerySettingsKnobOverrides::fromBSON(BSON("testDoubleKnobWire" << 0.5)).uassertNoErrors(),
        DBException,
        12194501);
}

TEST(QuerySettingsKnobOverridesTest, DoubleValidatorAcceptsAtMin) {
    auto overrides = QuerySettingsKnobOverrides::fromBSON(BSON("testDoubleKnobWire" << 1.0));
    ASSERT_EQ(std::get<double>(overrides.entries()[0].value), 1.0);
}

TEST(QuerySettingsKnobOverridesTest, DoubleValidatorAcceptsInRange) {
    auto overrides = QuerySettingsKnobOverrides::fromBSON(BSON("testDoubleKnobWire" << 5.0));
    ASSERT_EQ(std::get<double>(overrides.entries()[0].value), 5.0);
}

TEST(QuerySettingsKnobOverridesTest, DoubleValidatorAcceptsAtMax) {
    auto overrides = QuerySettingsKnobOverrides::fromBSON(BSON("testDoubleKnobWire" << 10.0));
    ASSERT_EQ(std::get<double>(overrides.entries()[0].value), 10.0);
}

TEST(QuerySettingsKnobOverridesTest, DoubleValidatorRejectsAboveMax) {
    ASSERT_THROWS_CODE(
        QuerySettingsKnobOverrides::fromBSON(BSON("testDoubleKnobWire" << 10.1)).uassertNoErrors(),
        DBException,
        12194501);
}

TEST(QuerySettingsKnobOverridesTest, ValidKnobBeforeInvalidKnobThrows) {
    auto bson = BSON("testIntKnobWire" << 5 << "testBoolKnobWire"
                                       << "notABool");
    ASSERT_THROWS_CODE(
        QuerySettingsKnobOverrides::fromBSON(bson).uassertNoErrors(), DBException, 12194501);
}

TEST(QuerySettingsKnobOverridesTest, UnknownKnobThrows) {
    ASSERT_THROWS_CODE(
        QuerySettingsKnobOverrides::fromBSON(BSON("totallyUnknownKnob" << 1)).uassertNoErrors(),
        DBException,
        12194501);
}

TEST(QuerySettingsKnobOverridesTest, ToBSONSerializesDeleteSentinelAsNull) {
    auto overrides = QuerySettingsKnobOverrides::fromBSON(BSON("testIntKnobWire" << BSONNULL));
    ASSERT_BSONOBJ_EQ(overrides.toBSON(), BSON("testIntKnobWire" << BSONNULL));
}

TEST(QuerySettingsKnobOverridesTest, MergeEmptyWithEmpty) {
    auto result = QuerySettingsKnobOverrides::merge({}, {});
    ASSERT_TRUE(result.empty());
}

TEST(QuerySettingsKnobOverridesTest, MergeAddsNewKnob) {
    auto lhs = QuerySettingsKnobOverrides::fromBSON(BSONObj{});
    auto rhs = QuerySettingsKnobOverrides::fromBSON(BSON("testIntKnobWire" << 7));
    auto result = QuerySettingsKnobOverrides::merge(lhs, rhs);
    ASSERT_BSONOBJ_EQ(result.toBSON(), rhs.toBSON());
}

TEST(QuerySettingsKnobOverridesTest, MergeUpdatesExistingKnob) {
    auto lhs = QuerySettingsKnobOverrides::fromBSON(BSON("testIntKnobWire" << 1));
    auto rhs = QuerySettingsKnobOverrides::fromBSON(BSON("testIntKnobWire" << 2));
    auto result = QuerySettingsKnobOverrides::merge(lhs, rhs);
    ASSERT_BSONOBJ_EQ(result.toBSON(), rhs.toBSON());
}

TEST(QuerySettingsKnobOverridesTest, MergePreservesLhsKnobsNotInRhs) {
    auto lhs = QuerySettingsKnobOverrides::fromBSON(
        BSON("testIntKnobWire" << 3 << "testBoolKnobWire" << false));
    auto rhs = QuerySettingsKnobOverrides::fromBSON(BSON("testBoolKnobWire" << true));
    auto result = QuerySettingsKnobOverrides::merge(lhs, rhs);
    auto expected = QuerySettingsKnobOverrides::fromBSON(
        BSON("testIntKnobWire" << 3 << "testBoolKnobWire" << true));
    ASSERT_BSONOBJ_EQ(result.toBSON(), expected.toBSON());
}

// merge() does not resolve removal sentinels; simplify() does. These tests exercise the combined
// merge()+simplify() flow used on the write path.
TEST(QuerySettingsKnobOverridesTest, MergeThenSimplifyRemovesKnob) {
    auto lhs = QuerySettingsKnobOverrides::fromBSON(BSON("testIntKnobWire" << 5));
    auto rhs = QuerySettingsKnobOverrides::fromBSON(BSON("testIntKnobWire" << BSONNULL));
    auto result = QuerySettingsKnobOverrides::merge(lhs, rhs);
    result.simplify();
    ASSERT_TRUE(result.empty());
}

TEST(QuerySettingsKnobOverridesTest, MergeThenSimplifyNullOnAbsentKnobIsNoop) {
    auto lhs = QuerySettingsKnobOverrides::fromBSON(BSON("testBoolKnobWire" << true));
    auto rhs = QuerySettingsKnobOverrides::fromBSON(BSON("testIntKnobWire" << BSONNULL));
    auto result = QuerySettingsKnobOverrides::merge(lhs, rhs);
    result.simplify();
    ASSERT_BSONOBJ_EQ(result.toBSON(), lhs.toBSON());
}

TEST(QuerySettingsKnobOverridesTest, MergeThenSimplifyAllKnobsRemovedYieldsEmpty) {
    auto lhs = QuerySettingsKnobOverrides::fromBSON(
        BSON("testIntKnobWire" << 1 << "testBoolKnobWire" << true));
    auto rhs = QuerySettingsKnobOverrides::fromBSON(
        BSON("testIntKnobWire" << BSONNULL << "testBoolKnobWire" << BSONNULL));
    auto result = QuerySettingsKnobOverrides::merge(lhs, rhs);
    result.simplify();
    ASSERT_TRUE(result.empty());
}

TEST(QuerySettingsKnobOverridesTest, SimplifyStripsDeleteSentinels) {
    auto overrides = QuerySettingsKnobOverrides::fromBSON(
        BSON("testIntKnobWire" << 7 << "testBoolKnobWire" << BSONNULL));
    overrides.simplify();
    ASSERT_BSONOBJ_EQ(overrides.toBSON(), BSON("testIntKnobWire" << 7));
}

TEST(QuerySettingsKnobOverridesTest, SimplifyOnlySentinelsYieldsEmpty) {
    auto overrides = QuerySettingsKnobOverrides::fromBSON(BSON("testIntKnobWire" << BSONNULL));
    overrides.simplify();
    ASSERT_TRUE(overrides.empty());
}

TEST(QuerySettingsKnobOverridesTest, SimplifyPreservesRealKnobs) {
    auto overrides = QuerySettingsKnobOverrides::fromBSON(
        BSON("testIntKnobWire" << 7 << "testBoolKnobWire" << true));
    overrides.simplify();
    ASSERT_BSONOBJ_EQ(overrides.toBSON(),
                      BSON("testIntKnobWire" << 7 << "testBoolKnobWire" << true));
}

// FCV-related tests. The test knobs have minFcv "9.0" (== kLatest), except testLowFcvKnob which
// has minFcv "8.0" (== kLastLTS). Initializes the global FCV first, as EnsureFCV requires it.

// Parsing must not FCV-validate: this path also handles forwarded (mongos to shard) and stored
// settings, which may legitimately contain knobs above the current FCV mid-downgrade or after an
// interrupted one. FCV validation of user input is covered by validateQueryKnobs().
TEST(QuerySettingsKnobOverridesTest, ParseAcceptsKnobAboveCurrentFcv) {
    QueryFCVEnvironmentForTest::setUp();
    // (Generic FCV reference): FCV-related query knob parse test.
    unittest::EnsureFCV fcv(multiversion::GenericFCV::kLastLTS);
    auto overrides = QuerySettingsKnobOverrides::fromBSON(
        BSON("testIntKnobWire" << 5 << "testLowFcvKnobWire" << 5));
    ASSERT_EQ(overrides.entries().size(), 2u);
}

TEST(QuerySettingsKnobOverridesTest, RemoveKnobsRequiringHigherFcvKeepsSupportedKnobs) {
    auto overrides = QuerySettingsKnobOverrides::fromBSON(
        BSON("testIntKnobWire" << 5 << "testLowFcvKnobWire" << 5));
    // (Generic FCV reference): FCV-gated query knob removal test.
    ASSERT_TRUE(overrides.removeKnobsRequiringHigherFcv(multiversion::GenericFCV::kLastLTS));
    ASSERT_BSONOBJ_EQ(overrides.toBSON(), BSON("testLowFcvKnobWire" << 5));
}

TEST(QuerySettingsKnobOverridesTest, RemoveKnobsRequiringHigherFcvNoopWhenAllSupported) {
    auto bson = BSON("testIntKnobWire" << 5 << "testLowFcvKnobWire" << 5);
    auto overrides = QuerySettingsKnobOverrides::fromBSON(bson);
    // (Generic FCV reference): FCV-gated query knob removal test.
    ASSERT_FALSE(overrides.removeKnobsRequiringHigherFcv(multiversion::GenericFCV::kLatest));
    ASSERT_BSONOBJ_EQ(overrides.toBSON(), bson);
}

TEST(QuerySettingsKnobOverridesTest, RemoveKnobsRequiringHigherFcvAllRemovedYieldsEmpty) {
    auto overrides = QuerySettingsKnobOverrides::fromBSON(BSON("testIntKnobWire" << 5));
    // (Generic FCV reference): FCV-gated query knob removal test.
    ASSERT_TRUE(overrides.removeKnobsRequiringHigherFcv(multiversion::GenericFCV::kLastLTS));
    ASSERT_TRUE(overrides.empty());
}

TEST(QuerySettingsKnobOverridesTest, RemoveKnobsRequiringHigherFcvRemovesDeleteSentinels) {
    auto overrides = QuerySettingsKnobOverrides::fromBSON(BSON("testIntKnobWire" << BSONNULL));
    // (Generic FCV reference): FCV-gated query knob removal test.
    ASSERT_TRUE(overrides.removeKnobsRequiringHigherFcv(multiversion::GenericFCV::kLastLTS));
    ASSERT_TRUE(overrides.empty());
}

TEST(QuerySettingsKnobOverridesTest, FailpointRecordsErrorButDoesNotThrowKeepsOthers) {
    FailPointEnableBlock fp("failQueryKnobOverridesParsing", BSON("name" << "testIntKnobWire"));

    auto overrides = QuerySettingsKnobOverrides::fromBSON(
        BSON("testIntKnobWire" << 5 << "testBoolKnobWire" << true));

    ASSERT_EQ(overrides.entries().size(), 1u);
    ASSERT_TRUE(std::holds_alternative<bool>(overrides.entries()[0].value));
    ASSERT_TRUE(std::get<bool>(overrides.entries()[0].value));
    ASSERT_EQ(overrides.errors().size(), 1u);
    ASSERT_EQ(overrides.errors()[0].code(), 12194501);
}

TEST(QuerySettingsKnobOverridesTest, UnknownKnobRecordsErrorKeepsOthers) {
    auto overrides = QuerySettingsKnobOverrides::fromBSON(
        BSON("totallyUnknownKnob" << 1 << "testBoolKnobWire" << true));

    ASSERT_EQ(overrides.entries().size(), 1u);
    ASSERT_TRUE(std::get<bool>(overrides.entries()[0].value));
    ASSERT_EQ(overrides.errors().size(), 1u);
    ASSERT_EQ(overrides.errors()[0].code(), 12194501);
}

TEST(QuerySettingsKnobOverridesTest, ValidationFailureRecordsErrorKeepsOthers) {
    // testIntKnob's validator rejects 0 (gt: 0).
    auto overrides = QuerySettingsKnobOverrides::fromBSON(
        BSON("testIntKnobWire" << 0 << "testBoolKnobWire" << true));

    ASSERT_EQ(overrides.entries().size(), 1u);
    ASSERT_TRUE(std::get<bool>(overrides.entries()[0].value));
    ASSERT_EQ(overrides.errors().size(), 1u);
    ASSERT_EQ(overrides.errors()[0].code(), 12194501);
}

TEST(QuerySettingsKnobOverridesTest, AllInvalidYieldsEmptyEntriesButHasErrors) {
    auto overrides = QuerySettingsKnobOverrides::fromBSON(BSON("totallyUnknownKnob" << 1));
    ASSERT_TRUE(overrides.empty());
    ASSERT_TRUE(overrides.hasErrors());
    ASSERT_EQ(overrides.errors().size(), 1u);
}

TEST(QuerySettingsKnobOverridesTest, UassertNoErrorsIsNoopWhenNoErrors) {
    auto overrides = QuerySettingsKnobOverrides::fromBSON(BSON("testIntKnobWire" << 5));
    overrides.uassertNoErrors();
}

TEST(QuerySettingsKnobOverridesTest, UassertNoErrorsThrowsFirstRecordedError) {
    auto overrides = QuerySettingsKnobOverrides::fromBSON(
        BSON("totallyUnknownKnob" << 1 << "testIntKnobWire" << 0));
    ASSERT_THROWS_CODE(overrides.uassertNoErrors(), DBException, 12194501);
}

TEST(QuerySettingsKnobOverridesTest, MergeConcatenatesErrorsFromBothOperands) {
    auto lhs = QuerySettingsKnobOverrides::fromBSON(BSON("totallyUnknownKnob" << 1));
    auto rhs = QuerySettingsKnobOverrides::fromBSON(BSON("testIntKnobWire" << 0));
    auto result = QuerySettingsKnobOverrides::merge(lhs, rhs);
    ASSERT_EQ(result.errors().size(), 2u);
}

}  // namespace
}  // namespace mongo::query_settings
