// rocks_record_store.cpp

/**
 *    Copyright (C) 2014 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
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

#include "mongo/db/operation_context.h"
#include "mongo/db/storage/rocks/rocks_record_store.h"
#include "mongo/db/storage/rocks/rocks_recovery_unit.h"

#include <rocksdb/db.h>
#include <rocksdb/slice.h>
#include <rocksdb/options.h>

namespace mongo {

    RocksRecordStore::RocksRecordStore( const StringData& ns,
                                        rocksdb::DB* db, // not owned here
                                        rocksdb::ColumnFamilyHandle* columnFamily )
        : RecordStore( ns ),
          _db( db ),
          _columnFamily( columnFamily ) {
        invariant( _db );
        invariant( _columnFamily );
        _tempIncrement = 1;
    }

    RocksRecordStore::~RocksRecordStore() {
    }

    long long RocksRecordStore::dataSize() const {
        // TODO(XXX)
        return -1;
    }

    long long RocksRecordStore::numRecords() const {
        // TODO(XXX)
        return -1;
    }

    bool RocksRecordStore::isCapped() const {
        return false;
    }

    int64_t RocksRecordStore::storageSize( BSONObjBuilder* extraInfo, int infoLevel ) const {
        return dataSize(); // todo: this isn't very good
    }

    RecordData RocksRecordStore::dataFor( const DiskLoc& loc) const {
        std::string value;
        rocksdb::Status status;
        status = _db->Get( rocksdb::ReadOptions(),
                           _columnFamily,
                           _makeKey( loc ),
                           &value );

        if ( !status.ok() ) {
            if ( status.IsNotFound() )
                return RecordData( NULL, 0 );

            log() << "rocks Get failed, blowing up: " << status.ToString();
            invariant( false );
        }

        // TODO(XXX) this is a double copy :(

        boost::shared_array<char> data( new char[value.size()] );
        memcpy( data.get(), value.c_str(), value.size() );

        return RecordData( data.get(), value.size(), data );
    }

    void RocksRecordStore::deleteRecord( OperationContext* txn, const DiskLoc& dl ) {
        RocksRecoveryUnit* ru = _getRecoveryUnit( txn );
        ru->writeBatch()->Delete( _columnFamily,
                                  _makeKey( dl ) );
    }

    StatusWith<DiskLoc> RocksRecordStore::insertRecord( OperationContext* txn,
                                                        const char* data,
                                                        int len,
                                                        bool enforceQuota ) {

        RocksRecoveryUnit* ru = _getRecoveryUnit( txn );

        DiskLoc loc = _nextId();

        ru->writeBatch()->Put( _columnFamily,
                               _makeKey( loc ),
                               rocksdb::Slice( data, len ) );

        return StatusWith<DiskLoc>( loc );
    }

    StatusWith<DiskLoc> RocksRecordStore::insertRecord( OperationContext* txn,
                                                        const DocWriter* doc,
                                                        bool enforceQuota ) {
        invariant( false );
    }

    StatusWith<DiskLoc> RocksRecordStore::updateRecord( OperationContext* txn,
                                                        const DiskLoc& loc,
                                                        const char* data,
                                                        int len,
                                                        bool enforceQuota,
                                                        UpdateMoveNotifier* notifier ) {
        RocksRecoveryUnit* ru = _getRecoveryUnit( txn );

        ru->writeBatch()->Put( _columnFamily,
                               _makeKey( loc ),
                               rocksdb::Slice( data, len ) );

        return StatusWith<DiskLoc>( loc );
    }

    Status RocksRecordStore::updateWithDamages( OperationContext* txn,
                                                const DiskLoc& loc,
                                                const char* damangeSource,
                                                const mutablebson::DamageVector& damages ) {
        // todo: this should use the merge functionality in rocks

        rocksdb::Slice key = _makeKey( loc );

        // get original value
        std::string value;
        rocksdb::Status status;
        status = _db->Get( rocksdb::ReadOptions(),
                           _columnFamily,
                           key,
                           &value );

        if ( !status.ok() ) {
            if ( status.IsNotFound() )
                return Status( ErrorCodes::InternalError, "doc not found for update in place" );

            log() << "rocks Get failed, blowing up: " << status.ToString();
            invariant( false );
        }

        // apply changes to our copy
        char* root = const_cast<char*>( value.c_str() );
        for( size_t i = 0; i < damages.size(); i++ ) {
            mutablebson::DamageEvent event = damages[i];
            const char* sourcePtr = damangeSource + event.sourceOffset;
            char* targetPtr = root + event.targetOffset;
            std::memcpy(targetPtr, sourcePtr, event.size);
        }

        // write back
        status = _db->Put( rocksdb::WriteOptions(),
                           _columnFamily,
                           key,
                           value );
        invariant( status.ok() );
        return Status::OK();
    }

    RecordIterator* RocksRecordStore::getIterator( OperationContext* txn,
                                                   const DiskLoc& start,
                                                   bool tailable,
                                                   const CollectionScanParams::Direction& dir
                                                   ) const {
        invariant( start == DiskLoc() );
        invariant( !tailable );

        return new Iterator( this, dir );
    }


    RecordIterator* RocksRecordStore::getIteratorForRepair(OperationContext* txn) const {
        return getIterator(txn);
    }

    std::vector<RecordIterator*> RocksRecordStore::getManyIterators(OperationContext* txn) const {
        invariant( false );
    }

    Status RocksRecordStore::truncate( OperationContext* txn ) {
        return Status( ErrorCodes::InternalError, "RocksRecordStore::truncate not supported yet" );
    }

    Status RocksRecordStore::compact( OperationContext* txn,
                                      RecordStoreCompactAdaptor* adaptor,
                                      const CompactOptions* options,
                                      CompactStats* stats ) {
        return Status(ErrorCodes::InternalError, "no compact support in RocksRecordStore" );
    }

    Status RocksRecordStore::validate( OperationContext* txn,
                                       bool full, bool scanData,
                                       ValidateAdaptor* adaptor,
                                       ValidateResults* results, BSONObjBuilder* output ) const {
        output->append( "rocks", "no validate yet" );
        return Status::OK();
    }

    void RocksRecordStore::appendCustomStats( OperationContext* txn,
                                              BSONObjBuilder* result,
                                              double scale ) const {
        string statsString;
        bool valid = _db->GetProperty( _columnFamily, "rocksdb.stats", &statsString );
        invariant( valid );
        result->append( "stats", statsString );
    }

    Status RocksRecordStore::touch( OperationContext* txn, BSONObjBuilder* output ) const {
        // should we do something for this?
        return Status::OK();
    }

    Status RocksRecordStore::setCustomOption( OperationContext* txn,
                                              const BSONElement& option,
                                              BSONObjBuilder* info ) {
        return Status( ErrorCodes::BadValue, "no custom option for RocksRecordStore" );
    }

    DiskLoc RocksRecordStore::_nextId() {
        return DiskLoc( 1, _tempIncrement++ );
    }

    rocksdb::Slice RocksRecordStore::_makeKey( const DiskLoc& loc ) const {
        return rocksdb::Slice( reinterpret_cast<const char*>( &loc ), sizeof( loc ) );
    }

    RocksRecoveryUnit* RocksRecordStore::_getRecoveryUnit( OperationContext* opCtx ) const {
        return dynamic_cast<RocksRecoveryUnit*>( opCtx->recoveryUnit() );
    }

    // --------

    RocksRecordStore::Iterator::Iterator( const RocksRecordStore* rs,
                                          const CollectionScanParams::Direction& dir )
        : _rs( rs ),
          _dir( dir ),
          _iterator( _rs->_db->NewIterator( rocksdb::ReadOptions(),
                                            rs->_columnFamily ) ) {
        if ( _forward() )
            _iterator->SeekToFirst();
        else
            _iterator->SeekToLast();
        _checkStatus();
    }

    void RocksRecordStore::Iterator::_checkStatus() {
        // todo: Fix me
        invariant( _iterator->status().ok() );
    }

    bool RocksRecordStore::Iterator::isEOF() {
        return !_iterator || !_iterator->Valid();
    }

    DiskLoc RocksRecordStore::Iterator::curr() {
        if ( !_iterator->Valid() )
            return DiskLoc();

        rocksdb::Slice slice = _iterator->key();
        return reinterpret_cast<const DiskLoc*>( slice.data() )[0];
    }

    DiskLoc RocksRecordStore::Iterator::getNext() {
        DiskLoc toReturn = curr();

        if ( _forward() )
            _iterator->Next();
        else
            _iterator->Prev();

        return toReturn;
    }

    void RocksRecordStore::Iterator::invalidate(const DiskLoc& dl) {
        _iterator.reset( NULL );
    }

    void RocksRecordStore::Iterator::prepareToYield() {
        // XXX: ???
    }

    bool RocksRecordStore::Iterator::recoverFromYield() {
        // XXX: ???
        return true;
    }

    RecordData RocksRecordStore::Iterator::dataFor( const DiskLoc& loc ) const {
        // XXX: use the iterator value
        return _rs->dataFor( loc );
    }

    bool RocksRecordStore::Iterator::_forward() const {
        return _dir == CollectionScanParams::FORWARD;
    }

    void RocksRecordStore::temp_cappedTruncateAfter(OperationContext* txn,
                                                    DiskLoc end,
                                                    bool inclusive) {
        invariant( !"no temp_cappedTruncateAfter with rocks" );
    }

}
