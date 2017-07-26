/**
*    Copyright (C) 2013-2014 MongoDB Inc.
*
*    This program is free software: you can redistribute it and/or  modify
*    it under the terms of the GNU Affero General Public License, version 3,
*    as published by the Free Software Foundation.
*
*    This program is distributed in the hope that it will be useful,
*    but WITHOUT ANY WARRANTY; without even the implied warranty of
*    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
*    GNU Affero General Public License for more details.
*
*    You should have received a copy of the GNU Affero General Public License
*    along with this program.  If not, see <http://www.gnu.org/licenses/>.
*
*    As a special exception, the copyright holders give permission to link the
*    code of portions of this program with the OpenSSL library under certain
*    conditions as described in each individual source file and distribute
*    linked combinations including the program with the OpenSSL library. You
*    must comply with the GNU Affero General Public License in all respects for
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
#include "mongo/db/client.h"
#include "mongo/db/concurrency/write_conflict_exception.h"
#include "mongo/db/curop.h"
#include "mongo/db/index/index_descriptor.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/keypattern.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/server_parameters.h"
#include "mongo/db/storage/storage_options.h"
#include "mongo/util/log.h"
#include "mongo/util/progress_meter.h"

namespace mongo {

using std::endl;
using std::pair;
using std::set;
using std::vector;

using IndexVersion = IndexDescriptor::IndexVersion;

namespace {

/**
 * Returns true if at least one prefix of any of the indexed fields causes the index to be multikey,
 * and returns false otherwise. This function returns false if the 'multikeyPaths' vector is empty.
 */
bool isMultikeyFromPaths(const MultikeyPaths& multikeyPaths) {
    return std::any_of(multikeyPaths.cbegin(),
                       multikeyPaths.cend(),
                       [](const std::set<std::size_t>& components) { return !components.empty(); });
}

}  // namespace

MONGO_EXPORT_SERVER_PARAMETER(failIndexKeyTooLong, bool, true);

//
// Comparison for external sorter interface
//

// Defined in db/structure/btree/key.cpp
// XXX TODO: rename to something more descriptive, etc. etc.
int oldCompare(const BSONObj& l, const BSONObj& r, const Ordering& o);

class BtreeExternalSortComparison {
public:
    BtreeExternalSortComparison(const BSONObj& ordering, IndexVersion version)
        : _ordering(Ordering::make(ordering)), _version(version) {
        invariant(IndexDescriptor::isIndexVersionSupported(version));
    }

    typedef std::pair<BSONObj, RecordId> Data;

    int operator()(const Data& l, const Data& r) const {
        int x = (_version == IndexVersion::kV0
                     ? oldCompare(l.first, r.first, _ordering)
                     : l.first.woCompare(r.first, _ordering, /*considerfieldname*/ false));
        if (x) {
            return x;
        }
        return l.second.compare(r.second);
    }

private:
    const Ordering _ordering;
    const IndexVersion _version;
};

IndexAccessMethod::IndexAccessMethod(IndexCatalogEntry* btreeState, SortedDataInterface* btree)
    : _btreeState(btreeState), _descriptor(btreeState->descriptor()), _newInterface(btree) {
    verify(IndexDescriptor::isIndexVersionSupported(_descriptor->version()));
}

bool IndexAccessMethod::ignoreKeyTooLong(OperationContext* opCtx) {
    // Ignore this error if we cannot write to the collection or if the user requested it
    const auto shouldRelaxConstraints =
        repl::ReplicationCoordinator::get(opCtx)->shouldRelaxIndexConstraints(
            opCtx, NamespaceString(_btreeState->ns()));
    return shouldRelaxConstraints || !failIndexKeyTooLong.load();
}

// Find the keys for obj, put them in the tree pointing to loc
Status IndexAccessMethod::insert(OperationContext* opCtx,
                                 const BSONObj& obj,
                                 const RecordId& loc,
                                 const InsertDeleteOptions& options,
                                 int64_t* numInserted) {
    invariant(numInserted);
    *numInserted = 0;
    BSONObjSet keys = SimpleBSONObjComparator::kInstance.makeBSONObjSet();
    MultikeyPaths multikeyPaths;
    // Delegate to the subclass.
    getKeys(obj, options.getKeysMode, &keys, &multikeyPaths);

    Status ret = Status::OK();
    for (BSONObjSet::const_iterator i = keys.begin(); i != keys.end(); ++i) {
        Status status = _newInterface->insert(opCtx, *i, loc, options.dupsAllowed);

        // Everything's OK, carry on.
        if (status.isOK()) {
            ++*numInserted;
            continue;
        }

        // Error cases.

        if (status.code() == ErrorCodes::KeyTooLong && ignoreKeyTooLong(opCtx)) {
            continue;
        }

        if (status.code() == ErrorCodes::DuplicateKeyValue) {
            // A document might be indexed multiple times during a background index build
            // if it moves ahead of the collection scan cursor (e.g. via an update).
            if (!_btreeState->isReady(opCtx)) {
                LOG(3) << "key " << *i << " already in index during background indexing (ok)";
                continue;
            }
        }

        // Clean up after ourselves.
        for (BSONObjSet::const_iterator j = keys.begin(); j != i; ++j) {
            removeOneKey(opCtx, *j, loc, options.dupsAllowed);
            *numInserted = 0;
        }

        return status;
    }

    if (*numInserted > 1 || isMultikeyFromPaths(multikeyPaths)) {
        _btreeState->setMultikey(opCtx, multikeyPaths);
    }

    return ret;
}

void IndexAccessMethod::removeOneKey(OperationContext* opCtx,
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

std::unique_ptr<SortedDataInterface::Cursor> IndexAccessMethod::newCursor(OperationContext* opCtx,
                                                                          bool isForward) const {
    return _newInterface->newCursor(opCtx, isForward);
}

std::unique_ptr<SortedDataInterface::Cursor> IndexAccessMethod::newRandomCursor(
    OperationContext* opCtx) const {
    return _newInterface->newRandomCursor(opCtx);
}

// Remove the provided doc from the index.
Status IndexAccessMethod::remove(OperationContext* opCtx,
                                 const BSONObj& obj,
                                 const RecordId& loc,
                                 const InsertDeleteOptions& options,
                                 int64_t* numDeleted) {
    invariant(numDeleted);
    *numDeleted = 0;
    BSONObjSet keys = SimpleBSONObjComparator::kInstance.makeBSONObjSet();
    // There's no need to compute the prefixes of the indexed fields that cause the index to be
    // multikey when removing a document since the index metadata isn't updated when keys are
    // deleted.
    MultikeyPaths* multikeyPaths = nullptr;
    getKeys(obj, options.getKeysMode, &keys, multikeyPaths);

    for (BSONObjSet::const_iterator i = keys.begin(); i != keys.end(); ++i) {
        removeOneKey(opCtx, *i, loc, options.dupsAllowed);
        ++*numDeleted;
    }

    return Status::OK();
}

Status IndexAccessMethod::initializeAsEmpty(OperationContext* opCtx) {
    return _newInterface->initAsEmpty(opCtx);
}

Status IndexAccessMethod::touch(OperationContext* opCtx, const BSONObj& obj) {
    BSONObjSet keys = SimpleBSONObjComparator::kInstance.makeBSONObjSet();
    // There's no need to compute the prefixes of the indexed fields that cause the index to be
    // multikey when paging a document's index entries into memory.
    MultikeyPaths* multikeyPaths = nullptr;
    getKeys(obj, GetKeysMode::kEnforceConstraints, &keys, multikeyPaths);

    std::unique_ptr<SortedDataInterface::Cursor> cursor(_newInterface->newCursor(opCtx));
    for (BSONObjSet::const_iterator i = keys.begin(); i != keys.end(); ++i) {
        cursor->seekExact(*i);
    }

    return Status::OK();
}


Status IndexAccessMethod::touch(OperationContext* opCtx) const {
    return _newInterface->touch(opCtx);
}

RecordId IndexAccessMethod::findSingle(OperationContext* opCtx, const BSONObj& requestedKey) const {
    // Generate the key for this index.
    BSONObj actualKey;
    if (_btreeState->getCollator()) {
        // For performance, call get keys only if there is a non-simple collation.
        BSONObjSet keys = SimpleBSONObjComparator::kInstance.makeBSONObjSet();
        MultikeyPaths* multikeyPaths = nullptr;
        getKeys(requestedKey, GetKeysMode::kEnforceConstraints, &keys, multikeyPaths);
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

void IndexAccessMethod::validate(OperationContext* opCtx,
                                 int64_t* numKeys,
                                 ValidateResults* fullResults) {
    long long keys = 0;
    _newInterface->fullValidate(opCtx, &keys, fullResults);
    *numKeys = keys;
}

bool IndexAccessMethod::appendCustomStats(OperationContext* opCtx,
                                          BSONObjBuilder* output,
                                          double scale) const {
    return _newInterface->appendCustomStats(opCtx, output, scale);
}

long long IndexAccessMethod::getSpaceUsedBytes(OperationContext* opCtx) const {
    return _newInterface->getSpaceUsedBytes(opCtx);
}

pair<vector<BSONObj>, vector<BSONObj>> IndexAccessMethod::setDifference(const BSONObjSet& left,
                                                                        const BSONObjSet& right) {
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

Status IndexAccessMethod::validateUpdate(OperationContext* opCtx,
                                         const BSONObj& from,
                                         const BSONObj& to,
                                         const RecordId& record,
                                         const InsertDeleteOptions& options,
                                         UpdateTicket* ticket,
                                         const MatchExpression* indexFilter) {
    if (!indexFilter || indexFilter->matchesBSON(from)) {
        // There's no need to compute the prefixes of the indexed fields that possibly caused the
        // index to be multikey when the old version of the document was written since the index
        // metadata isn't updated when keys are deleted.
        MultikeyPaths* multikeyPaths = nullptr;
        getKeys(from, options.getKeysMode, &ticket->oldKeys, multikeyPaths);
    }

    if (!indexFilter || indexFilter->matchesBSON(to)) {
        getKeys(to, options.getKeysMode, &ticket->newKeys, &ticket->newMultikeyPaths);
    }

    ticket->loc = record;
    ticket->dupsAllowed = options.dupsAllowed;

    std::tie(ticket->removed, ticket->added) = setDifference(ticket->oldKeys, ticket->newKeys);

    ticket->_isValid = true;

    return Status::OK();
}

Status IndexAccessMethod::update(OperationContext* opCtx,
                                 const UpdateTicket& ticket,
                                 int64_t* numInserted,
                                 int64_t* numDeleted) {
    invariant(numInserted);
    invariant(numDeleted);

    *numInserted = 0;
    *numDeleted = 0;

    if (!ticket._isValid) {
        return Status(ErrorCodes::InternalError, "Invalid UpdateTicket in update");
    }

    if (ticket.oldKeys.size() + ticket.added.size() - ticket.removed.size() > 1 ||
        isMultikeyFromPaths(ticket.newMultikeyPaths)) {
        _btreeState->setMultikey(opCtx, ticket.newMultikeyPaths);
    }

    for (size_t i = 0; i < ticket.removed.size(); ++i) {
        _newInterface->unindex(opCtx, ticket.removed[i], ticket.loc, ticket.dupsAllowed);
    }

    for (size_t i = 0; i < ticket.added.size(); ++i) {
        Status status =
            _newInterface->insert(opCtx, ticket.added[i], ticket.loc, ticket.dupsAllowed);
        if (!status.isOK()) {
            if (status.code() == ErrorCodes::KeyTooLong && ignoreKeyTooLong(opCtx)) {
                // Ignore.
                continue;
            }

            return status;
        }
    }

    *numInserted = ticket.added.size();
    *numDeleted = ticket.removed.size();

    return Status::OK();
}

Status IndexAccessMethod::compact(OperationContext* opCtx) {
    return this->_newInterface->compact(opCtx);
}

std::unique_ptr<IndexAccessMethod::BulkBuilder> IndexAccessMethod::initiateBulk(
    size_t maxMemoryUsageBytes) {
    return std::unique_ptr<BulkBuilder>(new BulkBuilder(this, _descriptor, maxMemoryUsageBytes));
}

IndexAccessMethod::BulkBuilder::BulkBuilder(const IndexAccessMethod* index,
                                            const IndexDescriptor* descriptor,
                                            size_t maxMemoryUsageBytes)
    : _sorter(Sorter::make(
          SortOptions()
              .TempDir(storageGlobalParams.dbpath + "/_tmp")
              .ExtSortAllowed()
              .MaxMemoryUsageBytes(maxMemoryUsageBytes),
          BtreeExternalSortComparison(descriptor->keyPattern(), descriptor->version()))),
      _real(index) {}

Status IndexAccessMethod::BulkBuilder::insert(OperationContext* opCtx,
                                              const BSONObj& obj,
                                              const RecordId& loc,
                                              const InsertDeleteOptions& options,
                                              int64_t* numInserted) {
    BSONObjSet keys = SimpleBSONObjComparator::kInstance.makeBSONObjSet();
    MultikeyPaths multikeyPaths;

    _real->getKeys(obj, options.getKeysMode, &keys, &multikeyPaths);

    _everGeneratedMultipleKeys = _everGeneratedMultipleKeys || (keys.size() > 1);

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

    for (BSONObjSet::iterator it = keys.begin(); it != keys.end(); ++it) {
        _sorter->add(*it, loc);
        _keysInserted++;
    }

    if (NULL != numInserted) {
        *numInserted += keys.size();
    }

    return Status::OK();
}


Status IndexAccessMethod::commitBulk(OperationContext* opCtx,
                                     std::unique_ptr<BulkBuilder> bulk,
                                     bool mayInterrupt,
                                     bool dupsAllowed,
                                     set<RecordId>* dupsToDrop) {
    Timer timer;

    std::unique_ptr<BulkBuilder::Sorter::Iterator> i(bulk->_sorter->done());

    stdx::unique_lock<Client> lk(*opCtx->getClient());
    ProgressMeterHolder pm(
        CurOp::get(opCtx)->setMessage_inlock("Index Bulk Build: (2/3) btree bottom up",
                                             "Index: (2/3) BTree Bottom Up Progress",
                                             bulk->_keysInserted,
                                             10));
    lk.unlock();

    std::unique_ptr<SortedDataBuilderInterface> builder;

    writeConflictRetry(opCtx, "setting index multikey flag", "", [&] {
        WriteUnitOfWork wunit(opCtx);

        if (bulk->_everGeneratedMultipleKeys || isMultikeyFromPaths(bulk->_indexMultikeyPaths)) {
            _btreeState->setMultikey(opCtx, bulk->_indexMultikeyPaths);
        }

        builder.reset(_newInterface->getBulkBuilder(opCtx, dupsAllowed));
        wunit.commit();
    });

    while (i->more()) {
        if (mayInterrupt) {
            opCtx->checkForInterrupt();
        }

        WriteUnitOfWork wunit(opCtx);
        // Improve performance in the btree-building phase by disabling rollback tracking.
        // This avoids copying all the written bytes to a buffer that is only used to roll back.
        // Note that this is safe to do, as this entire index-build-in-progress will be cleaned
        // up by the index system.
        opCtx->recoveryUnit()->setRollbackWritesDisabled();

        // Get the next datum and add it to the builder.
        BulkBuilder::Sorter::Data d = i->next();
        Status status = builder->addKey(d.first, d.second);

        if (!status.isOK()) {
            // Overlong key that's OK to skip?
            if (status.code() == ErrorCodes::KeyTooLong && ignoreKeyTooLong(opCtx)) {
                continue;
            }

            // Check if this is a duplicate that's OK to skip
            if (status.code() == ErrorCodes::DuplicateKey) {
                invariant(!dupsAllowed);  // shouldn't be getting DupKey errors if dupsAllowed.

                if (dupsToDrop) {
                    dupsToDrop->insert(d.second);
                    continue;
                }
            }

            return status;
        }

        // If we're here either it's a dup and we're cool with it or the addKey went just
        // fine.
        pm.hit();
        wunit.commit();
    }

    pm.finished();

    {
        stdx::lock_guard<Client> lk(*opCtx->getClient());
        CurOp::get(opCtx)->setMessage_inlock("Index Bulk Build: (3/3) btree-middle",
                                             "Index: (3/3) BTree Middle Progress");
    }

    LOG(timer.seconds() > 10 ? 0 : 1) << "\t done building bottom layer, going to commit";

    builder->commit(mayInterrupt);
    return Status::OK();
}

void IndexAccessMethod::getKeys(const BSONObj& obj,
                                GetKeysMode mode,
                                BSONObjSet* keys,
                                MultikeyPaths* multikeyPaths) const {
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
        doGetKeys(obj, keys, multikeyPaths);
    } catch (const UserException& ex) {
        if (mode == GetKeysMode::kEnforceConstraints) {
            throw;
        }

        // Suppress indexing errors when mode is kRelaxConstraints.
        keys->clear();
        if (multikeyPaths) {
            multikeyPaths->clear();
        }
        // Only suppress the errors in the whitelist.
        if (whiteList.find(ex.getCode()) == whiteList.end()) {
            throw;
        }
        LOG(1) << "Ignoring indexing error for idempotency reasons: " << redact(ex)
               << " when getting index keys of " << redact(obj);
    }
}

}  // namespace mongo

#include "mongo/db/sorter/sorter.cpp"
MONGO_CREATE_SORTER(mongo::BSONObj, mongo::RecordId, mongo::BtreeExternalSortComparison);
