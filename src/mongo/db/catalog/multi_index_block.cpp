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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kIndex

#include "mongo/platform/basic.h"

#include "mongo/db/catalog/multi_index_block.h"

#include <ostream>

#include "mongo/base/error_codes.h"
#include "mongo/bson/simple_bsonelement_comparator.h"
#include "mongo/db/audit.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/db/catalog/collection_catalog.h"
#include "mongo/db/catalog/multi_index_block_gen.h"
#include "mongo/db/catalog/uncommitted_collections.h"
#include "mongo/db/client.h"
#include "mongo/db/concurrency/write_conflict_exception.h"
#include "mongo/db/index/multikey_paths.h"
#include "mongo/db/multi_key_path_tracker.h"
#include "mongo/db/op_observer.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/query/collection_query_info.h"
#include "mongo/db/repl/repl_set_config.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/repl/tenant_migration_conflict_info.h"
#include "mongo/db/storage/storage_options.h"
#include "mongo/db/storage/write_unit_of_work.h"
#include "mongo/logv2/log.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/progress_meter.h"
#include "mongo/util/quick_exit.h"
#include "mongo/util/scopeguard.h"
#include "mongo/util/uuid.h"

namespace mongo {

MONGO_FAIL_POINT_DEFINE(hangAfterSettingUpIndexBuild);
MONGO_FAIL_POINT_DEFINE(hangAfterSettingUpIndexBuildUnlocked);
MONGO_FAIL_POINT_DEFINE(hangAfterStartingIndexBuild);
MONGO_FAIL_POINT_DEFINE(hangAfterStartingIndexBuildUnlocked);
MONGO_FAIL_POINT_DEFINE(hangIndexBuildDuringCollectionScanPhaseBeforeInsertion);
MONGO_FAIL_POINT_DEFINE(hangIndexBuildDuringCollectionScanPhaseAfterInsertion);
MONGO_FAIL_POINT_DEFINE(leaveIndexBuildUnfinishedForShutdown);

MultiIndexBlock::~MultiIndexBlock() {
    invariant(_buildIsCleanedUp);
}

MultiIndexBlock::OnCleanUpFn MultiIndexBlock::kNoopOnCleanUpFn = []() {};

void MultiIndexBlock::abortIndexBuild(OperationContext* opCtx,
                                      Collection* collection,
                                      OnCleanUpFn onCleanUp) noexcept {
    if (_collectionUUID) {
        // init() was previously called with a collection pointer, so ensure that the same
        // collection is being provided for clean up and the interface in not being abused.
        invariant(_collectionUUID.get() == collection->uuid());
    }

    if (_buildIsCleanedUp) {
        return;
    }

    auto nss = collection->ns();
    UncommittedCollections::get(opCtx).invariantHasExclusiveAccessToCollection(opCtx, nss);

    while (true) {
        try {
            WriteUnitOfWork wunit(opCtx);
            // This cleans up all index builds. Because that may need to write, it is done inside of
            // a WUOW. Nothing inside this block can fail, and it is made fatal if it does.
            for (size_t i = 0; i < _indexes.size(); i++) {
                _indexes[i].block->fail(opCtx, collection);
                _indexes[i].block->finalizeTemporaryTables(
                    opCtx, TemporaryRecordStore::FinalizationAction::kDelete);
            }

            onCleanUp();

            wunit.commit();
            _buildIsCleanedUp = true;
            return;
        } catch (const WriteConflictException&) {
            continue;
        } catch (const DBException& e) {
            if (e.toStatus() == ErrorCodes::ExceededMemoryLimit)
                continue;
            LOGV2_ERROR(20393,
                        "Caught exception while cleaning up partially built indexes: {e}",
                        "Caught exception while cleaning up partially built indexes",
                        "error"_attr = redact(e));
        } catch (const std::exception& e) {
            LOGV2_ERROR(20394,
                        "Caught exception while cleaning up partially built indexes: {e_what}",
                        "Caught exception while cleaning up partially built indexes",
                        "error"_attr = e.what());
        } catch (...) {
            LOGV2_ERROR(20395,
                        "Caught unknown exception while cleaning up partially built indexes");
        }
        fassertFailed(18644);
    }
}

void MultiIndexBlock::ignoreUniqueConstraint() {
    _ignoreUnique = true;
}

MultiIndexBlock::OnInitFn MultiIndexBlock::kNoopOnInitFn =
    [](std::vector<BSONObj>& specs) -> Status { return Status::OK(); };

MultiIndexBlock::OnInitFn MultiIndexBlock::makeTimestampedIndexOnInitFn(OperationContext* opCtx,
                                                                        const Collection* coll) {
    return [opCtx, ns = coll->ns()](std::vector<BSONObj>& specs) -> Status {
        opCtx->getServiceContext()->getOpObserver()->onStartIndexBuildSinglePhase(opCtx, ns);
        return Status::OK();
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
    invariant(opCtx->lockState()->isCollectionLockedForMode(collection->ns(), MODE_X),
              str::stream() << "Collection " << collection->ns() << " with UUID "
                            << collection->uuid() << " is holding the incorrect lock");
    _collectionUUID = collection->uuid();

    _buildIsCleanedUp = false;

    WriteUnitOfWork wunit(opCtx);

    invariant(_indexes.empty());

    // Guarantees that exceptions cannot be returned from index builder initialization except for
    // WriteConflictExceptions, which should be dealt with by the caller.
    try {
        // On rollback in init(), cleans up _indexes so that ~MultiIndexBlock doesn't try to clean
        // up _indexes manually (since the changes were already rolled back). Due to this, it is
        // thus legal to call init() again after it fails.
        opCtx->recoveryUnit()->onRollback([this, opCtx]() {
            for (auto& index : _indexes) {
                index.block->finalizeTemporaryTables(
                    opCtx, TemporaryRecordStore::FinalizationAction::kDelete);
            }
            _indexes.clear();
            _buildIsCleanedUp = true;
        });

        const auto& ns = collection->ns().ns();

        for (const auto& info : indexSpecs) {
            if (info["background"].isBoolean() && !info["background"].Bool()) {
                LOGV2(20383,
                      "Ignoring obsolete {{ background: false }} index build option because all "
                      "indexes are built in the background with the hybrid method",
                      "Ignoring obsolete { background: false } index build option because all "
                      "indexes are built in the background with the hybrid method");
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
            if (!status.isOK()) {
                // If we were given two identical indexes to build, we will run into an error trying
                // to set up the same index a second time in this for-loop. This is the only way to
                // encounter this error because callers filter out ready/in-progress indexes and
                // start the build while holding a lock throughout.
                if (status == ErrorCodes::IndexBuildAlreadyInProgress) {
                    invariant(indexSpecs.size() > 1,
                              str::stream() << "Collection: " << ns << " (" << _collectionUUID
                                            << "), Index spec: " << indexSpecs.front());
                    return {
                        ErrorCodes::OperationFailed,
                        "Cannot build two identical indexes. Try again without duplicate indexes."};
                }
                return status;
            }
            info = statusWithInfo.getValue();
            indexInfoObjs.push_back(info);

            IndexToBuild index;
            index.block = std::make_unique<IndexBuildBlock>(
                collection->getIndexCatalog(), collection->ns(), info, _method, _buildUUID);
            status = index.block->init(opCtx, collection);
            if (!status.isOK())
                return status;

            auto indexCleanupGuard = makeGuard([opCtx, &index] {
                index.block->finalizeTemporaryTables(
                    opCtx, TemporaryRecordStore::FinalizationAction::kDelete);
            });

            index.real = index.block->getEntry()->accessMethod();
            status = index.real->initializeAsEmpty(opCtx);
            if (!status.isOK())
                return status;

            index.bulk = index.real->initiateBulk(eachIndexBuildMaxMemoryUsageBytes);

            const IndexDescriptor* descriptor = index.block->getEntry()->descriptor();

            collection->getIndexCatalog()->prepareInsertDeleteOptions(
                opCtx, collection->ns(), descriptor, &index.options);

            // Index builds always relax constraints and check for violations at commit-time.
            index.options.getKeysMode = IndexAccessMethod::GetKeysMode::kRelaxConstraints;
            index.options.dupsAllowed = true;
            index.options.fromIndexBuilder = true;

            LOGV2(20384,
                  "Index build: starting on {namespace} properties: {properties} using method: "
                  "{method}",
                  "Index build: starting",
                  "namespace"_attr = ns,
                  "buildUUID"_attr = _buildUUID,
                  "properties"_attr = *descriptor,
                  "method"_attr = _method,
                  "maxTemporaryMemoryUsageMB"_attr =
                      eachIndexBuildMaxMemoryUsageBytes / 1024 / 1024);

            index.filterExpression = index.block->getEntry()->getFilterExpression();

            // TODO SERVER-14888 Suppress this in cases we don't want to audit.
            audit::logCreateIndex(opCtx->getClient(), &info, descriptor->indexName(), ns);

            indexCleanupGuard.dismiss();
            _indexes.push_back(std::move(index));
        }

        Status status = onInit(indexInfoObjs);
        if (!status.isOK()) {
            return status;
        }

        opCtx->recoveryUnit()->onCommit([ns, this](auto commitTs) {
            if (!_buildUUID) {
                return;
            }

            LOGV2(20346,
                  "Index build initialized: {buildUUID}: {nss} ({collection_uuid}): indexes: "
                  "{indexes_size}",
                  "Index build: initialized",
                  "buildUUID"_attr = _buildUUID,
                  "namespace"_attr = ns,
                  "collectionUUID"_attr = _collectionUUID,
                  "initializationTimestamp"_attr = commitTs);
        });

        wunit.commit();
        return indexInfoObjs;
    } catch (const WriteConflictException&) {
        // Avoid converting WCE to Status.
        throw;
    } catch (const TenantMigrationConflictException&) {
        // Avoid converting TenantMigrationConflictException to Status.
        throw;
    } catch (...) {
        auto status = exceptionToStatus();
        return {status.code(),
                str::stream() << "Caught exception during index builder initialization "
                              << collection->ns() << " (" << collection->uuid()
                              << "): " << status.reason() << ". " << indexSpecs.size()
                              << " provided. First index spec: "
                              << (indexSpecs.empty() ? BSONObj() : indexSpecs[0])};
    }
}

void failPointHangDuringBuild(FailPoint* fp, StringData where, const BSONObj& doc) {
    fp->executeIf(
        [&](const BSONObj& data) {
            LOGV2(20386,
                  "Hanging index build during collection scan phase insertion",
                  "where"_attr = where,
                  "doc"_attr = doc);

            fp->pauseWhileSet();
        },
        [&doc](const BSONObj& data) {
            auto fieldsToMatch = data.getObjectField("fieldsToMatch");
            return std::all_of(
                fieldsToMatch.begin(), fieldsToMatch.end(), [&doc](const auto& elem) {
                    return SimpleBSONElementComparator::kInstance.evaluate(elem ==
                                                                           doc[elem.fieldName()]);
                });
        });
}

Status MultiIndexBlock::insertAllDocumentsInCollection(OperationContext* opCtx,
                                                       Collection* collection) {
    invariant(!_buildIsCleanedUp);
    invariant(opCtx->lockState()->isNoop() || !opCtx->lockState()->inAWriteUnitOfWork());

    // UUIDs are not guaranteed during startup because the check happens after indexes are rebuilt.
    if (_collectionUUID) {
        invariant(_collectionUUID.get() == collection->uuid());
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

    if (MONGO_unlikely(hangAfterSettingUpIndexBuild.shouldFail())) {
        // Hang the build after the curOP info is set up.
        LOGV2(20387, "Hanging index build due to failpoint 'hangAfterSettingUpIndexBuild'");
        hangAfterSettingUpIndexBuild.pauseWhileSet();
    }

    if (MONGO_unlikely(hangAfterSettingUpIndexBuildUnlocked.shouldFail())) {
        uassert(4585200, "failpoint may not be set on foreground indexes", isBackgroundBuilding());

        // Unlock before hanging so replication recognizes we've completed.
        Locker::LockSnapshot lockInfo;
        invariant(opCtx->lockState()->saveLockStateAndUnlock(&lockInfo));

        LOGV2(4585201,
              "Hanging index build with no locks due to "
              "'hangAfterSettingUpIndexBuildUnlocked' failpoint");
        hangAfterSettingUpIndexBuildUnlocked.pauseWhileSet();

        opCtx->lockState()->restoreLockState(opCtx, lockInfo);
        opCtx->recoveryUnit()->abandonSnapshot();
    }

    Timer t;

    unsigned long long n = 0;

    PlanYieldPolicy::YieldPolicy yieldPolicy;
    if (isBackgroundBuilding()) {
        yieldPolicy = PlanYieldPolicy::YieldPolicy::YIELD_AUTO;
    } else {
        yieldPolicy = PlanYieldPolicy::YieldPolicy::WRITE_CONFLICT_RETRY_ONLY;
    }
    auto exec =
        collection->makePlanExecutor(opCtx, yieldPolicy, Collection::ScanDirection::kForward);

    // Hint to the storage engine that this collection scan should not keep data in the cache.
    bool readOnce = useReadOnceCursorsForIndexBuilds.load();
    opCtx->recoveryUnit()->setReadOnce(readOnce);

    try {
        invariant(_phase == IndexBuildPhaseEnum::kInitialized,
                  IndexBuildPhase_serializer(_phase).toString());
        _phase = IndexBuildPhaseEnum::kCollectionScan;

        BSONObj objToIndex;
        RecordId loc;
        PlanExecutor::ExecState state;
        while (PlanExecutor::ADVANCED == (state = exec->getNext(&objToIndex, &loc)) ||
               MONGO_unlikely(hangAfterStartingIndexBuild.shouldFail())) {
            auto interruptStatus = opCtx->checkForInterruptNoAssert();
            if (!interruptStatus.isOK())
                return opCtx->checkForInterruptNoAssert();

            if (PlanExecutor::ADVANCED != state) {
                continue;
            }

            progress->setTotalWhileRunning(collection->numRecords(opCtx));

            failPointHangDuringBuild(
                &hangIndexBuildDuringCollectionScanPhaseBeforeInsertion, "before", objToIndex);

            // The external sorter is not part of the storage engine and therefore does not need a
            // WriteUnitOfWork to write keys.
            Status ret = insertSingleDocumentForInitialSyncOrRecovery(opCtx, objToIndex, loc);
            if (!ret.isOK()) {
                return ret;
            }

            failPointHangDuringBuild(
                &hangIndexBuildDuringCollectionScanPhaseAfterInsertion, "after", objToIndex);

            // Go to the next document.
            progress->hit();
            n++;
        }
    } catch (...) {
        _phase = IndexBuildPhaseEnum::kInitialized;
        return exceptionToStatus();
    }

    if (MONGO_unlikely(leaveIndexBuildUnfinishedForShutdown.shouldFail())) {
        LOGV2(20389,
              "Index build interrupted due to 'leaveIndexBuildUnfinishedForShutdown' failpoint. "
              "Mimicking shutdown error code");
        return Status(
            ErrorCodes::InterruptedAtShutdown,
            "background index build interrupted due to failpoint. returning a shutdown error.");
    }

    if (MONGO_unlikely(hangAfterStartingIndexBuildUnlocked.shouldFail())) {
        // Unlock before hanging so replication recognizes we've completed.
        Locker::LockSnapshot lockInfo;
        invariant(opCtx->lockState()->saveLockStateAndUnlock(&lockInfo));

        LOGV2(20390,
              "Hanging index build with no locks due to "
              "'hangAfterStartingIndexBuildUnlocked' failpoint");
        hangAfterStartingIndexBuildUnlocked.pauseWhileSet();

        if (isBackgroundBuilding()) {
            opCtx->lockState()->restoreLockState(opCtx, lockInfo);
            opCtx->recoveryUnit()->abandonSnapshot();
        } else {
            invariant(!"the hangAfterStartingIndexBuildUnlocked failpoint can't be turned off for foreground index builds");
        }
    }

    progress->finished();

    LOGV2(20391,
          "Index build: collection scan done. scanned {n} total records in {t_seconds} seconds",
          "Index build: collection scan done",
          "buildUUID"_attr = _buildUUID,
          "totalRecords"_attr = n,
          "duration"_attr = duration_cast<Milliseconds>(Seconds(t.seconds())));

    Status ret = dumpInsertsFromBulk(opCtx);
    if (!ret.isOK())
        return ret;

    return Status::OK();
}

Status MultiIndexBlock::insertSingleDocumentForInitialSyncOrRecovery(OperationContext* opCtx,
                                                                     const BSONObj& doc,
                                                                     const RecordId& loc) {
    invariant(!_buildIsCleanedUp);
    for (size_t i = 0; i < _indexes.size(); i++) {
        if (_indexes[i].filterExpression && !_indexes[i].filterExpression->matchesBSON(doc)) {
            continue;
        }

        Status idxStatus = Status::OK();

        // When calling insert, BulkBuilderImpl's Sorter performs file I/O that may result in an
        // exception.
        try {
            idxStatus = _indexes[i].bulk->insert(opCtx, doc, loc, _indexes[i].options);
        } catch (...) {
            return exceptionToStatus();
        }

        if (!idxStatus.isOK())
            return idxStatus;
    }

    _lastRecordIdInserted = loc;

    return Status::OK();
}

Status MultiIndexBlock::dumpInsertsFromBulk(OperationContext* opCtx) {
    return dumpInsertsFromBulk(opCtx, nullptr);
}

Status MultiIndexBlock::dumpInsertsFromBulk(OperationContext* opCtx,
                                            std::set<RecordId>* dupRecords) {
    invariant(!_buildIsCleanedUp);
    invariant(opCtx->lockState()->isNoop() || !opCtx->lockState()->inAWriteUnitOfWork());

    // Initial sync adds documents to the sorter using
    // insertSingleDocumentForInitialSyncOrRecovery() instead of delegating to
    // insertDocumentsInCollection() to scan and insert the contents of the collection.
    // Therefore, it is possible for the phase of this MultiIndexBlock to be kInitialized
    // rather than kCollection when this function is called.
    invariant(_phase == IndexBuildPhaseEnum::kCollectionScan ||
                  _phase == IndexBuildPhaseEnum::kInitialized,
              IndexBuildPhase_serializer(_phase).toString());
    _phase = IndexBuildPhaseEnum::kBulkLoad;

    for (size_t i = 0; i < _indexes.size(); i++) {
        // When dupRecords is passed, 'dupsAllowed' should be passed to reflect whether or not the
        // index is unique.
        bool dupsAllowed = (dupRecords) ? !_indexes[i].block->getEntry()->descriptor()->unique()
                                        : _indexes[i].options.dupsAllowed;

        IndexCatalogEntry* entry = _indexes[i].block->getEntry();
        LOGV2_DEBUG(
            20392,
            1,
            "index build: inserting from external sorter into index: {entry_descriptor_indexName}",
            "entry_descriptor_indexName"_attr = entry->descriptor()->indexName());

        // SERVER-41918 This call to commitBulk() results in file I/O that may result in an
        // exception.
        try {
            Status status = _indexes[i].real->commitBulk(
                opCtx,
                _indexes[i].bulk.get(),
                dupsAllowed,
                [=](const KeyString::Value& duplicateKey) {
                    // Do not record duplicates when explicitly ignored. This may be the case on
                    // secondaries.
                    return dupsAllowed && !dupRecords && !_ignoreUnique &&
                            entry->indexBuildInterceptor()
                        ? entry->indexBuildInterceptor()->recordDuplicateKey(opCtx, duplicateKey)
                        : Status::OK();
                },
                dupRecords);

            if (!status.isOK()) {
                return status;
            }
        } catch (...) {
            return exceptionToStatus();
        }
    }

    return Status::OK();
}

Status MultiIndexBlock::drainBackgroundWrites(
    OperationContext* opCtx,
    RecoveryUnit::ReadSource readSource,
    IndexBuildInterceptor::DrainYieldPolicy drainYieldPolicy) {
    invariant(!_buildIsCleanedUp);
    invariant(!opCtx->lockState()->inAWriteUnitOfWork());

    // Background writes are drained three times (once without blocking writes and twice blocking
    // writes), so we may either be coming from the bulk load phase or be already in the drain
    // writes phase.
    invariant(_phase == IndexBuildPhaseEnum::kBulkLoad ||
                  _phase == IndexBuildPhaseEnum::kDrainWrites,
              IndexBuildPhase_serializer(_phase).toString());
    _phase = IndexBuildPhaseEnum::kDrainWrites;

    const Collection* coll =
        CollectionCatalog::get(opCtx).lookupCollectionByUUID(opCtx, _collectionUUID.get());

    // Drain side-writes table for each index. This only drains what is visible. Assuming intent
    // locks are held on the user collection, more writes can come in after this drain completes.
    // Callers are responsible for stopping writes by holding an S or X lock while draining before
    // completing the index build.
    for (size_t i = 0; i < _indexes.size(); i++) {
        auto interceptor = _indexes[i].block->getEntry()->indexBuildInterceptor();
        if (!interceptor)
            continue;

        // Track duplicates for later constraint checking for all index builds, except when
        // _ignoreUnique is set explicitly.
        auto trackDups = !_ignoreUnique ? IndexBuildInterceptor::TrackDuplicates::kTrack
                                        : IndexBuildInterceptor::TrackDuplicates::kNoTrack;
        auto status = interceptor->drainWritesIntoIndex(
            opCtx, coll, _indexes[i].options, trackDups, drainYieldPolicy);
        if (!status.isOK()) {
            return status;
        }
    }
    return Status::OK();
}

Status MultiIndexBlock::retrySkippedRecords(OperationContext* opCtx, Collection* collection) {
    invariant(!_buildIsCleanedUp);
    for (auto&& index : _indexes) {
        auto interceptor = index.block->getEntry()->indexBuildInterceptor();
        if (!interceptor)
            continue;

        auto status = interceptor->retrySkippedRecords(opCtx, collection);
        if (!status.isOK()) {
            return status;
        }
    }
    return Status::OK();
}

Status MultiIndexBlock::checkConstraints(OperationContext* opCtx) {
    invariant(!_buildIsCleanedUp);

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

void MultiIndexBlock::abortWithoutCleanupForRollback(OperationContext* opCtx) {
    _abortWithoutCleanup(opCtx, false /* shutdown */);
}

void MultiIndexBlock::abortWithoutCleanupForShutdown(OperationContext* opCtx) {
    _abortWithoutCleanup(opCtx, true /* shutdown */);
}

MultiIndexBlock::OnCreateEachFn MultiIndexBlock::kNoopOnCreateEachFn = [](const BSONObj& spec) {};
MultiIndexBlock::OnCommitFn MultiIndexBlock::kNoopOnCommitFn = []() {};

Status MultiIndexBlock::commit(OperationContext* opCtx,
                               Collection* collection,
                               OnCreateEachFn onCreateEach,
                               OnCommitFn onCommit) {
    invariant(!_buildIsCleanedUp);
    invariant(opCtx->lockState()->isCollectionLockedForMode(collection->ns(), MODE_X),
              str::stream() << "Collection " << collection->ns() << " with UUID "
                            << collection->uuid() << " is holding the incorrect lock");

    // UUIDs are not guaranteed during startup because the check happens after indexes are rebuilt.
    if (_collectionUUID) {
        invariant(_collectionUUID.get() == collection->uuid());
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
                _indexes[i].block->getEntry()->setMultikey(opCtx, collection, multikeyPaths.get());
            }
        }

        _indexes[i].block->success(opCtx, collection);

        // The bulk builder will track multikey information itself.
        const auto& bulkBuilder = _indexes[i].bulk;
        if (bulkBuilder->isMultikey()) {
            _indexes[i].block->getEntry()->setMultikey(
                opCtx, collection, bulkBuilder->getMultikeyPaths());
        }

        // The commit() function can be called multiple times on write conflict errors. Dropping the
        // temp tables cannot be rolled back, so do it only after the WUOW commits.
        opCtx->recoveryUnit()->onCommit([opCtx, i, this](auto commitTs) {
            _indexes[i].block->finalizeTemporaryTables(
                opCtx, TemporaryRecordStore::FinalizationAction::kDelete);
        });
    }

    onCommit();

    opCtx->recoveryUnit()->onCommit([collection, this](boost::optional<Timestamp> commitTime) {
        CollectionQueryInfo::get(collection).clearQueryCache(collection);
        _buildIsCleanedUp = true;
    });

    return Status::OK();
}

bool MultiIndexBlock::isBackgroundBuilding() const {
    return _method == IndexBuildMethod::kHybrid;
}

void MultiIndexBlock::setIndexBuildMethod(IndexBuildMethod indexBuildMethod) {
    _method = indexBuildMethod;
}

void MultiIndexBlock::_abortWithoutCleanup(OperationContext* opCtx, bool shutdown) {
    invariant(!_buildIsCleanedUp);
    UninterruptibleLockGuard noInterrupt(opCtx->lockState());
    // Lock if it's not already locked, to ensure storage engine cannot be destructed out from
    // underneath us.
    boost::optional<Lock::GlobalLock> lk;
    if (!opCtx->lockState()->isWriteLocked()) {
        lk.emplace(opCtx, MODE_IX);
    }

    auto action = TemporaryRecordStore::FinalizationAction::kDelete;

    if (_shouldWriteStateToDisk(opCtx, shutdown)) {
        _writeStateToDisk(opCtx);
        action = TemporaryRecordStore::FinalizationAction::kKeep;
    }

    for (auto& index : _indexes) {
        index.block->finalizeTemporaryTables(opCtx, action);
    }

    _buildIsCleanedUp = true;
}

bool MultiIndexBlock::_shouldWriteStateToDisk(OperationContext* opCtx, bool shutdown) const {
    return shutdown && _buildUUID && !_buildIsCleanedUp && _method == IndexBuildMethod::kHybrid &&
        opCtx->getServiceContext()->getStorageEngine()->supportsResumableIndexBuilds();
}

void MultiIndexBlock::_writeStateToDisk(OperationContext* opCtx) const {
    auto obj = _constructStateObject();
    auto rs = opCtx->getServiceContext()->getStorageEngine()->makeTemporaryRecordStore(opCtx);

    WriteUnitOfWork wuow(opCtx);

    auto status = rs->rs()->insertRecord(opCtx, obj.objdata(), obj.objsize(), Timestamp());
    if (!status.isOK()) {
        LOGV2_ERROR(4841501,
                    "Failed to write resumable index build state to disk",
                    logAttrs(*_buildUUID),
                    "error"_attr = status.getStatus());
        dassert(status,
                str::stream() << "Failed to write resumable index build state to disk. UUID: "
                              << *_buildUUID);

        rs->finalizeTemporaryTable(opCtx, TemporaryRecordStore::FinalizationAction::kDelete);
        return;
    }

    wuow.commit();

    LOGV2(4841502, "Wrote resumable index build state to disk", logAttrs(*_buildUUID));

    rs->finalizeTemporaryTable(opCtx, TemporaryRecordStore::FinalizationAction::kKeep);
}

BSONObj MultiIndexBlock::_constructStateObject() const {
    BSONObjBuilder builder;
    _buildUUID->appendToBuilder(&builder, "_id");
    builder.append("phase", IndexBuildPhase_serializer(_phase));

    if (_collectionUUID) {
        _collectionUUID->appendToBuilder(&builder, "collectionUUID");
    }

    // We can be interrupted by shutdown before inserting the first document from the collection
    // scan, in which case there is no _lastRecordIdInserted.
    if (_phase == IndexBuildPhaseEnum::kCollectionScan && _lastRecordIdInserted)
        builder.append("collectionScanPosition", _lastRecordIdInserted->repr());

    BSONArrayBuilder indexesArray(builder.subarrayStart("indexes"));
    for (const auto& index : _indexes) {
        BSONObjBuilder indexInfo(indexesArray.subobjStart());

        if (_phase == IndexBuildPhaseEnum::kCollectionScan ||
            _phase == IndexBuildPhaseEnum::kBulkLoad) {
            auto state = index.bulk->getSorterState();

            indexInfo.append("tempDir", state.tempDir);
            indexInfo.append("fileName", state.fileName);

            BSONArrayBuilder ranges(indexInfo.subarrayStart("ranges"));
            for (const auto& rangeInfo : state.ranges) {
                BSONObjBuilder range(ranges.subobjStart());

                range.append("startOffset", rangeInfo.startOffset);
                range.append("endOffset", rangeInfo.endOffset);
                range.appendNumber("checksum", static_cast<long long>(rangeInfo.checksum));

                range.done();
            }
            ranges.done();
        }

        auto indexBuildInterceptor = index.block->getEntry()->indexBuildInterceptor();
        indexInfo.append("sideWritesTable", indexBuildInterceptor->getSideWritesTableIdent());

        if (auto duplicateKeyTrackerTableIdent =
                indexBuildInterceptor->getDuplicateKeyTrackerTableIdent())
            indexInfo.append("duplicateKeyTrackerTable", *duplicateKeyTrackerTableIdent);

        if (auto skippedRecordTrackerTableIdent =
                indexBuildInterceptor->getSkippedRecordTracker()->getTableIdent())
            indexInfo.append("skippedRecordTrackerTable", *skippedRecordTrackerTableIdent);

        // TODO SERVER-49450: Consider only writing out the index name or key and getting the rest
        // of the spec from the durable catalog on startup.
        indexInfo.append("spec", index.block->getSpec());

        indexInfo.done();

        // Ensure the data we are referencing has been persisted to disk.
        index.bulk->persistDataForShutdown();
    }
    indexesArray.done();

    return builder.obj();
}
}  // namespace mongo
