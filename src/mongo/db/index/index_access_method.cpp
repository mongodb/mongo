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
#include "mongo/db/index/index_descriptor.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/keypattern.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/repl/timestamp_block.h"
#include "mongo/db/storage/durable_catalog.h"
#include "mongo/db/storage/storage_options.h"
#include "mongo/util/log.h"
#include "mongo/util/progress_meter.h"
#include "mongo/util/scopeguard.h"

namespace mongo {

using std::endl;
using std::pair;
using std::set;
using std::vector;

using IndexVersion = IndexDescriptor::IndexVersion;

namespace {

// Reserved RecordId against which multikey metadata keys are indexed.
static const RecordId kMultikeyMetadataKeyId =
    RecordId{RecordId::ReservedId::kWildcardMultikeyMetadataId};

/**
 * Returns true if at least one prefix of any of the indexed fields causes the index to be
 * multikey, and returns false otherwise. This function returns false if the 'multikeyPaths'
 * vector is empty.
 */
bool isMultikeyFromPaths(const MultikeyPaths& multikeyPaths) {
    return std::any_of(multikeyPaths.cbegin(),
                       multikeyPaths.cend(),
                       [](const std::set<std::size_t>& components) { return !components.empty(); });
}

std::vector<KeyString::Value> asVector(const KeyStringSet& keySet) {
    return {keySet.begin(), keySet.end()};
}
}  // namespace

struct BtreeExternalSortComparison {
    typedef std::pair<KeyString::Value, mongo::NullValue> Data;
    int operator()(const Data& l, const Data& r) const {
        return l.first.compare(r.first);
    }
};

AbstractIndexAccessMethod::AbstractIndexAccessMethod(IndexCatalogEntry* btreeState,
                                                     std::unique_ptr<SortedDataInterface> btree)
    : _btreeState(btreeState),
      _descriptor(btreeState->descriptor()),
      _newInterface(std::move(btree)) {
    verify(IndexDescriptor::isIndexVersionSupported(_descriptor->version()));
}

bool AbstractIndexAccessMethod::isFatalError(OperationContext* opCtx,
                                             Status status,
                                             KeyString::Value key) {
    // If the status is Status::OK() return false immediately.
    if (status.isOK()) {
        return false;
    }

    // A document might be indexed multiple times during a background index build if it moves ahead
    // of the cursor (e.g. via an update). We test this scenario and swallow the error accordingly.
    if (status == ErrorCodes::DuplicateKeyValue && !_btreeState->isReady(opCtx)) {
        LOG(3) << "KeyString " << key << " already in index during background indexing (ok)";
        return false;
    }
    return true;
}

// Find the keys for obj, put them in the tree pointing to loc.
Status AbstractIndexAccessMethod::insert(OperationContext* opCtx,
                                         const BSONObj& obj,
                                         const RecordId& loc,
                                         const InsertDeleteOptions& options,
                                         InsertResult* result) {
    invariant(options.fromIndexBuilder || !_btreeState->isHybridBuilding());

    KeyStringSet multikeyMetadataKeys;
    KeyStringSet keys;
    MultikeyPaths multikeyPaths;

    // Delegate to the subclass.
    getKeys(obj, options.getKeysMode, &keys, &multikeyMetadataKeys, &multikeyPaths, loc);

    return insertKeys(opCtx,
                      {keys.begin(), keys.end()},
                      {multikeyMetadataKeys.begin(), multikeyMetadataKeys.end()},
                      multikeyPaths,
                      loc,
                      options,
                      result);
}

Status AbstractIndexAccessMethod::insertKeys(OperationContext* opCtx,
                                             const vector<KeyString::Value>& keys,
                                             const vector<KeyString::Value>& multikeyMetadataKeys,
                                             const MultikeyPaths& multikeyPaths,
                                             const RecordId& loc,
                                             const InsertDeleteOptions& options,
                                             InsertResult* result) {
    // Add all new data keys, and all new multikey metadata keys, into the index. When iterating
    // over the data keys, each of them should point to the doc's RecordId. When iterating over
    // the multikey metadata keys, they should point to the reserved 'kMultikeyMetadataKeyId'.
    for (const auto keyVec : {&keys, &multikeyMetadataKeys}) {
        for (const auto& keyString : *keyVec) {
            bool unique = _descriptor->unique();
            Status status = _newInterface->insert(opCtx, keyString, !unique /* dupsAllowed */);

            // When duplicates are encountered and allowed, retry with dupsAllowed. Add the
            // key to the output vector so callers know which duplicate keys were inserted.
            if (ErrorCodes::DuplicateKey == status.code() && options.dupsAllowed) {
                invariant(unique);
                status = _newInterface->insert(opCtx, keyString, true /* dupsAllowed */);

                if (status.isOK() && result) {
                    auto key =
                        KeyString::toBson(keyString, getSortedDataInterface()->getOrdering());
                    result->dupsInserted.push_back(key);
                }
            }
            if (isFatalError(opCtx, status, keyString)) {
                return status;
            }
        }
    }

    if (result) {
        result->numInserted += keys.size() + multikeyMetadataKeys.size();
    }

    if (shouldMarkIndexAsMultikey(keys, multikeyMetadataKeys, multikeyPaths)) {
        _btreeState->setMultikey(opCtx, multikeyPaths);
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
        log() << "Assertion failure: _unindex failed on: " << _descriptor->parentNS()
              << " for index: " << _descriptor->indexName();
        log() << "Assertion failure: _unindex failed: " << redact(e) << "  KeyString:" << keyString
              << "  dl:" << loc;
        logContext();
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
                                             const std::vector<KeyString::Value>& keys,
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

Status AbstractIndexAccessMethod::touch(OperationContext* opCtx, const BSONObj& obj) {
    KeyStringSet keys;
    // There's no need to compute the prefixes of the indexed fields that cause the index to be
    // multikey when paging a document's index entries into memory.
    KeyStringSet* multikeyMetadataKeys = nullptr;
    MultikeyPaths* multikeyPaths = nullptr;
    getKeys(obj, GetKeysMode::kEnforceConstraints, &keys, multikeyMetadataKeys, multikeyPaths);

    std::unique_ptr<SortedDataInterface::Cursor> cursor(_newInterface->newCursor(opCtx));
    for (const auto& keyString : keys) {
        cursor->seekExact(keyString);
    }

    return Status::OK();
}


Status AbstractIndexAccessMethod::touch(OperationContext* opCtx) const {
    return _newInterface->touch(opCtx);
}

RecordId AbstractIndexAccessMethod::findSingle(OperationContext* opCtx,
                                               const BSONObj& requestedKey) const {
    // Generate the key for this index.
    KeyString::Value actualKey = [&]() {
        if (_btreeState->getCollator()) {
            // For performance, call get keys only if there is a non-simple collation.
            KeyStringSet keys;
            KeyStringSet* multikeyMetadataKeys = nullptr;
            MultikeyPaths* multikeyPaths = nullptr;
            getKeys(requestedKey,
                    GetKeysMode::kEnforceConstraints,
                    &keys,
                    multikeyMetadataKeys,
                    multikeyPaths);
            invariant(keys.size() == 1);
            return *keys.begin();
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
                                         ValidateResults* fullResults) const {
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

pair<vector<KeyString::Value>, vector<KeyString::Value>> AbstractIndexAccessMethod::setDifference(
    const KeyStringSet& left, const KeyStringSet& right, Ordering ordering) {
    // Two iterators to traverse the two sets in sorted order.
    auto leftIt = left.begin();
    auto rightIt = right.begin();
    vector<KeyString::Value> onlyLeft;
    vector<KeyString::Value> onlyRight;

    while (leftIt != left.end() && rightIt != right.end()) {
        const int cmp = leftIt->compare(*rightIt);
        if (cmp == 0) {
            /*
             * 'leftIt' and 'rightIt' compare equal using compare(), but may not be identical, which
             * should result in an index change.
             */
            auto leftKey = KeyString::toBson(
                leftIt->getBuffer(), leftIt->getSize(), ordering, leftIt->getTypeBits());
            auto rightKey = KeyString::toBson(
                rightIt->getBuffer(), rightIt->getSize(), ordering, rightIt->getTypeBits());
            if (!leftKey.binaryEqual(rightKey)) {
                onlyLeft.push_back(*leftIt);
                onlyRight.push_back(*rightIt);
            }
            ++leftIt;
            ++rightIt;
            continue;
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

    return {std::move(onlyLeft), std::move(onlyRight)};
}

void AbstractIndexAccessMethod::prepareUpdate(OperationContext* opCtx,
                                              IndexCatalogEntry* index,
                                              const BSONObj& from,
                                              const BSONObj& to,
                                              const RecordId& record,
                                              const InsertDeleteOptions& options,
                                              UpdateTicket* ticket) {
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
        getKeys(from, getKeysMode, &ticket->oldKeys, nullptr, nullptr, record);
    }

    if (!indexFilter || indexFilter->matchesBSON(to)) {
        getKeys(to,
                options.getKeysMode,
                &ticket->newKeys,
                &ticket->newMultikeyMetadataKeys,
                &ticket->newMultikeyPaths,
                record);
    }

    ticket->loc = record;
    ticket->dupsAllowed = options.dupsAllowed;

    std::tie(ticket->removed, ticket->added) =
        setDifference(ticket->oldKeys, ticket->newKeys, getSortedDataInterface()->getOrdering());

    ticket->_isValid = true;
}

Status AbstractIndexAccessMethod::update(OperationContext* opCtx,
                                         const UpdateTicket& ticket,
                                         int64_t* numInserted,
                                         int64_t* numDeleted) {
    invariant(!_btreeState->isHybridBuilding());
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

    // Add all new data keys, and all new multikey metadata keys, into the index. When iterating
    // over the data keys, each of them should point to the doc's RecordId. When iterating over
    // the multikey metadata keys, they should point to the reserved 'kMultikeyMetadataKeyId'.
    const auto newMultikeyMetadataKeys = asVector(ticket.newMultikeyMetadataKeys);
    for (const auto keySet : {&ticket.added, &newMultikeyMetadataKeys}) {
        for (const auto& keyString : *keySet) {
            Status status = _newInterface->insert(opCtx, keyString, ticket.dupsAllowed);
            if (isFatalError(opCtx, status, keyString)) {
                return status;
            }
        }
    }

    if (shouldMarkIndexAsMultikey(
            {ticket.newKeys.begin(), ticket.newKeys.end()},
            {ticket.newMultikeyMetadataKeys.begin(), ticket.newMultikeyMetadataKeys.end()},
            ticket.newMultikeyPaths)) {
        _btreeState->setMultikey(opCtx, ticket.newMultikeyPaths);
    }

    *numDeleted = ticket.removed.size();
    *numInserted = ticket.added.size();

    return Status::OK();
}

Status AbstractIndexAccessMethod::compact(OperationContext* opCtx) {
    return this->_newInterface->compact(opCtx);
}

class AbstractIndexAccessMethod::BulkBuilderImpl : public IndexAccessMethod::BulkBuilder {
public:
    BulkBuilderImpl(const IndexAccessMethod* index,
                    const IndexDescriptor* descriptor,
                    size_t maxMemoryUsageBytes);

    Status insert(OperationContext* opCtx,
                  const BSONObj& obj,
                  const RecordId& loc,
                  const InsertDeleteOptions& options) final;

    const MultikeyPaths& getMultikeyPaths() const final;

    bool isMultikey() const final;

    /**
     * Inserts all multikey metadata keys cached during the BulkBuilder's lifetime into the
     * underlying Sorter, finalizes it, and returns an iterator over the sorted dataset.
     */
    Sorter::Iterator* done() final;

    int64_t getKeysInserted() const final;

private:
    std::unique_ptr<Sorter> _sorter;
    const IndexAccessMethod* _real;
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
    size_t maxMemoryUsageBytes) {
    return std::make_unique<BulkBuilderImpl>(this, _descriptor, maxMemoryUsageBytes);
}

AbstractIndexAccessMethod::BulkBuilderImpl::BulkBuilderImpl(const IndexAccessMethod* index,
                                                            const IndexDescriptor* descriptor,
                                                            size_t maxMemoryUsageBytes)
    : _sorter(Sorter::make(SortOptions()
                               .TempDir(storageGlobalParams.dbpath + "/_tmp")
                               .ExtSortAllowed()
                               .MaxMemoryUsageBytes(maxMemoryUsageBytes),
                           BtreeExternalSortComparison(),
                           std::pair<KeyString::Value::SorterDeserializeSettings,
                                     mongo::NullValue::SorterDeserializeSettings>(
                               {index->getSortedDataInterface()->getKeyStringVersion()}, {}))),
      _real(index) {}

Status AbstractIndexAccessMethod::BulkBuilderImpl::insert(OperationContext* opCtx,
                                                          const BSONObj& obj,
                                                          const RecordId& loc,
                                                          const InsertDeleteOptions& options) {
    KeyStringSet keys;
    MultikeyPaths multikeyPaths;

    try {
        _real->getKeys(
            obj, options.getKeysMode, &keys, &_multikeyMetadataKeys, &multikeyPaths, loc);
    } catch (...) {
        return exceptionToStatus();
    }

    if (!multikeyPaths.empty()) {
        if (_indexMultikeyPaths.empty()) {
            _indexMultikeyPaths = multikeyPaths;
        } else {
            invariant(_indexMultikeyPaths.size() == multikeyPaths.size());
            for (size_t i = 0; i < multikeyPaths.size(); ++i) {
                _indexMultikeyPaths[i].insert(multikeyPaths[i].begin(), multikeyPaths[i].end());
            }
        }
    }

    for (const auto& keyString : keys) {
        _sorter->add(keyString, mongo::NullValue());
        ++_keysInserted;
    }

    _isMultiKey = _isMultiKey ||
        _real->shouldMarkIndexAsMultikey(
            {keys.begin(), keys.end()},
            {_multikeyMetadataKeys.begin(), _multikeyMetadataKeys.end()},
            multikeyPaths);

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
    for (const auto& keyString : _multikeyMetadataKeys) {
        _sorter->add(keyString, mongo::NullValue());
        ++_keysInserted;
    }
    return _sorter->done();
}

int64_t AbstractIndexAccessMethod::BulkBuilderImpl::getKeysInserted() const {
    return _keysInserted;
}

Status AbstractIndexAccessMethod::commitBulk(OperationContext* opCtx,
                                             BulkBuilder* bulk,
                                             bool dupsAllowed,
                                             set<RecordId>* dupRecords,
                                             std::vector<BSONObj>* dupKeysInserted) {
    // Cannot simultaneously report uninserted duplicates 'dupRecords' and inserted duplicates
    // 'dupKeysInserted'.
    invariant(!(dupRecords && dupKeysInserted));

    Timer timer;

    std::unique_ptr<BulkBuilder::Sorter::Iterator> it(bulk->done());

    static const char* message = "Index Build: inserting keys from external sorter into index";
    ProgressMeterHolder pm;
    {
        stdx::unique_lock<Client> lk(*opCtx->getClient());
        pm.set(CurOp::get(opCtx)->setProgress_inlock(
            message, bulk->getKeysInserted(), 3 /* secondsBetween */));
    }

    auto builder = std::unique_ptr<SortedDataBuilderInterface>(
        _newInterface->getBulkBuilder(opCtx, dupsAllowed));

    KeyString::Value previousKey;

    while (it->more()) {
        opCtx->checkForInterrupt();

        WriteUnitOfWork wunit(opCtx);

        // Get the next datum and add it to the builder.
        BulkBuilder::Sorter::Data data = it->next();

        // Assert that keys are retrieved from the sorter in non-decreasing order, but only in
        // debug builds since this check can be expensive.
        int cmpData;
        if (kDebugBuild || _descriptor->unique()) {
            cmpData = data.first.compareWithoutRecordId(previousKey);
            if (cmpData < 0) {
                severe() << "expected the next key" << data.first.toString()
                         << " to be greater than or equal to the previous key"
                         << previousKey.toString();
                fassertFailedNoTrace(31171);
            }
        }

        // Before attempting to insert, perform a duplicate key check.
        bool isDup = false;
        if (_descriptor->unique()) {
            isDup = cmpData == 0;
            if (isDup && !dupsAllowed) {
                if (dupRecords) {
                    RecordId recordId = KeyString::decodeRecordIdAtEnd(data.first.getBuffer(),
                                                                       data.first.getSize());
                    dupRecords->insert(recordId);
                    continue;
                }
                auto dupKey =
                    KeyString::toBson(data.first, getSortedDataInterface()->getOrdering());
                return buildDupKeyErrorStatus(dupKey.getOwned(),
                                              _descriptor->parentNS(),
                                              _descriptor->indexName(),
                                              _descriptor->keyPattern());
            }
        }

        Status status = builder->addKey(data.first);

        if (!status.isOK()) {
            // Duplicates are checked before inserting.
            invariant(status.code() != ErrorCodes::DuplicateKey);
            return status;
        }

        previousKey = data.first;

        if (isDup && dupsAllowed && dupKeysInserted) {
            auto dupKey = KeyString::toBson(data.first, getSortedDataInterface()->getOrdering());
            dupKeysInserted->push_back(dupKey.getOwned());
        }

        // If we're here either it's a dup and we're cool with it or the addKey went just fine.
        pm.hit();
        wunit.commit();
    }

    pm.finished();

    log() << "index build: inserted " << bulk->getKeysInserted()
          << " keys from external sorter into index in " << timer.seconds() << " seconds";

    WriteUnitOfWork wunit(opCtx);
    builder->commit(true);
    wunit.commit();
    return Status::OK();
}

void AbstractIndexAccessMethod::setIndexIsMultikey(OperationContext* opCtx, MultikeyPaths paths) {
    _btreeState->setMultikey(opCtx, paths);
}

void AbstractIndexAccessMethod::getKeys(const BSONObj& obj,
                                        GetKeysMode mode,
                                        KeyStringSet* keys,
                                        KeyStringSet* multikeyMetadataKeys,
                                        MultikeyPaths* multikeyPaths,
                                        boost::optional<RecordId> id) const {
    static stdx::unordered_set<int> whiteList{ErrorCodes::CannotBuildIndexKeys,
                                              // Btree
                                              ErrorCodes::CannotIndexParallelArrays,
                                              // FTS
                                              16732,
                                              16733,
                                              16675,
                                              17261,
                                              17262,
                                              // Hash
                                              16766,
                                              // Haystack
                                              16775,
                                              16776,
                                              // 2dsphere geo
                                              16755,
                                              16756,
                                              // 2d geo
                                              16804,
                                              13067,
                                              13068,
                                              13026,
                                              13027};
    try {
        doGetKeys(obj, keys, multikeyMetadataKeys, multikeyPaths, id);
    } catch (const AssertionException& ex) {
        // Suppress all indexing errors when mode is kRelaxConstraints.
        if (mode == GetKeysMode::kEnforceConstraints) {
            throw;
        }

        keys->clear();
        if (multikeyPaths) {
            multikeyPaths->clear();
        }
        // Only suppress the errors in the whitelist.
        if (whiteList.find(ex.code()) == whiteList.end()) {
            throw;
        }

        // If the document applies to the filter (which means that it should have never been
        // indexed), do not supress the error.
        const MatchExpression* filter = _btreeState->getFilterExpression();
        if (mode == GetKeysMode::kRelaxConstraintsUnfiltered && filter &&
            filter->matchesBSON(obj)) {
            throw;
        }

        LOG(1) << "Ignoring indexing error for idempotency reasons: " << redact(ex)
               << " when getting index keys of " << redact(obj);
    }
}

bool AbstractIndexAccessMethod::shouldMarkIndexAsMultikey(
    const vector<KeyString::Value>& keys,
    const vector<KeyString::Value>& multikeyMetadataKeys,
    const MultikeyPaths& multikeyPaths) const {
    return (keys.size() > 1 || isMultikeyFromPaths(multikeyPaths));
}

SortedDataInterface* AbstractIndexAccessMethod::getSortedDataInterface() const {
    return _newInterface.get();
}

/**
 * Generates a new file name on each call using a static, atomic and monotonically increasing
 * number.
 *
 * Each user of the Sorter must implement this function to ensure that all temporary files that the
 * Sorter instances produce are uniquely identified using a unique file name extension with separate
 * atomic variable. This is necessary because the sorter.cpp code is separately included in multiple
 * places, rather than compiled in one place and linked, and so cannot provide a globally unique ID.
 */
std::string nextFileName() {
    static AtomicWord<unsigned> indexAccessMethodFileCounter;
    return "extsort-index." + std::to_string(indexAccessMethodFileCounter.fetchAndAdd(1));
}

}  // namespace mongo

#include "mongo/db/sorter/sorter.cpp"
MONGO_CREATE_SORTER(mongo::KeyString::Value, mongo::NullValue, mongo::BtreeExternalSortComparison);
