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

        rocksdb::Slice singleByteSlice( "a" );
        rocksdb::SliceParts singleByteSliceParts( &singleByteSlice, 1 );

        struct IndexKey {
            IndexKey( const BSONObj& _obj, const DiskLoc& _loc )
                : obj( _obj ), loc( _loc ) {

                if ( obj.firstElement().fieldName()[0] ) {
                    // XXX move this to comparator
                    // need to strip
                    BSONObjBuilder b;
                    BSONObjIterator i( _obj );
                    while ( i.more() ) {
                        BSONElement e = i.next();
                        b.appendAs( e, "" );
                    }
                    obj = b.obj();
                }

                sliced[0] = rocksdb::Slice( obj.objdata(), obj.objsize() );
                sliced[1] = rocksdb::Slice( reinterpret_cast<char*>( &loc ), sizeof( DiskLoc ) );
            }

            rocksdb::SliceParts sliceParts() const {
                return rocksdb::SliceParts( sliced, 2 );
            }

            int size() const {
                return obj.objsize() + sizeof( DiskLoc );
            }

            string asString() const {
                string s( size(), 1 );
                memcpy( const_cast<char*>( s.c_str() ), sliced[0].data(), sliced[0].size() );
                memcpy( const_cast<char*>( s.c_str() + sliced[0].size() ),
                        sliced[1].data(), sliced[1].size() );
                return s;
            }

            BSONObj obj;
            DiskLoc loc;

            rocksdb::Slice sliced[2];
        };

        class RocksCursor : public SortedDataInterface::Cursor {
        public:
            RocksCursor( rocksdb::Iterator* iterator, bool direction )
                : _iterator( iterator ), _direction( direction ), _cached( false ) {

                // todo: maybe don't seek until we know we need to?
                if ( _forward() )
                    _iterator->SeekToFirst();
                else
                    _iterator->SeekToLast();
                _checkStatus();
            }

            virtual ~RocksCursor() {}

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
                IndexKey indexKey( key, loc );
                string buf = indexKey.asString();
                _iterator->Seek( buf );
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
                invariant( !"rocksdb has no advanceTo" );
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
                invariant( !"rocksdb has no customLocate" );
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
                invariant( !"rocksdb cursor doesn't do saving yet" );
            }

            void restorePosition() {
                invariant( !"rocksdb cursor doesn't do saving yet" );
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
            OperationContext* _txn; // not owned
            bool _direction;

            mutable bool _cached;
            mutable BSONObj _cachedKey;
            mutable DiskLoc _cachedLoc;
        };

    }

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
            Status status = dupKeyCheck( txn, key, loc );
            if ( !status.isOK() )
                return status;
        }

        IndexKey indexKey( key, loc );
        string buf = indexKey.asString();

        ru->writeBatch()->Put( _columnFamily,
                               indexKey.sliceParts(),
                               singleByteSliceParts );

        return Status::OK();
    }

    bool RocksBtreeImpl::unindex(OperationContext* txn,
                                 const BSONObj& key,
                                 const DiskLoc& loc) {
        RocksRecoveryUnit* ru = _getRecoveryUnit( txn );

        IndexKey indexKey( key, loc );
        string buf = indexKey.asString();

        string dummy;
        if ( !_db->KeyMayExist( rocksdb::ReadOptions(), _columnFamily, buf, &dummy ) )
            return 0;

        ru->writeBatch()->Delete( _columnFamily,
                                  buf );
        return 1; // XXX: fix? does it matter since its so slow to check?
    }

    Status RocksBtreeImpl::dupKeyCheck(OperationContext* txn, const BSONObj& key, const DiskLoc& loc) {
        // XXX: not done yet!
        return Status::OK();
    }

    void RocksBtreeImpl::fullValidate(OperationContext* txn, long long* numKeysOut) {
        // XXX: no key counts
        if ( numKeysOut )
            numKeysOut[0] = -1;
    }

    bool RocksBtreeImpl::isEmpty() {
        // XXX: todo
        return false;
    }

    Status RocksBtreeImpl::touch(OperationContext* txn) const {
        // no-op
        return Status::OK();
    }

    SortedDataInterface::Cursor* RocksBtreeImpl::newCursor(OperationContext* txn,
                                                           int direction) const {
        return new RocksCursor( _db->NewIterator( rocksdb::ReadOptions(),
                                                  _columnFamily ),
                                txn,
                                direction );
    }

    Status RocksBtreeImpl::initAsEmpty(OperationContext* txn) {
        // no-op
        return Status::OK();
    }

    RocksRecoveryUnit* RocksBtreeImpl::_getRecoveryUnit( OperationContext* opCtx ) const {
        return dynamic_cast<RocksRecoveryUnit*>( opCtx->recoveryUnit() );
    }

}
