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

#include <rocksdb/db.h>
#include <rocksdb/iterator.h>

#include "mongo/db/storage/rocks/rocks_engine.h"
#include "mongo/db/storage/rocks/rocks_record_store.h"
#include "mongo/db/storage/rocks/rocks_recovery_unit.h"

namespace mongo {

    namespace {

        rocksdb::Slice emptyByteSlice( "" );
        rocksdb::SliceParts emptyByteSliceParts( &emptyByteSlice, 1 );

        class RocksCursor : public SortedDataInterface::Cursor {
        public:
            RocksCursor( rocksdb::Iterator* iterator, bool forward )
                : _iterator( iterator ),
                  _forward( forward ),
                  _cached( false ) {

                // TODO: maybe don't seek until we know we need to?
                if ( _forward )
                    _iterator->SeekToFirst();
                else
                    _iterator->SeekToLast();
                _checkStatus();
            }

            virtual ~RocksCursor() { } 

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
                return _iterator->Valid() && realOther->_iterator->Valid() &&
                    _iterator->key() == realOther->_iterator->key();
            }

            void aboutToDeleteBucket(const DiskLoc& bucket) {
                // don't know if I need this or not
            }

            bool locate(const BSONObj& key, const DiskLoc& loc) {
                return _locate( RocksIndexEntry( key, loc ) );
            }


            void advanceTo(const BSONObj &keyBegin,
                           int keyBeginLen,
                           bool afterKey,
                           const vector<const BSONElement*>& keyEnd,
                           const vector<bool>& keyEndInclusive) {
                // XXX does this work with reverse iterators?
                BSONObj key = IndexEntryComparison::makeQueryObject (
                                         keyBegin,
                                         keyBeginLen,
                                         afterKey,
                                         keyEnd,
                                         keyEndInclusive,
                                         getDirection() );

                _locate( RocksIndexEntry( key, DiskLoc(), false ) );
            }

            /**
             * Locate a key with fields comprised of a combination of keyBegin fields and keyEnd
             * fields.
             */
            void customLocate(const BSONObj& keyBegin,
                              int keyBeginLen,
                              bool afterVersion,
                              const vector<const BSONElement*>& keyEnd,
                              const vector<bool>& keyEndInclusive) {
                // XXX I think these do the same thing????
                advanceTo( keyBegin, keyBeginLen, afterVersion, keyEnd, keyEndInclusive );
            }

            /**
             * Return OK if it's not
             * Otherwise return a status that can be displayed
             */
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
                _cached = false;
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

            void restorePosition() {
                _cached = false;

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

            /**
             * locate function which takes in a RocksIndexEntry. This logic is abstracted out into a
             * helper so that its possible to choose whether or not to pass a RocksIndexEntry with
             * the key fields stripped.
             */
            bool _locate( const RocksIndexEntry rie ) {
                _cached = false;
                string keyData = rie.asString();
                _iterator->Seek( keyData );
                _checkStatus();
                if ( !_iterator->Valid() )
                    return false;
                _load();

                // because considerFieldNames is false, it doesn't matter if we stripped the
                // fieldnames or not when constructing rie
                bool compareResult = rie.key().woCompare( _cachedKey, BSONObj(), false ) == 0;

                // if we can't find the result and we have a reverse iterator, we need to call
                // advance() so that we're at the first value less than (to the left of) what we
                // were searching for, rather than the first value greater than (to the right of)
                // the value we were searching for
                if ( !compareResult && !_forward && !isEOF() ) {
                    if ( isEOF() ) {
                        _iterator->SeekToLast();
                    } else {
                        advance();
                    }
                    invariant( !_cached );
                }

                return compareResult;
            }

            void _checkStatus() {
                // TODO: Fix me
                invariant( _iterator->status().ok() );
            }

            /**
             * Loads the cached key and diskloc. Do not call if isEOF() is true
             */
            void _load() const {
                invariant( !isEOF() );

                if ( _cached ) {
                    return;
                }

                _cached = true;
                rocksdb::Slice slice = _iterator->key();
                _cachedKey = BSONObj( slice.data() ).getOwned();
                _cachedLoc = reinterpret_cast<const DiskLoc*>( slice.data() + _cachedKey.objsize() )[0];
            }

            scoped_ptr<rocksdb::Iterator> _iterator;
            OperationContext* _txn; // not owned
            const bool _forward;

            mutable bool _cached;
            mutable BSONObj _cachedKey;
            mutable DiskLoc _cachedLoc;

            // not for caching, but rather for savePosition() and restorePosition()
            mutable bool _savedAtEnd;
            mutable BSONObj _savePositionObj;
            mutable DiskLoc _savePositionLoc;
        };

    }

    // RocksIndexEntry***********
    
    RocksIndexEntry::RocksIndexEntry( const BSONObj& key, const DiskLoc loc, bool stripFieldNames )
        : IndexKeyEntry( key, loc ) {

        if ( stripFieldNames && _key.firstElement().fieldName()[0] ) {
            BSONObjBuilder b;
            BSONObjIterator i( _key );
            while ( i.more() ) {
                BSONElement e = i.next();
                b.appendAs( e, "" );
            }
            _key = b.obj();
        }
    }

    RocksIndexEntry::RocksIndexEntry( const rocksdb::Slice& slice )
        : IndexKeyEntry( BSONObj(), DiskLoc() ) {
        _key = BSONObj( slice.data() ).getOwned();
        _loc = reinterpret_cast<const DiskLoc*>( slice.data() + _key.objsize() )[0];
    }

    string RocksIndexEntry::asString() const {
        string s( size(), 1 );
        memcpy( const_cast<char*>( s.c_str() ), _key.objdata(), _key.objsize() );

        const char* locData = reinterpret_cast<const char*>( &_loc );
        memcpy( const_cast<char*>( s.c_str() + _key.objsize() ), locData, sizeof( DiskLoc ) );

        return s;
    }

    // RocksSortedDataImpl***********

    RocksSortedDataImpl::RocksSortedDataImpl( rocksdb::DB* db, rocksdb::ColumnFamilyHandle* cf )
        : _db( db ), _columnFamily( cf ) {
        invariant( _db );
        invariant( _columnFamily );
    }

    SortedDataBuilderInterface* RocksSortedDataImpl::getBulkBuilder(OperationContext* txn,
                                                                    bool dupsAllowed) {
        invariant( false );
    }

    Status RocksSortedDataImpl::insert(OperationContext* txn,
                                       const BSONObj& key,
                                       const DiskLoc& loc,
                                       bool dupsAllowed) {

        RocksRecoveryUnit* ru = _getRecoveryUnit( txn );

        if ( !dupsAllowed ) {
            // XXX: this is slow
            Status status = dupKeyCheck( txn, key, loc );
            if ( !status.isOK() ) {
                return status;
            }
        }

        RocksIndexEntry rIndexEntry( key, loc );
        ru->writeBatch()->Put( _columnFamily, rIndexEntry.asString(), emptyByteSlice );

        return Status::OK();
    }

    bool RocksSortedDataImpl::unindex(OperationContext* txn,
                                      const BSONObj& key,
                                      const DiskLoc& loc) {
        RocksRecoveryUnit* ru = _getRecoveryUnit( txn );

        string keyData = RocksIndexEntry( key, loc ).asString();

        string dummy;
        rocksdb::ReadOptions options = RocksEngine::readOptionsWithSnapshot( txn );
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
        RocksIndexEntry rIndexEntry( key, loc );
        string keyData = rIndexEntry.asString();
        string dummy;

        rocksdb::ReadOptions options = RocksEngine::readOptionsWithSnapshot( txn );
        rocksdb::Status s =_db->Get( options, _columnFamily, keyData, &dummy );

        return s.ok() ? Status(ErrorCodes::DuplicateKey, dupKeyError(key)) : Status::OK();
    }

    void RocksSortedDataImpl::fullValidate(OperationContext* txn, long long* numKeysOut) {
        // XXX: no key counts
        if ( numKeysOut )
            numKeysOut[0] = -1;
    }

    bool RocksSortedDataImpl::isEmpty() {
        // XXX doesn't use snapshot
        rocksdb::Iterator* it = _db->NewIterator( rocksdb::ReadOptions(), _columnFamily );

        it->SeekToFirst();
        bool toRet = it->Valid();

        delete it;

        return toRet;
    }

    Status RocksSortedDataImpl::touch(OperationContext* txn) const {
        // no-op
        return Status::OK();
    }

    SortedDataInterface::Cursor* RocksSortedDataImpl::newCursor(OperationContext* txn, 
                                                                int direction) const {
        invariant( direction == 1 || direction == -1 && "invalid value for direction" );
        rocksdb::ReadOptions options = RocksEngine::readOptionsWithSnapshot( txn );
        return new RocksCursor( _db->NewIterator( options, _columnFamily ), direction == 1 );
    }

    Status RocksSortedDataImpl::initAsEmpty(OperationContext* txn) {
        // no-op
        return Status::OK();
    }

    RocksRecoveryUnit* RocksSortedDataImpl::_getRecoveryUnit( OperationContext* opCtx ) const {
        return dynamic_cast<RocksRecoveryUnit*>( opCtx->recoveryUnit() );
    }

}
