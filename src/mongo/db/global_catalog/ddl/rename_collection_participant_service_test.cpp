/**
 *    Copyright (C) 2021-present MongoDB, Inc.
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
        RenameParticipantInstance::_renameOrDropTarget(
            operationContext(), fromNss, toNss, options, sourceUUID, targetUUID);
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
 * TODO: SERVER-130940 Re-enable this test
 */
TEST_F(RenameCollectionParticipantServiceTest,
       DISABLED_RetryAfterCrossDatabaseRenameBeforeSourceDropDropsSource) {
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
