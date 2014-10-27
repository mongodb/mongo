// rocks_sorted_data_impl.cpp

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

#include "mongo/db/storage/rocks/rocks_sorted_data_impl.h"

#include <cstdlib>
#include <string>

#include <rocksdb/comparator.h>
#include <rocksdb/db.h>
#include <rocksdb/iterator.h>
#include <rocksdb/utilities/write_batch_with_index.h>

#include "mongo/db/storage/rocks/rocks_engine.h"
#include "mongo/db/storage/rocks/rocks_record_store.h"
#include "mongo/db/storage/rocks/rocks_recovery_unit.h"
#include "mongo/util/log.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {

    namespace {

        const int kTempKeyMaxSize = 1024; // Do the same as the heap implementation

        rocksdb::Slice emptyByteSlice( "" );
        rocksdb::SliceParts emptyByteSliceParts( &emptyByteSlice, 1 );

        // functions for converting between BSONObj-DiskLoc pairs and strings/rocksdb::Slices

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

        /**
         * Constructs a string containing the bytes of key followed by the bytes of loc.
         *
         * @param removeFieldNames true if the field names in key should be replaced with empty
         * strings, and false otherwise. Useful because field names are not necessary in an index
         * key, because the ordering of the fields is already known.
         */
        string makeString( const BSONObj& key, const DiskLoc loc, bool removeFieldNames = true ) {
            const BSONObj& finalKey = removeFieldNames ? stripFieldNames( key ) : key;
            string s( finalKey.objdata(), finalKey.objsize() );
            s.append( reinterpret_cast<const char*>( &loc ), sizeof( DiskLoc ) );

            return s;
        }

        /**
         * Constructs an IndexKeyEntry from a slice containing the bytes of a BSONObject followed
         * by the bytes of a DiskLoc
         */
        IndexKeyEntry makeIndexKeyEntry( const rocksdb::Slice& slice ) {
            BSONObj key = BSONObj( slice.data() ).getOwned();
            DiskLoc loc = *reinterpret_cast<const DiskLoc*>( slice.data() + key.objsize() );
            return IndexKeyEntry( key, loc );
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
                        boost::shared_ptr<rocksdb::ColumnFamilyHandle> columnFamily, bool forward, Ordering o)
                : _db(db),
                  _columnFamily(columnFamily),
                  _forward(forward),
                  _isCached(false),
                  _comparator(o) {
                _resetIterator(txn);

                // TODO: maybe don't seek until we know we need to?
                if (_forward)
                    _iterator->SeekToFirst();
                else
                    _iterator->SeekToLast();
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

            void aboutToDeleteBucket(const DiskLoc& bucket) {
                invariant( !"aboutToDeleteBucket should never be called from RocksSortedDataImpl" );
            }

            bool locate(const BSONObj& key, const DiskLoc& loc) {
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
                    _locate(key, minDiskLoc);
                } else {
                    _reverseLocate(key, maxDiskLoc);
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

            DiskLoc getDiskLoc() const {
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
                _savePositionLoc = getDiskLoc();
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
            bool _reverseLocate( const BSONObj& key, const DiskLoc loc ) {
                invariant( !_forward );

                const IndexKeyEntry keyEntry( key, loc );

                _isCached = false;
                // assumes fieldNames already stripped if necessary
                const string keyData = makeString( key, loc, false );
                _iterator->Seek( keyData );
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
            bool _locate( const BSONObj& key, const DiskLoc loc ) {
                invariant(_forward);

                _isCached = false;
                // assumes fieldNames already stripped if necessary
                const string keyData = makeString( key, loc, false );
                _iterator->Seek( keyData );
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
                rocksdb::Slice slice = _iterator->key();
                _cachedKey = BSONObj( slice.data() ).getOwned();
                _cachedLoc = *reinterpret_cast<const DiskLoc*>( slice.data() +
                                                                _cachedKey.objsize() );
            }

            rocksdb::DB* _db;                                       // not owned
            boost::shared_ptr<rocksdb::ColumnFamilyHandle> _columnFamily;
            scoped_ptr<rocksdb::Iterator> _iterator;
            const bool _forward;

            mutable bool _isCached;
            mutable BSONObj _cachedKey;
            mutable DiskLoc _cachedLoc;

            // not for caching, but rather for savePosition() and restorePosition()
            bool _savedAtEnd;
            BSONObj _savePositionObj;
            DiskLoc _savePositionLoc;

            // Used for comparing elements in reverse iterators. Because the rocksdb::Iterator is
            // only a forward iterator, it is sometimes necessary to compare index keys manually
            // when implementing a reverse iterator.
            IndexEntryComparison _comparator;
        };

        /**
         * Custom comparator for rocksdb used to compare Index Entries by BSONObj and DiskLoc
         */
        class RocksIndexEntryComparator : public rocksdb::Comparator {
            public:
                RocksIndexEntryComparator( const Ordering& order ): _indexComparator( order ) { }

                virtual int Compare(const rocksdb::Slice& a, const rocksdb::Slice& b) const {
                    if (a.size() == 0 || b.size() == 0) {
                        return a.size() == b.size() ? 0 : ((a.size() == 0) ? -1 : 1);
                    }
                    const IndexKeyEntry lhs = makeIndexKeyEntry( a );
                    const IndexKeyEntry rhs = makeIndexKeyEntry( b );
                    return _indexComparator.compare( lhs, rhs );
                }

                virtual const char* Name() const {
                    // changing this means that any existing mongod instances using rocks storage
                    // engine will not be able to start up again, because the comparator's name
                    // will not match
                    return "mongodb.RocksIndexEntryComparator";
                }

                virtual void FindShortestSeparator( std::string* start,
                        const rocksdb::Slice& limit ) const { }

                virtual void FindShortSuccessor( std::string* key ) const { }

            private:
                const IndexEntryComparison _indexComparator;
        };

    class WriteBufferCopyIntoHandler : public rocksdb::WriteBatch::Handler {
    public:
        WriteBufferCopyIntoHandler(rocksdb::WriteBatch* outWriteBatch,
                                   rocksdb::ColumnFamilyHandle* columnFamily) :
                _OutWriteBatch(outWriteBatch),
                _columnFamily(columnFamily) { }

        rocksdb::Status PutCF(uint32_t columnFamilyId, const rocksdb::Slice& key,
                              const rocksdb::Slice& value) {
            invariant(_OutWriteBatch);
            _OutWriteBatch->Put(_columnFamily, key, value);
            return rocksdb::Status::OK();
        }

    private:
        rocksdb::WriteBatch* _OutWriteBatch;
        rocksdb::ColumnFamilyHandle* _columnFamily;
    };

    class RocksBulkSortedBuilderImpl : public SortedDataBuilderInterface {
    public:
        RocksBulkSortedBuilderImpl(RocksSortedDataImpl* index, OperationContext* txn,
                                   bool dupsAllowed)
            : _index(index), _txn(txn), _dupsAllowed(dupsAllowed) {
            invariant(index->isEmpty(txn));
        }

        Status addKey(const BSONObj& key, const DiskLoc& loc) {
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
                                       const DiskLoc& loc,
                                       bool dupsAllowed) {

        if (key.objsize() >= kTempKeyMaxSize) {
            string msg = mongoutils::str::stream()
                         << "RocksSortedDataImpl::insert: key too large to index, failing " << ' '
                         << key.objsize() << ' ' << key;
                return Status(ErrorCodes::KeyTooLong, msg);
        }

        RocksRecoveryUnit* ru = RocksRecoveryUnit::getRocksRecoveryUnit(txn);

        if ( !dupsAllowed ) {
            // TODO need key locking to support unique indexes.
            Status status = dupKeyCheck( txn, key, loc );
            if ( !status.isOK() ) {
                return status;
            }
        }

        ru->incrementCounter(_numEntriesKey, &_numEntries, 1);

        ru->writeBatch()->Put(_columnFamily.get(), makeString(key, loc), emptyByteSlice);

        return Status::OK();
    }

    void RocksSortedDataImpl::unindex(OperationContext* txn,
                                      const BSONObj& key,
                                      const DiskLoc& loc,
                                      bool dupsAllowed) {
        RocksRecoveryUnit* ru = RocksRecoveryUnit::getRocksRecoveryUnit(txn);

        const string keyData = makeString( key, loc );

        string dummy;
        if (ru->Get(_columnFamily.get(), keyData, &dummy).IsNotFound()) {
            return;
        }

        ru->incrementCounter(_numEntriesKey, &_numEntries,  -1);

        ru->writeBatch()->Delete(_columnFamily.get(), keyData);
    }

    Status RocksSortedDataImpl::dupKeyCheck(OperationContext* txn,
                                            const BSONObj& key,
                                            const DiskLoc& loc) {
        boost::scoped_ptr<SortedDataInterface::Cursor> cursor(newCursor(txn, 1));
        cursor->locate(key, DiskLoc(0, 0));

        if (cursor->isEOF() || cursor->getKey() != key || cursor->getDiskLoc() == loc) {
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

    Status RocksSortedDataImpl::touch(OperationContext* txn) const {
        boost::scoped_ptr<rocksdb::Iterator> itr;
        // no need to use snapshot to load into memory
        itr.reset(_db->NewIterator(rocksdb::ReadOptions(), _columnFamily.get()));
        itr->SeekToFirst();
        for (; itr->Valid(); itr->Next()) {
            invariant(itr->status().ok());
        }

        return Status::OK();
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

    // ownership passes to caller
    rocksdb::Comparator* RocksSortedDataImpl::newRocksComparator( const Ordering& order ) {
        return new RocksIndexEntryComparator( order );
    }

}
