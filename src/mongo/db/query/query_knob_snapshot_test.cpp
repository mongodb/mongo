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

#include "mongo/db/query/query_knob_snapshot.h"

#include "mongo/db/query/query_execution_knobs_gen.h"
#include "mongo/unittest/assert.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/framework.h"

namespace mongo {
namespace {

TEST(QueryKnobSnapshotTest, SizeMatchesConstructorArgument) {
    auto snap = QueryKnobSnapshotBuilder{5}.build();
    ASSERT_EQ(snap.size(), 5u);
}

TEST(QueryKnobSnapshotTest, DefaultSourceIsDefault) {
    auto snap = QueryKnobSnapshotBuilder{3}.build();
    for (size_t i = 0; i < snap.size(); ++i) {
        ASSERT_EQ(snap.getSource(i), KnobSource::kDefault);
    }
}

TEST(QueryKnobSnapshotTest, RoundTripAllQueryKnobValueTypes) {
    // One slot per QueryKnobValue alternative plus enum (stored as int).
    // Slot 0: std::monostate — the null/removal sentinel; default-constructed slot holds it.
    // Slot 1: int
    // Slot 2: long long
    // Slot 3: double
    // Slot 4: bool
    // Slot 5: enum (QueryFrameworkControlEnum stored as int, retrieved via get<EnumT>)
    constexpr auto kEnumVal = QueryFrameworkControlEnum::kTrySbeRestricted;
    auto snap =
        std::move(QueryKnobSnapshotBuilder{6}
                      .set(1, QueryKnobValue{42}, KnobSource::kDefault)
                      .set(2, QueryKnobValue{123456789LL}, KnobSource::kDefault)
                      .set(3, QueryKnobValue{2.718}, KnobSource::kDefault)
                      .set(4, QueryKnobValue{true}, KnobSource::kDefault)
                      .set(5, QueryKnobValue{static_cast<int>(kEnumVal)}, KnobSource::kDefault))
            .build();

    ASSERT_EQ(snap.get<std::monostate>(0), std::monostate{});
    ASSERT_EQ(snap.get<int>(1), 42);
    ASSERT_EQ(snap.get<long long>(2), 123456789LL);
    ASSERT_APPROX_EQUAL(snap.get<double>(3), 2.718, 1e-9);
    ASSERT_EQ(snap.get<bool>(4), true);
    ASSERT_EQ(snap.get<QueryFrameworkControlEnum>(5), kEnumVal);
}

TEST(QueryKnobSnapshotTest, RoundTripInt) {
    auto snap =
        std::move(QueryKnobSnapshotBuilder{1}.set(0, QueryKnobValue{42}, KnobSource::kDefault))
            .build();
    ASSERT_EQ(snap.get<int>(0), 42);
}

TEST(QueryKnobSnapshotTest, RoundTripLongLong) {
    auto snap = std::move(QueryKnobSnapshotBuilder{1}.set(
                              0, QueryKnobValue{123456789LL}, KnobSource::kDefault))
                    .build();
    ASSERT_EQ(snap.get<long long>(0), 123456789LL);
}

TEST(QueryKnobSnapshotTest, RoundTripDouble) {
    auto snap =
        std::move(QueryKnobSnapshotBuilder{1}.set(0, QueryKnobValue{2.718}, KnobSource::kDefault))
            .build();
    ASSERT_APPROX_EQUAL(snap.get<double>(0), 2.718, 1e-9);
}

TEST(QueryKnobSnapshotTest, RoundTripBool) {
    auto snapFalse =
        std::move(QueryKnobSnapshotBuilder{1}.set(0, QueryKnobValue{false}, KnobSource::kDefault))
            .build();
    ASSERT_EQ(snapFalse.get<bool>(0), false);

    auto snapTrue =
        std::move(QueryKnobSnapshotBuilder{1}.set(0, QueryKnobValue{true}, KnobSource::kDefault))
            .build();
    ASSERT_EQ(snapTrue.get<bool>(0), true);
}

TEST(QueryKnobSnapshotTest, EnumCastRoundTrip) {
    auto enumVal = QueryFrameworkControlEnum::kForceClassicEngine;
    auto snap = std::move(QueryKnobSnapshotBuilder{1}.set(
                              0, QueryKnobValue{static_cast<int>(enumVal)}, KnobSource::kDefault))
                    .build();
    ASSERT_EQ(snap.get<QueryFrameworkControlEnum>(0), enumVal);
}

TEST(QueryKnobSnapshotTest, EnumCastAllValues) {
    auto snap =
        std::move(
            QueryKnobSnapshotBuilder{3}
                .set(0,
                     QueryKnobValue{static_cast<int>(QueryFrameworkControlEnum::kTrySbeEngine)},
                     KnobSource::kDefault)
                .set(1,
                     QueryKnobValue{static_cast<int>(QueryFrameworkControlEnum::kTrySbeRestricted)},
                     KnobSource::kDefault)
                .set(2,
                     QueryKnobValue{
                         static_cast<int>(QueryFrameworkControlEnum::kForceClassicEngine)},
                     KnobSource::kDefault))
            .build();
    ASSERT_EQ(snap.get<QueryFrameworkControlEnum>(0), QueryFrameworkControlEnum::kTrySbeEngine);
    ASSERT_EQ(snap.get<QueryFrameworkControlEnum>(1), QueryFrameworkControlEnum::kTrySbeRestricted);
    ASSERT_EQ(snap.get<QueryFrameworkControlEnum>(2),
              QueryFrameworkControlEnum::kForceClassicEngine);
}

TEST(QueryKnobSnapshotTest, SourceTrackingSetParameter) {
    auto snap =
        std::move(QueryKnobSnapshotBuilder{1}.set(0, QueryKnobValue{7}, KnobSource::kSetParameter))
            .build();
    ASSERT_EQ(snap.getSource(0), KnobSource::kSetParameter);
}

TEST(QueryKnobSnapshotTest, SourceTrackingQuerySettings) {
    auto snap =
        std::move(QueryKnobSnapshotBuilder{1}.set(0, QueryKnobValue{7}, KnobSource::kQuerySettings))
            .build();
    ASSERT_EQ(snap.getSource(0), KnobSource::kQuerySettings);
}

TEST(QueryKnobSnapshotTest, MultipleSlotSourcesAreIndependent) {
    auto snap = std::move(QueryKnobSnapshotBuilder{3}
                              .set(0, QueryKnobValue{1}, KnobSource::kDefault)
                              .set(1, QueryKnobValue{2}, KnobSource::kSetParameter)
                              .set(2, QueryKnobValue{3}, KnobSource::kQuerySettings))
                    .build();
    ASSERT_EQ(snap.getSource(0), KnobSource::kDefault);
    ASSERT_EQ(snap.getSource(1), KnobSource::kSetParameter);
    ASSERT_EQ(snap.getSource(2), KnobSource::kQuerySettings);
}

TEST(QueryKnobSnapshotTest, CopyIsIndependent) {
    auto original = std::move(QueryKnobSnapshotBuilder{2}
                                  .set(0, QueryKnobValue{10}, KnobSource::kDefault)
                                  .set(1, QueryKnobValue{20LL}, KnobSource::kSetParameter))
                        .build();

    // Copy-construct from original.
    QueryKnobSnapshot copy = original;
    ASSERT_EQ(copy.get<int>(0), 10);
    ASSERT_EQ(copy.get<long long>(1), 20LL);
    ASSERT_EQ(copy.getSource(0), KnobSource::kDefault);
    ASSERT_EQ(copy.getSource(1), KnobSource::kSetParameter);

    // Assigning a new snapshot to copy does not affect original.
    copy = std::move(QueryKnobSnapshotBuilder{2}
                         .set(0, QueryKnobValue{99}, KnobSource::kQuerySettings)
                         .set(1, QueryKnobValue{999LL}, KnobSource::kQuerySettings))
               .build();

    ASSERT_EQ(original.get<int>(0), 10);
    ASSERT_EQ(original.get<long long>(1), 20LL);
    ASSERT_EQ(original.getSource(0), KnobSource::kDefault);
    ASSERT_EQ(original.getSource(1), KnobSource::kSetParameter);
}

TEST(QueryKnobSnapshotTest, LastWriteWins) {
    auto snap = std::move(QueryKnobSnapshotBuilder{1}
                              .set(0, QueryKnobValue{5}, KnobSource::kDefault)
                              .set(0, QueryKnobValue{42}, KnobSource::kSetParameter))
                    .build();

    ASSERT_EQ(snap.get<int>(0), 42);
    ASSERT_EQ(snap.getSource(0), KnobSource::kSetParameter);
}

DEATH_TEST_REGEX(QueryKnobSnapshotDeathTest, GetOutOfBounds, "12312300") {
    QueryKnobSnapshotBuilder{2}.build().get<int>(2);
}

DEATH_TEST_REGEX(QueryKnobSnapshotDeathTest, GetSourceOutOfBounds, "12312301") {
    QueryKnobSnapshotBuilder{2}.build().getSource(2);
}

DEATH_TEST_REGEX(QueryKnobSnapshotDeathTest, SetOutOfBounds, "12312302") {
    QueryKnobSnapshotBuilder{2}.set(5, QueryKnobValue{1}, KnobSource::kDefault);
}

DEATH_TEST_REGEX(QueryKnobSnapshotDeathTest, SetMonostateValue, "12312303") {
    QueryKnobSnapshotBuilder{1}.set(0, QueryKnobValue{}, KnobSource::kDefault);
}

}  // namespace
}  // namespace mongo
