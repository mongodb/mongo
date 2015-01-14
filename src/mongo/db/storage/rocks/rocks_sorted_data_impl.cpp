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

#include "mongo/db/storage/rocks/rocks_sorted_data_impl.h"

#include <boost/scoped_ptr.hpp>
#include <boost/shared_ptr.hpp>
#include <cstdlib>
#include <sstream>
#include <string>
#include <vector>

#include <rocksdb/db.h>
#include <rocksdb/iterator.h>
#include <rocksdb/utilities/write_batch_with_index.h>

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

        const int kTempKeyMaxSize = 1024; // Do the same as the heap implementation

        // functions for converting between BSONObj-RecordId pairs and strings/rocksdb::Slices

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
            // TODO figure out how to include index name without dangerous casts
            ss << "dup key: " << key.toString();
            return ss.str();
        }

        /**
         * Rocks cursor
         */
        class RocksCursor : public SortedDataInterface::Cursor {
        public:
            RocksCursor(OperationContext* txn, rocksdb::DB* db,
                        boost::shared_ptr<rocksdb::ColumnFamilyHandle> columnFamily, bool forward,
                        Ordering order)
                : _db(db),
                  _columnFamily(columnFamily),
                  _forward(forward),
                  _order(order),
                  _isCached(false) {
                _resetIterator(txn);
                _checkStatus();
            }

            int getDirection() const {
                return _forward ? 1 : -1;
            }

            bool isEOF() const {
                return !_iterator->Valid();
            }

            /**
             * Will only be called with other from same index as this.
             * All EOF locs should be considered equal.
             */
            bool pointsToSamePlaceAs(const Cursor& other) const {
                const RocksCursor* realOther = dynamic_cast<const RocksCursor*>( &other );

                bool valid = _iterator->Valid();
                bool otherValid = realOther->_iterator->Valid();

                return ( !valid && !otherValid ) ||
                       ( valid && otherValid && _iterator->key() == realOther->_iterator->key() );
            }

            bool locate(const BSONObj& key, const RecordId& loc) {
                if (_forward) {
                    return _locate(stripFieldNames(key), loc);
                } else {
                    return _reverseLocate(stripFieldNames(key), loc);
                }
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

                if (_forward) {
                    _locate(key, RecordId::min());
                } else {
                    _reverseLocate(key, RecordId::max());
                }
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
                // XXX I think these do the same thing????
                advanceTo( keyBegin, keyBeginLen, afterVersion, keyEnd, keyEndInclusive );
            }

            BSONObj getKey() const {
                _load();
                return _cachedKey;
            }

            RecordId getRecordId() const {
                _load();
                return _cachedLoc;
            }

            void advance() {
                if ( _forward ) {
                    _iterator->Next();
                } else {
                    _iterator->Prev();
                }
                _isCached = false;
            }

            void savePosition() {
                if ( isEOF() ) {
                    _savedAtEnd = true;
                    return;
                }

                _savedAtEnd = false;
                _savePositionObj = getKey().getOwned();
                _savePositionLoc = getRecordId();
            }

            void restorePosition(OperationContext* txn) {
                _resetIterator(txn);
                _isCached = false;

                if ( _savedAtEnd ) {
                    if ( _forward ) {
                        _iterator->SeekToLast();
                    } else {
                        _iterator->SeekToFirst();
                    }

                    if ( _iterator->Valid() ) {
                        advance();
                    }

                    invariant( !_iterator->Valid() );
                    return;
                }

                // locate takes care of the case where the position that we are restoring has
                // already been deleted
                locate( _savePositionObj, _savePositionLoc );
            }

        private:
            void _resetIterator(OperationContext* txn) {
                invariant(txn);
                invariant(_db);
                auto ru = RocksRecoveryUnit::getRocksRecoveryUnit(txn);
                _iterator.reset(ru->NewIterator(_columnFamily.get()));
            }

            // _locate() for reverse iterators
            bool _reverseLocate( const BSONObj& key, const RecordId loc ) {
                invariant( !_forward );


                _isCached = false;
                // assumes fieldNames already stripped if necessary
                KeyString encodedKey(key, _order, loc);
                _iterator->Seek(rocksdb::Slice(encodedKey.getBuffer(), encodedKey.getSize()));
                _checkStatus();

                if ( !_iterator->Valid() ) { // seeking outside the range of the index
                    // this will give lower bound behavior
                    _iterator->SeekToLast();
                    _checkStatus();
                    return false;
                }

                _load();
                if (loc == _cachedLoc && key == _cachedKey) {
                    return true;
                }

                // if we can't find the exact result, we need to call advance() so that we're at the
                // first value less than (to the left of) what we were searching for, rather than
                // the first value greater than (to the right of) the value we were searching for.
                invariant( !isEOF() );
                advance();

                return false;
            }

            /**
             * locate function which takes in a IndexKeyEntry. This logic is abstracted out into a
             * helper so that its possible to choose whether or not to strip the fieldnames before
             * performing the actual locate logic.
             */
            bool _locate( const BSONObj& key, const RecordId loc ) {
                invariant(_forward);


                _isCached = false;
                // assumes fieldNames already stripped if necessary
                KeyString encodedKey(key, _order, loc);
                _iterator->Seek(rocksdb::Slice(encodedKey.getBuffer(), encodedKey.getSize()));
                _checkStatus();
                if ( !_iterator->Valid() )
                    return false;

                _load();
                return loc == _cachedLoc && key == _cachedKey;
            }

            void _checkStatus() {
                // TODO: Fix me
                if ( !_iterator->status().ok() ) {
                    log() << _iterator->status().ToString();
                    invariant( false );
                }
            }

            /**
             * Loads the cached key and diskloc. Do not call if isEOF() is true
             */
            void _load() const {
                invariant( !isEOF() );

                if ( _isCached ) {
                    return;
                }

                _isCached = true;
                rocksdb::Slice iterKey = _iterator->key();
                rocksdb::Slice iterValue = _iterator->value();
                KeyString::TypeBits typeBits;
                BufReader br(iterValue.data(), iterValue.size());
                typeBits.resetFromBuffer(&br);
                _cachedKey = KeyString::toBson(iterKey.data(), iterKey.size(), _order, typeBits);
                _cachedLoc = KeyString::decodeRecordIdAtEnd(iterKey.data(), iterKey.size());
            }

            rocksdb::DB* _db;                                       // not owned
            boost::shared_ptr<rocksdb::ColumnFamilyHandle> _columnFamily;
            scoped_ptr<rocksdb::Iterator> _iterator;
            const bool _forward;
            Ordering _order;

            mutable bool _isCached;
            mutable BSONObj _cachedKey;
            mutable RecordId _cachedLoc;

            // not for caching, but rather for savePosition() and restorePosition()
            bool _savedAtEnd;
            BSONObj _savePositionObj;
            RecordId _savePositionLoc;
        };

        class RocksBulkSortedBuilderImpl : public SortedDataBuilderInterface {
        public:
            RocksBulkSortedBuilderImpl(RocksSortedDataImpl* index, OperationContext* txn,
                                       bool dupsAllowed)
                : _index(index), _txn(txn), _dupsAllowed(dupsAllowed) {
                invariant(index->isEmpty(txn));
            }

            Status addKey(const BSONObj& key, const RecordId& loc) {
                // TODO maybe optimize based on a fact that index is empty?
                return _index->insert(_txn, key, loc, _dupsAllowed);
            }

            void commit(bool mayInterrupt) {
                WriteUnitOfWork uow(_txn);
                uow.commit();
            }

        private:
            RocksSortedDataImpl* _index;
            OperationContext* _txn;
            bool _dupsAllowed;
        };

    } // namespace

    // RocksSortedDataImpl***********

    RocksSortedDataImpl::RocksSortedDataImpl(rocksdb::DB* db,
                                             boost::shared_ptr<rocksdb::ColumnFamilyHandle> cf,
                                             std::string ident, Ordering order)
        : _db(db),
          _columnFamily(cf),
          _ident(std::move(ident)),
          _order(order),
          _numEntriesKey("numentries-" + _ident) {
        invariant(_db);
        invariant(_columnFamily.get());
        _numEntries = 0;
        string value;
        if (_db->Get(rocksdb::ReadOptions(), rocksdb::Slice(_numEntriesKey), &value).ok()) {
            long long numEntries;
            memcpy(&numEntries, value.data(), sizeof(numEntries));
            _numEntries.store(numEntries);
        } else {
            _numEntries.store(0);
        }
    }

    SortedDataBuilderInterface* RocksSortedDataImpl::getBulkBuilder(OperationContext* txn,
                                                                    bool dupsAllowed) {
        return new RocksBulkSortedBuilderImpl(this, txn, dupsAllowed);
    }

    Status RocksSortedDataImpl::insert(OperationContext* txn,
                                       const BSONObj& key,
                                       const RecordId& loc,
                                       bool dupsAllowed) {

        if (key.objsize() >= kTempKeyMaxSize) {
            string msg = mongoutils::str::stream()
                         << "RocksSortedDataImpl::insert: key too large to index, failing " << ' '
                         << key.objsize() << ' ' << key;
                return Status(ErrorCodes::KeyTooLong, msg);
        }

        if ( !dupsAllowed ) {
            // TODO this will encode KeyString twice (in dupKeyCheck and below in this function).
            // However, once we change unique index format, this will be optimized
            Status status = dupKeyCheck(txn, stripFieldNames(key), loc);
            if ( !status.isOK() ) {
                return status;
            }
        }

        KeyString encodedKey(key, _order, loc);

        auto ru = RocksRecoveryUnit::getRocksRecoveryUnit(txn);
        ru->incrementCounter(_numEntriesKey, &_numEntries, 1);

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

    void RocksSortedDataImpl::unindex(OperationContext* txn,
                                      const BSONObj& key,
                                      const RecordId& loc,
                                      bool dupsAllowed) {
        // If we're unindexing an index element, this means we already "locked" the RecordId of the
        // document. No need to register write here

        KeyString encodedKey(key, _order, loc);
        rocksdb::Slice keyItem(encodedKey.getBuffer(), encodedKey.getSize());

        string dummy;
        RocksRecoveryUnit* ru = RocksRecoveryUnit::getRocksRecoveryUnit(txn);
        if (ru->Get(_columnFamily.get(), keyItem, &dummy).IsNotFound()) {
            return;
        }

        ru->incrementCounter(_numEntriesKey, &_numEntries,  -1);

        ru->writeBatch()->Delete(_columnFamily.get(), keyItem);
    }

    Status RocksSortedDataImpl::dupKeyCheck(OperationContext* txn, const BSONObj& key,
                                            const RecordId& loc) {
        // To check if we're writing a duplicate key, we need to check two things:
        // 1. Has there been any writes to this key since our snapshot (committed or uncommitted)
        // 2. Is the key present in the snapshot
        // Note that this doesn't catch the situation where the key is present in the snapshot, but
        // deleted after the snapshot. This should be fine
        auto ru = RocksRecoveryUnit::getRocksRecoveryUnit(txn);
        // This will check if there is any write to the key after our snapshot (committed or
        // uncommitted). If the answer is yes, we will return dupkey.
        KeyString encodedKey(key, _order);
        if (!ru->transaction()->registerWrite(_getTransactionID(encodedKey))) {
            return Status(ErrorCodes::DuplicateKey, dupKeyError(key));
        }

        // This checks if the key is present in the snapshot (2)
        boost::scoped_ptr<SortedDataInterface::Cursor> cursor(newCursor(txn, 1));
        cursor->locate(key, RecordId::min());

        if (cursor->isEOF() || cursor->getKey() != key || cursor->getRecordId() == loc) {
            return Status::OK();
        } else {
            return Status(ErrorCodes::DuplicateKey, dupKeyError(key));
        }
    }

    void RocksSortedDataImpl::fullValidate(OperationContext* txn, bool full, long long* numKeysOut,
                                           BSONObjBuilder* output) const {
        if (numKeysOut) {
            *numKeysOut = 0;
            auto ru = RocksRecoveryUnit::getRocksRecoveryUnit(txn);
            boost::scoped_ptr<rocksdb::Iterator> it(ru->NewIterator(_columnFamily.get()));
            it->SeekToFirst();
            for (*numKeysOut = 0; it->Valid(); it->Next()) {
                ++(*numKeysOut);
            }
        }
    }

    bool RocksSortedDataImpl::isEmpty(OperationContext* txn) {
        auto ru = RocksRecoveryUnit::getRocksRecoveryUnit(txn);
        boost::scoped_ptr<rocksdb::Iterator> it(ru->NewIterator(_columnFamily.get()));

        it->SeekToFirst();
        return !it->Valid();
    }

    long long RocksSortedDataImpl::numEntries(OperationContext* txn) const {
        auto ru = RocksRecoveryUnit::getRocksRecoveryUnit(txn);
        return _numEntries.load(std::memory_order::memory_order_relaxed) +
            ru->getDeltaCounter(_numEntriesKey);
    }

    SortedDataInterface::Cursor* RocksSortedDataImpl::newCursor(OperationContext* txn,
                                                                int direction) const {
        invariant( ( direction == 1 || direction == -1 ) && "invalid value for direction" );
        return new RocksCursor(txn, _db, _columnFamily, direction == 1, _order);
    }

    Status RocksSortedDataImpl::initAsEmpty(OperationContext* txn) {
        // no-op
        return Status::OK();
    }

    long long RocksSortedDataImpl::getSpaceUsedBytes(OperationContext* txn) const {
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

    std::string RocksSortedDataImpl::_getTransactionID(const KeyString& key) const {
        // TODO optimize in the future
        return _ident + std::string(key.getBuffer(), key.getSize());
    }
}
