// in_memory_btree_impl.cpp

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

#include "mongo/platform/basic.h"

#include "mongo/db/storage/in_memory/in_memory_btree_impl.h"

#include <set>

#include "mongo/db/catalog/index_catalog_entry.h"
#include "mongo/db/storage/index_entry_comparison.h"
#include "mongo/db/storage/in_memory/in_memory_recovery_unit.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {
namespace {

    const int TempKeyMaxSize = 1024; // this goes away with SERVER-3372

    bool hasFieldNames(const BSONObj& obj) {
        BSONForEach(e, obj) {
            if (e.fieldName()[0])
                return true;
        }
        return false;
    }

    BSONObj stripFieldNames(const BSONObj& query) {
        if (!hasFieldNames(query))
            return query;

        BSONObjBuilder bb;
        BSONForEach(e, query) {
            bb.appendAs(e, StringData());
        }
        return bb.obj();
    }

    typedef std::set<IndexKeyEntry, IndexEntryComparison> IndexSet;

    // taken from btree_logic.cpp
    Status dupKeyError(const BSONObj& key) {
        StringBuilder sb;
        sb << "E11000 duplicate key error ";
        // sb << "index: " << _indexName << " "; // TODO
        sb << "dup key: " << key;
        return Status(ErrorCodes::DuplicateKey, sb.str());
    }

    bool isDup(const IndexSet& data, const BSONObj& key, RecordId loc) {
        const IndexSet::const_iterator it = data.find(IndexKeyEntry(key, RecordId()));
        if (it == data.end())
            return false;

        // Not a dup if the entry is for the same loc.
        return it->loc != loc;
    }

    class InMemoryBtreeBuilderImpl : public SortedDataBuilderInterface {
    public:
        InMemoryBtreeBuilderImpl(IndexSet* data, long long* currentKeySize, bool dupsAllowed)
                : _data(data),
                  _currentKeySize( currentKeySize ),
                  _dupsAllowed(dupsAllowed),
                  _comparator(_data->key_comp()) {
            invariant(_data->empty());
        }

        Status addKey(const BSONObj& key, const RecordId& loc) {
            // inserts should be in ascending (key, RecordId) order.

            if ( key.objsize() >= TempKeyMaxSize ) {
                return Status(ErrorCodes::KeyTooLong, "key too big");
            }

            invariant(loc.isNormal());
            invariant(!hasFieldNames(key));

            if (!_data->empty()) {
                // Compare specified key with last inserted key, ignoring its RecordId
                int cmp = _comparator.compare(IndexKeyEntry(key, RecordId()), *_last);
                if (cmp < 0 || (_dupsAllowed && cmp == 0 && loc < _last->loc)) {
                    return Status(ErrorCodes::InternalError,
                                  "expected ascending (key, RecordId) order in bulk builder");
                }
                else if (!_dupsAllowed && cmp == 0 && loc != _last->loc) {
                    return dupKeyError(key);
                }
            }

            BSONObj owned = key.getOwned();
            _last = _data->insert(_data->end(), IndexKeyEntry(owned, loc));
            *_currentKeySize += key.objsize();

            return Status::OK();
        }

    private:
        IndexSet* const _data;
        long long* _currentKeySize;
        const bool _dupsAllowed;

        IndexEntryComparison _comparator;  // used by the bulk builder to detect duplicate keys
        IndexSet::const_iterator _last;    // or (key, RecordId) ordering violations
    };

    class InMemoryBtreeImpl : public SortedDataInterface {
    public:
        InMemoryBtreeImpl(IndexSet* data)
            : _data(data) {
            _currentKeySize = 0;
        }

        virtual SortedDataBuilderInterface* getBulkBuilder(OperationContext* txn,
                                                           bool dupsAllowed) {
            return new InMemoryBtreeBuilderImpl(_data, &_currentKeySize, dupsAllowed);
        }

        virtual Status insert(OperationContext* txn,
                              const BSONObj& key,
                              const RecordId& loc,
                              bool dupsAllowed) {

            invariant(loc.isNormal());
            invariant(!hasFieldNames(key));

            if ( key.objsize() >= TempKeyMaxSize ) {
                string msg = mongoutils::str::stream()
                    << "InMemoryBtree::insert: key too large to index, failing "
                    << ' ' << key.objsize() << ' ' << key;
                return Status(ErrorCodes::KeyTooLong, msg);
            }

            // TODO optimization: save the iterator from the dup-check to speed up insert
            if (!dupsAllowed && isDup(*_data, key, loc))
                return dupKeyError(key);

            IndexKeyEntry entry(key.getOwned(), loc);
            if ( _data->insert(entry).second ) {
                _currentKeySize += key.objsize();
                txn->recoveryUnit()->registerChange(new IndexChange(_data, entry, true));
            }
            return Status::OK();
        }

        virtual void unindex(OperationContext* txn,
                             const BSONObj& key,
                             const RecordId& loc,
                             bool dupsAllowed) {
            invariant(loc.isNormal());
            invariant(!hasFieldNames(key));

            IndexKeyEntry entry(key.getOwned(), loc);
            const size_t numDeleted = _data->erase(entry);
            invariant(numDeleted <= 1);
            if ( numDeleted == 1 ) {
                _currentKeySize -= key.objsize();
                txn->recoveryUnit()->registerChange(new IndexChange(_data, entry, false));
            }
        }

        virtual void fullValidate(OperationContext* txn, bool full, long long *numKeysOut,
                                  BSONObjBuilder* output) const {
            // TODO check invariants?
            *numKeysOut = _data->size();
        }

        virtual void appendCustomStats(OperationContext* txn, BSONObjBuilder* output, double scale)
            const { }

        virtual long long getSpaceUsedBytes( OperationContext* txn ) const {
            return _currentKeySize + ( sizeof(IndexKeyEntry) * _data->size() );
        }

        virtual Status dupKeyCheck(OperationContext* txn, const BSONObj& key, const RecordId& loc) {
            invariant(!hasFieldNames(key));
            if (isDup(*_data, key, loc))
                return dupKeyError(key);
            return Status::OK();
        }

        virtual bool isEmpty(OperationContext* txn) {
            return _data->empty();
        }

        virtual Status touch(OperationContext* txn) const{
            // already in memory...
            return Status::OK();
        }

        class ForwardCursor : public SortedDataInterface::Cursor {
        public:
            ForwardCursor(const IndexSet& data, OperationContext* txn)
                : _txn(txn),
                  _data(data),
                  _it(data.end())
            {}

            virtual int getDirection() const { return 1; }

            virtual bool isEOF() const {
                return _it == _data.end();
            }

            virtual bool pointsToSamePlaceAs(const SortedDataInterface::Cursor& otherBase) const {
                const ForwardCursor& other = static_cast<const ForwardCursor&>(otherBase);
                invariant(&_data == &other._data); // iterators over same index
                return _it == other._it;
            }

            virtual void aboutToDeleteBucket(const RecordId& bucket) {
                invariant(!"aboutToDeleteBucket should not be called");
            }

            virtual bool locate(const BSONObj& keyRaw, const RecordId& loc) {
                const BSONObj key = stripFieldNames(keyRaw);
                _it = _data.lower_bound(IndexKeyEntry(key, loc)); // lower_bound is >= key
                if ( _it == _data.end() ) {
                    return false;
                }

                if ( _it->key != key ) {
                    return false;
                }

                return _it->loc == loc;
            }

            virtual void customLocate(const BSONObj& keyBegin,
                                      int keyBeginLen,
                                      bool afterKey,
                                      const vector<const BSONElement*>& keyEnd,
                                      const vector<bool>& keyEndInclusive) {
                // makeQueryObject handles stripping of fieldnames for us.
                _it = _data.lower_bound(IndexKeyEntry(IndexEntryComparison::makeQueryObject(
                                                        keyBegin,
                                                        keyBeginLen,
                                                        afterKey,
                                                        keyEnd,
                                                        keyEndInclusive,
                                                        1), // forward
                                                   RecordId()));
            }

            void advanceTo(const BSONObj &keyBegin,
                           int keyBeginLen,
                           bool afterKey,
                           const vector<const BSONElement*>& keyEnd,
                           const vector<bool>& keyEndInclusive) {
                // XXX I think these do the same thing????
                customLocate(keyBegin, keyBeginLen, afterKey, keyEnd, keyEndInclusive);
            }

            virtual BSONObj getKey() const {
                return _it->key;
            }

            virtual RecordId getRecordId() const {
                return _it->loc;
            }

            virtual void advance() {
                if (_it != _data.end())
                    ++_it;
            }

            virtual void savePosition() {
                if (_it == _data.end()) {
                    _savedAtEnd = true;
                    return;
                }

                _savedAtEnd = false;
                _savedKey = _it->key.getOwned();
                _savedLoc = _it->loc;
            }

            virtual void restorePosition(OperationContext* txn) {
                if (_savedAtEnd) {
                    _it = _data.end();
                }
                else {
                    locate(_savedKey, _savedLoc);
                }
            }

        private:

            OperationContext* _txn; // not owned
            const IndexSet& _data;
            IndexSet::const_iterator _it;

            // For save/restorePosition since _it may be invalidated durring a yield.
            bool _savedAtEnd;
            BSONObj _savedKey;
            RecordId _savedLoc;

        };

        // TODO see if this can share any code with ForwardIterator
        class ReverseCursor : public SortedDataInterface::Cursor {
        public:
            ReverseCursor(const IndexSet& data, OperationContext* txn)
                : _txn(txn),
                  _data(data),
                  _it(data.rend())
            {}

            virtual int getDirection() const { return -1; }

            virtual bool isEOF() const {
                return _it == _data.rend();
            }

            virtual bool pointsToSamePlaceAs(const SortedDataInterface::Cursor& otherBase) const {
                const ReverseCursor& other = static_cast<const ReverseCursor&>(otherBase);
                invariant(&_data == &other._data); // iterators over same index
                return _it == other._it;
            }

            virtual void aboutToDeleteBucket(const RecordId& bucket) {
                invariant(!"aboutToDeleteBucket should not be called");
            }

            virtual bool locate(const BSONObj& keyRaw, const RecordId& loc) {
                const BSONObj key = stripFieldNames(keyRaw);
                _it = lower_bound(IndexKeyEntry(key, loc)); // lower_bound is <= query

                if ( _it == _data.rend() ) {
                    return false;
                }


                if ( _it->key != key ) {
                    return false;
                }

                return _it->loc == loc;
            }

            virtual void customLocate(const BSONObj& keyBegin,
                                      int keyBeginLen,
                                      bool afterKey,
                                      const vector<const BSONElement*>& keyEnd,
                                      const vector<bool>& keyEndInclusive) {
                // makeQueryObject handles stripping of fieldnames for us.
                _it = lower_bound(IndexKeyEntry(IndexEntryComparison::makeQueryObject(
                                                  keyBegin,
                                                  keyBeginLen,
                                                  afterKey,
                                                  keyEnd,
                                                  keyEndInclusive,
                                                  -1), // reverse
                                             RecordId()));
            }

            void advanceTo(const BSONObj &keyBegin,
                           int keyBeginLen,
                           bool afterKey,
                           const vector<const BSONElement*>& keyEnd,
                           const vector<bool>& keyEndInclusive) {
                // XXX I think these do the same thing????
                customLocate(keyBegin, keyBeginLen, afterKey, keyEnd, keyEndInclusive);
            }

            virtual BSONObj getKey() const {
                return _it->key;
            }

            virtual RecordId getRecordId() const {
                return _it->loc;
            }

            virtual void advance() {
                if (_it != _data.rend())
                    ++_it;
            }

            virtual void savePosition() {
                if (_it == _data.rend()) {
                    _savedAtEnd = true;
                    return;
                }

                _savedAtEnd = false;
                _savedKey = _it->key.getOwned();
                _savedLoc = _it->loc;
            }

            virtual void restorePosition(OperationContext* txn) {
                if (_savedAtEnd) {
                    _it = _data.rend();
                }
                else {
                    locate(_savedKey, _savedLoc);
                }
            }

        private:
            /**
             * Returns the first entry <= query. This is equivalent to ForwardCursors use of
             * _data.lower_bound which returns the first entry >= query.
             */
            IndexSet::const_reverse_iterator lower_bound(const IndexKeyEntry& query) const {
                // using upper_bound since we want to the right-most entry matching the query.
                IndexSet::const_iterator it = _data.upper_bound(query);

                // upper_bound returns the entry to the right of the one we want. Helpfully,
                // converting to a reverse_iterator moves one to the left. This also correctly
                // handles the case where upper_bound returns end() by converting to rbegin(),
                // meaning that all data is to the right of the query.
                return IndexSet::const_reverse_iterator(it);
            }

            OperationContext* _txn; // not owned
            const IndexSet& _data;
            IndexSet::const_reverse_iterator _it;

            // For save/restorePosition since _it may be invalidated durring a yield.
            bool _savedAtEnd;
            BSONObj _savedKey;
            RecordId _savedLoc;
        };

        virtual SortedDataInterface::Cursor* newCursor(OperationContext* txn, int direction) const {
            if (direction == 1)
                return new ForwardCursor(*_data, txn);

            invariant(direction == -1);
            return new ReverseCursor(*_data, txn);
        }

        virtual Status initAsEmpty(OperationContext* txn) {
            // No-op
            return Status::OK();
        }

    private:
        class IndexChange : public RecoveryUnit::Change {
        public:
            IndexChange(IndexSet* data, const IndexKeyEntry& entry, bool insert)
                : _data(data), _entry(entry), _insert(insert)
            {}

            virtual void commit() {}
            virtual void rollback() {
                if (_insert)
                    _data->erase(_entry);
                else
                    _data->insert(_entry);
            }

        private:
            IndexSet* _data;
            const IndexKeyEntry _entry;
            const bool _insert;
        };

        IndexSet* _data;
        long long _currentKeySize;
    };
} // namespace

    // IndexCatalogEntry argument taken by non-const pointer for consistency with other Btree
    // factories. We don't actually modify it.
    SortedDataInterface* getInMemoryBtreeImpl(const Ordering& ordering,
                                              boost::shared_ptr<void>* dataInOut) {
        invariant(dataInOut);
        if (!*dataInOut) {
            *dataInOut = boost::make_shared<IndexSet>(IndexEntryComparison(ordering));
        }
        return new InMemoryBtreeImpl(static_cast<IndexSet*>(dataInOut->get()));
    }

}  // namespace mongo
