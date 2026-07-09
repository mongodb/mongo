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

#include "mongo/db/exec/single_doc_lookup/collection_acquirer.h"

#include "mongo/db/namespace_string.h"
#include "mongo/db/pipeline/shard_role_transaction_resources_stasher_for_pipeline.h"
#include "mongo/db/repl/storage_interface.h"
#include "mongo/db/shard_role/shard_catalog/catalog_test_fixture.h"
#include "mongo/db/shard_role/shard_catalog/collection_options.h"
#include "mongo/db/shard_role/shard_role.h"
#include "mongo/db/storage/recovery_unit.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"

#include <boost/optional/optional.hpp>

namespace mongo::exec::agg {
namespace {

const NamespaceString kTestNss =
    NamespaceString::createNamespaceString_forTest("test", "collection_acquirer");
const NamespaceString kOtherNss =
    NamespaceString::createNamespaceString_forTest("test", "other_collection");

class CollectionAcquirerTest : public CatalogTestFixture {
protected:
    void setUp() override {
        CatalogTestFixture::setUp();
        ASSERT_OK(storageInterface()->createCollection(
            operationContext(), kTestNss, CollectionOptions()));
    }

    // Acquires kTestNss for read (MODE_IS, lock-free) -- the same shape PreAcquired holds.
    CollectionAcquisition acquireTestCollection() {
        return acquireCollectionMaybeLockFree(
            operationContext(),
            CollectionAcquisitionRequest::fromOpCtx(
                operationContext(), kTestNss, AcquisitionPrerequisites::kRead));
    }

    // Acquires the collection, stashes the resulting TransactionResources into a fresh pipeline
    // stasher, and returns both. Mirrors how the pipeline hands an upfront acquisition to
    // PreAcquiredCollectionAcquirer.
    std::pair<boost::intrusive_ptr<ShardRoleTransactionResourcesStasherForPipeline>,
              CollectionAcquisition>
    stashTestCollection() {
        auto acquisition = acquireTestCollection();
        auto stasher = make_intrusive<ShardRoleTransactionResourcesStasherForPipeline>();
        stashTransactionResourcesFromOperationContext(operationContext(), stasher.get());
        return {std::move(stasher), std::move(acquisition)};
    }
};

using CollectionAcquirerDeathTest = CollectionAcquirerTest;

// --- assertLocalLookupReadAtOrAfter -----------------------------------------------------------

TEST_F(CollectionAcquirerTest, SnapshotGuardNoOpWhenAfterClusterTimeAbsent) {
    // No afterClusterTime: the guard never inspects the read snapshot.
    assertLocalLookupReadAtOrAfter(operationContext(), boost::none);
}

TEST_F(CollectionAcquirerTest, SnapshotGuardNoOpWhenReadUntimestamped) {
    // kNoTimestamp reads the latest committed data, trivially at or after the event.
    shard_role_details::getRecoveryUnit(operationContext())
        ->setTimestampReadSource(RecoveryUnit::ReadSource::kNoTimestamp);
    assertLocalLookupReadAtOrAfter(operationContext(), Timestamp(100, 1));
}

TEST_F(CollectionAcquirerTest, SnapshotGuardPassesWhenReadAtOrAfterEvent) {
    shard_role_details::getRecoveryUnit(operationContext())
        ->setTimestampReadSource(RecoveryUnit::ReadSource::kProvided, Timestamp(100, 1));
    // Read snapshot equals the event clusterTime -- allowed.
    assertLocalLookupReadAtOrAfter(operationContext(), Timestamp(100, 1));
    // Read snapshot strictly after the event clusterTime -- allowed.
    assertLocalLookupReadAtOrAfter(operationContext(), Timestamp(50, 1));
}

DEATH_TEST_REGEX_F(CollectionAcquirerDeathTest,
                   SnapshotGuardTassertsWhenReadPrecedesEvent,
                   "Tripwire assertion.*12841100") {
    shard_role_details::getRecoveryUnit(operationContext())
        ->setTimestampReadSource(RecoveryUnit::ReadSource::kProvided, Timestamp(50, 1));
    assertLocalLookupReadAtOrAfter(operationContext(), Timestamp(100, 1));
}

// --- OnDemandCollectionAcquirer ---------------------------------------------------------------

TEST_F(CollectionAcquirerTest, OnDemandAcquiresExistingCollection) {
    OnDemandCollectionAcquirer acquirer;
    auto handle = acquirer.acquireCollection(operationContext(), kTestNss, boost::none);
    ASSERT_TRUE(handle.exists());
    ASSERT_EQ(handle.nss(), kTestNss);
}

TEST_F(CollectionAcquirerTest, OnDemandReportsMissingCollectionAsNonExistent) {
    OnDemandCollectionAcquirer acquirer;
    auto handle = acquirer.acquireCollection(operationContext(), kOtherNss, boost::none);
    ASSERT_FALSE(handle.exists());
    ASSERT_EQ(handle.nss(), kOtherNss);
}

// --- PreAcquiredCollectionAcquirer ------------------------------------------------------------

TEST_F(CollectionAcquirerTest, PreAcquiredReturnsHeldAcquisitionForMatchingNss) {
    auto [stasher, acquisition] = stashTestCollection();
    auto expectedUuid = acquisition.uuid();
    PreAcquiredCollectionAcquirer acquirer(std::move(stasher), std::move(acquisition));

    auto handle = acquirer.acquireCollection(operationContext(), kTestNss, boost::none);
    ASSERT_TRUE(handle.exists());
    ASSERT_EQ(handle.nss(), kTestNss);
    ASSERT_EQ(handle.uuid(), expectedUuid);
}

DEATH_TEST_REGEX_F(CollectionAcquirerDeathTest,
                   PreAcquiredTassertsOnNullStasher,
                   "Tripwire assertion.*12841101") {
    auto acquisition = acquireTestCollection();
    PreAcquiredCollectionAcquirer acquirer(nullptr, std::move(acquisition));
}

DEATH_TEST_REGEX_F(CollectionAcquirerDeathTest,
                   PreAcquiredTassertsOnNssMismatch,
                   "Tripwire assertion.*12841102") {
    auto [stasher, acquisition] = stashTestCollection();
    PreAcquiredCollectionAcquirer acquirer(std::move(stasher), std::move(acquisition));
    acquirer.acquireCollection(operationContext(), kOtherNss, boost::none);
}

TEST_F(CollectionAcquirerTest, PreAcquiredAcceptsMatchingUuid) {
    auto [stasher, acquisition] = stashTestCollection();
    auto expectedUuid = acquisition.uuid();
    PreAcquiredCollectionAcquirer acquirer(std::move(stasher), std::move(acquisition));

    auto handle = acquirer.acquireCollection(operationContext(), kTestNss, expectedUuid);
    ASSERT_TRUE(handle.exists());
    ASSERT_EQ(handle.uuid(), expectedUuid);
}

TEST_F(CollectionAcquirerTest, PreAcquiredThrowsOnUuidMismatch) {
    auto [stasher, acquisition] = stashTestCollection();
    PreAcquiredCollectionAcquirer acquirer(std::move(stasher), std::move(acquisition));
    // A different UUID for the same nss (e.g. the namespace was dropped and recreated) must not be
    // silently served from the held acquisition, but must also not crash the server: this is a
    // real, expected runtime occurrence (a stale event racing a rename+recreate), not a
    // programming-contract violation, so it's a catchable CollectionUUIDMismatch, not a tassert.
    ASSERT_THROWS_CODE(acquirer.acquireCollection(operationContext(), kTestNss, UUID::gen()),
                       DBException,
                       ErrorCodes::CollectionUUIDMismatch);
}

}  // namespace
}  // namespace mongo::exec::agg
