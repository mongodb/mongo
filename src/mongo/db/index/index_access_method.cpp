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


#include "mongo/platform/basic.h"

#include "mongo/db/index/btree_access_method.h"

#include <utility>
#include <vector>

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/db/catalog/index_catalog.h"
#include "mongo/db/catalog/index_consistency.h"
#include "mongo/db/client.h"
#include "mongo/db/commands/server_status.h"
#include "mongo/db/concurrency/exception_util.h"
#include "mongo/db/curop.h"
#include "mongo/db/index/index_build_interceptor.h"
#include "mongo/db/index/index_descriptor.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/keypattern.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/repl/timestamp_block.h"
#include "mongo/db/storage/execution_context.h"
#include "mongo/db/storage/storage_options.h"
#include "mongo/logv2/log.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/util/progress_meter.h"
#include "mongo/util/scopeguard.h"
#include "mongo/util/stacktrace.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kCommand

namespace mongo {

using std::pair;

using IndexVersion = IndexDescriptor::IndexVersion;

MONGO_FAIL_POINT_DEFINE(hangIndexBuildDuringBulkLoadPhase);
MONGO_FAIL_POINT_DEFINE(hangIndexBuildDuringBulkLoadPhaseSecond);
MONGO_FAIL_POINT_DEFINE(hangDuringIndexBuildBulkLoadYield);
MONGO_FAIL_POINT_DEFINE(hangDuringIndexBuildBulkLoadYieldSecond);

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
    IndexBulkBuilderSSS() : ServerStatusSection("indexBulkBuilder") {}

    bool includeByDefault() const final {
        return true;
    }

    void addRequiredPrivileges(std::vector<Privilege>* out) final {}

    BSONObj generateSection(OperationContext* opCtx, const BSONElement& configElement) const final {
        BSONObjBuilder builder;
        builder.append("count", count.loadRelaxed());
        builder.append("resumed", resumed.loadRelaxed());
        builder.append("filesOpenedForExternalSort", sorterFileStats.opened.loadRelaxed());
        builder.append("filesClosedForExternalSort", sorterFileStats.closed.loadRelaxed());
        builder.append("spilledRanges", sorterTracker.spilledRanges.loadRelaxed());
        builder.append("bytesSpilled", sorterTracker.bytesSpilled.loadRelaxed());
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

} indexBulkBuilderSSS;

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

SortOptions makeSortOptions(size_t maxMemoryUsageBytes, StringData dbName, SorterFileStats* stats) {
    return SortOptions()
        .TempDir(storageGlobalParams.dbpath + "/_tmp")
        .ExtSortAllowed()
        .MaxMemoryUsageBytes(maxMemoryUsageBytes)
        .FileStats(stats)
        .Tracker(&indexBulkBuilderSSS.sorterTracker)
        .DBName(dbName.toString());
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
    typedef std::pair<KeyString::Value, mongo::NullValue> Data;
    int operator()(const Data& l, const Data& r) const {
        return l.first.compare(r.first);
    }
};

SortedDataIndexAccessMethod::SortedDataIndexAccessMethod(const IndexCatalogEntry* btreeState,
                                                         std::unique_ptr<SortedDataInterface> btree)
    : _indexCatalogEntry(btreeState),
      _descriptor(btreeState->descriptor()),
      _newInterface(std::move(btree)) {
    verify(IndexDescriptor::isIndexVersionSupported(_descriptor->version()));
}

Status SortedDataIndexAccessMethod::insert(OperationContext* opCtx,
                                           SharedBufferFragmentBuilder& pooledBuilder,
                                           const CollectionPtr& coll,
                                           const std::vector<BsonRecord>& bsonRecords,
                                           const InsertDeleteOptions& options,
                                           int64_t* numInserted) {
    for (auto bsonRecord : bsonRecords) {
        invariant(bsonRecord.id != RecordId());

        if (!bsonRecord.ts.isNull()) {
            Status status = opCtx->recoveryUnit()->setTimestamp(bsonRecord.ts);
            if (!status.isOK())
                return status;
        }

        auto& executionCtx = StorageExecutionContext::get(opCtx);
        auto keys = executionCtx.keys();
        auto multikeyMetadataKeys = executionCtx.multikeyMetadataKeys();
        auto multikeyPaths = executionCtx.multikeyPaths();

        getKeys(opCtx,
                coll,
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
            pooledBuilder,
            obj,
            InsertDeleteOptions::ConstraintEnforcementMode::kRelaxConstraintsUnfiltered,
            GetKeysContext::kRemovingKeys,
            keys.get(),
            nullptr,
            nullptr,
            loc);

    _unindexKeysOrWriteToSideTable(
        opCtx, coll->ns(), *keys, obj, logIfError, numDeleted, options, checkRecordId);
}

Status SortedDataIndexAccessMethod::update(OperationContext* opCtx,
                                           SharedBufferFragmentBuilder& pooledBufferBuilder,
                                           const BSONObj& oldDoc,
                                           const BSONObj& newDoc,
                                           const RecordId& loc,
                                           const CollectionPtr& coll,
                                           const InsertDeleteOptions& options,
                                           int64_t* numInserted,
                                           int64_t* numDeleted) {

    UpdateTicket updateTicket;
    prepareUpdate(opCtx, coll, oldDoc, newDoc, loc, options, &updateTicket);

    auto status = Status::OK();
    if (_indexCatalogEntry->isHybridBuilding() || !_indexCatalogEntry->isReady(opCtx)) {
        bool logIfError = false;
        _unindexKeysOrWriteToSideTable(opCtx,
                                       coll->ns(),
                                       updateTicket.removed,
                                       oldDoc,
                                       logIfError,
                                       numDeleted,
                                       options,
                                       CheckRecordId::Off);
        return _indexKeysOrWriteToSideTable(opCtx,
                                            coll,
                                            updateTicket.added,
                                            updateTicket.newMultikeyMetadataKeys,
                                            updateTicket.newMultikeyPaths,
                                            newDoc,
                                            options,
                                            numInserted);
    } else {
        return doUpdate(opCtx, coll, updateTicket, numInserted, numDeleted);
    }
}

Status SortedDataIndexAccessMethod::insertKeysAndUpdateMultikeyPaths(
    OperationContext* opCtx,
    const CollectionPtr& coll,
    const KeyStringSet& keys,
    const KeyStringSet& multikeyMetadataKeys,
    const MultikeyPaths& multikeyPaths,
    const InsertDeleteOptions& options,
    KeyHandlerFn&& onDuplicateKey,
    int64_t* numInserted) {
    // Insert the specified data keys into the index.
    auto status = insertKeys(opCtx, coll, keys, options, std::move(onDuplicateKey), numInserted);
    if (!status.isOK()) {
        return status;
    }
    // If these keys should cause the index to become multikey, pass them into the catalog.
    if (shouldMarkIndexAsMultikey(keys.size(), multikeyMetadataKeys, multikeyPaths)) {
        _indexCatalogEntry->setMultikey(opCtx, coll, multikeyMetadataKeys, multikeyPaths);
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
                                               const KeyStringSet& keys,
                                               const InsertDeleteOptions& options,
                                               KeyHandlerFn&& onDuplicateKey,
                                               int64_t* numInserted) {
    // Initialize the 'numInserted' out-parameter to zero in case the caller did not already do so.
    if (numInserted) {
        *numInserted = 0;
    }
    bool unique = _descriptor->unique();
    bool prepareUnique = _descriptor->prepareUnique();
    bool dupsAllowed;
    if (!_descriptor->isIdIndex() && !opCtx->isEnforcingConstraints() &&
        coll->isIndexReady(_descriptor->indexName())) {
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
        dupsAllowed = !coll->isIndexReady(_descriptor->indexName());
    } else {
        dupsAllowed = !unique;
    }
    // Add all new keys into the index. The RecordId for each is already encoded in the KeyString.
    for (const auto& keyString : keys) {
        auto status = _newInterface->insert(opCtx, keyString, dupsAllowed);

        // When duplicates are encountered and allowed, retry with dupsAllowed. Call
        // onDuplicateKey() with the inserted duplicate key.
        if (ErrorCodes::DuplicateKey == status.code() && options.dupsAllowed && !prepareUnique) {
            invariant(unique);

            status = _newInterface->insert(opCtx, keyString, true /* dupsAllowed */);
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
                                               const KeyString::Value& keyString,
                                               bool dupsAllowed) {

    try {
        _newInterface->unindex(opCtx, keyString, dupsAllowed);
    } catch (AssertionException& e) {
        NamespaceString ns = _indexCatalogEntry->getNSSFromCatalog(opCtx);
        LOGV2(20683,
              "Assertion failure: _unindex failed on: {namespace} for index: {indexName}. "
              "{error}  KeyString:{keyString}",
              "Assertion failure: _unindex failed",
              "error"_attr = redact(e),
              "keyString"_attr = keyString,
              "namespace"_attr = ns,
              "indexName"_attr = _descriptor->indexName());
        printStackTrace();
    }
}

std::unique_ptr<SortedDataInterface::Cursor> SortedDataIndexAccessMethod::newCursor(
    OperationContext* opCtx, bool isForward) const {
    return _newInterface->newCursor(opCtx, isForward);
}

Status SortedDataIndexAccessMethod::removeKeys(OperationContext* opCtx,
                                               const KeyStringSet& keys,
                                               const InsertDeleteOptions& options,
                                               int64_t* numDeleted) {

    for (const auto& key : keys) {
        removeOneKey(opCtx, key, options.dupsAllowed);
    }

    *numDeleted = keys.size();
    return Status::OK();
}

Status SortedDataIndexAccessMethod::initializeAsEmpty(OperationContext* opCtx) {
    return _newInterface->initAsEmpty(opCtx);
}

RecordId SortedDataIndexAccessMethod::findSingle(OperationContext* opCtx,
                                                 const CollectionPtr& collection,
                                                 const BSONObj& requestedKey) const {
    // Generate the key for this index.
    KeyString::Value actualKey = [&]() {
        if (_indexCatalogEntry->getCollator()) {
            // For performance, call get keys only if there is a non-simple collation.
            SharedBufferFragmentBuilder pooledBuilder(
                KeyString::HeapBuilder::kHeapAllocatorDefaultBytes);
            auto& executionCtx = StorageExecutionContext::get(opCtx);
            auto keys = executionCtx.keys();
            KeyStringSet* multikeyMetadataKeys = nullptr;
            MultikeyPaths* multikeyPaths = nullptr;

            getKeys(opCtx,
                    collection,
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
            KeyString::HeapBuilder requestedKeyString(
                getSortedDataInterface()->getKeyStringVersion(),
                BSONObj::stripFieldNames(requestedKey),
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

void SortedDataIndexAccessMethod::validate(OperationContext* opCtx,
                                           int64_t* numKeys,
                                           IndexValidateResults* fullResults) const {
    long long keys = 0;
    _newInterface->fullValidate(opCtx, &keys, fullResults);
    *numKeys = keys;
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
                                                const BSONObj& from,
                                                const BSONObj& to,
                                                const RecordId& record,
                                                const InsertDeleteOptions& options,
                                                UpdateTicket* ticket) const {
    SharedBufferFragmentBuilder pooledBuilder(KeyString::HeapBuilder::kHeapAllocatorDefaultBytes);
    const MatchExpression* indexFilter = _indexCatalogEntry->getFilterExpression();
    if (!indexFilter || indexFilter->matchesBSON(from)) {
        // Override key constraints when generating keys for removal. This only applies to keys
        // that do not apply to a partial filter expression.
        const auto getKeysMode = _indexCatalogEntry->isHybridBuilding()
            ? InsertDeleteOptions::ConstraintEnforcementMode::kRelaxConstraintsUnfiltered
            : options.getKeysMode;

        // There's no need to compute the prefixes of the indexed fields that possibly caused the
        // index to be multikey when the old version of the document was written since the index
        // metadata isn't updated when keys are deleted.
        getKeys(opCtx,
                collection,
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
                                             const UpdateTicket& ticket,
                                             int64_t* numInserted,
                                             int64_t* numDeleted) {
    invariant(!_indexCatalogEntry->isHybridBuilding());
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
        bool dupsAllowed = !_descriptor->prepareUnique() && ticket.dupsAllowed;
        auto status = _newInterface->insert(opCtx, keyString, dupsAllowed);
        if (!status.isOK())
            return status;
    }

    // If these keys should cause the index to become multikey, pass them into the catalog.
    if (shouldMarkIndexAsMultikey(
            ticket.newKeys.size(), ticket.newMultikeyMetadataKeys, ticket.newMultikeyPaths)) {
        _indexCatalogEntry->setMultikey(
            opCtx, coll, ticket.newMultikeyMetadataKeys, ticket.newMultikeyPaths);
    }

    // If we have some multikey metadata keys, they should have been added while marking the index
    // as multikey in the catalog. Add them to the count of keys inserted for completeness.
    *numInserted = ticket.added.size() + ticket.newMultikeyMetadataKeys.size();
    *numDeleted = ticket.removed.size();

    return Status::OK();
}

Status SortedDataIndexAccessMethod::compact(OperationContext* opCtx) {
    return this->_newInterface->compact(opCtx);
}

Ident* SortedDataIndexAccessMethod::getIdentPtr() const {
    return this->_newInterface.get();
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

void IndexAccessMethod::BulkBuilder::yield(OperationContext* opCtx,
                                           const Yieldable* yieldable,
                                           const NamespaceString& ns) {
    // Releasing locks means a new snapshot should be acquired when restored.
    opCtx->recoveryUnit()->abandonSnapshot();
    yieldable->yield();

    auto locker = opCtx->lockState();
    Locker::LockSnapshot snapshot;
    if (locker->saveLockStateAndUnlock(&snapshot)) {

        // Track the number of yields in CurOp.
        CurOp::get(opCtx)->yielded();

        auto failPointHang = [opCtx, &ns](FailPoint* fp) {
            fp->executeIf(
                [fp](auto&&) {
                    LOGV2(5180600, "Hanging index build during bulk load yield");
                    fp->pauseWhileSet();
                },
                [opCtx, &ns](auto&& config) {
                    return config.getStringField("namespace") == ns.ns();
                });
        };
        failPointHang(&hangDuringIndexBuildBulkLoadYield);
        failPointHang(&hangDuringIndexBuildBulkLoadYieldSecond);

        locker->restoreLockState(opCtx, snapshot);
    }
    yieldable->restore();
}

class SortedDataIndexAccessMethod::BulkBuilderImpl final : public IndexAccessMethod::BulkBuilder {
public:
    using Sorter = mongo::Sorter<KeyString::Value, mongo::NullValue>;

    BulkBuilderImpl(SortedDataIndexAccessMethod* iam,
                    size_t maxMemoryUsageBytes,
                    StringData dbName);

    BulkBuilderImpl(SortedDataIndexAccessMethod* iam,
                    size_t maxMemoryUsageBytes,
                    const IndexStateInfo& stateInfo,
                    StringData dbName);

    Status insert(OperationContext* opCtx,
                  const CollectionPtr& collection,
                  SharedBufferFragmentBuilder& pooledBuilder,
                  const BSONObj& obj,
                  const RecordId& loc,
                  const InsertDeleteOptions& options,
                  const std::function<void()>& saveCursorBeforeWrite,
                  const std::function<void()>& restoreCursorAfterWrite) final;

    Status commit(OperationContext* opCtx,
                  const CollectionPtr& collection,
                  bool dupsAllowed,
                  int32_t yieldIterations,
                  const KeyHandlerFn& onDuplicateKeyInserted,
                  const RecordIdHandlerFn& onDuplicateRecord) final;

    const MultikeyPaths& getMultikeyPaths() const final;

    bool isMultikey() const final;

    IndexStateInfo persistDataForShutdown() final;

private:
    void _insertMultikeyMetadataKeysIntoSorter();

    Sorter* _makeSorter(
        size_t maxMemoryUsageBytes,
        StringData dbName,
        boost::optional<StringData> fileName = boost::none,
        const boost::optional<std::vector<SorterRange>>& ranges = boost::none) const;

    Sorter::Settings _makeSorterSettings() const;

    SortedDataIndexAccessMethod* _iam;
    std::unique_ptr<Sorter> _sorter;
    int64_t _keysInserted = 0;

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
    size_t maxMemoryUsageBytes,
    const boost::optional<IndexStateInfo>& stateInfo,
    StringData dbName) {
    return stateInfo
        ? std::make_unique<BulkBuilderImpl>(this, maxMemoryUsageBytes, *stateInfo, dbName)
        : std::make_unique<BulkBuilderImpl>(this, maxMemoryUsageBytes, dbName);
}

SortedDataIndexAccessMethod::BulkBuilderImpl::BulkBuilderImpl(SortedDataIndexAccessMethod* iam,
                                                              size_t maxMemoryUsageBytes,
                                                              StringData dbName)
    : _iam(iam), _sorter(_makeSorter(maxMemoryUsageBytes, dbName)) {
    countNewBuildInStats();
}

SortedDataIndexAccessMethod::BulkBuilderImpl::BulkBuilderImpl(SortedDataIndexAccessMethod* iam,
                                                              size_t maxMemoryUsageBytes,
                                                              const IndexStateInfo& stateInfo,
                                                              StringData dbName)
    : _iam(iam),
      _sorter(
          _makeSorter(maxMemoryUsageBytes, dbName, stateInfo.getFileName(), stateInfo.getRanges())),
      _keysInserted(stateInfo.getNumKeys().value_or(0)),
      _isMultiKey(stateInfo.getIsMultikey()),
      _indexMultikeyPaths(createMultikeyPaths(stateInfo.getMultikeyPaths())) {
    countResumedBuildInStats();
}

Status SortedDataIndexAccessMethod::BulkBuilderImpl::insert(
    OperationContext* opCtx,
    const CollectionPtr& collection,
    SharedBufferFragmentBuilder& pooledBuilder,
    const BSONObj& obj,
    const RecordId& loc,
    const InsertDeleteOptions& options,
    const std::function<void()>& saveCursorBeforeWrite,
    const std::function<void()>& restoreCursorAfterWrite) {
    auto& executionCtx = StorageExecutionContext::get(opCtx);

    auto keys = executionCtx.keys();
    auto multikeyPaths = executionCtx.multikeyPaths();

    try {
        _iam->getKeys(opCtx,
                      collection,
                      pooledBuilder,
                      obj,
                      options.getKeysMode,
                      GetKeysContext::kAddingKeys,
                      keys.get(),
                      &_multikeyMetadataKeys,
                      multikeyPaths.get(),
                      loc,
                      [&](Status status, const BSONObj&, const boost::optional<RecordId>&) {
                          // If a key generation error was suppressed, record the document as
                          // "skipped" so the index builder can retry at a point when data is
                          // consistent.
                          auto interceptor = _iam->_indexCatalogEntry->indexBuildInterceptor();
                          if (interceptor && interceptor->getSkippedRecordTracker()) {
                              LOGV2_DEBUG(
                                  20684,
                                  1,
                                  "Recording suppressed key generation error to retry later: "
                                  "{error} on {loc}: {obj}",
                                  "error"_attr = status,
                                  "loc"_attr = loc,
                                  "obj"_attr = redact(obj));

                              // Save and restore the cursor around the write in case it throws a
                              // WCE internally and causes the cursor to be unpositioned.
                              saveCursorBeforeWrite();
                              interceptor->getSkippedRecordTracker()->record(opCtx, loc);
                              restoreCursorAfterWrite();
                          }
                      });
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
    return std::pair<KeyString::Value::SorterDeserializeSettings,
                     mongo::NullValue::SorterDeserializeSettings>(
        {_iam->getSortedDataInterface()->getKeyStringVersion()}, {});
}

SortedDataIndexAccessMethod::BulkBuilderImpl::Sorter*
SortedDataIndexAccessMethod::BulkBuilderImpl::_makeSorter(
    size_t maxMemoryUsageBytes,
    StringData dbName,
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

Status SortedDataIndexAccessMethod::BulkBuilderImpl::commit(
    OperationContext* opCtx,
    const CollectionPtr& collection,
    bool dupsAllowed,
    int32_t yieldIterations,
    const KeyHandlerFn& onDuplicateKeyInserted,
    const RecordIdHandlerFn& onDuplicateRecord) {

    Timer timer;

    const auto descriptor = _iam->_descriptor;
    auto ns = _iam->_indexCatalogEntry->getNSSFromCatalog(opCtx);

    _insertMultikeyMetadataKeysIntoSorter();
    std::unique_ptr<Sorter::Iterator> it(_sorter->done());

    static constexpr char message[] = "Index Build: inserting keys from external sorter into index";
    ProgressMeterHolder pm;
    {
        stdx::unique_lock<Client> lk(*opCtx->getClient());
        pm.set(
            CurOp::get(opCtx)->setProgress_inlock(message, _keysInserted, 3 /* secondsBetween */));
    }

    auto builder = _iam->getSortedDataInterface()->makeBulkBuilder(opCtx, dupsAllowed);

    KeyString::Value previousKey;

    for (int64_t i = 0; it->more(); i++) {
        opCtx->checkForInterrupt();

        auto failPointHang = [opCtx, i, &indexName = descriptor->indexName()](FailPoint* fp) {
            fp->executeIf(
                [fp, opCtx, i, &indexName](const BSONObj& data) {
                    LOGV2(4924400,
                          "Hanging index build during bulk load phase",
                          "iteration"_attr = i,
                          "index"_attr = indexName);

                    fp->pauseWhileSet(opCtx);
                },
                [i, &indexName](const BSONObj& data) {
                    auto indexNames = data.getObjectField("indexNames");
                    return i == data["iteration"].numberLong() &&
                        std::any_of(indexNames.begin(),
                                    indexNames.end(),
                                    [&indexName](const auto& elem) {
                                        return indexName == elem.String();
                                    });
                });
        };
        failPointHang(&hangIndexBuildDuringBulkLoadPhase);
        failPointHang(&hangIndexBuildDuringBulkLoadPhaseSecond);

        // Get the next datum and add it to the builder.
        Sorter::Data data = it->next();

        // Assert that keys are retrieved from the sorter in non-decreasing order, but only in debug
        // builds since this check can be expensive.
        int cmpData;
        if (descriptor->unique()) {
            cmpData = (_iam->getSortedDataInterface()->rsKeyFormat() == KeyFormat::Long)
                ? data.first.compareWithoutRecordIdLong(previousKey)
                : data.first.compareWithoutRecordIdStr(previousKey);
        }

        if (kDebugBuild && data.first.compare(previousKey) < 0) {
            LOGV2_FATAL_NOTRACE(
                31171,
                "Expected the next key to be greater than or equal to the previous key",
                "nextKey"_attr = data.first.toString(),
                "previousKey"_attr = previousKey.toString(),
                "index"_attr = descriptor->indexName());
        }

        // Before attempting to insert, perform a duplicate key check.
        bool isDup = (descriptor->unique()) ? (cmpData == 0) : false;
        if (isDup && !dupsAllowed) {
            Status status = _iam->_handleDuplicateKey(opCtx, data.first, onDuplicateRecord);
            if (!status.isOK()) {
                return status;
            }
            continue;
        }

        Status status = writeConflictRetry(opCtx, "addingKey", ns.ns(), [&] {
            WriteUnitOfWork wunit(opCtx);
            Status status = builder->addKey(data.first);
            if (!status.isOK()) {
                return status;
            }

            wunit.commit();
            return Status::OK();
        });

        if (!status.isOK()) {
            // Duplicates are checked before inserting.
            invariant(status.code() != ErrorCodes::DuplicateKey);
            return status;
        }

        previousKey = data.first;

        if (isDup) {
            status = onDuplicateKeyInserted(data.first);
            if (!status.isOK())
                return status;
        }

        // Starts yielding locks after the first non-zero 'yieldIterations' inserts.
        if (yieldIterations && (i + 1) % yieldIterations == 0) {
            yield(opCtx, &collection, ns);
        }

        // If we're here either it's a dup and we're cool with it or the addKey went just fine.
        pm.hit();
    }

    pm.finished();

    LOGV2(20685,
          "Index build: inserted {bulk_getKeysInserted} keys from external sorter into index in "
          "{timer_seconds} seconds",
          "Index build: inserted keys from external sorter into index",
          logAttrs(ns),
          "index"_attr = descriptor->indexName(),
          "keysInserted"_attr = _keysInserted,
          "duration"_attr = Milliseconds(Seconds(timer.seconds())));
    return Status::OK();
}

void SortedDataIndexAccessMethod::getKeys(OperationContext* opCtx,
                                          const CollectionPtr& collection,
                                          SharedBufferFragmentBuilder& pooledBufferBuilder,
                                          const BSONObj& obj,
                                          InsertDeleteOptions::ConstraintEnforcementMode mode,
                                          GetKeysContext context,
                                          KeyStringSet* keys,
                                          KeyStringSet* multikeyMetadataKeys,
                                          MultikeyPaths* multikeyPaths,
                                          const boost::optional<RecordId>& id,
                                          OnSuppressedErrorFn&& onSuppressedError) const {
    invariant(!id || _newInterface->rsKeyFormat() != KeyFormat::String || id->isStr(),
              fmt::format("RecordId is not in the same string format as its RecordStore; id: {}",
                          id->toString()));
    invariant(!id || _newInterface->rsKeyFormat() != KeyFormat::Long || id->isLong(),
              fmt::format("RecordId is not in the same long format as its RecordStore; id: {}",
                          id->toString()));

    try {
        if (_indexCatalogEntry->shouldValidateDocument()) {
            validateDocument(collection, obj, _descriptor->keyPattern());
        }
        doGetKeys(opCtx,
                  collection,
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

        if (ex.isA<ErrorCategory::Interruption>() || ex.isA<ErrorCategory::ShutdownError>()) {
            throw;
        }

        // If the document applies to the filter (which means that it should have never been
        // indexed), do not suppress the error.
        const MatchExpression* filter = _indexCatalogEntry->getFilterExpression();
        if (mode == InsertDeleteOptions::ConstraintEnforcementMode::kRelaxConstraintsUnfiltered &&
            filter && filter->matchesBSON(obj)) {
            throw;
        }

        if (onSuppressedError) {
            onSuppressedError(ex.toStatus(), obj, id);
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
    static const int64_t randomSuffix = SecureRandom().nextInt64();
    return str::stream() << "extsort-index." << indexAccessMethodFileCounter.fetchAndAdd(1) << '-'
                         << randomSuffix;
}

Status SortedDataIndexAccessMethod::_handleDuplicateKey(
    OperationContext* opCtx,
    const KeyString::Value& dataKey,
    const RecordIdHandlerFn& onDuplicateRecord) {
    RecordId recordId = (KeyFormat::Long == _newInterface->rsKeyFormat())
        ? KeyString::decodeRecordIdLongAtEnd(dataKey.getBuffer(), dataKey.getSize())
        : KeyString::decodeRecordIdStrAtEnd(dataKey.getBuffer(), dataKey.getSize());
    if (onDuplicateRecord) {
        return onDuplicateRecord(recordId);
    }

    BSONObj dupKey = KeyString::toBson(dataKey, getSortedDataInterface()->getOrdering());
    return buildDupKeyErrorStatus(dupKey.getOwned(),
                                  _indexCatalogEntry->getNSSFromCatalog(opCtx),
                                  _descriptor->indexName(),
                                  _descriptor->keyPattern(),
                                  _descriptor->collation());
}

Status SortedDataIndexAccessMethod::_indexKeysOrWriteToSideTable(
    OperationContext* opCtx,
    const CollectionPtr& coll,
    const KeyStringSet& keys,
    const KeyStringSet& multikeyMetadataKeys,
    const MultikeyPaths& multikeyPaths,
    const BSONObj& obj,
    const InsertDeleteOptions& options,
    int64_t* keysInsertedOut) {
    Status status = Status::OK();
    if (_indexCatalogEntry->isHybridBuilding()) {
        // The side table interface accepts only records that meet the criteria for this partial
        // index.
        // See SERVER-28975 and SERVER-39705 for details.
        if (auto filter = _indexCatalogEntry->getFilterExpression()) {
            if (!filter->matchesBSON(obj)) {
                return Status::OK();
            }
        }

        int64_t inserted = 0;
        status = _indexCatalogEntry->indexBuildInterceptor()->sideWrite(
            opCtx,
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
    const KeyStringSet& keys,
    const BSONObj& obj,
    bool logIfError,
    int64_t* const keysDeletedOut,
    InsertDeleteOptions options,  // copy!
    CheckRecordId checkRecordId) {

    options.logIfError = logIfError;

    if (_indexCatalogEntry->isHybridBuilding()) {
        // The side table interface accepts only records that meet the criteria for this partial
        // index.
        // See SERVER-28975 and SERVER-39705 for details.
        if (auto filter = _indexCatalogEntry->getFilterExpression()) {
            if (!filter->matchesBSON(obj)) {
                return;
            }
        }

        int64_t removed = 0;
        fassert(31155,
                _indexCatalogEntry->indexBuildInterceptor()->sideWrite(
                    opCtx, keys, {}, {}, IndexBuildInterceptor::Op::kDelete, &removed));
        if (keysDeletedOut) {
            *keysDeletedOut += removed;
        }

        return;
    }

    // On WiredTiger, we do blind unindexing of records for efficiency.  However, when duplicates
    // are allowed in unique indexes, WiredTiger does not do blind unindexing, and instead confirms
    // that the recordid matches the element we are removing.
    //
    // We need to disable blind-deletes if 'checkRecordId' is explicitly set 'On', or for
    // in-progress indexes, in order to force recordid-matching for unindex operations, since
    // initial sync can build an index over a collection with duplicates. See SERVER-17487 for more
    // details.
    options.dupsAllowed = options.dupsAllowed || !_indexCatalogEntry->isReady(opCtx) ||
        (checkRecordId == CheckRecordId::On);

    int64_t removed = 0;
    Status status = removeKeys(opCtx, keys, options, &removed);

    if (!status.isOK()) {
        LOGV2(20362,
              "Couldn't unindex record {obj} from collection {namespace}: {error}",
              "Couldn't unindex record",
              "record"_attr = redact(obj),
              "namespace"_attr = ns,
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
MONGO_CREATE_SORTER(mongo::KeyString::Value, mongo::NullValue, mongo::BtreeExternalSortComparison);
