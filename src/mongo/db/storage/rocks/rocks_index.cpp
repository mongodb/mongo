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
            RocksCursorBase(OperationContext* txn, rocksdb::DB* db,
                            boost::shared_ptr<rocksdb::ColumnFamilyHandle> columnFamily,
                            bool forward, Ordering order)
                : _db(db),
                  _columnFamily(columnFamily),
                  _forward(forward),
                  _order(order),
                  _isKeyCurrent(false) {
                auto ru = RocksRecoveryUnit::getRocksRecoveryUnit(txn);
                _iterator.reset(ru->NewIterator(_columnFamily.get()));
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
                return getRecordId() == other.getRecordId();
            }

            bool locate(const BSONObj& key, const RecordId& loc) {
                const BSONObj finalKey = stripFieldNames(key);
                fillKey(finalKey, loc);
                bool result = _locate(loc);
                // An explicit search at the start of the range should always return false
                if (loc == RecordId::min() || loc == RecordId::max()) {
                    return false;
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

                fillKey(key, RecordId());
                _locate(RecordId());
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
                if (_isKeyCurrent && !_keyBson.isEmpty()) {
                    return _keyBson;
                }
                loadKeyIfNeeded();
                _keyBson =
                    KeyString::toBson(_key.getBuffer(), _key.getSize(), _order, getTypeBits());
                return _keyBson;
            }

            void savePosition() {
                _savedEOF = isEOF();

                if (!_savedEOF) {
                    loadKeyIfNeeded();
                    _savedRecordId = getRecordId();
                    _iterator.reset();
                }
            }

            void restorePosition(OperationContext* txn) {
                auto ru = RocksRecoveryUnit::getRocksRecoveryUnit(txn);
                _iterator.reset(ru->NewIterator(_columnFamily.get()));

                if (!_savedEOF) {
                    _locate(_savedRecordId);
                }
            }

        protected:
            // Uses _key for the key. Implemented by unique and standard index
            virtual bool _locate(RecordId loc) = 0;

            // Must invalidateCache()
            virtual void fillKey(const BSONObj& key, RecordId loc) = 0;

            virtual const KeyString::TypeBits& getTypeBits() const = 0;

            void advanceCursor() {
                invalidateCache();
                if (_forward) {
                    _iterator->Next();
                } else {
                    _iterator->Prev();
                }
                checkStatus();
            }

            // Seeks to _key. Returns true on exact match.
            bool seekCursor() {
                invalidateCache();
                const rocksdb::Slice keySlice(_key.getBuffer(), _key.getSize());
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

            void loadKeyIfNeeded() const {
                if (_isKeyCurrent) {
                    return;
                }

                auto key = _iterator->key();
                _key.resetFromBuffer(key.data(), key.size());
                _isKeyCurrent = true;
            }

            virtual void invalidateCache() {
                _isKeyCurrent = false;
                _keyBson = BSONObj();
            }

            void checkStatus() {
                if ( !_iterator->status().ok() ) {
                    log() << _iterator->status().ToString();
                    // TODO: SERVER-16979 Correctly handle errors returned by RocksDB
                    invariant( false );
                }
            }

            rocksdb::DB* _db;                                       // not owned
            boost::shared_ptr<rocksdb::ColumnFamilyHandle> _columnFamily;
            boost::scoped_ptr<rocksdb::Iterator> _iterator;
            const bool _forward;
            Ordering _order;

            // These are for storing savePosition/restorePosition state
            bool _savedEOF;
            RecordId _savedRecordId;

            // These are all lazily loaded caches.
            mutable BSONObj _keyBson;    // if isEmpty, it is invalid and must be loaded from _key.
            mutable bool _isKeyCurrent;  // true if _key matches where the cursor is pointing
            mutable KeyString _key;
        };

        class RocksStandardCursor : public RocksCursorBase {
        public:
            RocksStandardCursor(OperationContext* txn, rocksdb::DB* db,
                                boost::shared_ptr<rocksdb::ColumnFamilyHandle> columnFamily,
                                bool forward, Ordering order)
                : RocksCursorBase(txn, db, columnFamily, forward, order), _isTypeBitsValid(false) {}

            virtual void invalidateCache() {
                RocksCursorBase::invalidateCache();
                _loc = RecordId();
                _isTypeBitsValid = false;
            }

            virtual void fillKey(const BSONObj& key, RecordId loc) {
                // Null cursors should start at the zero key to maintain search ordering in the
                // collator.
                // Reverse cursors should start on the last matching key.
                if (loc.isNull()) {
                    loc = _forward ? RecordId::min() : RecordId::max();
                }

                _key.resetToKey(key, _order, loc);
                invalidateCache();
            }
            virtual bool _locate(RecordId loc) {
                // loc already encoded in _key
                return seekCursor();
            }

            virtual RecordId getRecordId() const {
                if (isEOF()) {
                    return RecordId();
                }

                if (_loc.isNull()) {
                    loadKeyIfNeeded();
                    _loc = KeyString::decodeRecordIdAtEnd(_key.getBuffer(), _key.getSize());
                }

                dassert(!_loc.isNull());
                return _loc;
            }

            virtual void advance() {
                // Advance on a cursor at the end is a no-op
                if (isEOF()) {
                    return;
                }
                advanceCursor();
            }

            virtual const KeyString::TypeBits& getTypeBits() const {
                if (!_isTypeBitsValid) {
                    auto value = _iterator->value();
                    BufReader br(value.data(), value.size());
                    _typeBits.resetFromBuffer(&br);
                    _isTypeBitsValid = true;
                }

                return _typeBits;
            }

        private:
            mutable RecordId _loc;

            mutable bool _isTypeBitsValid;
            mutable KeyString::TypeBits _typeBits;
        };

        class RocksUniqueCursor : public RocksCursorBase {
        public:
            RocksUniqueCursor(OperationContext* txn, rocksdb::DB* db,
                              boost::shared_ptr<rocksdb::ColumnFamilyHandle> columnFamily,
                              bool forward, Ordering order)
                : RocksCursorBase(txn, db, columnFamily, forward, order), _recordsIndex(0) {}

            virtual void invalidateCache() {
                RocksCursorBase::invalidateCache();
                _records.clear();
            }

            virtual void fillKey(const BSONObj& key, RecordId loc) {
                invalidateCache();
                _key.resetToKey(key, _order);  // loc doesn't go in _key for unique indexes
            }

            virtual bool _locate(RecordId loc) {
                if (!seekCursor()) {
                    // If didn't seek to exact key, start at beginning of wherever we ended up.
                    return false;
                }
                dassert(!isEOF());

                if (loc.isNull()) {
                    // Null loc means means start and beginning or end of array as needed.
                    // so nothing to do
                    return true;
                }

                // If we get here we need to make sure we are positioned at the correct point of the
                // _records vector.
                if (_forward) {
                    while (getRecordId() < loc) {
                        _recordsIndex++;
                        if (_recordsIndex == _records.size()) {
                            // This means we exhausted the scan and didn't find a record in range.
                            advanceCursor();
                            return false;
                        }
                    }
                } else {
                    while (getRecordId() > loc) {
                        _recordsIndex++;
                        if (_recordsIndex == _records.size()) {
                            advanceCursor();
                            return false;
                        }
                    }
                }

                return true;
            }

            virtual RecordId getRecordId() const {
                if (isEOF()) {
                    return RecordId();
                }

                loadValueIfNeeded();
                dassert(!_records[_recordsIndex].first.isNull());
                return _records[_recordsIndex].first;
            }

            virtual void advance() {
                // Advance on a cursor at the end is a no-op
                if (isEOF()) {
                    return;
                }

                // We may just be advancing within the RecordIds for this key.
                loadValueIfNeeded();
                _recordsIndex++;
                if (_recordsIndex == _records.size()) {
                    advanceCursor();
                }
            }

            virtual const KeyString::TypeBits& getTypeBits() const {
                invariant(!isEOF());
                loadValueIfNeeded();
                return _records[_recordsIndex].second;
            }

        private:
            void loadValueIfNeeded() const {
                if (!_records.empty()) {
                    return;
                }

                _recordsIndex = 0;
                auto value = _iterator->value();
                BufReader br(value.data(), value.size());
                while (br.remaining()) {
                    RecordId loc = KeyString::decodeRecordId(&br);
                    _records.push_back(std::make_pair(loc, KeyString::TypeBits::fromBuffer(&br)));
                }
                invariant(!_records.empty());

                if (!_forward) {
                    std::reverse(_records.begin(), _records.end());
                }
            }

            mutable size_t _recordsIndex;
            mutable std::vector<std::pair<RecordId, KeyString::TypeBits> > _records;
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

    RocksIndexBase::RocksIndexBase(rocksdb::DB* db,
                                   boost::shared_ptr<rocksdb::ColumnFamilyHandle> cf,
                                   std::string ident, Ordering order)
        : _db(db), _columnFamily(cf), _ident(std::move(ident)), _order(order) {}

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
        boost::scoped_ptr<rocksdb::Iterator> it(ru->NewIterator(_columnFamily.get()));

        it->SeekToFirst();
        return !it->Valid();
    }

    Status RocksIndexBase::initAsEmpty(OperationContext* txn) {
        // no-op
        return Status::OK();
    }

    long long RocksIndexBase::getSpaceUsedBytes(OperationContext* txn) const {
        // TODO provide GetLiveFilesMetadata() with column family
        std::vector<rocksdb::LiveFileMetaData> metadata;
        _db->GetLiveFilesMetaData(&metadata);
        uint64_t spaceUsedBytes = 0;
        for (const auto& m : metadata) {
            if (m.column_family_name == _ident) {
                spaceUsedBytes += m.size;
            }
        }

        uint64_t walSpaceUsed = 0;
        _db->GetIntProperty(_columnFamily.get(), "rocksdb.cur-size-all-mem-tables", &walSpaceUsed);
        return spaceUsedBytes + walSpaceUsed;
    }

    std::string RocksIndexBase::_getTransactionID(const KeyString& key) const {
        // TODO optimize in the future
        return _ident + std::string(key.getBuffer(), key.getSize());
    }

    /// RocksUniqueIndex

    RocksUniqueIndex::RocksUniqueIndex(rocksdb::DB* db,
                                       boost::shared_ptr<rocksdb::ColumnFamilyHandle> cf,
                                       std::string ident, Ordering order)
        : RocksIndexBase(db, cf, ident, order) {}

    Status RocksUniqueIndex::insert(OperationContext* txn, const BSONObj& key, const RecordId& loc,
                                    bool dupsAllowed) {
        Status s = checkKeySize(key);
        if (!s.isOK()) {
            return s;
        }

        auto ru = RocksRecoveryUnit::getRocksRecoveryUnit(txn);

        KeyString encodedKey(key, _order);
        rocksdb::Slice keySlice(encodedKey.getBuffer(), encodedKey.getSize());

        if (!ru->transaction()->registerWrite(_getTransactionID(encodedKey))) {
            throw WriteConflictException();
        }

        std::string currentValue;
        auto getStatus = ru->Get(_columnFamily.get(), keySlice, &currentValue);
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
            ru->writeBatch()->Put(_columnFamily.get(), keySlice, valueSlice);
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
        ru->writeBatch()->Put(_columnFamily.get(), keySlice, valueVectorSlice);
        return Status::OK();
    }

    void RocksUniqueIndex::unindex(OperationContext* txn, const BSONObj& key, const RecordId& loc,
                                   bool dupsAllowed) {
        KeyString encodedKey(key, _order);
        rocksdb::Slice keySlice(encodedKey.getBuffer(), encodedKey.getSize());
        RocksRecoveryUnit* ru = RocksRecoveryUnit::getRocksRecoveryUnit(txn);

        // We can't let two threads unindex the same key
        if (!ru->transaction()->registerWrite(_getTransactionID(encodedKey))) {
            throw WriteConflictException();
        }

        if (!dupsAllowed) {
            ru->writeBatch()->Delete(_columnFamily.get(), keySlice);
            return;
        }

        // dups are allowed, so we have to deal with a vector of RecordIds.
        std::string currentValue;
        auto getStatus = ru->Get(_columnFamily.get(), keySlice, &currentValue);
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
                    ru->writeBatch()->Delete(_columnFamily.get(), keySlice);
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
        ru->writeBatch()->Put(_columnFamily.get(), keySlice, newValueSlice);
    }

    SortedDataInterface::Cursor* RocksUniqueIndex::newCursor(OperationContext* txn,
                                                             int direction) const {
        invariant( ( direction == 1 || direction == -1 ) && "invalid value for direction" );
        return new RocksUniqueCursor(txn, _db, _columnFamily, direction == 1, _order);
    }

    Status RocksUniqueIndex::dupKeyCheck(OperationContext* txn, const BSONObj& key,
                                         const RecordId& loc) {
        KeyString encodedKey(key, _order);
        rocksdb::Slice keySlice(encodedKey.getBuffer(), encodedKey.getSize());

        auto ru = RocksRecoveryUnit::getRocksRecoveryUnit(txn);
        std::string value;
        auto getStatus = ru->Get(_columnFamily.get(), keySlice, &value);
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
    RocksStandardIndex::RocksStandardIndex(rocksdb::DB* db,
                                           boost::shared_ptr<rocksdb::ColumnFamilyHandle> cf,
                                           std::string ident, Ordering order)
        : RocksIndexBase(db, cf, ident, order) {}

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
        rocksdb::Slice keySlice(encodedKey.getBuffer(), encodedKey.getSize());

        auto ru = RocksRecoveryUnit::getRocksRecoveryUnit(txn);

        rocksdb::Slice value;
        if (!encodedKey.getTypeBits().isAllZeros()) {
            value =
                rocksdb::Slice(reinterpret_cast<const char*>(encodedKey.getTypeBits().getBuffer()),
                               encodedKey.getTypeBits().getSize());
        }

        ru->writeBatch()->Put(_columnFamily.get(),
                              rocksdb::Slice(encodedKey.getBuffer(), encodedKey.getSize()), value);

        return Status::OK();
    }

    void RocksStandardIndex::unindex(OperationContext* txn, const BSONObj& key, const RecordId& loc,
                                     bool dupsAllowed) {
        invariant(dupsAllowed);
        // If we're unindexing an index element, this means we already "locked" the RecordId of the
        // document. No need to register write here

        KeyString encodedKey(key, _order, loc);
        rocksdb::Slice keySlice(encodedKey.getBuffer(), encodedKey.getSize());
        auto ru = RocksRecoveryUnit::getRocksRecoveryUnit(txn);
        ru->writeBatch()->Delete(_columnFamily.get(), keySlice);
    }

    SortedDataInterface::Cursor* RocksStandardIndex::newCursor(OperationContext* txn,
                                                               int direction) const {
        invariant( ( direction == 1 || direction == -1 ) && "invalid value for direction" );
        return new RocksStandardCursor(txn, _db, _columnFamily, direction == 1, _order);
    }


}  // namespace mongo
