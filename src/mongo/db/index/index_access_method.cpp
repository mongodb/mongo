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

#include <boost/container/container_fwd.hpp>
#include <fmt/format.h>
// IWYU pragma: no_include "boost/container/detail/std_fwd.hpp"
#include <boost/container/flat_set.hpp>
#include <boost/container/small_vector.hpp>
#include <boost/container/vector.hpp>
#include <boost/cstdint.hpp>
#include <boost/move/algo/move.hpp>
#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
// IWYU pragma: no_include "boost/intrusive/detail/iterator.hpp"
// IWYU pragma: no_include "boost/move/algo/detail/set_difference.hpp"
#include <algorithm>
#include <deque>
#include <exception>
#include <iterator>
#include <new>
#include <string>
#include <system_error>
#include <tuple>
#include <utility>
#include <vector>

#include <boost/optional/optional.hpp>

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/db/catalog/collection_options.h"
#include "mongo/db/catalog/index_catalog.h"
#include "mongo/db/catalog/validate_results.h"
#include "mongo/db/commands/server_status.h"
#include "mongo/db/concurrency/exception_util.h"
#include "mongo/db/curop.h"
#include "mongo/db/index/2d_access_method.h"
#include "mongo/db/index/btree_access_method.h"
#include "mongo/db/index/bulk_builder_common.h"
#include "mongo/db/index/columns_access_method.h"
#include "mongo/db/index/fts_access_method.h"
#include "mongo/db/index/hash_access_method.h"
#include "mongo/db/index/index_access_method.h"
#include "mongo/db/index/index_build_interceptor.h"
#include "mongo/db/index/index_descriptor.h"
#include "mongo/db/index/s2_access_method.h"
#include "mongo/db/index/s2_bucket_access_method.h"
#include "mongo/db/index/wildcard_access_method.h"
#include "mongo/db/index_names.h"
#include "mongo/db/matcher/expression.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"
#include "mongo/db/sorter/sorter.h"
#include "mongo/db/sorter/sorter_gen.h"
#include "mongo/db/storage/execution_context.h"
#include "mongo/db/storage/index_entry_comparison.h"
#include "mongo/db/storage/key_format.h"
#include "mongo/db/storage/kv/kv_engine.h"
#include "mongo/db/storage/recovery_unit.h"
#include "mongo/db/storage/storage_engine.h"
#include "mongo/db/storage/storage_options.h"
#include "mongo/db/transaction_resources.h"
#include "mongo/logv2/log.h"
#include "mongo/logv2/log_attr.h"
#include "mongo/logv2/log_component.h"
#include "mongo/logv2/redaction.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/platform/random.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/bufreader.h"
#include "mongo/util/database_name_util.h"
#include "mongo/util/decorable.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/namespace_string_util.h"
#include "mongo/util/stacktrace.h"
#include "mongo/util/str.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kCommand

namespace mongo {

using std::pair;

using IndexVersion = IndexDescriptor::IndexVersion;

MONGO_FAIL_POINT_DEFINE(hangIndexBuildDuringBulkLoadPhase);
MONGO_FAIL_POINT_DEFINE(hangIndexBuildDuringBulkLoadPhaseSecond);
MONGO_FAIL_POINT_DEFINE(hangDuringIndexBuildBulkLoadYield);
MONGO_FAIL_POINT_DEFINE(hangDuringIndexBuildBulkLoadYieldSecond);

/**
 * Static factory method that constructs and returns an appropriate IndexAccessMethod depending on
 * the type of the index.
 */
std::unique_ptr<IndexAccessMethod> IndexAccessMethod::make(
    OperationContext* opCtx,
    const NamespaceString& nss,
    const CollectionOptions& collectionOptions,
    IndexCatalogEntry* entry,
    StringData ident) {

    auto engine = opCtx->getServiceContext()->getStorageEngine()->getEngine();
    auto desc = entry->descriptor();
    auto makeSDI = [&] {
        return engine->getSortedDataInterface(opCtx, nss, collectionOptions, ident, desc);
    };
    auto makeCS = [&] {
        return engine->getColumnStore(opCtx, nss, collectionOptions, ident, desc);
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
    else if (IndexNames::COLUMN == type)
        return std::make_unique<ColumnStoreAccessMethod>(entry, makeCS());
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
        .ExtSortAllowed()
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

        auto& executionCtx = StorageExecutionContext::get(opCtx);
        auto keys = executionCtx.keys();
        auto multikeyMetadataKeys = executionCtx.multikeyMetadataKeys();
        auto multikeyPaths = executionCtx.multikeyPaths();

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
    auto& executionCtx = StorageExecutionContext::get(opCtx);

    // There's no need to compute the prefixes of the indexed fields that cause the index to be
    // multikey when removing a document since the index metadata isn't updated when keys are
    // deleted.
    auto keys = executionCtx.keys();
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
        return doUpdate(opCtx, coll, entry, updateTicket, numInserted, numDeleted);
    }
}

Status SortedDataIndexAccessMethod::insertKeysAndUpdateMultikeyPaths(
    OperationContext* opCtx,
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
        auto status =
            _newInterface->insert(opCtx, keyString, dupsAllowed, includeDuplicateRecordId);

        // When duplicates are encountered and allowed, retry with dupsAllowed. Call
        // onDuplicateKey() with the inserted duplicate key.
        if (ErrorCodes::DuplicateKey == status.code() && options.dupsAllowed && !prepareUnique) {
            invariant(unique);

            status = _newInterface->insert(
                opCtx, keyString, true /* dupsAllowed */, includeDuplicateRecordId);
            if (status.isOK() && onDuplicateKey) {
                status = onDuplicateKey(keyString);
            }
        }
        if (!status.isOK()) {
            return status;
        }
    }
    if (numInserted) {
        *numInserted = keys.size();
    }
    return Status::OK();
}

void SortedDataIndexAccessMethod::removeOneKey(OperationContext* opCtx,
                                               const IndexCatalogEntry* entry,
                                               const key_string::Value& keyString,
                                               bool dupsAllowed) const {

    try {
        _newInterface->unindex(opCtx, keyString, dupsAllowed);
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
    OperationContext* opCtx, bool isForward) const {
    return _newInterface->newCursor(opCtx, isForward);
}

Status SortedDataIndexAccessMethod::removeKeys(OperationContext* opCtx,
                                               const IndexCatalogEntry* entry,
                                               const KeyStringSet& keys,
                                               const InsertDeleteOptions& options,
                                               int64_t* numDeleted) const {

    for (const auto& key : keys) {
        removeOneKey(opCtx, entry, key, options.dupsAllowed);
    }

    *numDeleted = keys.size();
    return Status::OK();
}

Status SortedDataIndexAccessMethod::initializeAsEmpty(OperationContext* opCtx) {
    return _newInterface->initAsEmpty(opCtx);
}

RecordId SortedDataIndexAccessMethod::findSingle(OperationContext* opCtx,
                                                 const CollectionPtr& collection,
                                                 const IndexCatalogEntry* entry,
                                                 const BSONObj& requestedKey) const {
    // Generate the key for this index.
    key_string::Value actualKey = [&]() {
        if (entry->getCollator()) {
            // For performance, call get keys only if there is a non-simple collation.
            SharedBufferFragmentBuilder pooledBuilder(
                key_string::HeapBuilder::kHeapAllocatorDefaultBytes);
            auto& executionCtx = StorageExecutionContext::get(opCtx);
            auto keys = executionCtx.keys();
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
            return *keys->begin();
        } else {
            key_string::HeapBuilder requestedKeyString(
                getSortedDataInterface()->getKeyStringVersion(),
                requestedKey,
                getSortedDataInterface()->getOrdering());
            return requestedKeyString.release();
        }
    }();

    if (auto loc = _newInterface->findLoc(opCtx, actualKey)) {
        dassert(!loc->isNull());
        return std::move(*loc);
    }

    return RecordId();
}

IndexValidateResults SortedDataIndexAccessMethod::validate(OperationContext* opCtx,
                                                           bool full) const {
    return _newInterface->validate(opCtx, full);
}

int64_t SortedDataIndexAccessMethod::numKeys(OperationContext* opCtx) const {
    return _newInterface->numEntries(opCtx);
}

bool SortedDataIndexAccessMethod::appendCustomStats(OperationContext* opCtx,
                                                    BSONObjBuilder* output,
                                                    double scale) const {
    return _newInterface->appendCustomStats(opCtx, output, scale);
}

long long SortedDataIndexAccessMethod::getSpaceUsedBytes(OperationContext* opCtx) const {
    return _newInterface->getSpaceUsedBytes(opCtx);
}

long long SortedDataIndexAccessMethod::getFreeStorageBytes(OperationContext* opCtx) const {
    return _newInterface->getFreeStorageBytes(opCtx);
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
    if (!indexFilter || indexFilter->matchesBSON(from)) {
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

    if (!indexFilter || indexFilter->matchesBSON(to)) {
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
        _newInterface->unindex(opCtx, remKey, ticket.dupsAllowed);
    }

    // Add all new data keys into the index.
    for (const auto& keyString : ticket.added) {
        bool dupsAllowed = !entry->descriptor()->prepareUnique() && ticket.dupsAllowed;
        auto status = _newInterface->insert(opCtx, keyString, dupsAllowed);
        if (!status.isOK())
            return status;
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
                                                         const CompactOptions& options) {
    return this->_newInterface->compact(opCtx, options);
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
        key_string::Value::deserialize(reader, getSortedDataInterface()->getKeyStringVersion());

    const KeyStringSet keySet{keyString};
    if (opType == IndexBuildInterceptor::Op::kInsert) {
        int64_t numInserted;
        auto status = insertKeysAndUpdateMultikeyPaths(opCtx,
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
        shard_role_details::getRecoveryUnit(opCtx)->onRollback(
            [keysInserted, numInserted](OperationContext*) { *keysInserted -= numInserted; });
    } else {
        invariant(opType == IndexBuildInterceptor::Op::kDelete);
        int64_t numDeleted;
        Status s = removeKeys(opCtx, entry, {keySet.begin(), keySet.end()}, options, &numDeleted);
        if (!s.isOK()) {
            return s;
        }

        *keysDeleted += numDeleted;
        shard_role_details::getRecoveryUnit(opCtx)->onRollback(
            [keysDeleted, numDeleted](OperationContext*) { *keysDeleted -= numDeleted; });
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

const IndexCatalogEntry* IndexAccessMethod::BulkBuilder::yield(OperationContext* opCtx,
                                                               const CollectionPtr& collection,
                                                               const NamespaceString& ns,
                                                               const IndexCatalogEntry* entry) {
    const std::string indexIdent = entry->getIdent();

    // Releasing locks means a new snapshot should be acquired when restored.
    shard_role_details::getRecoveryUnit(opCtx)->abandonSnapshot();
    collection.yield();

    auto locker = shard_role_details::getLocker(opCtx);
    Locker::LockSnapshot snapshot;
    locker->saveLockStateAndUnlock(&snapshot);

    // Track the number of yields in CurOp.
    CurOp::get(opCtx)->yielded();

    auto failPointHang = [opCtx, &ns](FailPoint* fp) {
        fp->executeIf(
            [fp](auto&&) {
                LOGV2(5180600, "Hanging index build during bulk load yield");
                fp->pauseWhileSet();
            },
            [opCtx, &ns](auto&& config) {
                return NamespaceStringUtil::parseFailPointData(config, "namespace") == ns;
            });
    };
    failPointHang(&hangDuringIndexBuildBulkLoadYield);
    failPointHang(&hangDuringIndexBuildBulkLoadYieldSecond);

    locker->restoreLockState(opCtx, snapshot);
    collection.restore();

    // After yielding, the latest instance of the collection is fetched and can be
    // different from the collection instance prior to yielding. For this reason we need
    // to refresh the index entry pointer.
    if (!collection) {
        return nullptr;
    }

    return collection->getIndexCatalog()
        ->findIndexByIdent(opCtx, indexIdent, IndexCatalog::InclusionPolicy::kUnfinished)
        ->getEntry();
}

class SortedDataIndexAccessMethod::BulkBuilderImpl final
    : public BulkBuilderCommon<SortedDataIndexAccessMethod::BulkBuilderImpl> {
public:
    using Sorter = mongo::Sorter<key_string::Value, mongo::NullValue>;

    BulkBuilderImpl(const IndexCatalogEntry* entry,
                    SortedDataIndexAccessMethod* iam,
                    size_t maxMemoryUsageBytes,
                    const DatabaseName& dbName);

    BulkBuilderImpl(const IndexCatalogEntry* entry,
                    SortedDataIndexAccessMethod* iam,
                    size_t maxMemoryUsageBytes,
                    const IndexStateInfo& stateInfo,
                    const DatabaseName& dbName);

    Status insert(OperationContext* opCtx,
                  const CollectionPtr& collection,
                  const IndexCatalogEntry* entry,
                  const BSONObj& obj,
                  const RecordId& loc,
                  const InsertDeleteOptions& options,
                  const OnSuppressedErrorFn& onSuppressedError = nullptr,
                  const ShouldRelaxConstraintsFn& shouldRelaxConstraints = nullptr) final;

    const MultikeyPaths& getMultikeyPaths() const final;

    bool isMultikey() const final;

    IndexStateInfo persistDataForShutdown() final;

    std::unique_ptr<Sorter::Iterator> finalizeSort();

    std::unique_ptr<SortedDataBuilderInterface> setUpBulkInserter(OperationContext* opCtx,
                                                                  const IndexCatalogEntry* entry,
                                                                  bool dupsAllowed);

    void debugEnsureSorted(const Sorter::Data& data);

    bool duplicateCheck(OperationContext* opCtx,
                        const IndexCatalogEntry* entry,
                        const Sorter::Data& data,
                        bool dupsAllowed,
                        const RecordIdHandlerFn& onDuplicateRecord);

    void insertKey(std::unique_ptr<SortedDataBuilderInterface>& inserter, const Sorter::Data& data);

    Status keyCommitted(const KeyHandlerFn& onDuplicateKeyInserted,
                        const Sorter::Data& data,
                        bool isDup);

private:
    void _insertMultikeyMetadataKeysIntoSorter();

    Sorter* _makeSorter(
        size_t maxMemoryUsageBytes,
        const DatabaseName& dbName,
        boost::optional<StringData> fileName = boost::none,
        const boost::optional<std::vector<SorterRange>>& ranges = boost::none) const;

    Sorter::Settings _makeSorterSettings() const;

    SortedDataIndexAccessMethod* _iam;
    std::unique_ptr<Sorter> _sorter;

    key_string::Value _previousKey;

    // Set to true if any document added to the BulkBuilder causes the index to become multikey.
    bool _isMultiKey = false;

    // Holds the path components that cause this index to be multikey. The '_indexMultikeyPaths'
    // vector remains empty if this index doesn't support path-level multikey tracking.
    MultikeyPaths _indexMultikeyPaths;

    // Caches the set of all multikey metadata keys generated during the bulk build process.
    // These are inserted into the sorter after all normal data keys have been added, just
    // before the bulk build is committed.
    KeyStringSet _multikeyMetadataKeys;
};

std::unique_ptr<IndexAccessMethod::BulkBuilder> SortedDataIndexAccessMethod::initiateBulk(
    const IndexCatalogEntry* entry,
    size_t maxMemoryUsageBytes,
    const boost::optional<IndexStateInfo>& stateInfo,
    const DatabaseName& dbName) {
    return stateInfo
        ? std::make_unique<BulkBuilderImpl>(entry, this, maxMemoryUsageBytes, *stateInfo, dbName)
        : std::make_unique<BulkBuilderImpl>(entry, this, maxMemoryUsageBytes, dbName);
}

SortedDataIndexAccessMethod::BulkBuilderImpl::BulkBuilderImpl(const IndexCatalogEntry* entry,
                                                              SortedDataIndexAccessMethod* iam,
                                                              size_t maxMemoryUsageBytes,
                                                              const DatabaseName& dbName)
    : BulkBuilderCommon(0,
                        "Index Build: inserting keys from external sorter into index",
                        entry->descriptor()->indexName()),
      _iam(iam),
      _sorter(_makeSorter(maxMemoryUsageBytes, dbName)) {
    countNewBuildInStats();
}

SortedDataIndexAccessMethod::BulkBuilderImpl::BulkBuilderImpl(const IndexCatalogEntry* entry,
                                                              SortedDataIndexAccessMethod* iam,
                                                              size_t maxMemoryUsageBytes,
                                                              const IndexStateInfo& stateInfo,
                                                              const DatabaseName& dbName)
    : BulkBuilderCommon(stateInfo.getNumKeys().value_or(0),
                        "Index Build: inserting keys from external sorter into index",
                        entry->descriptor()->indexName()),
      _iam(iam),
      _sorter(
          _makeSorter(maxMemoryUsageBytes, dbName, stateInfo.getFileName(), stateInfo.getRanges())),
      _isMultiKey(stateInfo.getIsMultikey()),
      _indexMultikeyPaths(createMultikeyPaths(stateInfo.getMultikeyPaths())) {
    countResumedBuildInStats();
}

Status SortedDataIndexAccessMethod::BulkBuilderImpl::insert(
    OperationContext* opCtx,
    const CollectionPtr& collection,
    const IndexCatalogEntry* entry,
    const BSONObj& obj,
    const RecordId& loc,
    const InsertDeleteOptions& options,
    const OnSuppressedErrorFn& onSuppressedError,
    const ShouldRelaxConstraintsFn& shouldRelaxConstraints) {
    auto& executionCtx = StorageExecutionContext::get(opCtx);

    auto keys = executionCtx.keys();
    auto multikeyPaths = executionCtx.multikeyPaths();

    try {
        _iam->getKeys(opCtx,
                      collection,
                      entry,
                      _sorter->memPool(),
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
        if (_indexMultikeyPaths.empty()) {
            _indexMultikeyPaths = *multikeyPaths;
        } else {
            invariant(_indexMultikeyPaths.size() == multikeyPaths->size());
            for (size_t i = 0; i < multikeyPaths->size(); ++i) {
                _indexMultikeyPaths[i].insert(boost::container::ordered_unique_range_t(),
                                              (*multikeyPaths)[i].begin(),
                                              (*multikeyPaths)[i].end());
            }
        }
    }

    for (const auto& keyString : *keys) {
        _sorter->add(keyString, mongo::NullValue());
        ++_keysInserted;
    }

    _isMultiKey = _isMultiKey ||
        _iam->shouldMarkIndexAsMultikey(keys->size(), _multikeyMetadataKeys, *multikeyPaths);

    return Status::OK();
}

const MultikeyPaths& SortedDataIndexAccessMethod::BulkBuilderImpl::getMultikeyPaths() const {
    return _indexMultikeyPaths;
}

bool SortedDataIndexAccessMethod::BulkBuilderImpl::isMultikey() const {
    return _isMultiKey;
}

IndexStateInfo SortedDataIndexAccessMethod::BulkBuilderImpl::persistDataForShutdown() {
    _insertMultikeyMetadataKeysIntoSorter();
    auto state = _sorter->persistDataForShutdown();

    IndexStateInfo stateInfo;
    stateInfo.setFileName(StringData(state.fileName));
    stateInfo.setNumKeys(_keysInserted);
    stateInfo.setRanges(std::move(state.ranges));

    return stateInfo;
}

void SortedDataIndexAccessMethod::BulkBuilderImpl::_insertMultikeyMetadataKeysIntoSorter() {
    for (const auto& keyString : _multikeyMetadataKeys) {
        _sorter->add(keyString, mongo::NullValue());
        ++_keysInserted;
    }

    // We clear the multikey metadata keys to prevent them from being inserted into the Sorter
    // twice in the case that done() is called and then persistDataForShutdown() is later called.
    _multikeyMetadataKeys.clear();
}

SortedDataIndexAccessMethod::BulkBuilderImpl::Sorter::Settings
SortedDataIndexAccessMethod::BulkBuilderImpl::_makeSorterSettings() const {
    return std::pair<key_string::Value::SorterDeserializeSettings,
                     mongo::NullValue::SorterDeserializeSettings>(
        {_iam->getSortedDataInterface()->getKeyStringVersion()}, {});
}

SortedDataIndexAccessMethod::BulkBuilderImpl::Sorter*
SortedDataIndexAccessMethod::BulkBuilderImpl::_makeSorter(
    size_t maxMemoryUsageBytes,
    const DatabaseName& dbName,
    boost::optional<StringData> fileName,
    const boost::optional<std::vector<SorterRange>>& ranges) const {
    return fileName
        ? Sorter::makeFromExistingRanges(
              fileName->toString(),
              *ranges,
              makeSortOptions(maxMemoryUsageBytes, dbName, bulkBuilderFileStats()),
              BtreeExternalSortComparison(),
              _makeSorterSettings())
        : Sorter::make(makeSortOptions(maxMemoryUsageBytes, dbName, bulkBuilderFileStats()),
                       BtreeExternalSortComparison(),
                       _makeSorterSettings());
}

std::unique_ptr<mongo::Sorter<key_string::Value, mongo::NullValue>::Iterator>
SortedDataIndexAccessMethod::BulkBuilderImpl::finalizeSort() {
    _insertMultikeyMetadataKeysIntoSorter();
    return std::unique_ptr<Sorter::Iterator>(_sorter->done());
}

std::unique_ptr<SortedDataBuilderInterface>
SortedDataIndexAccessMethod::BulkBuilderImpl::setUpBulkInserter(OperationContext* opCtx,
                                                                const IndexCatalogEntry* entry,
                                                                bool dupsAllowed) {
    _ns = entry->getNSSFromCatalog(opCtx);
    return _iam->getSortedDataInterface()->makeBulkBuilder(opCtx, dupsAllowed);
}


void SortedDataIndexAccessMethod::BulkBuilderImpl::debugEnsureSorted(const Sorter::Data& data) {
    if (data.first.compare(_previousKey) < 0) {
        LOGV2_FATAL_NOTRACE(31171,
                            "Expected the next key to be greater than or equal to the previous key",
                            "nextKey"_attr = data.first.toString(),
                            "previousKey"_attr = _previousKey.toString(),
                            "index"_attr = _indexName);
    }
}

bool SortedDataIndexAccessMethod::BulkBuilderImpl::duplicateCheck(
    OperationContext* opCtx,
    const IndexCatalogEntry* entry,
    const Sorter::Data& data,
    bool dupsAllowed,
    const RecordIdHandlerFn& onDuplicateRecord) {

    auto descriptor = entry->descriptor();

    bool isDup = false;
    if (descriptor->unique()) {
        int cmpData = (_iam->getSortedDataInterface()->rsKeyFormat() == KeyFormat::Long)
            ? data.first.compareWithoutRecordIdLong(_previousKey)
            : data.first.compareWithoutRecordIdStr(_previousKey);
        isDup = (cmpData == 0);
    }

    // Before attempting to insert, perform a duplicate key check.
    if (isDup && !dupsAllowed) {
        uassertStatusOK(_iam->_handleDuplicateKey(opCtx, entry, data.first, onDuplicateRecord));
    }
    return isDup;
}

void SortedDataIndexAccessMethod::BulkBuilderImpl::insertKey(
    std::unique_ptr<SortedDataBuilderInterface>& inserter, const Sorter::Data& data) {
    uassertStatusOK(inserter->addKey(data.first));
}

Status SortedDataIndexAccessMethod::BulkBuilderImpl::keyCommitted(
    const KeyHandlerFn& onDuplicateKeyInserted, const Sorter::Data& data, bool isDup) {
    _previousKey = data.first;

    if (isDup) {
        return onDuplicateKeyInserted(data.first);
    }
    return Status::OK();
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
            filter && filter->matchesBSON(obj)) {
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

/**
 * Generates a new file name on each call using a static, atomic and monotonically increasing
 * number. Each name is suffixed with a random number generated at startup, to prevent name
 * collisions when the index build external sort files are preserved across restarts.
 *
 * Each user of the Sorter must implement this function to ensure that all temporary files that the
 * Sorter instances produce are uniquely identified using a unique file name extension with separate
 * atomic variable. This is necessary because the sorter.cpp code is separately included in multiple
 * places, rather than compiled in one place and linked, and so cannot provide a globally unique ID.
 */
std::string nextFileName() {
    static AtomicWord<unsigned> indexAccessMethodFileCounter;
    static const uint64_t randomSuffix = SecureRandom().nextUInt64();
    return str::stream() << "extsort-index." << indexAccessMethodFileCounter.fetchAndAdd(1) << '-'
                         << randomSuffix;
}

Status SortedDataIndexAccessMethod::_handleDuplicateKey(
    OperationContext* opCtx,
    const IndexCatalogEntry* entry,
    const key_string::Value& dataKey,
    const RecordIdHandlerFn& onDuplicateRecord) {
    RecordId recordId = (KeyFormat::Long == _newInterface->rsKeyFormat())
        ? key_string::decodeRecordIdLongAtEnd(dataKey.getBuffer(), dataKey.getSize())
        : key_string::decodeRecordIdStrAtEnd(dataKey.getBuffer(), dataKey.getSize());
    if (onDuplicateRecord) {
        return onDuplicateRecord(recordId);
    }

    BSONObj dupKey = key_string::toBson(dataKey, getSortedDataInterface()->getOrdering());
    return buildDupKeyErrorStatus(dupKey.getOwned(),
                                  entry->getNSSFromCatalog(opCtx),
                                  entry->descriptor()->indexName(),
                                  entry->descriptor()->keyPattern(),
                                  entry->descriptor()->collation());
}

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
            if (!filter->matchesBSON(obj)) {
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
            if (!filter->matchesBSON(obj)) {
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

    int64_t removed = 0;
    Status status = removeKeys(opCtx, entry, keys, options, &removed);

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

#undef MONGO_LOGV2_DEFAULT_COMPONENT
#include "mongo/db/sorter/sorter.cpp"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kCommand
MONGO_CREATE_SORTER(mongo::key_string::Value, mongo::NullValue, mongo::BtreeExternalSortComparison);
