// rocks_btree_impl.cpp

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

#include "mongo/db/storage/rocks/rocks_btree_impl.h"

#include <string>

#include <rocksdb/db.h>
#include <rocksdb/iterator.h>

#include "mongo/db/storage/rocks/rocks_engine.h"
#include "mongo/db/storage/rocks/rocks_recovery_unit.h"

namespace mongo {

    namespace {

        rocksdb::Slice emptyByteSlice( "" );
        rocksdb::SliceParts emptyByteSliceParts( &emptyByteSlice, 1 );

        class RocksCursor : public BtreeInterface::Cursor {
        public:
            // constructor that doesn't take a snapshot
            RocksCursor( rocksdb::Iterator* iterator, bool direction ):
                RocksCursor( iterator, direction, nullptr, nullptr ) { }

            // constructor that takes a snapshot
            RocksCursor( rocksdb::Iterator* iterator,
                    bool direction,
                    const rocksdb::Snapshot* snapshot,
                    rocksdb::DB* db )
                : _iterator( iterator ),
                  _direction( direction ),
                  _cached( false ),
                  _snapshot( snapshot ),
                  _db( db ) {

                invariant( ( snapshot == nullptr && db == nullptr )
                        || ( snapshot != nullptr && db != nullptr ));

                // TODO: maybe don't seek until we know we need to?
                if ( _forward() )
                    _iterator->SeekToFirst();
                else
                    _iterator->SeekToLast();
                _checkStatus();
            }

            virtual ~RocksCursor() { 
                if (_snapshot) {
                    _db->ReleaseSnapshot(_snapshot);
                }
            }

            int getDirection() const { return _direction; }

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
                _cached = false;
                string keyData = RocksIndexEntry( key, loc ).asString();
                _iterator->Seek( keyData );
                _checkStatus();
                if ( !_iterator->Valid() )
                    return false;
                _load();
                return key.woCompare( _cachedKey, BSONObj(), false ) == 0;
            }

            void advanceTo(const BSONObj &keyBegin,
                           int keyBeginLen,
                           bool afterKey,
                           const vector<const BSONElement*>& keyEnd,
                           const vector<bool>& keyEndInclusive) {
                // XXX does this work with reverse iterators?
                RocksIndexEntry ike( IndexEntryComparison::makeQueryObject (
                                         keyBegin,
                                         keyBeginLen,
                                         afterKey,
                                         keyEnd,
                                         keyEndInclusive,
                                         _forward() ),
                                     DiskLoc() );

                _iterator->Seek( ike.asString() );
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
                customLocate(keyBegin, keyBeginLen, afterVersion, keyEnd, keyEndInclusive);
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
                if ( _forward() )
                    _iterator->Next();
                else
                    _iterator->Prev();
                _cached = false;
            }

            void savePosition() {
                _savePositionObj = getKey();
                _savePositionLoc = getDiskLoc();
            }

            void restorePosition() {
                _iterator->SeekToFirst();
                _cached = false;
                invariant( locate( _savePositionObj, _savePositionLoc ) );
            }

        private:

            bool _forward() const { return _direction > 0; }

            void _checkStatus() {
                // todo: Fix me
                invariant( _iterator->status().ok() );
            }
            void _load() const {
                if ( _cached )
                    return;
                _cached = true;
                rocksdb::Slice slice = _iterator->key();
                _cachedKey = BSONObj( slice.data() );
                _cachedLoc = reinterpret_cast<const DiskLoc*>( slice.data() + _cachedKey.objsize() )[0];
            }

            scoped_ptr<rocksdb::Iterator> _iterator;
            bool _direction;

            mutable bool _cached;
            mutable BSONObj _cachedKey;
            mutable DiskLoc _cachedLoc;

            // not for caching, but rather for savePosition() and restorePosition()
            mutable BSONObj _savePositionObj;
            mutable DiskLoc _savePositionLoc;

            // we store the snapshot and database so that we can free the snapshot when we're done
            // using the cursor
            const rocksdb::Snapshot* _snapshot; // not owned
            rocksdb::DB* _db; // not owned
        };

    }

    // RocksIndexEntry
    
    RocksIndexEntry::RocksIndexEntry( const BSONObj& key, const DiskLoc loc )
        : IndexKeyEntry( key, loc ) {

        if ( _key.firstElement().fieldName()[0] ) {
            // XXX move this to comparator
            // need to strip
            BSONObjBuilder b;
            BSONObjIterator i( _key );
            while ( i.more() ) {
                BSONElement e = i.next();
                b.appendAs( e, "" );
            }
            _key = b.obj();
        }

        _sliced[0] = rocksdb::Slice( _key.objdata(), _key.objsize() );
        _sliced[1] = rocksdb::Slice( reinterpret_cast<char*>( &_loc ), sizeof( DiskLoc ) );
    }

    RocksIndexEntry::RocksIndexEntry( const rocksdb::Slice& slice )
        : IndexKeyEntry( BSONObj(), DiskLoc() ) {

        _key = BSONObj( slice.data() );
        invariant( !_key.firstElement().fieldName()[0] );

        _loc = reinterpret_cast<const DiskLoc*>( slice.data() + _key.objsize() )[0];

        _sliced[0] = rocksdb::Slice( _key.objdata(), _key.objsize() );
        _sliced[1] = rocksdb::Slice( reinterpret_cast<char*>( &_loc ), sizeof( DiskLoc ) );
    }

    string RocksIndexEntry::asString() const {
        string s( size(), 1 );
        memcpy( const_cast<char*>( s.c_str() ), _sliced[0].data(), _sliced[0].size() );
        memcpy( const_cast<char*>( s.c_str() + _sliced[0].size() ),
                _sliced[1].data(), _sliced[1].size() );
        return s;
    }

    // RocksBtreeImpl

    RocksBtreeImpl::RocksBtreeImpl( rocksdb::DB* db, rocksdb::ColumnFamilyHandle* cf )
        : _db( db ), _columnFamily( cf ) {
        invariant( _db );
        invariant( _columnFamily );
    }

    BtreeBuilderInterface* RocksBtreeImpl::getBulkBuilder(OperationContext* txn,
                                                          bool dupsAllowed) {
        invariant( false );
    }

    Status RocksBtreeImpl::insert(OperationContext* txn,
                                  const BSONObj& key,
                                  const DiskLoc& loc,
                                  bool dupsAllowed) {

        RocksRecoveryUnit* ru = _getRecoveryUnit( txn );

        if ( !dupsAllowed ) {
            // XXX: this is slow
            Status status = dupKeyCheck( key, loc );
            if ( !status.isOK() )
                return status;
        }

        RocksIndexEntry rIndexEntry( key, loc );

        ru->writeBatch()->Put( _columnFamily,
                               rIndexEntry.asString(),
                               emptyByteSlice );

        return Status::OK();
    }

    bool RocksBtreeImpl::unindex(OperationContext* txn,
                                 const BSONObj& key,
                                 const DiskLoc& loc) {
        RocksRecoveryUnit* ru = _getRecoveryUnit( txn );

        RocksIndexEntry rIndexEntry( key, loc );
        string keyData = rIndexEntry.asString();

        string dummy;
        if ( !_db->KeyMayExist( rocksdb::ReadOptions(), _columnFamily, keyData, &dummy ) )
            return 0;

        ru->writeBatch()->Delete( _columnFamily, keyData );
        return 1; // XXX: fix? does it matter since its so slow to check?
    }

    string RocksBtreeImpl::dupKeyError(const BSONObj& key) const {
        stringstream ss;
        ss << "E11000 duplicate key error ";
        // TODO figure out how to include index name without dangerous casts
        ss << "dup key: " << key.toString();
        return ss.str();
    }

    Status RocksBtreeImpl::dupKeyCheck(const BSONObj& key, const DiskLoc& loc) {
        RocksIndexEntry rIndexEntry( key, loc );
        string keyData = rIndexEntry.asString();
        string dummy;

        rocksdb::Status s =_db->Get( rocksdb::ReadOptions(), _columnFamily, keyData, &dummy );

        return s.ok() ? Status::OK() : Status(ErrorCodes::DuplicateKey, dupKeyError(key));
    }

    void RocksBtreeImpl::fullValidate(long long* numKeysOut) {
        // XXX: no key counts
        if ( numKeysOut )
            numKeysOut[0] = -1;
    }

    bool RocksBtreeImpl::isEmpty() {
        rocksdb::Iterator* it = _db->NewIterator( rocksdb::ReadOptions(), _columnFamily );

        it->SeekToFirst();
        bool toRet = it->Valid();

        delete it;

        return toRet;
    }

    Status RocksBtreeImpl::touch(OperationContext* txn) const {
        // no-op
        return Status::OK();
    }

    BtreeInterface::Cursor* RocksBtreeImpl::newCursor(int direction) const {
        rocksdb::ReadOptions options = rocksdb::ReadOptions();
        options.snapshot = _db->GetSnapshot();
        return new RocksCursor( _db->NewIterator( options, _columnFamily ),
                                                  direction,
                                                  options.snapshot,
                                                  _db );
    }

    Status RocksBtreeImpl::initAsEmpty(OperationContext* txn) {
        // no-op
        return Status::OK();
    }

    RocksRecoveryUnit* RocksBtreeImpl::_getRecoveryUnit( OperationContext* opCtx ) const {
        return dynamic_cast<RocksRecoveryUnit*>( opCtx->recoveryUnit() );
    }

}
