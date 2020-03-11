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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kCommand

#include "mongo/platform/basic.h"

#include <boost/optional.hpp>
#include <string>

#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/privilege.h"
#include "mongo/db/catalog_raii.h"
#include "mongo/db/commands.h"
#include "mongo/db/field_parser.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/range_arithmetic.h"
#include "mongo/db/s/chunk_move_write_concern_options.h"
#include "mongo/db/s/collection_sharding_runtime.h"
#include "mongo/db/s/shard_filtering_metadata_refresh.h"
#include "mongo/db/s/sharding_state.h"
#include "mongo/db/service_context.h"
#include "mongo/logv2/log.h"
#include "mongo/s/request_types/migration_secondary_throttle_options.h"

namespace mongo {
namespace {

enum class CleanupResult { kDone, kContinue, kError };

/**
 * Cleans up one range of orphaned data starting from a range that overlaps or starts at
 * 'startingFromKey'.  If empty, startingFromKey is the minimum key of the sharded range.
 *
 * @return CleanupResult::kContinue and 'stoppedAtKey' if orphaned range was found and cleaned
 * @return CleanupResult::kDone if no orphaned ranges remain
 * @return CleanupResult::kError and 'errMsg' if an error occurred
 *
 * If the collection is not sharded, returns CleanupResult::kDone.
 */
CleanupResult cleanupOrphanedData(OperationContext* opCtx,
                                  const NamespaceString& ns,
                                  const BSONObj& startingFromKeyConst,
                                  BSONObj* stoppedAtKey,
                                  std::string* errMsg) {
    BSONObj startingFromKey = startingFromKeyConst;
    boost::optional<ChunkRange> targetRange;
    SharedSemiFuture<void> cleanupCompleteFuture;

    {
        AutoGetCollection autoColl(opCtx, ns, MODE_IX);
        auto* const css = CollectionShardingRuntime::get(opCtx, ns);
        const auto collDesc = css->getCollectionDescription();
        if (!collDesc.isSharded()) {
            LOGV2(21911,
                  "skipping orphaned data cleanup for {ns_ns}, collection is not sharded",
                  "ns_ns"_attr = ns.ns());
            return CleanupResult::kDone;
        }

        BSONObj keyPattern = collDesc.getKeyPattern();
        if (!startingFromKey.isEmpty()) {
            if (!collDesc.isValidKey(startingFromKey)) {
                *errMsg = str::stream()
                    << "could not cleanup orphaned data, start key " << startingFromKey
                    << " does not match shard key pattern " << keyPattern;

                LOGV2(21912, "{errMsg}", "errMsg"_attr = *errMsg);
                return CleanupResult::kError;
            }
        } else {
            startingFromKey = collDesc.getMinKey();
        }

        targetRange = css->getNextOrphanRange(startingFromKey);
        if (!targetRange) {
            LOGV2_DEBUG(21913,
                        1,
                        "cleanupOrphaned requested for {ns} starting from {startingFromKey}, no "
                        "orphan ranges remain",
                        "ns"_attr = ns.toString(),
                        "startingFromKey"_attr = redact(startingFromKey));

            return CleanupResult::kDone;
        }

        *stoppedAtKey = targetRange->getMax();

        cleanupCompleteFuture =
            css->cleanUpRange(*targetRange, boost::none, CollectionShardingRuntime::kNow);
    }

    // Sleep waiting for our own deletion. We don't actually care about any others, so there is no
    // need to call css::waitForClean() here.

    LOGV2_DEBUG(21914,
                1,
                "cleanupOrphaned requested for {ns} starting from {startingFromKey}, removing next "
                "orphan range {targetRange}; waiting...",
                "ns"_attr = ns.toString(),
                "startingFromKey"_attr = redact(startingFromKey),
                "targetRange"_attr = redact(targetRange->toString()));

    Status result = cleanupCompleteFuture.getNoThrow(opCtx);

    LOGV2_DEBUG(
        21915, 1, "Finished waiting for last {ns} orphan range cleanup", "ns"_attr = ns.toString());

    if (!result.isOK()) {
        LOGV2(21916, "{result_reason}", "result_reason"_attr = redact(result.reason()));
        *errMsg = result.reason();
        return CleanupResult::kError;
    }

    return CleanupResult::kContinue;
}

/**
 * Cleanup orphaned data command.  Called on a particular namespace, and if the collection
 * is sharded will clean up a single orphaned data range which overlaps or starts after a
 * passed-in 'startingFromKey'.  Returns true and a 'stoppedAtKey' (which will start a
 * search for the next orphaned range if the command is called again) or no key if there
 * are no more orphaned ranges in the collection.
 *
 * If the collection is not sharded, returns true but no 'stoppedAtKey'.
 * On failure, returns false and an error message.
 *
 * Calling this command repeatedly until no 'stoppedAtKey' is returned ensures that the
 * full collection range is searched for orphaned documents, but since sharding state may
 * change between calls there is no guarantee that all orphaned documents were found unless
 * the balancer is off.
 *
 * Safe to call with the balancer on.
 *
 * Format:
 *
 * {
 *      cleanupOrphaned: <ns>,
 *      // optional parameters:
 *      startingAtKey: { <shardKeyValue> }, // defaults to lowest value
 *      secondaryThrottle: <bool>, // defaults to true
 *      // defaults to { w: "majority", wtimeout: 60000 }. Applies to individual writes.
 *      writeConcern: { <writeConcern options> }
 * }
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

    Status checkAuthForCommand(Client* client,
                               const std::string& dbname,
                               const BSONObj& cmdObj) const override {
        if (!AuthorizationSession::get(client)->isAuthorizedForActionsOnResource(
                ResourcePattern::forClusterResource(), ActionType::cleanupOrphaned)) {
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

    // Output
    static BSONField<BSONObj> stoppedAtKeyField;

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
                str::stream() << "Invalid namespace: " << nss.ns(),
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

        forceShardFilteringMetadataRefresh(opCtx, nss, true /* forceRefreshFromThisThread */);

        BSONObj stoppedAtKey;
        CleanupResult cleanupResult =
            cleanupOrphanedData(opCtx, nss, startingFromKey, &stoppedAtKey, &errmsg);

        if (cleanupResult == CleanupResult::kError) {
            return false;
        }

        if (cleanupResult == CleanupResult::kContinue) {
            result.append(stoppedAtKeyField(), stoppedAtKey);
        } else {
            dassert(cleanupResult == CleanupResult::kDone);
        }

        return true;
    }

} cleanupOrphanedCmd;

BSONField<std::string> CleanupOrphanedCommand::nsField("cleanupOrphaned");
BSONField<BSONObj> CleanupOrphanedCommand::startingFromKeyField("startingFromKey");
BSONField<BSONObj> CleanupOrphanedCommand::stoppedAtKeyField("stoppedAtKey");

}  // namespace
}  // namespace mongo
