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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kIndex

#include "mongo/platform/basic.h"

#include "mongo/db/catalog/multi_index_block.h"

#include <ostream>

#include "mongo/base/error_codes.h"
#include "mongo/db/audit.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/db/catalog/multi_index_block_gen.h"
#include "mongo/db/client.h"
#include "mongo/db/concurrency/write_conflict_exception.h"
#include "mongo/db/curop.h"
#include "mongo/db/index/index_build_interceptor.h"
#include "mongo/db/index/multikey_paths.h"
#include "mongo/db/multi_key_path_tracker.h"
#include "mongo/db/op_observer.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/repl_set_config.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/server_parameters.h"
#include "mongo/db/storage/storage_options.h"
#include "mongo/db/storage/write_unit_of_work.h"
#include "mongo/logger/redaction.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/fail_point_service.h"
#include "mongo/util/log.h"
#include "mongo/util/progress_meter.h"
#include "mongo/util/quick_exit.h"
#include "mongo/util/scopeguard.h"
#include "mongo/util/uuid.h"

namespace mongo {

namespace {

const StringData kBuildUUIDFieldName = "buildUUID"_sd;
const StringData kBuildingPhaseCompleteFieldName = "buildingPhaseComplete"_sd;
const StringData kRunTwoPhaseIndexBuildFieldName = "runTwoPhaseIndexBuild"_sd;
const StringData kCommitReadyMembersFieldName = "commitReadyMembers"_sd;

}  // namespace

MONGO_FAIL_POINT_DEFINE(crashAfterStartingIndexBuild);
MONGO_FAIL_POINT_DEFINE(hangAfterStartingIndexBuild);
MONGO_FAIL_POINT_DEFINE(hangAfterStartingIndexBuildUnlocked);
MONGO_FAIL_POINT_DEFINE(hangBeforeIndexBuildOf);
MONGO_FAIL_POINT_DEFINE(hangAfterIndexBuildOf);

MultiIndexBlock::~MultiIndexBlock() {
    invariant(_buildIsCleanedUp);
}

void MultiIndexBlock::cleanUpAfterBuild(OperationContext* opCtx, Collection* collection) {
    if (_collectionUUID) {
        // init() was previously called with a collection pointer, so ensure that the same
        // collection is being provided for clean up and the interface in not being abused.
        invariant(_collectionUUID.get() == collection->uuid().get());
    }

    if (!_needToCleanup && !_indexes.empty()) {
        collection->infoCache()->clearQueryCache();
    }

    if (!_needToCleanup || _indexes.empty()) {
        _buildIsCleanedUp = true;
        return;
    }

    // Make lock acquisition uninterruptible because onOpMessage() can take locks.
    UninterruptibleLockGuard noInterrupt(opCtx->lockState());

    while (true) {
        try {
            WriteUnitOfWork wunit(opCtx);
            // This cleans up all index builds. Because that may need to write, it is done inside of
            // a WUOW. Nothing inside this block can fail, and it is made fatal if it does.
            for (size_t i = 0; i < _indexes.size(); i++) {
                _indexes[i].block->fail(opCtx, collection);
            }

            auto replCoord = repl::ReplicationCoordinator::get(opCtx);
            // Nodes building an index on behalf of a user (e.g: `createIndexes`, `applyOps`) may
            // fail, removing the existence of the index from the catalog. This update must be
            // timestamped. A failure from `createIndexes` should not have a commit timestamp and
            // instead write a noop entry. A foreground `applyOps` index build may have a commit
            // timestamp already set.
            if (opCtx->recoveryUnit()->getCommitTimestamp().isNull() &&
                replCoord->canAcceptWritesForDatabase(opCtx, "admin")) {
                opCtx->getServiceContext()->getOpObserver()->onOpMessage(
                    opCtx,
                    BSON("msg" << std::string(str::stream() << "Failing index builds. Coll: "
                                                            << collection->ns())));
            }
            wunit.commit();
            _buildIsCleanedUp = true;
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

bool MultiIndexBlock::areHybridIndexBuildsEnabled() {
    // The mobile storage engine does not suport dupsAllowed mode on bulk builders, which means that
    // it does not support hybrid builds. See SERVER-38550
    if (storageGlobalParams.engine == "mobile") {
        return false;
    }

    // Hybrid index builds must only be used when in FCV 4.2. This restriction is due to the case
    // where an index build starts in FCV 4.0, then continues during an upgrade to FCV 4.2. Because
    // prepared transactions yield locks on secondaries, hybrid index builds may miss prepared, but
    // uncommitted writes, leading to data corruption. With two-phase index builds, an FCV 4.2-only
    // feature, the hybrid build will not complete until the primary writes an oplog entry
    // indicating the index build can finish, implying that there are no uncommitted prepared
    // transactions.
    if (!serverGlobalParams.featureCompatibility.isVersionInitialized() ||
        serverGlobalParams.featureCompatibility.getVersion() !=
            ServerGlobalParams::FeatureCompatibility::Version::kFullyUpgradedTo42) {
        return false;
    }

    return enableHybridIndexBuilds.load();
}

void MultiIndexBlock::ignoreUniqueConstraint() {
    _ignoreUnique = true;
}

MultiIndexBlock::OnInitFn MultiIndexBlock::kNoopOnInitFn = [] {};

MultiIndexBlock::OnInitFn MultiIndexBlock::makeTimestampedIndexOnInitFn(OperationContext* opCtx,
                                                                        const Collection* coll) {
    return [ opCtx, ns = coll->ns() ]() {
        auto replCoord = repl::ReplicationCoordinator::get(opCtx);
        if (opCtx->recoveryUnit()->getCommitTimestamp().isNull() &&
            replCoord->canAcceptWritesForDatabase(opCtx, "admin")) {
            // Only primaries must timestamp this write. Secondaries run this from within a
            // `TimestampBlock`. Primaries performing an index build via `applyOps` may have a
            // wrapping commit timestamp that will be used instead.
            opCtx->getServiceContext()->getOpObserver()->onOpMessage(
                opCtx,
                BSON("msg" << std::string(str::stream() << "Creating indexes. Coll: " << ns)));
        }
    };
}

StatusWith<std::vector<BSONObj>> MultiIndexBlock::init(OperationContext* opCtx,
                                                       Collection* collection,
                                                       const BSONObj& spec,
                                                       OnInitFn onInit) {
    const auto indexes = std::vector<BSONObj>(1, spec);
    return init(opCtx, collection, indexes, onInit);
}

StatusWith<std::vector<BSONObj>> MultiIndexBlock::init(OperationContext* opCtx,
                                                       Collection* collection,
                                                       const std::vector<BSONObj>& indexSpecs,
                                                       OnInitFn onInit) {
    if (State::kAborted == _getState()) {
        return {ErrorCodes::IndexBuildAborted,
                str::stream() << "Index build aborted: " << _abortReason
                              << ". Cannot initialize index builder: "
                              << collection->ns()
                              << (collection->uuid()
                                      ? (" (" + collection->uuid()->toString() + "): ")
                                      : ": ")
                              << indexSpecs.size()
                              << " provided. First index spec: "
                              << (indexSpecs.empty() ? BSONObj() : indexSpecs[0])};
    }

    // UUIDs are not guaranteed during startup because the check happens after indexes are rebuilt.
    if (collection->uuid()) {
        _collectionUUID = collection->uuid().get();
    }

    _buildIsCleanedUp = false;

    _updateCurOpOpDescription(opCtx, false);

    WriteUnitOfWork wunit(opCtx);

    invariant(_indexes.empty());

    // On rollback in init(), cleans up _indexes so that ~MultiIndexBlock doesn't try to clean up
    // _indexes manually (since the changes were already rolled back).
    // Due to this, it is thus legal to call init() again after it fails.
    opCtx->recoveryUnit()->onRollback([this]() { _indexes.clear(); });

    const auto& ns = collection->ns().ns();

    const auto idxCat = collection->getIndexCatalog();
    invariant(idxCat);
    invariant(idxCat->ok());
    Status status = idxCat->checkUnfinished();
    if (!status.isOK())
        return status;

    const bool enableHybrid = areHybridIndexBuildsEnabled();

    // Parse the specs if this builder is not building hybrid indexes, otherwise log a message.
    for (size_t i = 0; i < indexSpecs.size(); i++) {
        BSONObj info = indexSpecs[i];
        if (enableHybrid) {
            if (info["background"].isBoolean() && !info["background"].Bool()) {
                log() << "ignoring obselete { background: false } index build option because all "
                         "indexes are built in the background with the hybrid method";
            }
            continue;
        }

        // A single foreground build makes the entire builder foreground.
        if (info["background"].trueValue() && _method != IndexBuildMethod::kForeground) {
            _method = IndexBuildMethod::kBackground;
        } else {
            _method = IndexBuildMethod::kForeground;
        }
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
            collection->getIndexCatalog()->prepareSpecForCreate(opCtx, info);
        Status status = statusWithInfo.getStatus();
        if (!status.isOK())
            return status;
        info = statusWithInfo.getValue();
        indexInfoObjs.push_back(info);

        IndexToBuild index;
        index.block = collection->getIndexCatalog()->createIndexBuildBlock(opCtx, info, _method);
        status = index.block->init(opCtx, collection);
        if (!status.isOK())
            return status;

        index.real = index.block->getEntry()->accessMethod();
        status = index.real->initializeAsEmpty(opCtx);
        if (!status.isOK())
            return status;

        // Hybrid builds and non-hybrid foreground builds use the bulk builder.
        const bool useBulk =
            _method == IndexBuildMethod::kHybrid || _method == IndexBuildMethod::kForeground;
        if (useBulk) {
            // Bulk build process requires foreground building as it assumes nothing is changing
            // under it.
            index.bulk = index.real->initiateBulk(eachIndexBuildMaxMemoryUsageBytes);
        }

        const IndexDescriptor* descriptor = index.block->getEntry()->descriptor();

        collection->getIndexCatalog()->prepareInsertDeleteOptions(
            opCtx, descriptor, &index.options);

        // Allow duplicates when explicitly allowed or when using hybrid builds, which will perform
        // duplicate checking itself.
        index.options.dupsAllowed = index.options.dupsAllowed || _ignoreUnique ||
            index.block->getEntry()->isHybridBuilding();
        if (_ignoreUnique) {
            index.options.getKeysMode = IndexAccessMethod::GetKeysMode::kRelaxConstraints;
        }
        index.options.fromIndexBuilder = true;

        log() << "index build: starting on " << ns << " properties: " << descriptor->toString()
              << " using method: " << _method;
        if (index.bulk)
            log() << "build may temporarily use up to "
                  << eachIndexBuildMaxMemoryUsageBytes / 1024 / 1024 << " megabytes of RAM";

        index.filterExpression = index.block->getEntry()->getFilterExpression();

        // TODO SERVER-14888 Suppress this in cases we don't want to audit.
        audit::logCreateIndex(opCtx->getClient(), &info, descriptor->indexName(), ns);

        _indexes.push_back(std::move(index));
    }

    if (isBackgroundBuilding())
        _backgroundOperation.reset(new BackgroundOperation(ns));

    onInit();

    wunit.commit();

    if (MONGO_FAIL_POINT(crashAfterStartingIndexBuild)) {
        log() << "Index build interrupted due to 'crashAfterStartingIndexBuild' failpoint. Will "
                 "exit after waiting for changes to become durable (while holding onto locks).";
        // We are holding onto locks when calling waitUntilDurable, which is unsafe, but acceptable
        // for this failpoint until further work can be done. See SERVER-39591.
        if (opCtx->recoveryUnit()->waitUntilDurable()) {
            quickExit(EXIT_TEST);
        }
    }

    _setState(State::kRunning);

    return indexInfoObjs;
}

void failPointHangDuringBuild(FailPoint* fp, StringData where, const BSONObj& doc) {
    MONGO_FAIL_POINT_BLOCK(*fp, data) {
        int i = doc.getIntField("i");
        if (data.getData()["i"].numberInt() == i) {
            log() << "Hanging " << where << " index build of i=" << i;
            MONGO_FAIL_POINT_PAUSE_WHILE_SET((*fp));
        }
    }
}

Status MultiIndexBlock::insertAllDocumentsInCollection(OperationContext* opCtx,
                                                       Collection* collection) {
    invariant(opCtx->lockState()->isNoop() || !opCtx->lockState()->inAWriteUnitOfWork());

    // UUIDs are not guaranteed during startup because the check happens after indexes are rebuilt.
    if (_collectionUUID) {
        invariant(_collectionUUID.get() == collection->uuid().get());
    }

    // Refrain from persisting any multikey updates as a result from building the index. Instead,
    // accumulate them in the `MultikeyPathTracker` and do the write as part of the update that
    // commits the index.
    auto stopTracker = makeGuard(
        [this, opCtx] { MultikeyPathTracker::get(opCtx).stopTrackingMultikeyPathInfo(); });
    if (MultikeyPathTracker::get(opCtx).isTrackingMultikeyPathInfo()) {
        stopTracker.dismiss();
    }
    MultikeyPathTracker::get(opCtx).startTrackingMultikeyPathInfo();

    const char* curopMessage = "Index Build: scanning collection";
    const auto numRecords = collection->numRecords(opCtx);
    ProgressMeterHolder progress;
    {
        stdx::unique_lock<Client> lk(*opCtx->getClient());
        progress.set(CurOp::get(opCtx)->setProgress_inlock(curopMessage, numRecords));
    }

    Timer t;

    unsigned long long n = 0;

    PlanExecutor::YieldPolicy yieldPolicy;
    if (isBackgroundBuilding()) {
        yieldPolicy = PlanExecutor::YIELD_AUTO;
    } else {
        yieldPolicy = PlanExecutor::WRITE_CONFLICT_RETRY_ONLY;
    }
    auto exec =
        collection->makePlanExecutor(opCtx, yieldPolicy, Collection::ScanDirection::kForward);

    // Hint to the storage engine that this collection scan should not keep data in the cache.
    // Do not use read-once cursors for background builds because saveState/restoreState is called
    // with every insert into the index, which resets the collection scan cursor between every call
    // to getNextSnapshotted(). With read-once cursors enabled, this can evict data we may need to
    // read again, incurring a significant performance penalty.
    // Note: This does not apply to hybrid builds because they write keys to the external sorter.
    bool readOnce =
        _method != IndexBuildMethod::kBackground && useReadOnceCursorsForIndexBuilds.load();
    opCtx->recoveryUnit()->setReadOnce(readOnce);

    Snapshotted<BSONObj> objToIndex;
    RecordId loc;
    PlanExecutor::ExecState state;
    int retries = 0;  // non-zero when retrying our last document.
    while (retries ||
           (PlanExecutor::ADVANCED == (state = exec->getNextSnapshotted(&objToIndex, &loc))) ||
           MONGO_FAIL_POINT(hangAfterStartingIndexBuild)) {
        try {
            auto interruptStatus = opCtx->checkForInterruptNoAssert();
            if (!interruptStatus.isOK())
                return opCtx->checkForInterruptNoAssert();

            if (!retries && PlanExecutor::ADVANCED != state) {
                continue;
            }

            // Make sure we are working with the latest version of the document.
            if (objToIndex.snapshotId() != opCtx->recoveryUnit()->getSnapshotId() &&
                !collection->findDoc(opCtx, loc, &objToIndex)) {
                // Document was deleted so don't index it.
                retries = 0;
                continue;
            }

            // Done before insert so we can retry document if it WCEs.
            progress->setTotalWhileRunning(collection->numRecords(opCtx));

            failPointHangDuringBuild(&hangBeforeIndexBuildOf, "before", objToIndex.value());

            WriteUnitOfWork wunit(opCtx);
            Status ret = insert(opCtx, objToIndex.value(), loc);
            if (_method == IndexBuildMethod::kBackground)
                exec->saveState();
            if (!ret.isOK()) {
                // Fail the index build hard.
                return ret;
            }
            wunit.commit();
            if (_method == IndexBuildMethod::kBackground) {
                try {
                    exec->restoreState();  // Handles any WCEs internally.
                } catch (...) {
                    return exceptionToStatus();
                }
            }

            failPointHangDuringBuild(&hangAfterIndexBuildOf, "after", objToIndex.value());

            // Go to the next document
            progress->hit();
            n++;
            retries = 0;
        } catch (const WriteConflictException&) {
            // Only background builds write inside transactions, and therefore should only ever
            // generate WCEs.
            invariant(_method == IndexBuildMethod::kBackground);

            CurOp::get(opCtx)->debug().additiveMetrics.incrementWriteConflicts(1);
            retries++;  // logAndBackoff expects this to be 1 on first call.
            WriteConflictException::logAndBackoff(retries, "index creation", collection->ns().ns());

            // Can't use writeConflictRetry since we need to save/restore exec around call to
            // abandonSnapshot.
            exec->saveState();
            opCtx->recoveryUnit()->abandonSnapshot();
            try {
                exec->restoreState();  // Handles any WCEs internally.
            } catch (...) {
                return exceptionToStatus();
            }
        }
    }

    if (state != PlanExecutor::IS_EOF) {
        return exec->getMemberObjectStatus(objToIndex.value());
    }

    if (MONGO_FAIL_POINT(hangAfterStartingIndexBuildUnlocked)) {
        // Unlock before hanging so replication recognizes we've completed.
        Locker::LockSnapshot lockInfo;
        invariant(opCtx->lockState()->saveLockStateAndUnlock(&lockInfo));

        log() << "Hanging index build with no locks due to "
                 "'hangAfterStartingIndexBuildUnlocked' failpoint";
        MONGO_FAIL_POINT_PAUSE_WHILE_SET(hangAfterStartingIndexBuildUnlocked);

        if (isBackgroundBuilding()) {
            opCtx->lockState()->restoreLockState(opCtx, lockInfo);
            opCtx->recoveryUnit()->abandonSnapshot();
            return Status(ErrorCodes::OperationFailed,
                          "background index build aborted due to failpoint");
        } else {
            invariant(
                !"the hangAfterStartingIndexBuildUnlocked failpoint can't be turned off for foreground index builds");
        }
    }

    progress->finished();

    log() << "index build: collection scan done. scanned " << n << " total records in "
          << t.seconds() << " seconds";

    Status ret = dumpInsertsFromBulk(opCtx);
    if (!ret.isOK())
        return ret;

    return Status::OK();
}

Status MultiIndexBlock::insert(OperationContext* opCtx, const BSONObj& doc, const RecordId& loc) {
    if (State::kAborted == _getState()) {
        return {ErrorCodes::IndexBuildAborted,
                str::stream() << "Index build aborted: " << _abortReason};
    }

    for (size_t i = 0; i < _indexes.size(); i++) {
        if (_indexes[i].filterExpression && !_indexes[i].filterExpression->matchesBSON(doc)) {
            continue;
        }

        InsertResult result;
        Status idxStatus(ErrorCodes::InternalError, "");
        if (_indexes[i].bulk) {
            idxStatus = _indexes[i].bulk->insert(opCtx, doc, loc, _indexes[i].options);
        } else {
            idxStatus = _indexes[i].real->insert(opCtx, doc, loc, _indexes[i].options, &result);
        }

        if (!idxStatus.isOK())
            return idxStatus;
    }
    return Status::OK();
}

Status MultiIndexBlock::dumpInsertsFromBulk(OperationContext* opCtx) {
    return dumpInsertsFromBulk(opCtx, nullptr);
}

Status MultiIndexBlock::dumpInsertsFromBulk(OperationContext* opCtx,
                                            std::set<RecordId>* dupRecords) {
    if (State::kAborted == _getState()) {
        return {ErrorCodes::IndexBuildAborted,
                str::stream() << "Index build aborted: " << _abortReason};
    }

    invariant(opCtx->lockState()->isNoop() || !opCtx->lockState()->inAWriteUnitOfWork());
    for (size_t i = 0; i < _indexes.size(); i++) {
        if (_indexes[i].bulk == NULL)
            continue;

        // If 'dupRecords' is provided, it will be used to store all records that would result in
        // duplicate key errors. Only pass 'dupKeysInserted', which stores inserted duplicate keys,
        // when 'dupRecords' is not used because these two vectors are mutually incompatible.
        std::vector<BSONObj> dupKeysInserted;

        // When dupRecords is passed, 'dupsAllowed' should be passed to reflect whether or not the
        // index is unique.
        bool dupsAllowed = (dupRecords) ? !_indexes[i].block->getEntry()->descriptor()->unique()
                                        : _indexes[i].options.dupsAllowed;

        IndexCatalogEntry* entry = _indexes[i].block->getEntry();
        LOG(1) << "index build: inserting from external sorter into index: "
               << entry->descriptor()->indexName();
        Status status = _indexes[i].real->commitBulk(opCtx,
                                                     _indexes[i].bulk.get(),
                                                     dupsAllowed,
                                                     dupRecords,
                                                     (dupRecords) ? nullptr : &dupKeysInserted);
        if (!status.isOK()) {
            return status;
        }

        // Do not record duplicates when explicitly ignored. This may be the case on secondaries.
        auto interceptor = entry->indexBuildInterceptor();
        if (!interceptor || _ignoreUnique) {
            continue;
        }

        // Record duplicate key insertions for later verification.
        if (dupKeysInserted.size()) {
            status = interceptor->recordDuplicateKeys(opCtx, dupKeysInserted);
            if (!status.isOK()) {
                return status;
            }
        }
    }

    _updateCurOpOpDescription(opCtx, true);
    return Status::OK();
}

Status MultiIndexBlock::drainBackgroundWrites(OperationContext* opCtx,
                                              RecoveryUnit::ReadSource readSource) {
    if (State::kAborted == _getState()) {
        return {ErrorCodes::IndexBuildAborted,
                str::stream() << "Index build aborted: " << _abortReason
                              << ". Cannot complete drain phase for index build"
                              << (_collectionUUID ? (" on collection '" +
                                                     _collectionUUID.get().toString() + "'")
                                                  : ".")};
    }

    invariant(!opCtx->lockState()->inAWriteUnitOfWork());

    // Drain side-writes table for each index. This only drains what is visible. Assuming intent
    // locks are held on the user collection, more writes can come in after this drain completes.
    // Callers are responsible for stopping writes by holding an S or X lock while draining before
    // completing the index build.
    for (size_t i = 0; i < _indexes.size(); i++) {
        auto interceptor = _indexes[i].block->getEntry()->indexBuildInterceptor();
        if (!interceptor)
            continue;

        auto status = interceptor->drainWritesIntoIndex(opCtx, _indexes[i].options, readSource);
        if (!status.isOK()) {
            return status;
        }
    }
    return Status::OK();
}


Status MultiIndexBlock::checkConstraints(OperationContext* opCtx) {
    if (State::kAborted == _getState()) {
        return {ErrorCodes::IndexBuildAborted,
                str::stream() << "Index build aborted: " << _abortReason
                              << ". Cannot complete constraint checking for index build"
                              << (_collectionUUID ? (" on collection '" +
                                                     _collectionUUID.get().toString() + "'")
                                                  : ".")};
    }

    // For each index that may be unique, check that no recorded duplicates still exist. This can
    // only check what is visible on the index. Callers are responsible for ensuring all writes to
    // the collection are visible.
    for (size_t i = 0; i < _indexes.size(); i++) {
        auto interceptor = _indexes[i].block->getEntry()->indexBuildInterceptor();
        if (!interceptor)
            continue;

        auto status = interceptor->checkDuplicateKeyConstraints(opCtx);
        if (!status.isOK()) {
            return status;
        }
    }
    return Status::OK();
}

void MultiIndexBlock::abortWithoutCleanup() {
    _setStateToAbortedIfNotCommitted("aborted without cleanup"_sd);
    _indexes.clear();
    _needToCleanup = false;
}

MultiIndexBlock::OnCreateEachFn MultiIndexBlock::kNoopOnCreateEachFn = [](const BSONObj& spec) {};
MultiIndexBlock::OnCommitFn MultiIndexBlock::kNoopOnCommitFn = []() {};

Status MultiIndexBlock::commit(OperationContext* opCtx,
                               Collection* collection,
                               OnCreateEachFn onCreateEach,
                               OnCommitFn onCommit) {
    // UUIDs are not guaranteed during startup because the check happens after indexes are rebuilt.
    if (_collectionUUID) {
        invariant(_collectionUUID.get() == collection->uuid().get());
    }

    if (State::kAborted == _getState()) {
        return {
            ErrorCodes::IndexBuildAborted,
            str::stream() << "Index build aborted: " << _abortReason
                          << ". Cannot commit index builder: "
                          << collection->ns()
                          << (_collectionUUID ? (" (" + _collectionUUID->toString() + ")") : "")};
    }

    // Do not interfere with writing multikey information when committing index builds.
    auto restartTracker = makeGuard(
        [this, opCtx] { MultikeyPathTracker::get(opCtx).startTrackingMultikeyPathInfo(); });
    if (!MultikeyPathTracker::get(opCtx).isTrackingMultikeyPathInfo()) {
        restartTracker.dismiss();
    }
    MultikeyPathTracker::get(opCtx).stopTrackingMultikeyPathInfo();

    for (size_t i = 0; i < _indexes.size(); i++) {
        onCreateEach(_indexes[i].block->getSpec());

        // Do this before calling success(), which unsets the interceptor pointer on the index
        // catalog entry.
        auto interceptor = _indexes[i].block->getEntry()->indexBuildInterceptor();
        if (interceptor) {
            auto multikeyPaths = interceptor->getMultikeyPaths();
            if (multikeyPaths) {
                _indexes[i].block->getEntry()->setMultikey(opCtx, multikeyPaths.get());
            }
        }

        _indexes[i].block->success(opCtx, collection);

        // The bulk builder will track multikey information itself. Non-bulk builders re-use the
        // code path that a typical insert/update uses. State is altered on the non-bulk build
        // path to accumulate the multikey information on the `MultikeyPathTracker`.
        if (_indexes[i].bulk) {
            const auto& bulkBuilder = _indexes[i].bulk;
            if (bulkBuilder->isMultikey()) {
                _indexes[i].block->getEntry()->setMultikey(opCtx, bulkBuilder->getMultikeyPaths());
            }
        } else {
            auto multikeyPaths =
                boost::optional<MultikeyPaths>(MultikeyPathTracker::get(opCtx).getMultikeyPathInfo(
                    collection->ns(), _indexes[i].block->getIndexName()));
            if (multikeyPaths) {
                _indexes[i].block->getEntry()->setMultikey(opCtx, *multikeyPaths);
            }
        }
    }

    onCommit();

    // The state of this index build is set to Committed only when the WUOW commits.
    // It is possible for abort() to be called after the check at the beginning of this function and
    // before the WUOW is committed. If the WUOW commits, the final state of this index builder will
    // be Committed. Otherwise, the index builder state will remain as Aborted and further attempts
    // to commit this index build will fail.
    opCtx->recoveryUnit()->onCommit(
        [this](boost::optional<Timestamp> commitTime) { _setState(State::kCommitted); });

    // On rollback sets MultiIndexBlock::_needToCleanup to true.
    opCtx->recoveryUnit()->onRollback([this]() { _needToCleanup = true; });
    _needToCleanup = false;

    return Status::OK();
}

bool MultiIndexBlock::isCommitted() const {
    return State::kCommitted == _getState();
}

void MultiIndexBlock::abort(StringData reason) {
    _setStateToAbortedIfNotCommitted(reason);
}


bool MultiIndexBlock::isBackgroundBuilding() const {
    return _method == IndexBuildMethod::kBackground || _method == IndexBuildMethod::kHybrid;
}

MultiIndexBlock::State MultiIndexBlock::getState_forTest() const {
    return _getState();
}

MultiIndexBlock::State MultiIndexBlock::_getState() const {
    stdx::lock_guard<stdx::mutex> lock(_mutex);
    return _state;
}

void MultiIndexBlock::_setState(State newState) {
    invariant(State::kAborted != newState);
    stdx::lock_guard<stdx::mutex> lock(_mutex);
    _state = newState;
}

void MultiIndexBlock::_setStateToAbortedIfNotCommitted(StringData reason) {
    stdx::lock_guard<stdx::mutex> lock(_mutex);
    if (State::kCommitted == _state) {
        return;
    }
    _state = State::kAborted;
    _abortReason = reason.toString();
}

void MultiIndexBlock::_updateCurOpOpDescription(OperationContext* opCtx,
                                                bool isBuildingPhaseComplete) const {
    BSONObjBuilder builder;

    // TODO(SERVER-37980): Replace with index build UUID.
    auto buildUUID = UUID::gen();
    buildUUID.appendToBuilder(&builder, kBuildUUIDFieldName);

    builder.append(kBuildingPhaseCompleteFieldName, isBuildingPhaseComplete);

    builder.appendBool(kRunTwoPhaseIndexBuildFieldName, false);

    auto replCoord = repl::ReplicationCoordinator::get(opCtx);
    if (replCoord->isReplEnabled()) {
        // TODO(SERVER-37939): Update the membersBuilder array to state the actual commit ready
        // members.
        BSONArrayBuilder membersBuilder;
        auto config = replCoord->getConfig();
        for (auto it = config.membersBegin(); it != config.membersEnd(); ++it) {
            const auto& memberConfig = *it;
            if (memberConfig.isArbiter()) {
                continue;
            }
            membersBuilder.append(memberConfig.getHostAndPort().toString());
        }
        builder.append(kCommitReadyMembersFieldName, membersBuilder.arr());
    }

    stdx::unique_lock<Client> lk(*opCtx->getClient());
    auto curOp = CurOp::get(opCtx);
    builder.appendElementsUnique(curOp->opDescription());
    auto opDescObj = builder.obj();
    curOp->setOpDescription_inlock(opDescObj);
    curOp->ensureStarted();
}

std::ostream& operator<<(std::ostream& os, const MultiIndexBlock::State& state) {
    switch (state) {
        case MultiIndexBlock::State::kUninitialized:
            return os << "Uninitialized";
        case MultiIndexBlock::State::kRunning:
            return os << "Running";
        case MultiIndexBlock::State::kCommitted:
            return os << "Committed";
        case MultiIndexBlock::State::kAborted:
            return os << "Aborted";
    }
    MONGO_UNREACHABLE;
}

logger::LogstreamBuilder& operator<<(logger::LogstreamBuilder& out,
                                     const IndexBuildMethod& method) {
    switch (method) {
        case IndexBuildMethod::kHybrid:
            out.stream() << "Hybrid";
            break;
        case IndexBuildMethod::kBackground:
            out.stream() << "Background";
            break;
        case IndexBuildMethod::kForeground:
            out.stream() << "Foreground";
            break;
    }
    return out;
}

}  // namespace mongo
