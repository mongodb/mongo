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

#include "mongo/db/index/index_access_method.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/collection_crud/container_write.h"
#include "mongo/db/commands/server_status.h"
#include "mongo/db/curop.h"
#include "mongo/db/exec/matcher/matcher.h"
#include "mongo/db/index/2d_access_method.h"
#include "mongo/db/index/btree_access_method.h"
#include "mongo/db/index/fts_access_method.h"
#include "mongo/db/index/hash_access_method.h"
#include "mongo/db/index/preallocated_container_pool.h"
#include "mongo/db/index/s2_access_method.h"
#include "mongo/db/index/s2_bucket_access_method.h"
#include "mongo/db/index/wildcard_access_method.h"
#include "mongo/db/index_builds/index_build_interceptor.h"
#include "mongo/db/index_names.h"
#include "mongo/db/local_catalog/collection.h"
#include "mongo/db/local_catalog/collection_options.h"
#include "mongo/db/local_catalog/index_catalog.h"
#include "mongo/db/local_catalog/index_descriptor.h"
#include "mongo/db/local_catalog/lock_manager/exception_util.h"
#include "mongo/db/local_catalog/shard_role_api/transaction_resources.h"
#include "mongo/db/matcher/expression.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"
#include "mongo/db/sorter/sorter.h"
#include "mongo/db/sorter/sorter_template_defs.h"
#include "mongo/db/storage/index_entry_comparison.h"
#include "mongo/db/storage/kv/kv_engine.h"
#include "mongo/db/storage/recovery_unit.h"
#include "mongo/db/storage/storage_engine.h"
#include "mongo/db/storage/storage_options.h"
#include "mongo/db/validate/validate_results.h"
#include "mongo/logv2/log.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/bufreader.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/namespace_string_util.h"
#include "mongo/util/stacktrace.h"

#include <algorithm>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include <boost/container/container_fwd.hpp>
#include <boost/cstdint.hpp>
#include <boost/optional/optional.hpp>
#include <fmt/format.h>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kCommand

namespace mongo {

using std::pair;

using IndexVersion = IndexDescriptor::IndexVersion;

MONGO_FAIL_POINT_DEFINE(hangIndexBuildDuringBulkLoadPhase);
MONGO_FAIL_POINT_DEFINE(hangIndexBuildDuringBulkLoadPhaseSecond);

/**
 * Static factory method that constructs and returns an appropriate IndexAccessMethod depending on
 * the type of the index.
 */
std::unique_ptr<IndexAccessMethod> IndexAccessMethod::make(
    OperationContext* opCtx,
    RecoveryUnit& ru,
    const NamespaceString& nss,
    const CollectionOptions& collectionOptions,
    IndexCatalogEntry* entry,
    StringData ident) {

    auto engine = opCtx->getServiceContext()->getStorageEngine()->getEngine();
    auto desc = entry->descriptor();
    auto keyFormat =
        collectionOptions.clusteredIndex.has_value() ? KeyFormat::String : KeyFormat::Long;
    auto makeSDI = [&] {
        return engine->getSortedDataInterface(
            opCtx, ru, nss, *collectionOptions.uuid, ident, desc->toIndexConfig(), keyFormat);
    };
    const std::string& type = desc->getAccessMethodName();

    if ("" == type)
        return std::make_unique<BtreeAccessMethod>(entry, makeSDI());
    else if (IndexNames::HASHED == type)
        return std::make_unique<HashAccessMethod>(entry, makeSDI());
    else if (IndexNames::GEO_2DSPHERE == type)
        return std::make_unique<S2AccessMethod>(entry, makeSDI());
    else if (IndexNames::GEO_2DSPHERE_BUCKET == type)
        return std::make_unique<S2BucketAccessMethod>(entry, makeSDI());
    else if (IndexNames::TEXT == type)
        return std::make_unique<FTSAccessMethod>(entry, makeSDI());
    else if (IndexNames::GEO_2D == type)
        return std::make_unique<TwoDAccessMethod>(entry, makeSDI());
    else if (IndexNames::WILDCARD == type)
        return std::make_unique<WildcardAccessMethod>(entry, makeSDI());
    LOGV2(20688, "Can't find index for keyPattern", "keyPattern"_attr = desc->keyPattern());
    fassertFailed(31021);
}

namespace {

/**
 * Metrics for index bulk builder operations. Intended to support index build diagnostics
 * during the following scenarios:
 * - createIndex commands;
 * - collection cloning during initial sync; and
 * - resuming index builds at startup.
 *
 * Also includes statistics for disk usage (by the external sorter) for index builds that
 * do not fit in memory.
 */
class IndexBulkBuilderSSS : public ServerStatusSection {
public:
    using ServerStatusSection::ServerStatusSection;

    bool includeByDefault() const final {
        return true;
    }

    BSONObj generateSection(OperationContext* opCtx, const BSONElement& configElement) const final {
        BSONObjBuilder builder;
        builder.append("count", count.loadRelaxed());
        builder.append("resumed", resumed.loadRelaxed());
        builder.append("filesOpenedForExternalSort", sorterFileStats.opened.loadRelaxed());
        builder.append("filesClosedForExternalSort", sorterFileStats.closed.loadRelaxed());
        builder.append("spilledRanges", sorterTracker.spilledRanges.loadRelaxed());
        builder.append("bytesSpilledUncompressed",
                       sorterTracker.bytesSpilledUncompressed.loadRelaxed());
        builder.append("bytesSpilled", sorterTracker.bytesSpilled.loadRelaxed());
        builder.append("numSorted", sorterTracker.numSorted.loadRelaxed());
        builder.append("bytesSorted", sorterTracker.bytesSorted.loadRelaxed());
        builder.append("memUsage", sorterTracker.memUsage.loadRelaxed());
        return builder.obj();
    }

    // Number of instances of the bulk builder created.
    AtomicWord<long long> count;

    // Number of times the bulk builder was created for a resumable index build.
    // This value should not exceed 'count'.
    AtomicWord<long long> resumed;

    // Sorter statistics that are aggregate of all sorters.
    SorterTracker sorterTracker;

    // Number of times the external sorter opened/closed a file handle to spill data to disk.
    // This pair of counters in aggregate indicate the number of open file handles used by
    // the external sorter and may be useful in diagnosing situations where the process is
    // close to exhausting this finite resource.
    SorterFileStats sorterFileStats = {&sorterTracker};
};

auto& indexBulkBuilderSSS =
    *ServerStatusSectionBuilder<IndexBulkBuilderSSS>("indexBulkBuilder").forShard();

/**
 * Returns true if at least one prefix of any of the indexed fields causes the index to be
 * multikey, and returns false otherwise. This function returns false if the 'multikeyPaths'
 * vector is empty.
 */
bool isMultikeyFromPaths(const MultikeyPaths& multikeyPaths) {
    return std::any_of(multikeyPaths.cbegin(),
                       multikeyPaths.cend(),
                       [](const MultikeyComponents& components) { return !components.empty(); });
}

SortOptions makeSortOptions(size_t maxMemoryUsageBytes,
                            const DatabaseName& dbName,
                            SorterFileStats* stats) {
    return SortOptions()
        .TempDir(storageGlobalParams.dbpath + "/_tmp")
        .MaxMemoryUsageBytes(maxMemoryUsageBytes)
        .UseMemoryPool(true)
        .FileStats(stats)
        .Tracker(&indexBulkBuilderSSS.sorterTracker)
        .DBName(dbName);
}

MultikeyPaths createMultikeyPaths(const std::vector<MultikeyPath>& multikeyPathsVec) {
    MultikeyPaths multikeyPaths;
    for (const auto& multikeyPath : multikeyPathsVec) {
        multikeyPaths.emplace_back(boost::container::ordered_unique_range_t(),
                                   multikeyPath.getMultikeyComponents().begin(),
                                   multikeyPath.getMultikeyComponents().end());
    }

    return multikeyPaths;
}

}  // namespace

struct BtreeExternalSortComparison {
    int operator()(const key_string::Value& l, const key_string::Value& r) const {
        return l.compare(r);
    }
};

SortedDataIndexAccessMethod::SortedDataIndexAccessMethod(const IndexCatalogEntry* btreeState,
                                                         std::unique_ptr<SortedDataInterface> btree)
    : _newInterface(std::move(btree)) {
    MONGO_verify(IndexDescriptor::isIndexVersionSupported(btreeState->descriptor()->version()));
}

Status SortedDataIndexAccessMethod::insert(OperationContext* opCtx,
                                           SharedBufferFragmentBuilder& pooledBuilder,
                                           const CollectionPtr& coll,
                                           const IndexCatalogEntry* entry,
                                           const std::vector<BsonRecord>& bsonRecords,
                                           const InsertDeleteOptions& options,
                                           int64_t* numInserted) {
    for (const auto& bsonRecord : bsonRecords) {
        invariant(bsonRecord.id != RecordId());

        if (!bsonRecord.ts.isNull()) {
            Status status = shard_role_details::getRecoveryUnit(opCtx)->setTimestamp(bsonRecord.ts);
            if (!status.isOK())
                return status;
        }

        auto& containerPool = PreallocatedContainerPool::get(opCtx);
        auto keys = containerPool.keys();
        auto multikeyMetadataKeys = containerPool.multikeyMetadataKeys();
        auto multikeyPaths = containerPool.multikeyPaths();

        getKeys(opCtx,
                coll,
                entry,
                pooledBuilder,
                *bsonRecord.docPtr,
                options.getKeysMode,
                GetKeysContext::kAddingKeys,
                keys.get(),
                multikeyMetadataKeys.get(),
                multikeyPaths.get(),
                bsonRecord.id);

        Status status = _indexKeysOrWriteToSideTable(opCtx,
                                                     coll,
                                                     entry,
                                                     *keys,
                                                     *multikeyMetadataKeys,
                                                     *multikeyPaths,
                                                     *bsonRecord.docPtr,
                                                     options,
                                                     numInserted);
        if (!status.isOK()) {
            return status;
        }
    }

    return Status::OK();
}

void SortedDataIndexAccessMethod::remove(OperationContext* opCtx,
                                         SharedBufferFragmentBuilder& pooledBuilder,
                                         const CollectionPtr& coll,
                                         const IndexCatalogEntry* entry,
                                         const BSONObj& obj,
                                         const RecordId& loc,
                                         bool logIfError,
                                         const InsertDeleteOptions& options,
                                         int64_t* numDeleted,
                                         CheckRecordId checkRecordId) {
    auto& containerPool = PreallocatedContainerPool::get(opCtx);

    // There's no need to compute the prefixes of the indexed fields that cause the index to be
    // multikey when removing a document since the index metadata isn't updated when keys are
    // deleted.
    auto keys = containerPool.keys();
    getKeys(opCtx,
            coll,
            entry,
            pooledBuilder,
            obj,
            InsertDeleteOptions::ConstraintEnforcementMode::kRelaxConstraintsUnfiltered,
            GetKeysContext::kRemovingKeys,
            keys.get(),
            nullptr,
            nullptr,
            loc);

    _unindexKeysOrWriteToSideTable(
        opCtx, coll->ns(), entry, *keys, obj, logIfError, numDeleted, options, checkRecordId);
}

Status SortedDataIndexAccessMethod::update(OperationContext* opCtx,
                                           RecoveryUnit& ru,
                                           SharedBufferFragmentBuilder& pooledBufferBuilder,
                                           const BSONObj& oldDoc,
                                           const BSONObj& newDoc,
                                           const RecordId& loc,
                                           const CollectionPtr& coll,
                                           const IndexCatalogEntry* entry,
                                           const InsertDeleteOptions& options,
                                           int64_t* numInserted,
                                           int64_t* numDeleted) {
    UpdateTicket updateTicket;
    prepareUpdate(opCtx, coll, entry, oldDoc, newDoc, loc, options, &updateTicket);

    auto status = Status::OK();
    if (entry->isHybridBuilding() || !entry->isReady()) {
        bool logIfError = false;
        _unindexKeysOrWriteToSideTable(opCtx,
                                       coll->ns(),
                                       entry,
                                       updateTicket.removed,
                                       oldDoc,
                                       logIfError,
                                       numDeleted,
                                       options,
                                       CheckRecordId::Off);
        return _indexKeysOrWriteToSideTable(opCtx,
                                            coll,
                                            entry,
                                            updateTicket.added,
                                            updateTicket.newMultikeyMetadataKeys,
                                            updateTicket.newMultikeyPaths,
                                            newDoc,
                                            options,
                                            numInserted);
    } else {
        return doUpdate(opCtx, ru, coll, entry, updateTicket, numInserted, numDeleted);
    }
}

Status SortedDataIndexAccessMethod::insertKeysAndUpdateMultikeyPaths(
    OperationContext* opCtx,
    RecoveryUnit& ru,
    const CollectionPtr& coll,
    const IndexCatalogEntry* entry,
    const KeyStringSet& keys,
    const KeyStringSet& multikeyMetadataKeys,
    const MultikeyPaths& multikeyPaths,
    const InsertDeleteOptions& options,
    KeyHandlerFn&& onDuplicateKey,
    int64_t* numInserted,
    IncludeDuplicateRecordId includeDuplicateRecordId) {
    // Insert the specified data keys into the index.
    auto status = insertKeys(opCtx,
                             ru,
                             coll,
                             entry,
                             keys,
                             options,
                             std::move(onDuplicateKey),
                             numInserted,
                             includeDuplicateRecordId);
    if (!status.isOK()) {
        return status;
    }
    // If these keys should cause the index to become multikey, pass them into the catalog.
    if (shouldMarkIndexAsMultikey(keys.size(), multikeyMetadataKeys, multikeyPaths)) {
        entry->setMultikey(opCtx, coll, multikeyMetadataKeys, multikeyPaths);
    }
    // If we have some multikey metadata keys, they should have been added while marking the index
    // as multikey in the catalog. Add them to the count of keys inserted for completeness.
    if (numInserted && !multikeyMetadataKeys.empty()) {
        *numInserted += multikeyMetadataKeys.size();
    }
    return Status::OK();
}

Status SortedDataIndexAccessMethod::insertKeys(OperationContext* opCtx,
                                               RecoveryUnit& ru,
                                               const CollectionPtr& coll,
                                               const IndexCatalogEntry* entry,
                                               const KeyStringSet& keys,
                                               const InsertDeleteOptions& options,
                                               KeyHandlerFn&& onDuplicateKey,
                                               int64_t* numInserted,
                                               IncludeDuplicateRecordId includeDuplicateRecordId) {
    // Initialize the 'numInserted' out-parameter to zero in case the caller did not already do so.
    if (numInserted) {
        *numInserted = 0;
    }
    bool unique = entry->descriptor()->unique();
    bool prepareUnique = entry->descriptor()->prepareUnique();
    bool dupsAllowed;
    if (!entry->descriptor()->isIdIndex() && !opCtx->isEnforcingConstraints() &&
        coll->isIndexReady(entry->descriptor()->indexName())) {
        // Oplog application should avoid checking for duplicates on unique indexes except when:
        // 1. Building an index. We have to use the duplicate key error to record possible
        // conflicts.
        // 2. Inserting into the '_id' index. We never allow duplicates in the '_id' index.
        //
        // Additionally, unique indexes conflict checking can cause out-of-order updates in
        // wiredtiger. See SERVER-59831.
        dupsAllowed = true;
    } else if (prepareUnique) {
        // Before the index build commits, duplicate keys are allowed to exist with the
        // 'prepareUnique' option. After that, duplicates are not allowed.
        dupsAllowed = !coll->isIndexReady(entry->descriptor()->indexName());
    } else {
        dupsAllowed = !unique;
    }
    // Add all new keys into the index. The RecordId for each is already encoded in the KeyString.
    for (const auto& keyString : keys) {
        auto result =
            _newInterface->insert(opCtx, ru, keyString, dupsAllowed, includeDuplicateRecordId);

        // When duplicates are encountered and allowed, retry with dupsAllowed. Call
        // onDuplicateKey() with the inserted duplicate key.
        if (std::holds_alternative<SortedDataInterface::DuplicateKey>(result) &&
            options.dupsAllowed && !prepareUnique) {
            invariant(unique);

            result = _newInterface->insert(
                opCtx, ru, keyString, true /* dupsAllowed */, includeDuplicateRecordId);
            if (auto status = std::get_if<Status>(&result)) {
                if (status->isOK() && onDuplicateKey) {
                    result = onDuplicateKey(keyString);
                }
            }
        }
        if (auto duplicate = std::get_if<SortedDataInterface::DuplicateKey>(&result)) {
            return buildDupKeyErrorStatus(duplicate->key,
                                          coll->ns(),
                                          entry->descriptor()->indexName(),
                                          entry->descriptor()->keyPattern(),
                                          entry->descriptor()->collation(),
                                          std::move(duplicate->foundValue),
                                          std::move(duplicate->id));
        } else if (auto& status = std::get<Status>(result); !status.isOK()) {
            return status;
        }
    }
    if (numInserted) {
        *numInserted = keys.size();
    }
    return Status::OK();
}

void SortedDataIndexAccessMethod::removeOneKey(OperationContext* opCtx,
                                               RecoveryUnit& ru,
                                               const IndexCatalogEntry* entry,
                                               const key_string::Value& keyString,
                                               bool dupsAllowed) const {

    try {
        _newInterface->unindex(opCtx, ru, keyString, dupsAllowed);
    } catch (AssertionException& e) {
        if (e.code() == ErrorCodes::DataCorruptionDetected) {
            // DataCorruptionDetected errors are expected to have logged an error and added an entry
            // to the health log with the stack trace at the location where the error was initially
            // thrown. No need to do so again.
            throw;
        }

        NamespaceString ns = entry->getNSSFromCatalog(opCtx);
        LOGV2(20683,
              "Assertion failure: _unindex failed",
              "error"_attr = redact(e),
              "keyString"_attr = keyString,
              logAttrs(ns),
              "indexName"_attr = entry->descriptor()->indexName());
        printStackTrace();
    }
}

std::unique_ptr<SortedDataInterface::Cursor> SortedDataIndexAccessMethod::newCursor(
    OperationContext* opCtx, RecoveryUnit& ru, bool isForward) const {
    return _newInterface->newCursor(opCtx, ru, isForward);
}

Status SortedDataIndexAccessMethod::removeKeys(OperationContext* opCtx,
                                               RecoveryUnit& ru,
                                               const IndexCatalogEntry* entry,
                                               const KeyStringSet& keys,
                                               const InsertDeleteOptions& options,
                                               int64_t* numDeleted) const {

    for (const auto& key : keys) {
        removeOneKey(opCtx, ru, entry, key, options.dupsAllowed);
    }

    *numDeleted = keys.size();
    return Status::OK();
}

Status SortedDataIndexAccessMethod::initializeAsEmpty() {
    return _newInterface->initAsEmpty();
}

RecordId SortedDataIndexAccessMethod::findSingle(OperationContext* opCtx,
                                                 RecoveryUnit& ru,
                                                 const CollectionPtr& collection,
                                                 const IndexCatalogEntry* entry,
                                                 const BSONObj& requestedKey) const {
    boost::optional<RecordId> loc = [&]() {
        // Generate the key for this index.
        if (entry->getCollator()) {
            // For performance, call get keys only if there is a non-simple collation.
            SharedBufferFragmentBuilder pooledBuilder(
                key_string::HeapBuilder::kHeapAllocatorDefaultBytes);
            auto& containerPool = PreallocatedContainerPool::get(opCtx);
            auto keys = containerPool.keys();
            KeyStringSet* multikeyMetadataKeys = nullptr;
            MultikeyPaths* multikeyPaths = nullptr;

            getKeys(opCtx,
                    collection,
                    entry,
                    pooledBuilder,
                    requestedKey,
                    InsertDeleteOptions::ConstraintEnforcementMode::kEnforceConstraints,
                    GetKeysContext::kAddingKeys,
                    keys.get(),
                    multikeyMetadataKeys,
                    multikeyPaths,
                    boost::none /* loc */);
            invariant(keys->size() == 1);
            const key_string::Value& actualKey = *keys->begin();
            dassert(key_string::decodeDiscriminator(actualKey.getView(),
                                                    getSortedDataInterface()->getOrdering(),
                                                    actualKey.getTypeBits()) ==
                    key_string::Discriminator::kInclusive);
            return _newInterface->findLoc(opCtx, ru, actualKey.getView());
        } else {
            key_string::Builder requestedKeyString(getSortedDataInterface()->getKeyStringVersion(),
                                                   requestedKey,
                                                   getSortedDataInterface()->getOrdering());
            return _newInterface->findLoc(opCtx, ru, requestedKeyString.finishAndGetBuffer());
        }
    }();

    if (loc) {
        dassert(!loc->isNull());
        return std::move(*loc);
    }

    return RecordId();
}

IndexValidateResults SortedDataIndexAccessMethod::validate(
    OperationContext* opCtx,
    RecoveryUnit& ru,
    const CollectionValidation::ValidationOptions& options) const {
    return _newInterface->validate(opCtx, ru, options);
}

int64_t SortedDataIndexAccessMethod::numKeys(OperationContext* opCtx, RecoveryUnit& ru) const {
    return _newInterface->numEntries(opCtx, ru);
}

bool SortedDataIndexAccessMethod::appendCustomStats(OperationContext* opCtx,
                                                    RecoveryUnit& ru,
                                                    BSONObjBuilder* output,
                                                    double scale) const {
    return _newInterface->appendCustomStats(opCtx, ru, output, scale);
}

long long SortedDataIndexAccessMethod::getSpaceUsedBytes(OperationContext* opCtx,
                                                         RecoveryUnit& ru) const {
    return _newInterface->getSpaceUsedBytes(opCtx, ru);
}

long long SortedDataIndexAccessMethod::getFreeStorageBytes(OperationContext* opCtx,
                                                           RecoveryUnit& ru) const {
    return _newInterface->getFreeStorageBytes(opCtx, ru);
}

pair<KeyStringSet, KeyStringSet> SortedDataIndexAccessMethod::setDifference(
    const KeyStringSet& left, const KeyStringSet& right) {
    // Two iterators to traverse the two sets in sorted order.
    auto leftIt = left.begin();
    auto rightIt = right.begin();
    KeyStringSet::sequence_type onlyLeft;
    KeyStringSet::sequence_type onlyRight;

    while (leftIt != left.end() && rightIt != right.end()) {
        // Use compareWithTypeBits instead of the regular compare as we want just a difference in
        // typeinfo to also result in an index change.
        const int cmp = leftIt->compareWithTypeBits(*rightIt);
        if (cmp == 0) {
            ++leftIt;
            ++rightIt;
        } else if (cmp > 0) {
            onlyRight.push_back(*rightIt);
            ++rightIt;
        } else {
            onlyLeft.push_back(*leftIt);
            ++leftIt;
        }
    }

    // Add the rest of 'left' to 'onlyLeft', and the rest of 'right' to 'onlyRight', if any.
    onlyLeft.insert(onlyLeft.end(), leftIt, left.end());
    onlyRight.insert(onlyRight.end(), rightIt, right.end());

    KeyStringSet outLeft;
    KeyStringSet outRight;

    // The above algorithm guarantees that the elements are sorted and unique, so we can let the
    // container know so we get O(1) complexity adopting it.
    outLeft.adopt_sequence(boost::container::ordered_unique_range_t(), std::move(onlyLeft));
    outRight.adopt_sequence(boost::container::ordered_unique_range_t(), std::move(onlyRight));

    return {{std::move(outLeft)}, {std::move(outRight)}};
}

void SortedDataIndexAccessMethod::prepareUpdate(OperationContext* opCtx,
                                                const CollectionPtr& collection,
                                                const IndexCatalogEntry* entry,
                                                const BSONObj& from,
                                                const BSONObj& to,
                                                const RecordId& record,
                                                const InsertDeleteOptions& options,
                                                UpdateTicket* ticket) const {
    SharedBufferFragmentBuilder pooledBuilder(key_string::HeapBuilder::kHeapAllocatorDefaultBytes);
    const MatchExpression* indexFilter = entry->getFilterExpression();
    if (!indexFilter || exec::matcher::matchesBSON(indexFilter, from)) {
        // Override key constraints when generating keys for removal. This only applies to keys
        // that do not apply to a partial filter expression.
        const auto getKeysMode = entry->isHybridBuilding()
            ? InsertDeleteOptions::ConstraintEnforcementMode::kRelaxConstraintsUnfiltered
            : options.getKeysMode;

        // There's no need to compute the prefixes of the indexed fields that possibly caused the
        // index to be multikey when the old version of the document was written since the index
        // metadata isn't updated when keys are deleted.
        getKeys(opCtx,
                collection,
                entry,
                pooledBuilder,
                from,
                getKeysMode,
                GetKeysContext::kRemovingKeys,
                &ticket->oldKeys,
                nullptr,
                nullptr,
                record);
    }

    if (!indexFilter || exec::matcher::matchesBSON(indexFilter, to)) {
        getKeys(opCtx,
                collection,
                entry,
                pooledBuilder,
                to,
                options.getKeysMode,
                GetKeysContext::kAddingKeys,
                &ticket->newKeys,
                &ticket->newMultikeyMetadataKeys,
                &ticket->newMultikeyPaths,
                record);
    }

    ticket->loc = record;
    ticket->dupsAllowed = options.dupsAllowed;

    std::tie(ticket->removed, ticket->added) = setDifference(ticket->oldKeys, ticket->newKeys);

    ticket->_isValid = true;
}

Status SortedDataIndexAccessMethod::doUpdate(OperationContext* opCtx,
                                             RecoveryUnit& ru,
                                             const CollectionPtr& coll,
                                             const IndexCatalogEntry* entry,
                                             const UpdateTicket& ticket,
                                             int64_t* numInserted,
                                             int64_t* numDeleted) {
    invariant(!entry->isHybridBuilding());
    invariant(ticket.newKeys.size() ==
              ticket.oldKeys.size() + ticket.added.size() - ticket.removed.size());
    invariant(numInserted);
    invariant(numDeleted);

    *numInserted = 0;
    *numDeleted = 0;

    if (!ticket._isValid) {
        return Status(ErrorCodes::InternalError, "Invalid UpdateTicket in update");
    }

    for (const auto& remKey : ticket.removed) {
        _newInterface->unindex(opCtx, ru, remKey, ticket.dupsAllowed);
    }

    // Add all new data keys into the index.
    for (const auto& keyString : ticket.added) {
        bool dupsAllowed =
            (!entry->descriptor()->prepareUnique() || !opCtx->isEnforcingConstraints()) &&
            ticket.dupsAllowed;
        auto result = _newInterface->insert(opCtx, ru, keyString, dupsAllowed);
        if (auto duplicate = std::get_if<SortedDataInterface::DuplicateKey>(&result)) {
            return buildDupKeyErrorStatus(duplicate->key,
                                          coll->ns(),
                                          entry->descriptor()->indexName(),
                                          entry->descriptor()->keyPattern(),
                                          entry->descriptor()->collation(),
                                          std::move(duplicate->foundValue),
                                          std::move(duplicate->id));
        } else if (auto& status = std::get<Status>(result); !status.isOK()) {
            return status;
        }
    }

    // If these keys should cause the index to become multikey, pass them into the catalog.
    if (shouldMarkIndexAsMultikey(
            ticket.newKeys.size(), ticket.newMultikeyMetadataKeys, ticket.newMultikeyPaths)) {
        entry->setMultikey(opCtx, coll, ticket.newMultikeyMetadataKeys, ticket.newMultikeyPaths);
    }

    // If we have some multikey metadata keys, they should have been added while marking the index
    // as multikey in the catalog. Add them to the count of keys inserted for completeness.
    *numInserted = ticket.added.size() + ticket.newMultikeyMetadataKeys.size();
    *numDeleted = ticket.removed.size();

    return Status::OK();
}

StatusWith<int64_t> SortedDataIndexAccessMethod::compact(OperationContext* opCtx,
                                                         RecoveryUnit& ru,
                                                         const CompactOptions& options) {
    return this->_newInterface->compact(opCtx, ru, options);
}

Status SortedDataIndexAccessMethod::truncate(OperationContext* opCtx, RecoveryUnit& ru) {
    return this->_newInterface->truncate(opCtx, ru);
}

std::shared_ptr<Ident> SortedDataIndexAccessMethod::getSharedIdent() const {
    return this->_newInterface->getSharedIdent();
}

void SortedDataIndexAccessMethod::setIdent(std::shared_ptr<Ident> newIdent) {
    this->_newInterface->setIdent(std::move(newIdent));
}

Status SortedDataIndexAccessMethod::applyIndexBuildSideWrite(OperationContext* opCtx,
                                                             const CollectionPtr& coll,
                                                             const IndexCatalogEntry* entry,
                                                             const BSONObj& operation,
                                                             const InsertDeleteOptions& options,
                                                             KeyHandlerFn&& onDuplicateKey,
                                                             int64_t* const keysInserted,
                                                             int64_t* const keysDeleted) {
    auto opType = [&operation] {
        switch (operation.getStringField("op")[0]) {
            case 'i':
                return IndexBuildInterceptor::Op::kInsert;
            case 'd':
                return IndexBuildInterceptor::Op::kDelete;
            case 'u':
                return IndexBuildInterceptor::Op::kUpdate;
            default:
                MONGO_UNREACHABLE;
        }
    }();

    // Deserialize the encoded key_string::Value.
    int keyLen;
    const char* binKey = operation["key"].binData(keyLen);
    BufReader reader(binKey, keyLen);
    const key_string::Value keyString =
        key_string::Value::deserialize(reader,
                                       getSortedDataInterface()->getKeyStringVersion(),
                                       getSortedDataInterface()->rsKeyFormat());

    auto& ru = *shard_role_details::getRecoveryUnit(opCtx);
    const KeyStringSet keySet{keyString};
    if (opType == IndexBuildInterceptor::Op::kInsert) {
        int64_t numInserted;
        auto status = insertKeysAndUpdateMultikeyPaths(opCtx,
                                                       ru,
                                                       coll,
                                                       entry,
                                                       {keySet.begin(), keySet.end()},
                                                       {},
                                                       MultikeyPaths{},
                                                       options,
                                                       std::move(onDuplicateKey),
                                                       &numInserted);
        if (!status.isOK()) {
            return status;
        }

        *keysInserted += numInserted;
        ru.onRollback(
            [keysInserted, numInserted](OperationContext*) { *keysInserted -= numInserted; });
    } else {
        invariant(opType == IndexBuildInterceptor::Op::kDelete);
        int64_t numDeleted;
        Status s =
            removeKeys(opCtx, ru, entry, {keySet.begin(), keySet.end()}, options, &numDeleted);
        if (!s.isOK()) {
            return s;
        }

        *keysDeleted += numDeleted;
        ru.onRollback([keysDeleted, numDeleted](OperationContext*) { *keysDeleted -= numDeleted; });
    }
    return Status::OK();
}

void IndexAccessMethod::BulkBuilder::countNewBuildInStats() {
    indexBulkBuilderSSS.count.addAndFetch(1);
}

void IndexAccessMethod::BulkBuilder::countResumedBuildInStats() {
    indexBulkBuilderSSS.count.addAndFetch(1);
    indexBulkBuilderSSS.resumed.addAndFetch(1);
}

SorterFileStats* IndexAccessMethod::BulkBuilder::bulkBuilderFileStats() {
    return &indexBulkBuilderSSS.sorterFileStats;
}

SorterTracker* IndexAccessMethod::BulkBuilder::bulkBuilderTracker() {
    return &indexBulkBuilderSSS.sorterTracker;
}


class SortedDataIndexAccessMethod::BaseBulkBuilder : public IndexAccessMethod::BulkBuilder {
public:
    using Data = std::pair<key_string::Value, mongo::NullValue>;
    using Iterator = SortIteratorInterface<key_string::Value, mongo::NullValue>;
    using KeyHandlerFn = IndexAccessMethod::KeyHandlerFn;
    using RecordIdHandlerFn = IndexAccessMethod::RecordIdHandlerFn;

    BaseBulkBuilder(const IndexCatalogEntry* entry,
                    SortedDataIndexAccessMethod* iam,
                    size_t maxMemoryUsageBytes,
                    const DatabaseName& dbName,
                    StringData progressMessage);

    BaseBulkBuilder(const IndexCatalogEntry* entry,
                    SortedDataIndexAccessMethod* iam,
                    size_t maxMemoryUsageBytes,
                    const IndexStateInfo& stateInfo,
                    const DatabaseName& dbName,
                    StringData progressMessage);


    Status insert(OperationContext* opCtx,
                  const CollectionPtr& collection,
                  const IndexCatalogEntry* entry,
                  const BSONObj& obj,
                  const RecordId& loc,
                  const InsertDeleteOptions& options,
                  const OnSuppressedErrorFn& onSuppressedError = nullptr,
                  const ShouldRelaxConstraintsFn& shouldRelaxConstraints = nullptr) final;

    Status commit(OperationContext* opCtx,
                  RecoveryUnit& ru,
                  const CollectionPtr* collection,
                  const IndexCatalogEntry* entry,
                  bool dupsAllowed,
                  int32_t yieldIterations,
                  const KeyHandlerFn& onDuplicateKeyInserted,
                  const RecordIdHandlerFn& onDuplicateRecord,
                  const YieldFn& yieldFn) final;

protected:
    const MultikeyPaths& getMultikeyPaths() const final;

    void setMultikeyPaths(const MultikeyPaths& multikeyPaths);

    void setMultikeyPath(const MultikeyPaths& multikeyPaths, size_t idx);

    void clearMultikeyMetadataKeys();

    bool isMultikey() const final;

    void setIsMultikey(size_t numberOfKeys, const MultikeyPaths& multikeyPaths);

    int64_t _keysInserted = 0;

    SortedDataIndexAccessMethod* _iam;

    // Caches the set of all multikey metadata keys generated during the bulk build process.
    // These are inserted into the sorter after all normal data keys have been added, just
    // before the bulk build is committed.
    KeyStringSet _multikeyMetadataKeys;

private:
    virtual std::unique_ptr<mongo::Sorter<key_string::Value, mongo::NullValue>::Iterator>
    _finalizeSort(OperationContext* opCtx, RecoveryUnit& ru, const CollectionPtr& coll) = 0;

    virtual SharedBufferFragmentBuilder& _getMemPool() = 0;

    virtual void _insert(OperationContext* opCtx,
                         RecoveryUnit& ru,
                         const CollectionPtr& coll,
                         const IndexCatalogEntry& entry,
                         const key_string::Value& keyString) = 0;

    virtual void _addKeyForCommit(OperationContext* opCtx,
                                  RecoveryUnit& ru,
                                  const CollectionPtr& coll,
                                  const key_string::View& key) = 0;

    virtual void _finishCommit() = 0;

    void _debugEnsureSorted(const Data& data);

    bool _duplicateCheck(OperationContext* opCtx,
                         const IndexCatalogEntry* entry,
                         const Data& data,
                         bool dupsAllowed,
                         const RecordIdHandlerFn& onDuplicateRecord);

    key_string::Value _previousKey;
    const StringData _progressMessage;
    const std::string _indexName;
    NamespaceString _ns;

    // Set to true if any document added to the BulkBuilder causes the index to become multikey.
    bool _isMultiKey = false;

    // Holds the path components that cause this index to be multikey. The '_indexMultikeyPaths'
    // vector remains empty if this index doesn't support path-level multikey tracking.
    MultikeyPaths _indexMultikeyPaths;
};

SortedDataIndexAccessMethod::BaseBulkBuilder::BaseBulkBuilder(const IndexCatalogEntry* entry,
                                                              SortedDataIndexAccessMethod* iam,
                                                              size_t maxMemoryUsageBytes,
                                                              const DatabaseName& dbName,
                                                              StringData progressMessage)
    : _iam(iam), _progressMessage(progressMessage), _indexName(entry->descriptor()->indexName()) {
    countNewBuildInStats();
}

SortedDataIndexAccessMethod::BaseBulkBuilder::BaseBulkBuilder(const IndexCatalogEntry* entry,
                                                              SortedDataIndexAccessMethod* iam,
                                                              size_t maxMemoryUsageBytes,
                                                              const IndexStateInfo& stateInfo,
                                                              const DatabaseName& dbName,
                                                              StringData progressMessage)
    : _keysInserted(stateInfo.getNumKeys().value_or(0)),
      _iam(iam),
      _progressMessage(progressMessage),
      _indexName(entry->descriptor()->indexName()),
      _isMultiKey(stateInfo.getIsMultikey()),
      _indexMultikeyPaths(createMultikeyPaths(stateInfo.getMultikeyPaths())) {
    countResumedBuildInStats();
}

const MultikeyPaths& SortedDataIndexAccessMethod::BaseBulkBuilder::getMultikeyPaths() const {
    return _indexMultikeyPaths;
}

void SortedDataIndexAccessMethod::BaseBulkBuilder::setMultikeyPaths(
    const MultikeyPaths& multikeyPaths) {
    _indexMultikeyPaths = multikeyPaths;
}

void SortedDataIndexAccessMethod::BaseBulkBuilder::setMultikeyPath(
    const MultikeyPaths& multikeyPaths, size_t idx) {
    _indexMultikeyPaths[idx].insert(boost::container::ordered_unique_range_t(),
                                    (multikeyPaths)[idx].begin(),
                                    (multikeyPaths)[idx].end());
}


void SortedDataIndexAccessMethod::BaseBulkBuilder::clearMultikeyMetadataKeys() {
    _multikeyMetadataKeys.clear();
}

bool SortedDataIndexAccessMethod::BaseBulkBuilder::isMultikey() const {
    return _isMultiKey;
}

void SortedDataIndexAccessMethod::BaseBulkBuilder::setIsMultikey(
    size_t numberOfKeys, const MultikeyPaths& multikeyPaths) {
    _isMultiKey = _isMultiKey ||
        _iam->shouldMarkIndexAsMultikey(numberOfKeys, _multikeyMetadataKeys, multikeyPaths);
}

bool SortedDataIndexAccessMethod::BaseBulkBuilder::_duplicateCheck(
    OperationContext* opCtx,
    const IndexCatalogEntry* entry,
    const Data& data,
    bool dupsAllowed,
    const RecordIdHandlerFn& onDuplicateRecord) {

    // Duplicate checking is only applicable to unique (including id) indexes
    if (!entry->descriptor()->unique()) {
        return false;
    }

    auto keyFormat = _iam->getSortedDataInterface()->rsKeyFormat();
    auto& key = data.first;
    int cmpData = key.compareWithoutRecordId(_previousKey, keyFormat);
    if (cmpData != 0) {
        invariant(cmpData > 0);
        return false;
    }

    if (dupsAllowed) {
        return true;
    }

    RecordId recordId = key_string::decodeRecordIdAtEnd(key.getView(), keyFormat);

    // If supplied, onDuplicateRecord may be able to clean up the state, such as by moving the
    // duplicate to a different collection
    if (onDuplicateRecord) {
        uassertStatusOK(onDuplicateRecord(recordId));
        return true;
    }

    // Otherwise we just report the duplicate error
    BSONObj dupKey = key_string::toBson(key, _iam->getSortedDataInterface()->getOrdering());
    uassertStatusOK(buildDupKeyErrorStatus(dupKey.getOwned(),
                                           entry->getNSSFromCatalog(opCtx),
                                           entry->descriptor()->indexName(),
                                           entry->descriptor()->keyPattern(),
                                           entry->descriptor()->collation()));
    MONGO_COMPILER_UNREACHABLE;  // The status will never be OK
}

void SortedDataIndexAccessMethod::BaseBulkBuilder::_debugEnsureSorted(const Data& data) {
    if (data.first.compare(_previousKey) < 0) {
        LOGV2_FATAL_NOTRACE(31171,
                            "Expected the next key to be greater than or equal to the previous key",
                            "nextKey"_attr = data.first.toString(),
                            "previousKey"_attr = _previousKey.toString(),
                            "index"_attr = _indexName);
    }
}

Status SortedDataIndexAccessMethod::BaseBulkBuilder::insert(
    OperationContext* opCtx,
    const CollectionPtr& collection,
    const IndexCatalogEntry* entry,
    const BSONObj& obj,
    const RecordId& loc,
    const InsertDeleteOptions& options,
    const OnSuppressedErrorFn& onSuppressedError,
    const ShouldRelaxConstraintsFn& shouldRelaxConstraints) {
    auto& containerPool = PreallocatedContainerPool::get(opCtx);

    auto keys = containerPool.keys();
    auto multikeyPaths = containerPool.multikeyPaths();

    try {
        _iam->getKeys(opCtx,
                      collection,
                      entry,
                      _getMemPool(),
                      obj,
                      options.getKeysMode,
                      GetKeysContext::kAddingKeys,
                      keys.get(),
                      &_multikeyMetadataKeys,
                      multikeyPaths.get(),
                      loc,
                      onSuppressedError,
                      shouldRelaxConstraints);
    } catch (...) {
        return exceptionToStatus();
    }

    if (!multikeyPaths->empty()) {
        auto currMultikeyPaths = getMultikeyPaths();
        if (currMultikeyPaths.empty()) {
            setMultikeyPaths(*multikeyPaths);
        } else {
            invariant(currMultikeyPaths.size() == multikeyPaths->size());
            for (size_t i = 0; i < multikeyPaths->size(); ++i) {
                setMultikeyPath(*multikeyPaths, i);
            }
        }
    }

    for (const auto& keyString : *keys) {
        _insert(opCtx, *shard_role_details::getRecoveryUnit(opCtx), collection, *entry, keyString);
        ++_keysInserted;
    }

    setIsMultikey(keys->size(), *multikeyPaths);

    return Status::OK();
}

Status SortedDataIndexAccessMethod::BaseBulkBuilder::commit(
    OperationContext* opCtx,
    RecoveryUnit& ru,
    const CollectionPtr* collection,
    const IndexCatalogEntry* entry,
    bool dupsAllowed,
    int32_t yieldIterations,
    const KeyHandlerFn& onDuplicateKeyInserted,
    const RecordIdHandlerFn& onDuplicateRecord,
    const YieldFn& yieldFn) {
    Timer timer;

    _ns = entry->getNSSFromCatalog(opCtx);
    auto it = _finalizeSort(opCtx, ru, *collection);

    ProgressMeterHolder pm;
    {
        stdx::unique_lock<Client> lk(*opCtx->getClient());
        pm.set(lk,
               CurOp::get(opCtx)->setProgress(
                   lk, _progressMessage, _keysInserted, 3 /* secondsBetween */),
               opCtx);
    }

    int64_t iterations = 0;
    while (it->more()) {
        opCtx->checkForInterrupt();

        auto failPointHang = [opCtx, iterations, &indexName = _indexName](FailPoint& fp) {
            fp.executeIf(
                [&fp, opCtx, iterations, &indexName](const BSONObj& data) {
                    LOGV2(4924400,
                          "Hanging index build during bulk load phase",
                          "iteration"_attr = iterations,
                          "index"_attr = indexName);

                    fp.pauseWhileSet(opCtx);
                },
                [iterations, &indexName](const BSONObj& data) {
                    auto indexNames = data.getObjectField("indexNames");
                    return iterations == data["iteration"].numberLong() &&
                        std::any_of(indexNames.begin(),
                                    indexNames.end(),
                                    [&indexName](const auto& elem) {
                                        return indexName == elem.String();
                                    });
                });
        };
        failPointHang(hangIndexBuildDuringBulkLoadPhase);
        failPointHang(hangIndexBuildDuringBulkLoadPhaseSecond);

        auto data = it->next();
        if (kDebugBuild) {
            _debugEnsureSorted(data);
        }

        // Before attempting to insert, check if this key is a duplicate of the previous one
        // inserted. onDuplicateRecord may attempt to perform a write to repair the state, which can
        // potentially fail.
        bool isDup;
        try {
            isDup = _duplicateCheck(opCtx, entry, data, dupsAllowed, onDuplicateRecord);
        } catch (DBException& e) {
            return e.toStatus();
        }

        if (isDup && !dupsAllowed) {
            // onDuplicateRecord took care of processing this duplicate key, so we don't need to do
            // anything.
            continue;
        }

        _previousKey = data.first;

        try {
            writeConflictRetry(opCtx, "addingKey", _ns, [&] {
                WriteUnitOfWork wunit(opCtx);
                _addKeyForCommit(opCtx, ru, *collection, data.first);
                wunit.commit();
            });
        } catch (DBException& e) {
            Status status = e.toStatus();
            // Duplicates are checked before inserting.
            invariant(status.code() != ErrorCodes::DuplicateKey);
            return status;
        }

        if (isDup) {
            if (auto status = onDuplicateKeyInserted(data.first); !status.isOK())
                return status;
        }

        // Yield locks every 'yieldIterations' key insertions.
        if (yieldIterations > 0 && (++iterations % yieldIterations == 0)) {
            std::tie(collection, entry) = yieldFn(opCtx);
        }

        {
            stdx::unique_lock<Client> lk(*opCtx->getClient());
            // If we're here either it's a dup and we're cool with it or the addKey went
            // just fine.
            pm.get(lk)->hit();
        }
    }

    {
        stdx::unique_lock<Client> lk(*opCtx->getClient());
        pm.get(lk)->finished();
    }

    _finishCommit();

    LOGV2(20685,
          "Index build: inserted keys from external sorter into index",
          logAttrs(_ns),
          "index"_attr = _indexName,
          "keysInserted"_attr = _keysInserted,
          "duration"_attr = duration_cast<Milliseconds>(timer.elapsed()));
    return Status::OK();
}

class SortedDataIndexAccessMethod::PrimaryDrivenBulkBuilder final
    : public SortedDataIndexAccessMethod::BaseBulkBuilder {
public:
    PrimaryDrivenBulkBuilder(const IndexCatalogEntry* entry,
                             SortedDataIndexAccessMethod* iam,
                             size_t maxMemoryUsageBytes,
                             const DatabaseName& dbName);


    IndexStateInfo persistDataForShutdown() final;

private:
    std::unique_ptr<mongo::Sorter<key_string::Value, mongo::NullValue>::Iterator> _finalizeSort(
        OperationContext* opCtx, RecoveryUnit& ru, const CollectionPtr& coll) final;

    SharedBufferFragmentBuilder& _getMemPool() final;

    void _insert(OperationContext* opCtx,
                 RecoveryUnit& ru,
                 const CollectionPtr& coll,
                 const IndexCatalogEntry& entry,
                 const key_string::Value& keyString) final;

    void _addKeyForCommit(OperationContext* opCtx,
                          RecoveryUnit& ru,
                          const CollectionPtr& coll,
                          const key_string::View& key) final;

    void _finishCommit() final {}

    SharedBufferFragmentBuilder _memPool;
};

SortedDataIndexAccessMethod::PrimaryDrivenBulkBuilder::PrimaryDrivenBulkBuilder(
    const IndexCatalogEntry* entry,
    SortedDataIndexAccessMethod* iam,
    size_t maxMemoryUsageBytes,
    const DatabaseName& dbName)
    : BaseBulkBuilder(entry,
                      iam,
                      maxMemoryUsageBytes,
                      dbName,
                      "Index Build: sorting and inserting keys into the index table"),
      _memPool(sorter::makeMemPool()) {}

SharedBufferFragmentBuilder& SortedDataIndexAccessMethod::PrimaryDrivenBulkBuilder::_getMemPool() {
    return _memPool;
}

void SortedDataIndexAccessMethod::PrimaryDrivenBulkBuilder::_insert(
    OperationContext* opCtx,
    RecoveryUnit& ru,
    const CollectionPtr& coll,
    const IndexCatalogEntry& entry,
    const key_string::Value& keyString) {
    if (entry.descriptor()->unique() &&
        _iam->getSortedDataInterface()->findLoc(opCtx, ru, keyString.getViewWithoutRecordId())) {
        uassertStatusOK(buildDupKeyErrorStatus(keyString,
                                               coll->ns(),
                                               entry.descriptor()->indexName(),
                                               entry.descriptor()->keyPattern(),
                                               entry.descriptor()->collation(),
                                               _iam->getSortedDataInterface()->getOrdering()));
    }

    WriteUnitOfWork wuow{opCtx};
    uassertStatusOK(container_write::insert(opCtx,
                                            ru,
                                            coll,
                                            _iam->getSortedDataInterface()->getContainer(),
                                            keyString.getView(),
                                            keyString.getTypeBitsView()));
    wuow.commit();
}

std::unique_ptr<mongo::Sorter<key_string::Value, mongo::NullValue>::Iterator>
SortedDataIndexAccessMethod::PrimaryDrivenBulkBuilder::_finalizeSort(OperationContext* opCtx,
                                                                     RecoveryUnit& ru,
                                                                     const CollectionPtr& coll) {
    for (auto&& key : _multikeyMetadataKeys) {
        WriteUnitOfWork wuow{opCtx};
        uassertStatusOK(container_write::insert(opCtx,
                                                ru,
                                                coll,
                                                _iam->getSortedDataInterface()->getContainer(),
                                                key.getView(),
                                                key.getTypeBitsView()));
        wuow.commit();
        ++_keysInserted;
    }
    return std::make_unique<sorter::InMemIterator<key_string::Value, mongo::NullValue>>();
}

void SortedDataIndexAccessMethod::PrimaryDrivenBulkBuilder::_addKeyForCommit(
    OperationContext* opCtx,
    RecoveryUnit& ru,
    const CollectionPtr& coll,
    const key_string::View& key) {
    uassertStatusOK(container_write::insert(opCtx,
                                            ru,
                                            coll,
                                            _iam->getSortedDataInterface()->getContainer(),
                                            key.getKeyAndRecordIdView(),
                                            key.getTypeBitsView()));
}

IndexStateInfo SortedDataIndexAccessMethod::PrimaryDrivenBulkBuilder::persistDataForShutdown() {
    MONGO_UNREACHABLE_TASSERT(1081640);
}

class SortedDataIndexAccessMethod::HybridBulkBuilder final
    : public SortedDataIndexAccessMethod::BaseBulkBuilder {
public:
    using Sorter = mongo::Sorter<key_string::Value, mongo::NullValue>;

    HybridBulkBuilder(const IndexCatalogEntry* entry,
                      SortedDataIndexAccessMethod* iam,
                      size_t maxMemoryUsageBytes,
                      const DatabaseName& dbName);

    HybridBulkBuilder(const IndexCatalogEntry* entry,
                      SortedDataIndexAccessMethod* iam,
                      size_t maxMemoryUsageBytes,
                      const IndexStateInfo& stateInfo,
                      const DatabaseName& dbName);

    IndexStateInfo persistDataForShutdown() final;

private:
    std::unique_ptr<mongo::Sorter<key_string::Value, mongo::NullValue>::Iterator> _finalizeSort(
        OperationContext* opCtx, RecoveryUnit& ru, const CollectionPtr& coll) final;

    SharedBufferFragmentBuilder& _getMemPool() final;

    void _insert(OperationContext* opCtx,
                 RecoveryUnit& ru,
                 const CollectionPtr& coll,
                 const IndexCatalogEntry& entry,
                 const key_string::Value& keyString) final;

    void _addKeyForCommit(OperationContext* opCtx,
                          RecoveryUnit& ru,
                          const CollectionPtr& coll,
                          const key_string::View& key) final;

    void _finishCommit() final;

    void _insertMultikeyMetadataKeysIntoSorter();

    std::unique_ptr<Sorter> _makeSorter(
        size_t maxMemoryUsageBytes,
        const DatabaseName& dbName,
        boost::optional<StringData> fileName = boost::none,
        const boost::optional<std::vector<SorterRange>>& ranges = boost::none) const;

    Sorter::Settings _makeSorterSettings() const;
    std::unique_ptr<Sorter> _sorter;
    std::unique_ptr<SortedDataBuilderInterface> _builder;
};

std::unique_ptr<IndexAccessMethod::BulkBuilder> SortedDataIndexAccessMethod::initiateBulk(
    const IndexCatalogEntry* entry,
    size_t maxMemoryUsageBytes,
    const boost::optional<IndexStateInfo>& stateInfo,
    const DatabaseName& dbName,
    const IndexBuildMethodEnum& method) {
    if (method == IndexBuildMethodEnum::kPrimaryDriven) {
        invariant(!stateInfo);
        return std::make_unique<PrimaryDrivenBulkBuilder>(entry, this, maxMemoryUsageBytes, dbName);
    }
    return stateInfo
        ? std::make_unique<HybridBulkBuilder>(entry, this, maxMemoryUsageBytes, *stateInfo, dbName)
        : std::make_unique<HybridBulkBuilder>(entry, this, maxMemoryUsageBytes, dbName);
}

SortedDataIndexAccessMethod::HybridBulkBuilder::HybridBulkBuilder(const IndexCatalogEntry* entry,
                                                                  SortedDataIndexAccessMethod* iam,
                                                                  size_t maxMemoryUsageBytes,
                                                                  const DatabaseName& dbName)
    : BaseBulkBuilder(entry,
                      iam,
                      maxMemoryUsageBytes,
                      dbName,
                      "Index Build: inserting keys from external sorter into index"),
      _sorter(_makeSorter(maxMemoryUsageBytes, dbName)) {}

SortedDataIndexAccessMethod::HybridBulkBuilder::HybridBulkBuilder(const IndexCatalogEntry* entry,
                                                                  SortedDataIndexAccessMethod* iam,
                                                                  size_t maxMemoryUsageBytes,
                                                                  const IndexStateInfo& stateInfo,
                                                                  const DatabaseName& dbName)
    : BaseBulkBuilder(entry,
                      iam,
                      maxMemoryUsageBytes,
                      stateInfo,
                      dbName,
                      "Index Build: inserting keys from external sorter into index"),
      _sorter(_makeSorter(
          maxMemoryUsageBytes, dbName, stateInfo.getFileName(), stateInfo.getRanges())) {}

SharedBufferFragmentBuilder& SortedDataIndexAccessMethod::HybridBulkBuilder::_getMemPool() {
    return _sorter->memPool();
}

IndexStateInfo SortedDataIndexAccessMethod::HybridBulkBuilder::persistDataForShutdown() {
    _insertMultikeyMetadataKeysIntoSorter();
    auto state = _sorter->persistDataForShutdown();

    IndexStateInfo stateInfo;
    stateInfo.setFileName(state.fileName);
    stateInfo.setNumKeys(_keysInserted);
    stateInfo.setRanges(std::move(state.ranges));

    return stateInfo;
}

void SortedDataIndexAccessMethod::HybridBulkBuilder::_insert(OperationContext* opCtx,
                                                             RecoveryUnit& ru,
                                                             const CollectionPtr& coll,
                                                             const IndexCatalogEntry& entry,
                                                             const key_string::Value& keyString) {
    _sorter->add(keyString, mongo::NullValue());
}

void SortedDataIndexAccessMethod::HybridBulkBuilder::_addKeyForCommit(OperationContext* opCtx,
                                                                      RecoveryUnit& ru,
                                                                      const CollectionPtr& coll,
                                                                      const key_string::View& key) {
    if (!_builder) {
        _builder = _iam->getSortedDataInterface()->makeBulkBuilder(opCtx, ru);
    }
    _builder->addKey(ru, key);
}

void SortedDataIndexAccessMethod::HybridBulkBuilder::_finishCommit() {
    _builder.reset();
}

void SortedDataIndexAccessMethod::HybridBulkBuilder::_insertMultikeyMetadataKeysIntoSorter() {
    for (const auto& keyString : _multikeyMetadataKeys) {
        _sorter->add(keyString, mongo::NullValue());
        ++_keysInserted;
    }

    // We clear the multikey metadata keys to prevent them from being inserted into the Sorter
    // twice in the case that done() is called and then persistDataForShutdown() is later called.
    clearMultikeyMetadataKeys();
}

std::unique_ptr<SortedDataIndexAccessMethod::HybridBulkBuilder::Sorter>
SortedDataIndexAccessMethod::HybridBulkBuilder::_makeSorter(
    size_t maxMemoryUsageBytes,
    const DatabaseName& dbName,
    boost::optional<StringData> fileName,
    const boost::optional<std::vector<SorterRange>>& ranges) const {
    return fileName
        ? Sorter::makeFromExistingRanges(
              std::string{*fileName},
              *ranges,
              makeSortOptions(maxMemoryUsageBytes, dbName, bulkBuilderFileStats()),
              BtreeExternalSortComparison(),
              _makeSorterSettings())
        : Sorter::make(makeSortOptions(maxMemoryUsageBytes, dbName, bulkBuilderFileStats()),
                       BtreeExternalSortComparison(),
                       _makeSorterSettings());
}

SortedDataIndexAccessMethod::HybridBulkBuilder::Sorter::Settings
SortedDataIndexAccessMethod::HybridBulkBuilder::_makeSorterSettings() const {
    return std::pair<key_string::Value::SorterDeserializeSettings,
                     mongo::NullValue::SorterDeserializeSettings>(
        {_iam->getSortedDataInterface()->getKeyStringVersion(),
         _iam->getSortedDataInterface()->rsKeyFormat()},
        {});
}

std::unique_ptr<mongo::Sorter<key_string::Value, mongo::NullValue>::Iterator>
SortedDataIndexAccessMethod::HybridBulkBuilder::_finalizeSort(OperationContext* opCtx,
                                                              RecoveryUnit& ru,
                                                              const CollectionPtr& coll) {
    _insertMultikeyMetadataKeysIntoSorter();
    return _sorter->done();
}

void SortedDataIndexAccessMethod::getKeys(
    OperationContext* opCtx,
    const CollectionPtr& collection,
    const IndexCatalogEntry* entry,
    SharedBufferFragmentBuilder& pooledBufferBuilder,
    const BSONObj& obj,
    InsertDeleteOptions::ConstraintEnforcementMode mode,
    GetKeysContext context,
    KeyStringSet* keys,
    KeyStringSet* multikeyMetadataKeys,
    MultikeyPaths* multikeyPaths,
    const boost::optional<RecordId>& id,
    const OnSuppressedErrorFn& onSuppressedErrorFn,
    const ShouldRelaxConstraintsFn& shouldRelaxConstraints) const {
    invariant(!id || _newInterface->rsKeyFormat() != KeyFormat::String || id->isStr(),
              fmt::format("RecordId is not in the same string format as its RecordStore; id: {}",
                          id->toString()));
    invariant(!id || _newInterface->rsKeyFormat() != KeyFormat::Long || id->isLong(),
              fmt::format("RecordId is not in the same long format as its RecordStore; id: {}",
                          id->toString()));

    try {
        if (entry->shouldValidateDocument()) {
            validateDocument(collection, obj, entry->descriptor()->keyPattern());
        }
        doGetKeys(opCtx,
                  collection,
                  entry,
                  pooledBufferBuilder,
                  obj,
                  context,
                  keys,
                  multikeyMetadataKeys,
                  multikeyPaths,
                  id);
    } catch (const AssertionException& ex) {
        // Suppress all indexing errors when mode is kRelaxConstraints.
        if (mode == InsertDeleteOptions::ConstraintEnforcementMode::kEnforceConstraints) {
            throw;
        }

        keys->clear();
        if (multikeyPaths) {
            multikeyPaths->clear();
        }

        if (!opCtx->checkForInterruptNoAssert().isOK()) {
            throw;
        }

        // If the document applies to the filter (which means that it should have never been
        // indexed), do not suppress the error.
        const MatchExpression* filter = entry->getFilterExpression();
        if (mode == InsertDeleteOptions::ConstraintEnforcementMode::kRelaxConstraintsUnfiltered &&
            filter && exec::matcher::matchesBSON(filter, obj)) {
            throw;
        }

        if (mode == InsertDeleteOptions::ConstraintEnforcementMode::kRelaxConstraintsCallback) {
            invariant(shouldRelaxConstraints);
            if (!shouldRelaxConstraints(opCtx, collection)) {
                throw;
            }
        }

        if (onSuppressedErrorFn) {
            onSuppressedErrorFn(opCtx, entry, ex.toStatus(), obj, id);
        } else {
            LOGV2_DEBUG(20686,
                        1,
                        "Suppressed key generation error",
                        "error"_attr = redact(ex.toStatus()),
                        "loc"_attr = id,
                        "obj"_attr = redact(obj));
        }
    }
}

bool SortedDataIndexAccessMethod::shouldMarkIndexAsMultikey(
    size_t numberOfKeys,
    const KeyStringSet& multikeyMetadataKeys,
    const MultikeyPaths& multikeyPaths) const {
    return numberOfKeys > 1 || isMultikeyFromPaths(multikeyPaths);
}

void SortedDataIndexAccessMethod::validateDocument(const CollectionPtr& collection,
                                                   const BSONObj& obj,
                                                   const BSONObj& keyPattern) const {}

Status SortedDataIndexAccessMethod::_indexKeysOrWriteToSideTable(
    OperationContext* opCtx,
    const CollectionPtr& coll,
    const IndexCatalogEntry* entry,
    const KeyStringSet& keys,
    const KeyStringSet& multikeyMetadataKeys,
    const MultikeyPaths& multikeyPaths,
    const BSONObj& obj,
    const InsertDeleteOptions& options,
    int64_t* keysInsertedOut) {
    Status status = Status::OK();
    if (entry->isHybridBuilding()) {
        // The side table interface accepts only records that meet the criteria for this partial
        // index.
        // See SERVER-28975 and SERVER-39705 for details.
        if (auto filter = entry->getFilterExpression()) {
            if (!exec::matcher::matchesBSON(filter, obj)) {
                return Status::OK();
            }
        }

        int64_t inserted = 0;
        status = entry->indexBuildInterceptor()->sideWrite(opCtx,
                                                           entry,
                                                           keys,
                                                           multikeyMetadataKeys,
                                                           multikeyPaths,
                                                           IndexBuildInterceptor::Op::kInsert,
                                                           &inserted);
        if (keysInsertedOut) {
            *keysInsertedOut += inserted;
        }
    } else {
        int64_t numInserted = 0;
        status = insertKeysAndUpdateMultikeyPaths(
            opCtx,
            *shard_role_details::getRecoveryUnit(opCtx),
            coll,
            entry,
            keys,
            {multikeyMetadataKeys.begin(), multikeyMetadataKeys.end()},
            multikeyPaths,
            options,
            nullptr,
            &numInserted);
        if (keysInsertedOut) {
            *keysInsertedOut += numInserted;
        }
    }

    return status;
}

void SortedDataIndexAccessMethod::_unindexKeysOrWriteToSideTable(
    OperationContext* opCtx,
    const NamespaceString& ns,
    const IndexCatalogEntry* entry,
    const KeyStringSet& keys,
    const BSONObj& obj,
    bool logIfError,
    int64_t* const keysDeletedOut,
    InsertDeleteOptions options,  // copy!
    CheckRecordId checkRecordId) {

    if (entry->isHybridBuilding()) {
        // The side table interface accepts only records that meet the criteria for this partial
        // index.
        // See SERVER-28975 and SERVER-39705 for details.
        if (auto filter = entry->getFilterExpression()) {
            if (!exec::matcher::matchesBSON(filter, obj)) {
                return;
            }
        }

        int64_t removed = 0;
        fassert(31155,
                entry->indexBuildInterceptor()->sideWrite(
                    opCtx, entry, keys, {}, {}, IndexBuildInterceptor::Op::kDelete, &removed));
        if (keysDeletedOut) {
            *keysDeletedOut += removed;
        }

        return;
    }

    // On WiredTiger, we do blind unindexing of records for efficiency.  However, when duplicates
    // are allowed in unique indexes, WiredTiger does not do blind unindexing, and instead confirms
    // that the recordid matches the element we are removing.
    //
    // We need to disable blind-deletes if 'checkRecordId' is explicitly set 'On'.
    options.dupsAllowed = options.dupsAllowed || checkRecordId == CheckRecordId::On;

    auto& ru = *shard_role_details::getRecoveryUnit(opCtx);
    int64_t removed = 0;
    Status status = removeKeys(opCtx, ru, entry, keys, options, &removed);

    if (!status.isOK()) {
        LOGV2(20362,
              "Couldn't unindex record",
              "record"_attr = redact(obj),
              logAttrs(ns),
              "error"_attr = redact(status));
    }

    if (keysDeletedOut) {
        *keysDeletedOut += removed;
    }
}

}  // namespace mongo
