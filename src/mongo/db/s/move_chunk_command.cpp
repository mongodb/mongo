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

#include "mongo/db/auth/action_set.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_manager.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/commands.h"
#include "mongo/db/range_deleter_service.h"
#include "mongo/db/s/chunk_move_write_concern_options.h"
#include "mongo/db/s/collection_metadata.h"
#include "mongo/db/s/migration_source_manager.h"
#include "mongo/db/s/move_timing_helper.h"
#include "mongo/db/s/sharding_state.h"
#include "mongo/s/catalog/dist_lock_manager.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/grid.h"
#include "mongo/s/migration_secondary_throttle_options.h"
#include "mongo/s/move_chunk_request.h"
#include "mongo/util/fail_point_service.h"
#include "mongo/util/log.h"

namespace mongo {

using std::string;

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

/**
 * If the specified status is not OK logs a warning and throws a DBException corresponding to the
 * specified status.
 */
void uassertStatusOKWithWarning(const Status& status) {
    if (!status.isOK()) {
        warning() << "Chunk move failed" << causedBy(status);
        uassertStatusOK(status);
    }
}

// Tests can pause and resume moveChunk's progress at each step by enabling/disabling each failpoint
MONGO_FP_DECLARE(moveChunkHangAtStep1);
MONGO_FP_DECLARE(moveChunkHangAtStep2);
MONGO_FP_DECLARE(moveChunkHangAtStep3);
MONGO_FP_DECLARE(moveChunkHangAtStep4);
MONGO_FP_DECLARE(moveChunkHangAtStep5);
MONGO_FP_DECLARE(moveChunkHangAtStep6);

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
        uassert(ErrorCodes::InvalidOptions, "need to specify a valid namespace", nss.isValid());

        MoveChunkRequest moveChunkRequest =
            uassertStatusOK(MoveChunkRequest::createFromCommand(nss, cmdObj));

        ShardingState* const shardingState = ShardingState::get(txn);

        if (!shardingState->enabled()) {
            shardingState->initialize(txn, moveChunkRequest.getConfigServerCS().toString());
        }

        shardingState->setShardName(moveChunkRequest.getFromShardId());

        const auto writeConcernForRangeDeleter =
            uassertStatusOK(ChunkMoveWriteConcernOptions::getEffectiveWriteConcern(
                moveChunkRequest.getSecondaryThrottle()));

        // Make sure we're as up-to-date as possible with shard information. This catches the case
        // where we might have changed a shard's host by removing/adding a shard with the same name.
        grid.shardRegistry()->reload(txn);

        MoveTimingHelper moveTimingHelper(txn,
                                          "from",
                                          nss.ns(),
                                          moveChunkRequest.getMinKey(),
                                          moveChunkRequest.getMaxKey(),
                                          6,  // Total number of steps
                                          &errmsg,
                                          moveChunkRequest.getToShardId(),
                                          moveChunkRequest.getFromShardId());

        moveTimingHelper.done(1);
        MONGO_FAIL_POINT_PAUSE_WHILE_SET(moveChunkHangAtStep1);

        ScopedSetInMigration scopedInMigration(txn, moveChunkRequest);

        BSONObj shardKeyPattern;

        {
            MigrationSourceManager migrationSourceManager(txn, moveChunkRequest);

            shardKeyPattern =
                migrationSourceManager.getCommittedMetadata()->getKeyPattern().getOwned();

            moveTimingHelper.done(2);
            MONGO_FAIL_POINT_PAUSE_WHILE_SET(moveChunkHangAtStep2);

            Status startCloneStatus = migrationSourceManager.startClone(txn);
            if (startCloneStatus == ErrorCodes::ChunkTooBig) {
                // TODO: This is for compatibility with pre-3.2 balancer, which does not recognize
                // the ChunkTooBig error code and instead uses the "chunkTooBig" field in the
                // response. Remove after 3.4 is released.
                errmsg = startCloneStatus.reason();
                result.appendBool("chunkTooBig", true);
                return false;
            }

            uassertStatusOKWithWarning(startCloneStatus);
            moveTimingHelper.done(3);
            MONGO_FAIL_POINT_PAUSE_WHILE_SET(moveChunkHangAtStep3);

            uassertStatusOKWithWarning(migrationSourceManager.awaitToCatchUp(txn));
            moveTimingHelper.done(4);
            MONGO_FAIL_POINT_PAUSE_WHILE_SET(moveChunkHangAtStep4);

            // Ensure distributed lock is still held
            Status checkDistLockStatus = scopedInMigration.checkDistLock();
            if (!checkDistLockStatus.isOK()) {
                const string msg = str::stream() << "not entering migrate critical section due to "
                                                 << checkDistLockStatus.toString();
                warning() << msg;
                migrationSourceManager.cleanupOnError(txn);

                uasserted(checkDistLockStatus.code(), msg);
            }

            uassertStatusOKWithWarning(migrationSourceManager.enterCriticalSection(txn));
            uassertStatusOKWithWarning(migrationSourceManager.commitDonateChunk(txn));
            moveTimingHelper.done(5);
            MONGO_FAIL_POINT_PAUSE_WHILE_SET(moveChunkHangAtStep5);
        }

        // Schedule the range deleter
        RangeDeleterOptions deleterOptions(KeyRange(moveChunkRequest.getNss().ns(),
                                                    moveChunkRequest.getMinKey().getOwned(),
                                                    moveChunkRequest.getMaxKey().getOwned(),
                                                    shardKeyPattern));
        deleterOptions.writeConcern = writeConcernForRangeDeleter;
        deleterOptions.waitForOpenCursors = true;
        deleterOptions.fromMigrate = true;
        deleterOptions.onlyRemoveOrphanedDocs = true;
        deleterOptions.removeSaverReason = "post-cleanup";

        if (moveChunkRequest.getWaitForDelete()) {
            log() << "doing delete inline for cleanup of chunk data";

            string errMsg;

            // This is an immediate delete, and as a consequence, there could be more
            // deletes happening simultaneously than there are deleter worker threads.
            if (!getDeleter()->deleteNow(txn, deleterOptions, &errMsg)) {
                log() << "Error occured while performing cleanup: " << errMsg;
            }
        } else {
            log() << "forking for cleanup of chunk data";

            string errMsg;
            if (!getDeleter()->queueDelete(txn,
                                           deleterOptions,
                                           NULL,  // Don't want to be notified
                                           &errMsg)) {
                log() << "could not queue migration cleanup: " << errMsg;
            }
        }

        moveTimingHelper.done(6);
        MONGO_FAIL_POINT_PAUSE_WHILE_SET(moveChunkHangAtStep6);

        return true;
    }

} moveChunkCmd;

}  // namespace
}  // namespace mongo
