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

#include "mongo/db/shard_role/shard_catalog/txn_wildcard_multikey_paths.h"

#include "mongo/db/service_context_test_fixture.h"
#include "mongo/db/shard_role/transaction_resources.h"
#include "mongo/db/storage/recovery_unit.h"
#include "mongo/db/storage/recovery_unit_noop.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

class TxnWildcardMultikeyPathsTest : public ServiceContextTest {
protected:
    void setUp() override {
        ServiceContextTest::setUp();
        _opCtx = makeOperationContext();
        shard_role_details::setRecoveryUnit(_opCtx.get(),
                                            std::make_unique<RecoveryUnitNoop>(),
                                            WriteUnitOfWork::RecoveryUnitState::kNotInUnitOfWork);
    }

    OperationContext* opCtx() {
        return _opCtx.get();
    }

    const UUID testUuid = UUID::gen();

private:
    ServiceContext::UniqueOperationContext _opCtx;
};

TEST_F(TxnWildcardMultikeyPathsTest, EmptyByDefault) {
    auto& cache = TxnWildcardMultikeyPaths::get(opCtx());
    ASSERT_TRUE(cache.empty());
}

TEST_F(TxnWildcardMultikeyPathsTest, AppendAndReadBack) {
    auto& cache = TxnWildcardMultikeyPaths::get(opCtx());
    cache.append(testUuid, "wc_idx", {FieldRef("a.b"_sd), FieldRef("c"_sd)});
    ASSERT_FALSE(cache.empty());

    std::set<FieldRef> out;
    cache.appendMatchingPaths(testUuid, "wc_idx", &out);
    ASSERT_EQ(out.size(), 2u);
    ASSERT_TRUE(out.contains(FieldRef("a.b"_sd)));
    ASSERT_TRUE(out.contains(FieldRef("c"_sd)));
}

TEST_F(TxnWildcardMultikeyPathsTest, IndexSegregationByName) {
    auto& cache = TxnWildcardMultikeyPaths::get(opCtx());
    cache.append(testUuid, "idx1", {FieldRef("a"_sd)});
    cache.append(testUuid, "idx2", {FieldRef("b"_sd)});

    std::set<FieldRef> out1;
    cache.appendMatchingPaths(testUuid, "idx1", &out1);
    ASSERT_EQ(out1.size(), 1u);
    ASSERT_TRUE(out1.contains(FieldRef("a"_sd)));

    std::set<FieldRef> out2;
    cache.appendMatchingPaths(testUuid, "idx2", &out2);
    ASSERT_EQ(out2.size(), 1u);
    ASSERT_TRUE(out2.contains(FieldRef("b"_sd)));
}

TEST_F(TxnWildcardMultikeyPathsTest, AppendIsAdditiveAndDeduplicates) {
    auto& cache = TxnWildcardMultikeyPaths::get(opCtx());
    cache.append(testUuid, "wc", {FieldRef("a"_sd)});
    cache.append(testUuid, "wc", {FieldRef("a"_sd), FieldRef("b"_sd)});

    std::set<FieldRef> out;
    cache.appendMatchingPaths(testUuid, "wc", &out);
    ASSERT_EQ(out.size(), 2u);
}

TEST_F(TxnWildcardMultikeyPathsTest, AppendMatchingPathsOnEmptyIsNoOp) {
    auto& cache = TxnWildcardMultikeyPaths::get(opCtx());
    std::set<FieldRef> out;
    cache.appendMatchingPaths(testUuid, "any", &out);
    ASSERT_TRUE(out.empty());
}

TEST_F(TxnWildcardMultikeyPathsTest, AppendMatchingPathsForUnknownIndexIsNoOp) {
    auto& cache = TxnWildcardMultikeyPaths::get(opCtx());
    cache.append(testUuid, "exists", {FieldRef("a"_sd)});

    std::set<FieldRef> out;
    cache.appendMatchingPaths(testUuid, "absent", &out);
    ASSERT_TRUE(out.empty());
}

TEST_F(TxnWildcardMultikeyPathsTest, AppendMatchingPathsReturnsAllCachedForIndex) {
    // appendMatchingPaths returns every cached path for the index. Over-reporting is safe
    // because multikey is monotonic — extra paths only force more conservative bounds.
    auto& cache = TxnWildcardMultikeyPaths::get(opCtx());
    cache.append(testUuid,
                 "wc",
                 {FieldRef("a"_sd),
                  FieldRef("a.b"_sd),
                  FieldRef("a.b.x"_sd),
                  FieldRef("a.c"_sd),
                  FieldRef("unrelated"_sd)});

    std::set<FieldRef> out;
    cache.appendMatchingPaths(testUuid, "wc", &out);
    ASSERT_EQ(out.size(), 5u);
}

TEST_F(TxnWildcardMultikeyPathsTest, AppendMatchingPathsPreservesPriorOutEntries) {
    // appendMatchingPaths must merge into `out`, not clear/replace it.
    auto& cache = TxnWildcardMultikeyPaths::get(opCtx());
    cache.append(testUuid, "wc", {FieldRef("a"_sd)});

    std::set<FieldRef> out;
    out.insert(FieldRef("preexisting.entry"_sd));
    cache.appendMatchingPaths(testUuid, "wc", &out);

    ASSERT_EQ(out.size(), 2u);
    ASSERT_TRUE(out.count(FieldRef("preexisting.entry"_sd)));
    ASSERT_TRUE(out.count(FieldRef("a"_sd)));
}

TEST_F(TxnWildcardMultikeyPathsTest, DeepPathsRoundtripThroughCache) {
    // Deep FieldRefs should be stored and returned without loss.
    auto& cache = TxnWildcardMultikeyPaths::get(opCtx());
    cache.append(testUuid, "wc", {FieldRef("a"_sd), FieldRef("a.b.c.d.e.f.g"_sd)});

    std::set<FieldRef> out;
    cache.appendMatchingPaths(testUuid, "wc", &out);
    ASSERT_EQ(out.size(), 2u);
    ASSERT_TRUE(out.count(FieldRef("a.b.c.d.e.f.g"_sd)));
}

TEST_F(TxnWildcardMultikeyPathsTest, AppendAfterAppendMatchingPathsIsVisible) {
    // Cache stays mutable across lookups; subsequent appends show up immediately.
    auto& cache = TxnWildcardMultikeyPaths::get(opCtx());
    cache.append(testUuid, "wc", {FieldRef("a"_sd)});

    std::set<FieldRef> firstOut;
    cache.appendMatchingPaths(testUuid, "wc", &firstOut);
    ASSERT_EQ(firstOut.size(), 1u);

    cache.append(testUuid, "wc", {FieldRef("a.late"_sd)});

    std::set<FieldRef> secondOut;
    cache.appendMatchingPaths(testUuid, "wc", &secondOut);
    ASSERT_EQ(secondOut.size(), 2u);
    ASSERT_TRUE(secondOut.count(FieldRef("a.late"_sd)));
}

TEST_F(TxnWildcardMultikeyPathsTest, DiesWithSnapshotOnAbandon) {
    {
        auto& cache = TxnWildcardMultikeyPaths::get(opCtx());
        cache.append(testUuid, "wc", {FieldRef("a"_sd)});
        ASSERT_FALSE(cache.empty());
    }

    shard_role_details::getRecoveryUnit(opCtx())->abandonSnapshot();

    auto& cacheAfter = TxnWildcardMultikeyPaths::get(opCtx());
    ASSERT_TRUE(cacheAfter.empty());
}

TEST_F(TxnWildcardMultikeyPathsTest, TryGetReturnsNullptrUntilGetIsCalled) {
    ASSERT_EQ(TxnWildcardMultikeyPaths::tryGet(opCtx()), nullptr);

    auto& cache = TxnWildcardMultikeyPaths::get(opCtx());
    cache.append(testUuid, "wc", {FieldRef("a"_sd)});

    const auto* tried = TxnWildcardMultikeyPaths::tryGet(opCtx());
    ASSERT_NE(tried, nullptr);
    ASSERT_FALSE(tried->empty());
}

TEST_F(TxnWildcardMultikeyPathsTest, TryGetIsNullptrAgainAfterAbandon) {
    TxnWildcardMultikeyPaths::get(opCtx()).append(testUuid, "wc", {FieldRef("a"_sd)});
    ASSERT_NE(TxnWildcardMultikeyPaths::tryGet(opCtx()), nullptr);

    shard_role_details::getRecoveryUnit(opCtx())->abandonSnapshot();

    ASSERT_EQ(TxnWildcardMultikeyPaths::tryGet(opCtx()), nullptr);
}

}  // namespace
}  // namespace mongo
