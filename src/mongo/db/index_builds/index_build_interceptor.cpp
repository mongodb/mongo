// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/index_builds/index_build_interceptor.h"

#include "mongo/base/error_codes.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/bson/bsontypes_util.h"
#include "mongo/bson/timestamp.h"
#include "mongo/bson/util/builder.h"
#include "mongo/db/client.h"
#include "mongo/db/curop.h"
#include "mongo/db/index/index_access_method.h"
#include "mongo/db/index/multikey_paths.h"
#include "mongo/db/index_builds/duplicate_key_tracker.h"
#include "mongo/db/index_builds/index_builds_common.h"
#include "mongo/db/index_builds/primary_driven/enabled.h"
#include "mongo/db/multi_key_path_tracker.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"
#include "mongo/db/shard_role/shard_catalog/collection.h"
#include "mongo/db/shard_role/shard_catalog/index_catalog.h"
#include "mongo/db/shard_role/shard_catalog/index_descriptor.h"
#include "mongo/db/shard_role/transaction_resources.h"
#include "mongo/db/storage/recovery_unit.h"
#include "mongo/db/storage/storage_engine.h"
#include "mongo/db/storage/write_unit_of_work.h"
#include "mongo/logv2/log.h"
#include "mongo/otel/metrics/metric_names.h"
#include "mongo/otel/metrics/metric_unit.h"
#include "mongo/otel/metrics/metrics_counter.h"
#include "mongo/otel/metrics/metrics_service.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

#include <cstdint>
#include <vector>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kIndex


namespace mongo {

namespace {
auto& sideWritesInsertedCounter = otel::metrics::MetricsService::instance().createInt64Counter(
    otel::metrics::MetricNames::kIndexBuildSideWritesInserted,
    "Total number of side write inserts written",
    otel::metrics::MetricUnit::kOperations);
auto& sideWritesDeletedCounter = otel::metrics::MetricsService::instance().createInt64Counter(
    otel::metrics::MetricNames::kIndexBuildSideWritesDeleted,
    "Total number of side write deletes written",
    otel::metrics::MetricUnit::kOperations);
}  // namespace

IndexBuildInterceptor::IndexBuildInterceptor(OperationContext* opCtx,
                                             const IndexBuildInfo& indexBuildInfo,
                                             LazyRecordStore::CreateMode createMode,
                                             bool unique)
    : _sideWritesTracker([&]() {
          uassert(10709201, "sideWritesIdent is not provided", indexBuildInfo.sideWritesIdent);
          return SideWritesTracker{opCtx, *indexBuildInfo.sideWritesIdent, createMode};
      }()),
      _skippedRecordTracker([&]() {
          uassert(
              10709202, "skippedRecordsIdent is not provided", indexBuildInfo.skippedRecordsIdent);
          return SkippedRecordTracker(opCtx, *indexBuildInfo.skippedRecordsIdent, createMode);
      }()),
      _skipNumAppliedCheck(createMode == LazyRecordStore::CreateMode::openExisting) {
    if (unique) {
        uassert(10709203,
                "constraintViolationsIdent is not provided",
                indexBuildInfo.constraintViolationsIdent);
        _duplicateKeyTracker = std::make_unique<DuplicateKeyTracker>(
            opCtx, *indexBuildInfo.constraintViolationsIdent, createMode);
    }
    auto isPrimaryDrivenIndexBuild = index_builds::primary_driven::enabled(
        opCtx, serverGlobalParams.featureCompatibility.acquireFCVSnapshot());
    if (isPrimaryDrivenIndexBuild) {
        uassert(11411100, "sorterIdent is not provided", indexBuildInfo.sorterIdent);
        uassert(11411101,
                "Primary-driven index builds cannot use deferred table creation",
                createMode != LazyRecordStore::CreateMode::deferred);
        _sorterTable.emplace(opCtx, *indexBuildInfo.sorterIdent, createMode);
    }
}

Status IndexBuildInterceptor::recordDuplicateKey(OperationContext* opCtx,
                                                 const CollectionPtr& coll,
                                                 const IndexCatalogEntry* indexCatalogEntry,
                                                 const key_string::View& key) const {
    invariant(indexCatalogEntry->descriptor()->unique());
    return _duplicateKeyTracker->recordKey(opCtx, coll, indexCatalogEntry, key);
}

Status IndexBuildInterceptor::checkDuplicateKeyConstraints(
    OperationContext* opCtx,
    const CollectionPtr& coll,
    const IndexCatalogEntry* indexCatalogEntry) const {
    if (!_duplicateKeyTracker) {
        return Status::OK();
    }
    if (auto duplicate = _duplicateKeyTracker->checkConstraints(opCtx, coll, indexCatalogEntry)) {
        return buildDupKeyErrorStatus(duplicate->key,
                                      coll->ns(),
                                      indexCatalogEntry->descriptor()->indexName(),
                                      indexCatalogEntry->descriptor()->keyPattern(),
                                      indexCatalogEntry->descriptor()->collation(),
                                      std::move(duplicate->foundValue),
                                      std::move(duplicate->id));
    }
    return Status::OK();
}

Status IndexBuildInterceptor::drainWritesIntoIndex(
    OperationContext* opCtx,
    const CollectionPtr& coll,
    const IndexCatalogEntry* indexCatalogEntry,
    const InsertDeleteOptions& options,
    const OnMultikeyPathsRecoveredFn& onMultikeyPathsRecovered,
    TrackDuplicates trackDuplicates,
    DrainYieldPolicy drainYieldPolicy) {
    // Sorted index types may choose to disallow duplicates (enforcing an unique index).
    // Only sorted indexes will use this lambda passed through the IndexAccessMethod interface.
    auto onDuplicateKeyFn = [=, this](const CollectionPtr& coll,
                                      const key_string::View& duplicateKey) {
        return trackDuplicates == TrackDuplicates::kTrack
            ? recordDuplicateKey(opCtx, coll, indexCatalogEntry, duplicateKey)
            : Status::OK();
    };

    // Report any multikey state the batch recovered from its records while its transaction is
    // still open, so the caller can persist it alongside the keys it applied.
    SideWritesTracker::OnBatchAppliedFn onBatchApplied;
    if (onMultikeyPathsRecovered) {
        onBatchApplied = [&](OperationContext* opCtx) -> Status {
            boost::optional<MultikeyPaths> recovered;
            {
                std::unique_lock<std::mutex> lk(_multikeyPathMutex);
                if (!std::exchange(_multikeyPathsRecovered, false)) {
                    return Status::OK();
                }
                recovered = _multikeyPaths;
            }
            auto status = onMultikeyPathsRecovered(opCtx, recovered.value_or(MultikeyPaths{}));
            if (!status.isOK()) {
                // Leave the state flagged so a later batch reports it again.
                std::unique_lock<std::mutex> lk(_multikeyPathMutex);
                _multikeyPathsRecovered = true;
            }
            return status;
        };
    }

    return _sideWritesTracker.drainWritesIntoIndex(opCtx,
                                                   coll,
                                                   indexCatalogEntry,
                                                   options,
                                                   onDuplicateKeyFn,
                                                   onBatchApplied,
                                                   drainYieldPolicy);
}

bool IndexBuildInterceptor::areAllWritesApplied(OperationContext* opCtx) const {
    return _checkAllWritesApplied(opCtx, false);
}

void IndexBuildInterceptor::invariantAllWritesApplied(OperationContext* opCtx) const {
    _checkAllWritesApplied(opCtx, true);
}

bool IndexBuildInterceptor::_checkAllWritesApplied(OperationContext* opCtx, bool fatal) const {
    if (!_sideWritesTracker.checkAllWritesApplied(opCtx, fatal)) {
        return false;
    }

    if (_skipNumAppliedCheck) {
        return true;
    }

    auto writesRecorded = _sideWritesTracker.count();
    auto writesApplied = _sideWritesTracker.numApplied();
    if (writesRecorded != writesApplied) {
        dassert(writesRecorded == writesRecorded,
                (str::stream() << "The number of side writes recorded does not match the number "
                                  "applied, despite the table appearing empty. Writes recorded: "
                               << writesRecorded << ", applied: " << writesApplied));
        LOGV2_WARNING(20692,
                      "The number of side writes recorded does not match the number applied, "
                      "despite the table appearing empty",
                      "recorded"_attr = writesRecorded,
                      "applied"_attr = writesApplied);
    }

    return true;
}

boost::optional<MultikeyPaths> IndexBuildInterceptor::getMultikeyPaths() const {
    std::unique_lock<std::mutex> lk(_multikeyPathMutex);
    return _multikeyPaths;
}

void IndexBuildInterceptor::_mergeMultikeyPaths(const MultikeyPaths& multikeyPaths) {
    std::unique_lock<std::mutex> lk(_multikeyPathMutex);
    if (_multikeyPaths) {
        MultikeyPathTracker::mergeMultikeyPaths(&_multikeyPaths.value(), multikeyPaths);
    } else {
        // `mergeMultikeyPaths` is sensitive to the two inputs having the same multikey "shape".
        // Initialize `_multikeyPaths` with the right shape from the first result.
        _multikeyPaths = multikeyPaths;
    }
}

void IndexBuildInterceptor::recordDrainedMultikeyPaths(const MultikeyPaths& multikeyPaths) {
    _mergeMultikeyPaths(multikeyPaths);
    std::unique_lock<std::mutex> lk(_multikeyPathMutex);
    _multikeyPathsRecovered = true;
}

Status IndexBuildInterceptor::sideWrite(OperationContext* opCtx,
                                        const CollectionPtr& coll,
                                        const IndexCatalogEntry* indexCatalogEntry,
                                        const KeyStringSet& keys,
                                        const KeyStringSet& multikeyMetadataKeys,
                                        const MultikeyPaths& multikeyPaths,
                                        Op op,
                                        int64_t* const numKeysOut) {
    invariant(shard_role_details::getLocker(opCtx)->inAWriteUnitOfWork());

    // Maintain parity with IndexAccessMethods handling of key counting. Only include
    // `multikeyMetadataKeys` when inserting.
    *numKeysOut = keys.size() + (op == Op::kInsert ? multikeyMetadataKeys.size() : 0);

    // Maintain parity with IndexAccessMethod's handling of whether keys could change the multikey
    // state on the index.
    bool isMultikey = indexCatalogEntry->accessMethod()->asSortedData()->shouldMarkIndexAsMultikey(
        keys.size(), multikeyMetadataKeys, multikeyPaths);

    // No need to take the multikeyPaths mutex if this would not change any multikey state.
    bool isMultikeyInsert = op == Op::kInsert && isMultikey;
    if (isMultikeyInsert) {
        // SERVER-39705: It's worth noting that a document may not generate any keys, but be
        // described as being multikey. This step must be done to maintain parity with `validate`s
        // expectations.
        _mergeMultikeyPaths(multikeyPaths);
    }

    if (*numKeysOut == 0) {
        return Status::OK();
    }

    shard_role_details::getRecoveryUnit(opCtx)->onCommit(
        [written = *numKeysOut, op](OperationContext*, boost::optional<Timestamp>) {
            if (op == Op::kInsert) {
                sideWritesInsertedCounter.add(written);
            } else {
                sideWritesDeletedCounter.add(written);
            }
        });

    // The multikey state this document implies, to be recorded on the first of its key records.
    // Each record holds a single key, so the drain cannot re-derive from them that a document
    // generated several.
    BSONObj multikeyFields;
    if (isMultikeyInsert) {
        BSONObjBuilder multikeyBuilder;
        multikeyBuilder.append(kSideWriteMultikeyFieldName, true);
        if (!multikeyPaths.empty()) {
            // Empty paths mean the index does not support path-level multikey tracking; the flag
            // above is then the whole of the state. Only serialize when there is more to say, since
            // `multikey_paths::serialize` expects one entry per key pattern field.
            BSONObjBuilder pathsBuilder(
                multikeyBuilder.subobjStart(kSideWriteMultikeyPathsFieldName));
            multikey_paths::serialize(
                indexCatalogEntry->descriptor()->keyPattern(), multikeyPaths, pathsBuilder);
            pathsBuilder.doneFast();
        }
        multikeyFields = multikeyBuilder.obj();
    }

    // Reuse the same builder to avoid an allocation per key.
    BufBuilder builder;
    std::vector<BSONObj> toInsert;
    for (const auto& keyString : keys) {
        // Documents inserted into this table must be consumed in insert-order.
        // Additionally, these writes should be timestamped with the same timestamps that the
        // other writes making up this operation are given. When index builds can cope with
        // replication rollbacks, side table writes associated with a CUD operation should
        // remain/rollback along with the corresponding oplog entry.

        // Serialize the key_string::Value into a binary format for storage. Since the
        // key_string::Value also contains TypeBits information, it is not sufficient to just read
        // from getBuffer().
        builder.reset();
        keyString.serialize(builder);
        BSONBinData binData(builder.buf(), builder.len(), BinDataGeneral);

        BSONObjBuilder recordBuilder;
        recordBuilder.append("op", op == Op::kInsert ? "i" : "d");
        recordBuilder.append("key", binData);
        if (toInsert.empty() && !multikeyFields.isEmpty()) {
            // Only the first record carries the multikey state. Every record is applied, and
            // applying it more than once is idempotent, so one is enough.
            recordBuilder.appendElements(multikeyFields);
        }
        toInsert.emplace_back(recordBuilder.obj());
    }

    if (op == Op::kInsert) {
        // Wildcard indexes write multikey path information, typically part of the catalog document,
        // to the index itself. Multikey information is never deleted, so we only need to add this
        // data on the insert path.
        for (const auto& keyString : multikeyMetadataKeys) {
            builder.reset();
            keyString.serialize(builder);
            BSONBinData binData(builder.buf(), builder.len(), BinDataGeneral);

            BSONObjBuilder recordBuilder;
            recordBuilder.append("op", "i");
            recordBuilder.append("key", binData);
            if (toInsert.empty() && !multikeyFields.isEmpty()) {
                // A wildcard index reports multikey through its metadata keys alone, so a document
                // can produce these records and no key records at all. The state then rides on the
                // first metadata record instead.
                recordBuilder.appendElements(multikeyFields);
            }
            toInsert.emplace_back(recordBuilder.obj());
        }
    }

    return _sideWritesTracker.bufferSideWrite(opCtx, coll, indexCatalogEntry, std::move(toInsert));
}

Status IndexBuildInterceptor::retrySkippedRecords(
    OperationContext* opCtx,
    const CollectionPtr& collection,
    const IndexCatalogEntry* indexCatalogEntry,
    const OnMultikeyPathsRecoveredFn& onMultikeyPathsRecovered,
    RetrySkippedRecordMode mode) {
    return _skippedRecordTracker.retrySkippedRecords(
        opCtx, collection, indexCatalogEntry, onMultikeyPathsRecovered, mode);
}

namespace index_builds {
namespace {
const auto _pendingInterceptors = ServiceContext::declareDecoration<PendingInterceptors>();
}  // namespace

PendingInterceptors& getPendingInterceptors(ServiceContext* svcCtx) {
    return _pendingInterceptors(svcCtx);
}

std::shared_ptr<IndexBuildInterceptor> PendingInterceptors::find(
    std::string_view indexIdent) const {
    std::lock_guard lk(_mutex);
    auto it = _interceptors.find(indexIdent);
    return it == _interceptors.end() ? nullptr : it->second;
}

bool PendingInterceptors::contains(std::string_view indexIdent) const {
    std::lock_guard lk(_mutex);
    return _interceptors.contains(indexIdent);
}

void PendingInterceptors::add(std::string_view indexIdent,
                              std::shared_ptr<IndexBuildInterceptor> interceptor) {
    std::lock_guard lk(_mutex);
    _interceptors[std::string{indexIdent}] = std::move(interceptor);
}

void PendingInterceptors::erase(std::string_view indexIdent) {
    std::lock_guard lk(_mutex);
    _interceptors.erase(indexIdent);
}

void PendingInterceptors::clear() {
    std::lock_guard lk(_mutex);
    _interceptors.clear();
}

}  // namespace index_builds

}  // namespace mongo
