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

#include "mongo/db/storage/rocks/rocks_sorted_data_impl.h"

#include <string>

#include <rocksdb/comparator.h>
#include <rocksdb/db.h>
#include <rocksdb/iterator.h>

#include "mongo/db/storage/rocks/rocks_engine.h"
#include "mongo/db/storage/rocks/rocks_record_store.h"
#include "mongo/db/storage/rocks/rocks_recovery_unit.h"
#include "mongo/util/log.h"

namespace mongo {

    namespace {

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

        /**
         * Rocks cursor
         */
        class RocksCursor : public SortedDataInterface::Cursor {
        public:
            RocksCursor( rocksdb::Iterator* iterator, bool forward, Ordering o )
                : _iterator( iterator ), _forward( forward ), _isCached( false ), _comparator( o ) {

                // TODO: maybe don't seek until we know we need to?
                if ( _forward )
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
                // if key is an empty BSONObj, the default behavior is to seek to the
                // beginning of the iterator and return false.
                if ( key == BSONObj() ) {
                    if ( _forward ) {
                        _iterator->SeekToFirst();
                    } else {
                        _iterator->SeekToLast();
                    }

                    return false;
                }

                return _locate( stripFieldNames( key ), loc );
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

                _locate( key, DiskLoc() );
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

            void restorePosition(OperationContext*) {
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
                    _iterator->SeekToLast();
                    _checkStatus();

                    if ( _iterator->Valid() ) {
                        _load();
                        IndexKeyEntry smallestEntry( _cachedKey, _cachedLoc );

                        if ( _comparator.compare( keyEntry, smallestEntry ) < 0 ) {
                            // a reverse iterator seeking lower than the lowest key is left in an
                            // invalid position.
                            advance();
                            invariant( isEOF() );
                        } else {
                            // a reverse iterator seeking higher than highest key is positioned on
                            // the highest key.
                            _iterator->SeekToLast();
                            _checkStatus();
                            _load();
                            IndexKeyEntry largestEntry( _cachedKey, _cachedLoc );
                            invariant( _comparator.compare( keyEntry, largestEntry ) > 0 );
                        }
                    }

                    return false;
                }

                _load();
                IndexKeyEntry foundEntry( _cachedKey, _cachedLoc );
                if ( _comparator.compare( keyEntry, foundEntry ) == 0 ) {
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
                if ( !_forward ) {
                    return _reverseLocate( key, loc );
                }

                _isCached = false;
                // assumes fieldNames already stripped if necessary
                const string keyData = makeString( key, loc, false );
                _iterator->Seek( keyData );
                _checkStatus();
                if ( !_iterator->Valid() )
                    return false;

                _load();
                return _comparator.compare( IndexKeyEntry( key, loc ),
                                            IndexKeyEntry( _cachedKey, _cachedLoc ) ) == 0;
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

                virtual int Compare( const rocksdb::Slice& a, const rocksdb::Slice& b ) const {
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
    } // namespace

    // RocksSortedDataImpl***********

    RocksSortedDataImpl::RocksSortedDataImpl( rocksdb::DB* db,
            rocksdb::ColumnFamilyHandle* cf,
            Ordering order ) : _db( db ), _columnFamily( cf ), _order( order ) {
        invariant( _db );
        invariant( _columnFamily );
    }

    SortedDataBuilderInterface* RocksSortedDataImpl::getBulkBuilder(OperationContext* txn,
                                                                    bool dupsAllowed) {
        invariant( !"getBulkBuilder not yet implemented" );
    }

    Status RocksSortedDataImpl::insert(OperationContext* txn,
                                       const BSONObj& key,
                                       const DiskLoc& loc,
                                       bool dupsAllowed) {

        RocksRecoveryUnit* ru = _getRecoveryUnit( txn );

        if ( !dupsAllowed ) {
            // TODO need key locking to support unique indexes.
            Status status = dupKeyCheck( txn, key, loc );
            if ( !status.isOK() ) {
                return status;
            }
        }

        ru->writeBatch()->Put( _columnFamily, makeString( key, loc ), emptyByteSlice );

        return Status::OK();
    }

    bool RocksSortedDataImpl::unindex(OperationContext* txn,
                                      const BSONObj& key,
                                      const DiskLoc& loc) {
        RocksRecoveryUnit* ru = _getRecoveryUnit( txn );

        const string keyData = makeString( key, loc );

        string dummy;
        const rocksdb::ReadOptions options = RocksEngine::readOptionsWithSnapshot( txn );
        if ( !_db->KeyMayExist( options,_columnFamily, keyData, &dummy ) ) {
            return false;
        }

        ru->writeBatch()->Delete( _columnFamily, keyData );
        return true; // XXX: fix? does it matter since its so slow to check?
    }

    string RocksSortedDataImpl::dupKeyError(const BSONObj& key) const {
        stringstream ss;
        ss << "E11000 duplicate key error ";
        // TODO figure out how to include index name without dangerous casts
        ss << "dup key: " << key.toString();
        return ss.str();
    }

    Status RocksSortedDataImpl::dupKeyCheck(OperationContext* txn,
                                            const BSONObj& key,
                                            const DiskLoc& loc) {
        boost::scoped_ptr<SortedDataInterface::Cursor> cursor( newCursor( txn, 1 ) );

        if ( !cursor->locate( key, DiskLoc() ) || cursor->isEOF() ||
             cursor->getDiskLoc() == loc ) {
            return Status::OK();
        } else {
            return Status( ErrorCodes::DuplicateKey, dupKeyError( key ) );
        }
    }

    void RocksSortedDataImpl::fullValidate(OperationContext* txn, long long* numKeysOut) const {
        // XXX: no key counts
        if ( numKeysOut ) {
            *numKeysOut = -1;
        }
    }

    bool RocksSortedDataImpl::isEmpty( OperationContext* txn ) {
        // XXX doesn't use snapshot
        boost::scoped_ptr<rocksdb::Iterator> it( _db->NewIterator( rocksdb::ReadOptions(),
                                                                   _columnFamily ) );

        it->SeekToFirst();
        return it->Valid();
    }

    Status RocksSortedDataImpl::touch(OperationContext* txn) const {
        // no-op
        return Status::OK();
    }

    SortedDataInterface::Cursor* RocksSortedDataImpl::newCursor(OperationContext* txn,
                                                                int direction) const {
        invariant( ( direction == 1 || direction == -1 ) && "invalid value for direction" );
        rocksdb::ReadOptions options = RocksEngine::readOptionsWithSnapshot( txn );
        return new RocksCursor(
                _db->NewIterator( options, _columnFamily ), direction == 1, _order );
    }

    Status RocksSortedDataImpl::initAsEmpty(OperationContext* txn) {
        // no-op
        return Status::OK();
    }

    long long RocksSortedDataImpl::getSpaceUsedBytes( OperationContext* txn ) const {
        boost::scoped_ptr<rocksdb::Iterator> iter( _db->NewIterator( rocksdb::ReadOptions(),
                                                                     _columnFamily ) );
        iter->SeekToFirst();
        const rocksdb::Slice rangeStart = iter->key();
        iter->SeekToLast();
        // This is exclusive when we would prefer it be inclusive.
        // AFB best way to get approximate size for a whole column family.
        const rocksdb::Slice rangeEnd = iter->key();

        rocksdb::Range fullIndexRange( rangeStart, rangeEnd );
        uint64_t spacedUsedBytes = 0;

        // TODO Rocks specifies that this may not include recently written data. Figure out if
        // that's okay
        _db->GetApproximateSizes( _columnFamily, &fullIndexRange, 1, &spacedUsedBytes );

        return spacedUsedBytes;
    }

    // ownership passes to caller
    rocksdb::Comparator* RocksSortedDataImpl::newRocksComparator( const Ordering& order ) {
        return new RocksIndexEntryComparator( order );
    }

    RocksRecoveryUnit* RocksSortedDataImpl::_getRecoveryUnit( OperationContext* opCtx ) const {
        return dynamic_cast<RocksRecoveryUnit*>( opCtx->recoveryUnit() );
    }

}
