// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

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
