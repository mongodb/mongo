/**
 *    Copyright (C) 2015 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects
 *    for all of the code used other than as permitted herein. If you modify
 *    file(s) with this exception, you may extend this exception to your
 *    version of the file(s), but you are not obligated to do so. If you do not
 *    wish to do so, delete this exception statement from your version. If you
 *    delete this exception statement from all source files in the program,
 *    then also delete it in the license file.
 */

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kSharding

#include "mongo/platform/basic.h"

#include "mongo/client/connpool.h"
#include "mongo/db/auth/action_set.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_manager.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/client.h"
#include "mongo/db/commands.h"
#include "mongo/db/range_deleter_service.h"
#include "mongo/db/s/collection_metadata.h"
#include "mongo/db/s/migration_impl.h"
#include "mongo/db/s/sharding_state.h"
#include "mongo/logger/ramlog.h"
#include "mongo/s/catalog/type_chunk.h"
#include "mongo/s/client/shard_connection.h"
#include "mongo/s/chunk_version.h"
#include "mongo/s/grid.h"
#include "mongo/util/fail_point_service.h"
#include "mongo/util/log.h"

namespace mongo {

using std::string;
using str::stream;

namespace {

// Tests can pause and resume moveChunk's progress at each step by enabling/disabling each failpoint
MONGO_FP_DECLARE(moveChunkHangAtStep1);
MONGO_FP_DECLARE(moveChunkHangAtStep2);
MONGO_FP_DECLARE(moveChunkHangAtStep3);
MONGO_FP_DECLARE(moveChunkHangAtStep4);
MONGO_FP_DECLARE(moveChunkHangAtStep5);
MONGO_FP_DECLARE(moveChunkHangAtStep6);

Tee* const migrateLog = RamLog::get("migrate");

/**
 * This is the main entry for moveChunk, which is called to initiate a move by a donor side. It can
 * be called by either mongos as a result of a user request or an automatic balancing action.
 *
 * Format:
 * {
 *   moveChunk: "namespace",
 *   from: "hostAndPort",
 *   fromShard: "shardName",
 *   to: "hostAndPort",
 *   toShard: "shardName",
 *   min: {},
 *   max: {},
 *   maxChunkBytes: numeric,
 *   configdb: "hostAndPort",
 *
 *   // optional
 *   secondaryThrottle: bool, //defaults to true.
 *   writeConcern: {} // applies to individual writes.
 * }
 *
 */
class MoveChunkCommand : public Command {
public:
    MoveChunkCommand() : Command("moveChunk") {}

    void help(std::stringstream& help) const override {
        help << "should not be calling this directly";
    }

    bool slaveOk() const override {
        return false;
    }

    bool adminOnly() const override {
        return true;
    }

    bool isWriteCommandForConfigServer() const override {
        return false;
    }

    Status checkAuthForCommand(ClientBasic* client,
                               const std::string& dbname,
                               const BSONObj& cmdObj) override {
        if (!AuthorizationSession::get(client)->isAuthorizedForActionsOnResource(
                ResourcePattern::forExactNamespace(NamespaceString(parseNs(dbname, cmdObj))),
                ActionType::moveChunk)) {
            return Status(ErrorCodes::Unauthorized, "Unauthorized");
        }
        return Status::OK();
    }

    std::string parseNs(const std::string& dbname, const BSONObj& cmdObj) const override {
        return parseNsFullyQualified(dbname, cmdObj);
    }

    bool run(OperationContext* txn,
             const string& dbname,
             BSONObj& cmdObj,
             int options,
             std::string& errmsg,
             BSONObjBuilder& result) override {
        // 1. Parse options
        // 2. Make sure my view is complete and lock the distributed lock to ensure shard
        //    metadata stability.
        // 3. Migration
        //    Retrieve all RecordIds, which need to be migrated in order to do as little seeking
        //    as possible during transfer. Retrieval of the RecordIds happens under a collection
        //    lock, but then the collection lock is dropped. This opens up an opportunity for
        //    repair or compact to invalidate these RecordIds, because these commands do not
        //    synchronized with migration. Note that data modifications are not a problem,
        //    because we are registered for change notifications.
        // 4. Pause till migrate is caught up
        // 5. LOCK (critical section)
        //    a) update my config, essentially locking
        //    b) finish migrate
        //    c) update config server
        //    d) logChange to config server
        // 6. Wait for all current cursors to expire
        // 7. Remove data locally

        // -------------------------------

        // 1.
        const string ns = parseNs(dbname, cmdObj);
        if (ns.empty()) {
            return appendCommandStatus(
                result, Status(ErrorCodes::InvalidOptions, "need to specify namespace in command"));
        }

        ShardingState* const shardingState = ShardingState::get(txn);

        // This could be the first call that enables sharding - make sure we initialize the
        // sharding state for this shard.
        if (!shardingState->enabled()) {
            if (cmdObj["configdb"].type() != String) {
                const string msg = "sharding not enabled";
                warning() << msg;
                return appendCommandStatus(result, Status(ErrorCodes::IllegalOperation, msg));
            }

            const string configdb = cmdObj["configdb"].String();
            shardingState->initialize(txn, configdb);
        }

        ChunkMoveOperationState chunkMoveState{txn, NamespaceString(ns)};
        uassertStatusOK(chunkMoveState.initialize(cmdObj));

        // Initialize our current shard name in the shard state if needed
        shardingState->setShardName(chunkMoveState.getFromShard());

        const auto moveWriteConcernOptions =
            uassertStatusOK(ChunkMoveWriteConcernOptions::initFromCommand(cmdObj));
        const auto& secThrottleObj = moveWriteConcernOptions.getSecThrottle();
        const auto& writeConcern = moveWriteConcernOptions.getWriteConcern();

        // Do inline deletion
        bool waitForDelete = cmdObj["waitForDelete"].trueValue();
        if (waitForDelete) {
            log() << "moveChunk waiting for full cleanup after move";
        }

        BSONElement maxSizeElem = cmdObj["maxChunkSizeBytes"];
        if (maxSizeElem.eoo() || !maxSizeElem.isNumber()) {
            return appendCommandStatus(
                result, Status(ErrorCodes::InvalidOptions, "need to specify maxChunkSizeBytes"));
        }

        const long long maxChunkSizeBytes = maxSizeElem.numberLong();

        MoveTimingHelper timing(txn,
                                "from",
                                ns,
                                chunkMoveState.getMinKey(),
                                chunkMoveState.getMaxKey(),
                                6,  // Total number of steps
                                &errmsg,
                                chunkMoveState.getToShard(),
                                chunkMoveState.getFromShard());

        log() << "received moveChunk request: " << cmdObj << migrateLog;

        timing.done(1);

        MONGO_FAIL_POINT_PAUSE_WHILE_SET(moveChunkHangAtStep1);

        // 2.

        if (shardingState->migrationSourceManager()->isActive()) {
            const std::string msg =
                "Not starting chunk migration because another migration is already in progress";
            warning() << msg;
            return appendCommandStatus(result,
                                       Status(ErrorCodes::ConflictingOperationInProgress, msg));
        }

        auto distLock = uassertStatusOK(chunkMoveState.acquireMoveMetadata());

        BSONObj chunkInfo = BSON(
            "min" << chunkMoveState.getMinKey() << "max" << chunkMoveState.getMaxKey() << "from"
                  << chunkMoveState.getFromShard() << "to" << chunkMoveState.getToShard());

        grid.catalogManager(txn)->logChange(txn, "moveChunk.start", ns, chunkInfo);

        const auto origCollMetadata = chunkMoveState.getCollMetadata();
        BSONObj shardKeyPattern = origCollMetadata->getKeyPattern();

        log() << "moveChunk request accepted at version " << chunkMoveState.getShardVersion();

        timing.done(2);

        Status distLockStatus = distLock->checkForPendingCatalogSwap();
        if (!distLockStatus.isOK()) {
            warning() << "Aborting migration due to need to swap current catalog manager"
                      << causedBy(distLockStatus);
            return appendCommandStatus(result, distLockStatus);
        }

        MONGO_FAIL_POINT_PAUSE_WHILE_SET(moveChunkHangAtStep2);

        // 3.

        auto moveChunkStartStatus = chunkMoveState.start(shardKeyPattern);

        if (!moveChunkStartStatus.isOK()) {
            warning() << moveChunkStartStatus.toString();
            return appendCommandStatus(result, moveChunkStartStatus);
        }

        {
            // See comment at the top of the function for more information on what kind of
            // synchronization is used here.
            if (!shardingState->migrationSourceManager()->storeCurrentLocs(
                    txn, maxChunkSizeBytes, errmsg, result)) {
                warning() << errmsg;
                return false;
            }

            const bool isSecondaryThrottle(writeConcern.shouldWaitForOtherNodes());

            BSONObjBuilder recvChunkStartBuilder;
            recvChunkStartBuilder.append("_recvChunkStart", ns);
            recvChunkStartBuilder.append("from", chunkMoveState.getFromShardCS().toString());
            recvChunkStartBuilder.append("fromShardName", chunkMoveState.getFromShard());
            recvChunkStartBuilder.append("toShardName", chunkMoveState.getToShard());
            recvChunkStartBuilder.append("min", chunkMoveState.getMinKey());
            recvChunkStartBuilder.append("max", chunkMoveState.getMaxKey());
            recvChunkStartBuilder.append("shardKeyPattern", shardKeyPattern);
            recvChunkStartBuilder.append("configServer",
                                         shardingState->getConfigServer(txn).toString());
            recvChunkStartBuilder.append("secondaryThrottle", isSecondaryThrottle);

            // Follow the same convention in moveChunk.
            if (isSecondaryThrottle && !secThrottleObj.isEmpty()) {
                recvChunkStartBuilder.append("writeConcern", secThrottleObj);
            }

            BSONObj res;

            try {
                // Use ShardConnection even though this operation isn't versioned to ensure that
                // the ConfigServerMetadata gets sent along with the command.
                ShardConnection connTo(chunkMoveState.getToShardCS(), "");
                connTo->runCommand("admin", recvChunkStartBuilder.done(), res);
                connTo.done();
            } catch (const DBException& e) {
                Status exceptionStatus = e.toStatus();
                const string msg = stream() << "moveChunk could not contact to: shard "
                                            << chunkMoveState.getToShard() << " to start transfer"
                                            << causedBy(exceptionStatus);
                warning() << msg;
                return appendCommandStatus(result, exceptionStatus);
            }

            Status recvChunkStartStatus = getStatusFromCommandResult(res);
            if (!recvChunkStartStatus.isOK()) {
                const string msg = stream()
                    << "moveChunk failed to engage TO-shard in the data transfer: "
                    << causedBy(recvChunkStartStatus);
                result.append("cause", res);
                warning() << msg;
                return appendCommandStatus(result, Status(recvChunkStartStatus.code(), msg));
            }
        }

        timing.done(3);

        distLockStatus = distLock->checkForPendingCatalogSwap();
        if (!distLockStatus.isOK()) {
            warning() << "Aborting migration due to need to swap current catalog manager"
                      << causedBy(distLockStatus);
            return appendCommandStatus(result, distLockStatus);
        }

        MONGO_FAIL_POINT_PAUSE_WHILE_SET(moveChunkHangAtStep3);

        // 4.

        // Track last result from TO shard for sanity check
        BSONObj res;

        // Don't want a single chunk move to take more than a day
        for (int i = 0; i < 86400; i++) {
            invariant(!txn->lockState()->isLocked());

            // Exponential sleep backoff, up to 1024ms. Don't sleep much on the first few
            // iterations, since we want empty chunk migrations to be fast.
            sleepmillis(1 << std::min(i, 10));

            res = BSONObj();

            try {
                ScopedDbConnection conn(chunkMoveState.getToShardCS());
                conn->runCommand("admin", BSON("_recvChunkStatus" << 1), res);
                conn.done();
                res = res.getOwned();
            } catch (const DBException& e) {
                Status exceptionStatus = e.toStatus();

                warning() << "moveChunk could not contact to: shard " << chunkMoveState.getToShard()
                          << " to monitor transfer " << causedBy(exceptionStatus);

                return appendCommandStatus(result, exceptionStatus);
            }

            Status recvChunkStatus = getStatusFromCommandResult(res);
            if (!recvChunkStatus.isOK()) {
                const string msg = stream()
                    << "moveChunk failed to contact TO-shard to monitor the data transfer: "
                    << causedBy(recvChunkStatus);
                warning() << msg;
                return appendCommandStatus(result, Status(recvChunkStatus.code(), msg));
            }

            if (res["state"].String() == "fail") {
                warning() << "moveChunk error transferring data caused migration abort: " << res
                          << migrateLog;
                errmsg = "data transfer error";
                result.append("cause", res);
                result.append("code", ErrorCodes::OperationFailed);
                return false;
            }

            if (res["ns"].str() != ns ||
                res["from"].str() != chunkMoveState.getFromShardCS().toString() ||
                !res["min"].isABSONObj() ||
                res["min"].Obj().woCompare(chunkMoveState.getMinKey()) != 0 ||
                !res["max"].isABSONObj() ||
                res["max"].Obj().woCompare(chunkMoveState.getMaxKey()) != 0) {
                // This can happen when the destination aborted the migration and
                // received another recvChunk before this thread sees the transition
                // to the abort state. This is currently possible only if multiple migrations
                // are happening at once. This is an unfortunate consequence of the shards not
                // being able to keep track of multiple incoming and outgoing migrations.
                const string msg = stream() << "Destination shard aborted migration, "
                                               "now running a new one: " << res;
                warning() << msg;
                return appendCommandStatus(result, Status(ErrorCodes::OperationIncomplete, msg));
            }

            LOG(0) << "moveChunk data transfer progress: " << res
                   << " my mem used: " << shardingState->migrationSourceManager()->mbUsed()
                   << migrateLog;

            if (res["state"].String() == "steady") {
                break;
            }

            if (shardingState->migrationSourceManager()->mbUsed() > (500 * 1024 * 1024)) {
                // This is too much memory for us to use so we're going to abort the migration

                BSONObj abortRes;

                ScopedDbConnection conn(chunkMoveState.getToShardCS());
                if (!conn->runCommand("admin", BSON("_recvChunkAbort" << 1), abortRes)) {
                    warning() << "Error encountered while trying to abort migration on "
                              << "destination shard" << chunkMoveState.getToShardCS();
                }
                conn.done();

                error() << "aborting migrate because too much memory used res: " << abortRes
                        << migrateLog;
                errmsg = "aborting migrate because too much memory used";
                result.appendBool("split", true);
                result.append("code", ErrorCodes::ExceededMemoryLimit);

                return false;
            }

            txn->checkForInterrupt();

            distLockStatus = distLock->checkForPendingCatalogSwap();
            if (!distLockStatus.isOK()) {
                warning() << "Aborting migration due to need to swap current catalog manager"
                          << causedBy(distLockStatus);
                return appendCommandStatus(result, distLockStatus);
            }
        }

        timing.done(4);

        distLockStatus = distLock->checkForPendingCatalogSwap();
        if (!distLockStatus.isOK()) {
            warning() << "Aborting migration due to need to swap current catalog manager"
                      << causedBy(distLockStatus);
            return appendCommandStatus(result, distLockStatus);
        }

        MONGO_FAIL_POINT_PAUSE_WHILE_SET(moveChunkHangAtStep4);

        // 5.

        // Before we get into the critical section of the migration, let's double check that the
        // docs have been cloned, the config servers are reachable, and the lock is in place.
        log() << "About to check if it is safe to enter critical section";

        // Ensure all cloned docs have actually been transferred
        const std::size_t locsRemaining =
            shardingState->migrationSourceManager()->cloneLocsRemaining();
        if (locsRemaining != 0) {
            const string msg = stream()
                << "moveChunk cannot enter critical section before all data is"
                << " cloned, " << locsRemaining << " locs were not transferred"
                << " but to-shard reported " << res;

            // Should never happen, but safe to abort before critical section
            error() << msg << migrateLog;
            dassert(false);
            return appendCommandStatus(result, Status(ErrorCodes::OperationIncomplete, msg));
        }

        // Ensure distributed lock still held
        Status lockStatus = distLock->checkStatus();
        if (!lockStatus.isOK()) {
            const string msg = stream() << "not entering migrate critical section because "
                                        << lockStatus.toString();
            warning() << msg;
            return appendCommandStatus(result, Status(lockStatus.code(), msg));
        }

        uassertStatusOK(chunkMoveState.commitMigration());
        timing.done(5);

        MONGO_FAIL_POINT_PAUSE_WHILE_SET(moveChunkHangAtStep5);

        // 6.

        RangeDeleterOptions deleterOptions(KeyRange(ns,
                                                    chunkMoveState.getMinKey().getOwned(),
                                                    chunkMoveState.getMaxKey().getOwned(),
                                                    shardKeyPattern));
        deleterOptions.writeConcern = writeConcern;
        deleterOptions.waitForOpenCursors = true;
        deleterOptions.fromMigrate = true;
        deleterOptions.onlyRemoveOrphanedDocs = true;
        deleterOptions.removeSaverReason = "post-cleanup";

        if (waitForDelete) {
            log() << "doing delete inline for cleanup of chunk data" << migrateLog;

            string errMsg;

            // This is an immediate delete, and as a consequence, there could be more
            // deletes happening simultaneously than there are deleter worker threads.
            if (!getDeleter()->deleteNow(txn, deleterOptions, &errMsg)) {
                log() << "Error occured while performing cleanup: " << errMsg;
            }
        } else {
            log() << "forking for cleanup of chunk data" << migrateLog;

            string errMsg;
            if (!getDeleter()->queueDelete(txn,
                                           deleterOptions,
                                           NULL,  // Don't want to be notified
                                           &errMsg)) {
                log() << "could not queue migration cleanup: " << errMsg;
            }
        }

        timing.done(6);

        MONGO_FAIL_POINT_PAUSE_WHILE_SET(moveChunkHangAtStep6);

        return true;
    }

} moveChunkCmd;

}  // namespace
}  // namespace mongo
