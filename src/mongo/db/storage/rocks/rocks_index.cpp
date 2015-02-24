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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kStorage

#include "mongo/platform/basic.h"

#include "mongo/db/storage/rocks/rocks_index.h"

#include <boost/scoped_ptr.hpp>
#include <boost/shared_ptr.hpp>
#include <cstdlib>
#include <sstream>
#include <string>
#include <vector>

#include <rocksdb/db.h>
#include <rocksdb/iterator.h>
#include <rocksdb/utilities/write_batch_with_index.h>

#include "mongo/base/checked_cast.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/concurrency/write_conflict_exception.h"
#include "mongo/db/storage/index_entry_comparison.h"
#include "mongo/db/storage/rocks/rocks_engine.h"
#include "mongo/db/storage/rocks/rocks_record_store.h"
#include "mongo/db/storage/rocks/rocks_recovery_unit.h"
#include "mongo/db/storage/rocks/rocks_util.h"
#include "mongo/util/log.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {

    using boost::scoped_ptr;
    using boost::shared_ptr;
    using std::string;
    using std::stringstream;
    using std::vector;

    namespace {

        /**
         * Strips the field names from a BSON object
         */
        BSONObj stripFieldNames( const BSONObj& obj ) {
            BSONObjBuilder b;
            BSONObjIterator i( obj );
            while ( i.more() ) {
                BSONElement e = i.next();
                b.appendAs( e, "" );
            }
            return b.obj();
        }

        string dupKeyError(const BSONObj& key) {
            stringstream ss;
            ss << "E11000 duplicate key error ";
            ss << "dup key: " << key.toString();
            return ss.str();
        }

        const int kTempKeyMaxSize = 1024;  // Do the same as the heap implementation

        Status checkKeySize(const BSONObj& key) {
            if (key.objsize() >= kTempKeyMaxSize) {
                string msg = mongoutils::str::stream()
                             << "RocksIndex::insert: key too large to index, failing " << ' '
                             << key.objsize() << ' ' << key;
                return Status(ErrorCodes::KeyTooLong, msg);
            }
            return Status::OK();
        }

        /**
         * Functionality shared by both unique and standard index
         */
        class RocksCursorBase : public SortedDataInterface::Cursor {
        public:
            RocksCursorBase(OperationContext* txn, rocksdb::DB* db, std::string prefix,
                            bool forward, Ordering order)
                : _db(db),
                  _prefix(prefix),
                  _forward(forward),
                  _order(order),
                  _locateCacheValid(false) {
                auto ru = RocksRecoveryUnit::getRocksRecoveryUnit(txn);
                _iterator.reset(ru->NewIterator(_prefix));
                _currentSequenceNumber = ru->snapshot()->GetSequenceNumber();
                checkStatus();
            }

            int getDirection() const { return _forward ? 1 : -1; }
            bool isEOF() const { return _iterator.get() == nullptr || !_iterator->Valid(); }

            /**
             * Will only be called with other from same index as this.
             * All EOF locs should be considered equal.
             */
            bool pointsToSamePlaceAs(const Cursor& genOther) const {
                const RocksCursorBase& other = checked_cast<const RocksCursorBase&>(genOther);

                if (isEOF() && other.isEOF()) {
                    return true;
                } else if (isEOF() || other.isEOF()) {
                    return false;
                }

                if (_iterator->key() != other._iterator->key()) {
                    return false;
                }

                // even if keys are equal, record IDs might be different (for unique indexes, since
                // in non-unique indexes RecordID is already encoded in the key)
                return _loc == other._loc;
            }

            virtual void advance() {
                // Advance on a cursor at the end is a no-op
                if (isEOF()) {
                    return;
                }
                advanceCursor();
                updatePosition();
            }

            bool locate(const BSONObj& key, const RecordId& loc) {
                const BSONObj finalKey = stripFieldNames(key);

                if (_locateCacheValid == true && finalKey == _locateCacheKey &&
                    loc == _locateCacheRecordId) {
                    // exact same call to locate()
                    return _locateCacheResult;
                }

                fillQuery(finalKey, loc, &_query);
                bool result = _locate(_query, loc);
                updatePosition();
                // An explicit search at the start of the range should always return false
                if (loc == RecordId::min() || loc == RecordId::max()) {
                    result = false;
                }

                {
                    // memoization
                    _locateCacheKey = finalKey.getOwned();
                    _locateCacheRecordId = loc;
                    _locateCacheResult = result;
                    _locateCacheValid = true;
                }
                return result;
            }

            // same first five args as IndexEntryComparison::makeQueryObject (which is commented).
            void advanceTo(const BSONObj &keyBegin,
                           int keyBeginLen,
                           bool afterKey,
                           const vector<const BSONElement*>& keyEnd,
                           const vector<bool>& keyEndInclusive) {
                // make a key representing the location to which we want to advance.
                BSONObj key = IndexEntryComparison::makeQueryObject(
                                         keyBegin,
                                         keyBeginLen,
                                         afterKey,
                                         keyEnd,
                                         keyEndInclusive,
                                         getDirection() );

                fillQuery(key, RecordId(), &_query);
                _locate(_query, RecordId());
                updatePosition();
            }

            /**
             * Locate a key with fields comprised of a combination of keyBegin fields and keyEnd
             * fields. Also same first five args as IndexEntryComparison::makeQueryObject (which is
             * commented).
             */
            void customLocate(const BSONObj& keyBegin,
                              int keyBeginLen,
                              bool afterVersion,
                              const vector<const BSONElement*>& keyEnd,
                              const vector<bool>& keyEndInclusive) {
                advanceTo( keyBegin, keyBeginLen, afterVersion, keyEnd, keyEndInclusive );
            }

            BSONObj getKey() const {
                if (isEOF()) {
                    return BSONObj();
                }

                if (!_keyBsonCache.isEmpty()) {
                    return _keyBsonCache;
                }

                _keyBsonCache =
                    KeyString::toBson(_key.getBuffer(), _key.getSize(), _order, _typeBits);

                return _keyBsonCache;
            }

            RecordId getRecordId() const { return _loc; }

            void savePosition() {
                _savedEOF = isEOF();
            }

            void restorePosition(OperationContext* txn) {
                auto ru = RocksRecoveryUnit::getRocksRecoveryUnit(txn);
                if (_currentSequenceNumber != ru->snapshot()->GetSequenceNumber()) {
                    _iterator.reset(ru->NewIterator(_prefix));
                    _currentSequenceNumber = ru->snapshot()->GetSequenceNumber();

                    if (!_savedEOF) {
                        _locate(_key, _loc);
                        updatePosition();
                    }
                }
            }

        protected:
            // Uses _key for the key. Implemented by unique and standard index
            virtual bool _locate(const KeyString& query, RecordId loc) = 0;

            virtual void fillQuery(const BSONObj& key, RecordId loc, KeyString* query) const = 0;

            // Called after _key has been filled in. Must not throw WriteConflictException.
            virtual void updateLocAndTypeBits() = 0;


            void advanceCursor() {
                if (_forward) {
                    _iterator->Next();
                } else {
                    _iterator->Prev();
                }
            }

            // Seeks to query. Returns true on exact match.
            bool seekCursor(const KeyString& query) {
                const rocksdb::Slice keySlice(query.getBuffer(), query.getSize());
                _iterator->Seek(keySlice);
                checkStatus();
                if (!_iterator->Valid()) {
                    if (!_forward) {
                        // this will give lower bound behavior for backwards
                        _iterator->SeekToLast();
                        checkStatus();
                    }
                    return false;
                }

                if (_iterator->key() == keySlice) {
                    return true;
                }

                if (!_forward) {
                    // if we can't find the exact result going backwards, we need to call Prev() so
                    // that we're at the first value less than (to the left of) what we were
                    // searching
                    // for, rather than the first value greater than (to the right of) the value we
                    // were
                    // searching for.
                    _iterator->Prev();
                }

                return false;
            }

            void updatePosition() {
                if (isEOF()) {
                    _loc = RecordId();
                    return;
                }

                checkStatus();

                auto key = _iterator->key();
                _key.resetFromBuffer(key.data(), key.size());
                _keyBsonCache = BSONObj();    // Invalidate cached BSONObj.

                _locateCacheValid = false;    // Invalidate locate cache
                _locateCacheKey = BSONObj();  // Invalidate locate cache

                updateLocAndTypeBits();
            }

            void checkStatus() {
                if ( !_iterator->status().ok() ) {
                    log() << _iterator->status().ToString();
                    // TODO: SERVER-16979 Correctly handle errors returned by RocksDB
                    invariant( false );
                }
            }

            rocksdb::DB* _db;                                       // not owned
            std::string _prefix;
            boost::scoped_ptr<rocksdb::Iterator> _iterator;
            const bool _forward;
            Ordering _order;

            // These are for storing savePosition/restorePosition state
            bool _savedEOF;
            RecordId _savedRecordId;
            rocksdb::SequenceNumber _currentSequenceNumber;

            KeyString _key;
            KeyString::TypeBits _typeBits;
            RecordId _loc;
            mutable BSONObj _keyBsonCache;  // if isEmpty, cache invalid and must be loaded from
                                            // _key.

            KeyString _query;

            // These are for caching repeated calls to locate()
            bool _locateCacheValid;
            BSONObj _locateCacheKey;
            RecordId _locateCacheRecordId;
            bool _locateCacheResult;
        };

        class RocksStandardCursor : public RocksCursorBase {
        public:
            RocksStandardCursor(OperationContext* txn, rocksdb::DB* db, std::string prefix,
                                bool forward, Ordering order)
                : RocksCursorBase(txn, db, prefix, forward, order) {}

            virtual void fillQuery(const BSONObj& key, RecordId loc, KeyString* query) const {
                // Null cursors should start at the zero key to maintain search ordering in the
                // collator.
                // Reverse cursors should start on the last matching key.
                if (loc.isNull()) {
                    loc = _forward ? RecordId::min() : RecordId::max();
                }

                query->resetToKey(key, _order, loc);
            }

            virtual bool _locate(const KeyString& query, RecordId loc) {
                // loc already encoded in _key
                return seekCursor(query);
            }

            virtual void updateLocAndTypeBits() {
                _loc = KeyString::decodeRecordIdAtEnd(_key.getBuffer(), _key.getSize());
                auto value = _iterator->value();
                BufReader br(value.data(), value.size());
                _typeBits.resetFromBuffer(&br);
            }
        };

        class RocksUniqueCursor : public RocksCursorBase {
        public:
            RocksUniqueCursor(OperationContext* txn, rocksdb::DB* db, std::string prefix,
                              bool forward, Ordering order)
                : RocksCursorBase(txn, db, prefix, forward, order) {}

            virtual void fillQuery(const BSONObj& key, RecordId loc, KeyString* query) const {
                query->resetToKey(key, _order);  // loc doesn't go in _query for unique indexes
            }

            virtual bool _locate(const KeyString& query, RecordId loc) {
                if (!seekCursor(query)) {
                    // If didn't seek to exact key, start at beginning of wherever we ended up.
                    return false;
                }
                dassert(!isEOF());

                // If we get here we need to look at the actual RecordId for this key and make sure
                // we are supposed to see it.

                auto value = _iterator->value();
                BufReader br(value.data(), value.size());
                RecordId locInIndex = KeyString::decodeRecordId(&br);

                if ((_forward && (locInIndex < loc)) || (!_forward && (locInIndex > loc))) {
                    advanceCursor();
                }

                return true;
            }

            void updateLocAndTypeBits() {
                // We assume that cursors can only ever see unique indexes in their "pristine"
                // state,
                // where no duplicates are possible. The cases where dups are allowed should hold
                // sufficient locks to ensure that no cursor ever sees them.

                auto value = _iterator->value();
                BufReader br(value.data(), value.size());
                _loc = KeyString::decodeRecordId(&br);
                _typeBits.resetFromBuffer(&br);

                if (!br.atEof()) {
                    severe() << "Unique index cursor seeing multiple records for key " << getKey();
                    fassertFailed(28609);
                }
            }
        };

        // TODO optimize and create two implementations -- one for unique and one for standard index
        class RocksIndexBulkBuilder : public SortedDataBuilderInterface {
        public:
            RocksIndexBulkBuilder(RocksIndexBase* index, OperationContext* txn, bool dupsAllowed)
                : _index(index), _txn(txn), _dupsAllowed(dupsAllowed) {
                invariant(index->isEmpty(txn));
            }

            Status addKey(const BSONObj& key, const RecordId& loc) {
                return _index->insert(_txn, key, loc, _dupsAllowed);
            }

            void commit(bool mayInterrupt) {
                WriteUnitOfWork uow(_txn);
                uow.commit();
            }

        private:
            RocksIndexBase* _index;
            OperationContext* _txn;
            bool _dupsAllowed;
        };

    } // namespace

    /// RocksIndexBase

    RocksIndexBase::RocksIndexBase(rocksdb::DB* db, std::string prefix, std::string ident,
                                   Ordering order)
        : _db(db), _prefix(prefix), _ident(std::move(ident)), _order(order) {}

    SortedDataBuilderInterface* RocksIndexBase::getBulkBuilder(OperationContext* txn,
                                                               bool dupsAllowed) {
        return new RocksIndexBulkBuilder(this, txn, dupsAllowed);
    }

    void RocksIndexBase::fullValidate(OperationContext* txn, bool full, long long* numKeysOut,
                                      BSONObjBuilder* output) const {
        if (numKeysOut) {
            boost::scoped_ptr<SortedDataInterface::Cursor> cursor(newCursor(txn, 1));
            cursor->locate(minKey, RecordId::min());
            *numKeysOut = 0;
            while (!cursor->isEOF()) {
                cursor->advance();
                (*numKeysOut)++;
            }
        }
    }

    bool RocksIndexBase::isEmpty(OperationContext* txn) {
        auto ru = RocksRecoveryUnit::getRocksRecoveryUnit(txn);
        boost::scoped_ptr<rocksdb::Iterator> it(ru->NewIterator(_prefix));

        it->SeekToFirst();
        return !it->Valid();
    }

    Status RocksIndexBase::initAsEmpty(OperationContext* txn) {
        // no-op
        return Status::OK();
    }

    long long RocksIndexBase::getSpaceUsedBytes(OperationContext* txn) const {
        uint64_t storageSize;
        std::string nextPrefix = std::move(rocksGetNextPrefix(_prefix));
        rocksdb::Range wholeRange(_prefix, nextPrefix);
        _db->GetApproximateSizes(&wholeRange, 1, &storageSize);
        // There might be some bytes in the WAL that we don't count here. Some
        // tests depend on the fact that non-empty indexes have non-zero sizes
        return static_cast<long long>(
            std::max(storageSize, static_cast<uint64_t>(1)));
    }

    std::string RocksIndexBase::_makePrefixedKey(const std::string& prefix,
                                                 const KeyString& encodedKey) {
        std::string key(prefix);
        key.append(encodedKey.getBuffer(), encodedKey.getSize());
        return key;
    }

    /// RocksUniqueIndex

    RocksUniqueIndex::RocksUniqueIndex(rocksdb::DB* db, std::string prefix, std::string ident,
                                       Ordering order)
        : RocksIndexBase(db, prefix, ident, order) {}

    Status RocksUniqueIndex::insert(OperationContext* txn, const BSONObj& key, const RecordId& loc,
                                    bool dupsAllowed) {
        Status s = checkKeySize(key);
        if (!s.isOK()) {
            return s;
        }

        KeyString encodedKey(key, _order);
        std::string prefixedKey(_makePrefixedKey(_prefix, encodedKey));

        auto ru = RocksRecoveryUnit::getRocksRecoveryUnit(txn);
        if (!ru->transaction()->registerWrite(prefixedKey)) {
            throw WriteConflictException();
        }

        std::string currentValue;
        auto getStatus = ru->Get(prefixedKey, &currentValue);
        if (!getStatus.ok() && !getStatus.IsNotFound()) {
            // This means that Get() returned an error
            // TODO: SERVER-16979 Correctly handle errors returned by RocksDB
            invariant(false);
        } else if (getStatus.IsNotFound()) {
            // nothing here. just insert the value
            KeyString value(loc);
            if (!encodedKey.getTypeBits().isAllZeros()) {
                value.appendTypeBits(encodedKey.getTypeBits());
            }
            rocksdb::Slice valueSlice(value.getBuffer(), value.getSize());
            ru->writeBatch()->Put(prefixedKey, valueSlice);
            return Status::OK();
        }

        // we are in a weird state where there might be multiple values for a key
        // we put them all in the "list"
        // Note that we can't omit AllZeros when there are multiple locs for a value. When we remove
        // down to a single value, it will be cleaned up.

        bool insertedLoc = false;
        KeyString valueVector;
        BufReader br(currentValue.data(), currentValue.size());
        while (br.remaining()) {
            RecordId locInIndex = KeyString::decodeRecordId(&br);
            if (loc == locInIndex) {
                return Status::OK();  // already in index
            }

            if (!insertedLoc && loc < locInIndex) {
                valueVector.appendRecordId(loc);
                valueVector.appendTypeBits(encodedKey.getTypeBits());
                insertedLoc = true;
            }

            // Copy from old to new value
            valueVector.appendRecordId(locInIndex);
            valueVector.appendTypeBits(KeyString::TypeBits::fromBuffer(&br));
        }

        if (!dupsAllowed) {
            return Status(ErrorCodes::DuplicateKey, dupKeyError(key));
        }

        if (!insertedLoc) {
            // This loc is higher than all currently in the index for this key
            valueVector.appendRecordId(loc);
            valueVector.appendTypeBits(encodedKey.getTypeBits());
        }

        rocksdb::Slice valueVectorSlice(valueVector.getBuffer(), valueVector.getSize());
        ru->writeBatch()->Put(prefixedKey, valueVectorSlice);
        return Status::OK();
    }

    void RocksUniqueIndex::unindex(OperationContext* txn, const BSONObj& key, const RecordId& loc,
                                   bool dupsAllowed) {
        KeyString encodedKey(key, _order);
        std::string prefixedKey(_makePrefixedKey(_prefix, encodedKey));

        auto ru = RocksRecoveryUnit::getRocksRecoveryUnit(txn);
        // We can't let two threads unindex the same key
        if (!ru->transaction()->registerWrite(prefixedKey)) {
            throw WriteConflictException();
        }

        if (!dupsAllowed) {
            ru->writeBatch()->Delete(prefixedKey);
            return;
        }

        // dups are allowed, so we have to deal with a vector of RecordIds.
        std::string currentValue;
        auto getStatus = ru->Get(prefixedKey, &currentValue);
        if (!getStatus.ok() && !getStatus.IsNotFound()) {
            // This means that Get() returned an error
            // TODO: SERVER-16979 Correctly handle errors returned by RocksDB
            invariant(false);
        } else if (getStatus.IsNotFound()) {
            // nothing here. just return
            return;
        }

        bool foundLoc = false;
        std::vector<std::pair<RecordId, KeyString::TypeBits>> records;

        BufReader br(currentValue.data(), currentValue.size());
        while (br.remaining()) {
            RecordId locInIndex = KeyString::decodeRecordId(&br);
            KeyString::TypeBits typeBits = KeyString::TypeBits::fromBuffer(&br);

            if (loc == locInIndex) {
                if (records.empty() && !br.remaining()) {
                    // This is the common case: we are removing the only loc for this key.
                    // Remove the whole entry.
                    ru->writeBatch()->Delete(prefixedKey);
                    return;
                }

                foundLoc = true;
                continue;
            }

            records.push_back(std::make_pair(locInIndex, typeBits));
        }

        if (!foundLoc) {
            warning().stream() << loc << " not found in the index for key " << key;
            return; // nothing to do
        }

        // Put other locs for this key back in the index.
        KeyString newValue;
        invariant(!records.empty());
        for (size_t i = 0; i < records.size(); i++) {
            newValue.appendRecordId(records[i].first);
            // When there is only one record, we can omit AllZeros TypeBits. Otherwise they need
            // to be included.
            if (!(records[i].second.isAllZeros() && records.size() == 1)) {
                newValue.appendTypeBits(records[i].second);
            }
        }

        rocksdb::Slice newValueSlice(newValue.getBuffer(), newValue.getSize());
        ru->writeBatch()->Put(prefixedKey, newValueSlice);
    }

    SortedDataInterface::Cursor* RocksUniqueIndex::newCursor(OperationContext* txn,
                                                             int direction) const {
        invariant( ( direction == 1 || direction == -1 ) && "invalid value for direction" );
        return new RocksUniqueCursor(txn, _db, _prefix, direction == 1, _order);
    }

    Status RocksUniqueIndex::dupKeyCheck(OperationContext* txn, const BSONObj& key,
                                         const RecordId& loc) {
        KeyString encodedKey(key, _order);
        std::string prefixedKey(_makePrefixedKey(_prefix, encodedKey));

        auto ru = RocksRecoveryUnit::getRocksRecoveryUnit(txn);
        std::string value;
        auto getStatus = ru->Get(prefixedKey, &value);
        if (!getStatus.ok() && !getStatus.IsNotFound()) {
            // This means that Get() returned an error
            // TODO: SERVER-16979 Correctly handle errors returned by RocksDB
            invariant(false);
        } else if (getStatus.IsNotFound()) {
            // not found, not duplicate key
            return Status::OK();
        }

        // If the key exists, check if we already have this loc at this key. If so, we don't
        // consider that to be a dup.
        BufReader br(value.data(), value.size());
        while (br.remaining()) {
            if (KeyString::decodeRecordId(&br) == loc) {
                return Status::OK();
            }

            KeyString::TypeBits::fromBuffer(&br);  // Just calling this to advance reader.
        }
        return Status(ErrorCodes::DuplicateKey, dupKeyError(key));
    }

    /// RocksStandardIndex
    RocksStandardIndex::RocksStandardIndex(rocksdb::DB* db, std::string prefix, std::string ident,
                                           Ordering order)
        : RocksIndexBase(db, prefix, ident, order) {}

    Status RocksStandardIndex::insert(OperationContext* txn, const BSONObj& key,
                                      const RecordId& loc, bool dupsAllowed) {
        invariant(dupsAllowed);
        Status s = checkKeySize(key);
        if (!s.isOK()) {
            return s;
        }

        // If we're inserting an index element, this means we already "locked" the RecordId of the
        // document. No need to register write here
        KeyString encodedKey(key, _order, loc);
        std::string prefixedKey(_makePrefixedKey(_prefix, encodedKey));

        rocksdb::Slice value;
        if (!encodedKey.getTypeBits().isAllZeros()) {
            value =
                rocksdb::Slice(reinterpret_cast<const char*>(encodedKey.getTypeBits().getBuffer()),
                               encodedKey.getTypeBits().getSize());
        }

        auto ru = RocksRecoveryUnit::getRocksRecoveryUnit(txn);
        ru->writeBatch()->Put(prefixedKey, value);

        return Status::OK();
    }

    void RocksStandardIndex::unindex(OperationContext* txn, const BSONObj& key, const RecordId& loc,
                                     bool dupsAllowed) {
        invariant(dupsAllowed);
        // If we're unindexing an index element, this means we already "locked" the RecordId of the
        // document. No need to register write here

        KeyString encodedKey(key, _order, loc);
        std::string prefixedKey(_makePrefixedKey(_prefix, encodedKey));
        auto ru = RocksRecoveryUnit::getRocksRecoveryUnit(txn);
        ru->writeBatch()->Delete(prefixedKey);
    }

    SortedDataInterface::Cursor* RocksStandardIndex::newCursor(OperationContext* txn,
                                                               int direction) const {
        invariant( ( direction == 1 || direction == -1 ) && "invalid value for direction" );
        return new RocksStandardCursor(txn, _db, _prefix, direction == 1, _order);
    }


}  // namespace mongo
