/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bson_field.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/resource_pattern.h"
#include "mongo/db/commands.h"
#include "mongo/db/database_name.h"
#include "mongo/db/field_parser.h"
#include "mongo/db/global_catalog/type_chunk.h"
#include "mongo/db/local_catalog/catalog_raii.h"
#include "mongo/db/local_catalog/collection.h"
#include "mongo/db/local_catalog/lock_manager/lock_manager_defs.h"
#include "mongo/db/local_catalog/shard_role_catalog/collection_metadata.h"
#include "mongo/db/local_catalog/shard_role_catalog/collection_sharding_runtime.h"
#include "mongo/db/local_catalog/shard_role_catalog/shard_filtering_metadata_refresh.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/s/range_deletion_util.h"
#include "mongo/db/service_context.h"
#include "mongo/db/topology/sharding_state.h"
#include "mongo/db/versioning_protocol/chunk_version.h"
#include "mongo/logv2/log.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/s/request_types/cleanup_orphaned_gen.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/duration.h"
#include "mongo/util/str.h"
#include "mongo/util/time_support.h"
#include "mongo/util/uuid.h"

#include <string>

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kCommand

namespace mongo {
namespace {

/**
 * Waits for all possibly orphaned ranges on 'nss' to be cleaned up.
 *
 * @return Status::OK() if no orphaned ranges remain
 * @return ErrorCode::OrphanedRangeCleanUpFailed and 'errMsg' if an error occurred
 *
 * If the collection is not sharded, returns Status::OK().
 */
Status cleanupOrphanedData(OperationContext* opCtx,
                           const NamespaceString& ns,
                           const BSONObj& startingFromKeyConst) {
    boost::optional<ChunkRange> range;
    boost::optional<UUID> collectionUuid;
    {
        AutoGetCollection autoColl(opCtx, ns, MODE_IX);
        if (!autoColl.getCollection()) {
            LOGV2(4416000,
                  "cleanupOrphaned skipping waiting for orphaned data cleanup because "
                  "collection does not exist",
                  logAttrs(ns));
            return Status::OK();
        }
        collectionUuid.emplace(autoColl.getCollection()->uuid());

        const auto scopedCsr =
            CollectionShardingRuntime::assertCollectionLockedAndAcquireShared(opCtx, ns);
        auto optCollDescr = scopedCsr->getCurrentMetadataIfKnown();
        if (!optCollDescr || !optCollDescr->isSharded()) {
            LOGV2(4416001,
                  "cleanupOrphaned skipping waiting for orphaned data cleanup because "
                  "collection is not sharded",
                  logAttrs(ns));
            return Status::OK();
        }
        range.emplace(optCollDescr->getMinKey(), optCollDescr->getMaxKey());

        // Though the 'startingFromKey' parameter is not used as the min key of the range to
        // wait for, we still validate that 'startingFromKey' in the same way as the original
        // cleanupOrphaned logic did if 'startingFromKey' is present.
        BSONObj keyPattern = optCollDescr->getKeyPattern();
        if (!startingFromKeyConst.isEmpty() && !optCollDescr->isValidKey(startingFromKeyConst)) {
            LOGV2_ERROR_OPTIONS(
                4416002,
                {logv2::UserAssertAfterLog(ErrorCodes::OrphanedRangeCleanUpFailed)},
                "Could not cleanup orphaned data because start key does not match shard key "
                "pattern",
                "startKey"_attr = redact(startingFromKeyConst),
                "shardKeyPattern"_attr = keyPattern);
        }
    }

    // We actually want to wait until there are no range deletion tasks for this namespace/UUID,
    // but we don't have a good way to wait for that event, so instead we wait for there to be
    // no tasks being processed in memory for this namespace/UUID.
    // However, it's possible this node has recently stepped up, and the stepup recovery task to
    // resubmit range deletion tasks for processing has not yet completed. In that case,
    // waitForClean will return though there are still tasks in config.rangeDeletions, so we
    // sleep for a short time and then try waitForClean again.
    while (auto numRemainingDeletionTasks =
               rangedeletionutil::checkForConflictingDeletions(opCtx, *range, *collectionUuid)) {
        LOGV2(4416003,
              "cleanupOrphaned going to wait for range deletion tasks to complete",
              logAttrs(ns),
              "collectionUUID"_attr = *collectionUuid,
              "numRemainingDeletionTasks"_attr = numRemainingDeletionTasks);

        auto status = CollectionShardingRuntime::waitForClean(
            opCtx, ns, *collectionUuid, *range, Date_t::max());

        if (!status.isOK()) {
            std::string errMsg = status.reason();
            return Status(ErrorCodes::OrphanedRangeCleanUpFailed, errMsg);
        }

        opCtx->sleepFor(Milliseconds(1000));
    }

    return Status::OK();
}

/**
 * Called on a particular namespace, and if the collection is sharded will wait for the number of
 * range deletion tasks on the collection on this shard to reach zero.
 *
 * Since the sharding state may change after this call returns, there is no guarantee that orphans
 * won't re-appear as a result of migrations that commit after this call returns.
 *
 * Safe to call with the balancer on.
 */
class CleanupOrphanedCommand final : public TypedCommand<CleanupOrphanedCommand> {

public:
    using Request = CleanupOrphaned;

    class Invocation final : public InvocationBase {
    public:
        using InvocationBase::InvocationBase;

        void typedRun(OperationContext* opCtx) {
            const NamespaceString& nss = ns();
            uassert(ErrorCodes::InvalidNamespace,
                    str::stream() << "Invalid namespace: " << nss.toStringForErrorMsg(),
                    nss.isValid());

            ShardingState* const shardingState = ShardingState::get(opCtx);

            uassert(ErrorCodes::ShardingStateNotInitialized,
                    str::stream() << "server is not part of a sharded cluster or the sharding "
                                     "metadata is not yet initialized. ",
                    shardingState->enabled());

            uassertStatusOK(
                FilteringMetadataCache::get(opCtx)->onCollectionPlacementVersionMismatch(
                    opCtx, nss, boost::none));

            BSONObj startingFromKey;

            if (request().getStartingFromKey().has_value()) {
                startingFromKey = request().getStartingFromKey().value();
            }

            Status cleanupResult = cleanupOrphanedData(opCtx, nss, startingFromKey);

            uassertStatusOK(cleanupResult);
        }

    private:
        void doCheckAuthorization(OperationContext* opCtx) const override {
            uassert(ErrorCodes::Unauthorized,
                    "Unauthorized",
                    AuthorizationSession::get(opCtx->getClient())
                        ->isAuthorizedForActionsOnResource(
                            ResourcePattern::forClusterResource(request().getDbName().tenantId()),
                            ActionType::cleanupOrphaned));
        }

        bool supportsWriteConcern() const override {
            return true;
        }

        NamespaceString ns() const override {
            return request().getCommandParameter();
        }
    };

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kNever;
    }

    bool adminOnly() const override {
        return true;
    }
};
MONGO_REGISTER_COMMAND(CleanupOrphanedCommand).forShard();

}  // namespace
}  // namespace mongo
