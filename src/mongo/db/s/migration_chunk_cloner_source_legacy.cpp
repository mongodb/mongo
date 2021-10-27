
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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kSharding

#include "mongo/platform/basic.h"

#include "mongo/db/s/migration_chunk_cloner_source_legacy.h"

#include "mongo/base/status.h"
#include "mongo/client/read_preference.h"
#include "mongo/db/catalog/index_catalog.h"
#include "mongo/db/catalog_raii.h"
#include "mongo/db/dbhelpers.h"
#include "mongo/db/exec/plan_stage.h"
#include "mongo/db/exec/working_set_common.h"
#include "mongo/db/index/index_access_method.h"
#include "mongo/db/index/index_descriptor.h"
#include "mongo/db/query/internal_plans.h"
#include "mongo/db/repl/optime.h"
#include "mongo/db/repl/replication_process.h"
#include "mongo/db/s/start_chunk_clone_request.h"
#include "mongo/db/server_parameters.h"
#include "mongo/db/service_context.h"
#include "mongo/executor/remote_command_request.h"
#include "mongo/executor/remote_command_response.h"
#include "mongo/executor/task_executor.h"
#include "mongo/executor/task_executor_pool.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/grid.h"
#include "mongo/util/elapsed_tracker.h"
#include "mongo/util/log.h"
#include "mongo/util/mongoutils/str.h"
#include "mongo/util/scopeguard.h"
#include "mongo/util/time_support.h"

namespace mongo {
namespace {

/**
 * The maximum percentage of untrasferred chunk mods at the end of a catch up iteration
 * that may be deferred to the next phase of the migration protocol (where new writes get blocked).
 */
MONGO_EXPORT_SERVER_PARAMETER(maxCatchUpPercentageBeforeBlockingWrites, int, 10);

const char kRecvChunkStatus[] = "_recvChunkStatus";
const char kRecvChunkCommit[] = "_recvChunkCommit";
const char kRecvChunkAbort[] = "_recvChunkAbort";

const int kMaxObjectPerChunk{250000};

bool isInRange(const BSONObj& obj,
               const BSONObj& min,
               const BSONObj& max,
               const ShardKeyPattern& shardKeyPattern) {
    BSONObj k = shardKeyPattern.extractShardKeyFromDoc(obj);
    return k.woCompare(min) >= 0 && k.woCompare(max) < 0;
}

BSONObj createRequestWithSessionId(StringData commandName,
                                   const NamespaceString& nss,
                                   const MigrationSessionId& sessionId,
                                   bool waitForSteadyOrDone = false) {
    BSONObjBuilder builder;
    builder.append(commandName, nss.ns());
    builder.append("waitForSteadyOrDone", waitForSteadyOrDone);
    sessionId.append(&builder);
    return builder.obj();
}

}  // namespace

/**
 * Used to receive invalidation notifications from operations, which delete documents.
 */
class DeleteNotificationStage final : public PlanStage {
public:
    DeleteNotificationStage(MigrationChunkClonerSourceLegacy* cloner, OperationContext* opCtx)
        : PlanStage("SHARDING_NOTIFY_DELETE", opCtx), _cloner(cloner) {}

    void doInvalidate(OperationContext* opCtx, const RecordId& dl, InvalidationType type) override {
        if (type == INVALIDATION_DELETION) {
            stdx::lock_guard<stdx::mutex> sl(_cloner->_mutex);
            _cloner->_cloneLocs.erase(dl);
        }
    }

    StageState doWork(WorkingSetID* out) override {
        MONGO_UNREACHABLE;
    }

    bool isEOF() override {
        MONGO_UNREACHABLE;
    }

    std::unique_ptr<PlanStageStats> getStats() override {
        MONGO_UNREACHABLE;
    }

    SpecificStats* getSpecificStats() const override {
        MONGO_UNREACHABLE;
    }

    StageType stageType() const override {
        return STAGE_NOTIFY_DELETE;
    }

private:
    MigrationChunkClonerSourceLegacy* const _cloner;
};

/**
 * Used to commit work for LogOpForSharding. Used to keep track of changes in documents that are
 * part of a chunk being migrated.
 */
class LogOpForShardingHandler final : public RecoveryUnit::Change {
public:
    /**
     * Invariant: idObj should belong to a document that is part of the active chunk being migrated
     */
    LogOpForShardingHandler(MigrationChunkClonerSourceLegacy* cloner,
                            const BSONObj& idObj,
                            const char op,
                            const repl::OpTime& opTime,
                            const repl::OpTime& prePostImageOpTime)
        : _cloner(cloner),
          _idObj(idObj.getOwned()),
          _op(op),
          _opTime(opTime),
          _prePostImageOpTime(prePostImageOpTime) {}

    void commit(boost::optional<Timestamp>) override {
        switch (_op) {
            case 'd': {
                stdx::lock_guard<stdx::mutex> sl(_cloner->_mutex);
                _cloner->_deleted.push_back(_idObj);
                ++_cloner->_untransferredDeletesCounter;
                _cloner->_memoryUsed += _idObj.firstElement().size() + 5;
            } break;

            case 'i':
            case 'u': {
                stdx::lock_guard<stdx::mutex> sl(_cloner->_mutex);
                _cloner->_reload.push_back(_idObj);
                ++_cloner->_untransferredUpsertsCounter;
                _cloner->_memoryUsed += _idObj.firstElement().size() + 5;
            } break;

            default:
                MONGO_UNREACHABLE;
        }

        if (auto sessionSource = _cloner->_sessionCatalogSource.get()) {
            if (!_prePostImageOpTime.isNull()) {
                sessionSource->notifyNewWriteOpTime(_prePostImageOpTime);
            }

            if (!_opTime.isNull()) {
                sessionSource->notifyNewWriteOpTime(_opTime);
            }
        }
    }

    void rollback() override {}

private:
    MigrationChunkClonerSourceLegacy* const _cloner;
    const BSONObj _idObj;
    const char _op;
    const repl::OpTime _opTime;
    const repl::OpTime _prePostImageOpTime;
};

MigrationChunkClonerSourceLegacy::MigrationChunkClonerSourceLegacy(MoveChunkRequest request,
                                                                   const BSONObj& shardKeyPattern,
                                                                   ConnectionString donorConnStr,
                                                                   HostAndPort recipientHost)
    : _args(std::move(request)),
      _shardKeyPattern(shardKeyPattern),
      _sessionId(MigrationSessionId::generate(_args.getFromShardId().toString(),
                                              _args.getToShardId().toString())),
      _donorConnStr(std::move(donorConnStr)),
      _recipientHost(std::move(recipientHost)) {}

MigrationChunkClonerSourceLegacy::~MigrationChunkClonerSourceLegacy() {
    invariant(_state == kDone);
    invariant(!_deleteNotifyExec);
}

Status MigrationChunkClonerSourceLegacy::startClone(OperationContext* opCtx) {
    invariant(_state == kNew);
    invariant(!opCtx->lockState()->isLocked());

    auto const replCoord = repl::ReplicationCoordinator::get(opCtx);
    if (replCoord->getReplicationMode() == repl::ReplicationCoordinator::modeReplSet) {
        _sessionCatalogSource =
            stdx::make_unique<SessionCatalogMigrationSource>(opCtx, _args.getNss());

        // Prime up the session migration source if there are oplog entries to migrate.
        _sessionCatalogSource->fetchNextOplog(opCtx);
    }

    // Load the ids of the currently available documents
    auto storeCurrentLocsStatus = _storeCurrentLocs(opCtx);
    if (!storeCurrentLocsStatus.isOK()) {
        return storeCurrentLocsStatus;
    }

    // Tell the recipient shard to start cloning
    BSONObjBuilder cmdBuilder;
    StartChunkCloneRequest::appendAsCommand(&cmdBuilder,
                                            _args.getNss(),
                                            _sessionId,
                                            _donorConnStr,
                                            _args.getFromShardId(),
                                            _args.getToShardId(),
                                            _args.getMinKey(),
                                            _args.getMaxKey(),
                                            _shardKeyPattern.toBSON(),
                                            _args.getSecondaryThrottle());

    auto startChunkCloneResponseStatus = _callRecipient(cmdBuilder.obj());
    if (!startChunkCloneResponseStatus.isOK()) {
        return startChunkCloneResponseStatus.getStatus();
    }

    // TODO (Kal): Setting the state to kCloning below means that if cancelClone was called we will
    // send a cancellation command to the recipient. The reason to limit the cases when we send
    // cancellation is for backwards compatibility with 3.2 nodes, which cannot differentiate
    // between cancellations for different migration sessions. It is thus possible that a second
    // migration from different donor, but the same recipient would certainly abort an already
    // running migration.
    stdx::lock_guard<stdx::mutex> sl(_mutex);
    _state = kCloning;

    return Status::OK();
}

Status MigrationChunkClonerSourceLegacy::awaitUntilCriticalSectionIsAppropriate(
    OperationContext* opCtx, Milliseconds maxTimeToWait) {
    invariant(_state == kCloning);
    invariant(!opCtx->lockState()->isLocked());

    const auto startTime = Date_t::now();

    int iteration = 0;
    while ((Date_t::now() - startTime) < maxTimeToWait) {
        auto responseStatus = _callRecipient(
            createRequestWithSessionId(kRecvChunkStatus, _args.getNss(), _sessionId, true));
        if (!responseStatus.isOK()) {
            return responseStatus.getStatus().withContext(
                "Failed to contact recipient shard to monitor data transfer");
        }

        const BSONObj& res = responseStatus.getValue();

        if (!res["waited"].boolean()) {
            sleepmillis(1LL << std::min(iteration, 10));
        }
        iteration++;

        stdx::lock_guard<stdx::mutex> sl(_mutex);

        const std::size_t cloneLocsRemaining = _cloneLocs.size();

        log() << "moveChunk data transfer progress: " << redact(res) << " mem used: " << _memoryUsed
              << " documents remaining to clone: " << cloneLocsRemaining;

        if (res["state"].String() == "steady") {
            if (cloneLocsRemaining != 0) {
                return {ErrorCodes::OperationIncomplete,
                        str::stream() << "Unable to enter critical section because the recipient "
                                         "shard thinks all data is cloned while there are still "
                                      << cloneLocsRemaining
                                      << " documents remaining"};
            }

            return Status::OK();
        }

        bool supportsCriticalSectionDuringCatchUp = false;
        if (auto featureSupportedField =
                res[StartChunkCloneRequest::kSupportsCriticalSectionDuringCatchUp]) {
            if (!featureSupportedField.booleanSafe()) {
                return {ErrorCodes::Error(563070),
                        str::stream()
                            << "Illegal value for "
                            << StartChunkCloneRequest::kSupportsCriticalSectionDuringCatchUp};
            }
            supportsCriticalSectionDuringCatchUp = true;
        }

        if (res["state"].String() == "catchup" && supportsCriticalSectionDuringCatchUp) {
            int64_t estimatedUntransferredModsSize =
                _untransferredDeletesCounter * _averageObjectIdSize +
                _untransferredUpsertsCounter * _averageObjectSizeForCloneLocs;
            auto estimatedUntransferredChunkPercentage =
                (std::min(_args.getMaxChunkSizeBytes(), estimatedUntransferredModsSize) * 100) /
                _args.getMaxChunkSizeBytes();
            if (estimatedUntransferredChunkPercentage <
                maxCatchUpPercentageBeforeBlockingWrites.load()) {
                // The recipient is sufficiently caught-up with the writes on the donor.
                // Block writes, so that it can drain everything.
                return Status::OK();
            }
        }

        if (res["state"].String() == "fail") {
            return {ErrorCodes::OperationFailed,
                    str::stream() << "Data transfer error: " << res["errmsg"].str()};
        }

        auto migrationSessionIdStatus = MigrationSessionId::extractFromBSON(res);
        if (!migrationSessionIdStatus.isOK()) {
            return {ErrorCodes::OperationIncomplete,
                    str::stream() << "Unable to retrieve the id of the migration session due to "
                                  << migrationSessionIdStatus.getStatus().toString()};
        }

        if (res["ns"].str() != _args.getNss().ns() ||
            (res.hasField("fromShardId")
                 ? (res["fromShardId"].str() != _args.getFromShardId().toString())
                 : (res["from"].str() != _donorConnStr.toString())) ||
            !res["min"].isABSONObj() || res["min"].Obj().woCompare(_args.getMinKey()) != 0 ||
            !res["max"].isABSONObj() || res["max"].Obj().woCompare(_args.getMaxKey()) != 0 ||
            !_sessionId.matches(migrationSessionIdStatus.getValue())) {
            // This can happen when the destination aborted the migration and received another
            // recvChunk before this thread sees the transition to the abort state. This is
            // currently possible only if multiple migrations are happening at once. This is an
            // unfortunate consequence of the shards not being able to keep track of multiple
            // incoming and outgoing migrations.
            return {ErrorCodes::OperationIncomplete,
                    "Destination shard aborted migration because a new one is running"};
        }

        if (_memoryUsed > 500 * 1024 * 1024) {
            // This is too much memory for us to use so we're going to abort the migration
            return {ErrorCodes::ExceededMemoryLimit,
                    "Aborting migration because of high memory usage"};
        }

        Status interruptStatus = opCtx->checkForInterruptNoAssert();
        if (!interruptStatus.isOK()) {
            return interruptStatus;
        }
    }

    return {ErrorCodes::ExceededTimeLimit, "Timed out waiting for the cloner to catch up"};
}

StatusWith<BSONObj> MigrationChunkClonerSourceLegacy::commitClone(OperationContext* opCtx) {
    invariant(_state == kCloning);
    invariant(!opCtx->lockState()->isLocked());

    if (_sessionCatalogSource) {
        _sessionCatalogSource->onCommitCloneStarted();
    }

    auto responseStatus =
        _callRecipient(createRequestWithSessionId(kRecvChunkCommit, _args.getNss(), _sessionId));

    if (responseStatus.isOK()) {
        _cleanup(opCtx);

        if (_sessionCatalogSource && _sessionCatalogSource->hasMoreOplog()) {
            return {ErrorCodes::SessionTransferIncomplete,
                    "destination shard finished committing but there are still some session "
                    "metadata that needs to be transferred"};
        }

        return responseStatus;
    }

    cancelClone(opCtx);
    return responseStatus.getStatus();
}

void MigrationChunkClonerSourceLegacy::cancelClone(OperationContext* opCtx) {
    invariant(!opCtx->lockState()->isLocked());

    if (_sessionCatalogSource) {
        _sessionCatalogSource->onCloneCleanup();
    }

    switch (_state) {
        case kDone:
            break;
        case kCloning: {
            const auto status = _callRecipient(createRequestWithSessionId(
                                                   kRecvChunkAbort, _args.getNss(), _sessionId))
                                    .getStatus();
            if (!status.isOK()) {
                LOG(0) << "Failed to cancel migration " << causedBy(redact(status));
            }
        }
        // Intentional fall through
        case kNew:
            _cleanup(opCtx);
            break;
        default:
            MONGO_UNREACHABLE;
    }
}

bool MigrationChunkClonerSourceLegacy::isDocumentInMigratingChunk(const BSONObj& doc) {
    return isInRange(doc, _args.getMinKey(), _args.getMaxKey(), _shardKeyPattern);
}

void MigrationChunkClonerSourceLegacy::onInsertOp(OperationContext* opCtx,
                                                  const BSONObj& insertedDoc,
                                                  const repl::OpTime& opTime) {
    dassert(opCtx->lockState()->isCollectionLockedForMode(_args.getNss().ns(), MODE_IX));

    BSONElement idElement = insertedDoc["_id"];
    if (idElement.eoo()) {
        warning() << "logInsertOp got a document with no _id field, ignoring inserted document: "
                  << redact(insertedDoc);
        return;
    }

    if (!isInRange(insertedDoc, _args.getMinKey(), _args.getMaxKey(), _shardKeyPattern)) {
        return;
    }

    if (opCtx->getTxnNumber()) {
        opCtx->recoveryUnit()->registerChange(
            new LogOpForShardingHandler(this, idElement.wrap(), 'i', opTime, {}));
    } else {
        opCtx->recoveryUnit()->registerChange(
            new LogOpForShardingHandler(this, idElement.wrap(), 'i', {}, {}));
    }
}

void MigrationChunkClonerSourceLegacy::onUpdateOp(OperationContext* opCtx,
                                                  const BSONObj& updatedDoc,
                                                  const repl::OpTime& opTime,
                                                  const repl::OpTime& prePostImageOpTime) {
    dassert(opCtx->lockState()->isCollectionLockedForMode(_args.getNss().ns(), MODE_IX));

    BSONElement idElement = updatedDoc["_id"];
    if (idElement.eoo()) {
        warning() << "logUpdateOp got a document with no _id field, ignoring updatedDoc: "
                  << redact(updatedDoc);
        return;
    }

    if (!isInRange(updatedDoc, _args.getMinKey(), _args.getMaxKey(), _shardKeyPattern)) {
        return;
    }

    if (opCtx->getTxnNumber()) {
        opCtx->recoveryUnit()->registerChange(
            new LogOpForShardingHandler(this, idElement.wrap(), 'u', opTime, prePostImageOpTime));
    } else {
        opCtx->recoveryUnit()->registerChange(
            new LogOpForShardingHandler(this, idElement.wrap(), 'u', {}, {}));
    }
}

void MigrationChunkClonerSourceLegacy::onDeleteOp(OperationContext* opCtx,
                                                  const BSONObj& deletedDocId,
                                                  const repl::OpTime& opTime,
                                                  const repl::OpTime& preImageOpTime) {
    dassert(opCtx->lockState()->isCollectionLockedForMode(_args.getNss().ns(), MODE_IX));

    BSONElement idElement = deletedDocId["_id"];
    if (idElement.eoo()) {
        warning() << "logDeleteOp got a document with no _id field, ignoring deleted doc: "
                  << redact(deletedDocId);
        return;
    }

    if (opCtx->getTxnNumber()) {
        opCtx->recoveryUnit()->registerChange(
            new LogOpForShardingHandler(this, idElement.wrap(), 'd', opTime, preImageOpTime));
    } else {
        opCtx->recoveryUnit()->registerChange(
            new LogOpForShardingHandler(this, idElement.wrap(), 'd', {}, {}));
    }
}

uint64_t MigrationChunkClonerSourceLegacy::getCloneBatchBufferAllocationSize() {
    stdx::lock_guard<stdx::mutex> sl(_mutex);

    return std::min(static_cast<uint64_t>(BSONObjMaxUserSize),
                    _averageObjectSizeForCloneLocs * _cloneLocs.size());
}

Status MigrationChunkClonerSourceLegacy::nextCloneBatch(OperationContext* opCtx,
                                                        Collection* collection,
                                                        BSONArrayBuilder* arrBuilder) {
    dassert(opCtx->lockState()->isCollectionLockedForMode(_args.getNss().ns(), MODE_IS));

    ElapsedTracker tracker(opCtx->getServiceContext()->getFastClockSource(),
                           internalQueryExecYieldIterations.load(),
                           Milliseconds(internalQueryExecYieldPeriodMS.load()));

    stdx::lock_guard<stdx::mutex> sl(_mutex);

    std::set<RecordId>::iterator it;

    for (it = _cloneLocs.begin(); it != _cloneLocs.end(); ++it) {
        // We must always make progress in this method by at least one document because empty return
        // indicates there is no more initial clone data.
        if (arrBuilder->arrSize() && tracker.intervalHasElapsed()) {
            break;
        }

        Snapshotted<BSONObj> doc;
        if (collection->findDoc(opCtx, *it, &doc)) {
            // Use the builder size instead of accumulating the document sizes directly so that we
            // take into consideration the overhead of BSONArray indices.
            if (arrBuilder->arrSize() &&
                (arrBuilder->len() + doc.value().objsize() + 1024) > BSONObjMaxUserSize) {
                break;
            }

            arrBuilder->append(doc.value());
        }
    }

    _cloneLocs.erase(_cloneLocs.begin(), it);

    // If we have drained all the cloned data, there is no need to keep the delete notify executor
    // around
    if (_cloneLocs.empty() && _deleteNotifyExec) {
        // We have a different OperationContext than when we created the PlanExecutor, so need to
        // manually destroy it ourselves.
        _deleteNotifyExec->dispose(opCtx, collection->getCursorManager());
        _deleteNotifyExec.reset();
    }

    return Status::OK();
}

Status MigrationChunkClonerSourceLegacy::nextModsBatch(OperationContext* opCtx,
                                                       Database* db,
                                                       BSONObjBuilder* builder) {
    dassert(opCtx->lockState()->isCollectionLockedForMode(_args.getNss().ns(), MODE_IS));

    std::list<BSONObj> deleteList;
    std::list<BSONObj> updateList;

    {
        stdx::lock_guard<stdx::mutex> sl(_mutex);

        // All clone data must have been drained before starting to fetch the incremental changes
        invariant(_cloneLocs.empty());

        // The "snapshot" for delete and update list must be taken under a single lock. This is to
        // ensure that we will preserve the causal order of writes. Always consume the delete
        // buffer first, before the update buffer. If the delete is causally before the update to
        // the same doc, then there's no problem since we consume the delete buffer first. If the
        // delete is causally after, we will not be able to see the document when we attempt to
        // fetch it, so it's also ok.
        deleteList.splice(deleteList.cbegin(), _deleted);
        updateList.splice(updateList.cbegin(), _reload);
    }

    auto totalDocSize = _xferDeletes(builder, &deleteList, 0);
    totalDocSize = _xferUpdates(opCtx, db, builder, &updateList, totalDocSize);

    builder->append("size", totalDocSize);

    // Put back remaining ids we didn't consume
    stdx::lock_guard<stdx::mutex> sl(_mutex);
    _deleted.splice(_deleted.cbegin(), deleteList);
    _untransferredDeletesCounter = _deleted.size();
    _reload.splice(_reload.cbegin(), updateList);
    _untransferredUpsertsCounter = _reload.size();

    return Status::OK();
}

void MigrationChunkClonerSourceLegacy::_cleanup(OperationContext* opCtx) {
    {
        stdx::lock_guard<stdx::mutex> sl(_mutex);
        _state = kDone;
        _reload.clear();
        _untransferredUpsertsCounter = 0;
        _deleted.clear();
        _untransferredDeletesCounter = 0;
    }
    // Implicitly resets _deleteNotifyExec to avoid possible invariant failure
    // in on destruction of MigrationChunkClonerSourceLegacy, and will always
    // call deleteNotifyExec destructor on scope exit even if something in the
    // below if statement fails
    auto deleteNotifyExec = std::move(_deleteNotifyExec);

    if (deleteNotifyExec) {
        // Don't allow an Interrupt exception to prevent _deleteNotifyExec from getting cleaned up.
        UninterruptibleLockGuard noInterrupt(opCtx->lockState());

        AutoGetCollection autoColl(opCtx, _args.getNss(), MODE_IS);
        const auto cursorManager =
            autoColl.getCollection() ? autoColl.getCollection()->getCursorManager() : nullptr;
        deleteNotifyExec->dispose(opCtx, cursorManager);
    }
}

StatusWith<BSONObj> MigrationChunkClonerSourceLegacy::_callRecipient(const BSONObj& cmdObj) {
    executor::RemoteCommandResponse responseStatus(
        Status{ErrorCodes::InternalError, "Uninitialized value"});

    auto executor = Grid::get(getGlobalServiceContext())->getExecutorPool()->getFixedExecutor();
    auto scheduleStatus = executor->scheduleRemoteCommand(
        executor::RemoteCommandRequest(_recipientHost, "admin", cmdObj, nullptr),
        [&responseStatus](const executor::TaskExecutor::RemoteCommandCallbackArgs& args) {
            responseStatus = args.response;
        });

    if (!scheduleStatus.isOK()) {
        return scheduleStatus.getStatus();
    }

    executor->wait(scheduleStatus.getValue());

    auto checkNotMasterOrNetwork = [](const Status& status) {
        return ErrorCodes::isNotMasterError(status.code()) ||
            ErrorCodes::isNetworkError(status.code()) ||
            status.code() == ErrorCodes::NetworkInterfaceExceededTimeLimit;
    };

    if (!responseStatus.isOK()) {
        if (checkNotMasterOrNetwork(responseStatus.status)) {
            // Convert the error before it's returned to load balancer's MigrationManager
            // to avoid it marking this host as having network issues.
            warning() << "Migration chunk received error from " << _recipientHost << ": "
                      << responseStatus.status << ", converting to OperationFailed";
            responseStatus.status =
                Status(ErrorCodes::OperationFailed, responseStatus.status.toString());
        }
        return responseStatus.status;
    }

    Status commandStatus = getStatusFromCommandResult(responseStatus.data);
    if (!commandStatus.isOK()) {
        if (checkNotMasterOrNetwork(commandStatus)) {
            // Convert the error before it's returned to load balancer's MigrationManager
            // to avoid it marking this host as no longer primary.
            warning() << "Migration chunk received error from " << _recipientHost << ": "
                      << commandStatus << ", converting to OperationFailed";
            commandStatus = Status(ErrorCodes::OperationFailed, commandStatus.toString());
        }
        return commandStatus;
    }

    return responseStatus.data.getOwned();
}

Status MigrationChunkClonerSourceLegacy::_storeCurrentLocs(OperationContext* opCtx) {
    AutoGetCollection autoColl(opCtx, _args.getNss(), MODE_IS);

    Collection* const collection = autoColl.getCollection();
    if (!collection) {
        return {ErrorCodes::NamespaceNotFound,
                str::stream() << "Collection " << _args.getNss().ns() << " does not exist."};
    }

    // Allow multiKey based on the invariant that shard keys must be single-valued. Therefore, any
    // multi-key index prefixed by shard key cannot be multikey over the shard key fields.
    IndexDescriptor* const shardKeyIdx =
        collection->getIndexCatalog()->findShardKeyPrefixedIndex(opCtx,
                                                                 _shardKeyPattern.toBSON(),
                                                                 false);  // requireSingleKey
    if (!shardKeyIdx) {
        return {ErrorCodes::IndexNotFound,
                str::stream() << "can't find index with prefix " << _shardKeyPattern.toBSON()
                              << " in storeCurrentLocs for "
                              << _args.getNss().ns()};
    }

    // Install the stage, which will listen for notifications on the collection
    auto statusWithDeleteNotificationPlanExecutor =
        PlanExecutor::make(opCtx,
                           stdx::make_unique<WorkingSet>(),
                           stdx::make_unique<DeleteNotificationStage>(this, opCtx),
                           collection,
                           PlanExecutor::YIELD_MANUAL);
    if (!statusWithDeleteNotificationPlanExecutor.isOK()) {
        return statusWithDeleteNotificationPlanExecutor.getStatus();
    }

    _deleteNotifyExec = std::move(statusWithDeleteNotificationPlanExecutor.getValue());

    // Assume both min and max non-empty, append MinKey's to make them fit chosen index
    const KeyPattern kp(shardKeyIdx->keyPattern());

    BSONObj min = Helpers::toKeyFormat(kp.extendRangeBound(_args.getMinKey(), false));
    BSONObj max = Helpers::toKeyFormat(kp.extendRangeBound(_args.getMaxKey(), false));

    // We can afford to yield here because any change to the base data that we might miss is already
    // being queued and will migrate in the 'transferMods' stage.
    auto exec = InternalPlanner::indexScan(opCtx,
                                           collection,
                                           shardKeyIdx,
                                           min,
                                           max,
                                           BoundInclusion::kIncludeStartKeyOnly,
                                           PlanExecutor::YIELD_AUTO);

    // Use the average object size to estimate how many objects a full chunk would carry do that
    // while traversing the chunk's range using the sharding index, below there's a fair amount of
    // slack before we determine a chunk is too large because object sizes will vary.
    unsigned long long maxRecsWhenFull;
    long long avgRecSize;

    const long long totalRecs = collection->numRecords(opCtx);
    if (totalRecs > 0) {
        avgRecSize = collection->dataSize(opCtx) / totalRecs;
        maxRecsWhenFull = _args.getMaxChunkSizeBytes() / avgRecSize;
        maxRecsWhenFull = 2 * maxRecsWhenFull;  // pad some slack
    } else {
        avgRecSize = 0;
        maxRecsWhenFull = kMaxObjectPerChunk + 1;
    }

    // Do a full traversal of the chunk and don't stop even if we think it is a large chunk we want
    // the number of records to better report, in that case.
    bool isLargeChunk = false;
    unsigned long long recCount = 0;

    BSONObj obj;
    RecordId recordId;
    PlanExecutor::ExecState state;
    while (PlanExecutor::ADVANCED == (state = exec->getNext(&obj, &recordId))) {
        Status interruptStatus = opCtx->checkForInterruptNoAssert();
        if (!interruptStatus.isOK()) {
            return interruptStatus;
        }

        if (!isLargeChunk) {
            stdx::lock_guard<stdx::mutex> lk(_mutex);
            _cloneLocs.insert(recordId);
        }

        if (++recCount > maxRecsWhenFull) {
            isLargeChunk = true;
            // Continue on despite knowing that it will fail, just to get the correct value for
            // recCount
        }
    }

    if (PlanExecutor::DEAD == state || PlanExecutor::FAILURE == state) {
        return WorkingSetCommon::getMemberObjectStatus(obj).withContext(
            "Executor error while scanning for documents belonging to chunk");
    }

    const uint64_t collectionAverageObjectSize = collection->averageObjectSize(opCtx);

    uint64_t averageObjectIdSize = 0;
    const uint64_t defaultObjectIdSize = OID::kOIDSize;
    if (totalRecs > 0) {
        const auto indexCatalog = collection->getIndexCatalog();
        const auto idIdx = indexCatalog->findIdIndex(opCtx);
        if (!idIdx) {
            return {ErrorCodes::IndexNotFound,
                    str::stream() << "can't find index '_id' in storeCurrentLocs for "
                                  << _args.getNss().ns()};
        }
        averageObjectIdSize = indexCatalog->getIndex(idIdx)->getSpaceUsedBytes(opCtx) / totalRecs;
    }

    if (isLargeChunk) {
        return {
            ErrorCodes::ChunkTooBig,
            str::stream() << "Cannot move chunk: the maximum number of documents for a chunk is "
                          << maxRecsWhenFull
                          << ", the maximum chunk size is "
                          << _args.getMaxChunkSizeBytes()
                          << ", average document size is "
                          << avgRecSize
                          << ". Found "
                          << recCount
                          << " documents in chunk "
                          << " ns: "
                          << _args.getNss().ns()
                          << " "
                          << _args.getMinKey()
                          << " -> "
                          << _args.getMaxKey()};
    }

    stdx::lock_guard<stdx::mutex> lk(_mutex);
    _averageObjectSizeForCloneLocs = collectionAverageObjectSize + defaultObjectIdSize;
    _averageObjectIdSize = std::max(averageObjectIdSize, defaultObjectIdSize);
    return Status::OK();
}

long long MigrationChunkClonerSourceLegacy::_xferDeletes(BSONObjBuilder* builder,
                                                         std::list<BSONObj>* removeList,
                                                         long long initialSize) {
    const long long maxSize = 1024 * 1024;

    if (removeList->empty() || initialSize > maxSize) {
        return initialSize;
    }

    long long totalSize = initialSize;
    BSONArrayBuilder arr(builder->subarrayStart("deleted"));

    auto docIdIter = removeList->begin();
    for (; docIdIter != removeList->end() && totalSize < maxSize; ++docIdIter) {
        BSONObj idDoc = *docIdIter;
        arr.append(idDoc);
        totalSize += idDoc.objsize();
    }

    removeList->erase(removeList->begin(), docIdIter);

    arr.done();
    return totalSize;
}

long long MigrationChunkClonerSourceLegacy::_xferUpdates(OperationContext* opCtx,
                                                         Database* db,
                                                         BSONObjBuilder* builder,
                                                         std::list<BSONObj>* updateList,
                                                         long long initialSize) {
    const long long maxSize = 1024 * 1024;

    if (updateList->empty() || initialSize > maxSize) {
        return initialSize;
    }

    const auto& nss = _args.getNss();
    BSONArrayBuilder arr(builder->subarrayStart("reload"));
    long long totalSize = initialSize;

    auto iter = updateList->begin();
    for (; iter != updateList->end() && totalSize < maxSize; ++iter) {
        auto idDoc = *iter;

        BSONObj fullDoc;
        if (Helpers::findById(opCtx, db, nss.ns().c_str(), idDoc, fullDoc)) {
            arr.append(fullDoc);
            totalSize += fullDoc.objsize();
        }
    }

    updateList->erase(updateList->begin(), iter);

    arr.done();
    return totalSize;
}

boost::optional<repl::OpTime> MigrationChunkClonerSourceLegacy::nextSessionMigrationBatch(
    OperationContext* opCtx, BSONArrayBuilder* arrBuilder) {
    repl::OpTime opTimeToWait;

    if (!_sessionCatalogSource) {
        return boost::none;
    }

    while (_sessionCatalogSource->hasMoreOplog()) {
        auto result = _sessionCatalogSource->getLastFetchedOplog();

        if (!result.oplog) {
            // Last fetched turned out empty, try to see if there are more
            _sessionCatalogSource->fetchNextOplog(opCtx);
            continue;
        }

        auto newOpTime = result.oplog->getOpTime();
        auto oplogDoc = result.oplog->toBSON();

        // Use the builder size instead of accumulating the document sizes directly so that we
        // take into consideration the overhead of BSONArray indices.
        if (arrBuilder->arrSize() &&
            (arrBuilder->len() + oplogDoc.objsize() + 1024) > BSONObjMaxUserSize) {
            break;
        }

        arrBuilder->append(oplogDoc);
        _sessionCatalogSource->fetchNextOplog(opCtx);

        if (result.shouldWaitForMajority) {
            if (opTimeToWait < newOpTime) {
                opTimeToWait = newOpTime;
            }
        }
    }

    return boost::make_optional(opTimeToWait);
}

std::shared_ptr<Notification<bool>>
MigrationChunkClonerSourceLegacy::getNotificationForNextSessionMigrationBatch() {
    if (!_sessionCatalogSource) {
        return nullptr;
    }

    return _sessionCatalogSource->getNotificationForNewOplog();
}

}  // namespace mongo
