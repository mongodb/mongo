// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/query_knobs/query_knob_snapshot.h"

#include "mongo/db/query/query_execution_knobs_gen.h"
#include "mongo/db/query/query_knobs/query_knob.h"
#include "mongo/db/query/query_knobs/query_knob_test_knobs.h"
#include "mongo/unittest/assert.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/framework.h"
#include "mongo/unittest/server_parameter_guard.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/scopeguard.h"

#include <chrono>
#include <cstddef>
#include <future>
#include <tuple>
#include <utility>

#include <boost/optional/optional.hpp>

namespace mongo {
namespace {

TEST(QueryKnobSnapshotTest, SizeMatchesConstructorArgument) {
    auto builder = QueryKnobSnapshotBuilder{5};
    for (QueryKnobId::value_t i = 0; i < 5; i++) {
        builder.set(QueryKnobId{i}, QueryKnobValue{static_cast<int>(i)}, KnobSource::kDefault);
    }
    ASSERT_EQ(std::move(builder).build().size(), 5u);
}

TEST(QueryKnobSnapshotTest, RoundTripAllQueryKnobValueTypes) {
    // One slot per QueryKnobValue alternative plus enum (stored as int).
    // Slot 0: int
    // Slot 1: long long
    // Slot 2: double
    // Slot 3: bool
    // Slot 4: enum (QueryFrameworkControlEnum stored as int, retrieved via get<EnumT>)
    constexpr auto kEnumVal = QueryFrameworkControlEnum::kTrySbeRestricted;
    auto snap =
        std::move(QueryKnobSnapshotBuilder{5}
                      .set(QueryKnobId{0}, QueryKnobValue{42}, KnobSource::kDefault)
                      .set(QueryKnobId{1}, QueryKnobValue{123456789LL}, KnobSource::kDefault)
                      .set(QueryKnobId{2}, QueryKnobValue{2.718}, KnobSource::kDefault)
                      .set(QueryKnobId{3}, QueryKnobValue{true}, KnobSource::kDefault)
                      .set(QueryKnobId{4},
                           QueryKnobValue{static_cast<int>(kEnumVal)},
                           KnobSource::kDefault))
            .build();

    ASSERT_EQ(snap.get<int>(QueryKnobId{0}), 42);
    ASSERT_EQ(snap.get<long long>(QueryKnobId{1}), 123456789LL);
    ASSERT_APPROX_EQUAL(snap.get<double>(QueryKnobId{2}), 2.718, 1e-9);
    ASSERT_EQ(snap.get<bool>(QueryKnobId{3}), true);
    ASSERT_EQ(snap.get<QueryFrameworkControlEnum>(QueryKnobId{4}), kEnumVal);
}

TEST(QueryKnobSnapshotTest, RoundTripInt) {
    auto snap = std::move(QueryKnobSnapshotBuilder{1}.set(
                              QueryKnobId{0}, QueryKnobValue{42}, KnobSource::kDefault))
                    .build();
    ASSERT_EQ(snap.get<int>(QueryKnobId{0}), 42);
}

TEST(QueryKnobSnapshotTest, RoundTripLongLong) {
    auto snap = std::move(QueryKnobSnapshotBuilder{1}.set(
                              QueryKnobId{0}, QueryKnobValue{123456789LL}, KnobSource::kDefault))
                    .build();
    ASSERT_EQ(snap.get<long long>(QueryKnobId{0}), 123456789LL);
}

TEST(QueryKnobSnapshotTest, RoundTripDouble) {
    auto snap = std::move(QueryKnobSnapshotBuilder{1}.set(
                              QueryKnobId{0}, QueryKnobValue{2.718}, KnobSource::kDefault))
                    .build();
    ASSERT_APPROX_EQUAL(snap.get<double>(QueryKnobId{0}), 2.718, 1e-9);
}

TEST(QueryKnobSnapshotTest, RoundTripBool) {
    auto snapFalse = std::move(QueryKnobSnapshotBuilder{1}.set(
                                   QueryKnobId{0}, QueryKnobValue{false}, KnobSource::kDefault))
                         .build();
    ASSERT_EQ(snapFalse.get<bool>(QueryKnobId{0}), false);

    auto snapTrue = std::move(QueryKnobSnapshotBuilder{1}.set(
                                  QueryKnobId{0}, QueryKnobValue{true}, KnobSource::kDefault))
                        .build();
    ASSERT_EQ(snapTrue.get<bool>(QueryKnobId{0}), true);
}

TEST(QueryKnobSnapshotTest, EnumCastRoundTrip) {
    auto enumVal = QueryFrameworkControlEnum::kForceClassicEngine;
    auto snap = std::move(QueryKnobSnapshotBuilder{1}.set(QueryKnobId{0},
                                                          QueryKnobValue{static_cast<int>(enumVal)},
                                                          KnobSource::kDefault))
                    .build();
    ASSERT_EQ(snap.get<QueryFrameworkControlEnum>(QueryKnobId{0}), enumVal);
}

TEST(QueryKnobSnapshotTest, EnumCastAllValues) {
    auto snap =
        std::move(
            QueryKnobSnapshotBuilder{3}
                .set(QueryKnobId{0},
                     QueryKnobValue{static_cast<int>(QueryFrameworkControlEnum::kTrySbeEngine)},
                     KnobSource::kDefault)
                .set(QueryKnobId{1},
                     QueryKnobValue{static_cast<int>(QueryFrameworkControlEnum::kTrySbeRestricted)},
                     KnobSource::kDefault)
                .set(QueryKnobId{2},
                     QueryKnobValue{
                         static_cast<int>(QueryFrameworkControlEnum::kForceClassicEngine)},
                     KnobSource::kDefault))
            .build();
    ASSERT_EQ(snap.get<QueryFrameworkControlEnum>(QueryKnobId{0}),
              QueryFrameworkControlEnum::kTrySbeEngine);
    ASSERT_EQ(snap.get<QueryFrameworkControlEnum>(QueryKnobId{1}),
              QueryFrameworkControlEnum::kTrySbeRestricted);
    ASSERT_EQ(snap.get<QueryFrameworkControlEnum>(QueryKnobId{2}),
              QueryFrameworkControlEnum::kForceClassicEngine);
}

TEST(QueryKnobSnapshotTest, SourceTrackingSetParameter) {
    auto snap = std::move(QueryKnobSnapshotBuilder{1}.set(
                              QueryKnobId{0}, QueryKnobValue{7}, KnobSource::kSetParameter))
                    .build();
    ASSERT_EQ(snap.getSource(QueryKnobId{0}), KnobSource::kSetParameter);
}

TEST(QueryKnobSnapshotTest, SourceTrackingQuerySettings) {
    auto snap = std::move(QueryKnobSnapshotBuilder{1}.set(
                              QueryKnobId{0}, QueryKnobValue{7}, KnobSource::kQuerySettings))
                    .build();
    ASSERT_EQ(snap.getSource(QueryKnobId{0}), KnobSource::kQuerySettings);
}

TEST(QueryKnobSnapshotTest, MultipleSlotSourcesAreIndependent) {
    auto snap = std::move(QueryKnobSnapshotBuilder{3}
                              .set(QueryKnobId{0}, QueryKnobValue{1}, KnobSource::kDefault)
                              .set(QueryKnobId{1}, QueryKnobValue{2}, KnobSource::kSetParameter)
                              .set(QueryKnobId{2}, QueryKnobValue{3}, KnobSource::kQuerySettings))
                    .build();
    ASSERT_EQ(snap.getSource(QueryKnobId{0}), KnobSource::kDefault);
    ASSERT_EQ(snap.getSource(QueryKnobId{1}), KnobSource::kSetParameter);
    ASSERT_EQ(snap.getSource(QueryKnobId{2}), KnobSource::kQuerySettings);
}

TEST(QueryKnobSnapshotTest, CopyIsIndependent) {
    auto original =
        std::move(QueryKnobSnapshotBuilder{2}
                      .set(QueryKnobId{0}, QueryKnobValue{10}, KnobSource::kDefault)
                      .set(QueryKnobId{1}, QueryKnobValue{20LL}, KnobSource::kSetParameter))
            .build();

    // Copy-construct from original.
    QueryKnobSnapshot copy = original;
    ASSERT_EQ(copy.get<int>(QueryKnobId{0}), 10);
    ASSERT_EQ(copy.get<long long>(QueryKnobId{1}), 20LL);
    ASSERT_EQ(copy.getSource(QueryKnobId{0}), KnobSource::kDefault);
    ASSERT_EQ(copy.getSource(QueryKnobId{1}), KnobSource::kSetParameter);

    // Assigning a new snapshot to copy does not affect original.
    copy = std::move(QueryKnobSnapshotBuilder{2}
                         .set(QueryKnobId{0}, QueryKnobValue{99}, KnobSource::kQuerySettings)
                         .set(QueryKnobId{1}, QueryKnobValue{999LL}, KnobSource::kQuerySettings))
               .build();

    ASSERT_EQ(original.get<int>(QueryKnobId{0}), 10);
    ASSERT_EQ(original.get<long long>(QueryKnobId{1}), 20LL);
    ASSERT_EQ(original.getSource(QueryKnobId{0}), KnobSource::kDefault);
    ASSERT_EQ(original.getSource(QueryKnobId{1}), KnobSource::kSetParameter);
}

TEST(QueryKnobSnapshotTest, LastWriteWins) {
    auto snap = std::move(QueryKnobSnapshotBuilder{1}
                              .set(QueryKnobId{0}, QueryKnobValue{5}, KnobSource::kDefault)
                              .set(QueryKnobId{0}, QueryKnobValue{42}, KnobSource::kSetParameter))
                    .build();

    ASSERT_EQ(snap.get<int>(QueryKnobId{0}), 42);
    ASSERT_EQ(snap.getSource(QueryKnobId{0}), KnobSource::kSetParameter);
}

DEATH_TEST_REGEX(QueryKnobSnapshotDeathTest, GetOutOfBounds, "12312300") {
    QueryKnobSnapshotBuilder{0}.build().get<int>(QueryKnobId{2});
}

DEATH_TEST_REGEX(QueryKnobSnapshotDeathTest, GetSourceOutOfBounds, "12312301") {
    std::move(
        QueryKnobSnapshotBuilder{1}.set(QueryKnobId{0}, QueryKnobValue{0}, KnobSource::kDefault))
        .build()
        .getSource(QueryKnobId{2});
}

DEATH_TEST_REGEX(QueryKnobSnapshotDeathTest, SetOutOfBounds, "12312302") {
    QueryKnobSnapshotBuilder{2}.set(QueryKnobId{5}, QueryKnobValue{1}, KnobSource::kDefault);
}

DEATH_TEST_REGEX(QueryKnobSnapshotDeathTest, SetDeleteQueryKnobOverrideValue, "12312303") {
    QueryKnobSnapshotBuilder{1}.set(
        QueryKnobId{0}, DeleteQueryKnobOverride(), KnobSource::kDefault);
}

DEATH_TEST_REGEX(QueryKnobSnapshotDeathTest,
                 UnsetQueryKnobValues,
                 "12611000.*invalid call to QueryKnobSnapshot::build\\(\\) with unset query knob "
                 "values") {
    std::ignore = QueryKnobSnapshotBuilder(1).build();
}

// Builds a snapshot of 'n' slots, all set to 0.
QueryKnobSnapshot makeSnapshot(size_t n) {
    QueryKnobSnapshotBuilder b{n};
    for (QueryKnobId::value_t i = 0; i < n; ++i) {
        b.set(QueryKnobId{i}, QueryKnobValue{0}, KnobSource::kDefault);
    }
    return std::move(b).build();
}

TEST(QueryKnobSnapshotCacheTest, GetSnapshotReturnsInitialValue) {
    QueryKnobSnapshotCache cache{makeSnapshot(1)};
    ASSERT_EQ(cache.getSnapshot().get<int>(QueryKnobId{0}), 0);
}

TEST(QueryKnobSnapshotCacheTest, UpdateDoesNotMutateSnapshotAlreadyHeld) {
    QueryKnobSnapshotCache cache{makeSnapshot(1)};
    auto before = cache.getSnapshot();
    cache.updateKnobValue(QueryKnobId{0}, QueryKnobValue{99}, KnobSource::kSetParameter);
    ASSERT_EQ(before.get<int>(QueryKnobId{0}), 0);
    ASSERT_EQ(before.getSource(QueryKnobId{0}), KnobSource::kDefault);
}

TEST(QueryKnobSnapshotCacheTest, SequentialUpdatesCompose) {
    QueryKnobSnapshotCache cache{makeSnapshot(2)};

    cache.updateKnobValue(QueryKnobId{0}, QueryKnobValue{42}, KnobSource::kSetParameter);
    {
        auto snap = cache.getSnapshot();
        ASSERT_EQ(snap.get<int>(QueryKnobId{0}), 42);
        ASSERT_EQ(snap.getSource(QueryKnobId{0}), KnobSource::kSetParameter);
        ASSERT_EQ(snap.get<int>(QueryKnobId{1}), 0);
        ASSERT_EQ(snap.getSource(QueryKnobId{1}), KnobSource::kDefault);
    }

    // A second update on the same slot must transition both the value and the source, while
    // leaving the untouched slot alone.
    cache.updateKnobValue(QueryKnobId{1}, QueryKnobValue{7}, KnobSource::kQuerySettings);
    {
        auto snap = cache.getSnapshot();
        ASSERT_EQ(snap.get<int>(QueryKnobId{0}), 42);
        ASSERT_EQ(snap.getSource(QueryKnobId{0}), KnobSource::kSetParameter);
        ASSERT_EQ(snap.get<int>(QueryKnobId{1}), 7);
        ASSERT_EQ(snap.getSource(QueryKnobId{1}), KnobSource::kQuerySettings);
    }
}

// Writer A pinned inside the intent-locked critical section must block writer B at the intent
// lock. After A is released, B reads A's installed snapshot as its base, so both updates are
// preserved (no lost update).
TEST(QueryKnobSnapshotCacheTest, WritersAreSerializedByIntentLock) {
    QueryKnobSnapshotCache cache{makeSnapshot(2)};

    boost::optional<FailPointEnableBlock> fp;
    fp.emplace("hangInQueryKnobSnapshotCacheUpdate");
    const auto initial = fp->initialTimesEntered();

    auto aFut = std::async(std::launch::async, [&] {
        cache.updateKnobValue(QueryKnobId{0}, QueryKnobValue{1}, KnobSource::kSetParameter);
    });
    (*fp)->waitForTimesEntered(initial + 1);

    auto bFut = std::async(std::launch::async, [&] {
        cache.updateKnobValue(QueryKnobId{1}, QueryKnobValue{2}, KnobSource::kSetParameter);
    });

    // While A is parked holding the intent lock, B must be blocked at the lock acquisition.
    ASSERT_EQ(bFut.wait_for(std::chrono::milliseconds(50)), std::future_status::timeout);

    fp.reset();
    aFut.get();
    bFut.get();

    auto snap = cache.getSnapshot();
    ASSERT_EQ(snap.get<int>(QueryKnobId{0}), 1);
    ASSERT_EQ(snap.get<int>(QueryKnobId{1}), 2);
}

TEST(QueryKnobSnapshotCacheTest, HeldReadLockBlocksWriterInstall) {
    QueryKnobSnapshotCache cache{makeSnapshot(1)};

    boost::optional<FailPointEnableBlock> fp;
    fp.emplace("hangInQueryKnobSnapshotCacheRead");
    const auto initial = fp->initialTimesEntered();

    auto readerFut = std::async(std::launch::async, [&] { std::ignore = cache.getSnapshot(); });
    (*fp)->waitForTimesEntered(initial + 1);

    auto writerFut = std::async(std::launch::async, [&] {
        cache.updateKnobValue(QueryKnobId{0}, QueryKnobValue{1}, KnobSource::kSetParameter);
    });
    ASSERT_EQ(writerFut.wait_for(std::chrono::milliseconds(50)), std::future_status::timeout);

    fp.reset();
    readerFut.get();
    writerFut.get();
    ASSERT_EQ(cache.getSnapshot().get<int>(QueryKnobId{0}), 1);
}

TEST(QueryKnobSnapshotCacheTest, WriterBuildPhaseDoesNotBlockReaders) {
    QueryKnobSnapshotCache cache{makeSnapshot(1)};
    cache.updateKnobValue(QueryKnobId{0}, QueryKnobValue{1}, KnobSource::kSetParameter);

    boost::optional<FailPointEnableBlock> fp;
    fp.emplace("hangInQueryKnobSnapshotCacheUpdate");
    const auto initial = fp->initialTimesEntered();

    auto writerFut = std::async(std::launch::async, [&] {
        cache.updateKnobValue(QueryKnobId{0}, QueryKnobValue{2}, KnobSource::kSetParameter);
    });
    (*fp)->waitForTimesEntered(initial + 1);

    // Writer is parked after building, before taking the write lock. A concurrent reader
    // must see the pre-write value without blocking.
    ASSERT_EQ(cache.getSnapshot().get<int>(QueryKnobId{0}), 1);

    fp.reset();
    writerFut.get();
    ASSERT_EQ(cache.getSnapshot().get<int>(QueryKnobId{0}), 2);
}

TEST(QueryKnobSnapshotCacheTest, ConcurrentReadersDontBlockEachOther) {
    QueryKnobSnapshotCache cache{makeSnapshot(1)};

    boost::optional<FailPointEnableBlock> fp;
    fp.emplace("hangInQueryKnobSnapshotCacheRead");
    const auto initial = fp->initialTimesEntered();

    auto readerAFut = std::async(std::launch::async, [&] { std::ignore = cache.getSnapshot(); });
    (*fp)->waitForTimesEntered(initial + 1);

    auto readerBFut = std::async(std::launch::async, [&] { std::ignore = cache.getSnapshot(); });
    (*fp)->waitForTimesEntered(initial + 2);

    fp.reset();
    readerAFut.get();
    readerBFut.get();
}

TEST(QueryKnobSnapshotCacheTest, SnapshotReflectsSetParameterUpdate) {
    ASSERT_EQ(QueryKnobSnapshotCache::instance().getSnapshot().get<int>(test_knobs::testIntKnob.id),
              42);
    {
        unittest::ServerParameterGuard ctrl("testIntKnob", 999);
        auto snap = QueryKnobSnapshotCache::instance().getThreadLocalSnapshot();
        ASSERT_EQ(snap.get<int>(test_knobs::testIntKnob.id), 999);
        ASSERT_EQ(snap.getSource(test_knobs::testIntKnob.id), KnobSource::kSetParameter);
    }
    auto snap = QueryKnobSnapshotCache::instance().getThreadLocalSnapshot();
    ASSERT_EQ(snap.get<int>(test_knobs::testIntKnob.id), 42);
}

TEST(QueryKnobSnapshotCacheTest, SnapshotReflectsSetParameterUpdateEnum) {
    ASSERT_EQ(QueryKnobSnapshotCache::instance().getSnapshot().get<TestKnobModeEnum>(
                  test_knobs::testEnumKnob.id),
              TestKnobModeEnum::kAlpha);
    {
        unittest::ServerParameterGuard ctrl("testEnumKnob", "beta");
        auto snap = QueryKnobSnapshotCache::instance().getThreadLocalSnapshot();
        ASSERT_EQ(snap.get<TestKnobModeEnum>(test_knobs::testEnumKnob.id), TestKnobModeEnum::kBeta);
        ASSERT_EQ(snap.getSource(test_knobs::testEnumKnob.id), KnobSource::kSetParameter);
    }
    auto snap = QueryKnobSnapshotCache::instance().getThreadLocalSnapshot();
    ASSERT_EQ(snap.get<TestKnobModeEnum>(test_knobs::testEnumKnob.id), TestKnobModeEnum::kAlpha);
}

// SERVER-130638: regression test for a stale thread-local cache after many knob updates.
// '_version' used to be an Atomic<uint8_t>, so exactly 256 knob updates would bring it back to
// the same value, and a thread whose cached snapshot matched the pre-wraparound value would see a
// false cache hit in getThreadLocalSnapshot() and never refresh. This drives 256 updates (far more
// than a uint8_t could survive) and confirms the thread-local cache still reflects the latest
// value.
//
// updateKnobValue() is only reachable from production code through the registered query-knobs
// listener, not through the public (const-only) instance() accessor, so this drives updates the
// same way ServerParameterGuard does: through the real ServerParameter::set() path.
TEST(QueryKnobSnapshotCacheTest, ThreadLocalCacheSurvivesManyUpdates) {
    const auto& cache = QueryKnobSnapshotCache::instance();

    const int initialValue = cache.getSnapshot().get<int>(test_knobs::testIntKnob.id);
    auto before = cache.getThreadLocalSnapshot();
    ASSERT_EQ(before.get<int>(test_knobs::testIntKnob.id), initialValue);

    auto* serverParam = ServerParameterSet::getNodeParameterSet()->getIfExists("testIntKnob");
    ASSERT(serverParam);
    auto setValue = [&](int value) {
        BSONObjBuilder b;
        b.append("testIntKnob", value);
        uassertStatusOK(serverParam->set(b.obj().firstElement(), boost::none));
    };

    ON_BLOCK_EXIT([&] { setValue(initialValue); });

    // 256 updates would have wrapped the old uint8_t version counter back to the value 'before'
    // observed. testIntKnob is validated to (0, 1000).
    for (int i = 0; i < 256; i++) {
        setValue(100 + i);
    }
    const int expectedCurrentValue = 100 + 255;
    ASSERT_EQ(cache.getSnapshot().get<int>(test_knobs::testIntKnob.id), expectedCurrentValue);

    auto after = cache.getThreadLocalSnapshot();
    ASSERT_EQ(after.get<int>(test_knobs::testIntKnob.id), expectedCurrentValue);
}

}  // namespace
}  // namespace mongo
