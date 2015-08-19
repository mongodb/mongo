/**
 *    Copyright (C) 2008-2015 MongoDB Inc.
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

#include <algorithm>
#include <map>
#include <string>
#include <vector>

#include "mongo/client/connpool.h"
#include "mongo/db/auth/action_set.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_manager.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/privilege.h"
#include "mongo/db/catalog/index_create.h"
#include "mongo/db/clientcursor.h"
#include "mongo/db/commands.h"
#include "mongo/db/concurrency/lock_state.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/dbhelpers.h"
#include "mongo/db/field_parser.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/op_observer.h"
#include "mongo/db/range_deleter_service.h"
#include "mongo/db/repl/replication_coordinator_global.h"
#include "mongo/db/s/collection_metadata.h"
#include "mongo/db/s/sharded_connection_info.h"
#include "mongo/db/s/sharding_state.h"
#include "mongo/db/write_concern.h"
#include "mongo/logger/ramlog.h"
#include "mongo/s/catalog/catalog_manager.h"
#include "mongo/s/catalog/type_chunk.h"
#include "mongo/s/chunk.h"
#include "mongo/s/chunk_version.h"
#include "mongo/s/client/shard.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/config.h"
#include "mongo/s/d_state.h"
#include "mongo/s/grid.h"
#include "mongo/stdx/chrono.h"
#include "mongo/stdx/memory.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/exit.h"
#include "mongo/util/fail_point_service.h"
#include "mongo/util/log.h"

namespace mongo {

using std::list;
using std::set;
using std::string;
using std::unique_ptr;
using std::vector;

namespace {

const int kDefaultWTimeoutMs = 60 * 1000;
const WriteConcernOptions DefaultWriteConcern(2, WriteConcernOptions::NONE, kDefaultWTimeoutMs);

Tee* migrateLog = RamLog::get("migrate");

/**
 * Returns the default write concern for migration cleanup (at donor shard) and cloning documents
 * (on the destination shard).
 */
WriteConcernOptions getDefaultWriteConcern() {
    repl::ReplicationCoordinator* replCoordinator = repl::getGlobalReplicationCoordinator();
    if (replCoordinator->getReplicationMode() == mongo::repl::ReplicationCoordinator::modeReplSet) {
        Status status = replCoordinator->checkIfWriteConcernCanBeSatisfied(DefaultWriteConcern);
        if (status.isOK()) {
            return DefaultWriteConcern;
        }
    }

    return WriteConcernOptions(1, WriteConcernOptions::NONE, 0);
}

struct MigrateStatusHolder {
    MigrateStatusHolder(OperationContext* txn,
                        MigrationSourceManager* migrateSourceManager,
                        const std::string& ns,
                        const BSONObj& min,
                        const BSONObj& max,
                        const BSONObj& shardKeyPattern)
        : _txn(txn), _migrateSourceManager(migrateSourceManager) {
        _isAnotherMigrationActive =
            !_migrateSourceManager->start(txn, ns, min, max, shardKeyPattern);
    }

    ~MigrateStatusHolder() {
        if (!_isAnotherMigrationActive) {
            _migrateSourceManager->done(_txn);
        }
    }

    bool isAnotherMigrationActive() const {
        return _isAnotherMigrationActive;
    }

private:
    OperationContext* const _txn;
    MigrationSourceManager* const _migrateSourceManager;

    bool _isAnotherMigrationActive;
};

}  // namespace

MONGO_FP_DECLARE(failMigrationCommit);
MONGO_FP_DECLARE(failMigrationConfigWritePrepare);
MONGO_FP_DECLARE(failMigrationApplyOps);

class TransferModsCommand : public Command {
public:
    TransferModsCommand() : Command("_transferMods") {}

    void help(std::stringstream& h) const {
        h << "internal";
    }

    virtual bool slaveOk() const {
        return false;
    }

    virtual bool adminOnly() const {
        return true;
    }

    virtual bool isWriteCommandForConfigServer() const {
        return false;
    }

    virtual void addRequiredPrivileges(const std::string& dbname,
                                       const BSONObj& cmdObj,
                                       std::vector<Privilege>* out) {
        ActionSet actions;
        actions.addAction(ActionType::internal);
        out->push_back(Privilege(ResourcePattern::forClusterResource(), actions));
    }

    bool run(OperationContext* txn,
             const string&,
             BSONObj& cmdObj,
             int,
             string& errmsg,
             BSONObjBuilder& result) {
        return ShardingState::get(txn)->migrationSourceManager()->transferMods(txn, errmsg, result);
    }

} transferModsCommand;

class InitialCloneCommand : public Command {
public:
    InitialCloneCommand() : Command("_migrateClone") {}

    void help(std::stringstream& h) const {
        h << "internal";
    }

    virtual bool slaveOk() const {
        return false;
    }

    virtual bool adminOnly() const {
        return true;
    }

    virtual bool isWriteCommandForConfigServer() const {
        return false;
    }

    virtual void addRequiredPrivileges(const std::string& dbname,
                                       const BSONObj& cmdObj,
                                       std::vector<Privilege>* out) {
        ActionSet actions;
        actions.addAction(ActionType::internal);
        out->push_back(Privilege(ResourcePattern::forClusterResource(), actions));
    }

    bool run(OperationContext* txn,
             const string&,
             BSONObj& cmdObj,
             int,
             string& errmsg,
             BSONObjBuilder& result) {
        return ShardingState::get(txn)->migrationSourceManager()->clone(txn, errmsg, result);
    }

} initialCloneCommand;

// Tests can pause / resume moveChunk's progress at each step by enabling / disabling each fail
// point.
MONGO_FP_DECLARE(moveChunkHangAtStep1);
MONGO_FP_DECLARE(moveChunkHangAtStep2);
MONGO_FP_DECLARE(moveChunkHangAtStep3);
MONGO_FP_DECLARE(moveChunkHangAtStep4);
MONGO_FP_DECLARE(moveChunkHangAtStep5);
MONGO_FP_DECLARE(moveChunkHangAtStep6);

/**
 * this is the main entry for moveChunk
 * called to initial a move
 * usually by a mongos
 * this is called on the "from" side
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
 */
class MoveChunkCommand : public Command {
public:
    MoveChunkCommand() : Command("moveChunk") {}
    virtual void help(std::stringstream& help) const {
        help << "should not be calling this directly";
    }

    virtual bool slaveOk() const {
        return false;
    }
    virtual bool adminOnly() const {
        return true;
    }
    virtual bool isWriteCommandForConfigServer() const {
        return false;
    }
    virtual Status checkAuthForCommand(ClientBasic* client,
                                       const std::string& dbname,
                                       const BSONObj& cmdObj) {
        if (!AuthorizationSession::get(client)->isAuthorizedForActionsOnResource(
                ResourcePattern::forExactNamespace(NamespaceString(parseNs(dbname, cmdObj))),
                ActionType::moveChunk)) {
            return Status(ErrorCodes::Unauthorized, "Unauthorized");
        }
        return Status::OK();
    }
    virtual std::string parseNs(const std::string& dbname, const BSONObj& cmdObj) const {
        return parseNsFullyQualified(dbname, cmdObj);
    }

    bool run(OperationContext* txn,
             const string& dbname,
             BSONObj& cmdObj,
             int,
             string& errmsg,
             BSONObjBuilder& result) {
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
        //
        // 4. pause till migrate caught up
        // 5. LOCK
        //    a) update my config, essentially locking
        //    b) finish migrate
        //    c) update config server
        //    d) logChange to config server
        // 6. wait for all current cursors to expire
        // 7. remove data locally

        // -------------------------------

        // 1.
        string ns = parseNs(dbname, cmdObj);

        // The shard addresses, redundant, but allows for validation
        string toShardHost = cmdObj["to"].str();
        string fromShardHost = cmdObj["from"].str();

        // The shard names
        string toShardName = cmdObj["toShard"].str();
        string fromShardName = cmdObj["fromShard"].str();

        // Process secondary throttle settings and assign defaults if necessary.
        BSONObj secThrottleObj;
        WriteConcernOptions writeConcern;
        Status status = writeConcern.parseSecondaryThrottle(cmdObj, &secThrottleObj);

        if (!status.isOK()) {
            if (status.code() != ErrorCodes::WriteConcernNotDefined) {
                warning() << status.toString();
                return appendCommandStatus(result, status);
            }

            writeConcern = getDefaultWriteConcern();
        } else {
            repl::ReplicationCoordinator* replCoordinator = repl::getGlobalReplicationCoordinator();

            if (replCoordinator->getReplicationMode() ==
                    repl::ReplicationCoordinator::modeMasterSlave &&
                writeConcern.shouldWaitForOtherNodes()) {
                warning() << "moveChunk cannot check if secondary throttle setting "
                          << writeConcern.toBSON()
                          << " can be enforced in a master slave configuration";
            }

            Status status = replCoordinator->checkIfWriteConcernCanBeSatisfied(writeConcern);
            if (!status.isOK() && status != ErrorCodes::NoReplicationEnabled) {
                warning() << status.toString();
                return appendCommandStatus(result, status);
            }
        }

        if (writeConcern.shouldWaitForOtherNodes() &&
            writeConcern.wTimeout == WriteConcernOptions::kNoTimeout) {
            // Don't allow no timeout.
            writeConcern.wTimeout = kDefaultWTimeoutMs;
        }

        // Do inline deletion
        bool waitForDelete = cmdObj["waitForDelete"].trueValue();
        if (waitForDelete) {
            log() << "moveChunk waiting for full cleanup after move";
        }

        BSONObj min = cmdObj["min"].Obj();
        BSONObj max = cmdObj["max"].Obj();
        BSONElement maxSizeElem = cmdObj["maxChunkSizeBytes"];

        if (ns.empty()) {
            errmsg = "need to specify namespace in command";
            return false;
        }

        if (toShardName.empty()) {
            errmsg = "need to specify shard to move chunk to";
            return false;
        }
        if (fromShardName.empty()) {
            errmsg = "need to specify shard to move chunk from";
            return false;
        }

        if (min.isEmpty()) {
            errmsg = "need to specify a min";
            return false;
        }

        if (max.isEmpty()) {
            errmsg = "need to specify a max";
            return false;
        }

        if (maxSizeElem.eoo() || !maxSizeElem.isNumber()) {
            errmsg = "need to specify maxChunkSizeBytes";
            return false;
        }

        const long long maxChunkSize = maxSizeElem.numberLong();  // in bytes

        ShardingState* const shardingState = ShardingState::get(txn);

        // This could be the first call that enables sharding - make sure we initialize the
        // sharding state for this shard.
        if (!shardingState->enabled()) {
            if (cmdObj["configdb"].type() != String) {
                errmsg = "sharding not enabled";
                warning() << errmsg;
                return false;
            }

            const string configdb = cmdObj["configdb"].String();
            shardingState->initialize(txn, configdb);
        }

        // Initialize our current shard name in the shard state if needed
        shardingState->setShardName(fromShardName);

        // Make sure we're as up-to-date as possible with shard information
        // This catches the case where we had to previously changed a shard's host by
        // removing/adding a shard with the same name
        grid.shardRegistry()->reload(txn);

        MoveTimingHelper timing(
            txn, "from", ns, min, max, 6 /* steps */, &errmsg, toShardName, fromShardName);

        log() << "received moveChunk request: " << cmdObj << migrateLog;

        timing.done(1);

        MONGO_FAIL_POINT_PAUSE_WHILE_SET(moveChunkHangAtStep1);

        // 2.

        if (shardingState->migrationSourceManager()->isActive()) {
            errmsg = "migration already in progress";
            warning() << errmsg;
            return false;
        }

        //
        // Get the distributed lock
        //

        string whyMessage(str::stream() << "migrating chunk [" << minKey << ", " << maxKey
                                        << ") in " << ns);
        auto scopedDistLock = grid.catalogManager(txn)->distLock(txn, ns, whyMessage);

        if (!scopedDistLock.isOK()) {
            errmsg = stream() << "could not acquire collection lock for " << ns
                              << " to migrate chunk [" << minKey << "," << maxKey << ")"
                              << causedBy(scopedDistLock.getStatus());

            warning() << errmsg;
            return false;
        }

        BSONObj chunkInfo =
            BSON("min" << min << "max" << max << "from" << fromShardName << "to" << toShardName);

        grid.catalogManager(txn)->logChange(
            txn, txn->getClient()->clientAddress(true), "moveChunk.start", ns, chunkInfo);

        // Always refresh our metadata remotely
        ChunkVersion origShardVersion;
        Status refreshStatus = shardingState->refreshMetadataNow(txn, ns, &origShardVersion);

        if (!refreshStatus.isOK()) {
            errmsg = str::stream() << "moveChunk cannot start migrate of chunk "
                                   << "[" << minKey << "," << maxKey << ")"
                                   << causedBy(refreshStatus.reason());

            warning() << errmsg;
            return false;
        }

        if (origShardVersion.majorVersion() == 0) {
            // It makes no sense to migrate if our version is zero and we have no chunks
            errmsg = str::stream() << "moveChunk cannot start migrate of chunk "
                                   << "[" << minKey << "," << maxKey << ")"
                                   << " with zero shard version";

            warning() << errmsg;
            return false;
        }

        // From mongos >= v3.0.
        BSONElement epochElem(cmdObj["epoch"]);
        if (epochElem.type() == jstOID) {
            OID cmdEpoch = epochElem.OID();

            if (cmdEpoch != origShardVersion.epoch()) {
                errmsg = str::stream() << "moveChunk cannot move chunk "
                                       << "[" << minKey << "," << maxKey << "), "
                                       << "collection may have been dropped. "
                                       << "current epoch: " << origShardVersion.epoch()
                                       << ", cmd epoch: " << cmdEpoch;
                warning() << errmsg;
                return false;
            }
        }

        // Get collection metadata
        const std::shared_ptr<CollectionMetadata> origCollMetadata(
            shardingState->getCollectionMetadata(ns));

        // With nonzero shard version, we must have metadata
        invariant(NULL != origCollMetadata);

        ChunkVersion origCollVersion = origCollMetadata->getCollVersion();
        BSONObj shardKeyPattern = origCollMetadata->getKeyPattern();

        // With nonzero shard version, we must have a coll version >= our shard version
        invariant(origCollVersion >= origShardVersion);

        // With nonzero shard version, we must have a shard key
        invariant(!shardKeyPattern.isEmpty());

        ChunkType origChunk;
        if (!origCollMetadata->getNextChunk(min, &origChunk) || origChunk.getMin().woCompare(min) ||
            origChunk.getMax().woCompare(max)) {
            // Our boundaries are different from those passed in
            errmsg = str::stream() << "moveChunk cannot find chunk "
                                   << "[" << minKey << "," << maxKey << ")"
                                   << " to migrate, the chunk boundaries may be stale";

            warning() << errmsg;
            return false;
        }

        log() << "moveChunk request accepted at version " << origShardVersion;

        timing.done(2);

        MONGO_FAIL_POINT_PAUSE_WHILE_SET(moveChunkHangAtStep2);

        // 3.
        MigrateStatusHolder statusHolder(
            txn, shardingState->migrationSourceManager(), ns, min, max, shardKeyPattern);

        if (statusHolder.isAnotherMigrationActive()) {
            errmsg = "moveChunk is already in progress from this shard";
            warning() << errmsg;
            return false;
        }

        ConnectionString fromShardCS;
        ConnectionString toShardCS;

        // Resolve the shard connection strings.
        {
            std::shared_ptr<Shard> fromShard = grid.shardRegistry()->getShard(txn, fromShardName);
            uassert(28674,
                    str::stream() << "Source shard " << fromShardName
                                  << " is missing. This indicates metadata corruption.",
                    fromShard);

            fromShardCS = fromShard->getConnString();

            std::shared_ptr<Shard> toShard = grid.shardRegistry()->getShard(txn, toShardName);
            uassert(28675,
                    str::stream() << "Destination shard " << toShardName
                                  << " is missing. This indicates metadata corruption.",
                    toShard);

            toShardCS = toShard->getConnString();
        }

        {
            // See comment at the top of the function for more information on what
            // synchronization is used here.
            if (!shardingState->migrationSourceManager()->storeCurrentLocs(
                    txn, maxChunkSize, errmsg, result)) {
                warning() << errmsg;
                return false;
            }

            BSONObj res;
            bool ok;

            const bool isSecondaryThrottle(writeConcern.shouldWaitForOtherNodes());

            BSONObjBuilder recvChunkStartBuilder;
            recvChunkStartBuilder.append("_recvChunkStart", ns);
            recvChunkStartBuilder.append("from", fromShardCS.toString());
            recvChunkStartBuilder.append("fromShardName", fromShardName);
            recvChunkStartBuilder.append("toShardName", toShardName);
            recvChunkStartBuilder.append("min", min);
            recvChunkStartBuilder.append("max", max);
            recvChunkStartBuilder.append("shardKeyPattern", shardKeyPattern);
            recvChunkStartBuilder.append("configServer", shardingState->getConfigServer(txn));
            recvChunkStartBuilder.append("secondaryThrottle", isSecondaryThrottle);

            // Follow the same convention in moveChunk.
            if (isSecondaryThrottle && !secThrottleObj.isEmpty()) {
                recvChunkStartBuilder.append("writeConcern", secThrottleObj);
            }

            try {
                ScopedDbConnection connTo(toShardCS);
                ok = connTo->runCommand("admin", recvChunkStartBuilder.done(), res);
                connTo.done();
            } catch (DBException& e) {
                errmsg = str::stream() << "moveChunk could not contact to: shard " << toShardName
                                       << " to start transfer" << causedBy(e);
                warning() << errmsg;
                return false;
            }

            if (!ok) {
                errmsg = "moveChunk failed to engage TO-shard in the data transfer: ";
                verify(res["errmsg"].type());
                errmsg += res["errmsg"].String();
                result.append("cause", res);
                warning() << errmsg;
                return false;
            }
        }

        timing.done(3);

        MONGO_FAIL_POINT_PAUSE_WHILE_SET(moveChunkHangAtStep3);

        // 4.

        // Track last result from TO shard for sanity check
        BSONObj res;
        for (int i = 0; i < 86400; i++) {  // don't want a single chunk move to take more than a day
            invariant(!txn->lockState()->isLocked());

            // Exponential sleep backoff, up to 1024ms. Don't sleep much on the first few
            // iterations, since we want empty chunk migrations to be fast.
            sleepmillis(1 << std::min(i, 10));

            ScopedDbConnection conn(toShardCS);
            bool ok;
            res = BSONObj();
            try {
                ok = conn->runCommand("admin", BSON("_recvChunkStatus" << 1), res);
                res = res.getOwned();
            } catch (DBException& e) {
                errmsg = str::stream() << "moveChunk could not contact to: shard " << toShardName
                                       << " to monitor transfer" << causedBy(e);
                warning() << errmsg;
                return false;
            }

            conn.done();

            if (res["ns"].str() != ns || res["from"].str() != fromShardCS.toString() ||
                !res["min"].isABSONObj() || res["min"].Obj().woCompare(min) != 0 ||
                !res["max"].isABSONObj() || res["max"].Obj().woCompare(max) != 0) {
                // This can happen when the destination aborted the migration and
                // received another recvChunk before this thread sees the transition
                // to the abort state. This is currently possible only if multiple migrations
                // are happening at once. This is an unfortunate consequence of the shards not
                // being able to keep track of multiple incoming and outgoing migrations.
                errmsg = str::stream() << "Destination shard aborted migration, "
                                          "now running a new one: " << res;
                warning() << errmsg;
                return false;
            }

            LOG(0) << "moveChunk data transfer progress: " << res
                   << " my mem used: " << shardingState->migrationSourceManager()->mbUsed()
                   << migrateLog;

            if (!ok || res["state"].String() == "fail") {
                warning() << "moveChunk error transferring data caused migration abort: " << res
                          << migrateLog;
                errmsg = "data transfer error";
                result.append("cause", res);
                return false;
            }

            if (res["state"].String() == "steady")
                break;

            if (shardingState->migrationSourceManager()->mbUsed() > (500 * 1024 * 1024)) {
                // This is too much memory for us to use for this so we're going to abort
                // the migrate
                ScopedDbConnection conn(toShardCS);

                BSONObj res;
                if (!conn->runCommand("admin", BSON("_recvChunkAbort" << 1), res)) {
                    warning() << "Error encountered while trying to abort migration on "
                              << "destination shard" << toShardCS;
                }

                res = res.getOwned();
                conn.done();
                error() << "aborting migrate because too much memory used res: " << res
                        << migrateLog;
                errmsg = "aborting migrate because too much memory used";
                result.appendBool("split", true);
                return false;
            }

            txn->checkForInterrupt();
        }

        timing.done(4);

        MONGO_FAIL_POINT_PAUSE_WHILE_SET(moveChunkHangAtStep4);

        // 5.

        // Before we get into the critical section of the migration, let's double check
        // that the docs have been cloned, the config servers are reachable,
        // and the lock is in place.
        log() << "About to check if it is safe to enter critical section";

        // Ensure all cloned docs have actually been transferred
        std::size_t locsRemaining = shardingState->migrationSourceManager()->cloneLocsRemaining();
        if (locsRemaining != 0) {
            errmsg = str::stream() << "moveChunk cannot enter critical section before all data is"
                                   << " cloned, " << locsRemaining << " locs were not transferred"
                                   << " but to-shard reported " << res;

            // Should never happen, but safe to abort before critical section
            error() << errmsg << migrateLog;
            dassert(false);
            return false;
        }

        // Ensure distributed lock still held
        Status lockStatus = scopedDistLock.getValue().checkStatus();
        if (!lockStatus.isOK()) {
            errmsg = str::stream() << "not entering migrate critical section because "
                                   << lockStatus.toString();
            warning() << errmsg;
            return false;
        }

        log() << "About to enter migrate critical section";

        {
            // 5.a
            // we're under the collection lock here, so no other migrate can change maxVersion
            // or CollectionMetadata state
            shardingState->migrationSourceManager()->setInCriticalSection(true);
            ChunkVersion myVersion = origCollVersion;
            myVersion.incMajor();

            {
                ScopedTransaction transaction(txn, MODE_IX);
                Lock::DBLock lk(txn->lockState(), nsToDatabaseSubstring(ns), MODE_IX);
                Lock::CollectionLock collLock(txn->lockState(), ns, MODE_X);
                verify(myVersion > shardingState->getVersion(ns));

                // bump the metadata's version up and "forget" about the chunk being moved
                // this is not the commit point but in practice the state in this shard won't
                // until the commit it done
                shardingState->donateChunk(txn, ns, min, max, myVersion);
            }

            log() << "moveChunk setting version to: " << myVersion << migrateLog;

            // 5.b
            // we're under the collection lock here, too, so we can undo the chunk donation because
            // no other state change could be ongoing

            BSONObj res;
            bool ok;

            try {
                ScopedDbConnection connTo(toShardCS, 35.0);
                ok = connTo->runCommand("admin", BSON("_recvChunkCommit" << 1), res);
                connTo.done();
            } catch (DBException& e) {
                errmsg = str::stream() << "moveChunk could not contact to: shard "
                                       << toShardCS.toString() << " to commit transfer"
                                       << causedBy(e);
                warning() << errmsg;
                ok = false;
            }

            if (!ok || MONGO_FAIL_POINT(failMigrationCommit)) {
                log() << "moveChunk migrate commit not accepted by TO-shard: " << res
                      << " resetting shard version to: " << origShardVersion << migrateLog;
                {
                    ScopedTransaction transaction(txn, MODE_IX);
                    Lock::DBLock dbLock(txn->lockState(), nsToDatabaseSubstring(ns), MODE_IX);
                    Lock::CollectionLock collLock(txn->lockState(), ns, MODE_X);

                    log() << "moveChunk collection lock acquired to reset shard version from "
                             "failed migration";

                    // revert the chunk manager back to the state before "forgetting" about the
                    // chunk
                    shardingState->undoDonateChunk(txn, ns, origCollMetadata);
                }
                log() << "Shard version successfully reset to clean up failed migration";

                errmsg = "_recvChunkCommit failed!";
                result.append("cause", res);
                return false;
            }

            log() << "moveChunk migrate commit accepted by TO-shard: " << res << migrateLog;

            // 5.c

            // version at which the next highest lastmod will be set
            // if the chunk being moved is the last in the shard, nextVersion is that chunk's
            // lastmod otherwise the highest version is from the chunk being bumped on the
            // FROM-shard
            ChunkVersion nextVersion;

            // we want to go only once to the configDB but perhaps change two chunks, the one being
            // migrated and another local one (so to bump version for the entire shard)
            // we use the 'applyOps' mechanism to group the two updates and make them safer
            // TODO pull config update code to a module

            BSONArrayBuilder updates;
            {
                // update for the chunk being moved
                BSONObjBuilder op;
                op.append("op", "u");
                op.appendBool("b", false /* no upserting */);
                op.append("ns", ChunkType::ConfigNS);

                BSONObjBuilder n(op.subobjStart("o"));
                n.append(ChunkType::name(), Chunk::genID(ns, min));
                myVersion.addToBSON(n, ChunkType::DEPRECATED_lastmod());
                n.append(ChunkType::ns(), ns);
                n.append(ChunkType::min(), min);
                n.append(ChunkType::max(), max);
                n.append(ChunkType::shard(), toShardName);
                n.done();

                BSONObjBuilder q(op.subobjStart("o2"));
                q.append(ChunkType::name(), Chunk::genID(ns, min));
                q.done();

                updates.append(op.obj());
            }

            nextVersion = myVersion;

            // if we have chunks left on the FROM shard, update the version of one of them as
            // well.  we can figure that out by grabbing the metadata installed on 5.a

            const std::shared_ptr<CollectionMetadata> bumpedCollMetadata(
                shardingState->getCollectionMetadata(ns));
            if (bumpedCollMetadata->getNumChunks() > 0) {
                // get another chunk on that shard
                ChunkType bumpChunk;
                bool chunkRes =
                    bumpedCollMetadata->getNextChunk(bumpedCollMetadata->getMinKey(), &bumpChunk);
                BSONObj bumpMin = bumpChunk.getMin();
                BSONObj bumpMax = bumpChunk.getMax();

                (void)chunkRes;  // for compile warning on non-debug
                dassert(chunkRes);
                dassert(bumpMin.woCompare(min) != 0);

                BSONObjBuilder op;
                op.append("op", "u");
                op.appendBool("b", false);
                op.append("ns", ChunkType::ConfigNS);

                nextVersion.incMinor();  // same as used on donateChunk
                BSONObjBuilder n(op.subobjStart("o"));
                n.append(ChunkType::name(), Chunk::genID(ns, bumpMin));
                nextVersion.addToBSON(n, ChunkType::DEPRECATED_lastmod());
                n.append(ChunkType::ns(), ns);
                n.append(ChunkType::min(), bumpMin);
                n.append(ChunkType::max(), bumpMax);
                n.append(ChunkType::shard(), fromShardName);
                n.done();

                BSONObjBuilder q(op.subobjStart("o2"));
                q.append(ChunkType::name(), Chunk::genID(ns, bumpMin));
                q.done();

                updates.append(op.obj());

                log() << "moveChunk updating self version to: " << nextVersion << " through "
                      << bumpMin << " -> " << bumpMax << " for collection '" << ns << "'"
                      << migrateLog;
            } else {
                log() << "moveChunk moved last chunk out for collection '" << ns << "'"
                      << migrateLog;
            }

            BSONArrayBuilder preCond;
            {
                BSONObjBuilder b;
                b.append("ns", ChunkType::ConfigNS);
                b.append("q",
                         BSON("query" << BSON(ChunkType::ns(ns)) << "orderby"
                                      << BSON(ChunkType::DEPRECATED_lastmod() << -1)));
                {
                    BSONObjBuilder bb(b.subobjStart("res"));
                    // TODO: For backwards compatibility, we can't yet require an epoch here
                    bb.appendTimestamp(ChunkType::DEPRECATED_lastmod(), origCollVersion.toLong());
                    bb.done();
                }
                preCond.append(b.obj());
            }

            Status applyOpsStatus{Status::OK()};
            try {
                // For testing migration failures
                if (MONGO_FAIL_POINT(failMigrationConfigWritePrepare)) {
                    throw DBException("mock migration failure before config write",
                                      PrepareConfigsFailedCode);
                }

                applyOpsStatus = grid.catalogManager(txn)
                                     ->applyChunkOpsDeprecated(txn, updates.arr(), preCond.arr());

                if (MONGO_FAIL_POINT(failMigrationApplyOps)) {
                    throw SocketException(SocketException::RECV_ERROR,
                                          shardingState->getConfigServer(txn));
                }
            } catch (const DBException& ex) {
                warning() << ex << migrateLog;
                applyOpsStatus = ex.toStatus();
            }

            if (applyOpsStatus == ErrorCodes::PrepareConfigsFailedCode) {
                // In the process of issuing the migrate commit, the SyncClusterConnection
                // checks that the config servers are reachable. If they are not, we are
                // sure that the applyOps command was not sent to any of the configs, so we
                // can safely back out of the migration here, by resetting the shard
                // version that we bumped up to in the donateChunk() call above.

                log() << "About to acquire moveChunk coll lock to reset shard version from "
                      << "failed migration";

                {
                    ScopedTransaction transaction(txn, MODE_IX);
                    Lock::DBLock dbLock(txn->lockState(), nsToDatabaseSubstring(ns), MODE_IX);
                    Lock::CollectionLock collLock(txn->lockState(), ns, MODE_X);

                    // Revert the metadata back to the state before "forgetting"
                    // about the chunk.
                    shardingState->undoDonateChunk(txn, ns, origCollMetadata);
                }

                log() << "Shard version successfully reset to clean up failed migration";

                errmsg = "Failed to send migrate commit to configs because " + errmsg;
                return false;

            } else if (!applyOpsStatus.isOK()) {
                // this could be a blip in the connectivity
                // wait out a few seconds and check if the commit request made it
                //
                // if the commit made it to the config, we'll see the chunk in the new shard and
                // there's no action
                // if the commit did not make it, currently the only way to fix this state is to
                // bounce the mongod so that the old state (before migrating) be brought in

                warning() << "moveChunk commit outcome ongoing" << migrateLog;
                sleepsecs(10);

                // look for the chunk in this shard whose version got bumped
                // we assume that if that mod made it to the config, the applyOps was successful
                try {
                    std::vector<ChunkType> newestChunk;
                    Status status = grid.catalogManager(txn)
                                        ->getChunks(txn,
                                                    BSON(ChunkType::ns(ns)),
                                                    BSON(ChunkType::DEPRECATED_lastmod() << -1),
                                                    1,
                                                    &newestChunk,
                                                    nullptr);
                    uassertStatusOK(status);

                    ChunkVersion checkVersion;
                    if (!newestChunk.empty()) {
                        invariant(newestChunk.size() == 1);
                        checkVersion = newestChunk[0].getVersion();
                    }

                    if (checkVersion.equals(nextVersion)) {
                        log() << "moveChunk commit confirmed" << migrateLog;
                        errmsg.clear();

                    } else {
                        error() << "moveChunk commit failed: version is at " << checkVersion
                                << " instead of " << nextVersion << migrateLog;
                        error() << "TERMINATING" << migrateLog;
                        dbexit(EXIT_SHARDING_ERROR);
                    }
                } catch (...) {
                    error() << "moveChunk failed to get confirmation of commit" << migrateLog;
                    error() << "TERMINATING" << migrateLog;
                    dbexit(EXIT_SHARDING_ERROR);
                }
            }

            shardingState->migrationSourceManager()->setInCriticalSection(false);

            // 5.d
            BSONObjBuilder commitInfo;
            commitInfo.appendElements(chunkInfo);
            if (res["counts"].type() == Object) {
                commitInfo.appendElements(res["counts"].Obj());
            }

            grid.catalogManager(txn)->logChange(txn,
                                                txn->getClient()->clientAddress(true),
                                                "moveChunk.commit",
                                                ns,
                                                commitInfo.obj());
        }

        shardingState->migrationSourceManager()->done(txn);
        timing.done(5);

        MONGO_FAIL_POINT_PAUSE_WHILE_SET(moveChunkHangAtStep5);

        // 6.
        // NOTE: It is important that the distributed collection lock be held for this step.
        RangeDeleter* deleter = getDeleter();
        RangeDeleterOptions deleterOptions(
            KeyRange(ns, min.getOwned(), max.getOwned(), shardKeyPattern));
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
            if (!deleter->deleteNow(txn, deleterOptions, &errMsg)) {
                log() << "Error occured while performing cleanup: " << errMsg;
            }
        } else {
            log() << "forking for cleanup of chunk data" << migrateLog;

            string errMsg;
            if (!deleter->queueDelete(txn,
                                      deleterOptions,
                                      NULL,  // Don't want to be notified.
                                      &errMsg)) {
                log() << "could not queue migration cleanup: " << errMsg;
            }
        }

        timing.done(6);

        MONGO_FAIL_POINT_PAUSE_WHILE_SET(moveChunkHangAtStep6);

        return true;
    }

} moveChunkCmd;

/* -----
   below this are the "to" side commands

   command to initiate
   worker thread
     does initial clone
     pulls initial change set
     keeps pulling
     keeps state
   command to get state
   commend to "commit"
*/

/**
 * Command for initiating the recipient side of the migration to start copying data
 * from the donor shard.
 *
 * {
 *   _recvChunkStart: "namespace",
 *   congfigServer: "hostAndPort",
 *   from: "hostAndPort",
 *   fromShardName: "shardName",
 *   toShardName: "shardName",
 *   min: {},
 *   max: {},
 *   shardKeyPattern: {},
 *
 *   // optional
 *   secondaryThrottle: bool, // defaults to true
 *   writeConcern: {} // applies to individual writes.
 * }
 */
class RecvChunkStartCommand : public Command {
public:
    RecvChunkStartCommand() : Command("_recvChunkStart") {}

    void help(std::stringstream& h) const {
        h << "internal";
    }

    virtual bool slaveOk() const {
        return false;
    }

    virtual bool adminOnly() const {
        return true;
    }

    virtual bool isWriteCommandForConfigServer() const {
        return false;
    }

    virtual void addRequiredPrivileges(const std::string& dbname,
                                       const BSONObj& cmdObj,
                                       std::vector<Privilege>* out) {
        ActionSet actions;
        actions.addAction(ActionType::internal);
        out->push_back(Privilege(ResourcePattern::forClusterResource(), actions));
    }

    bool run(OperationContext* txn,
             const string&,
             BSONObj& cmdObj,
             int,
             string& errmsg,
             BSONObjBuilder& result) {
        ShardingState* const shardingState = ShardingState::get(txn);

        // Active state of TO-side migrations (MigrateStatus) is serialized by distributed
        // collection lock.
        if (shardingState->migrationDestinationManager()->getActive()) {
            errmsg = "migrate already in progress";
            return false;
        }

        // Pending deletes (for migrations) are serialized by the distributed collection lock,
        // we are sure we registered a delete for a range *before* we can migrate-in a
        // subrange.
        const size_t numDeletes = getDeleter()->getTotalDeletes();
        if (numDeletes > 0) {
            errmsg = str::stream() << "can't accept new chunks because "
                                   << " there are still " << numDeletes
                                   << " deletes from previous migration";

            warning() << errmsg;
            return false;
        }

        if (!shardingState->enabled()) {
            if (!cmdObj["configServer"].eoo()) {
                dassert(cmdObj["configServer"].type() == String);
                shardingState->initialize(txn, cmdObj["configServer"].String());
            } else {
                errmsg = str::stream()
                    << "cannot start recv'ing chunk, "
                    << "sharding is not enabled and no config server was provided";

                warning() << errmsg;
                return false;
            }
        }

        if (!cmdObj["toShardName"].eoo()) {
            dassert(cmdObj["toShardName"].type() == String);
            shardingState->setShardName(cmdObj["toShardName"].String());
        }

        string ns = cmdObj.firstElement().String();
        BSONObj min = cmdObj["min"].Obj().getOwned();
        BSONObj max = cmdObj["max"].Obj().getOwned();

        // Refresh our collection manager from the config server, we need a collection manager to
        // start registering pending chunks. We force the remote refresh here to make the behavior
        // consistent and predictable, generally we'd refresh anyway, and to be paranoid.
        ChunkVersion currentVersion;

        Status status = shardingState->refreshMetadataNow(txn, ns, &currentVersion);
        if (!status.isOK()) {
            errmsg = str::stream() << "cannot start recv'ing chunk "
                                   << "[" << min << "," << max << ")" << causedBy(status.reason());

            warning() << errmsg;
            return false;
        }

        // Process secondary throttle settings and assign defaults if necessary.
        WriteConcernOptions writeConcern;
        status = writeConcern.parseSecondaryThrottle(cmdObj, NULL);

        if (!status.isOK()) {
            if (status.code() != ErrorCodes::WriteConcernNotDefined) {
                warning() << status.toString();
                return appendCommandStatus(result, status);
            }

            writeConcern = getDefaultWriteConcern();
        } else {
            repl::ReplicationCoordinator* replCoordinator = repl::getGlobalReplicationCoordinator();

            if (replCoordinator->getReplicationMode() ==
                    repl::ReplicationCoordinator::modeMasterSlave &&
                writeConcern.shouldWaitForOtherNodes()) {
                warning() << "recvChunk cannot check if secondary throttle setting "
                          << writeConcern.toBSON()
                          << " can be enforced in a master slave configuration";
            }

            Status status = replCoordinator->checkIfWriteConcernCanBeSatisfied(writeConcern);
            if (!status.isOK() && status != ErrorCodes::NoReplicationEnabled) {
                warning() << status.toString();
                return appendCommandStatus(result, status);
            }
        }

        if (writeConcern.shouldWaitForOtherNodes() &&
            writeConcern.wTimeout == WriteConcernOptions::kNoTimeout) {
            // Don't allow no timeout.
            writeConcern.wTimeout = kDefaultWTimeoutMs;
        }

        BSONObj shardKeyPattern;
        if (cmdObj.hasField("shardKeyPattern")) {
            shardKeyPattern = cmdObj["shardKeyPattern"].Obj().getOwned();
        } else {
            // shardKeyPattern may not be provided if another shard is from pre 2.2
            // In that case, assume the shard key pattern is the same as the range
            // specifiers provided.
            BSONObj keya = Helpers::inferKeyPattern(min);
            BSONObj keyb = Helpers::inferKeyPattern(max);
            verify(keya == keyb);

            warning()
                << "No shard key pattern provided by source shard for migration."
                   " This is likely because the source shard is running a version prior to 2.2."
                   " Falling back to assuming the shard key matches the pattern of the min and max"
                   " chunk range specifiers.  Inferred shard key: " << keya;

            shardKeyPattern = keya.getOwned();
        }

        const string fromShard(cmdObj["from"].String());

        Status startStatus = shardingState->migrationDestinationManager()->start(
            ns, fromShard, min, max, shardKeyPattern, currentVersion.epoch(), writeConcern);

        if (!startStatus.isOK()) {
            return appendCommandStatus(result, startStatus);
        }

        result.appendBool("started", true);
        return true;
    }

} recvChunkStartCmd;

class RecvChunkStatusCommand : public Command {
public:
    RecvChunkStatusCommand() : Command("_recvChunkStatus") {}

    void help(std::stringstream& h) const {
        h << "internal";
    }

    virtual bool slaveOk() const {
        return false;
    }

    virtual bool adminOnly() const {
        return true;
    }

    virtual bool isWriteCommandForConfigServer() const {
        return false;
    }

    virtual void addRequiredPrivileges(const std::string& dbname,
                                       const BSONObj& cmdObj,
                                       std::vector<Privilege>* out) {
        ActionSet actions;
        actions.addAction(ActionType::internal);
        out->push_back(Privilege(ResourcePattern::forClusterResource(), actions));
    }

    bool run(OperationContext* txn,
             const string&,
             BSONObj& cmdObj,
             int,
             string& errmsg,
             BSONObjBuilder& result) {
        ShardingState::get(txn)->migrationDestinationManager()->report(result);
        return 1;
    }

} recvChunkStatusCommand;

class RecvChunkCommitCommand : public Command {
public:
    RecvChunkCommitCommand() : Command("_recvChunkCommit") {}

    void help(std::stringstream& h) const {
        h << "internal";
    }

    virtual bool slaveOk() const {
        return false;
    }

    virtual bool adminOnly() const {
        return true;
    }

    virtual bool isWriteCommandForConfigServer() const {
        return false;
    }

    virtual void addRequiredPrivileges(const std::string& dbname,
                                       const BSONObj& cmdObj,
                                       std::vector<Privilege>* out) {
        ActionSet actions;
        actions.addAction(ActionType::internal);
        out->push_back(Privilege(ResourcePattern::forClusterResource(), actions));
    }

    bool run(OperationContext* txn,
             const string&,
             BSONObj& cmdObj,
             int,
             string& errmsg,
             BSONObjBuilder& result) {
        bool ok = ShardingState::get(txn)->migrationDestinationManager()->startCommit();
        ShardingState::get(txn)->migrationDestinationManager()->report(result);
        return ok;
    }

} recvChunkCommitCommand;

class RecvChunkAbortCommand : public Command {
public:
    RecvChunkAbortCommand() : Command("_recvChunkAbort") {}

    void help(std::stringstream& h) const {
        h << "internal";
    }

    virtual bool slaveOk() const {
        return false;
    }

    virtual bool adminOnly() const {
        return true;
    }

    virtual bool isWriteCommandForConfigServer() const {
        return false;
    }

    virtual void addRequiredPrivileges(const std::string& dbname,
                                       const BSONObj& cmdObj,
                                       std::vector<Privilege>* out) {
        ActionSet actions;
        actions.addAction(ActionType::internal);
        out->push_back(Privilege(ResourcePattern::forClusterResource(), actions));
    }

    bool run(OperationContext* txn,
             const string&,
             BSONObj& cmdObj,
             int,
             string& errmsg,
             BSONObjBuilder& result) {
        ShardingState::get(txn)->migrationDestinationManager()->abort();
        ShardingState::get(txn)->migrationDestinationManager()->report(result);
        return true;
    }

} recvChunkAbortCommand;

void logOpForSharding(OperationContext* txn,
                      const char* opstr,
                      const char* ns,
                      const BSONObj& obj,
                      BSONObj* patt,
                      bool notInActiveChunk) {
    ShardingState::get(txn)->migrationSourceManager()->logOp(
        txn, opstr, ns, obj, patt, notInActiveChunk);
}

}  // namespace mongo
