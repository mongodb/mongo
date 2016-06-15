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

#include "mongo/db/s/migration_destination_manager.h"

#include <list>
#include <vector>

#include "mongo/client/connpool.h"
#include "mongo/db/auth/authorization_manager_global.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/db/catalog/database.h"
#include "mongo/db/catalog/document_validation.h"
#include "mongo/db/catalog/index_create.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/dbhelpers.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/ops/delete.h"
#include "mongo/db/range_deleter_service.h"
#include "mongo/db/repl/repl_client_info.h"
#include "mongo/db/repl/replication_coordinator_global.h"
#include "mongo/db/s/collection_metadata.h"
#include "mongo/db/s/collection_sharding_state.h"
#include "mongo/db/s/move_timing_helper.h"
#include "mongo/db/s/sharded_connection_info.h"
#include "mongo/db/s/sharding_state.h"
#include "mongo/db/service_context.h"
#include "mongo/db/storage/mmap_v1/dur.h"
#include "mongo/logger/ramlog.h"
#include "mongo/s/catalog/type_chunk.h"
#include "mongo/s/shard_key_pattern.h"
#include "mongo/stdx/chrono.h"
#include "mongo/util/fail_point_service.h"
#include "mongo/util/log.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {

using std::string;
using str::stream;

namespace {

Tee* migrateLog = RamLog::get("migrate");

/**
 * Returns a human-readabale name of the migration manager's state.
 */
string stateToString(MigrationDestinationManager::State state) {
    switch (state) {
        case MigrationDestinationManager::READY:
            return "ready";
        case MigrationDestinationManager::CLONE:
            return "clone";
        case MigrationDestinationManager::CATCHUP:
            return "catchup";
        case MigrationDestinationManager::STEADY:
            return "steady";
        case MigrationDestinationManager::COMMIT_START:
            return "commitStart";
        case MigrationDestinationManager::DONE:
            return "done";
        case MigrationDestinationManager::FAIL:
            return "fail";
        case MigrationDestinationManager::ABORT:
            return "abort";
        default:
            MONGO_UNREACHABLE;
    }
}

bool isInRange(const BSONObj& obj,
               const BSONObj& min,
               const BSONObj& max,
               const BSONObj& shardKeyPattern) {
    ShardKeyPattern shardKey(shardKeyPattern);
    BSONObj k = shardKey.extractShardKeyFromDoc(obj);
    return k.woCompare(min) >= 0 && k.woCompare(max) < 0;
}

/**
 * Checks if an upsert of a remote document will override a local document with the same _id but in
 * a different range on this shard. Must be in WriteContext to avoid races and DBHelper errors.
 *
 * TODO: Could optimize this check out if sharding on _id.
 */
bool willOverrideLocalId(OperationContext* txn,
                         const string& ns,
                         BSONObj min,
                         BSONObj max,
                         BSONObj shardKeyPattern,
                         Database* db,
                         BSONObj remoteDoc,
                         BSONObj* localDoc) {
    *localDoc = BSONObj();
    if (Helpers::findById(txn, db, ns.c_str(), remoteDoc, *localDoc)) {
        return !isInRange(*localDoc, min, max, shardKeyPattern);
    }

    return false;
}

/**
 * Returns true if the majority of the nodes and the nodes corresponding to the given writeConcern
 * (if not empty) have applied till the specified lastOp.
 */
bool opReplicatedEnough(OperationContext* txn,
                        const repl::OpTime& lastOpApplied,
                        const WriteConcernOptions& writeConcern) {
    WriteConcernOptions majorityWriteConcern;
    majorityWriteConcern.wTimeout = -1;
    majorityWriteConcern.wMode = WriteConcernOptions::kMajority;
    Status majorityStatus = repl::getGlobalReplicationCoordinator()
                                ->awaitReplication(txn, lastOpApplied, majorityWriteConcern)
                                .status;

    if (!writeConcern.shouldWaitForOtherNodes()) {
        return majorityStatus.isOK();
    }

    // Enforce the user specified write concern after "majority" so it covers the union of the 2
    // write concerns
    WriteConcernOptions userWriteConcern(writeConcern);
    userWriteConcern.wTimeout = -1;
    Status userStatus = repl::getGlobalReplicationCoordinator()
                            ->awaitReplication(txn, lastOpApplied, userWriteConcern)
                            .status;

    return majorityStatus.isOK() && userStatus.isOK();
}

/**
 * Create the migration clone request BSON object to send to the source
 * shard.
 *
 * 'sessionId' unique identifier for this migration.
 */
BSONObj createMigrateCloneRequest(const MigrationSessionId& sessionId) {
    BSONObjBuilder builder;
    builder.append("_migrateClone", 1);
    sessionId.append(&builder);
    return builder.obj();
}

/**
 * Create the migration transfer mods request BSON object to send to the source
 * shard.
 *
 * 'sessionId' unique identifier for this migration.
 */
BSONObj createTransferModsRequest(const MigrationSessionId& sessionId) {
    BSONObjBuilder builder;
    builder.append("_transferMods", 1);
    sessionId.append(&builder);
    return builder.obj();
}

// Enabling / disabling these fail points pauses / resumes MigrateStatus::_go(), the thread which
// receives a chunk migration from the donor.
MONGO_FP_DECLARE(migrateThreadHangAtStep1);
MONGO_FP_DECLARE(migrateThreadHangAtStep2);
MONGO_FP_DECLARE(migrateThreadHangAtStep3);
MONGO_FP_DECLARE(migrateThreadHangAtStep4);
MONGO_FP_DECLARE(migrateThreadHangAtStep5);
MONGO_FP_DECLARE(migrateThreadHangAtStep6);

MONGO_FP_DECLARE(failMigrationReceivedOutOfRangeOperation);

}  // namespace

MigrationDestinationManager::MigrationDestinationManager() = default;

MigrationDestinationManager::~MigrationDestinationManager() = default;

MigrationDestinationManager::State MigrationDestinationManager::getState() const {
    stdx::lock_guard<stdx::mutex> sl(_mutex);
    return _state;
}

void MigrationDestinationManager::setState(State newState) {
    stdx::lock_guard<stdx::mutex> sl(_mutex);
    _state = newState;
}

bool MigrationDestinationManager::isActive() const {
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    return _sessionId.is_initialized();
}

void MigrationDestinationManager::report(BSONObjBuilder& b) {
    stdx::lock_guard<stdx::mutex> sl(_mutex);

    b.appendBool("active", _sessionId.is_initialized());

    if (_sessionId) {
        b.append("sessionId", _sessionId->toString());
    }

    b.append("ns", _ns);
    b.append("from", _from);
    b.append("min", _min);
    b.append("max", _max);
    b.append("shardKeyPattern", _shardKeyPattern);

    b.append("state", stateToString(_state));

    if (_state == FAIL) {
        b.append("errmsg", _errmsg);
    }

    BSONObjBuilder bb(b.subobjStart("counts"));
    bb.append("cloned", _numCloned);
    bb.append("clonedBytes", _clonedBytes);
    bb.append("catchup", _numCatchup);
    bb.append("steady", _numSteady);
    bb.done();
}

Status MigrationDestinationManager::start(const string& ns,
                                          const MigrationSessionId& sessionId,
                                          const string& fromShard,
                                          const BSONObj& min,
                                          const BSONObj& max,
                                          const BSONObj& shardKeyPattern,
                                          const OID& epoch,
                                          const WriteConcernOptions& writeConcern) {
    stdx::lock_guard<stdx::mutex> lk(_mutex);

    if (_sessionId) {
        return Status(ErrorCodes::ConflictingOperationInProgress,
                      str::stream() << "Active migration already in progress "
                                    << "ns: "
                                    << _ns
                                    << ", from: "
                                    << _from
                                    << ", min: "
                                    << _min
                                    << ", max: "
                                    << _max);
    }

    _state = READY;
    _errmsg = "";

    _ns = ns;
    _from = fromShard;
    _min = min;
    _max = max;
    _shardKeyPattern = shardKeyPattern;

    _chunkMarkedPending = false;

    _numCloned = 0;
    _clonedBytes = 0;
    _numCatchup = 0;
    _numSteady = 0;

    _sessionId = sessionId;

    // TODO: If we are here, the migrate thread must have completed, otherwise _active above would
    // be false, so this would never block. There is no better place with the current implementation
    // where to join the thread.
    if (_migrateThreadHandle.joinable()) {
        _migrateThreadHandle.join();
    }

    _migrateThreadHandle = stdx::thread(
        [this, ns, sessionId, min, max, shardKeyPattern, fromShard, epoch, writeConcern]() {
            _migrateThread(
                ns, sessionId, min, max, shardKeyPattern, fromShard, epoch, writeConcern);
        });

    return Status::OK();
}

void MigrationDestinationManager::abort() {
    stdx::lock_guard<stdx::mutex> sl(_mutex);
    _state = ABORT;
    _errmsg = "aborted";
}

bool MigrationDestinationManager::startCommit(const MigrationSessionId& sessionId) {
    stdx::unique_lock<stdx::mutex> lock(_mutex);

    if (_state != STEADY) {
        return false;
    }

    // In STEADY state we must have active migration
    invariant(_sessionId);

    // This check guards against the (very unlikely) situation where the current donor shard has
    // been stalled for some time, during which the recipient shard crashed or timed out and started
    // serving as a recipient of chunks for another collection (note that it cannot be the same
    // collection, because the old donor still holds the collection lock).
    if (!_sessionId->matches(sessionId)) {
        warning() << "startCommit received commit request from a stale session "
                  << sessionId.toString() << ". Current session is " << _sessionId->toString();
        return false;
    }

    _state = COMMIT_START;

    const auto deadline = Date_t::now() + Seconds(30);

    while (_sessionId) {
        if (stdx::cv_status::timeout ==
            _isActiveCV.wait_until(lock, deadline.toSystemTimePoint())) {
            _state = FAIL;
            log() << "startCommit never finished!" << migrateLog;
            return false;
        }
    }

    if (_state == DONE) {
        return true;
    }

    log() << "startCommit failed, final data failed to transfer" << migrateLog;
    return false;
}

void MigrationDestinationManager::_migrateThread(std::string ns,
                                                 MigrationSessionId sessionId,
                                                 BSONObj min,
                                                 BSONObj max,
                                                 BSONObj shardKeyPattern,
                                                 std::string fromShard,
                                                 OID epoch,
                                                 WriteConcernOptions writeConcern) {
    Client::initThread("migrateThread");
    auto opCtx = getGlobalServiceContext()->makeOperationContext(&cc());

    if (getGlobalAuthorizationManager()->isAuthEnabled()) {
        ShardedConnectionInfo::addHook();
        AuthorizationSession::get(opCtx->getClient())->grantInternalAuthorization();
    }

    try {
        _migrateDriver(
            opCtx.get(), ns, sessionId, min, max, shardKeyPattern, fromShard, epoch, writeConcern);
    } catch (std::exception& e) {
        {
            stdx::lock_guard<stdx::mutex> sl(_mutex);
            _state = FAIL;
            _errmsg = e.what();
        }

        error() << "migrate failed: " << e.what() << migrateLog;
    } catch (...) {
        {
            stdx::lock_guard<stdx::mutex> sl(_mutex);
            _state = FAIL;
            _errmsg = "UNKNOWN ERROR";
        }

        error() << "migrate failed with unknown exception" << migrateLog;
    }

    if (getState() != DONE) {
        // Unprotect the range if needed/possible on unsuccessful TO migration
        Status status = _forgetPending(opCtx.get(), NamespaceString(ns), min, max, epoch);
        if (!status.isOK()) {
            warning() << "Failed to remove pending range" << causedBy(status);
        }
    }

    stdx::lock_guard<stdx::mutex> lk(_mutex);
    _sessionId = boost::none;
    _isActiveCV.notify_all();
}

void MigrationDestinationManager::_migrateDriver(OperationContext* txn,
                                                 const string& ns,
                                                 const MigrationSessionId& sessionId,
                                                 const BSONObj& min,
                                                 const BSONObj& max,
                                                 const BSONObj& shardKeyPattern,
                                                 const std::string& fromShard,
                                                 const OID& epoch,
                                                 const WriteConcernOptions& writeConcern) {
    invariant(isActive());
    invariant(getState() == READY);
    invariant(!min.isEmpty());
    invariant(!max.isEmpty());

    DisableDocumentValidation validationDisabler(txn);

    log() << "starting receiving-end of migration of chunk " << min << " -> " << max
          << " for collection " << ns << " from " << fromShard << " at epoch " << epoch.toString();

    string errmsg;
    MoveTimingHelper timing(txn, "to", ns, min, max, 6 /* steps */, &errmsg, ShardId(), ShardId());

    ScopedDbConnection conn(fromShard);

    // Just tests the connection
    conn->getLastError();

    const NamespaceString nss(ns);

    {
        // 0. copy system.namespaces entry if collection doesn't already exist
        OldClientWriteContext ctx(txn, ns);
        if (!repl::getGlobalReplicationCoordinator()->canAcceptWritesFor(nss)) {
            errmsg = str::stream() << "Not primary during migration: " << ns
                                   << ": checking if collection exists";
            warning() << errmsg;
            setState(FAIL);
            return;
        }

        // Only copy if ns doesn't already exist
        Database* const db = ctx.db();

        Collection* const collection = db->getCollection(ns);
        if (!collection) {
            std::list<BSONObj> infos = conn->getCollectionInfos(
                nsToDatabase(ns), BSON("name" << nsToCollectionSubstring(ns)));

            BSONObj options;
            if (infos.size() > 0) {
                BSONObj entry = infos.front();
                if (entry["options"].isABSONObj()) {
                    options = entry["options"].Obj();
                }
            }

            WriteUnitOfWork wuow(txn);
            Status status = userCreateNS(txn, db, ns, options, false);
            if (!status.isOK()) {
                warning() << "failed to create collection [" << ns << "] "
                          << " with options " << options << ": " << status;
            }
            wuow.commit();
        }
    }

    {
        // 1. copy indexes

        std::vector<BSONObj> indexSpecs;

        {
            const std::list<BSONObj> indexes = conn->getIndexSpecs(ns);
            indexSpecs.insert(indexSpecs.begin(), indexes.begin(), indexes.end());
        }

        ScopedTransaction scopedXact(txn, MODE_IX);
        Lock::DBLock lk(txn->lockState(), nsToDatabaseSubstring(ns), MODE_X);
        OldClientContext ctx(txn, ns);

        if (!repl::getGlobalReplicationCoordinator()->canAcceptWritesFor(nss)) {
            errmsg = str::stream() << "Not primary during migration: " << ns;
            warning() << errmsg;
            setState(FAIL);
            return;
        }

        Database* db = ctx.db();
        Collection* collection = db->getCollection(ns);
        if (!collection) {
            errmsg = str::stream() << "collection dropped during migration: " << ns;
            warning() << errmsg;
            setState(FAIL);
            return;
        }

        MultiIndexBlock indexer(txn, collection);
        indexer.removeExistingIndexes(&indexSpecs);

        if (!indexSpecs.empty()) {
            // Only copy indexes if the collection does not have any documents.
            if (collection->numRecords(txn) > 0) {
                errmsg = str::stream() << "aborting migration, shard is missing "
                                       << indexSpecs.size() << " indexes and "
                                       << "collection is not empty. Non-trivial "
                                       << "index creation should be scheduled manually";
                warning() << errmsg;
                setState(FAIL);
                return;
            }

            Status status = indexer.init(indexSpecs);
            if (!status.isOK()) {
                errmsg = str::stream() << "failed to create index before migrating data. "
                                       << " error: " << status.toString();
                warning() << errmsg;
                setState(FAIL);
                return;
            }

            status = indexer.insertAllDocumentsInCollection();
            if (!status.isOK()) {
                errmsg = str::stream() << "failed to create index before migrating data. "
                                       << " error: " << status.toString();
                warning() << errmsg;
                setState(FAIL);
                return;
            }

            WriteUnitOfWork wunit(txn);
            indexer.commit();

            for (size_t i = 0; i < indexSpecs.size(); i++) {
                // make sure to create index on secondaries as well
                getGlobalServiceContext()->getOpObserver()->onCreateIndex(
                    txn, db->getSystemIndexesName(), indexSpecs[i], true /* fromMigrate */);
            }

            wunit.commit();
        }

        timing.done(1);

        MONGO_FAIL_POINT_PAUSE_WHILE_SET(migrateThreadHangAtStep1);
    }

    {
        // 2. delete any data already in range

        RangeDeleterOptions deleterOptions(
            KeyRange(ns, min.getOwned(), max.getOwned(), shardKeyPattern));
        deleterOptions.writeConcern = writeConcern;

        // No need to wait since all existing cursors will filter out this range when returning the
        // results
        deleterOptions.waitForOpenCursors = false;
        deleterOptions.fromMigrate = true;
        deleterOptions.onlyRemoveOrphanedDocs = true;
        deleterOptions.removeSaverReason = "preCleanup";

        string errMsg;

        if (!getDeleter()->deleteNow(txn, deleterOptions, &errMsg)) {
            warning() << "Failed to queue delete for migrate abort: " << errMsg;
            setState(FAIL);
            return;
        }

        Status status = _notePending(txn, NamespaceString(ns), min, max, epoch);
        if (!status.isOK()) {
            warning() << errmsg;
            setState(FAIL);
            return;
        }

        timing.done(2);

        MONGO_FAIL_POINT_PAUSE_WHILE_SET(migrateThreadHangAtStep2);
    }

    State currentState = getState();
    if (currentState == FAIL || currentState == ABORT) {
        string errMsg;
        RangeDeleterOptions deleterOptions(
            KeyRange(ns, min.getOwned(), max.getOwned(), shardKeyPattern));
        deleterOptions.writeConcern = writeConcern;
        // No need to wait since all existing cursors will filter out this range when
        // returning the results.
        deleterOptions.waitForOpenCursors = false;
        deleterOptions.fromMigrate = true;
        deleterOptions.onlyRemoveOrphanedDocs = true;

        if (!getDeleter()->queueDelete(txn, deleterOptions, NULL /* notifier */, &errMsg)) {
            warning() << "Failed to queue delete for migrate abort: " << errMsg;
        }
    }

    {
        // 3. Initial bulk clone
        setState(CLONE);

        const BSONObj migrateCloneRequest = createMigrateCloneRequest(sessionId);

        while (true) {
            BSONObj res;
            if (!conn->runCommand("admin",
                                  migrateCloneRequest,
                                  res)) {  // gets array of objects to copy, in disk order
                setState(FAIL);
                errmsg = "_migrateClone failed: ";
                errmsg += res.toString();
                error() << errmsg << migrateLog;
                conn.done();
                return;
            }

            BSONObj arr = res["objects"].Obj();
            int thisTime = 0;

            BSONObjIterator i(arr);
            while (i.more()) {
                txn->checkForInterrupt();

                if (getState() == ABORT) {
                    errmsg = str::stream() << "Migration abort requested while "
                                           << "copying documents";
                    error() << errmsg << migrateLog;
                    return;
                }

                BSONObj docToClone = i.next().Obj();
                {
                    OldClientWriteContext cx(txn, ns);

                    BSONObj localDoc;
                    if (willOverrideLocalId(
                            txn, ns, min, max, shardKeyPattern, cx.db(), docToClone, &localDoc)) {
                        string errMsg = str::stream() << "cannot migrate chunk, local document "
                                                      << localDoc << " has same _id as cloned "
                                                      << "remote document " << docToClone;

                        warning() << errMsg;

                        // Exception will abort migration cleanly
                        uasserted(16976, errMsg);
                    }

                    Helpers::upsert(txn, ns, docToClone, true);
                }
                thisTime++;

                {
                    stdx::lock_guard<stdx::mutex> statsLock(_mutex);
                    _numCloned++;
                    _clonedBytes += docToClone.objsize();
                }

                if (writeConcern.shouldWaitForOtherNodes()) {
                    repl::ReplicationCoordinator::StatusAndDuration replStatus =
                        repl::getGlobalReplicationCoordinator()->awaitReplication(
                            txn,
                            repl::ReplClientInfo::forClient(txn->getClient()).getLastOp(),
                            writeConcern);
                    if (replStatus.status.code() == ErrorCodes::WriteConcernFailed) {
                        warning() << "secondaryThrottle on, but doc insert timed out; "
                                     "continuing";
                    } else {
                        massertStatusOK(replStatus.status);
                    }
                }
            }

            if (thisTime == 0)
                break;
        }

        timing.done(3);

        MONGO_FAIL_POINT_PAUSE_WHILE_SET(migrateThreadHangAtStep3);
    }

    // If running on a replicated system, we'll need to flush the docs we cloned to the
    // secondaries
    repl::OpTime lastOpApplied = repl::ReplClientInfo::forClient(txn->getClient()).getLastOp();

    const BSONObj xferModsRequest = createTransferModsRequest(sessionId);

    {
        // 4. Do bulk of mods
        setState(CATCHUP);

        while (true) {
            BSONObj res;
            if (!conn->runCommand("admin", xferModsRequest, res)) {
                setState(FAIL);
                errmsg = "_transferMods failed: ";
                errmsg += res.toString();
                error() << "_transferMods failed: " << res << migrateLog;
                conn.done();
                return;
            }

            if (res["size"].number() == 0) {
                break;
            }

            _applyMigrateOp(txn, ns, min, max, shardKeyPattern, res, &lastOpApplied);

            const int maxIterations = 3600 * 50;

            int i;
            for (i = 0; i < maxIterations; i++) {
                txn->checkForInterrupt();

                if (getState() == ABORT) {
                    errmsg = str::stream() << "Migration abort requested while waiting "
                                           << "for replication at catch up stage";
                    error() << errmsg << migrateLog;

                    return;
                }

                if (opReplicatedEnough(txn, lastOpApplied, writeConcern))
                    break;

                if (i > 100) {
                    warning() << "secondaries having hard time keeping up with migrate"
                              << migrateLog;
                }

                sleepmillis(20);
            }

            if (i == maxIterations) {
                errmsg = "secondary can't keep up with migrate";
                error() << errmsg << migrateLog;
                conn.done();
                setState(FAIL);
                return;
            }
        }

        timing.done(4);

        MONGO_FAIL_POINT_PAUSE_WHILE_SET(migrateThreadHangAtStep4);
    }

    {
        // Pause to wait for replication. This will prevent us from going into critical section
        // until we're ready.
        Timer t;
        while (t.minutes() < 600) {
            txn->checkForInterrupt();

            if (getState() == ABORT) {
                errmsg = "Migration abort requested while waiting for replication";
                error() << errmsg << migrateLog;
                return;
            }

            log() << "Waiting for replication to catch up before entering critical section";

            if (_flushPendingWrites(txn, ns, min, max, lastOpApplied, writeConcern)) {
                break;
            }

            sleepsecs(1);
        }

        if (t.minutes() >= 600) {
            setState(FAIL);
            errmsg = "Cannot go to critical section because secondaries cannot keep up";
            error() << errmsg << migrateLog;
            return;
        }
    }

    {
        // 5. Wait for commit
        setState(STEADY);

        bool transferAfterCommit = false;
        while (getState() == STEADY || getState() == COMMIT_START) {
            txn->checkForInterrupt();

            // Make sure we do at least one transfer after recv'ing the commit message. If we aren't
            // sure that at least one transfer happens *after* our state changes to COMMIT_START,
            // there could be mods still on the FROM shard that got logged *after* our _transferMods
            // but *before* the critical section.
            if (getState() == COMMIT_START) {
                transferAfterCommit = true;
            }

            BSONObj res;
            if (!conn->runCommand("admin", xferModsRequest, res)) {
                log() << "_transferMods failed in STEADY state: " << res << migrateLog;
                errmsg = res.toString();
                setState(FAIL);
                conn.done();
                return;
            }

            if (res["size"].number() > 0 &&
                _applyMigrateOp(txn, ns, min, max, shardKeyPattern, res, &lastOpApplied)) {
                continue;
            }

            if (getState() == ABORT) {
                return;
            }

            // We know we're finished when:
            // 1) The from side has told us that it has locked writes (COMMIT_START)
            // 2) We've checked at least one more time for un-transmitted mods
            if (getState() == COMMIT_START && transferAfterCommit == true) {
                if (_flushPendingWrites(txn, ns, min, max, lastOpApplied, writeConcern)) {
                    break;
                }
            }

            // Only sleep if we aren't committing
            if (getState() == STEADY)
                sleepmillis(10);
        }

        if (getState() == FAIL) {
            errmsg = "timed out waiting for commit";
            return;
        }

        timing.done(5);

        MONGO_FAIL_POINT_PAUSE_WHILE_SET(migrateThreadHangAtStep5);
    }

    setState(DONE);
    timing.done(6);
    MONGO_FAIL_POINT_PAUSE_WHILE_SET(migrateThreadHangAtStep6);
    conn.done();
}

bool MigrationDestinationManager::_applyMigrateOp(OperationContext* txn,
                                                  const string& ns,
                                                  const BSONObj& min,
                                                  const BSONObj& max,
                                                  const BSONObj& shardKeyPattern,
                                                  const BSONObj& xfer,
                                                  repl::OpTime* lastOpApplied) {
    repl::OpTime dummy;
    if (lastOpApplied == NULL) {
        lastOpApplied = &dummy;
    }

    bool didAnything = false;

    if (xfer["deleted"].isABSONObj()) {
        ScopedTransaction scopedXact(txn, MODE_IX);
        Lock::DBLock dlk(txn->lockState(), nsToDatabaseSubstring(ns), MODE_IX);
        Helpers::RemoveSaver rs("moveChunk", ns, "removedDuring");

        BSONObjIterator i(xfer["deleted"].Obj());  // deleted documents
        while (i.more()) {
            Lock::CollectionLock clk(txn->lockState(), ns, MODE_X);
            OldClientContext ctx(txn, ns);

            BSONObj id = i.next().Obj();

            // do not apply delete if doc does not belong to the chunk being migrated
            BSONObj fullObj;
            if (Helpers::findById(txn, ctx.db(), ns.c_str(), id, fullObj)) {
                if (!isInRange(fullObj, min, max, shardKeyPattern)) {
                    if (MONGO_FAIL_POINT(failMigrationReceivedOutOfRangeOperation)) {
                        invariant(0);
                    }
                    continue;
                }
            }

            if (serverGlobalParams.moveParanoia) {
                rs.goingToDelete(fullObj);
            }

            deleteObjects(txn,
                          ctx.db() ? ctx.db()->getCollection(ns) : nullptr,
                          ns,
                          id,
                          PlanExecutor::YIELD_MANUAL,
                          true /* justOne */,
                          false /* god */,
                          true /* fromMigrate */);

            *lastOpApplied = repl::ReplClientInfo::forClient(txn->getClient()).getLastOp();
            didAnything = true;
        }
    }

    if (xfer["reload"].isABSONObj()) {  // modified documents (insert/update)
        BSONObjIterator i(xfer["reload"].Obj());
        while (i.more()) {
            OldClientWriteContext cx(txn, ns);

            BSONObj updatedDoc = i.next().Obj();

            // do not apply insert/update if doc does not belong to the chunk being migrated
            if (!isInRange(updatedDoc, min, max, shardKeyPattern)) {
                if (MONGO_FAIL_POINT(failMigrationReceivedOutOfRangeOperation)) {
                    invariant(0);
                }
                continue;
            }

            BSONObj localDoc;
            if (willOverrideLocalId(
                    txn, ns, min, max, shardKeyPattern, cx.db(), updatedDoc, &localDoc)) {
                string errMsg = str::stream() << "cannot migrate chunk, local document " << localDoc
                                              << " has same _id as reloaded remote document "
                                              << updatedDoc;

                warning() << errMsg;

                // Exception will abort migration cleanly
                uasserted(16977, errMsg);
            }

            // We are in write lock here, so sure we aren't killing
            Helpers::upsert(txn, ns, updatedDoc, true);

            *lastOpApplied = repl::ReplClientInfo::forClient(txn->getClient()).getLastOp();
            didAnything = true;
        }
    }

    return didAnything;
}

bool MigrationDestinationManager::_flushPendingWrites(OperationContext* txn,
                                                      const std::string& ns,
                                                      BSONObj min,
                                                      BSONObj max,
                                                      const repl::OpTime& lastOpApplied,
                                                      const WriteConcernOptions& writeConcern) {
    if (!opReplicatedEnough(txn, lastOpApplied, writeConcern)) {
        repl::OpTime op(lastOpApplied);
        OCCASIONALLY warning() << "migrate commit waiting for a majority of slaves for '" << ns
                               << "' " << min << " -> " << max << " waiting for: " << op
                               << migrateLog;
        return false;
    }

    log() << "migrate commit succeeded flushing to secondaries for '" << ns << "' " << min << " -> "
          << max << migrateLog;

    {
        // Get global lock to wait for write to be commited to journal.
        ScopedTransaction scopedXact(txn, MODE_S);
        Lock::GlobalRead lk(txn->lockState());

        // if durability is on, force a write to journal
        if (getDur().commitNow(txn)) {
            log() << "migrate commit flushed to journal for '" << ns << "' " << min << " -> " << max
                  << migrateLog;
        }
    }

    return true;
}

Status MigrationDestinationManager::_notePending(OperationContext* txn,
                                                 const NamespaceString& nss,
                                                 const BSONObj& min,
                                                 const BSONObj& max,
                                                 const OID& epoch) {
    ScopedTransaction scopedXact(txn, MODE_IX);
    AutoGetCollection autoColl(txn, nss, MODE_IX, MODE_X);

    auto css = CollectionShardingState::get(txn, nss);
    auto metadata = css->getMetadata();

    // This can currently happen because drops aren't synchronized with in-migrations.  The idea for
    // checking this here is that in the future we shouldn't have this problem.
    if (!metadata || metadata->getCollVersion().epoch() != epoch) {
        return {ErrorCodes::StaleShardVersion,
                str::stream() << "could not note chunk "
                              << "["
                              << min
                              << ","
                              << max
                              << ")"
                              << " as pending because the epoch for "
                              << nss.ns()
                              << " has changed from "
                              << epoch
                              << " to "
                              << (metadata ? metadata->getCollVersion().epoch()
                                           : ChunkVersion::UNSHARDED().epoch())};
    }

    ChunkType chunk;
    chunk.setMin(min);
    chunk.setMax(max);

    css->setMetadata(metadata->clonePlusPending(chunk));

    stdx::lock_guard<stdx::mutex> sl(_mutex);
    invariant(!_chunkMarkedPending);
    _chunkMarkedPending = true;

    return Status::OK();
}

Status MigrationDestinationManager::_forgetPending(OperationContext* txn,
                                                   const NamespaceString& nss,
                                                   const BSONObj& min,
                                                   const BSONObj& max,
                                                   const OID& epoch) {
    {
        stdx::lock_guard<stdx::mutex> sl(_mutex);
        if (!_chunkMarkedPending) {
            return Status::OK();
        }

        _chunkMarkedPending = false;
    }

    ScopedTransaction scopedXact(txn, MODE_IX);
    AutoGetCollection autoColl(txn, nss, MODE_IX, MODE_X);

    auto css = CollectionShardingState::get(txn, nss);
    auto metadata = css->getMetadata();

    // This can currently happen because drops aren't synchronized with in-migrations. The idea for
    // checking this here is that in the future we shouldn't have this problem.
    if (!metadata || metadata->getCollVersion().epoch() != epoch) {
        return {ErrorCodes::StaleShardVersion,
                str::stream() << "no need to forget pending chunk "
                              << "["
                              << min
                              << ","
                              << max
                              << ")"
                              << " because the epoch for "
                              << nss.ns()
                              << " has changed from "
                              << epoch
                              << " to "
                              << (metadata ? metadata->getCollVersion().epoch()
                                           : ChunkVersion::UNSHARDED().epoch())};
    }

    ChunkType chunk;
    chunk.setMin(min);
    chunk.setMax(max);

    css->setMetadata(metadata->cloneMinusPending(chunk));
    return Status::OK();
}

}  // namespace mongo
