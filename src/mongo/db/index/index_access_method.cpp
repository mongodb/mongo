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
#include "mongo/db/index/index_access_method_gen.h"
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

std::vector<BSONObj> asVector(const BSONObjSet& objSet) {
    return {objSet.begin(), objSet.end()};
}

// TODO SERVER-36385: Remove this
const int TempKeyMaxSize = 1024;

// TODO SERVER-36385: Completely remove the key size check in 4.4
Status checkKeySize(const BSONObj& key) {
    if (key.objsize() >= TempKeyMaxSize) {
        std::string msg = str::stream()
            << "Index key too large to index, failing " << key.objsize() << ' ' << redact(key);
        return Status(ErrorCodes::KeyTooLong, msg);
    }
    return Status::OK();
}

}  // namespace

// TODO SERVER-36386: Remove the server parameter
bool failIndexKeyTooLongParam() {
    // Always return true in FCV 4.2 although FCV 4.2 actually never needs to
    // check this value because there shouldn't be any KeyTooLong errors in FCV 4.2.
    if (serverGlobalParams.featureCompatibility.getVersion() ==
        ServerGlobalParams::FeatureCompatibility::Version::kFullyUpgradedTo42)
        return true;
    return failIndexKeyTooLong.load();
}

class BtreeExternalSortComparison {
public:
    BtreeExternalSortComparison(const BSONObj& ordering, IndexVersion version)
        : _ordering(Ordering::make(ordering)), _version(version) {
        invariant(IndexDescriptor::isIndexVersionSupported(version));
    }

    typedef std::pair<BSONObj, RecordId> Data;

    int operator()(const Data& l, const Data& r) const {
        if (int x = l.first.woCompare(r.first, _ordering, /*considerfieldname*/ false))
            return x;
        return l.second.compare(r.second);
    }

private:
    const Ordering _ordering;
    const IndexVersion _version;
};

AbstractIndexAccessMethod::AbstractIndexAccessMethod(IndexCatalogEntry* btreeState,
                                                     SortedDataInterface* btree)
    : _btreeState(btreeState), _descriptor(btreeState->descriptor()), _newInterface(btree) {
    verify(IndexDescriptor::isIndexVersionSupported(_descriptor->version()));
}

// TODO SERVER-36385: Remove this when there is no KeyTooLong error.
bool AbstractIndexAccessMethod::ignoreKeyTooLong() {
    return !failIndexKeyTooLongParam();
}

// TODO SERVER-36385: Remove this when there is no KeyTooLong error.
bool AbstractIndexAccessMethod::shouldCheckIndexKeySize(OperationContext* opCtx) {
    // The index key size ought to be checked for nodes in FCV 4.0.  However, it is possible for an
    // index build to have begun on a secondary while in FCV 4.2 and then have FCV drop to 4.0.  For
    // those builds, we should NOT check the index key length; instead of crashing, we are going to
    // allow them.

    // The check for RSTL confirms that we are a primary, or a secondary that started a build while
    // in FCV 4.0.  In FCV 4.2, secondary index builds unlock the RSTL, and thus are not allowed to
    // call shouldRelaxIndexConstraints(), since that function checks replication state and that is
    // not allowed unless you hold the RSTL.
    return serverGlobalParams.featureCompatibility.isVersionInitialized() &&
        serverGlobalParams.featureCompatibility.getVersion() ==
        ServerGlobalParams::FeatureCompatibility::Version::kFullyDowngradedTo40 &&
        opCtx->lockState()->isRSTLLocked() &&
        !repl::ReplicationCoordinator::get(opCtx)->shouldRelaxIndexConstraints(opCtx,
                                                                               _btreeState->ns());
    ;
}

bool AbstractIndexAccessMethod::isFatalError(OperationContext* opCtx, Status status, BSONObj key) {
    // If the status is Status::OK(), or if it is ErrorCodes::KeyTooLong and the user has chosen to
    // ignore this error, return false immediately.
    // TODO SERVER-36385: Remove this when there is no KeyTooLong error.
    if (status.isOK() || (status == ErrorCodes::KeyTooLong && ignoreKeyTooLong())) {
        return false;
    }

    // A document might be indexed multiple times during a background index build if it moves ahead
    // of the cursor (e.g. via an update). We test this scenario and swallow the error accordingly.
    if (status == ErrorCodes::DuplicateKeyValue && !_btreeState->isReady(opCtx)) {
        LOG(3) << "key " << key << " already in index during background indexing (ok)";
        return false;
    }
    return true;
}

// Find the keys for obj, put them in the tree pointing to loc.
Status AbstractIndexAccessMethod::insert(OperationContext* opCtx,
                                         const BSONObj& obj,
                                         const RecordId& loc,
                                         const InsertDeleteOptions& options,
                                         KeyHandlerFn&& onDuplicateKey,
                                         int64_t* numInserted) {
    invariant(options.fromIndexBuilder || !_btreeState->isHybridBuilding());

    BSONObjSet multikeyMetadataKeys = SimpleBSONObjComparator::kInstance.makeBSONObjSet();
    BSONObjSet keys = SimpleBSONObjComparator::kInstance.makeBSONObjSet();
    MultikeyPaths multikeyPaths;

    // Delegate to the subclass.
    getKeys(obj,
            options.getKeysMode,
            GetKeysContext::kReadOrAddKeys,
            &keys,
            &multikeyMetadataKeys,
            &multikeyPaths);

    return insertKeysAndUpdateMultikeyPaths(
        opCtx,
        {keys.begin(), keys.end()},
        {multikeyMetadataKeys.begin(), multikeyMetadataKeys.end()},
        multikeyPaths,
        loc,
        options,
        std::move(onDuplicateKey),
        numInserted);
}

Status AbstractIndexAccessMethod::insertKeysAndUpdateMultikeyPaths(
    OperationContext* opCtx,
    const std::vector<BSONObj>& keys,
    const std::vector<BSONObj>& multikeyMetadataKeys,
    const MultikeyPaths& multikeyPaths,
    const RecordId& loc,
    const InsertDeleteOptions& options,
    KeyHandlerFn&& onDuplicateKey,
    int64_t* numInserted) {
    // Insert the specified data keys into the index.
    auto status = insertKeys(opCtx, keys, loc, options, std::move(onDuplicateKey), numInserted);
    if (!status.isOK()) {
        return status;
    }
    // If these keys should cause the index to become multikey, pass them into the catalog.
    if (shouldMarkIndexAsMultikey(keys, multikeyMetadataKeys, multikeyPaths)) {
        _btreeState->setMultikey(opCtx, multikeyMetadataKeys, multikeyPaths);
    }
    // If we have some multikey metadata keys, they should have been added while marking the index
    // as multikey in the catalog. Add them to the count of keys inserted for completeness.
    if (numInserted && !multikeyMetadataKeys.empty()) {
        *numInserted += multikeyMetadataKeys.size();
    }
    return Status::OK();
}

Status AbstractIndexAccessMethod::insertKeys(OperationContext* opCtx,
                                             const vector<BSONObj>& keys,
                                             const RecordId& loc,
                                             const InsertDeleteOptions& options,
                                             KeyHandlerFn&& onDuplicateKey,
                                             int64_t* numInserted) {
    // Initialize the 'numInserted' out-parameter to zero in case the caller did not already do so.
    if (numInserted) {
        *numInserted = 0;
    }
    // Add all new data keys into the index with the specified RecordId.
    bool checkIndexKeySize = shouldCheckIndexKeySize(opCtx);
    for (const auto& key : keys) {
        Status status = checkIndexKeySize ? checkKeySize(key) : Status::OK();
        if (status.isOK()) {
            bool unique = _descriptor->unique();
            StatusWith<SpecialFormatInserted> ret =
                _newInterface->insert(opCtx, key, loc, !unique /* dupsAllowed */);
            status = ret.getStatus();

            // When duplicates are encountered and allowed, retry with dupsAllowed. Call
            // onDuplicateKey() with the inserted duplicate key.
            if (ErrorCodes::DuplicateKey == status.code() && options.dupsAllowed) {
                invariant(unique);
                ret = _newInterface->insert(opCtx, key, loc, true /* dupsAllowed */);
                status = ret.getStatus();

                if (status.isOK() && ret.getValue() != SpecialFormatInserted::NothingInserted &&
                    onDuplicateKey) {
                    // Only run the duplicate key handler if we inserted the key ourselves. Someone
                    // else could have already inserted this exact key, but in that case we don't
                    // count it as a duplicate.
                    status = onDuplicateKey(key);
                }
            }

            if (status.isOK() && ret.getValue() == SpecialFormatInserted::LongTypeBitsInserted)
                DurableCatalog::get(opCtx)->setIndexKeyStringWithLongTypeBitsExistsOnDisk(opCtx);
        }
        if (isFatalError(opCtx, status, key)) {
            return status;
        }
    }
    if (numInserted) {
        *numInserted = keys.size();
    }
    return Status::OK();
}

void AbstractIndexAccessMethod::removeOneKey(OperationContext* opCtx,
                                             const BSONObj& key,
                                             const RecordId& loc,
                                             bool dupsAllowed) {

    try {
        _newInterface->unindex(opCtx, key, loc, dupsAllowed);
    } catch (AssertionException& e) {
        log() << "Assertion failure: _unindex failed " << _descriptor->indexNamespace();
        log() << "Assertion failure: _unindex failed: " << redact(e) << "  key:" << key.toString()
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
                                             const std::vector<BSONObj>& keys,
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
    BSONObjSet keys = SimpleBSONObjComparator::kInstance.makeBSONObjSet();
    // There's no need to compute the prefixes of the indexed fields that cause the index to be
    // multikey when paging a document's index entries into memory.
    BSONObjSet* multikeyMetadataKeys = nullptr;
    MultikeyPaths* multikeyPaths = nullptr;
    getKeys(obj,
            GetKeysMode::kEnforceConstraints,
            GetKeysContext::kReadOrAddKeys,
            &keys,
            multikeyMetadataKeys,
            multikeyPaths);

    std::unique_ptr<SortedDataInterface::Cursor> cursor(_newInterface->newCursor(opCtx));
    for (const auto& key : keys) {
        cursor->seekExact(key);
    }

    return Status::OK();
}


Status AbstractIndexAccessMethod::touch(OperationContext* opCtx) const {
    return _newInterface->touch(opCtx);
}

RecordId AbstractIndexAccessMethod::findSingle(OperationContext* opCtx,
                                               const BSONObj& requestedKey) const {
    // Generate the key for this index.
    BSONObj actualKey;
    if (_btreeState->getCollator()) {
        // For performance, call get keys only if there is a non-simple collation.
        BSONObjSet keys = SimpleBSONObjComparator::kInstance.makeBSONObjSet();
        BSONObjSet* multikeyMetadataKeys = nullptr;
        MultikeyPaths* multikeyPaths = nullptr;
        getKeys(requestedKey,
                GetKeysMode::kEnforceConstraints,
                GetKeysContext::kReadOrAddKeys,
                &keys,
                multikeyMetadataKeys,
                multikeyPaths);
        invariant(keys.size() == 1);
        actualKey = *keys.begin();
    } else {
        actualKey = requestedKey;
    }

    std::unique_ptr<SortedDataInterface::Cursor> cursor(_newInterface->newCursor(opCtx));
    const auto requestedInfo = kDebugBuild ? SortedDataInterface::Cursor::kKeyAndLoc
                                           : SortedDataInterface::Cursor::kWantLoc;
    if (auto kv = cursor->seekExact(actualKey, requestedInfo)) {
        // StorageEngine should guarantee these.
        dassert(!kv->loc.isNull());
        dassert(kv->key.woCompare(actualKey, /*order*/ BSONObj(), /*considerFieldNames*/ false) ==
                0);

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

pair<vector<BSONObj>, vector<BSONObj>> AbstractIndexAccessMethod::setDifference(
    const BSONObjSet& left, const BSONObjSet& right) {
    // Two iterators to traverse the two sets in sorted order.
    auto leftIt = left.begin();
    auto rightIt = right.begin();
    vector<BSONObj> onlyLeft;
    vector<BSONObj> onlyRight;

    while (leftIt != left.end() && rightIt != right.end()) {
        const int cmp = leftIt->woCompare(*rightIt);
        if (cmp == 0) {
            // 'leftIt' and 'rightIt' compare equal using woCompare(), but may not be identical,
            // which should result in an index change.
            if (!leftIt->binaryEqual(*rightIt)) {
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
        getKeys(
            from, getKeysMode, GetKeysContext::kRemovingKeys, &ticket->oldKeys, nullptr, nullptr);
    }

    if (!indexFilter || indexFilter->matchesBSON(to)) {
        getKeys(to,
                options.getKeysMode,
                GetKeysContext::kReadOrAddKeys,
                &ticket->newKeys,
                &ticket->newMultikeyMetadataKeys,
                &ticket->newMultikeyPaths);
    }

    ticket->loc = record;
    ticket->dupsAllowed = options.dupsAllowed;

    std::tie(ticket->removed, ticket->added) = setDifference(ticket->oldKeys, ticket->newKeys);

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
        _newInterface->unindex(opCtx, remKey, ticket.loc, ticket.dupsAllowed);
    }

    bool checkIndexKeySize = shouldCheckIndexKeySize(opCtx);

    // Add all new data keys into the index with the specified RecordId.
    for (const auto& key : ticket.added) {
        Status status = checkIndexKeySize ? checkKeySize(key) : Status::OK();
        if (status.isOK()) {
            StatusWith<SpecialFormatInserted> ret =
                _newInterface->insert(opCtx, key, ticket.loc, ticket.dupsAllowed);
            status = ret.getStatus();
            if (status.isOK() && ret.getValue() == SpecialFormatInserted::LongTypeBitsInserted)
                DurableCatalog::get(opCtx)->setIndexKeyStringWithLongTypeBitsExistsOnDisk(opCtx);
        }
        if (isFatalError(opCtx, status, key)) {
            return status;
        }
    }

    // If these keys should cause the index to become multikey, pass them into the catalog.
    if (shouldMarkIndexAsMultikey(
            {ticket.newKeys.begin(), ticket.newKeys.end()},
            {ticket.newMultikeyMetadataKeys.begin(), ticket.newMultikeyMetadataKeys.end()},
            ticket.newMultikeyPaths)) {
        _btreeState->setMultikey(
            opCtx,
            {ticket.newMultikeyMetadataKeys.begin(), ticket.newMultikeyMetadataKeys.end()},
            ticket.newMultikeyPaths);
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
    BSONObjSet _multikeyMetadataKeys{SimpleBSONObjComparator::kInstance.makeBSONObjSet()};
};

std::unique_ptr<IndexAccessMethod::BulkBuilder> AbstractIndexAccessMethod::initiateBulk(
    size_t maxMemoryUsageBytes) {
    return std::make_unique<BulkBuilderImpl>(this, _descriptor, maxMemoryUsageBytes);
}

AbstractIndexAccessMethod::BulkBuilderImpl::BulkBuilderImpl(const IndexAccessMethod* index,
                                                            const IndexDescriptor* descriptor,
                                                            size_t maxMemoryUsageBytes)
    : _sorter(Sorter::make(
          SortOptions()
              .TempDir(storageGlobalParams.dbpath + "/_tmp")
              .ExtSortAllowed()
              .MaxMemoryUsageBytes(maxMemoryUsageBytes),
          BtreeExternalSortComparison(descriptor->keyPattern(), descriptor->version()))),
      _real(index) {}

Status AbstractIndexAccessMethod::BulkBuilderImpl::insert(OperationContext* opCtx,
                                                          const BSONObj& obj,
                                                          const RecordId& loc,
                                                          const InsertDeleteOptions& options) {
    BSONObjSet keys = SimpleBSONObjComparator::kInstance.makeBSONObjSet();
    MultikeyPaths multikeyPaths;

    try {
        _real->getKeys(obj,
                       options.getKeysMode,
                       GetKeysContext::kReadOrAddKeys,
                       &keys,
                       &_multikeyMetadataKeys,
                       &multikeyPaths);
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

    for (const auto& key : keys) {
        _sorter->add(key, loc);
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
    for (const auto& key : _multikeyMetadataKeys) {
        _sorter->add(key, kMultikeyMetadataKeyId);
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
                                             KeyHandlerFn&& onDuplicateKey,
                                             set<RecordId>* dupRecords) {
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

    // We need to check the index key size possibly on a primary-started index build; never on a
    // replicated index build.
    bool checkIndexKeySize = shouldCheckIndexKeySize(opCtx);

    BSONObj previousKey;
    const Ordering ordering = Ordering::make(_descriptor->keyPattern());

    while (it->more()) {
        opCtx->checkForInterrupt();

        WriteUnitOfWork wunit(opCtx);

        // Get the next datum and add it to the builder.
        BulkBuilder::Sorter::Data data = it->next();

        // Before attempting to insert, perform a duplicate key check.
        bool isDup = false;
        if (_descriptor->unique()) {
            isDup = data.first.woCompare(previousKey, ordering) == 0;
            if (isDup && !dupsAllowed) {
                if (dupRecords) {
                    dupRecords->insert(data.second);
                    continue;
                }
                return buildDupKeyErrorStatus(data.first,
                                              _descriptor->parentNS(),
                                              _descriptor->indexName(),
                                              _descriptor->keyPattern(),
                                              _descriptor->collation());
            }
        }

        Status status = checkIndexKeySize ? checkKeySize(data.first) : Status::OK();
        if (status.isOK()) {
            StatusWith<SpecialFormatInserted> ret = builder->addKey(data.first, data.second);
            status = ret.getStatus();
            if (status.isOK() && ret.getValue() == SpecialFormatInserted::LongTypeBitsInserted)
                DurableCatalog::get(opCtx)->setIndexKeyStringWithLongTypeBitsExistsOnDisk(opCtx);
        }

        if (!status.isOK()) {
            // Duplicates are checked before inserting.
            invariant(status.code() != ErrorCodes::DuplicateKey);

            // Overlong key that's OK to skip?
            // TODO SERVER-36385: Remove this when there is no KeyTooLong error.
            if (status.code() == ErrorCodes::KeyTooLong && ignoreKeyTooLong()) {
                continue;
            }

            return status;
        }

        previousKey = data.first.getOwned();

        if (isDup) {
            status = onDuplicateKey(data.first);
            if (!status.isOK())
                return status;
        }

        // If we're here either it's a dup and we're cool with it or the addKey went just fine.
        pm.hit();
        wunit.commit();
    }

    pm.finished();

    log() << "index build: inserted " << bulk->getKeysInserted()
          << " keys from external sorter into index in " << timer.seconds() << " seconds";

    WriteUnitOfWork wunit(opCtx);
    SpecialFormatInserted specialFormatInserted = builder->commit(true /* mayInterrupt */);
    // It's ok to insert KeyStrings with long TypeBits but we need to mark the feature
    // tracker bit so that downgrade binary which cannot read the long TypeBits fails to
    // start up.
    if (specialFormatInserted == SpecialFormatInserted::LongTypeBitsInserted)
        DurableCatalog::get(opCtx)->setIndexKeyStringWithLongTypeBitsExistsOnDisk(opCtx);
    wunit.commit();
    return Status::OK();
}

void AbstractIndexAccessMethod::setIndexIsMultikey(OperationContext* opCtx,
                                                   std::vector<BSONObj> multikeyMetadataKeys,
                                                   MultikeyPaths paths) {
    _btreeState->setMultikey(opCtx, multikeyMetadataKeys, paths);
}

void AbstractIndexAccessMethod::getKeys(const BSONObj& obj,
                                        GetKeysMode mode,
                                        GetKeysContext context,
                                        BSONObjSet* keys,
                                        BSONObjSet* multikeyMetadataKeys,
                                        MultikeyPaths* multikeyPaths) const {
    // TODO SERVER-36385: Remove ErrorCodes::KeyTooLong.
    static stdx::unordered_set<int> whiteList{ErrorCodes::CannotBuildIndexKeys,
                                              // Btree
                                              ErrorCodes::KeyTooLong,
                                              ErrorCodes::CannotIndexParallelArrays,
                                              // FTS
                                              16732,
                                              16733,
                                              16675,
                                              17261,
                                              17262,
                                              // Hash
                                              16766,
                                              // Ambiguous array field name
                                              16746,
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
        doGetKeys(obj, context, keys, multikeyMetadataKeys, multikeyPaths);
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
    const vector<BSONObj>& keys,
    const vector<BSONObj>& multikeyMetadataKeys,
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
MONGO_CREATE_SORTER(mongo::BSONObj, mongo::RecordId, mongo::BtreeExternalSortComparison);
