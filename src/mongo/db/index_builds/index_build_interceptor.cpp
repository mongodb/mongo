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
#include "mongo/db/collection_crud/container_write.h"
#include "mongo/db/curop.h"
#include "mongo/db/index/index_access_method.h"
#include "mongo/db/index_builds/duplicate_key_tracker.h"
#include "mongo/db/index_builds/index_builds_common.h"
#include "mongo/db/multi_key_path_tracker.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"
#include "mongo/db/shard_role/shard_catalog/collection.h"
#include "mongo/db/shard_role/shard_catalog/index_catalog.h"
#include "mongo/db/shard_role/shard_catalog/index_descriptor.h"
#include "mongo/db/shard_role/transaction_resources.h"
#include "mongo/db/storage/recovery_unit.h"
#include "mongo/db/storage/storage_engine.h"
#include "mongo/db/storage/storage_parameters_gen.h"
#include "mongo/db/storage/write_unit_of_work.h"
#include "mongo/logv2/log.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

#include <cstdint>
#include <vector>

#include <boost/container/flat_set.hpp>
#include <boost/container/small_vector.hpp>
#include <boost/container/vector.hpp>
#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kIndex


namespace mongo {

IndexBuildInterceptor::IndexBuildInterceptor(OperationContext* opCtx,
                                             const IndexCatalogEntry* entry,
                                             const IndexBuildInfo& indexBuildInfo,
                                             bool resume,
                                             bool generateTableWrites)
    : _generateTableWrites(generateTableWrites),
      _sideWritesTracker([&]() {
          uassert(10709201, "sideWritesIdent is not provided", indexBuildInfo.sideWritesIdent);
          return SideWritesTracker{opCtx, *indexBuildInfo.sideWritesIdent, resume};
      }()),
      _skippedRecordTracker([&]() {
          uassert(10709202,
                  "skippedRecordsTrackerIdent is not provided",
                  indexBuildInfo.skippedRecordsTrackerIdent);
          return SkippedRecordTracker(
              opCtx, *indexBuildInfo.skippedRecordsTrackerIdent, /*tableExists=*/resume);
      }()),
      _skipNumAppliedCheck(true) {
    if (entry->descriptor()->unique()) {
        uassert(10709203,
                "constraintViolationsTrackerIdent is not provided",
                indexBuildInfo.constraintViolationsTrackerIdent);
        _duplicateKeyTracker = std::make_unique<DuplicateKeyTracker>(
            opCtx, entry, *indexBuildInfo.constraintViolationsTrackerIdent, /*tableExists=*/resume);
    }
    // TODO(SERVER-110289): Use utility function instead of checking fcvSnapshot.
    const auto fcvSnapshot = serverGlobalParams.featureCompatibility.acquireFCVSnapshot();
    auto isPrimaryDrivenIndexBuild = fcvSnapshot.isVersionInitialized() &&
        feature_flags::gFeatureFlagPrimaryDrivenIndexBuilds.isEnabled(
            VersionContext::getDecoration(opCtx), fcvSnapshot);
    if (isPrimaryDrivenIndexBuild) {
        uassert(11411100, "sorterIdent is not provided", indexBuildInfo.sorterIdent);
        uassert(11411101, "Resumability with the sorter is not supported", !resume);
        auto storageEngine = opCtx->getServiceContext()->getStorageEngine();
        _sorterTable = storageEngine->makeTemporaryRecordStore(
            opCtx, *indexBuildInfo.sorterIdent, KeyFormat::Long);
    }
}

void IndexBuildInterceptor::keepTemporaryTables() {
    _sideWritesTracker.keepTemporaryTable();
    if (_duplicateKeyTracker) {
        _duplicateKeyTracker->keepTemporaryTable();
    }
    _skippedRecordTracker.keepTemporaryTable();
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
    if (auto duplicate = _duplicateKeyTracker->checkConstraints(opCtx, indexCatalogEntry)) {
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

Status IndexBuildInterceptor::drainWritesIntoIndex(OperationContext* opCtx,
                                                   const CollectionPtr& coll,
                                                   const IndexCatalogEntry* indexCatalogEntry,
                                                   const InsertDeleteOptions& options,
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

    return _sideWritesTracker.drainWritesIntoIndex(
        opCtx, coll, indexCatalogEntry, options, onDuplicateKeyFn, drainYieldPolicy);
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
    stdx::unique_lock<stdx::mutex> lk(_multikeyPathMutex);
    return _multikeyPaths;
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
    if (op == Op::kInsert && isMultikey) {
        // SERVER-39705: It's worth noting that a document may not generate any keys, but be
        // described as being multikey. This step must be done to maintain parity with `validate`s
        // expectations.
        stdx::unique_lock<stdx::mutex> lk(_multikeyPathMutex);
        if (_multikeyPaths) {
            MultikeyPathTracker::mergeMultikeyPaths(&_multikeyPaths.value(), multikeyPaths);
        } else {
            // `mergeMultikeyPaths` is sensitive to the two inputs having the same multikey
            // "shape". Initialize `_multikeyPaths` with the right shape from the first result.
            _multikeyPaths = multikeyPaths;
        }
    }

    if (*numKeysOut == 0) {
        return Status::OK();
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
        toInsert.emplace_back(BSON("op" << (op == Op::kInsert ? "i" : "d") << "key" << binData));
    }

    if (op == Op::kInsert) {
        // Wildcard indexes write multikey path information, typically part of the catalog document,
        // to the index itself. Multikey information is never deleted, so we only need to add this
        // data on the insert path.
        for (const auto& keyString : multikeyMetadataKeys) {
            builder.reset();
            keyString.serialize(builder);
            BSONBinData binData(builder.buf(), builder.len(), BinDataGeneral);
            toInsert.emplace_back(BSON("op" << "i"
                                            << "key" << binData));
        }
    }

    return _sideWritesTracker.bufferSideWrite(opCtx, coll, indexCatalogEntry, std::move(toInsert));
}

Status IndexBuildInterceptor::retrySkippedRecords(OperationContext* opCtx,
                                                  const CollectionPtr& collection,
                                                  const IndexCatalogEntry* indexCatalogEntry,
                                                  RetrySkippedRecordMode mode) {
    return _skippedRecordTracker.retrySkippedRecords(opCtx, collection, indexCatalogEntry, mode);
}

}  // namespace mongo
