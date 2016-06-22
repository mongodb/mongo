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
#include "mongo/util/concurrency/notification.h"
#include "mongo/util/fail_point_service.h"
#include "mongo/util/log.h"

namespace mongo {

using std::string;

namespace {

/**
 * Acquires a distributed lock for the specified collection or throws if lock cannot be acquired.
 */
DistLockManager::ScopedDistLock acquireCollectionDistLock(OperationContext* txn,
                                                          const MoveChunkRequest& args) {
    const string whyMessage(str::stream()
                            << "migrating chunk "
                            << ChunkRange(args.getMinKey(), args.getMaxKey()).toString()
                            << " in "
                            << args.getNss().ns());
    auto distLockStatus =
        Grid::get(txn)->catalogClient(txn)->distLock(txn, args.getNss().ns(), whyMessage);
    if (!distLockStatus.isOK()) {
        const string msg = str::stream()
            << "Could not acquire collection lock for " << args.getNss().ns()
            << " to migrate chunk [" << args.getMinKey() << "," << args.getMaxKey() << ") due to "
            << distLockStatus.getStatus().toString();
        warning() << msg;
        uasserted(distLockStatus.getStatus().code(), msg);
    }

    return std::move(distLockStatus.getValue());
}

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

    bool supportsWriteConcern(const BSONObj& cmd) const override {
        return true;
    }

    Status checkAuthForCommand(ClientBasic* client,
                               const string& dbname,
                               const BSONObj& cmdObj) override {
        if (!AuthorizationSession::get(client)->isAuthorizedForActionsOnResource(
                ResourcePattern::forClusterResource(), ActionType::internal)) {
            return Status(ErrorCodes::Unauthorized, "Unauthorized");
        }
        return Status::OK();
    }

    string parseNs(const string& dbname, const BSONObj& cmdObj) const override {
        return parseNsFullyQualified(dbname, cmdObj);
    }

    bool run(OperationContext* txn,
             const string& dbname,
             BSONObj& cmdObj,
             int options,
             string& errmsg,
             BSONObjBuilder& result) override {
        const MoveChunkRequest moveChunkRequest = uassertStatusOK(
            MoveChunkRequest::createFromCommand(NamespaceString(parseNs(dbname, cmdObj)), cmdObj));

        ShardingState* const shardingState = ShardingState::get(txn);

        if (!shardingState->enabled()) {
            shardingState->initializeFromConfigConnString(
                txn, moveChunkRequest.getConfigServerCS().toString());
        }

        shardingState->setShardName(moveChunkRequest.getFromShardId().toString());

        // Make sure we're as up-to-date as possible with shard information. This catches the case
        // where we might have changed a shard's host by removing/adding a shard with the same name.
        grid.shardRegistry()->reload(txn);

        auto scopedRegisterMigration =
            uassertStatusOK(shardingState->registerMigration(moveChunkRequest));

        Status status = {ErrorCodes::InternalError, "Uninitialized value"};

        // Check if there is an existing migration running and if so, join it
        if (scopedRegisterMigration.mustExecute()) {
            try {
                _runImpl(txn, moveChunkRequest);
                status = Status::OK();
            } catch (const DBException& e) {
                status = e.toStatus();
            } catch (const std::exception& e) {
                scopedRegisterMigration.complete(
                    {ErrorCodes::InternalError,
                     str::stream() << "Severe error occurred while running moveChunk command: "
                                   << e.what()});
                throw;
            }

            scopedRegisterMigration.complete(status);
        } else {
            status = scopedRegisterMigration.waitForCompletion(txn);
        }

        if (status == ErrorCodes::ChunkTooBig) {
            // This code is for compatibility with pre-3.2 balancer, which does not recognize the
            // ChunkTooBig error code and instead uses the "chunkTooBig" field in the response.
            // TODO: Remove after 3.4 is released.
            errmsg = status.reason();
            result.appendBool("chunkTooBig", true);
            return false;
        }

        uassertStatusOK(status);
        return true;
    }

private:
    static void _runImpl(OperationContext* txn, const MoveChunkRequest& moveChunkRequest) {
        const auto writeConcernForRangeDeleter =
            uassertStatusOK(ChunkMoveWriteConcernOptions::getEffectiveWriteConcern(
                txn, moveChunkRequest.getSecondaryThrottle()));

        string unusedErrMsg;
        MoveTimingHelper moveTimingHelper(txn,
                                          "from",
                                          moveChunkRequest.getNss().ns(),
                                          moveChunkRequest.getMinKey(),
                                          moveChunkRequest.getMaxKey(),
                                          6,  // Total number of steps
                                          &unusedErrMsg,
                                          moveChunkRequest.getToShardId(),
                                          moveChunkRequest.getFromShardId());

        moveTimingHelper.done(1);
        MONGO_FAIL_POINT_PAUSE_WHILE_SET(moveChunkHangAtStep1);

        BSONObj shardKeyPattern;

        {
            // Acquire the collection distributed lock if necessary
            boost::optional<DistLockManager::ScopedDistLock> scopedCollectionDistLock;
            if (moveChunkRequest.getTakeDistLock()) {
                scopedCollectionDistLock = acquireCollectionDistLock(txn, moveChunkRequest);
            }

            MigrationSourceManager migrationSourceManager(txn, moveChunkRequest);

            shardKeyPattern = migrationSourceManager.getKeyPattern().getOwned();

            moveTimingHelper.done(2);
            MONGO_FAIL_POINT_PAUSE_WHILE_SET(moveChunkHangAtStep2);

            uassertStatusOKWithWarning(migrationSourceManager.startClone(txn));
            moveTimingHelper.done(3);
            MONGO_FAIL_POINT_PAUSE_WHILE_SET(moveChunkHangAtStep3);

            uassertStatusOKWithWarning(migrationSourceManager.awaitToCatchUp(txn));
            moveTimingHelper.done(4);
            MONGO_FAIL_POINT_PAUSE_WHILE_SET(moveChunkHangAtStep4);

            // Ensure the distributed lock is still held if this shard owns it.
            if (moveChunkRequest.getTakeDistLock()) {
                Status checkDistLockStatus = scopedCollectionDistLock->checkStatus();
                if (!checkDistLockStatus.isOK()) {
                    migrationSourceManager.cleanupOnError(txn);

                    uassertStatusOKWithWarning(
                        {checkDistLockStatus.code(),
                         str::stream() << "not entering migrate critical section due to "
                                       << checkDistLockStatus.toString()});
                }
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
    }

} moveChunkCmd;

}  // namespace
}  // namespace mongo
