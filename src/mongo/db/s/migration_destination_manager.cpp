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
#include "mongo/db/s/collection_range_deleter.h"
#include "mongo/db/s/collection_sharding_state.h"
#include "mongo/db/s/migration_util.h"
#include "mongo/db/s/move_timing_helper.h"
#include "mongo/db/s/sharding_state.h"
#include "mongo/db/service_context.h"
#include "mongo/s/catalog/type_chunk.h"
#include "mongo/s/shard_key_pattern.h"
#include "mongo/stdx/chrono.h"
#include "mongo/util/concurrency/notification.h"
#include "mongo/util/fail_point_service.h"
#include "mongo/util/log.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {

using std::string;
using str::stream;

namespace {

const WriteConcernOptions kMajorityWriteConcern(WriteConcernOptions::kMajority,
                                                // Note: Even though we're setting UNSET here,
                                                // kMajority implies JOURNAL if journaling is
                                                // supported by mongod and
                                                // writeConcernMajorityJournalDefault is set to true
                                                // in the ReplSetConfig.
                                                WriteConcernOptions::SyncMode::UNSET,
                                                -1);

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
bool willOverrideLocalId(OperationContext* opCtx,
                         const NamespaceString& nss,
                         BSONObj min,
                         BSONObj max,
                         BSONObj shardKeyPattern,
                         Database* db,
                         BSONObj remoteDoc,
                         BSONObj* localDoc) {
    *localDoc = BSONObj();
    if (Helpers::findById(opCtx, db, nss.ns(), remoteDoc, *localDoc)) {
        return !isInRange(*localDoc, min, max, shardKeyPattern);
    }

    return false;
}

/**
 * Returns true if the majority of the nodes and the nodes corresponding to the given writeConcern
 * (if not empty) have applied till the specified lastOp.
 */
bool opReplicatedEnough(OperationContext* opCtx,
                        const repl::OpTime& lastOpApplied,
                        const WriteConcernOptions& writeConcern) {
    WriteConcernResult writeConcernResult;

    Status waitForMajorityWriteConcernStatus =
        waitForWriteConcern(opCtx, lastOpApplied, kMajorityWriteConcern, &writeConcernResult);
    if (!waitForMajorityWriteConcernStatus.isOK()) {
        return false;
    }

    // Enforce the user specified write concern after "majority" so it covers the union of the 2
    // write concerns in case the user's write concern is stronger than majority
    WriteConcernOptions userWriteConcern(writeConcern);
    userWriteConcern.wTimeout = -1;

    Status waitForUserWriteConcernStatus =
        waitForWriteConcern(opCtx, lastOpApplied, userWriteConcern, &writeConcernResult);
    if (!waitForUserWriteConcernStatus.isOK()) {
        return false;
    }

    return true;
}

/**
 * Create the migration clone request BSON object to send to the source shard.
 *
 * 'sessionId' unique identifier for this migration.
 */
BSONObj createMigrateCloneRequest(const NamespaceString& nss, const MigrationSessionId& sessionId) {
    BSONObjBuilder builder;
    builder.append("_migrateClone", nss.ns());
    sessionId.append(&builder);
    return builder.obj();
}

/**
 * Create the migration transfer mods request BSON object to send to the source shard.
 *
 * 'sessionId' unique identifier for this migration.
 */
BSONObj createTransferModsRequest(const NamespaceString& nss, const MigrationSessionId& sessionId) {
    BSONObjBuilder builder;
    builder.append("_transferMods", nss.ns());
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

void MigrationDestinationManager::setStateFail(std::string msg) {
    log() << msg;
    {
        stdx::lock_guard<stdx::mutex> sl(_mutex);
        _errmsg = std::move(msg);
        _state = FAIL;
    }
}

void MigrationDestinationManager::setStateFailWarn(std::string msg) {
    warning() << msg;
    {
        stdx::lock_guard<stdx::mutex> sl(_mutex);
        _errmsg = std::move(msg);
        _state = FAIL;
    }
}

bool MigrationDestinationManager::isActive() const {
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    return _isActive_inlock();
}

bool MigrationDestinationManager::_isActive_inlock() const {
    return _sessionId.is_initialized();
}

void MigrationDestinationManager::report(BSONObjBuilder& b) {
    stdx::lock_guard<stdx::mutex> sl(_mutex);

    b.appendBool("active", _sessionId.is_initialized());

    if (_sessionId) {
        b.append("sessionId", _sessionId->toString());
    }

    b.append("ns", _nss.ns());
    b.append("from", _fromShardConnString.toString());
    b.append("min", _min);
    b.append("max", _max);
    b.append("shardKeyPattern", _shardKeyPattern);

    b.append("state", stateToString(_state));

    if (_state == FAIL) {
        invariant(!_errmsg.empty());
        b.append("errmsg", _errmsg);
    }

    BSONObjBuilder bb(b.subobjStart("counts"));
    bb.append("cloned", _numCloned);
    bb.append("clonedBytes", _clonedBytes);
    bb.append("catchup", _numCatchup);
    bb.append("steady", _numSteady);
    bb.done();
}

BSONObj MigrationDestinationManager::getMigrationStatusReport() {
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    if (_isActive_inlock()) {
        return migrationutil::makeMigrationStatusDocument(
            _nss, _fromShard, _toShard, false, _min, _max);
    } else {
        return BSONObj();
    }
}

Status MigrationDestinationManager::start(const NamespaceString& nss,
                                          ScopedRegisterReceiveChunk scopedRegisterReceiveChunk,
                                          const MigrationSessionId& sessionId,
                                          const ConnectionString& fromShardConnString,
                                          const ShardId& fromShard,
                                          const ShardId& toShard,
                                          const BSONObj& min,
                                          const BSONObj& max,
                                          const BSONObj& shardKeyPattern,
                                          const OID& epoch,
                                          const WriteConcernOptions& writeConcern) {
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    invariant(!_sessionId);
    invariant(!_scopedRegisterReceiveChunk);

    _state = READY;
    _errmsg = "";

    _nss = nss;
    _fromShardConnString = fromShardConnString;
    _fromShard = fromShard;
    _toShard = toShard;
    _min = min;
    _max = max;
    _shardKeyPattern = shardKeyPattern;

    _chunkMarkedPending = false;

    _numCloned = 0;
    _clonedBytes = 0;
    _numCatchup = 0;
    _numSteady = 0;

    _sessionId = sessionId;
    _scopedRegisterReceiveChunk = std::move(scopedRegisterReceiveChunk);

    // TODO: If we are here, the migrate thread must have completed, otherwise _active above
    // would be false, so this would never block. There is no better place with the current
    // implementation where to join the thread.
    if (_migrateThreadHandle.joinable()) {
        _migrateThreadHandle.join();
    }

    _migrateThreadHandle =
        stdx::thread([this, min, max, shardKeyPattern, fromShardConnString, epoch, writeConcern]() {
            _migrateThread(min, max, shardKeyPattern, fromShardConnString, epoch, writeConcern);
        });

    return Status::OK();
}

Status MigrationDestinationManager::abort(const MigrationSessionId& sessionId) {

    stdx::lock_guard<stdx::mutex> sl(_mutex);

    if (!_sessionId) {
        return Status::OK();
    }

    if (!_sessionId->matches(sessionId)) {
        return {ErrorCodes::CommandFailed,
                str::stream() << "received abort request from a stale session "
                              << sessionId.toString()
                              << ". Current session is "
                              << _sessionId->toString()};
    }

    _state = ABORT;
    _errmsg = "aborted";

    return Status::OK();
}

void MigrationDestinationManager::abortWithoutSessionIdCheck() {
    stdx::lock_guard<stdx::mutex> sl(_mutex);
    _state = ABORT;
    _errmsg = "aborted without session id check";
}

Status MigrationDestinationManager::startCommit(const MigrationSessionId& sessionId) {

    stdx::unique_lock<stdx::mutex> lock(_mutex);

    if (_state != STEADY) {
        return {ErrorCodes::CommandFailed,
                str::stream() << "Migration startCommit attempted when not in STEADY state."
                              << " Sender's session is "
                              << sessionId.toString()
                              << (_sessionId ? (". Current session is " + _sessionId->toString())
                                             : ". No active session on this shard.")};
    }

    // In STEADY state we must have active migration
    invariant(_sessionId);

    // This check guards against the (unusual) situation where the current donor shard has stalled,
    // during which the recipient shard crashed or timed out, and then began serving as a recipient
    // or donor for another migration.
    if (!_sessionId->matches(sessionId)) {
        return {ErrorCodes::CommandFailed,
                str::stream() << "startCommit received commit request from a stale session "
                              << sessionId.toString()
                              << ". Current session is "
                              << _sessionId->toString()};
    }

    _state = COMMIT_START;

    auto const deadline = Date_t::now() + Seconds(30);
    while (_sessionId) {
        if (stdx::cv_status::timeout ==
            _isActiveCV.wait_until(lock, deadline.toSystemTimePoint())) {
            _errmsg = str::stream() << "startCommit timed out waiting, " << _sessionId->toString();
            _state = FAIL;
            return {ErrorCodes::CommandFailed, _errmsg};
        }
    }
    if (_state != DONE) {
        return {ErrorCodes::CommandFailed, "startCommit failed, final data failed to transfer"};
    }
    return Status::OK();
}

void MigrationDestinationManager::_migrateThread(BSONObj min,
                                                 BSONObj max,
                                                 BSONObj shardKeyPattern,
                                                 ConnectionString fromShardConnString,
                                                 OID epoch,
                                                 WriteConcernOptions writeConcern) {
    Client::initThread("migrateThread");
    auto opCtx = getGlobalServiceContext()->makeOperationContext(&cc());

    if (getGlobalAuthorizationManager()->isAuthEnabled()) {
        AuthorizationSession::get(opCtx->getClient())->grantInternalAuthorization();
    }

    try {
        _migrateDriver(
            opCtx.get(), min, max, shardKeyPattern, fromShardConnString, epoch, writeConcern);
    } catch (std::exception& e) {
        setStateFail(str::stream() << "migrate failed: " << redact(e.what()));
    } catch (...) {
        setStateFail("migrate failed with unknown exception: UNKNOWN ERROR");
    }

    if (getState() != DONE) {
        _forgetPending(opCtx.get(), _nss, epoch, ChunkRange(min, max));
    }

    stdx::lock_guard<stdx::mutex> lk(_mutex);
    _sessionId.reset();
    _scopedRegisterReceiveChunk.reset();
    _isActiveCV.notify_all();
}

void MigrationDestinationManager::_migrateDriver(OperationContext* opCtx,
                                                 const BSONObj& min,
                                                 const BSONObj& max,
                                                 const BSONObj& shardKeyPattern,
                                                 const ConnectionString& fromShardConnString,
                                                 const OID& epoch,
                                                 const WriteConcernOptions& writeConcern) {
    invariant(isActive());
    invariant(_sessionId);
    invariant(_scopedRegisterReceiveChunk);
    invariant(!min.isEmpty());
    invariant(!max.isEmpty());

    log() << "Starting receiving end of migration of chunk " << redact(min) << " -> " << redact(max)
          << " for collection " << _nss.ns() << " from " << fromShardConnString << " at epoch "
          << epoch.toString() << " with session id " << *_sessionId;

    MoveTimingHelper timing(
        opCtx, "to", _nss.ns(), min, max, 6 /* steps */, &_errmsg, ShardId(), ShardId());

    const auto initialState = getState();

    if (initialState == ABORT) {
        error() << "Migration abort requested before it started";
        return;
    }

    invariant(initialState == READY);

    ScopedDbConnection conn(fromShardConnString);

    // Just tests the connection
    conn->getLastError();

    DisableDocumentValidation validationDisabler(opCtx);

    std::vector<BSONObj> indexSpecs;
    BSONObj idIndexSpec;
    {
        auto indexes = conn->getIndexSpecs(_nss.ns());
        for (auto&& spec : indexes) {
            indexSpecs.push_back(spec);
            if (auto indexNameElem = spec[IndexDescriptor::kIndexNameFieldName]) {
                if (indexNameElem.type() == BSONType::String &&
                    indexNameElem.valueStringData() == "_id_"_sd) {
                    idIndexSpec = spec;
                }
            }
        }
    }

    {
        // 0. copy system.namespaces entry if collection doesn't already exist

        OldClientWriteContext ctx(opCtx, _nss.ns());
        if (!repl::getGlobalReplicationCoordinator()->canAcceptWritesFor(opCtx, _nss)) {
            setStateFailWarn(str::stream() << "Not primary during migration: " << _nss.ns()
                                           << ": checking if collection exists");
            return;
        }

        // Only copy if ns doesn't already exist
        Database* const db = ctx.db();

        Collection* const collection = db->getCollection(opCtx, _nss);
        if (!collection) {
            std::list<BSONObj> infos =
                conn->getCollectionInfos(_nss.db().toString(), BSON("name" << _nss.coll()));

            BSONObj options;
            if (infos.size() > 0) {
                BSONObj entry = infos.front();
                if (entry["options"].isABSONObj()) {
                    options = entry["options"].Obj();
                }
            }

            WriteUnitOfWork wuow(opCtx);
            const bool createDefaultIndexes = true;
            Status status = userCreateNS(opCtx,
                                         db,
                                         _nss.ns(),
                                         options,
                                         CollectionOptions::parseForCommand,
                                         createDefaultIndexes,
                                         idIndexSpec);
            if (!status.isOK()) {
                warning() << "failed to create collection [" << _nss << "] "
                          << " with options " << options << ": " << redact(status);
            }
            wuow.commit();
        }
    }

    {
        // 1. copy indexes

        Lock::DBLock lk(opCtx, _nss.db(), MODE_X);
        OldClientContext ctx(opCtx, _nss.ns());

        if (!repl::getGlobalReplicationCoordinator()->canAcceptWritesFor(opCtx, _nss)) {
            setStateFailWarn(str::stream() << "Not primary during migration: " << _nss.ns());
            return;
        }

        Database* db = ctx.db();
        Collection* collection = db->getCollection(opCtx, _nss);
        if (!collection) {
            setStateFailWarn(str::stream() << "collection dropped during migration: " << _nss.ns());
            return;
        }

        MultiIndexBlock indexer(opCtx, collection);
        indexer.removeExistingIndexes(&indexSpecs);

        if (!indexSpecs.empty()) {
            // Only copy indexes if the collection does not have any documents.
            if (collection->numRecords(opCtx) > 0) {
                setStateFailWarn(str::stream() << "aborting migration, shard is missing "
                                               << indexSpecs.size()
                                               << " indexes and "
                                               << "collection is not empty. Non-trivial "
                                               << "index creation should be scheduled manually");
                return;
            }

            auto indexInfoObjs = indexer.init(indexSpecs);
            if (!indexInfoObjs.isOK()) {
                setStateFailWarn(str::stream() << "failed to create index before migrating data. "
                                               << " error: "
                                               << redact(indexInfoObjs.getStatus()));
                return;
            }

            auto status = indexer.insertAllDocumentsInCollection();
            if (!status.isOK()) {
                setStateFailWarn(str::stream() << "failed to create index before migrating data. "
                                               << " error: "
                                               << redact(status));
                return;
            }

            WriteUnitOfWork wunit(opCtx);
            indexer.commit();

            for (auto&& infoObj : indexInfoObjs.getValue()) {
                // make sure to create index on secondaries as well
                getGlobalServiceContext()->getOpObserver()->onCreateIndex(
                    opCtx, collection->ns(), collection->uuid(), infoObj, true /* fromMigrate */);
            }

            wunit.commit();
        }

        timing.done(1);
        MONGO_FAIL_POINT_PAUSE_WHILE_SET(migrateThreadHangAtStep1);
    }

    {
        // 2. Synchronously delete any data which might have been left orphaned in the range
        // being moved, and wait for completion

        auto footprint = ChunkRange(min, max);
        Status status = _notePending(opCtx, _nss, epoch, footprint);
        if (!status.isOK()) {
            setStateFail(status.reason());
            return;
        }

        _chunkMarkedPending = true;  // no lock needed, only the migrate thread looks.

        status = CollectionShardingState::waitForClean(opCtx, _nss, epoch, footprint);
        if (!status.isOK()) {
            setStateFail(status.reason());
            return;
        }

        timing.done(2);
        MONGO_FAIL_POINT_PAUSE_WHILE_SET(migrateThreadHangAtStep2);
    }

    {
        // 3. Initial bulk clone
        setState(CLONE);

        const BSONObj migrateCloneRequest = createMigrateCloneRequest(_nss, *_sessionId);

        while (true) {
            BSONObj res;
            if (!conn->runCommand("admin",
                                  migrateCloneRequest,
                                  res)) {  // gets array of objects to copy, in disk order
                setStateFail(str::stream() << "_migrateClone failed: " << redact(res.toString()));
                conn.done();
                return;
            }

            BSONObj arr = res["objects"].Obj();
            int thisTime = 0;

            BSONObjIterator i(arr);
            while (i.more()) {
                opCtx->checkForInterrupt();

                if (getState() == ABORT) {
                    log() << "Migration aborted while copying documents";
                    return;
                }

                BSONObj docToClone = i.next().Obj();
                {
                    OldClientWriteContext cx(opCtx, _nss.ns());

                    BSONObj localDoc;
                    if (willOverrideLocalId(opCtx,
                                            _nss,
                                            min,
                                            max,
                                            shardKeyPattern,
                                            cx.db(),
                                            docToClone,
                                            &localDoc)) {
                        string errMsg = str::stream() << "cannot migrate chunk, local document "
                                                      << redact(localDoc)
                                                      << " has same _id as cloned "
                                                      << "remote document " << redact(docToClone);

                        warning() << errMsg;

                        // Exception will abort migration cleanly
                        uasserted(16976, errMsg);
                    }

                    Helpers::upsert(opCtx, _nss.ns(), docToClone, true);
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
                            opCtx,
                            repl::ReplClientInfo::forClient(opCtx->getClient()).getLastOp(),
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
    repl::OpTime lastOpApplied = repl::ReplClientInfo::forClient(opCtx->getClient()).getLastOp();

    const BSONObj xferModsRequest = createTransferModsRequest(_nss, *_sessionId);

    {
        // 4. Do bulk of mods
        setState(CATCHUP);

        while (true) {
            BSONObj res;
            if (!conn->runCommand("admin", xferModsRequest, res)) {
                setStateFail(str::stream() << "_transferMods failed: " << redact(res));
                conn.done();
                return;
            }

            if (res["size"].number() == 0) {
                break;
            }

            _applyMigrateOp(opCtx, _nss, min, max, shardKeyPattern, res, &lastOpApplied);

            const int maxIterations = 3600 * 50;

            int i;
            for (i = 0; i < maxIterations; i++) {
                opCtx->checkForInterrupt();

                if (getState() == ABORT) {
                    log() << "Migration aborted while waiting for replication at catch up stage";
                    return;
                }

                if (opReplicatedEnough(opCtx, lastOpApplied, writeConcern))
                    break;

                if (i > 100) {
                    log() << "secondaries having hard time keeping up with migrate";
                }

                sleepmillis(20);
            }

            if (i == maxIterations) {
                setStateFail("secondary can't keep up with migrate");
                conn.done();
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
            opCtx->checkForInterrupt();

            if (getState() == ABORT) {
                log() << "Migration aborted while waiting for replication";
                return;
            }

            log() << "Waiting for replication to catch up before entering critical section";

            if (_flushPendingWrites(opCtx, _nss.ns(), min, max, lastOpApplied, writeConcern)) {
                break;
            }

            sleepsecs(1);
        }

        if (t.minutes() >= 600) {
            setStateFail("Cannot go to critical section because secondaries cannot keep up");
            return;
        }
    }

    {
        // 5. Wait for commit
        setState(STEADY);

        bool transferAfterCommit = false;
        while (getState() == STEADY || getState() == COMMIT_START) {
            opCtx->checkForInterrupt();

            // Make sure we do at least one transfer after recv'ing the commit message. If we
            // aren't sure that at least one transfer happens *after* our state changes to
            // COMMIT_START, there could be mods still on the FROM shard that got logged
            // *after* our _transferMods but *before* the critical section.
            if (getState() == COMMIT_START) {
                transferAfterCommit = true;
            }

            BSONObj res;
            if (!conn->runCommand("admin", xferModsRequest, res)) {
                setStateFail(str::stream() << "_transferMods failed in STEADY state: "
                                           << redact(res));
                conn.done();
                return;
            }

            if (res["size"].number() > 0 &&
                _applyMigrateOp(opCtx, _nss, min, max, shardKeyPattern, res, &lastOpApplied)) {
                continue;
            }

            if (getState() == ABORT) {
                log() << "Migration aborted while transferring mods";
                return;
            }

            // We know we're finished when:
            // 1) The from side has told us that it has locked writes (COMMIT_START)
            // 2) We've checked at least one more time for un-transmitted mods
            if (getState() == COMMIT_START && transferAfterCommit == true) {
                if (_flushPendingWrites(opCtx, _nss.ns(), min, max, lastOpApplied, writeConcern)) {
                    break;
                }
            }

            // Only sleep if we aren't committing
            if (getState() == STEADY)
                sleepmillis(10);
        }

        if (getState() == FAIL) {
            setStateFail("timed out waiting for commit");
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

bool MigrationDestinationManager::_applyMigrateOp(OperationContext* opCtx,
                                                  const NamespaceString& nss,
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
        Lock::DBLock dlk(opCtx, nss.db(), MODE_IX);
        Helpers::RemoveSaver rs("moveChunk", nss.ns(), "removedDuring");

        BSONObjIterator i(xfer["deleted"].Obj());  // deleted documents
        while (i.more()) {
            Lock::CollectionLock clk(opCtx->lockState(), nss.ns(), MODE_X);
            OldClientContext ctx(opCtx, nss.ns());

            BSONObj id = i.next().Obj();

            // do not apply delete if doc does not belong to the chunk being migrated
            BSONObj fullObj;
            if (Helpers::findById(opCtx, ctx.db(), nss.ns(), id, fullObj)) {
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

            deleteObjects(opCtx,
                          ctx.db() ? ctx.db()->getCollection(opCtx, nss) : nullptr,
                          nss,
                          id,
                          true /* justOne */,
                          false /* god */,
                          true /* fromMigrate */);

            *lastOpApplied = repl::ReplClientInfo::forClient(opCtx->getClient()).getLastOp();
            didAnything = true;
        }
    }

    if (xfer["reload"].isABSONObj()) {  // modified documents (insert/update)
        BSONObjIterator i(xfer["reload"].Obj());
        while (i.more()) {
            OldClientWriteContext cx(opCtx, nss.ns());

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
                    opCtx, nss, min, max, shardKeyPattern, cx.db(), updatedDoc, &localDoc)) {
                string errMsg = str::stream() << "cannot migrate chunk, local document " << localDoc
                                              << " has same _id as reloaded remote document "
                                              << updatedDoc;

                warning() << errMsg;

                // Exception will abort migration cleanly
                uasserted(16977, errMsg);
            }

            // We are in write lock here, so sure we aren't killing
            Helpers::upsert(opCtx, nss.ns(), updatedDoc, true);

            *lastOpApplied = repl::ReplClientInfo::forClient(opCtx->getClient()).getLastOp();
            didAnything = true;
        }
    }

    return didAnything;
}

bool MigrationDestinationManager::_flushPendingWrites(OperationContext* opCtx,
                                                      const std::string& ns,
                                                      BSONObj min,
                                                      BSONObj max,
                                                      const repl::OpTime& lastOpApplied,
                                                      const WriteConcernOptions& writeConcern) {
    if (!opReplicatedEnough(opCtx, lastOpApplied, writeConcern)) {
        repl::OpTime op(lastOpApplied);
        OCCASIONALLY log() << "migrate commit waiting for a majority of slaves for '" << ns << "' "
                           << redact(min) << " -> " << redact(max) << " waiting for: " << op;
        return false;
    }

    log() << "migrate commit succeeded flushing to secondaries for '" << ns << "' " << min << " -> "
          << max;

    return true;
}

Status MigrationDestinationManager::_notePending(OperationContext* opCtx,
                                                 NamespaceString const& nss,
                                                 OID const& epoch,
                                                 ChunkRange const& range) {
    AutoGetCollection autoColl(opCtx, nss, MODE_IX, MODE_X);
    auto css = CollectionShardingState::get(opCtx, nss);
    auto metadata = css->getMetadata();

    // This can currently happen because drops aren't synchronized with in-migrations.  The idea
    // for checking this here is that in the future we shouldn't have this problem.
    if (!metadata || metadata->getCollVersion().epoch() != epoch) {
        return {ErrorCodes::StaleShardVersion,
                str::stream() << "not noting chunk " << redact(range.toString())
                              << " as pending because the epoch of "
                              << nss.ns()
                              << " changed"};
    }

    // start clearing any leftovers that would be in the new chunk
    if (!css->beginReceive(range)) {
        return {ErrorCodes::RangeOverlapConflict,
                str::stream() << "Collection " << nss.ns() << " range " << redact(range.toString())
                              << " migration aborted; documents in range may still be in use on the"
                                 " destination shard."};
    }
    return Status::OK();
}

void MigrationDestinationManager::_forgetPending(OperationContext* opCtx,
                                                 const NamespaceString& nss,
                                                 OID const& epoch,
                                                 ChunkRange const& range) {

    if (!_chunkMarkedPending) {  // (no lock needed, only the migrate thread looks at this.)
        return;  // no documents can have been moved in, so there is nothing to clean up.
    }

    AutoGetCollection autoColl(opCtx, nss, MODE_IX, MODE_X);
    auto css = CollectionShardingState::get(opCtx, nss);
    auto metadata = css->getMetadata();

    // This can currently happen because drops aren't synchronized with in-migrations. The idea for
    // checking this here is that in the future we shouldn't have this problem.
    if (!metadata || metadata->getCollVersion().epoch() != epoch) {
        log() << "no need to forget pending chunk " << redact(range.toString())
              << " because the epoch for " << nss.ns() << " changed";
        return;
    }

    css->forgetReceive(range);
}

}  // namespace mongo
