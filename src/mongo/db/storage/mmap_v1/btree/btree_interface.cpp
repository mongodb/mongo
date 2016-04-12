/**
 *    Copyright (C) 2014 MongoDB Inc.
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

#include <string>

#include "mongo/db/storage/sorted_data_interface.h"


#include "mongo/db/operation_context.h"
#include "mongo/db/storage/mmap_v1/btree/btree_logic.h"
#include "mongo/db/storage/mmap_v1/record_store_v1_base.h"
#include "mongo/stdx/memory.h"

namespace mongo {
namespace {

using std::unique_ptr;
using std::string;
using std::vector;

template <class OnDiskFormat>
class BtreeBuilderInterfaceImpl final : public SortedDataBuilderInterface {
public:
    BtreeBuilderInterfaceImpl(OperationContext* trans,
                              typename BtreeLogic<OnDiskFormat>::Builder* builder)
        : _builder(builder), _trans(trans) {}

    Status addKey(const BSONObj& key, const RecordId& loc) {
        return _builder->addKey(key, DiskLoc::fromRecordId(loc));
    }

private:
    std::unique_ptr<typename BtreeLogic<OnDiskFormat>::Builder> _builder;

    // Not owned here.
    OperationContext* _trans;
};

template <class OnDiskFormat>
class BtreeInterfaceImpl final : public SortedDataInterface {
public:
    BtreeInterfaceImpl(HeadManager* headManager,
                       RecordStore* recordStore,
                       SavedCursorRegistry* cursorRegistry,
                       const Ordering& ordering,
                       const string& indexName,
                       bool isUnique) {
        _btree.reset(new BtreeLogic<OnDiskFormat>(
            headManager, recordStore, cursorRegistry, ordering, indexName, isUnique));
    }

    virtual ~BtreeInterfaceImpl() {}

    virtual SortedDataBuilderInterface* getBulkBuilder(OperationContext* txn, bool dupsAllowed) {
        return new BtreeBuilderInterfaceImpl<OnDiskFormat>(txn,
                                                           _btree->newBuilder(txn, dupsAllowed));
    }

    virtual Status insert(OperationContext* txn,
                          const BSONObj& key,
                          const RecordId& loc,
                          bool dupsAllowed) {
        return _btree->insert(txn, key, DiskLoc::fromRecordId(loc), dupsAllowed);
    }

    virtual void unindex(OperationContext* txn,
                         const BSONObj& key,
                         const RecordId& loc,
                         bool dupsAllowed) {
        _btree->unindex(txn, key, DiskLoc::fromRecordId(loc));
    }

    virtual void fullValidate(OperationContext* txn,
                              long long* numKeysOut,
                              ValidateResults* fullResults) const {
        *numKeysOut = _btree->fullValidate(txn, NULL, false, false, 0);
    }

    virtual bool appendCustomStats(OperationContext* txn,
                                   BSONObjBuilder* output,
                                   double scale) const {
        return false;
    }

    virtual long long getSpaceUsedBytes(OperationContext* txn) const {
        return _btree->getRecordStore()->dataSize(txn);
    }

    virtual Status dupKeyCheck(OperationContext* txn, const BSONObj& key, const RecordId& loc) {
        return _btree->dupKeyCheck(txn, key, DiskLoc::fromRecordId(loc));
    }

    virtual bool isEmpty(OperationContext* txn) {
        return _btree->isEmpty(txn);
    }

    virtual Status touch(OperationContext* txn) const {
        return _btree->touch(txn);
    }

    class Cursor final : public SortedDataInterface::Cursor {
    public:
        Cursor(OperationContext* txn, const BtreeLogic<OnDiskFormat>* btree, bool forward)
            : _txn(txn), _btree(btree), _direction(forward ? 1 : -1), _ofs(0) {}

        boost::optional<IndexKeyEntry> next(RequestedInfo parts) override {
            if (isEOF())
                return {};
            if (_lastMoveWasRestore) {
                // Return current position rather than advancing.
                _lastMoveWasRestore = false;
            } else {
                _btree->advance(_txn, &_bucket, &_ofs, _direction);
            }

            if (atEndPoint())
                markEOF();
            return curr(parts);
        }

        void setEndPosition(const BSONObj& key, bool inclusive) override {
            if (key.isEmpty()) {
                // This means scan to end of index.
                _endState = boost::none;
                return;
            }

            _endState = {{key, inclusive}};
            seekEndCursor();  // Completes initialization of _endState.
        }

        boost::optional<IndexKeyEntry> seek(const BSONObj& key,
                                            bool inclusive,
                                            RequestedInfo parts) override {
            locate(key, inclusive == forward() ? RecordId::min() : RecordId::max());
            _lastMoveWasRestore = false;

            if (isEOF())
                return {};
            dassert(inclusive ? compareKeys(getKey(), key) >= 0 : compareKeys(getKey(), key) > 0);
            return curr(parts);
        }


        boost::optional<IndexKeyEntry> seek(const IndexSeekPoint& seekPoint,
                                            RequestedInfo parts) override {
            bool canUseAdvanceTo = false;
            if (!isEOF()) {
                int cmp = _btree->customBSONCmp(getKey(), seekPoint, _direction);

                // advanceTo requires that we are positioned "earlier" in the index than the
                // seek point, in scan order.
                canUseAdvanceTo = forward() ? cmp < 0 : cmp > 0;
            }


            if (canUseAdvanceTo) {
                // This takes advantage of current location.
                _btree->advanceTo(_txn, &_bucket, &_ofs, seekPoint, _direction);
            } else {
                // Start at root.
                _bucket = _btree->getHead(_txn);
                _ofs = 0;
                _btree->customLocate(_txn, &_bucket, &_ofs, seekPoint, _direction);
            }

            _lastMoveWasRestore = false;

            if (atOrPastEndPointAfterSeeking())
                markEOF();
            return curr(parts);
        }

        void save() override {
            if (!_lastMoveWasRestore)
                _savedEOF = isEOF();

            if (!isEOF()) {
                _saved.bucket = _bucket;
                _btree->savedCursors()->registerCursor(&_saved);
                // Don't want to change saved position if we only moved during restore.
                if (!_lastMoveWasRestore) {
                    _saved.key = getKey().getOwned();
                    _saved.loc = getDiskLoc();
                }
            }
            // Doing nothing with end cursor since it will do full reseek on restore.
        }

        void saveUnpositioned() override {
            // Don't leak our registration if save() was previously called.
            if (!_saved.bucket.isNull())
                _btree->savedCursors()->unregisterCursor(&_saved);

            _saved.bucket = DiskLoc();
            _savedEOF = true;
        }

        void restore() override {
            // Always do a full seek on restore. We cannot use our last position since index
            // entries may have been inserted closer to our endpoint and we would need to move
            // over them.
            seekEndCursor();

            if (_savedEOF) {
                markEOF();
                return;
            }

            if (_btree->savedCursors()->unregisterCursor(&_saved)) {
                // We can use the fast restore mechanism.
                _btree->restorePosition(_txn, _saved.key, _saved.loc, _direction, &_bucket, &_ofs);
            } else {
                // Need to find our position from the root.
                locate(_saved.key, _saved.loc.toRecordId());
            }

            _lastMoveWasRestore = isEOF()  // We weren't EOF but now are.
                || (!_btree->isUnique() && getDiskLoc() != _saved.loc) ||
                compareKeys(getKey(), _saved.key) != 0;
        }

        void detachFromOperationContext() final {
            _txn = nullptr;
        }

        void reattachToOperationContext(OperationContext* txn) final {
            _txn = txn;
        }

    private:
        bool isEOF() const {
            return _bucket.isNull();
        }
        void markEOF() {
            _bucket = DiskLoc();
        }

        boost::optional<IndexKeyEntry> curr(RequestedInfo parts) {
            if (isEOF())
                return {};
            return {{(parts & kWantKey) ? getKey() : BSONObj(),
                     (parts & kWantLoc) ? getDiskLoc().toRecordId() : RecordId()}};
        }

        bool atEndPoint() const {
            return _endState && _bucket == _endState->bucket && (isEOF() || _ofs == _endState->ofs);
        }

        bool atOrPastEndPointAfterSeeking() const {
            if (!_endState)
                return false;
            if (isEOF())
                return true;

            int cmp = compareKeys(getKey(), _endState->key);
            return _endState->inclusive ? cmp > 0 : cmp >= 0;
        }

        void locate(const BSONObj& key, const RecordId& loc) {
            _btree->locate(_txn, key, DiskLoc::fromRecordId(loc), _direction, &_ofs, &_bucket);
            if (atOrPastEndPointAfterSeeking())
                markEOF();
        }

        // Returns comparison relative to direction of scan. If rhs would be seen later, returns
        // a positive value.
        int compareKeys(const BSONObj& lhs, const BSONObj& rhs) const {
            int cmp = lhs.woCompare(rhs, _btree->ordering(), /*considerFieldName*/ false);
            return forward() ? cmp : -cmp;
        }

        BSONObj getKey() const {
            return _btree->getKey(_txn, _bucket, _ofs);
        }
        DiskLoc getDiskLoc() const {
            return _btree->getDiskLoc(_txn, _bucket, _ofs);
        }

        void seekEndCursor() {
            if (!_endState)
                return;
            _btree->locate(_txn,
                           _endState->key,
                           forward() == _endState->inclusive ? DiskLoc::max() : DiskLoc::min(),
                           _direction,
                           &_endState->ofs,
                           &_endState->bucket);  // pure out params.
        }

        bool forward() const {
            return _direction == 1;
        }

        OperationContext* _txn;  // not owned
        const BtreeLogic<OnDiskFormat>* const _btree;
        const int _direction;

        DiskLoc _bucket;
        int _ofs;

        struct EndState {
            BSONObj key;
            bool inclusive;
            DiskLoc bucket;
            int ofs;
        };
        boost::optional<EndState> _endState;

        // Used by next to decide to return current position rather than moving. Should be reset
        // to false by any operation that moves the cursor, other than subsequent save/restore
        // pairs.
        bool _lastMoveWasRestore = false;

        // Only used by save/restore() if _bucket is non-Null.
        bool _savedEOF = false;
        SavedCursorRegistry::SavedCursor _saved;
    };

    virtual std::unique_ptr<SortedDataInterface::Cursor> newCursor(OperationContext* txn,
                                                                   bool isForward = true) const {
        return stdx::make_unique<Cursor>(txn, _btree.get(), isForward);
    }

    class RandomCursor final : public SortedDataInterface::Cursor {
    public:
        RandomCursor(OperationContext* txn, const BtreeLogic<OnDiskFormat>* btree)
            : _txn(txn), _btree(btree) {}

        boost::optional<IndexKeyEntry> next(RequestedInfo parts) override {
            if (_btree->isEmpty(_txn)) {
                return {};
            }
            return _btree->getRandomEntry(_txn);
        }

        void detachFromOperationContext() final {
            _txn = nullptr;
        }

        void reattachToOperationContext(OperationContext* txn) final {
            _txn = txn;
        }

        //
        // Should never be called.
        //
        void setEndPosition(const BSONObj& key, bool inclusive) override {
            MONGO_UNREACHABLE;
        }
        boost::optional<IndexKeyEntry> seek(const BSONObj& key,
                                            bool inclusive,
                                            RequestedInfo parts) override {
            MONGO_UNREACHABLE;
        }
        boost::optional<IndexKeyEntry> seek(const IndexSeekPoint& seekPoint,
                                            RequestedInfo parts) override {
            MONGO_UNREACHABLE;
        }

        //
        // May be called, but are no-ops.
        //
        void save() override {}
        void saveUnpositioned() override {}
        void restore() override {}

    private:
        OperationContext* _txn;
        const BtreeLogic<OnDiskFormat>* const _btree;
    };

    virtual std::unique_ptr<SortedDataInterface::Cursor> newRandomCursor(
        OperationContext* txn) const {
        return stdx::make_unique<RandomCursor>(txn, _btree.get());
    }

    virtual Status initAsEmpty(OperationContext* txn) {
        return _btree->initAsEmpty(txn);
    }

private:
    unique_ptr<BtreeLogic<OnDiskFormat>> _btree;
};
}  // namespace

SortedDataInterface* getMMAPV1Interface(HeadManager* headManager,
                                        RecordStore* recordStore,
                                        SavedCursorRegistry* cursorRegistry,
                                        const Ordering& ordering,
                                        const string& indexName,
                                        int version,
                                        bool isUnique) {
    if (0 == version) {
        return new BtreeInterfaceImpl<BtreeLayoutV0>(
            headManager, recordStore, cursorRegistry, ordering, indexName, isUnique);
    } else {
        invariant(1 == version);
        return new BtreeInterfaceImpl<BtreeLayoutV1>(
            headManager, recordStore, cursorRegistry, ordering, indexName, isUnique);
    }
}

}  // namespace mongo
