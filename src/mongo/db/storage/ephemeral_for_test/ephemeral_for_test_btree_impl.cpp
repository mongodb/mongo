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

#include "mongo/db/storage/ephemeral_for_test/ephemeral_for_test_btree_impl.h"

#include <memory>
#include <set>

#include "mongo/db/catalog/index_catalog_entry.h"
#include "mongo/db/storage/ephemeral_for_test/ephemeral_for_test_recovery_unit.h"
#include "mongo/db/storage/index_entry_comparison.h"
#include "mongo/db/storage/key_string.h"
#include "mongo/util/str.h"

namespace mongo {

using std::shared_ptr;
using std::string;
using std::vector;

namespace {

typedef std::set<IndexKeyEntry, IndexEntryComparison> IndexSet;

bool keyExists(const IndexSet& data, const BSONObj& key) {
    IndexSet::const_iterator it = data.find(IndexKeyEntry(key, RecordId()));
    return it != data.end();
}

bool isDup(const IndexSet& data, const BSONObj& key) {
    IndexSet::const_iterator it = data.find(IndexKeyEntry(key, RecordId()));
    if (it == data.end())
        return false;

    ++it;
    if (it == data.end())
        return false;

    return it->key.woCompare(key, BSONObj(), false) == 0;
}

class EphemeralForTestBtreeBuilderImpl : public SortedDataBuilderInterface {
public:
    EphemeralForTestBtreeBuilderImpl(IndexSet* data,
                                     long long* currentKeySize,
                                     const Ordering& ordering,
                                     bool dupsAllowed,
                                     const NamespaceString& collectionNamespace,
                                     const std::string& indexName,
                                     const BSONObj& keyPattern)
        : _data(data),
          _currentKeySize(currentKeySize),
          _ordering(ordering),
          _dupsAllowed(dupsAllowed),
          _comparator(_data->key_comp()),
          _collectionNamespace(collectionNamespace),
          _indexName(indexName),
          _keyPattern(keyPattern) {
        invariant(_data->empty());
    }

    Status addKey(const BSONObj& key, const RecordId& loc) {
        // inserts should be in ascending (key, RecordId) order.

        invariant(loc.isValid());
        invariant(!key.hasFieldNames());

        if (!_data->empty()) {
            // Compare specified key with last inserted key, ignoring its RecordId
            int cmp = _comparator.compare(IndexKeyEntry(key, RecordId()), *_last);
            if (cmp < 0 || (_dupsAllowed && cmp == 0 && loc < _last->loc)) {
                return Status(ErrorCodes::InternalError,
                              "expected ascending (key, RecordId) order in bulk builder");
            } else if (!_dupsAllowed && cmp == 0 && loc != _last->loc) {
                return buildDupKeyErrorStatus(key, _collectionNamespace, _indexName, _keyPattern);
            }
        }

        BSONObj owned = key.getOwned();
        _last = _data->insert(_data->end(), IndexKeyEntry(owned, loc));
        *_currentKeySize += key.objsize();

        return Status::OK();
    }

    Status addKey(const KeyString::Value& keyString, const RecordId& loc) {
        dassert(loc == KeyString::decodeRecordIdAtEnd(keyString.getBuffer(), keyString.getSize()));

        auto key = KeyString::toBson(keyString, _ordering);

        return addKey(key, loc);
    }

private:
    IndexSet* const _data;
    long long* _currentKeySize;
    const Ordering& _ordering;
    const bool _dupsAllowed;

    IndexEntryComparison _comparator;  // used by the bulk builder to detect duplicate keys
    IndexSet::const_iterator _last;    // or (key, RecordId) ordering violations

    const NamespaceString _collectionNamespace;
    const std::string _indexName;
    const BSONObj _keyPattern;
};

class EphemeralForTestBtreeImpl : public SortedDataInterface {
public:
    EphemeralForTestBtreeImpl(IndexSet* data,
                              const Ordering& ordering,
                              bool isUnique,
                              const NamespaceString& collectionNamespace,
                              const std::string& indexName,
                              const BSONObj& keyPattern)
        : SortedDataInterface(KeyString::Version::kLatestVersion, ordering),
          _data(data),
          _isUnique(isUnique),
          _collectionNamespace(collectionNamespace),
          _indexName(indexName),
          _keyPattern(keyPattern) {
        _currentKeySize = 0;
    }

    virtual SortedDataBuilderInterface* getBulkBuilder(OperationContext* opCtx, bool dupsAllowed) {
        return new EphemeralForTestBtreeBuilderImpl(_data,
                                                    &_currentKeySize,
                                                    _ordering,
                                                    dupsAllowed,
                                                    _collectionNamespace,
                                                    _indexName,
                                                    _keyPattern);
    }

    virtual Status insert(OperationContext* opCtx,
                          const BSONObj& key,
                          const RecordId& loc,
                          bool dupsAllowed) {
        invariant(loc.isValid());
        invariant(!key.hasFieldNames());


        // TODO optimization: save the iterator from the dup-check to speed up insert
        if (!dupsAllowed && keyExists(*_data, key))
            return buildDupKeyErrorStatus(key, _collectionNamespace, _indexName, _keyPattern);

        IndexKeyEntry entry(key.getOwned(), loc);
        if (_data->insert(entry).second) {
            _currentKeySize += key.objsize();
            opCtx->recoveryUnit()->registerChange(new IndexChange(_data, entry, true));
        }
        return Status::OK();
    }

    virtual Status insert(OperationContext* opCtx,
                          const KeyString::Value& keyString,
                          const RecordId& loc,
                          bool dupsAllowed) {
        dassert(loc == KeyString::decodeRecordIdAtEnd(keyString.getBuffer(), keyString.getSize()));

        auto key = KeyString::toBson(keyString, _ordering);

        return insert(opCtx, key, loc, dupsAllowed);
    }

    virtual void unindex(OperationContext* opCtx,
                         const BSONObj& key,
                         const RecordId& loc,
                         bool dupsAllowed) {
        invariant(loc.isValid());
        invariant(!key.hasFieldNames());

        IndexKeyEntry entry(key.getOwned(), loc);
        const size_t numDeleted = _data->erase(entry);
        invariant(numDeleted <= 1);
        if (numDeleted == 1) {
            _currentKeySize -= key.objsize();
            opCtx->recoveryUnit()->registerChange(new IndexChange(_data, entry, false));
        }
    }

    virtual void unindex(OperationContext* opCtx,
                         const KeyString::Value& keyString,
                         const RecordId& loc,
                         bool dupsAllowed) {
        dassert(loc == KeyString::decodeRecordIdAtEnd(keyString.getBuffer(), keyString.getSize()));

        auto key = KeyString::toBson(keyString, _ordering);

        return unindex(opCtx, key, loc, dupsAllowed);
    }

    virtual void fullValidate(OperationContext* opCtx,
                              long long* numKeysOut,
                              ValidateResults* fullResults) const {
        // TODO check invariants?
        *numKeysOut = _data->size();
    }

    virtual bool appendCustomStats(OperationContext* opCtx,
                                   BSONObjBuilder* output,
                                   double scale) const {
        return false;
    }

    virtual long long getSpaceUsedBytes(OperationContext* opCtx) const {
        return _currentKeySize + (sizeof(IndexKeyEntry) * _data->size());
    }

    virtual Status dupKeyCheck(OperationContext* opCtx, const BSONObj& key) {
        invariant(!key.hasFieldNames());
        if (isDup(*_data, key))
            return buildDupKeyErrorStatus(key, _collectionNamespace, _indexName, _keyPattern);
        return Status::OK();
    }

    virtual bool isEmpty(OperationContext* opCtx) {
        return _data->empty();
    }

    virtual Status touch(OperationContext* opCtx) const {
        // already in memory...
        return Status::OK();
    }

    class Cursor final : public SortedDataInterface::Cursor {
    public:
        Cursor(OperationContext* opCtx, const IndexSet& data, bool isForward, bool isUnique)
            : _opCtx(opCtx),
              _data(data),
              _forward(isForward),
              _isUnique(isUnique),
              _it(data.end()) {}

        boost::optional<IndexKeyEntry> next(RequestedInfo parts) override {
            if (_lastMoveWasRestore) {
                // Return current position rather than advancing.
                _lastMoveWasRestore = false;
            } else {
                advance();
                if (atEndPoint())
                    _isEOF = true;
            }

            if (_isEOF)
                return {};
            return *_it;
        }

        void setEndPosition(const BSONObj& key, bool inclusive) override {
            if (key.isEmpty()) {
                // This means scan to end of index.
                _endState = boost::none;
                return;
            }

            // NOTE: this uses the opposite min/max rules as a normal seek because a forward
            // scan should land after the key if inclusive and before if exclusive.
            _endState = EndState(BSONObj::stripFieldNames(key),
                                 _forward == inclusive ? RecordId::max() : RecordId::min());
            seekEndCursor();
        }

        boost::optional<IndexKeyEntry> seek(const BSONObj& key,
                                            bool inclusive,
                                            RequestedInfo parts) override {
            if (key.isEmpty()) {
                _it = inclusive ? _data.begin() : _data.end();
                _isEOF = (_it == _data.end());
                if (_isEOF) {
                    return {};
                }
            } else {
                const BSONObj query = BSONObj::stripFieldNames(key);
                locate(query, _forward == inclusive ? RecordId::min() : RecordId::max());
                _lastMoveWasRestore = false;
                if (_isEOF)
                    return {};
                dassert(inclusive ? compareKeys(_it->key, query) >= 0
                                  : compareKeys(_it->key, query) > 0);
            }

            return *_it;
        }

        boost::optional<IndexKeyEntry> seek(const IndexSeekPoint& seekPoint,
                                            RequestedInfo parts) override {
            // Query encodes exclusive case so it can be treated as an inclusive query.
            const BSONObj query = IndexEntryComparison::makeQueryObject(seekPoint, _forward);
            locate(query, _forward ? RecordId::min() : RecordId::max());
            _lastMoveWasRestore = false;
            if (_isEOF)
                return {};
            dassert(compareKeys(_it->key, query) >= 0);
            return *_it;
        }

        void save() override {
            // Keep original position if we haven't moved since the last restore.
            _opCtx = nullptr;
            if (_lastMoveWasRestore)
                return;

            if (_isEOF) {
                saveUnpositioned();
                return;
            }

            _savedAtEnd = false;
            _savedKey = _it->key.getOwned();
            _savedLoc = _it->loc;
            // Doing nothing with end cursor since it will do full reseek on restore.
        }

        void saveUnpositioned() override {
            _savedAtEnd = true;
            // Doing nothing with end cursor since it will do full reseek on restore.
        }

        void restore() override {
            // Always do a full seek on restore. We cannot use our last position since index
            // entries may have been inserted closer to our endpoint and we would need to move
            // over them.
            seekEndCursor();

            if (_savedAtEnd) {
                _isEOF = true;
                return;
            }

            // Need to find our position from the root.
            locate(_savedKey, _savedLoc);

            _lastMoveWasRestore = _isEOF;  // We weren't EOF but now are.
            if (!_lastMoveWasRestore) {
                // For standard (non-unique) indices, restoring to either a new key or a new record
                // id means that the next key should be the one we just restored to.
                //
                // Cursors for unique indices should never return the same key twice, so we don't
                // consider the restore as having moved the cursor position if the record id
                // changes. In this case we use a null record id so that only the keys are compared.
                auto savedLocToUse = _isUnique ? RecordId() : _savedLoc;
                _lastMoveWasRestore =
                    (_data.value_comp().compare(*_it, {_savedKey, savedLocToUse}) != 0);
            }
        }

        void detachFromOperationContext() final {
            _opCtx = nullptr;
        }

        void reattachToOperationContext(OperationContext* opCtx) final {
            _opCtx = opCtx;
        }

    private:
        bool atEndPoint() const {
            return _endState && _it == _endState->it;
        }

        // Advances once in the direction of the scan, updating _isEOF as needed.
        // Does nothing if already _isEOF.
        void advance() {
            if (_isEOF)
                return;
            if (_forward) {
                if (_it != _data.end())
                    ++_it;
                if (_it == _data.end() || atEndPoint())
                    _isEOF = true;
            } else {
                if (_it == _data.begin() || _data.empty()) {
                    _isEOF = true;
                } else {
                    --_it;
                }
                if (atEndPoint())
                    _isEOF = true;
            }
        }

        bool atOrPastEndPointAfterSeeking() const {
            if (_isEOF)
                return true;
            if (!_endState)
                return false;

            const int cmp = _data.value_comp().compare(*_it, _endState->query);

            // We set up _endState->query to be in between the last in-range value and the first
            // out-of-range value. In particular, it is constructed to never equal any legal
            // index key.
            dassert(cmp != 0);

            if (_forward) {
                // We may have landed after the end point.
                return cmp > 0;
            } else {
                // We may have landed before the end point.
                return cmp < 0;
            }
        }

        void locate(const BSONObj& key, const RecordId& loc) {
            _isEOF = false;
            const auto query = IndexKeyEntry(key, loc);
            _it = _data.lower_bound(query);
            if (_forward) {
                if (_it == _data.end())
                    _isEOF = true;
            } else {
                // lower_bound lands us on or after query. Reverse cursors must be on or before.
                if (_it == _data.end() || _data.value_comp().compare(*_it, query) > 0)
                    advance();  // sets _isEOF if there is nothing more to return.
            }

            if (atOrPastEndPointAfterSeeking())
                _isEOF = true;
        }

        // Returns comparison relative to direction of scan. If rhs would be seen later, returns
        // a positive value.
        int compareKeys(const BSONObj& lhs, const BSONObj& rhs) const {
            int cmp = _data.value_comp().compare({lhs, RecordId()}, {rhs, RecordId()});
            return _forward ? cmp : -cmp;
        }

        void seekEndCursor() {
            if (!_endState || _data.empty())
                return;

            auto it = _data.lower_bound(_endState->query);
            if (!_forward) {
                // lower_bound lands us on or after query. Reverse cursors must be on or before.
                if (it == _data.end() || _data.value_comp().compare(*it, _endState->query) > 0) {
                    if (it == _data.begin()) {
                        it = _data.end();  // all existing data in range.
                    } else {
                        --it;
                    }
                }
            }

            if (it != _data.end())
                dassert(compareKeys(it->key, _endState->query.key) >= 0);
            _endState->it = it;
        }

        OperationContext* _opCtx;  // not owned
        const IndexSet& _data;
        const bool _forward;
        const bool _isUnique;
        bool _isEOF = true;
        IndexSet::const_iterator _it;

        struct EndState {
            EndState(BSONObj key, RecordId loc) : query(std::move(key), loc) {}

            IndexKeyEntry query;
            IndexSet::const_iterator it;
        };
        boost::optional<EndState> _endState;

        // Used by next to decide to return current position rather than moving. Should be reset
        // to false by any operation that moves the cursor, other than subsequent save/restore
        // pairs.
        bool _lastMoveWasRestore = false;

        // For save/restore since _it may be invalidated during a yield.
        bool _savedAtEnd = false;
        BSONObj _savedKey;
        RecordId _savedLoc;
    };

    virtual std::unique_ptr<SortedDataInterface::Cursor> newCursor(OperationContext* opCtx,
                                                                   bool isForward) const {
        return std::make_unique<Cursor>(opCtx, *_data, isForward, _isUnique);
    }

    virtual Status initAsEmpty(OperationContext* opCtx) {
        // No-op
        return Status::OK();
    }

private:
    class IndexChange : public RecoveryUnit::Change {
    public:
        IndexChange(IndexSet* data, const IndexKeyEntry& entry, bool insert)
            : _data(data), _entry(entry), _insert(insert) {}

        virtual void commit(boost::optional<Timestamp>) {}
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
    const bool _isUnique;

    const NamespaceString _collectionNamespace;
    const std::string _indexName;
    const BSONObj _keyPattern;
};
}  // namespace

// IndexCatalogEntry argument taken by non-const pointer for consistency with other Btree
// factories. We don't actually modify it.
std::unique_ptr<SortedDataInterface> getEphemeralForTestBtreeImpl(
    const Ordering& ordering,
    bool isUnique,
    const NamespaceString& collectionNamespace,
    const std::string& indexName,
    const BSONObj& keyPattern,
    std::shared_ptr<void>* dataInOut) {
    invariant(dataInOut);
    if (!*dataInOut) {
        *dataInOut = std::make_shared<IndexSet>(IndexEntryComparison(ordering));
    }
    return std::make_unique<EphemeralForTestBtreeImpl>(static_cast<IndexSet*>(dataInOut->get()),
                                                       ordering,
                                                       isUnique,
                                                       collectionNamespace,
                                                       indexName,
                                                       keyPattern);
}

}  // namespace mongo
