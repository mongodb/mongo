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

#include "mongo/db/s/migration_chunk_cloner_source_legacy.h"

#include "mongo/base/status.h"
#include "mongo/client/read_preference.h"
#include "mongo/client/remote_command_targeter.h"
#include "mongo/db/concurrency/locker.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/dbhelpers.h"
#include "mongo/db/exec/plan_stage.h"
#include "mongo/db/exec/working_set_common.h"
#include "mongo/db/index/index_descriptor.h"
#include "mongo/db/query/internal_plans.h"
#include "mongo/db/s/start_chunk_clone_request.h"
#include "mongo/db/service_context.h"
#include "mongo/executor/remote_command_request.h"
#include "mongo/executor/remote_command_response.h"
#include "mongo/executor/task_executor.h"
#include "mongo/executor/task_executor_pool.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/s/chunk.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/grid.h"
#include "mongo/util/elapsed_tracker.h"
#include "mongo/util/log.h"
#include "mongo/util/mongoutils/str.h"
#include "mongo/util/scopeguard.h"
#include "mongo/util/time_support.h"

namespace mongo {
namespace {

const char kRecvChunkStatus[] = "_recvChunkStatus";
const char kRecvChunkCommit[] = "_recvChunkCommit";
const char kRecvChunkAbort[] = "_recvChunkAbort";

bool isInRange(const BSONObj& obj,
               const BSONObj& min,
               const BSONObj& max,
               const ShardKeyPattern& shardKeyPattern) {
    BSONObj k = shardKeyPattern.extractShardKeyFromDoc(obj);
    return k.woCompare(min) >= 0 && k.woCompare(max) < 0;
}

BSONObj createRecvChunkCommitRequest(const NamespaceString& nss,
                                     const MigrationSessionId& sessionId) {
    BSONObjBuilder builder;
    builder.append(kRecvChunkCommit, nss.ns());
    sessionId.append(&builder);
    return builder.obj();
}

}  // namespace

/**
 * Used to receive invalidation notifications from operations, which delete documents.
 */
class DeleteNotificationStage final : public PlanStage {
public:
    DeleteNotificationStage(MigrationChunkClonerSourceLegacy* cloner, OperationContext* txn)
        : PlanStage("SHARDING_NOTIFY_DELETE", txn), _cloner(cloner) {}

    void doInvalidate(OperationContext* txn, const RecordId& dl, InvalidationType type) override {
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
                            const char op)
        : _cloner(cloner), _idObj(idObj.getOwned()), _op(op) {}

    void commit() override {
        switch (_op) {
            case 'd': {
                stdx::lock_guard<stdx::mutex> sl(_cloner->_mutex);
                _cloner->_deleted.push_back(_idObj);
                _cloner->_memoryUsed += _idObj.firstElement().size() + 5;
                break;
            }

            case 'i':
            case 'u': {
                stdx::lock_guard<stdx::mutex> sl(_cloner->_mutex);
                _cloner->_reload.push_back(_idObj);
                _cloner->_memoryUsed += _idObj.firstElement().size() + 5;
                break;
            }

            default:
                MONGO_UNREACHABLE;
        }
    }

    void rollback() override {}

private:
    MigrationChunkClonerSourceLegacy* const _cloner;
    const BSONObj _idObj;
    const char _op;
};

MigrationChunkClonerSourceLegacy::MigrationChunkClonerSourceLegacy(MoveChunkRequest request,
                                                                   const BSONObj& shardKeyPattern)
    : _args(std::move(request)),
      _shardKeyPattern(shardKeyPattern),
      _sessionId(MigrationSessionId::generate(_args.getFromShardId().toString(),
                                              _args.getToShardId().toString())) {}

MigrationChunkClonerSourceLegacy::~MigrationChunkClonerSourceLegacy() {
    invariant(!_deleteNotifyExec);
}

Status MigrationChunkClonerSourceLegacy::startClone(OperationContext* txn) {
    invariant(!txn->lockState()->isLocked());
    auto scopedGuard = MakeGuard([&] { cancelClone(txn); });

    // Resolve the donor and recipient shards and their connection string

    {
        auto donorShard = grid.shardRegistry()->getShard(txn, _args.getFromShardId());
        _donorCS = donorShard->getConnString();
    }

    {
        auto recipientShard = grid.shardRegistry()->getShard(txn, _args.getToShardId());
        auto shardHostStatus = recipientShard->getTargeter()->findHost(
            ReadPreferenceSetting{ReadPreference::PrimaryOnly});
        if (!shardHostStatus.isOK()) {
            return shardHostStatus.getStatus();
        }

        _recipientHost = std::move(shardHostStatus.getValue());
    }

    // Prepare the currently available documents
    Status status = _storeCurrentLocs(txn);
    if (!status.isOK()) {
        return status;
    }

    // Tell the recipient shard to start cloning
    BSONObjBuilder cmdBuilder;
    StartChunkCloneRequest::appendAsCommand(&cmdBuilder,
                                            _args.getNss(),
                                            _sessionId,
                                            _args.getConfigServerCS(),
                                            _donorCS,
                                            _args.getToShardId(),
                                            _args.getMinKey(),
                                            _args.getMaxKey(),
                                            _shardKeyPattern.toBSON(),
                                            _args.getSecondaryThrottle());

    auto responseStatus = _callRecipient(cmdBuilder.obj());
    if (!responseStatus.isOK()) {
        return responseStatus.getStatus();
    }

    scopedGuard.Dismiss();
    return Status::OK();
}

Status MigrationChunkClonerSourceLegacy::awaitUntilCriticalSectionIsAppropriate(
    OperationContext* txn, Milliseconds maxTimeToWait) {
    invariant(!txn->lockState()->isLocked());
    auto scopedGuard = MakeGuard([&] { cancelClone(txn); });

    const auto startTime = Date_t::now();

    int iteration = 0;
    while ((Date_t::now() - startTime) < maxTimeToWait) {
        // Exponential sleep backoff, up to 1024ms. Don't sleep much on the first few iterations,
        // since we want empty chunk migrations to be fast.
        sleepmillis(1 << std::min(iteration, 10));
        iteration++;

        auto responseStatus = _callRecipient(BSON(kRecvChunkStatus << _args.getNss().ns()));
        if (!responseStatus.isOK()) {
            return {responseStatus.getStatus().code(),
                    str::stream()
                        << "Failed to contact recipient shard to monitor data transfer due to "
                        << responseStatus.getStatus().toString()};
        }

        BSONObj res = std::move(responseStatus.getValue());

        log() << "moveChunk data transfer progress: " << res << " my mem used: " << _memoryUsed;

        if (res["state"].String() == "steady") {
            // Ensure all cloned docs have actually been transferred
            const std::size_t locsRemaining = _cloneLocs.size();
            if (locsRemaining != 0) {
                return {
                    ErrorCodes::OperationIncomplete,
                    str::stream()
                        << "cannot enter critical section before all data is cloned, "
                        << locsRemaining
                        << " locs were not transferred but to-shard thinks they are all cloned"};
            }

            scopedGuard.Dismiss();
            return Status::OK();
        }

        if (res["state"].String() == "fail") {
            return {ErrorCodes::OperationFailed, "Data transfer error"};
        }

        if (res["ns"].str() != _args.getNss().ns() || res["from"].str() != _donorCS.toString() ||
            !res["min"].isABSONObj() || res["min"].Obj().woCompare(_args.getMinKey()) != 0 ||
            !res["max"].isABSONObj() || res["max"].Obj().woCompare(_args.getMaxKey()) != 0) {
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

        Status interruptStatus = txn->checkForInterruptNoAssert();
        if (!interruptStatus.isOK()) {
            return interruptStatus;
        }
    }

    scopedGuard.Dismiss();
    return {ErrorCodes::ExceededTimeLimit, "Timed out waiting for the cloner to catch up"};
}

Status MigrationChunkClonerSourceLegacy::commitClone(OperationContext* txn) {
    invariant(!txn->lockState()->isLocked());

    {
        stdx::lock_guard<stdx::mutex> sl(_mutex);
        invariant(!_cloneCompleted);
    }

    auto responseStatus = _callRecipient(createRecvChunkCommitRequest(_args.getNss(), _sessionId));
    if (responseStatus.isOK()) {
        _cleanup(txn);
        return Status::OK();
    }

    cancelClone(txn);
    return responseStatus.getStatus();
}

void MigrationChunkClonerSourceLegacy::cancelClone(OperationContext* txn) {
    invariant(!txn->lockState()->isLocked());

    {
        stdx::lock_guard<stdx::mutex> sl(_mutex);
        if (_cloneCompleted)
            return;
    }

    _callRecipient(BSON(kRecvChunkAbort << _args.getNss().ns()));
    _cleanup(txn);
}

bool MigrationChunkClonerSourceLegacy::isDocumentInMigratingChunk(OperationContext* txn,
                                                                  const BSONObj& doc) {
    return isInRange(doc, _args.getMinKey(), _args.getMaxKey(), _shardKeyPattern);
}

void MigrationChunkClonerSourceLegacy::onInsertOp(OperationContext* txn,
                                                  const BSONObj& insertedDoc) {
    dassert(txn->lockState()->isCollectionLockedForMode(_args.getNss().ns(), MODE_IX));

    BSONElement idElement = insertedDoc["_id"];
    if (idElement.eoo()) {
        warning() << "logInsertOp got a document with no _id field, ignoring inserted document: "
                  << insertedDoc;
        return;
    }

    if (!isInRange(insertedDoc, _args.getMinKey(), _args.getMaxKey(), _shardKeyPattern)) {
        return;
    }

    txn->recoveryUnit()->registerChange(new LogOpForShardingHandler(this, idElement.wrap(), 'i'));
}

void MigrationChunkClonerSourceLegacy::onUpdateOp(OperationContext* txn,
                                                  const BSONObj& updatedDoc) {
    dassert(txn->lockState()->isCollectionLockedForMode(_args.getNss().ns(), MODE_IX));

    BSONElement idElement = updatedDoc["_id"];
    if (idElement.eoo()) {
        warning() << "logUpdateOp got a document with no _id field, ignoring updatedDoc: "
                  << updatedDoc;
        return;
    }

    if (!isInRange(updatedDoc, _args.getMinKey(), _args.getMaxKey(), _shardKeyPattern)) {
        return;
    }

    txn->recoveryUnit()->registerChange(new LogOpForShardingHandler(this, idElement.wrap(), 'u'));
}

void MigrationChunkClonerSourceLegacy::onDeleteOp(OperationContext* txn,
                                                  const BSONObj& deletedDocId) {
    dassert(txn->lockState()->isCollectionLockedForMode(_args.getNss().ns(), MODE_IX));

    BSONElement idElement = deletedDocId["_id"];
    if (idElement.eoo()) {
        warning() << "logDeleteOp got a document with no _id field, ignoring deleted doc: "
                  << deletedDocId;
        return;
    }

    txn->recoveryUnit()->registerChange(new LogOpForShardingHandler(this, idElement.wrap(), 'd'));
}

uint64_t MigrationChunkClonerSourceLegacy::getCloneBatchBufferAllocationSize() {
    stdx::lock_guard<stdx::mutex> sl(_mutex);

    return std::min(static_cast<uint64_t>(BSONObjMaxUserSize),
                    _averageObjectSizeForCloneLocs * _cloneLocs.size());
}

Status MigrationChunkClonerSourceLegacy::nextCloneBatch(OperationContext* txn,
                                                        Collection* collection,
                                                        BSONArrayBuilder* arrBuilder) {
    dassert(txn->lockState()->isCollectionLockedForMode(_args.getNss().ns(), MODE_IS));

    ElapsedTracker tracker(txn->getServiceContext()->getFastClockSource(),
                           internalQueryExecYieldIterations,
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
        if (collection->findDoc(txn, *it, &doc)) {
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

    return Status::OK();
}

Status MigrationChunkClonerSourceLegacy::nextModsBatch(OperationContext* txn,
                                                       Database* db,
                                                       BSONObjBuilder* builder) {
    dassert(txn->lockState()->isCollectionLockedForMode(_args.getNss().ns(), MODE_IS));

    stdx::lock_guard<stdx::mutex> sl(_mutex);

    // All clone data must have been drained before starting to fetch the incremental changes
    invariant(_cloneLocs.empty());

    long long docSizeAccumulator = 0;

    _xfer(txn, db, &_deleted, builder, "deleted", &docSizeAccumulator, false);
    _xfer(txn, db, &_reload, builder, "reload", &docSizeAccumulator, true);

    builder->append("size", docSizeAccumulator);

    return Status::OK();
}

void MigrationChunkClonerSourceLegacy::_cleanup(OperationContext* txn) {
    {
        stdx::lock_guard<stdx::mutex> sl(_mutex);
        _cloneCompleted = true;
    }

    ScopedTransaction scopedXact(txn, MODE_IS);
    AutoGetCollection autoColl(txn, _args.getNss(), MODE_IS);

    if (_deleteNotifyExec) {
        _deleteNotifyExec.reset();
    }
}

StatusWith<BSONObj> MigrationChunkClonerSourceLegacy::_callRecipient(const BSONObj& cmdObj) {
    StatusWith<executor::RemoteCommandResponse> responseStatus(
        Status{ErrorCodes::InternalError, "Uninitialized value"});

    auto executor = grid.getExecutorPool()->getArbitraryExecutor();
    auto scheduleStatus = executor->scheduleRemoteCommand(
        executor::RemoteCommandRequest(_recipientHost, "admin", cmdObj),
        [&responseStatus](const executor::TaskExecutor::RemoteCommandCallbackArgs& args) {
            responseStatus = args.response;
        });

    // TODO: Update RemoteCommandTargeter on NotMaster errors.
    if (!scheduleStatus.isOK()) {
        return scheduleStatus.getStatus();
    }

    executor->wait(scheduleStatus.getValue());

    if (!responseStatus.isOK()) {
        return responseStatus.getStatus();
    }

    Status commandStatus = getStatusFromCommandResult(responseStatus.getValue().data);
    if (!commandStatus.isOK()) {
        return commandStatus;
    }

    return responseStatus.getValue().data.getOwned();
}

Status MigrationChunkClonerSourceLegacy::_storeCurrentLocs(OperationContext* txn) {
    ScopedTransaction scopedXact(txn, MODE_IS);
    AutoGetCollection autoColl(txn, _args.getNss(), MODE_IS);

    Collection* const collection = autoColl.getCollection();
    if (!collection) {
        return {ErrorCodes::NamespaceNotFound,
                str::stream() << "Collection " << _args.getNss().ns() << " does not exist."};
    }

    // Allow multiKey based on the invariant that shard keys must be single-valued. Therefore, any
    // multi-key index prefixed by shard key cannot be multikey over the shard key fields.
    IndexDescriptor* idx =
        collection->getIndexCatalog()->findShardKeyPrefixedIndex(txn,
                                                                 _shardKeyPattern.toBSON(),
                                                                 false);  // requireSingleKey
    if (!idx) {
        return {ErrorCodes::IndexNotFound,
                str::stream() << "can't find index with prefix " << _shardKeyPattern.toBSON()
                              << " in storeCurrentLocs for "
                              << _args.getNss().ns()};
    }

    // Install the stage, which will listen for notifications on the collection
    {
        stdx::lock_guard<stdx::mutex> sl(_mutex);

        invariant(!_deleteNotifyExec);

        // Takes ownership of 'ws' and 'dns'.
        auto statusWithPlanExecutor =
            PlanExecutor::make(txn,
                               stdx::make_unique<WorkingSet>(),
                               stdx::make_unique<DeleteNotificationStage>(this, txn),
                               collection,
                               PlanExecutor::YIELD_MANUAL);
        invariant(statusWithPlanExecutor.isOK());

        _deleteNotifyExec = std::move(statusWithPlanExecutor.getValue());
        _deleteNotifyExec->registerExec(collection);
    }

    // Assume both min and max non-empty, append MinKey's to make them fit chosen index
    const KeyPattern kp(idx->keyPattern());

    BSONObj min = Helpers::toKeyFormat(kp.extendRangeBound(_args.getMinKey(), false));
    BSONObj max = Helpers::toKeyFormat(kp.extendRangeBound(_args.getMaxKey(), false));

    std::unique_ptr<PlanExecutor> exec(InternalPlanner::indexScan(txn,
                                                                  collection,
                                                                  idx,
                                                                  min,
                                                                  max,
                                                                  false,  // endKeyInclusive
                                                                  PlanExecutor::YIELD_MANUAL));

    // We can afford to yield here because any change to the base data that we might miss is already
    // being queued and will migrate in the 'transferMods' stage.
    exec->setYieldPolicy(PlanExecutor::YIELD_AUTO, collection);

    // Use the average object size to estimate how many objects a full chunk would carry do that
    // while traversing the chunk's range using the sharding index, below there's a fair amount of
    // slack before we determine a chunk is too large because object sizes will vary.
    unsigned long long maxRecsWhenFull;
    long long avgRecSize;

    const long long totalRecs = collection->numRecords(txn);
    if (totalRecs > 0) {
        avgRecSize = collection->dataSize(txn) / totalRecs;
        maxRecsWhenFull = _args.getMaxChunkSizeBytes() / avgRecSize;
        maxRecsWhenFull = std::min((unsigned long long)(Chunk::MaxObjectPerChunk + 1),
                                   130 * maxRecsWhenFull / 100 /* slack */);
    } else {
        avgRecSize = 0;
        maxRecsWhenFull = Chunk::MaxObjectPerChunk + 1;
    }

    // Do a full traversal of the chunk and don't stop even if we think it is a large chunk we want
    // the number of records to better report, in that case.
    bool isLargeChunk = false;
    unsigned long long recCount = 0;

    BSONObj obj;
    RecordId recordId;
    PlanExecutor::ExecState state;
    while (PlanExecutor::ADVANCED == (state = exec->getNext(&obj, &recordId))) {
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
        return {ErrorCodes::InternalError,
                str::stream() << "Executor error while scanning for documents belonging to chunk: "
                              << WorkingSetCommon::toStatusString(obj)};
    }

    exec.reset();

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

    _averageObjectSizeForCloneLocs = static_cast<uint64_t>(collection->averageObjectSize(txn) + 12);

    return Status::OK();
}

void MigrationChunkClonerSourceLegacy::_xfer(OperationContext* txn,
                                             Database* db,
                                             std::list<BSONObj>* docIdList,
                                             BSONObjBuilder* builder,
                                             const char* fieldName,
                                             long long* sizeAccumulator,
                                             bool explode) {
    const long long maxSize = 1024 * 1024;

    if (docIdList->size() == 0 || *sizeAccumulator > maxSize) {
        return;
    }

    const std::string& ns = _args.getNss().ns();

    BSONArrayBuilder arr(builder->subarrayStart(fieldName));

    std::list<BSONObj>::iterator docIdIter = docIdList->begin();
    while (docIdIter != docIdList->end() && *sizeAccumulator < maxSize) {
        BSONObj idDoc = *docIdIter;
        if (explode) {
            BSONObj fullDoc;
            if (Helpers::findById(txn, db, ns.c_str(), idDoc, fullDoc)) {
                arr.append(fullDoc);
                *sizeAccumulator += fullDoc.objsize();
            }
        } else {
            arr.append(idDoc);
            *sizeAccumulator += idDoc.objsize();
        }

        docIdIter = docIdList->erase(docIdIter);
    }

    arr.done();
}

}  // namespace mongo
