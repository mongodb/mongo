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

#include "mongo/db/index/btree_access_method.h"

#include <utility>
#include <vector>

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/db/catalog/index_catalog.h"
#include "mongo/db/catalog/index_consistency.h"
#include "mongo/db/client.h"
#include "mongo/db/concurrency/write_conflict_exception.h"
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
#include "mongo/util/progress_meter.h"
#include "mongo/util/scopeguard.h"
#include "mongo/util/stacktrace.h"

namespace mongo {

using std::pair;
using std::set;

using IndexVersion = IndexDescriptor::IndexVersion;

MONGO_FAIL_POINT_DEFINE(hangIndexBuildDuringBulkLoadPhase);
MONGO_FAIL_POINT_DEFINE(hangIndexBuildDuringBulkLoadPhaseSecond);
MONGO_FAIL_POINT_DEFINE(hangDuringIndexBuildBulkLoadYield);
MONGO_FAIL_POINT_DEFINE(hangDuringIndexBuildBulkLoadYieldSecond);

namespace {

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

SortOptions makeSortOptions(size_t maxMemoryUsageBytes, StringData dbName) {
    return SortOptions()
        .TempDir(storageGlobalParams.dbpath + "/_tmp")
        .ExtSortAllowed()
        .MaxMemoryUsageBytes(maxMemoryUsageBytes)
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

AbstractIndexAccessMethod::AbstractIndexAccessMethod(const IndexCatalogEntry* btreeState,
                                                     std::unique_ptr<SortedDataInterface> btree)
    : _indexCatalogEntry(btreeState),
      _descriptor(btreeState->descriptor()),
      _newInterface(std::move(btree)) {
    verify(IndexDescriptor::isIndexVersionSupported(_descriptor->version()));
}

// Find the keys for obj, put them in the tree pointing to loc.
Status AbstractIndexAccessMethod::insert(OperationContext* opCtx,
                                         SharedBufferFragmentBuilder& pooledBufferBuilder,
                                         const CollectionPtr& coll,
                                         const BSONObj& obj,
                                         const RecordId& loc,
                                         const InsertDeleteOptions& options,
                                         KeyHandlerFn&& onDuplicateKey,
                                         int64_t* numInserted) {
    invariant(options.fromIndexBuilder || !_indexCatalogEntry->isHybridBuilding());

    auto& executionCtx = StorageExecutionContext::get(opCtx);

    auto keys = executionCtx.keys();
    auto multikeyMetadataKeys = executionCtx.multikeyMetadataKeys();
    auto multikeyPaths = executionCtx.multikeyPaths();

    getKeys(opCtx,
            coll,
            pooledBufferBuilder,
            obj,
            options.getKeysMode,
            GetKeysContext::kAddingKeys,
            keys.get(),
            multikeyMetadataKeys.get(),
            multikeyPaths.get(),
            loc,
            kNoopOnSuppressedErrorFn);

    return insertKeysAndUpdateMultikeyPaths(opCtx,
                                            coll,
                                            *keys,
                                            *multikeyMetadataKeys,
                                            *multikeyPaths,
                                            loc,
                                            options,
                                            std::move(onDuplicateKey),
                                            numInserted);
}

Status AbstractIndexAccessMethod::insertKeysAndUpdateMultikeyPaths(
    OperationContext* opCtx,
    const CollectionPtr& coll,
    const KeyStringSet& keys,
    const KeyStringSet& multikeyMetadataKeys,
    const MultikeyPaths& multikeyPaths,
    const RecordId& loc,
    const InsertDeleteOptions& options,
    KeyHandlerFn&& onDuplicateKey,
    int64_t* numInserted) {
    // Insert the specified data keys into the index.
    auto status =
        insertKeys(opCtx, coll, keys, loc, options, std::move(onDuplicateKey), numInserted);
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

Status AbstractIndexAccessMethod::insertKeys(OperationContext* opCtx,
                                             const CollectionPtr& coll,
                                             const KeyStringSet& keys,
                                             const RecordId& loc,
                                             const InsertDeleteOptions& options,
                                             KeyHandlerFn&& onDuplicateKey,
                                             int64_t* numInserted) {
    // Initialize the 'numInserted' out-parameter to zero in case the caller did not already do so.
    if (numInserted) {
        *numInserted = 0;
    }
    // Add all new keys into the index. The RecordId for each is already encoded in the KeyString.
    for (const auto& keyString : keys) {
        bool unique = _descriptor->unique();
        Status status = _newInterface->insert(opCtx, keyString, !unique /* dupsAllowed */);

        // When duplicates are encountered and allowed, retry with dupsAllowed. Call
        // onDuplicateKey() with the inserted duplicate key.
        if (ErrorCodes::DuplicateKey == status.code() && options.dupsAllowed) {
            invariant(unique);
            status = _newInterface->insert(opCtx, keyString, true /* dupsAllowed */);

            if (status.isOK() && onDuplicateKey)
                status = onDuplicateKey(keyString);
        }
        if (!status.isOK())
            return status;
    }
    if (numInserted) {
        *numInserted = keys.size();
    }
    return Status::OK();
}

void AbstractIndexAccessMethod::removeOneKey(OperationContext* opCtx,
                                             const KeyString::Value& keyString,
                                             const RecordId& loc,
                                             bool dupsAllowed) {

    try {
        _newInterface->unindex(opCtx, keyString, dupsAllowed);
    } catch (AssertionException& e) {
        NamespaceString ns = _indexCatalogEntry->getNSSFromCatalog(opCtx);
        LOGV2(20683,
              "Assertion failure: _unindex failed on: {namespace} for index: {indexName}. "
              "{error}  KeyString:{keyString}  dl:{recordId}",
              "Assertion failure: _unindex failed",
              "error"_attr = redact(e),
              "keyString"_attr = keyString,
              "recordId"_attr = loc,
              "namespace"_attr = ns,
              "indexName"_attr = _descriptor->indexName());
        printStackTrace();
    }
}

std::unique_ptr<SortedDataInterface::Cursor> AbstractIndexAccessMethod::newCursor(
    OperationContext* opCtx, bool isForward) const {
    return _newInterface->newCursor(opCtx, isForward);
}

std::unique_ptr<SortedDataInterface::Cursor> AbstractIndexAccessMethod::newCursor(
    OperationContext* opCtx) const {
    return newCursor(opCtx, true);
}

Status AbstractIndexAccessMethod::removeKeys(OperationContext* opCtx,
                                             const KeyStringSet& keys,
                                             const RecordId& loc,
                                             const InsertDeleteOptions& options,
                                             int64_t* numDeleted) {

    for (const auto& key : keys) {
        removeOneKey(opCtx, key, loc, options.dupsAllowed);
    }

    *numDeleted = keys.size();
    return Status::OK();
}

Status AbstractIndexAccessMethod::initializeAsEmpty(OperationContext* opCtx) {
    return _newInterface->initAsEmpty(opCtx);
}

RecordId AbstractIndexAccessMethod::findSingle(OperationContext* opCtx,
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
                    GetKeysMode::kEnforceConstraints,
                    GetKeysContext::kAddingKeys,
                    keys.get(),
                    multikeyMetadataKeys,
                    multikeyPaths,
                    boost::none,  // loc
                    kNoopOnSuppressedErrorFn);
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

    std::unique_ptr<SortedDataInterface::Cursor> cursor(_newInterface->newCursor(opCtx));
    const auto requestedInfo = kDebugBuild ? SortedDataInterface::Cursor::kKeyAndLoc
                                           : SortedDataInterface::Cursor::kWantLoc;
    if (auto kv = cursor->seekExact(actualKey, requestedInfo)) {
        // StorageEngine should guarantee these.
        dassert(!kv->loc.isNull());
        dassert(kv->key.woCompare(KeyString::toBson(actualKey.getBuffer(),
                                                    actualKey.getSize(),
                                                    getSortedDataInterface()->getOrdering(),
                                                    actualKey.getTypeBits()),
                                  /*order*/ BSONObj(),
                                  /*considerFieldNames*/ false) == 0);

        return kv->loc;
    }

    return RecordId();
}

void AbstractIndexAccessMethod::validate(OperationContext* opCtx,
                                         int64_t* numKeys,
                                         IndexValidateResults* fullResults) const {
    long long keys = 0;
    _newInterface->fullValidate(opCtx, &keys, fullResults);
    *numKeys = keys;
}

bool AbstractIndexAccessMethod::appendCustomStats(OperationContext* opCtx,
                                                  BSONObjBuilder* output,
                                                  double scale) const {
    return _newInterface->appendCustomStats(opCtx, output, scale);
}

long long AbstractIndexAccessMethod::getSpaceUsedBytes(OperationContext* opCtx) const {
    return _newInterface->getSpaceUsedBytes(opCtx);
}

long long AbstractIndexAccessMethod::getFreeStorageBytes(OperationContext* opCtx) const {
    return _newInterface->getFreeStorageBytes(opCtx);
}

pair<KeyStringSet, KeyStringSet> AbstractIndexAccessMethod::setDifference(
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

void AbstractIndexAccessMethod::prepareUpdate(OperationContext* opCtx,
                                              const CollectionPtr& collection,
                                              const IndexCatalogEntry* index,
                                              const BSONObj& from,
                                              const BSONObj& to,
                                              const RecordId& record,
                                              const InsertDeleteOptions& options,
                                              UpdateTicket* ticket) const {
    SharedBufferFragmentBuilder pooledBuilder(KeyString::HeapBuilder::kHeapAllocatorDefaultBytes);
    const MatchExpression* indexFilter = index->getFilterExpression();
    if (!indexFilter || indexFilter->matchesBSON(from)) {
        // Override key constraints when generating keys for removal. This only applies to keys
        // that do not apply to a partial filter expression.
        const auto getKeysMode = index->isHybridBuilding()
            ? IndexAccessMethod::GetKeysMode::kRelaxConstraintsUnfiltered
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
                record,
                kNoopOnSuppressedErrorFn);
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
                record,
                kNoopOnSuppressedErrorFn);
    }

    ticket->loc = record;
    ticket->dupsAllowed = options.dupsAllowed;

    std::tie(ticket->removed, ticket->added) = setDifference(ticket->oldKeys, ticket->newKeys);

    ticket->_isValid = true;
}

Status AbstractIndexAccessMethod::update(OperationContext* opCtx,
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
        Status status = _newInterface->insert(opCtx, keyString, ticket.dupsAllowed);
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

Status AbstractIndexAccessMethod::compact(OperationContext* opCtx) {
    return this->_newInterface->compact(opCtx);
}

class AbstractIndexAccessMethod::BulkBuilderImpl : public IndexAccessMethod::BulkBuilder {
public:
    BulkBuilderImpl(const IndexCatalogEntry* indexCatalogEntry,
                    size_t maxMemoryUsageBytes,
                    StringData dbName);

    BulkBuilderImpl(const IndexCatalogEntry* index,
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

    const MultikeyPaths& getMultikeyPaths() const final;

    bool isMultikey() const final;

    /**
     * Inserts all multikey metadata keys cached during the BulkBuilder's lifetime into the
     * underlying Sorter, finalizes it, and returns an iterator over the sorted dataset.
     */
    Sorter::Iterator* done() final;

    int64_t getKeysInserted() const final;

    Sorter::PersistedState persistDataForShutdown() final;

private:
    void _insertMultikeyMetadataKeysIntoSorter();

    Sorter* _makeSorter(
        size_t maxMemoryUsageBytes,
        StringData dbName,
        boost::optional<StringData> fileName = boost::none,
        const boost::optional<std::vector<SorterRange>>& ranges = boost::none) const;

    Sorter::Settings _makeSorterSettings() const;

    const IndexCatalogEntry* _indexCatalogEntry;
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

std::unique_ptr<IndexAccessMethod::BulkBuilder> AbstractIndexAccessMethod::initiateBulk(
    size_t maxMemoryUsageBytes,
    const boost::optional<IndexStateInfo>& stateInfo,
    StringData dbName) {
    return stateInfo
        ? std::make_unique<BulkBuilderImpl>(
              _indexCatalogEntry, maxMemoryUsageBytes, *stateInfo, dbName)
        : std::make_unique<BulkBuilderImpl>(_indexCatalogEntry, maxMemoryUsageBytes, dbName);
}

AbstractIndexAccessMethod::BulkBuilderImpl::BulkBuilderImpl(const IndexCatalogEntry* index,
                                                            size_t maxMemoryUsageBytes,
                                                            StringData dbName)
    : _indexCatalogEntry(index), _sorter(_makeSorter(maxMemoryUsageBytes, dbName)) {}

AbstractIndexAccessMethod::BulkBuilderImpl::BulkBuilderImpl(const IndexCatalogEntry* index,
                                                            size_t maxMemoryUsageBytes,
                                                            const IndexStateInfo& stateInfo,
                                                            StringData dbName)
    : _indexCatalogEntry(index),
      _sorter(
          _makeSorter(maxMemoryUsageBytes, dbName, stateInfo.getFileName(), stateInfo.getRanges())),
      _keysInserted(stateInfo.getNumKeys().value_or(0)),
      _isMultiKey(stateInfo.getIsMultikey()),
      _indexMultikeyPaths(createMultikeyPaths(stateInfo.getMultikeyPaths())) {}

Status AbstractIndexAccessMethod::BulkBuilderImpl::insert(
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
        _indexCatalogEntry->accessMethod()->getKeys(
            opCtx,
            collection,
            pooledBuilder,
            obj,
            options.getKeysMode,
            GetKeysContext::kAddingKeys,
            keys.get(),
            &_multikeyMetadataKeys,
            multikeyPaths.get(),
            loc,
            [&](Status status, const BSONObj&, boost::optional<RecordId>) {
                // If a key generation error was suppressed, record the document as "skipped" so the
                // index builder can retry at a point when data is consistent.
                auto interceptor = _indexCatalogEntry->indexBuildInterceptor();
                if (interceptor && interceptor->getSkippedRecordTracker()) {
                    LOGV2_DEBUG(20684,
                                1,
                                "Recording suppressed key generation error to retry later: "
                                "{error} on {loc}: {obj}",
                                "error"_attr = status,
                                "loc"_attr = loc,
                                "obj"_attr = redact(obj));

                    // Save and restore the cursor around the write in case it throws a WCE
                    // internally and causes the cursor to be unpositioned.
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
        _indexCatalogEntry->accessMethod()->shouldMarkIndexAsMultikey(
            keys->size(), _multikeyMetadataKeys, *multikeyPaths);

    return Status::OK();
}

const MultikeyPaths& AbstractIndexAccessMethod::BulkBuilderImpl::getMultikeyPaths() const {
    return _indexMultikeyPaths;
}

bool AbstractIndexAccessMethod::BulkBuilderImpl::isMultikey() const {
    return _isMultiKey;
}

IndexAccessMethod::BulkBuilder::Sorter::Iterator*
AbstractIndexAccessMethod::BulkBuilderImpl::done() {
    _insertMultikeyMetadataKeysIntoSorter();
    return _sorter->done();
}

int64_t AbstractIndexAccessMethod::BulkBuilderImpl::getKeysInserted() const {
    return _keysInserted;
}

AbstractIndexAccessMethod::BulkBuilder::Sorter::PersistedState
AbstractIndexAccessMethod::BulkBuilderImpl::persistDataForShutdown() {
    _insertMultikeyMetadataKeysIntoSorter();
    return _sorter->persistDataForShutdown();
}

void AbstractIndexAccessMethod::BulkBuilderImpl::_insertMultikeyMetadataKeysIntoSorter() {
    for (const auto& keyString : _multikeyMetadataKeys) {
        _sorter->add(keyString, mongo::NullValue());
        ++_keysInserted;
    }

    // We clear the multikey metadata keys to prevent them from being inserted into the Sorter
    // twice in the case that done() is called and then persistDataForShutdown() is later called.
    _multikeyMetadataKeys.clear();
}

AbstractIndexAccessMethod::BulkBuilderImpl::Sorter::Settings
AbstractIndexAccessMethod::BulkBuilderImpl::_makeSorterSettings() const {
    return std::pair<KeyString::Value::SorterDeserializeSettings,
                     mongo::NullValue::SorterDeserializeSettings>(
        {_indexCatalogEntry->accessMethod()->getSortedDataInterface()->getKeyStringVersion()}, {});
}

AbstractIndexAccessMethod::BulkBuilderImpl::Sorter*
AbstractIndexAccessMethod::BulkBuilderImpl::_makeSorter(
    size_t maxMemoryUsageBytes,
    StringData dbName,
    boost::optional<StringData> fileName,
    const boost::optional<std::vector<SorterRange>>& ranges) const {
    return fileName ? Sorter::makeFromExistingRanges(fileName->toString(),
                                                     *ranges,
                                                     makeSortOptions(maxMemoryUsageBytes, dbName),
                                                     BtreeExternalSortComparison(),
                                                     _makeSorterSettings())
                    : Sorter::make(makeSortOptions(maxMemoryUsageBytes, dbName),
                                   BtreeExternalSortComparison(),
                                   _makeSorterSettings());
}

void AbstractIndexAccessMethod::_yieldBulkLoad(OperationContext* opCtx,
                                               const Yieldable* yieldable,
                                               const NamespaceString& ns) const {
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

Status AbstractIndexAccessMethod::commitBulk(OperationContext* opCtx,
                                             const CollectionPtr& collection,
                                             BulkBuilder* bulk,
                                             bool dupsAllowed,
                                             int32_t yieldIterations,
                                             const KeyHandlerFn& onDuplicateKeyInserted,
                                             const RecordIdHandlerFn& onDuplicateRecord) {
    Timer timer;

    auto ns = _indexCatalogEntry->getNSSFromCatalog(opCtx);

    std::unique_ptr<BulkBuilder::Sorter::Iterator> it(bulk->done());

    static constexpr char message[] = "Index Build: inserting keys from external sorter into index";
    ProgressMeterHolder pm;
    {
        stdx::unique_lock<Client> lk(*opCtx->getClient());
        pm.set(CurOp::get(opCtx)->setProgress_inlock(
            message, bulk->getKeysInserted(), 3 /* secondsBetween */));
    }

    auto builder = _newInterface->makeBulkBuilder(opCtx, dupsAllowed);

    KeyString::Value previousKey;

    for (int64_t i = 0; it->more(); i++) {
        opCtx->checkForInterrupt();

        auto failPointHang = [opCtx, i, &indexName = _descriptor->indexName()](FailPoint* fp) {
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
        BulkBuilder::Sorter::Data data = it->next();

        // Assert that keys are retrieved from the sorter in non-decreasing order, but only in debug
        // builds since this check can be expensive.
        int cmpData;
        if (_descriptor->unique()) {
            cmpData = data.first.compareWithoutRecordIdLong(previousKey);
        }

        if (kDebugBuild && data.first.compare(previousKey) < 0) {
            LOGV2_FATAL_NOTRACE(
                31171,
                "Expected the next key to be greater than or equal to the previous key",
                "nextKey"_attr = data.first.toString(),
                "previousKey"_attr = previousKey.toString(),
                "index"_attr = _descriptor->indexName());
        }

        // Before attempting to insert, perform a duplicate key check.
        bool isDup = (_descriptor->unique()) ? (cmpData == 0) : false;
        if (isDup && !dupsAllowed) {
            Status status = _handleDuplicateKey(opCtx, data.first, onDuplicateRecord);
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
            _yieldBulkLoad(opCtx, &collection, ns);
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
          "index"_attr = _descriptor->indexName(),
          "keysInserted"_attr = bulk->getKeysInserted(),
          "duration"_attr = Milliseconds(Seconds(timer.seconds())));
    return Status::OK();
}

void AbstractIndexAccessMethod::setIndexIsMultikey(OperationContext* opCtx,
                                                   const CollectionPtr& collection,
                                                   KeyStringSet multikeyMetadataKeys,
                                                   MultikeyPaths paths) {
    _indexCatalogEntry->setMultikey(opCtx, collection, multikeyMetadataKeys, paths);
}

IndexAccessMethod::OnSuppressedErrorFn IndexAccessMethod::kNoopOnSuppressedErrorFn =
    [](Status status, const BSONObj& obj, boost::optional<RecordId> loc) {
        LOGV2_DEBUG(
            20686,
            1,
            "Suppressed key generation error: {error} when getting index keys for {loc}: {obj}",
            "error"_attr = redact(status),
            "loc"_attr = loc,
            "obj"_attr = redact(obj));
    };

void AbstractIndexAccessMethod::getKeys(OperationContext* opCtx,
                                        const CollectionPtr& collection,
                                        SharedBufferFragmentBuilder& pooledBufferBuilder,
                                        const BSONObj& obj,
                                        GetKeysMode mode,
                                        GetKeysContext context,
                                        KeyStringSet* keys,
                                        KeyStringSet* multikeyMetadataKeys,
                                        MultikeyPaths* multikeyPaths,
                                        boost::optional<RecordId> id,
                                        OnSuppressedErrorFn onSuppressedError) const {
    invariant(!id || _newInterface->rsKeyFormat() != KeyFormat::String || id->isStr(),
              fmt::format("RecordId is not in the same string format as its RecordStore; id: {}",
                          id->toString()));
    invariant(!id || _newInterface->rsKeyFormat() != KeyFormat::Long || id->isLong(),
              fmt::format("RecordId is not in the same long format as its RecordStore; id: {}",
                          id->toString()));

    try {
        validateDocument(collection, obj, _descriptor->keyPattern());
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
        if (mode == GetKeysMode::kEnforceConstraints) {
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
        if (mode == GetKeysMode::kRelaxConstraintsUnfiltered && filter &&
            filter->matchesBSON(obj)) {
            throw;
        }

        onSuppressedError(ex.toStatus(), obj, id);
    }
}

bool AbstractIndexAccessMethod::shouldMarkIndexAsMultikey(
    size_t numberOfKeys,
    const KeyStringSet& multikeyMetadataKeys,
    const MultikeyPaths& multikeyPaths) const {
    return numberOfKeys > 1 || isMultikeyFromPaths(multikeyPaths);
}

void AbstractIndexAccessMethod::validateDocument(const CollectionPtr& collection,
                                                 const BSONObj& obj,
                                                 const BSONObj& keyPattern) const {}

SortedDataInterface* AbstractIndexAccessMethod::getSortedDataInterface() const {
    return _newInterface.get();
}

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

Status AbstractIndexAccessMethod::_handleDuplicateKey(OperationContext* opCtx,
                                                      const KeyString::Value& dataKey,
                                                      const RecordIdHandlerFn& onDuplicateRecord) {
    RecordId recordId = KeyString::decodeRecordIdLongAtEnd(dataKey.getBuffer(), dataKey.getSize());
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
}  // namespace mongo

#include "mongo/db/sorter/sorter.cpp"
MONGO_CREATE_SORTER(mongo::KeyString::Value, mongo::NullValue, mongo::BtreeExternalSortComparison);
