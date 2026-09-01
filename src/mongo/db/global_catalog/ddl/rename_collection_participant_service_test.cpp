// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/global_catalog/ddl/rename_collection_participant_service.h"

#include "mongo/bson/bsonmisc.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/shard_role/lock_manager/d_concurrency.h"
#include "mongo/db/shard_role/shard_catalog/collection_catalog.h"
#include "mongo/db/shard_role/shard_catalog/rename_collection.h"
#include "mongo/db/sharding_environment/shard_server_test_fixture.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/uuid.h"

#include <boost/none.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {

class RenameCollectionParticipantServiceTest : public ShardServerTestFixture,
                                               public testing::WithParamInterface<bool> {
protected:
    boost::optional<UUID> getCollectionUuid(const NamespaceString& nss) {
        auto* opCtx = operationContext();
        Lock::DBLock dbLock(opCtx, nss.dbName(), MODE_IS);
        Lock::CollectionLock collLock(opCtx, nss, MODE_IS);
        const auto collection =
            CollectionCatalog::get(opCtx)->lookupCollectionByNamespace(opCtx, nss);
        if (!collection) {
            return boost::none;
        }
        return collection->uuid();
    }

    void renameOrDropTarget(const NamespaceString& fromNss,
                            const NamespaceString& toNss,
                            const RenameCollectionOptions& options,
                            const UUID& sourceUUID,
                            const boost::optional<UUID>& targetUUID) {
        RenameParticipantInstance::_renameOrDropTarget(operationContext(),
                                                       fromNss,
                                                       toNss,
                                                       options,
                                                       sourceUUID,
                                                       targetUUID,
                                                       false /* isNonAuthoritative */);
    }

    NamespaceString makeTargetNss(const NamespaceString& sourceNss) const {
        const bool crossDb = GetParam();
        return crossDb
            ? NamespaceString::createNamespaceString_forTest("renameParticipantTarget", "target")
            : NamespaceString::createNamespaceString_forTest(sourceNss.dbName(), "target");
    }
};

INSTANTIATE_TEST_SUITE_P(CrossAndWithinDBRename,
                         RenameCollectionParticipantServiceTest,
                         testing::Values(false,
                                         true),  // false = same db rename, true = cross db rename
                         [](const testing::TestParamInfo<bool>& info) {
                             return info.param ? "CrossDbRename" : "WithinDbRename";
                         });

/**
 * This test covers a case where rename was successful and the source collection was deleted but
 * the coordinator failed to checkpoint that kPhase before it died (step down). When this phase is
 * retried, we need to ensure that the target collection is NOT dropped (SERVER-131805).
 */
TEST_P(RenameCollectionParticipantServiceTest,
       RetryAfterCompletedCrossDatabaseRenamePreservesTarget) {
    const auto sourceNss =
        NamespaceString::createNamespaceString_forTest("renameParticipantSource", "source");
    const auto targetNss = makeTargetNss(sourceNss);

    createTestCollection(operationContext(), targetNss);
    DBDirectClient client(operationContext());
    client.insert(targetNss, BSON("_id" << 1));

    // Post-rename UUID currently on disk at targetNss.
    const auto targetUUID = getCollectionUuid(targetNss);
    ASSERT(targetUUID);

    // Coordinator source UUID. The source collection no longer exists locally.
    const auto sourceUUID = GetParam() ? UUID::gen() : *targetUUID;

    RenameCollectionOptions options;
    // The rename already completed: the target on disk has this UUID.
    options.newTargetCollectionUuid = *targetUUID;

    renameOrDropTarget(sourceNss, targetNss, options, sourceUUID, boost::none);

    const auto targetUUIDAfterRetry = getCollectionUuid(targetNss);
    ASSERT(targetUUIDAfterRetry)
        << "The recovered participant dropped the already-renamed cross-database target";
    ASSERT_EQ(*targetUUID, *targetUUIDAfterRetry);
    ASSERT_EQ(1, client.count(targetNss));
}

/**
 * Same scenario as RetryAfterCompletedCrossDatabaseRenamePreservesTarget, but with dropTarget=true
 * and a stale coordinator targetUUID that does not match newTargetCollectionUuid. Without the
 * targetHasNewUuid uassert exemption, retry would fail with 5807602 even though the rename already
 * completed (SERVER-131863).
 */
TEST_P(RenameCollectionParticipantServiceTest,
       RetryAfterCompletedCrossDatabaseRenameWithStaleTargetUUIDPreservesTarget) {
    const auto sourceNss =
        NamespaceString::createNamespaceString_forTest("renameParticipantSource", "source");
    const auto targetNss = makeTargetNss(sourceNss);

    createTestCollection(operationContext(), targetNss);
    DBDirectClient client(operationContext());
    client.insert(targetNss, BSON("_id" << 1));

    // Post-rename UUID currently on disk at targetNss.
    const auto targetUUID = getCollectionUuid(targetNss);
    ASSERT(targetUUID);

    // Coordinator source UUID. The source collection no longer exists locally.
    const auto sourceUUID = GetParam() ? UUID::gen() : *targetUUID;

    // Pre-rename target UUID persisted in the participant doc. It no longer matches the
    // collection on disk after the rename completed.
    auto originalTargetUUID = UUID::gen();

    RenameCollectionOptions options;
    options.dropTarget = true;
    // The rename already completed: the target on disk has this UUID.
    options.newTargetCollectionUuid = *targetUUID;

    renameOrDropTarget(sourceNss, targetNss, options, sourceUUID, originalTargetUUID);

    const auto targetUUIDAfterRetry = getCollectionUuid(targetNss);
    ASSERT(targetUUIDAfterRetry)
        << "The recovered participant dropped the already-renamed cross-database target";
    ASSERT_EQ(*targetUUID, *targetUUIDAfterRetry);
    ASSERT_EQ(1, client.count(targetNss));
}

/**
 * Similar test as RetryAfterCompletedCrossDatabaseRenamePreservesTarget with one key difference:
 * the rename completed but the source was not dropped. The test ensures that after the retry
 * source gets dropped and the target stays intact.
 * NOTE: This case is only applicable for cross-db rename.
 */
TEST_F(RenameCollectionParticipantServiceTest,
       RetryAfterCrossDatabaseRenameBeforeSourceDropDropsSource) {
    const auto sourceNss =
        NamespaceString::createNamespaceString_forTest("renameParticipantSource", "source");
    const auto targetNss =
        NamespaceString::createNamespaceString_forTest("renameParticipantTarget", "target");
    createTestCollection(operationContext(), sourceNss);
    createTestCollection(operationContext(), targetNss);
    DBDirectClient client(operationContext());
    client.insert(sourceNss, BSON("_id" << 1));
    client.insert(targetNss, BSON("_id" << 1));
    // Source still exists locally and must be dropped on retry.
    const auto sourceUUID = getCollectionUuid(sourceNss);
    // Target already has the post-rename UUID assigned by the coordinator.
    const auto targetUUID = getCollectionUuid(targetNss);
    ASSERT(sourceUUID);
    ASSERT(targetUUID);
    RenameCollectionOptions options;
    // The rename already completed on the target, but the source still needs to be dropped.
    options.newTargetCollectionUuid = *targetUUID;
    renameOrDropTarget(sourceNss, targetNss, options, *sourceUUID, boost::none);
    ASSERT_FALSE(getCollectionUuid(sourceNss))
        << "The recovered participant did not drop the source collection";
    const auto targetUUIDAfterRetry = getCollectionUuid(targetNss);
    ASSERT(targetUUIDAfterRetry);
    ASSERT_EQ(*targetUUID, *targetUUIDAfterRetry);
    ASSERT_EQ(1, client.count(targetNss));
}

}  // namespace mongo
