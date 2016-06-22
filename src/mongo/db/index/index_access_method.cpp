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
    BtreeExternalSortComparison(const BSONObj& ordering, int version)
        : _ordering(Ordering::make(ordering)), _version(version) {
        invariant(version == 1 || version == 0);
    }

    typedef std::pair<BSONObj, RecordId> Data;

    int operator()(const Data& l, const Data& r) const {
        int x = (_version == 1 ? l.first.woCompare(r.first, _ordering, /*considerfieldname*/ false)
                               : oldCompare(l.first, r.first, _ordering));
        if (x) {
            return x;
        }
        return l.second.compare(r.second);
    }

private:
    const Ordering _ordering;
    const int _version;
};

IndexAccessMethod::IndexAccessMethod(IndexCatalogEntry* btreeState, SortedDataInterface* btree)
    : _btreeState(btreeState), _descriptor(btreeState->descriptor()), _newInterface(btree) {
    verify(0 == _descriptor->version() || 1 == _descriptor->version());
}

bool IndexAccessMethod::ignoreKeyTooLong(OperationContext* txn) {
    // Ignore this error if we're on a secondary or if the user requested it
    const auto canAcceptWritesForNs = repl::ReplicationCoordinator::get(txn)->canAcceptWritesFor(
        NamespaceString(_btreeState->ns()));
    return !canAcceptWritesForNs || !failIndexKeyTooLong;
}

// Find the keys for obj, put them in the tree pointing to loc
Status IndexAccessMethod::insert(OperationContext* txn,
                                 const BSONObj& obj,
                                 const RecordId& loc,
                                 const InsertDeleteOptions& options,
                                 int64_t* numInserted) {
    invariant(numInserted);
    *numInserted = 0;
    BSONObjSet keys;
    MultikeyPaths multikeyPaths;
    // Delegate to the subclass.
    getKeys(obj, &keys, &multikeyPaths);

    Status ret = Status::OK();
    for (BSONObjSet::const_iterator i = keys.begin(); i != keys.end(); ++i) {
        Status status = _newInterface->insert(txn, *i, loc, options.dupsAllowed);

        // Everything's OK, carry on.
        if (status.isOK()) {
            ++*numInserted;
            continue;
        }

        // Error cases.

        if (status.code() == ErrorCodes::KeyTooLong && ignoreKeyTooLong(txn)) {
            continue;
        }

        if (status.code() == ErrorCodes::DuplicateKeyValue) {
            // A document might be indexed multiple times during a background index build
            // if it moves ahead of the collection scan cursor (e.g. via an update).
            if (!_btreeState->isReady(txn)) {
                LOG(3) << "key " << *i << " already in index during background indexing (ok)";
                continue;
            }
        }

        // Clean up after ourselves.
        for (BSONObjSet::const_iterator j = keys.begin(); j != i; ++j) {
            removeOneKey(txn, *j, loc, options.dupsAllowed);
            *numInserted = 0;
        }

        return status;
    }

    if (*numInserted > 1 || isMultikeyFromPaths(multikeyPaths)) {
        _btreeState->setMultikey(txn, multikeyPaths);
    }

    return ret;
}

void IndexAccessMethod::removeOneKey(OperationContext* txn,
                                     const BSONObj& key,
                                     const RecordId& loc,
                                     bool dupsAllowed) {
    try {
        _newInterface->unindex(txn, key, loc, dupsAllowed);
    } catch (AssertionException& e) {
        log() << "Assertion failure: _unindex failed " << _descriptor->indexNamespace() << endl;
        log() << "Assertion failure: _unindex failed: " << e.what() << "  key:" << key.toString()
              << "  dl:" << loc;
        logContext();
    }
}

std::unique_ptr<SortedDataInterface::Cursor> IndexAccessMethod::newCursor(OperationContext* txn,
                                                                          bool isForward) const {
    return _newInterface->newCursor(txn, isForward);
}

std::unique_ptr<SortedDataInterface::Cursor> IndexAccessMethod::newRandomCursor(
    OperationContext* txn) const {
    return _newInterface->newRandomCursor(txn);
}

// Remove the provided doc from the index.
Status IndexAccessMethod::remove(OperationContext* txn,
                                 const BSONObj& obj,
                                 const RecordId& loc,
                                 const InsertDeleteOptions& options,
                                 int64_t* numDeleted) {
    invariant(numDeleted);
    *numDeleted = 0;
    BSONObjSet keys;
    // There's no need to compute the prefixes of the indexed fields that cause the index to be
    // multikey when removing a document since the index metadata isn't updated when keys are
    // deleted.
    MultikeyPaths* multikeyPaths = nullptr;
    getKeys(obj, &keys, multikeyPaths);

    for (BSONObjSet::const_iterator i = keys.begin(); i != keys.end(); ++i) {
        removeOneKey(txn, *i, loc, options.dupsAllowed);
        ++*numDeleted;
    }

    return Status::OK();
}

Status IndexAccessMethod::initializeAsEmpty(OperationContext* txn) {
    return _newInterface->initAsEmpty(txn);
}

Status IndexAccessMethod::touch(OperationContext* txn, const BSONObj& obj) {
    BSONObjSet keys;
    // There's no need to compute the prefixes of the indexed fields that cause the index to be
    // multikey when paging a document's index entries into memory.
    MultikeyPaths* multikeyPaths = nullptr;
    getKeys(obj, &keys, multikeyPaths);

    std::unique_ptr<SortedDataInterface::Cursor> cursor(_newInterface->newCursor(txn));
    for (BSONObjSet::const_iterator i = keys.begin(); i != keys.end(); ++i) {
        cursor->seekExact(*i);
    }

    return Status::OK();
}


Status IndexAccessMethod::touch(OperationContext* txn) const {
    return _newInterface->touch(txn);
}

RecordId IndexAccessMethod::findSingle(OperationContext* txn, const BSONObj& key) const {
    // Generate the key for this index.
    BSONObjSet keys;
    MultikeyPaths* multikeyPaths = nullptr;
    getKeys(key, &keys, multikeyPaths);
    invariant(keys.size() == 1);

    std::unique_ptr<SortedDataInterface::Cursor> cursor(_newInterface->newCursor(txn));
    const auto requestedInfo = kDebugBuild ? SortedDataInterface::Cursor::kKeyAndLoc
                                           : SortedDataInterface::Cursor::kWantLoc;
    if (auto kv = cursor->seekExact(*keys.begin(), requestedInfo)) {
        // StorageEngine should guarantee these.
        dassert(!kv->loc.isNull());
        dassert(kv->key.woCompare(
                    *keys.begin(), /*order*/ BSONObj(), /*considerFieldNames*/ false) == 0);

        return kv->loc;
    }

    return RecordId();
}

Status IndexAccessMethod::validate(OperationContext* txn,
                                   int64_t* numKeys,
                                   ValidateResults* fullResults) {
    long long keys = 0;
    _newInterface->fullValidate(txn, &keys, fullResults);
    *numKeys = keys;
    return Status::OK();
}

bool IndexAccessMethod::appendCustomStats(OperationContext* txn,
                                          BSONObjBuilder* output,
                                          double scale) const {
    return _newInterface->appendCustomStats(txn, output, scale);
}

long long IndexAccessMethod::getSpaceUsedBytes(OperationContext* txn) const {
    return _newInterface->getSpaceUsedBytes(txn);
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

Status IndexAccessMethod::validateUpdate(OperationContext* txn,
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
        getKeys(from, &ticket->oldKeys, multikeyPaths);
    }

    if (!indexFilter || indexFilter->matchesBSON(to)) {
        getKeys(to, &ticket->newKeys, &ticket->newMultikeyPaths);
    }

    ticket->loc = record;
    ticket->dupsAllowed = options.dupsAllowed;

    std::tie(ticket->removed, ticket->added) = setDifference(ticket->oldKeys, ticket->newKeys);

    ticket->_isValid = true;

    return Status::OK();
}

Status IndexAccessMethod::update(OperationContext* txn,
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
        _btreeState->setMultikey(txn, ticket.newMultikeyPaths);
    }

    for (size_t i = 0; i < ticket.removed.size(); ++i) {
        _newInterface->unindex(txn, ticket.removed[i], ticket.loc, ticket.dupsAllowed);
    }

    for (size_t i = 0; i < ticket.added.size(); ++i) {
        Status status = _newInterface->insert(txn, ticket.added[i], ticket.loc, ticket.dupsAllowed);
        if (!status.isOK()) {
            if (status.code() == ErrorCodes::KeyTooLong && ignoreKeyTooLong(txn)) {
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

Status IndexAccessMethod::compact(OperationContext* txn) {
    return this->_newInterface->compact(txn);
}

std::unique_ptr<IndexAccessMethod::BulkBuilder> IndexAccessMethod::initiateBulk() {
    return std::unique_ptr<BulkBuilder>(new BulkBuilder(this, _descriptor));
}

IndexAccessMethod::BulkBuilder::BulkBuilder(const IndexAccessMethod* index,
                                            const IndexDescriptor* descriptor)
    : _sorter(Sorter::make(
          SortOptions()
              .TempDir(storageGlobalParams.dbpath + "/_tmp")
              .ExtSortAllowed()
              .MaxMemoryUsageBytes(100 * 1024 * 1024),
          BtreeExternalSortComparison(descriptor->keyPattern(), descriptor->version()))),
      _real(index) {}

Status IndexAccessMethod::BulkBuilder::insert(OperationContext* txn,
                                              const BSONObj& obj,
                                              const RecordId& loc,
                                              const InsertDeleteOptions& options,
                                              int64_t* numInserted) {
    BSONObjSet keys;
    MultikeyPaths multikeyPaths;
    _real->getKeys(obj, &keys, &multikeyPaths);

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


Status IndexAccessMethod::commitBulk(OperationContext* txn,
                                     std::unique_ptr<BulkBuilder> bulk,
                                     bool mayInterrupt,
                                     bool dupsAllowed,
                                     set<RecordId>* dupsToDrop) {
    Timer timer;

    std::unique_ptr<BulkBuilder::Sorter::Iterator> i(bulk->_sorter->done());

    stdx::unique_lock<Client> lk(*txn->getClient());
    ProgressMeterHolder pm(*txn->setMessage_inlock("Index Bulk Build: (2/3) btree bottom up",
                                                   "Index: (2/3) BTree Bottom Up Progress",
                                                   bulk->_keysInserted,
                                                   10));
    lk.unlock();

    std::unique_ptr<SortedDataBuilderInterface> builder;

    MONGO_WRITE_CONFLICT_RETRY_LOOP_BEGIN {
        WriteUnitOfWork wunit(txn);

        if (bulk->_everGeneratedMultipleKeys || isMultikeyFromPaths(bulk->_indexMultikeyPaths)) {
            _btreeState->setMultikey(txn, bulk->_indexMultikeyPaths);
        }

        builder.reset(_newInterface->getBulkBuilder(txn, dupsAllowed));
        wunit.commit();
    }
    MONGO_WRITE_CONFLICT_RETRY_LOOP_END(txn, "setting index multikey flag", "");

    while (i->more()) {
        if (mayInterrupt) {
            txn->checkForInterrupt();
        }

        WriteUnitOfWork wunit(txn);
        // Improve performance in the btree-building phase by disabling rollback tracking.
        // This avoids copying all the written bytes to a buffer that is only used to roll back.
        // Note that this is safe to do, as this entire index-build-in-progress will be cleaned
        // up by the index system.
        txn->recoveryUnit()->setRollbackWritesDisabled();

        // Get the next datum and add it to the builder.
        BulkBuilder::Sorter::Data d = i->next();
        Status status = builder->addKey(d.first, d.second);

        if (!status.isOK()) {
            // Overlong key that's OK to skip?
            if (status.code() == ErrorCodes::KeyTooLong && ignoreKeyTooLong(txn)) {
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
        stdx::lock_guard<Client> lk(*txn->getClient());
        CurOp::get(txn)->setMessage_inlock("Index Bulk Build: (3/3) btree-middle",
                                           "Index: (3/3) BTree Middle Progress");
    }

    LOG(timer.seconds() > 10 ? 0 : 1) << "\t done building bottom layer, going to commit";

    builder->commit(mayInterrupt);
    return Status::OK();
}

}  // namespace mongo

#include "mongo/db/sorter/sorter.cpp"
MONGO_CREATE_SORTER(mongo::BSONObj, mongo::RecordId, mongo::BtreeExternalSortComparison);
