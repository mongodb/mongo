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
#include "mongo/db/auth/authorization_manager.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/catalog/collection_catalog_entry.h"
#include "mongo/db/catalog/document_validation.h"
#include "mongo/db/catalog/index_create.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/dbhelpers.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/ops/delete.h"
#include "mongo/db/repl/repl_client_info.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/s/collection_sharding_state.h"
#include "mongo/db/s/migration_util.h"
#include "mongo/db/s/move_timing_helper.h"
#include "mongo/db/service_context.h"
#include "mongo/s/catalog/type_chunk.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/shard_key_pattern.h"
#include "mongo/stdx/chrono.h"
#include "mongo/util/concurrency/notification.h"
#include "mongo/util/fail_point_service.h"
#include "mongo/util/log.h"
#include "mongo/util/mongoutils/str.h"
#include "mongo/util/producer_consumer_queue.h"
#include "mongo/util/scopeguard.h"

namespace mongo {
namespace {

const auto getMigrationDestinationManager =
    ServiceContext::declareDecoration<MigrationDestinationManager>();

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
std::string stateToString(MigrationDestinationManager::State state) {
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
    writeConcernResult.wTimedOut = false;

    Status majorityStatus =
        waitForWriteConcern(opCtx, lastOpApplied, kMajorityWriteConcern, &writeConcernResult);
    if (!majorityStatus.isOK()) {
        if (!writeConcernResult.wTimedOut) {
            uassertStatusOK(majorityStatus);
        }
        return false;
    }

    // Enforce the user specified write concern after "majority" so it covers the union of the 2
    // write concerns in case the user's write concern is stronger than majority
    WriteConcernOptions userWriteConcern(writeConcern);
    userWriteConcern.wTimeout = -1;
    writeConcernResult.wTimedOut = false;

    Status userStatus =
        waitForWriteConcern(opCtx, lastOpApplied, userWriteConcern, &writeConcernResult);
    if (!userStatus.isOK()) {
        if (!writeConcernResult.wTimedOut) {
            uassertStatusOK(userStatus);
        }
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
MONGO_FAIL_POINT_DEFINE(migrateThreadHangAtStep1);
MONGO_FAIL_POINT_DEFINE(migrateThreadHangAtStep2);
MONGO_FAIL_POINT_DEFINE(migrateThreadHangAtStep3);
MONGO_FAIL_POINT_DEFINE(migrateThreadHangAtStep4);
MONGO_FAIL_POINT_DEFINE(migrateThreadHangAtStep5);
MONGO_FAIL_POINT_DEFINE(migrateThreadHangAtStep6);

MONGO_FAIL_POINT_DEFINE(failMigrationLeaveOrphans);
MONGO_FAIL_POINT_DEFINE(failMigrationReceivedOutOfRangeOperation);

}  // namespace

MigrationDestinationManager::MigrationDestinationManager() = default;

MigrationDestinationManager::~MigrationDestinationManager() = default;

MigrationDestinationManager* MigrationDestinationManager::get(OperationContext* opCtx) {
    return &getMigrationDestinationManager(opCtx->getServiceContext());
}

MigrationDestinationManager::State MigrationDestinationManager::getState() const {
    stdx::lock_guard<stdx::mutex> sl(_mutex);
    return _state;
}

void MigrationDestinationManager::setState(State newState) {
    stdx::lock_guard<stdx::mutex> sl(_mutex);
    _state = newState;
    _stateChangedCV.notify_all();
}

void MigrationDestinationManager::_setStateFail(StringData msg) {
    log() << msg;
    {
        stdx::lock_guard<stdx::mutex> sl(_mutex);
        _errmsg = msg.toString();
        _state = FAIL;
        _stateChangedCV.notify_all();
    }

    _sessionMigration->forceFail(msg);
}

void MigrationDestinationManager::_setStateFailWarn(StringData msg) {
    warning() << msg;
    {
        stdx::lock_guard<stdx::mutex> sl(_mutex);
        _errmsg = msg.toString();
        _state = FAIL;
        _stateChangedCV.notify_all();
    }

    _sessionMigration->forceFail(msg);
}

bool MigrationDestinationManager::isActive() const {
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    return _isActive(lk);
}

bool MigrationDestinationManager::_isActive(WithLock) const {
    return _sessionId.is_initialized();
}

void MigrationDestinationManager::report(BSONObjBuilder& b,
                                         OperationContext* opCtx,
                                         bool waitForSteadyOrDone) {
    if (waitForSteadyOrDone) {
        stdx::unique_lock<stdx::mutex> lock(_mutex);
        try {
            opCtx->waitForConditionOrInterruptFor(_stateChangedCV, lock, Seconds(1), [&]() -> bool {
                return _state != READY && _state != CLONE && _state != CATCHUP;
            });
        } catch (...) {
            // Ignoring this error because this is an optional parameter and we catch timeout
            // exceptions later.
        }
        b.append("waited", true);
    }
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
    if (_isActive(lk)) {
        return migrationutil::makeMigrationStatusDocument(
            _nss, _fromShard, _toShard, false, _min, _max);
    } else {
        return BSONObj();
    }
}

Status MigrationDestinationManager::start(const NamespaceString& nss,
                                          ScopedReceiveChunk scopedReceiveChunk,
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
    invariant(!_scopedReceiveChunk);

    _state = READY;
    _stateChangedCV.notify_all();
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
    _scopedReceiveChunk = std::move(scopedReceiveChunk);

    // TODO: If we are here, the migrate thread must have completed, otherwise _active above
    // would be false, so this would never block. There is no better place with the current
    // implementation where to join the thread.
    if (_migrateThreadHandle.joinable()) {
        _migrateThreadHandle.join();
    }

    _sessionMigration =
        stdx::make_unique<SessionCatalogMigrationDestination>(fromShard, *_sessionId);

    _migrateThreadHandle =
        stdx::thread([this, min, max, shardKeyPattern, fromShardConnString, epoch, writeConcern]() {
            _migrateThread(min, max, shardKeyPattern, fromShardConnString, epoch, writeConcern);
        });

    return Status::OK();
}

void MigrationDestinationManager::cloneDocumentsFromDonor(
    OperationContext* opCtx,
    stdx::function<void(OperationContext*, BSONObjIterator)> insertBatchFn,
    stdx::function<BSONObj(OperationContext*)> fetchBatchFn) {

    ProducerConsumerQueue<BSONObj> batches(1);
    stdx::thread inserterThread{[&] {
        Client::initThreadIfNotAlready("chunkInserter");
        auto inserterOpCtx = Client::getCurrent()->makeOperationContext();
        auto consumerGuard = MakeGuard([&] { batches.closeConsumerEnd(); });
        try {
            while (true) {
                auto nextBatch = batches.pop(inserterOpCtx.get());
                auto arr = nextBatch["objects"].Obj();
                if (arr.isEmpty()) {
                    return;
                }
                insertBatchFn(inserterOpCtx.get(), BSONObjIterator(arr));
            }
        } catch (...) {
            stdx::lock_guard<Client> lk(*opCtx->getClient());
            opCtx->getServiceContext()->killOperation(opCtx, exceptionToStatus().code());
            log() << "Batch insertion failed " << causedBy(redact(exceptionToStatus()));
        }
    }};
    auto inserterThreadJoinGuard = MakeGuard([&] {
        batches.closeProducerEnd();
        inserterThread.join();
    });

    while (true) {
        opCtx->checkForInterrupt();

        auto res = fetchBatchFn(opCtx);

        opCtx->checkForInterrupt();
        batches.push(res.getOwned(), opCtx);
        auto arr = res["objects"].Obj();
        if (arr.isEmpty()) {
            inserterThreadJoinGuard.Dismiss();
            inserterThread.join();
            opCtx->checkForInterrupt();
            break;
        }
    }
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
    _stateChangedCV.notify_all();
    _errmsg = "aborted";

    return Status::OK();
}

void MigrationDestinationManager::abortWithoutSessionIdCheck() {
    stdx::lock_guard<stdx::mutex> sl(_mutex);
    _state = ABORT;
    _stateChangedCV.notify_all();
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

    _sessionMigration->finish();
    _state = COMMIT_START;
    _stateChangedCV.notify_all();

    auto const deadline = Date_t::now() + Seconds(30);
    while (_sessionId) {
        if (stdx::cv_status::timeout ==
            _isActiveCV.wait_until(lock, deadline.toSystemTimePoint())) {
            _errmsg = str::stream() << "startCommit timed out waiting, " << _sessionId->toString();
            _state = FAIL;
            _stateChangedCV.notify_all();
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
    auto opCtx = Client::getCurrent()->makeOperationContext();


    if (AuthorizationManager::get(opCtx->getServiceContext())->isAuthEnabled()) {
        AuthorizationSession::get(opCtx->getClient())->grantInternalAuthorization();
    }

    try {
        _migrateDriver(
            opCtx.get(), min, max, shardKeyPattern, fromShardConnString, epoch, writeConcern);
    } catch (...) {
        _setStateFail(str::stream() << "migrate failed: " << redact(exceptionToStatus()));
    }

    if (getState() != DONE && !MONGO_FAIL_POINT(failMigrationLeaveOrphans)) {
        _forgetPending(opCtx.get(), _nss, epoch, ChunkRange(min, max));
    }

    stdx::lock_guard<stdx::mutex> lk(_mutex);
    _sessionId.reset();
    _scopedReceiveChunk.reset();
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
    invariant(_scopedReceiveChunk);
    invariant(!min.isEmpty());
    invariant(!max.isEmpty());

    auto const serviceContext = opCtx->getServiceContext();

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

    std::vector<BSONObj> donorIndexSpecs;
    BSONObj donorIdIndexSpec;
    BSONObj donorOptions;
    {
        // 0. Get the collection indexes and options from the donor shard.

        // Do not hold any locks while issuing remote calls.
        invariant(!opCtx->lockState()->isLocked());

        // Get indexes by calling listIndexes against the donor.
        auto indexes = conn->getIndexSpecs(_nss.ns());
        for (auto&& spec : indexes) {
            donorIndexSpecs.push_back(spec);
            if (auto indexNameElem = spec[IndexDescriptor::kIndexNameFieldName]) {
                if (indexNameElem.type() == BSONType::String &&
                    indexNameElem.valueStringData() == "_id_"_sd) {
                    donorIdIndexSpec = spec;
                }
            }
        }

        // Get collection options by calling listCollections against the donor.
        std::list<BSONObj> infos =
            conn->getCollectionInfos(_nss.db().toString(), BSON("name" << _nss.coll()));

        if (infos.size() != 1) {
            _setStateFailWarn(str::stream()
                              << "expected listCollections against the donor shard for "
                              << _nss.ns()
                              << " to return 1 entry, but got "
                              << infos.size()
                              << " entries");
            return;
        }

        BSONObj entry = infos.front();

        // The entire options include both the settable options under the 'options' field in the
        // listCollections response, and the UUID under the 'info' field.
        BSONObjBuilder donorOptionsBob;

        if (entry["options"].isABSONObj()) {
            donorOptionsBob.appendElements(entry["options"].Obj());
        }

        BSONObj info;
        if (entry["info"].isABSONObj()) {
            info = entry["info"].Obj();
        }
        if (info["uuid"].eoo()) {
            _setStateFailWarn(str::stream()
                              << "The donor shard did not return a UUID for collection "
                              << _nss.ns()
                              << " as part of its listCollections response: "
                              << entry
                              << ", but this node expects to see a UUID.");
            return;
        }
        donorOptionsBob.append(info["uuid"]);

        donorOptions = donorOptionsBob.obj();
    }

    {
        // 1. Create the collection (if it doesn't already exist) and create any indexes we are
        // missing (auto-heal indexes).

        // Hold the DBLock in X mode across creating the collection and indexes, so that a
        // concurrent dropIndex cannot run between creating the collection and indexes and fail with
        // IndexNotFound, though the index will get created.
        // We could take the DBLock in IX mode while checking if the collection already exists and
        // then upgrade it to X mode while creating the collection and indexes, but there is no way
        // to upgrade a DBLock once it's taken without releasing it, so we pre-emptively take it in
        // mode X.
        Lock::DBLock lk(opCtx, _nss.db(), MODE_X);

        OldClientWriteContext ctx(opCtx, _nss.ns());
        if (!repl::ReplicationCoordinator::get(opCtx)->canAcceptWritesFor(opCtx, _nss)) {
            _setStateFailWarn(str::stream() << "Not primary during migration: " << _nss.ns()
                                            << ": checking if collection exists");
            return;
        }

        Database* const db = ctx.db();
        Collection* collection = db->getCollection(opCtx, _nss);
        if (collection) {
            // We have an entry for a collection by this name. Check that our collection's UUID
            // matches the donor's.
            boost::optional<UUID> donorUUID;
            if (!donorOptions["uuid"].eoo()) {
                donorUUID.emplace(UUID::parse(donorOptions));
            }

            if (collection->uuid() != donorUUID) {
                _setStateFailWarn(
                    str::stream()
                    << "Cannot receive chunk "
                    << redact(ChunkRange(min, max).toString())
                    << " for collection "
                    << _nss.ns()
                    << " because we already have an identically named collection with UUID "
                    << (collection->uuid() ? collection->uuid()->toString() : "(none)")
                    << ", which differs from the donor's UUID "
                    << (donorUUID ? donorUUID->toString() : "(none)")
                    << ". Manually drop the collection on this shard if it contains data from a "
                       "previous incarnation of "
                    << _nss.ns());
                return;
            }
        } else {
            // We do not have a collection by this name. Create the collection with the donor's
            // options.
            WriteUnitOfWork wuow(opCtx);
            const bool createDefaultIndexes = true;
            Status status = Database::userCreateNS(opCtx,
                                                   db,
                                                   _nss.ns(),
                                                   donorOptions,
                                                   CollectionOptions::parseForStorage,
                                                   createDefaultIndexes,
                                                   donorIdIndexSpec);
            if (!status.isOK()) {
                warning() << "failed to create collection [" << _nss << "] "
                          << " with options " << donorOptions << ": " << redact(status);
            }
            wuow.commit();
            collection = db->getCollection(opCtx, _nss);
        }

        MultiIndexBlock indexer(opCtx, collection);
        indexer.removeExistingIndexes(&donorIndexSpecs);

        if (!donorIndexSpecs.empty()) {
            // Only copy indexes if the collection does not have any documents.
            if (collection->numRecords(opCtx) > 0) {
                _setStateFailWarn(str::stream() << "aborting migration, shard is missing "
                                                << donorIndexSpecs.size()
                                                << " indexes and "
                                                << "collection is not empty. Non-trivial "
                                                << "index creation should be scheduled manually");
                return;
            }

            auto indexInfoObjs = indexer.init(donorIndexSpecs);
            if (!indexInfoObjs.isOK()) {
                _setStateFailWarn(str::stream() << "failed to create index before migrating data. "
                                                << " error: "
                                                << redact(indexInfoObjs.getStatus()));
                return;
            }

            WriteUnitOfWork wunit(opCtx);
            indexer.commit();

            for (auto&& infoObj : indexInfoObjs.getValue()) {
                // make sure to create index on secondaries as well
                serviceContext->getOpObserver()->onCreateIndex(
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

        const ChunkRange footprint(min, max);
        auto notification = _notePending(opCtx, _nss, epoch, footprint);
        // Wait for the range deletion to report back
        if (!notification.waitStatus(opCtx).isOK()) {
            _setStateFail(redact(notification.waitStatus(opCtx).reason()));
            return;
        }

        // Wait for any other, overlapping queued deletions to drain
        auto status = CollectionShardingState::waitForClean(opCtx, _nss, epoch, footprint);
        if (!status.isOK()) {
            _setStateFail(redact(status.reason()));
            return;
        }

        timing.done(2);
        MONGO_FAIL_POINT_PAUSE_WHILE_SET(migrateThreadHangAtStep2);
    }

    {
        // 3. Initial bulk clone
        setState(CLONE);

        _sessionMigration->start(opCtx->getServiceContext());

        const BSONObj migrateCloneRequest = createMigrateCloneRequest(_nss, *_sessionId);

        _chunkMarkedPending = true;  // no lock needed, only the migrate thread looks.

        auto insertBatchFn = [&](OperationContext* opCtx, BSONObjIterator docs) {
            while (docs.more()) {
                opCtx->checkForInterrupt();

                if (getState() == ABORT) {
                    auto message = "Migration aborted while copying documents";
                    log() << message;
                    uasserted(50748, message);
                }

                BSONObj docToClone = docs.next().Obj();
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
                        const std::string errMsg = str::stream()
                            << "cannot migrate chunk, local document " << redact(localDoc)
                            << " has same _id as cloned "
                            << "remote document " << redact(docToClone);
                        warning() << errMsg;

                        // Exception will abort migration cleanly
                        uasserted(16976, errMsg);
                    }
                    Helpers::upsert(opCtx, _nss.ns(), docToClone, true);
                }
                {
                    stdx::lock_guard<stdx::mutex> statsLock(_mutex);
                    _numCloned++;
                    _clonedBytes += docToClone.objsize();
                }
                if (writeConcern.shouldWaitForOtherNodes()) {
                    repl::ReplicationCoordinator::StatusAndDuration replStatus =
                        repl::ReplicationCoordinator::get(opCtx)->awaitReplication(
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
        };

        auto fetchBatchFn = [&](OperationContext* opCtx) {
            BSONObj res;
            if (!conn->runCommand("admin",
                                  migrateCloneRequest,
                                  res)) {  // gets array of objects to copy, in disk order
                conn.done();
                const std::string errMsg = str::stream() << "_migrateClone failed: "
                                                         << redact(res.toString());
                uasserted(50747, errMsg);
            }
            return res;
        };

        cloneDocumentsFromDonor(opCtx, insertBatchFn, fetchBatchFn);

        timing.done(3);
        MONGO_FAIL_POINT_PAUSE_WHILE_SET(migrateThreadHangAtStep3);

        if (MONGO_FAIL_POINT(failMigrationLeaveOrphans)) {
            _setStateFail(str::stream() << "failing migration after cloning " << _numCloned
                                        << " docs due to failMigrationLeaveOrphans failpoint");
            return;
        }
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
                _setStateFail(str::stream() << "_transferMods failed: " << redact(res));
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
                _setStateFail("secondary can't keep up with migrate");
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

            opCtx->sleepFor(Seconds(1));
        }

        if (t.minutes() >= 600) {
            _setStateFail("Cannot go to critical section because secondaries cannot keep up");
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
                _setStateFail(str::stream() << "_transferMods failed in STEADY state: "
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
            _setStateFail("timed out waiting for commit");
            return;
        }

        timing.done(5);
        MONGO_FAIL_POINT_PAUSE_WHILE_SET(migrateThreadHangAtStep5);
    }

    _sessionMigration->join();
    if (_sessionMigration->getState() == SessionCatalogMigrationDestination::State::ErrorOccurred) {
        _setStateFail(redact(_sessionMigration->getErrMsg()));
        return;
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
                        MONGO_UNREACHABLE;
                    }
                    continue;
                }
            }

            if (serverGlobalParams.moveParanoia) {
                rs.goingToDelete(fullObj).transitional_ignore();
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
                    MONGO_UNREACHABLE;
                }
                continue;
            }

            BSONObj localDoc;
            if (willOverrideLocalId(
                    opCtx, nss, min, max, shardKeyPattern, cx.db(), updatedDoc, &localDoc)) {
                const std::string errMsg = str::stream()
                    << "cannot migrate chunk, local document " << redact(localDoc)
                    << " has same _id as reloaded remote document " << redact(updatedDoc);
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

    log() << "migrate commit succeeded flushing to secondaries for '" << ns << "' " << redact(min)
          << " -> " << redact(max);

    return true;
}

CollectionShardingState::CleanupNotification MigrationDestinationManager::_notePending(
    OperationContext* opCtx,
    NamespaceString const& nss,
    OID const& epoch,
    ChunkRange const& range) {

    AutoGetCollection autoColl(opCtx, nss, MODE_IX, MODE_X);
    auto css = CollectionShardingState::get(opCtx, nss);
    auto metadata = css->getMetadata(opCtx);

    // This can currently happen because drops aren't synchronized with in-migrations. The idea for
    // checking this here is that in the future we shouldn't have this problem.
    if (!metadata || metadata->getCollVersion().epoch() != epoch) {
        return Status{ErrorCodes::StaleShardVersion,
                      str::stream() << "not noting chunk " << redact(range.toString())
                                    << " as pending because the epoch of "
                                    << nss.ns()
                                    << " changed"};
    }

    // Start clearing any leftovers that would be in the new chunk
    auto notification = css->beginReceive(range);
    if (notification.ready() && !notification.waitStatus(opCtx).isOK()) {
        return notification.waitStatus(opCtx).withContext(
            str::stream() << "Collection " << nss.ns() << " range " << redact(range.toString())
                          << " migration aborted");
    }
    return notification;
}

void MigrationDestinationManager::_forgetPending(OperationContext* opCtx,
                                                 const NamespaceString& nss,
                                                 OID const& epoch,
                                                 ChunkRange const& range) {

    if (!_chunkMarkedPending) {  // (no lock needed, only the migrate thread looks at this.)
        return;  // no documents can have been moved in, so there is nothing to clean up.
    }

    UninterruptibleLockGuard noInterrupt(opCtx->lockState());
    AutoGetCollection autoColl(opCtx, nss, MODE_IX, MODE_X);
    auto css = CollectionShardingState::get(opCtx, nss);
    auto metadata = css->getMetadata(opCtx);

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
