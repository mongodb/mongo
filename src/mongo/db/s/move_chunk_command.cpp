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
#include "mongo/db/s/chunk_move_write_concern_options.h"
#include "mongo/db/s/collection_metadata.h"
#include "mongo/db/s/migration_impl.h"
#include "mongo/db/s/operation_sharding_state.h"
#include "mongo/db/s/sharding_state.h"
#include "mongo/logger/ramlog.h"
#include "mongo/s/catalog/dist_lock_manager.h"
#include "mongo/s/catalog/type_chunk.h"
#include "mongo/s/client/shard_connection.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/chunk_version.h"
#include "mongo/s/grid.h"
#include "mongo/s/migration_secondary_throttle_options.h"
#include "mongo/s/move_chunk_request.h"
#include "mongo/util/fail_point_service.h"
#include "mongo/util/log.h"

namespace mongo {

using std::string;
using str::stream;

namespace {

/**
 * RAII object, which will register an active migration with the global sharding state so that no
 * subsequent migrations may start until the previous one has completed.
 */
class ScopedSetInMigration {
    MONGO_DISALLOW_COPYING(ScopedSetInMigration);

public:
    /**
     * Registers a new migration with the global sharding state and acquires distributed lock on the
     * collection so other shards cannot do migrations. If a migration is already active it will
     * throw a user assertion with a ConflictingOperationInProgress code. Otherwise it may throw any
     * other error due to distributed lock acquisition failure.
     */
    ScopedSetInMigration(OperationContext* txn, MoveChunkRequest args)
        : _scopedRegisterMigration(txn, args.getNss()),
          _collectionDistLock(_acquireDistLock(txn, args)) {}

    ~ScopedSetInMigration() = default;

    /**
     * Blocking call, which polls the state of the distributed lock and ensures that it is still
     * held.
     */
    Status checkDistLock() {
        return _collectionDistLock.checkStatus();
    }

private:
    /**
     * Acquires a distributed lock for the specified colleciton or throws if lock cannot be
     * acquired.
     */
    DistLockManager::ScopedDistLock _acquireDistLock(OperationContext* txn,
                                                     const MoveChunkRequest& args) {
        const std::string whyMessage(str::stream() << "migrating chunk [" << args.getMinKey()
                                                   << ", " << args.getMaxKey() << ") in "
                                                   << args.getNss().ns());
        auto distLockStatus =
            grid.catalogManager(txn)->distLock(txn, args.getNss().ns(), whyMessage);
        if (!distLockStatus.isOK()) {
            const std::string msg = str::stream()
                << "Could not acquire collection lock for " << args.getNss().ns()
                << " to migrate chunk [" << args.getMinKey() << "," << args.getMaxKey()
                << ") due to " << distLockStatus.getStatus().toString();
            warning() << msg;
            uasserted(distLockStatus.getStatus().code(), msg);
        }

        return std::move(distLockStatus.getValue());
    }

    // The scoped migration registration
    ShardingState::ScopedRegisterMigration _scopedRegisterMigration;

    // Handle for the the distributed lock, which protects other migrations from happening on the
    // same collection
    DistLockManager::ScopedDistLock _collectionDistLock;
};

// Tests can pause and resume moveChunk's progress at each step by enabling/disabling each failpoint
MONGO_FP_DECLARE(moveChunkHangAtStep1);
MONGO_FP_DECLARE(moveChunkHangAtStep2);
MONGO_FP_DECLARE(moveChunkHangAtStep3);
MONGO_FP_DECLARE(moveChunkHangAtStep4);
MONGO_FP_DECLARE(moveChunkHangAtStep5);
MONGO_FP_DECLARE(moveChunkHangAtStep6);

Tee* const migrateLog = RamLog::get("migrate");

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

    Status checkAuthForCommand(ClientBasic* client,
                               const std::string& dbname,
                               const BSONObj& cmdObj) override {
        if (!AuthorizationSession::get(client)->isAuthorizedForActionsOnResource(
                ResourcePattern::forClusterResource(), ActionType::internal)) {
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
        const NamespaceString nss = NamespaceString(parseNs(dbname, cmdObj));
        if (!nss.isValid()) {
            return appendCommandStatus(
                result, Status(ErrorCodes::InvalidOptions, "need to specify namespace in command"));
        }

        MoveChunkRequest moveChunkRequest =
            uassertStatusOK(MoveChunkRequest::createFromCommand(nss, cmdObj));

        ShardingState* const shardingState = ShardingState::get(txn);

        if (!shardingState->enabled()) {
            shardingState->initialize(txn, moveChunkRequest.getConfigServerCS().toString());
        }

        shardingState->setShardName(moveChunkRequest.getFromShardId());

        const auto& oss = OperationShardingState::get(txn);
        if (!oss.hasShardVersion()) {
            uassertStatusOK(
                Status(ErrorCodes::InvalidOptions, "moveChunk command is missing shard version"));
        }

        const auto writeConcernForRangeDeleter =
            uassertStatusOK(ChunkMoveWriteConcernOptions::getEffectiveWriteConcern(
                moveChunkRequest.getSecondaryThrottle()));

        // Make sure we're as up-to-date as possible with shard information. This catches the case
        // where we might have changed a shard's host by removing/adding a shard with the same name.
        grid.shardRegistry()->reload(txn);

        MONGO_FAIL_POINT_PAUSE_WHILE_SET(moveChunkHangAtStep1);

        ScopedSetInMigration scopedInMigration(txn, moveChunkRequest);

        ChunkMoveOperationState chunkMoveState{txn, nss};
        uassertStatusOK(chunkMoveState.initialize(cmdObj));

        MoveTimingHelper timing(txn,
                                "from",
                                nss.ns(),
                                chunkMoveState.getMinKey(),
                                chunkMoveState.getMaxKey(),
                                6,  // Total number of steps
                                &errmsg,
                                chunkMoveState.getToShard(),
                                chunkMoveState.getFromShard());

        log() << "received moveChunk request: " << cmdObj << migrateLog;

        timing.done(1);

        // 2.

        if (shardingState->migrationSourceManager()->isActive()) {
            const std::string msg =
                "Not starting chunk migration because another migration is already in progress";
            warning() << msg;
            return appendCommandStatus(result,
                                       Status(ErrorCodes::ConflictingOperationInProgress, msg));
        }

        uassertStatusOK(chunkMoveState.acquireMoveMetadata());

        grid.catalogManager(txn)->logChange(txn,
                                            "moveChunk.start",
                                            nss.ns(),
                                            BSON("min" << chunkMoveState.getMinKey() << "max"
                                                       << chunkMoveState.getMaxKey() << "from"
                                                       << chunkMoveState.getFromShard() << "to"
                                                       << chunkMoveState.getToShard()));

        const auto origCollMetadata = chunkMoveState.getCollMetadata();
        BSONObj shardKeyPattern = origCollMetadata->getKeyPattern();

        log() << "moveChunk request accepted at version " << chunkMoveState.getShardVersion();

        timing.done(2);

        MONGO_FAIL_POINT_PAUSE_WHILE_SET(moveChunkHangAtStep2);

        // 3.

        const auto migrationSessionId = MigrationSessionId::generate(chunkMoveState.getFromShard(),
                                                                     chunkMoveState.getToShard());
        auto moveChunkStartStatus = chunkMoveState.start(migrationSessionId, shardKeyPattern);

        if (!moveChunkStartStatus.isOK()) {
            warning() << moveChunkStartStatus.toString();
            return appendCommandStatus(result, moveChunkStartStatus);
        }

        {
            // See comment at the top of the function for more information on what kind of
            // synchronization is used here.
            if (!shardingState->migrationSourceManager()->storeCurrentLocs(
                    txn, moveChunkRequest.getMaxChunkSizeBytes(), errmsg, result)) {
                warning() << errmsg;
                return false;
            }

            BSONObjBuilder recvChunkStartBuilder;
            recvChunkStartBuilder.append("_recvChunkStart", nss.ns());
            migrationSessionId.append(&recvChunkStartBuilder);
            recvChunkStartBuilder.append("from", chunkMoveState.getFromShardCS().toString());
            recvChunkStartBuilder.append("fromShardName", chunkMoveState.getFromShard());
            recvChunkStartBuilder.append("toShardName", chunkMoveState.getToShard());
            recvChunkStartBuilder.append("min", chunkMoveState.getMinKey());
            recvChunkStartBuilder.append("max", chunkMoveState.getMaxKey());
            recvChunkStartBuilder.append("shardKeyPattern", shardKeyPattern);
            recvChunkStartBuilder.append("configServer",
                                         shardingState->getConfigServer(txn).toString());
            moveChunkRequest.getSecondaryThrottle().append(&recvChunkStartBuilder);

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

            if (res["ns"].str() != nss.ns() ||
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
        }

        timing.done(4);

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
        Status lockStatus = scopedInMigration.checkDistLock();
        if (!lockStatus.isOK()) {
            const string msg = stream() << "not entering migrate critical section because "
                                        << lockStatus.toString();
            warning() << msg;
            return appendCommandStatus(result, Status(lockStatus.code(), msg));
        }

        uassertStatusOK(chunkMoveState.commitMigration(migrationSessionId));
        timing.done(5);

        MONGO_FAIL_POINT_PAUSE_WHILE_SET(moveChunkHangAtStep5);

        // 6.

        RangeDeleterOptions deleterOptions(KeyRange(nss.ns(),
                                                    chunkMoveState.getMinKey().getOwned(),
                                                    chunkMoveState.getMaxKey().getOwned(),
                                                    shardKeyPattern));
        deleterOptions.writeConcern = writeConcernForRangeDeleter;
        deleterOptions.waitForOpenCursors = true;
        deleterOptions.fromMigrate = true;
        deleterOptions.onlyRemoveOrphanedDocs = true;
        deleterOptions.removeSaverReason = "post-cleanup";

        if (moveChunkRequest.getWaitForDelete()) {
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
