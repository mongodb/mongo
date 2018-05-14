/**
 *    Copyright (C) 2017 MongoDB Inc.
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
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kIndex

#include "mongo/platform/basic.h"

#include "mongo/db/catalog/index_create_impl.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/init.h"
#include "mongo/client/dbclientinterface.h"
#include "mongo/db/audit.h"
#include "mongo/db/background.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/db/client.h"
#include "mongo/db/clientcursor.h"
#include "mongo/db/concurrency/write_conflict_exception.h"
#include "mongo/db/curop.h"
#include "mongo/db/exec/working_set_common.h"
#include "mongo/db/index/multikey_paths.h"
#include "mongo/db/multi_key_path_tracker.h"
#include "mongo/db/op_observer.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/query/internal_plans.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/server_parameters.h"
#include "mongo/stdx/memory.h"
#include "mongo/stdx/mutex.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/fail_point_service.h"
#include "mongo/util/log.h"
#include "mongo/util/processinfo.h"
#include "mongo/util/progress_meter.h"
#include "mongo/util/quick_exit.h"
#include "mongo/util/scopeguard.h"

namespace mongo {

using std::unique_ptr;
using std::string;
using std::endl;

MONGO_FP_DECLARE(crashAfterStartingIndexBuild);
MONGO_FP_DECLARE(hangAfterStartingIndexBuild);
MONGO_FP_DECLARE(hangAfterStartingIndexBuildUnlocked);
MONGO_FP_DECLARE(slowBackgroundIndexBuild);

AtomicInt32 maxIndexBuildMemoryUsageMegabytes(500);

class ExportedMaxIndexBuildMemoryUsageParameter
    : public ExportedServerParameter<std::int32_t, ServerParameterType::kStartupAndRuntime> {
public:
    ExportedMaxIndexBuildMemoryUsageParameter()
        : ExportedServerParameter<std::int32_t, ServerParameterType::kStartupAndRuntime>(
              ServerParameterSet::getGlobal(),
              "maxIndexBuildMemoryUsageMegabytes",
              &maxIndexBuildMemoryUsageMegabytes) {}

    virtual Status validate(const std::int32_t& potentialNewValue) {
        if (potentialNewValue < 100) {
            return Status(
                ErrorCodes::BadValue,
                "maxIndexBuildMemoryUsageMegabytes must be greater than or equal to 100 MB");
        }

        return Status::OK();
    }

} exportedMaxIndexBuildMemoryUsageParameter;


/**
 * On rollback sets MultiIndexBlockImpl::_needToCleanup to true.
 */
class MultiIndexBlockImpl::SetNeedToCleanupOnRollback : public RecoveryUnit::Change {
public:
    explicit SetNeedToCleanupOnRollback(MultiIndexBlockImpl* indexer) : _indexer(indexer) {}

    virtual void commit(boost::optional<Timestamp>) {}
    virtual void rollback() {
        _indexer->_needToCleanup = true;
    }

private:
    MultiIndexBlockImpl* const _indexer;
};

/**
 * On rollback in init(), cleans up _indexes so that ~MultiIndexBlock doesn't try to clean
 * up _indexes manually (since the changes were already rolled back).
 * Due to this, it is thus legal to call init() again after it fails.
 */
class MultiIndexBlockImpl::CleanupIndexesVectorOnRollback : public RecoveryUnit::Change {
public:
    explicit CleanupIndexesVectorOnRollback(MultiIndexBlockImpl* indexer) : _indexer(indexer) {}

    virtual void commit(boost::optional<Timestamp>) {}
    virtual void rollback() {
        _indexer->_indexes.clear();
    }

private:
    MultiIndexBlockImpl* const _indexer;
};

MultiIndexBlockImpl::MultiIndexBlockImpl(OperationContext* opCtx, Collection* collection)
    : _collection(collection),
      _opCtx(opCtx),
      _buildInBackground(false),
      _allowInterruption(false),
      _ignoreUnique(false),
      _needToCleanup(true) {}

MultiIndexBlockImpl::~MultiIndexBlockImpl() {
    if (!_needToCleanup && !_indexes.empty()) {
        _collection->infoCache()->clearQueryCache();
    }

    if (!_needToCleanup || _indexes.empty())
        return;

    // Make lock acquisition uninterruptible because onOpMessage() and WUOW.commit() could take
    // locks.
    UninterruptibleLockGuard(_opCtx->lockState());

    while (true) {
        try {
            WriteUnitOfWork wunit(_opCtx);
            // This cleans up all index builds. Because that may need to write, it is done inside of
            // a WUOW. Nothing inside this block can fail, and it is made fatal if it does.
            for (size_t i = 0; i < _indexes.size(); i++) {
                _indexes[i].block->fail();
            }

            auto replCoord = repl::ReplicationCoordinator::get(_opCtx);
            // Nodes building an index on behalf of a user (e.g: `createIndexes`, `applyOps`) may
            // fail, removing the existence of the index from the catalog. This update must be
            // timestamped. A failure from `createIndexes` should not have a commit timestamp and
            // instead write a noop entry. A foreground `applyOps` index build may have a commit
            // timestamp already set.
            if (_opCtx->recoveryUnit()->getCommitTimestamp().isNull() &&
                replCoord->canAcceptWritesForDatabase(_opCtx, "admin")) {

                // Make lock acquisition uninterruptible because writing an op message takes a lock.
                _opCtx->getServiceContext()->getOpObserver()->onOpMessage(
                    _opCtx,
                    BSON("msg" << std::string(str::stream() << "Failing index builds. Coll: "
                                                            << _collection->ns().ns())));
            }
            wunit.commit();
            return;
        } catch (const WriteConflictException&) {
            continue;
        } catch (const DBException& e) {
            if (e.toStatus() == ErrorCodes::ExceededMemoryLimit)
                continue;
            error() << "Caught exception while cleaning up partially built indexes: " << redact(e);
        } catch (const std::exception& e) {
            error() << "Caught exception while cleaning up partially built indexes: " << e.what();
        } catch (...) {
            error() << "Caught unknown exception while cleaning up partially built indexes.";
        }
        fassertFailed(18644);
    }
}

void MultiIndexBlockImpl::removeExistingIndexes(std::vector<BSONObj>* specs) const {
    for (size_t i = 0; i < specs->size(); i++) {
        Status status =
            _collection->getIndexCatalog()->prepareSpecForCreate(_opCtx, (*specs)[i]).getStatus();
        if (status.code() == ErrorCodes::IndexAlreadyExists) {
            specs->erase(specs->begin() + i);
            i--;
        }
        // intentionally ignoring other error codes
    }
}

StatusWith<std::vector<BSONObj>> MultiIndexBlockImpl::init(const BSONObj& spec) {
    const auto indexes = std::vector<BSONObj>(1, spec);
    return init(indexes);
}

StatusWith<std::vector<BSONObj>> MultiIndexBlockImpl::init(const std::vector<BSONObj>& indexSpecs) {
    WriteUnitOfWork wunit(_opCtx);

    invariant(_indexes.empty());
    _opCtx->recoveryUnit()->registerChange(new CleanupIndexesVectorOnRollback(this));

    const string& ns = _collection->ns().ns();

    const auto idxCat = _collection->getIndexCatalog();
    invariant(idxCat);
    invariant(idxCat->ok());
    Status status = idxCat->checkUnfinished();
    if (!status.isOK())
        return status;

    for (size_t i = 0; i < indexSpecs.size(); i++) {
        BSONObj info = indexSpecs[i];

        string pluginName = IndexNames::findPluginName(info["key"].Obj());
        if (pluginName.size()) {
            Status s = _collection->getIndexCatalog()->_upgradeDatabaseMinorVersionIfNeeded(
                _opCtx, pluginName);
            if (!s.isOK())
                return s;
        }

        // Any foreground indexes make all indexes be built in the foreground.
        _buildInBackground = (_buildInBackground && initBackgroundIndexFromSpec(info));
    }

    std::vector<BSONObj> indexInfoObjs;
    indexInfoObjs.reserve(indexSpecs.size());
    std::size_t eachIndexBuildMaxMemoryUsageBytes = 0;
    if (!indexSpecs.empty()) {
        eachIndexBuildMaxMemoryUsageBytes =
            static_cast<std::size_t>(maxIndexBuildMemoryUsageMegabytes.load()) * 1024 * 1024 /
            indexSpecs.size();
    }

    for (size_t i = 0; i < indexSpecs.size(); i++) {
        BSONObj info = indexSpecs[i];
        StatusWith<BSONObj> statusWithInfo =
            _collection->getIndexCatalog()->prepareSpecForCreate(_opCtx, info);
        Status status = statusWithInfo.getStatus();
        if (!status.isOK())
            return status;
        info = statusWithInfo.getValue();
        indexInfoObjs.push_back(info);

        IndexToBuild index;
        index.block.reset(new IndexCatalogImpl::IndexBuildBlock(_opCtx, _collection, info));
        status = index.block->init();
        if (!status.isOK())
            return status;

        index.real = index.block->getEntry()->accessMethod();
        status = index.real->initializeAsEmpty(_opCtx);
        if (!status.isOK())
            return status;

        if (!_buildInBackground) {
            // Bulk build process requires foreground building as it assumes nothing is changing
            // under it.
            index.bulk = index.real->initiateBulk(eachIndexBuildMaxMemoryUsageBytes);
        }

        const IndexDescriptor* descriptor = index.block->getEntry()->descriptor();

        IndexCatalog::prepareInsertDeleteOptions(_opCtx, descriptor, &index.options);
        index.options.dupsAllowed = index.options.dupsAllowed || _ignoreUnique;
        if (_ignoreUnique) {
            index.options.getKeysMode = IndexAccessMethod::GetKeysMode::kRelaxConstraints;
        }

        log() << "build index on: " << ns << " properties: " << descriptor->toString();
        if (index.bulk)
            log() << "\t building index using bulk method; build may temporarily use up to "
                  << eachIndexBuildMaxMemoryUsageBytes / 1024 / 1024 << " megabytes of RAM";

        index.filterExpression = index.block->getEntry()->getFilterExpression();

        // TODO SERVER-14888 Suppress this in cases we don't want to audit.
        audit::logCreateIndex(_opCtx->getClient(), &info, descriptor->indexName(), ns);

        _indexes.push_back(std::move(index));
    }

    if (_buildInBackground)
        _backgroundOperation.reset(new BackgroundOperation(ns));

    auto replCoord = repl::ReplicationCoordinator::get(_opCtx);
    if (_opCtx->recoveryUnit()->getCommitTimestamp().isNull() &&
        replCoord->canAcceptWritesForDatabase(_opCtx, "admin")) {
        // Only primaries must timestamp this write. Secondaries run this from within a
        // `TimestampBlock`. Primaries performing an index build via `applyOps` may have a
        // wrapping commit timestamp that will be used instead.
        _opCtx->getServiceContext()->getOpObserver()->onOpMessage(
            _opCtx, BSON("msg" << std::string(str::stream() << "Creating indexes. Coll: " << ns)));
    }

    wunit.commit();

    if (MONGO_FAIL_POINT(crashAfterStartingIndexBuild)) {
        log() << "Index build interrupted due to 'crashAfterStartingIndexBuild' failpoint. Exiting "
                 "after waiting for changes to become durable.";
        Locker::LockSnapshot lockInfo;
        _opCtx->lockState()->saveLockStateAndUnlock(&lockInfo);
        if (_opCtx->recoveryUnit()->waitUntilDurable()) {
            quickExit(EXIT_TEST);
        }
    }

    return indexInfoObjs;
}

Status MultiIndexBlockImpl::insertAllDocumentsInCollection(std::set<RecordId>* dupsOut) {
    invariant(!_opCtx->lockState()->inAWriteUnitOfWork());

    // Refrain from persisting any multikey updates as a result from building the index. Instead,
    // accumulate them in the `MultikeyPathTracker` and do the write as part of the update that
    // commits the index.
    auto stopTracker =
        MakeGuard([this] { MultikeyPathTracker::get(_opCtx).stopTrackingMultikeyPathInfo(); });
    if (MultikeyPathTracker::get(_opCtx).isTrackingMultikeyPathInfo()) {
        stopTracker.Dismiss();
    }
    MultikeyPathTracker::get(_opCtx).startTrackingMultikeyPathInfo();

    const char* curopMessage = _buildInBackground ? "Index Build (background)" : "Index Build";
    const auto numRecords = _collection->numRecords(_opCtx);
    stdx::unique_lock<Client> lk(*_opCtx->getClient());
    ProgressMeterHolder progress(
        CurOp::get(_opCtx)->setMessage_inlock(curopMessage, curopMessage, numRecords));
    lk.unlock();

    Timer t;

    unsigned long long n = 0;

    PlanExecutor::YieldPolicy yieldPolicy;
    if (_buildInBackground) {
        invariant(_allowInterruption);
        yieldPolicy = PlanExecutor::YIELD_AUTO;
    } else {
        yieldPolicy = PlanExecutor::WRITE_CONFLICT_RETRY_ONLY;
    }
    auto exec =
        InternalPlanner::collectionScan(_opCtx, _collection->ns().ns(), _collection, yieldPolicy);

    Snapshotted<BSONObj> objToIndex;
    RecordId loc;
    PlanExecutor::ExecState state;
    int retries = 0;  // non-zero when retrying our last document.
    while (retries ||
           (PlanExecutor::ADVANCED == (state = exec->getNextSnapshotted(&objToIndex, &loc))) ||
           MONGO_FAIL_POINT(hangAfterStartingIndexBuild)) {
        try {
            if (_allowInterruption)
                _opCtx->checkForInterrupt();

            if (!(retries || PlanExecutor::ADVANCED == state) ||
                MONGO_FAIL_POINT(slowBackgroundIndexBuild)) {
                log() << "Hanging index build due to failpoint";
                invariant(_allowInterruption);
                sleepmillis(1000);
                continue;
            }

            // Make sure we are working with the latest version of the document.
            if (objToIndex.snapshotId() != _opCtx->recoveryUnit()->getSnapshotId() &&
                !_collection->findDoc(_opCtx, loc, &objToIndex)) {
                // doc was deleted so don't index it.
                retries = 0;
                continue;
            }

            // Done before insert so we can retry document if it WCEs.
            progress->setTotalWhileRunning(_collection->numRecords(_opCtx));

            WriteUnitOfWork wunit(_opCtx);
            Status ret = insert(objToIndex.value(), loc);
            if (_buildInBackground)
                exec->saveState();
            if (ret.isOK()) {
                wunit.commit();
            } else if (dupsOut && ret.code() == ErrorCodes::DuplicateKey) {
                // If dupsOut is non-null, we should only fail the specific insert that
                // led to a DuplicateKey rather than the whole index build.
                dupsOut->insert(loc);
            } else {
                // Fail the index build hard.
                return ret;
            }
            if (_buildInBackground) {
                auto restoreStatus = exec->restoreState();  // Handles any WCEs internally.
                if (!restoreStatus.isOK()) {
                    return restoreStatus;
                }
            }

            // Go to the next document
            progress->hit();
            n++;
            retries = 0;
        } catch (const WriteConflictException&) {
            CurOp::get(_opCtx)->debug().writeConflicts++;
            retries++;  // logAndBackoff expects this to be 1 on first call.
            WriteConflictException::logAndBackoff(
                retries, "index creation", _collection->ns().ns());

            // Can't use writeConflictRetry since we need to save/restore exec around call to
            // abandonSnapshot.
            exec->saveState();
            _opCtx->recoveryUnit()->abandonSnapshot();
            auto restoreStatus = exec->restoreState();  // Handles any WCEs internally.
            if (!restoreStatus.isOK()) {
                return restoreStatus;
            }
        }
    }

    if (state != PlanExecutor::IS_EOF) {
        return WorkingSetCommon::getMemberObjectStatus(objToIndex.value());
    }

    if (MONGO_FAIL_POINT(hangAfterStartingIndexBuildUnlocked)) {
        // Unlock before hanging so replication recognizes we've completed.
        Locker::LockSnapshot lockInfo;
        _opCtx->lockState()->saveLockStateAndUnlock(&lockInfo);
        while (MONGO_FAIL_POINT(hangAfterStartingIndexBuildUnlocked)) {
            log() << "Hanging index build with no locks due to "
                     "'hangAfterStartingIndexBuildUnlocked' failpoint";
            sleepmillis(1000);
        }

        if (_buildInBackground) {
            _opCtx->lockState()->restoreLockState(_opCtx, lockInfo);
            _opCtx->recoveryUnit()->abandonSnapshot();
            return Status(ErrorCodes::OperationFailed,
                          "background index build aborted due to failpoint");
        } else {
            invariant(
                !"the hangAfterStartingIndexBuildUnlocked failpoint can't be turned off for foreground index builds");
        }
    }

    progress->finished();

    Status ret = doneInserting(dupsOut);
    if (!ret.isOK())
        return ret;

    log() << "build index done.  scanned " << n << " total records. " << t.seconds() << " secs";

    return Status::OK();
}

Status MultiIndexBlockImpl::insert(const BSONObj& doc, const RecordId& loc) {
    for (size_t i = 0; i < _indexes.size(); i++) {
        if (_indexes[i].filterExpression && !_indexes[i].filterExpression->matchesBSON(doc)) {
            continue;
        }

        int64_t unused;
        Status idxStatus(ErrorCodes::InternalError, "");
        if (_indexes[i].bulk) {
            idxStatus = _indexes[i].bulk->insert(_opCtx, doc, loc, _indexes[i].options, &unused);
        } else {
            idxStatus = _indexes[i].real->insert(_opCtx, doc, loc, _indexes[i].options, &unused);
        }

        if (!idxStatus.isOK())
            return idxStatus;
    }
    return Status::OK();
}

Status MultiIndexBlockImpl::doneInserting(std::set<RecordId>* dupsOut) {
    invariant(!_opCtx->lockState()->inAWriteUnitOfWork());
    for (size_t i = 0; i < _indexes.size(); i++) {
        if (_indexes[i].bulk == NULL)
            continue;
        LOG(1) << "\t bulk commit starting for index: "
               << _indexes[i].block->getEntry()->descriptor()->indexName();
        Status status = _indexes[i].real->commitBulk(_opCtx,
                                                     _indexes[i].bulk.get(),
                                                     _allowInterruption,
                                                     _indexes[i].options.dupsAllowed,
                                                     dupsOut);
        if (!status.isOK()) {
            return status;
        }
    }

    return Status::OK();
}

void MultiIndexBlockImpl::abortWithoutCleanup() {
    _indexes.clear();
    _needToCleanup = false;
}

void MultiIndexBlockImpl::commit() {
    // Do not interfere with writing multikey information when committing index builds.
    auto restartTracker =
        MakeGuard([this] { MultikeyPathTracker::get(_opCtx).startTrackingMultikeyPathInfo(); });
    if (!MultikeyPathTracker::get(_opCtx).isTrackingMultikeyPathInfo()) {
        restartTracker.Dismiss();
    }
    MultikeyPathTracker::get(_opCtx).stopTrackingMultikeyPathInfo();

    for (size_t i = 0; i < _indexes.size(); i++) {
        _indexes[i].block->success();

        // The bulk builder will track multikey information itself. Non-bulk builders re-use the
        // code path that a typical insert/update uses. State is altered on the non-bulk build
        // path to accumulate the multikey information on the `MultikeyPathTracker`.
        if (_indexes[i].bulk) {
            const auto& bulkBuilder = _indexes[i].bulk;
            if (bulkBuilder->isMultikey()) {
                _indexes[i].block->getEntry()->setMultikey(_opCtx, bulkBuilder->getMultikeyPaths());
            }
        } else {
            auto multikeyPaths =
                boost::optional<MultikeyPaths>(MultikeyPathTracker::get(_opCtx).getMultikeyPathInfo(
                    _collection->ns(), _indexes[i].block->getIndexName()));
            if (multikeyPaths) {
                _indexes[i].block->getEntry()->setMultikey(_opCtx, *multikeyPaths);
            }
        }
    }

    _opCtx->recoveryUnit()->registerChange(new SetNeedToCleanupOnRollback(this));
    _needToCleanup = false;
}
}  // namespace mongo
