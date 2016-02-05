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

#include "mongo/db/s/migration_source_manager.h"

#include <set>
#include <vector>

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/concurrency/d_concurrency.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/dbhelpers.h"
#include "mongo/db/exec/plan_stage.h"
#include "mongo/db/index/index_descriptor.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/query/internal_plans.h"
#include "mongo/db/record_id.h"
#include "mongo/logger/ramlog.h"
#include "mongo/s/chunk.h"
#include "mongo/s/d_state.h"
#include "mongo/s/shard_key_pattern.h"
#include "mongo/util/elapsed_tracker.h"
#include "mongo/util/log.h"

namespace mongo {

using std::list;
using std::string;
using std::unique_ptr;

namespace {

Tee* migrateLog = RamLog::get("migrate");

/**
 * Used to receive invalidation notifications.
 *
 * XXX: move to the exec/ directory.
 */
class DeleteNotificationStage final : public PlanStage {
public:
    DeleteNotificationStage(MigrationSourceManager* migrationSourceManager)
        : PlanStage("NOTIFY_DELETE", nullptr), _migrationSourceManager(migrationSourceManager) {}

    void doInvalidate(OperationContext* txn, const RecordId& dl, InvalidationType type) override {
        if (type == INVALIDATION_DELETION) {
            _migrationSourceManager->aboutToDelete(dl);
        }
    }

    StageState doWork(WorkingSetID* out) override {
        MONGO_UNREACHABLE;
    }

    bool isEOF() final {
        MONGO_UNREACHABLE;
    }

    unique_ptr<PlanStageStats> getStats() final {
        MONGO_UNREACHABLE;
    }

    SpecificStats* getSpecificStats() const final {
        MONGO_UNREACHABLE;
    }

    StageType stageType() const final {
        return STAGE_NOTIFY_DELETE;
    }

private:
    MigrationSourceManager* const _migrationSourceManager;
};

bool isInRange(const BSONObj& obj,
               const BSONObj& min,
               const BSONObj& max,
               const BSONObj& shardKeyPattern) {
    ShardKeyPattern shardKey(shardKeyPattern);
    BSONObj k = shardKey.extractShardKeyFromDoc(obj);
    return k.woCompare(min) >= 0 && k.woCompare(max) < 0;
}

}  // namespace

/**
 * Used to commit work for LogOpForSharding. Used to keep track of changes in documents that are
 * part of a chunk being migrated.
 */
class LogOpForShardingHandler final : public RecoveryUnit::Change {
public:
    /**
     * Invariant: idObj should belong to a document that is part of the active chunk being migrated
     */
    LogOpForShardingHandler(MigrationSourceManager* migrateSourceManager,
                            const BSONObj& idObj,
                            const char op)
        : _migrationSourceManager(migrateSourceManager), _idObj(idObj.getOwned()), _op(op) {}

    void commit() override {
        switch (_op) {
            case 'd': {
                stdx::lock_guard<stdx::mutex> sl(_migrationSourceManager->_mutex);
                _migrationSourceManager->_deleted.push_back(_idObj);
                _migrationSourceManager->_memoryUsed += _idObj.firstElement().size() + 5;
                break;
            }

            case 'i':
            case 'u': {
                stdx::lock_guard<stdx::mutex> sl(_migrationSourceManager->_mutex);
                _migrationSourceManager->_reload.push_back(_idObj);
                _migrationSourceManager->_memoryUsed += _idObj.firstElement().size() + 5;
                break;
            }

            default:
                MONGO_UNREACHABLE;
        }
    }

    void rollback() override {}

private:
    MigrationSourceManager* const _migrationSourceManager;
    const BSONObj _idObj;
    const char _op;
};


MigrationSourceManager::MigrationSourceManager() = default;

MigrationSourceManager::~MigrationSourceManager() = default;

bool MigrationSourceManager::start(OperationContext* txn,
                                   const MigrationSessionId& sessionId,
                                   const std::string& ns,
                                   const BSONObj& min,
                                   const BSONObj& max,
                                   const BSONObj& shardKeyPattern) {
    invariant(!min.isEmpty());
    invariant(!max.isEmpty());
    invariant(!ns.empty());

    // Get global shared to synchronize with logOp. Also see comments in the class
    // members declaration for more details.
    Lock::GlobalRead globalShared(txn->lockState());

    stdx::lock_guard<stdx::mutex> lk(_mutex);

    if (_sessionId) {
        return false;
    }

    _nss = NamespaceString(ns);
    _min = min;
    _max = max;
    _shardKeyPattern = shardKeyPattern;

    invariant(_deleted.size() == 0);
    invariant(_reload.size() == 0);
    invariant(_memoryUsed == 0);

    _sessionId = sessionId;

    stdx::lock_guard<stdx::mutex> tLock(_cloneLocsMutex);
    invariant(_cloneLocs.size() == 0);

    return true;
}

void MigrationSourceManager::done(OperationContext* txn) {
    log() << "MigrateFromStatus::done About to acquire global lock to exit critical section";

    // Get global shared to synchronize with logOp. Also see comments in the class
    // members declaration for more details.
    Lock::GlobalRead globalShared(txn->lockState());

    stdx::lock_guard<stdx::mutex> lk(_mutex);

    _sessionId = boost::none;
    _deleteNotifyExec.reset(NULL);
    _inCriticalSection = false;
    _inCriticalSectionCV.notify_all();

    _deleted.clear();
    _reload.clear();
    _memoryUsed = 0;

    stdx::lock_guard<stdx::mutex> cloneLock(_cloneLocsMutex);
    _cloneLocs.clear();
}

void MigrationSourceManager::logInsertOp(OperationContext* txn,
                                         const char* ns,
                                         const BSONObj& obj,
                                         bool notInActiveChunk) {
    ensureShardVersionOKOrThrow(txn, ns);

    if (notInActiveChunk)
        return;

    dassert(txn->lockState()->isWriteLocked());  // Must have Global IX.

    if (!_sessionId || (_nss != ns))
        return;

    BSONElement idElement = obj["_id"];
    if (idElement.eoo()) {
        warning() << "logInsertOp got a document with no _id field, ignoring inserted document: "
                  << obj << migrateLog;
        return;
    }
    BSONObj idObj(idElement.wrap());

    if (!isInRange(obj, _min, _max, _shardKeyPattern)) {
        return;
    }

    txn->recoveryUnit()->registerChange(new LogOpForShardingHandler(this, idObj, 'i'));
}

void MigrationSourceManager::logUpdateOp(OperationContext* txn,
                                         const char* ns,
                                         const BSONObj& updatedDoc,
                                         bool notInActiveChunk) {
    ensureShardVersionOKOrThrow(txn, ns);

    if (notInActiveChunk)
        return;

    dassert(txn->lockState()->isWriteLocked());  // Must have Global IX.

    if (!_sessionId || (_nss != ns))
        return;

    BSONElement idElement = updatedDoc["_id"];
    if (idElement.eoo()) {
        warning() << "logUpdateOp got a document with no _id field, ignoring updatedDoc: "
                  << updatedDoc << migrateLog;
        return;
    }
    BSONObj idObj(idElement.wrap());

    if (!isInRange(updatedDoc, _min, _max, _shardKeyPattern)) {
        return;
    }

    txn->recoveryUnit()->registerChange(new LogOpForShardingHandler(this, idObj, 'u'));
}

void MigrationSourceManager::logDeleteOp(OperationContext* txn,
                                         const char* ns,
                                         const BSONObj& obj,
                                         bool notInActiveChunk) {
    ensureShardVersionOKOrThrow(txn, ns);

    if (notInActiveChunk)
        return;

    dassert(txn->lockState()->isWriteLocked());  // Must have Global IX.

    BSONElement idElement = obj["_id"];
    if (idElement.eoo()) {
        warning() << "logDeleteOp got a document with no _id field, ignoring deleted doc: " << obj
                  << migrateLog;
        return;
    }
    BSONObj idObj(idElement.wrap());

    txn->recoveryUnit()->registerChange(new LogOpForShardingHandler(this, idObj, 'd'));
}

bool MigrationSourceManager::isInMigratingChunk(const NamespaceString& ns, const BSONObj& doc) {
    if (!_sessionId)
        return false;

    if (ns != _nss)
        return false;

    return isInRange(doc, _min, _max, _shardKeyPattern);
}

bool MigrationSourceManager::transferMods(OperationContext* txn,
                                          const MigrationSessionId& sessionId,
                                          string& errmsg,
                                          BSONObjBuilder& b) {
    long long size = 0;

    {
        AutoGetCollectionForRead ctx(txn, _getNS());

        stdx::lock_guard<stdx::mutex> sl(_mutex);

        if (!_sessionId) {
            errmsg = "no active migration!";
            return false;
        }

        // TODO after 3.4 release, !sessionId.isEmpty() can be removed: versions >= 3.2 will
        // all have sessionId implemented. (two more instances below).
        // A mongod version < v3.2 will not have sessionId, in which case it is empty and ignored.
        if (!sessionId.isEmpty() && !_sessionId->matches(sessionId)) {
            errmsg = str::stream() << "requested migration session id " << sessionId.toString()
                                   << " does not match active session id "
                                   << _sessionId->toString();
            return false;
        }

        // TODO: fix SERVER-16540 race
        _xfer(txn, _nss.ns(), ctx.getDb(), &_deleted, b, "deleted", size, false);
        _xfer(txn, _nss.ns(), ctx.getDb(), &_reload, b, "reload", size, true);
    }

    b.append("size", size);

    return true;
}

bool MigrationSourceManager::storeCurrentLocs(OperationContext* txn,
                                              long long maxChunkSize,
                                              string& errmsg,
                                              BSONObjBuilder& result) {
    ScopedTransaction scopedXact(txn, MODE_IS);
    AutoGetCollection autoColl(txn, _getNS(), MODE_IS);

    Collection* collection = autoColl.getCollection();
    if (!collection) {
        errmsg = "ns not found, should be impossible";
        return false;
    }

    // Allow multiKey based on the invariant that shard keys must be single-valued. Therefore, any
    // multi-key index prefixed by shard key cannot be multikey over the shard key fields.
    IndexDescriptor* idx =
        collection->getIndexCatalog()->findShardKeyPrefixedIndex(txn,
                                                                 _shardKeyPattern,
                                                                 false);  // requireSingleKey

    if (idx == NULL) {
        errmsg = str::stream() << "can't find index with prefix " << _shardKeyPattern
                               << " in storeCurrentLocs for " << _nss.toString();
        return false;
    }

    // Assume both min and max non-empty, append MinKey's to make them fit chosen index
    BSONObj min;
    BSONObj max;
    KeyPattern kp(idx->keyPattern());

    {
        // It's alright not to lock _mutex all the way through based on the assumption that this is
        // only called by the main thread that drives the migration and only it can start and stop
        // the current migration.
        stdx::lock_guard<stdx::mutex> sl(_mutex);

        invariant(_deleteNotifyExec.get() == NULL);
        unique_ptr<WorkingSet> ws = stdx::make_unique<WorkingSet>();
        unique_ptr<DeleteNotificationStage> dns = stdx::make_unique<DeleteNotificationStage>(this);

        // Takes ownership of 'ws' and 'dns'.
        auto statusWithPlanExecutor = PlanExecutor::make(
            txn, std::move(ws), std::move(dns), collection, PlanExecutor::YIELD_MANUAL);
        invariant(statusWithPlanExecutor.isOK());

        _deleteNotifyExec = std::move(statusWithPlanExecutor.getValue());
        _deleteNotifyExec->registerExec();

        min = Helpers::toKeyFormat(kp.extendRangeBound(_min, false));
        max = Helpers::toKeyFormat(kp.extendRangeBound(_max, false));
    }

    unique_ptr<PlanExecutor> exec(InternalPlanner::indexScan(txn,
                                                             collection,
                                                             idx,
                                                             min,
                                                             max,
                                                             false,  // endKeyInclusive
                                                             PlanExecutor::YIELD_MANUAL));

    // We can afford to yield here because any change to the base data that we might miss is already
    // being queued and will migrate in the 'transferMods' stage.
    exec->setYieldPolicy(PlanExecutor::YIELD_AUTO);

    // Use the average object size to estimate how many objects a full chunk would carry do that
    // while traversing the chunk's range using the sharding index, below there's a fair amount of
    // slack before we determine a chunk is too large because object sizes will vary.
    unsigned long long maxRecsWhenFull;
    long long avgRecSize;

    const long long totalRecs = collection->numRecords(txn);
    if (totalRecs > 0) {
        avgRecSize = collection->dataSize(txn) / totalRecs;
        maxRecsWhenFull = maxChunkSize / avgRecSize;
        maxRecsWhenFull = std::min((unsigned long long)(Chunk::MaxObjectPerChunk + 1),
                                   130 * maxRecsWhenFull / 100 /* slack */);
    } else {
        avgRecSize = 0;
        maxRecsWhenFull = Chunk::MaxObjectPerChunk + 1;
    }

    // Do a full traversal of the chunk and don't stop even if we think it is a large chunk we want
    // the number of records to better report, in that case
    bool isLargeChunk = false;
    unsigned long long recCount = 0;

    RecordId recordId;
    while (PlanExecutor::ADVANCED == exec->getNext(NULL, &recordId)) {
        if (!isLargeChunk) {
            stdx::lock_guard<stdx::mutex> lk(_cloneLocsMutex);
            _cloneLocs.insert(recordId);
        }

        if (++recCount > maxRecsWhenFull) {
            isLargeChunk = true;
            // Continue on despite knowing that it will fail, just to get the correct value for
            // recCount
        }
    }

    exec.reset();

    if (isLargeChunk) {
        stdx::lock_guard<stdx::mutex> sl(_mutex);
        warning() << "cannot move chunk: the maximum number of documents for a chunk is "
                  << maxRecsWhenFull << " , the maximum chunk size is " << maxChunkSize
                  << " , average document size is " << avgRecSize << ". Found " << recCount
                  << " documents in chunk "
                  << " ns: " << _nss << " " << _min << " -> " << _max << migrateLog;

        result.appendBool("chunkTooBig", true);
        result.appendNumber("estimatedChunkSize", (long long)(recCount * avgRecSize));
        errmsg = "chunk too big to move";
        return false;
    }

    log() << "moveChunk number of documents: " << cloneLocsRemaining() << migrateLog;
    return true;
}

bool MigrationSourceManager::clone(OperationContext* txn,
                                   const MigrationSessionId& sessionId,
                                   string& errmsg,
                                   BSONObjBuilder& result) {
    ElapsedTracker tracker(internalQueryExecYieldIterations, internalQueryExecYieldPeriodMS);

    int allocSize = 0;

    {
        AutoGetCollection autoColl(txn, _getNS(), MODE_IS);

        stdx::lock_guard<stdx::mutex> sl(_mutex);

        if (!_sessionId) {
            errmsg = "not active";
            return false;
        }

        // A mongod version < v3.2 will not have sessionId, in which case it is empty and ignored.
        if (!sessionId.isEmpty() && !_sessionId->matches(sessionId)) {
            errmsg = str::stream() << "requested migration session id " << sessionId.toString()
                                   << " does not match active session id "
                                   << _sessionId->toString();
            return false;
        }

        Collection* collection = autoColl.getCollection();
        if (!collection) {
            errmsg = str::stream() << "collection " << _nss.toString() << " does not exist";
            return false;
        }

        allocSize = std::min(
            BSONObjMaxUserSize,
            static_cast<int>((12 + collection->averageObjectSize(txn)) * cloneLocsRemaining()));
    }

    bool isBufferFilled = false;
    BSONArrayBuilder clonedDocsArrayBuilder(allocSize);
    while (!isBufferFilled) {
        AutoGetCollection autoColl(txn, _getNS(), MODE_IS);

        stdx::lock_guard<stdx::mutex> sl(_mutex);

        if (!_sessionId) {
            errmsg = "not active";
            return false;
        }

        // A mongod version < v3.2 will not have sessionId, in which case it is empty and ignored.
        if (!sessionId.isEmpty() && !_sessionId->matches(sessionId)) {
            errmsg = str::stream() << "migration session id changed from " << sessionId.toString()
                                   << " to " << _sessionId->toString()
                                   << " while initial clone was active";
            return false;
        }

        // TODO: fix SERVER-16540 race
        Collection* collection = autoColl.getCollection();
        if (!collection) {
            errmsg = str::stream() << "collection " << _nss.toString() << " does not exist";
            return false;
        }

        stdx::lock_guard<stdx::mutex> lk(_cloneLocsMutex);

        std::set<RecordId>::iterator cloneLocsIter = _cloneLocs.begin();
        for (; cloneLocsIter != _cloneLocs.end(); ++cloneLocsIter) {
            if (tracker.intervalHasElapsed())  // should I yield?
                break;

            RecordId recordId = *cloneLocsIter;
            Snapshotted<BSONObj> doc;
            if (!collection->findDoc(txn, recordId, &doc)) {
                // doc was deleted
                continue;
            }

            // Use the builder size instead of accumulating 'doc's size so that we take
            // into consideration the overhead of BSONArray indices, and *always*
            // append one doc.
            if (clonedDocsArrayBuilder.arrSize() != 0 &&
                (clonedDocsArrayBuilder.len() + doc.value().objsize() + 1024) >
                    BSONObjMaxUserSize) {
                isBufferFilled = true;  // break out of outer while loop
                break;
            }

            clonedDocsArrayBuilder.append(doc.value());
        }

        _cloneLocs.erase(_cloneLocs.begin(), cloneLocsIter);

        // Note: must be holding _cloneLocsMutex, don't move this inside while condition!
        if (_cloneLocs.empty()) {
            break;
        }
    }

    result.appendArray("objects", clonedDocsArrayBuilder.arr());
    return true;
}

void MigrationSourceManager::aboutToDelete(const RecordId& dl) {
    // Even though above we call findDoc to check for existance that check only works for non-mmapv1
    // engines, and this is needed for mmapv1.
    stdx::lock_guard<stdx::mutex> lk(_cloneLocsMutex);
    _cloneLocs.erase(dl);
}

std::size_t MigrationSourceManager::cloneLocsRemaining() const {
    stdx::lock_guard<stdx::mutex> lk(_cloneLocsMutex);
    return _cloneLocs.size();
}

long long MigrationSourceManager::mbUsed() const {
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    return _memoryUsed / (1024 * 1024);
}

bool MigrationSourceManager::getInCriticalSection() const {
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    return _inCriticalSection;
}

void MigrationSourceManager::setInCriticalSection(bool inCritSec) {
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    _inCriticalSection = inCritSec;
    _inCriticalSectionCV.notify_all();
}

bool MigrationSourceManager::waitTillNotInCriticalSection(int maxSecondsToWait) {
    const auto deadline = stdx::chrono::system_clock::now() + Seconds(maxSecondsToWait);
    stdx::unique_lock<stdx::mutex> lk(_mutex);
    while (_inCriticalSection) {
        log() << "Waiting for " << maxSecondsToWait
              << " seconds for the migration critical section to end";

        if (stdx::cv_status::timeout == _inCriticalSectionCV.wait_until(lk, deadline)) {
            return false;
        }
    }

    return true;
}

bool MigrationSourceManager::isActive() const {
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    return _sessionId.is_initialized();
}

void MigrationSourceManager::_xfer(OperationContext* txn,
                                   const string& ns,
                                   Database* db,
                                   std::list<BSONObj>* docIdList,
                                   BSONObjBuilder& builder,
                                   const char* fieldName,
                                   long long& size,
                                   bool explode) {
    const long long maxSize = 1024 * 1024;

    if (docIdList->size() == 0 || size > maxSize) {
        return;
    }

    BSONArrayBuilder arr(builder.subarrayStart(fieldName));

    list<BSONObj>::iterator docIdIter = docIdList->begin();
    while (docIdIter != docIdList->end() && size < maxSize) {
        BSONObj idDoc = *docIdIter;
        if (explode) {
            BSONObj fullDoc;
            if (Helpers::findById(txn, db, ns.c_str(), idDoc, fullDoc)) {
                arr.append(fullDoc);
                size += fullDoc.objsize();
            }
        } else {
            arr.append(idDoc);
            size += idDoc.objsize();
        }

        docIdIter = docIdList->erase(docIdIter);
    }

    arr.done();
}

NamespaceString MigrationSourceManager::_getNS() const {
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    return _nss;
}

}  // namespace mongo
