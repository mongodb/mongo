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

#include <boost/optional.hpp>

#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/catalog_raii.h"
#include "mongo/db/commands.h"
#include "mongo/db/field_parser.h"
#include "mongo/db/s/collection_sharding_runtime.h"
#include "mongo/db/s/migration_util.h"
#include "mongo/db/s/shard_filtering_metadata_refresh.h"
#include "mongo/db/s/sharding_runtime_d_params_gen.h"
#include "mongo/db/s/sharding_state.h"
#include "mongo/db/service_context.h"
#include "mongo/logv2/log.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kCommand

namespace mongo {
namespace {

enum class CleanupResult { kDone, kContinue, kError };

/**
 * Waits for all possibly orphaned ranges on 'nss' to be cleaned up.
 *
 * @return CleanupResult::kDone if no orphaned ranges remain
 * @return CleanupResult::kError and 'errMsg' if an error occurred
 *
 * If the collection is not sharded, returns CleanupResult::kDone.
 */
CleanupResult cleanupOrphanedData(OperationContext* opCtx,
                                  const NamespaceString& ns,
                                  const BSONObj& startingFromKeyConst,
                                  std::string* errMsg) {
    boost::optional<ChunkRange> range;
    boost::optional<UUID> collectionUuid;
    {
        AutoGetCollection autoColl(opCtx, ns, MODE_IX);
        if (!autoColl.getCollection()) {
            LOGV2(4416000,
                  "cleanupOrphaned skipping waiting for orphaned data cleanup because "
                  "{namespace} does not exist",
                  "cleanupOrphaned skipping waiting for orphaned data cleanup because "
                  "collection does not exist",
                  logAttrs(ns));
            return CleanupResult::kDone;
        }
        collectionUuid.emplace(autoColl.getCollection()->uuid());

        const auto scopedCsr =
            CollectionShardingRuntime::assertCollectionLockedAndAcquireShared(opCtx, ns);
        auto optCollDescr = scopedCsr->getCurrentMetadataIfKnown();
        if (!optCollDescr || !optCollDescr->isSharded()) {
            LOGV2(4416001,
                  "cleanupOrphaned skipping waiting for orphaned data cleanup because "
                  "{namespace} is not sharded",
                  "cleanupOrphaned skipping waiting for orphaned data cleanup because "
                  "collection is not sharded",
                  logAttrs(ns));
            return CleanupResult::kDone;
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
               migrationutil::checkForConflictingDeletions(opCtx, *range, *collectionUuid)) {
        uassert(ErrorCodes::ResumableRangeDeleterDisabled,
                "Failing cleanupOrphaned because the disableResumableRangeDeleter server parameter "
                "is set to true and this shard contains range deletion tasks for the collection.",
                !disableResumableRangeDeleter.load());

        LOGV2(4416003,
              "cleanupOrphaned going to wait for range deletion tasks to complete",
              logAttrs(ns),
              "collectionUUID"_attr = *collectionUuid,
              "numRemainingDeletionTasks"_attr = numRemainingDeletionTasks);

        auto status = CollectionShardingRuntime::waitForClean(
            opCtx, ns, *collectionUuid, *range, Date_t::max());

        if (!status.isOK()) {
            *errMsg = status.reason();
            return CleanupResult::kError;
        }

        opCtx->sleepFor(Milliseconds(1000));
    }

    return CleanupResult::kDone;
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
class CleanupOrphanedCommand : public ErrmsgCommandDeprecated {
public:
    CleanupOrphanedCommand() : ErrmsgCommandDeprecated("cleanupOrphaned") {}

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kNever;
    }

    bool adminOnly() const override {
        return true;
    }

    Status checkAuthForOperation(OperationContext* opCtx,
                                 const DatabaseName&,
                                 const BSONObj&) const override {
        if (!AuthorizationSession::get(opCtx->getClient())
                 ->isAuthorizedForActionsOnResource(ResourcePattern::forClusterResource(),
                                                    ActionType::cleanupOrphaned)) {
            return Status(ErrorCodes::Unauthorized, "Not authorized for cleanupOrphaned command.");
        }
        return Status::OK();
    }

    bool supportsWriteConcern(const BSONObj& cmd) const override {
        return true;
    }

    // Input
    static BSONField<std::string> nsField;
    static BSONField<BSONObj> startingFromKeyField;

    bool errmsgRun(OperationContext* opCtx,
                   std::string const& db,
                   const BSONObj& cmdObj,
                   std::string& errmsg,
                   BSONObjBuilder& result) override {
        std::string ns;
        if (!FieldParser::extract(cmdObj, nsField, &ns, &errmsg)) {
            return false;
        }

        const NamespaceString nss(ns);
        uassert(ErrorCodes::InvalidNamespace,
                str::stream() << "Invalid namespace: " << nss.toStringForErrorMsg(),
                nss.isValid());

        BSONObj startingFromKey;
        if (!FieldParser::extract(cmdObj, startingFromKeyField, &startingFromKey, &errmsg)) {
            return false;
        }

        ShardingState* const shardingState = ShardingState::get(opCtx);

        if (!shardingState->enabled()) {
            errmsg = str::stream() << "server is not part of a sharded cluster or "
                                   << "the sharding metadata is not yet initialized.";
            return false;
        }

        onCollectionPlacementVersionMismatch(opCtx, nss, boost::none);

        CleanupResult cleanupResult = cleanupOrphanedData(opCtx, nss, startingFromKey, &errmsg);

        if (cleanupResult == CleanupResult::kError) {
            return false;
        }
        dassert(cleanupResult == CleanupResult::kDone);

        return true;
    }

} cleanupOrphanedCmd;

BSONField<std::string> CleanupOrphanedCommand::nsField("cleanupOrphaned");
BSONField<BSONObj> CleanupOrphanedCommand::startingFromKeyField("startingFromKey");

}  // namespace
}  // namespace mongo
