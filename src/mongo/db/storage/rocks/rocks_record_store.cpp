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
                                        rocksdb::ColumnFamilyHandle* columnFamily,
                                        rocksdb::ColumnFamilyHandle* metadataColumnFamily,
                                        bool isCapped,
                                        int64_t cappedMaxSize,
                                        int64_t cappedMaxDocs,
                                        CappedDocumentDeleteCallback* cappedDeleteCallback )
        : RecordStore( ns ),
          _db( db ),
          _columnFamily( columnFamily ),
          _metadataColumnFamily( metadataColumnFamily ),
          _isCapped( isCapped ),
          _cappedMaxSize( cappedMaxSize ),
          _cappedMaxDocs( cappedMaxDocs ),
          _cappedDeleteCallback( cappedDeleteCallback ),
          // Set default options (XXX should custom options be persistent?)
          _defaultReadOptions( ) {
        invariant( _db );
        invariant( _columnFamily );
        invariant( _metadataColumnFamily );

        if (_isCapped) {
            invariant(_cappedMaxSize > 0);
            invariant(_cappedMaxDocs == -1 || _cappedMaxDocs > 0);
        }
        else {
            invariant(_cappedMaxSize == -1);
            invariant(_cappedMaxDocs == -1);
        }

        // Get next id
        // XXX not using a Snapshot here
        boost::scoped_ptr<rocksdb::Iterator> iter( db->NewIterator( _readOptions(),
                                                                    columnFamily ) );
        iter->SeekToLast();
        if (iter->Valid()) {
            rocksdb::Slice lastSlice = iter->key();
            DiskLoc lastLoc = reinterpret_cast<DiskLoc*>( const_cast<char*>( 
                                                           lastSlice.data() ) )[0];
            _nextIdNum = lastLoc.a() + ( (uint64_t) lastLoc.getOfs() << 32 ) + 1;
        }
        else
            _nextIdNum = 1;

        // load metadata
        std::string value;
        // XXX not using a Snapshot here
        if (!_db->Get( _readOptions(),
                       _metadataColumnFamily,
                       rocksdb::Slice("numRecords"),
                       &value ).ok()) {
            _numRecords = 0;
        }
        else
            _numRecords = *((long long *) value.data());
        // XXX not using a Snapshot here
        if (!_db->Get( _readOptions(),
                       _metadataColumnFamily,
                       rocksdb::Slice("dataSize"),
                       &value ).ok()) {
            _dataSize = 0;
        }
        else
            _dataSize = *((long long *) value.data());
    }

    RocksRecordStore::~RocksRecordStore() { }

    long long RocksRecordStore::dataSize() const {
        return _dataSize;
    }

    long long RocksRecordStore::numRecords() const {
        return _numRecords;
    }

    bool RocksRecordStore::isCapped() const {
        return _isCapped;
    }

    int64_t RocksRecordStore::storageSize( OperationContext* txn, 
                                           BSONObjBuilder* extraInfo, 
                                           int infoLevel ) const {
        return dataSize(); // todo: this isn't very good
    }

    RecordData RocksRecordStore::dataFor( const DiskLoc& loc) const {
        // ownership passes to the shared_array created below
        std::string* value = new std::string();

        // XXX not using a Snapshot here
        rocksdb::Status status = _db->Get( _readOptions(), _columnFamily, _makeKey( loc ), value );

        if ( !status.ok() ) {
            log() << "rocks Get failed, blowing up: " << status.ToString();
            delete value;
            invariant( false );
        }

        boost::shared_array<char> data( reinterpret_cast<char*>( value ) );

        return RecordData( value->data(), value->size(), data );
    }

    void RocksRecordStore::deleteRecord( OperationContext* txn, const DiskLoc& dl ) {
        RocksRecoveryUnit* ru = _getRecoveryUnit( txn );

        std::string old_value;
        _db->Get( _readOptions( txn ), _columnFamily, _makeKey( dl ), &old_value );
        int old_length = old_value.size();

        ru->writeBatch()->Delete( _columnFamily,
                                  _makeKey( dl ) );

        _changeNumRecords(txn, false);
        _increaseDataSize(txn, -old_length);
    }

    bool RocksRecordStore::cappedAndNeedDelete() const {
        if (!_isCapped)
            return false;

        if (_dataSize > _cappedMaxSize)
            return true;

        if ((_cappedMaxDocs != -1) && (numRecords() > _cappedMaxDocs))
            return true;

        return false;
    }

    void RocksRecordStore::cappedDeleteAsNeeded(OperationContext* txn) {
        // This persistent iterator is necessary since you can't read your own writes
        boost::scoped_ptr<rocksdb::Iterator> iter( _db->NewIterator( rocksdb::ReadOptions(),
                                                   _columnFamily ) );
        iter->SeekToFirst();
        while ( cappedAndNeedDelete() ) {
            invariant(_numRecords > 0);

            invariant ( iter->Valid() );
            rocksdb::Slice slice = iter->key();
            DiskLoc oldest = reinterpret_cast<const DiskLoc*>( slice.data() )[0];

            if ( _cappedDeleteCallback )
                uassertStatusOK(_cappedDeleteCallback->aboutToDeleteCapped(txn, oldest));

            deleteRecord(txn, oldest);
            iter->Next();
        }
    }

    StatusWith<DiskLoc> RocksRecordStore::insertRecord( OperationContext* txn,
                                                        const char* data,
                                                        int len,
                                                        bool enforceQuota ) {
        if ( _isCapped && len > _cappedMaxSize ) {
            return StatusWith<DiskLoc>( ErrorCodes::BadValue,
                                       "object to insert exceeds cappedMaxSize" );
        }

        RocksRecoveryUnit* ru = _getRecoveryUnit( txn );

        DiskLoc loc = _nextId();

        ru->writeBatch()->Put( _columnFamily, _makeKey( loc ), rocksdb::Slice( data, len ) );

        _changeNumRecords( txn, true );
        _increaseDataSize( txn, len );

        cappedDeleteAsNeeded(txn);

        return StatusWith<DiskLoc>( loc );
    }

    StatusWith<DiskLoc> RocksRecordStore::insertRecord( OperationContext* txn,
                                                        const DocWriter* doc,
                                                        bool enforceQuota ) {
        const int len = doc->documentSize();

        if ( _isCapped && len > _cappedMaxSize ) {
            return StatusWith<DiskLoc>( ErrorCodes::BadValue,
                                       "object to insert exceeds cappedMaxSize" );
        }

        boost::shared_array<char> buf( new char[len] );
        doc->writeDocument( buf.get() );

        RocksRecoveryUnit* ru = _getRecoveryUnit( txn );

        DiskLoc loc = _nextId();

        ru->writeBatch()->Put( _columnFamily, _makeKey( loc ), rocksdb::Slice( buf.get(), len ) );

        _changeNumRecords( txn, true );
        _increaseDataSize( txn, len );

        cappedDeleteAsNeeded( txn );

        return StatusWith<DiskLoc>( loc );
    }

    StatusWith<DiskLoc> RocksRecordStore::updateRecord( OperationContext* txn,
                                                        const DiskLoc& loc,
                                                        const char* data,
                                                        int len,
                                                        bool enforceQuota,
                                                        UpdateMoveNotifier* notifier ) {
        RocksRecoveryUnit* ru = _getRecoveryUnit( txn );

        std::string old_value;
        rocksdb::Status status = _db->Get( _readOptions( txn ),
                                           _columnFamily,
                                           _makeKey( loc ),
                                           &old_value );

        if ( !status.ok() ) {
            log() << "rocks Get failed, blowing up: " << status.ToString();
            invariant( false );
        }

        int old_length = old_value.size();

        ru->writeBatch()->Put( _columnFamily, _makeKey( loc ), rocksdb::Slice( data, len ) );

        _increaseDataSize(txn, len - old_length);

        cappedDeleteAsNeeded(txn);

        return StatusWith<DiskLoc>( loc );
    }

    Status RocksRecordStore::updateWithDamages( OperationContext* txn,
                                                const DiskLoc& loc,
                                                const char* damangeSource,
                                                const mutablebson::DamageVector& damages ) {
        RocksRecoveryUnit* ru = _getRecoveryUnit( txn );

        rocksdb::Slice key = _makeKey( loc );

        // get original value
        std::string value;
        rocksdb::Status status;
        status = _db->Get( _readOptions( txn ), _columnFamily, key, &value );

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
        ru->writeBatch()->Put( _columnFamily, key, value );

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


    RecordIterator* RocksRecordStore::getIteratorForRepair( OperationContext* txn ) const {
        return getIterator( txn );
    }

    std::vector<RecordIterator*> RocksRecordStore::getManyIterators( OperationContext* txn ) const {
        // XXX do we want this to actually return a set of iterators?

        std::vector<RecordIterator*> iterators;
        iterators.push_back( getIterator( txn ) );
        return iterators;
    }

    Status RocksRecordStore::truncate( OperationContext* txn ) {
        boost::scoped_ptr<RecordIterator> iter( getIterator( txn ) );
        while( !iter->isEOF() ) {
            DiskLoc loc = iter->getNext();
            deleteRecord( txn, loc );
        }
        // also clear current write batch
        RocksRecoveryUnit* ru = _getRecoveryUnit( txn );
        rocksdb::WriteBatch* wb = ru->writeBatch();
        if ( wb != NULL )
            wb->Clear();

        return Status::OK();
    }

    Status RocksRecordStore::compact( OperationContext* txn,
                                      RecordStoreCompactAdaptor* adaptor,
                                      const CompactOptions* options,
                                      CompactStats* stats ) {
        rocksdb::Status status = _db->CompactRange( _columnFamily, nullptr, nullptr );
        invariant ( status.ok() );
        return Status::OK();
    }

    Status RocksRecordStore::validate( OperationContext* txn,
                                       bool full, bool scanData,
                                       ValidateAdaptor* adaptor,
                                       ValidateResults* results,
                                       BSONObjBuilder* output ) const {
        RecordIterator* iter = getIterator( txn );
        while( !iter->isEOF() ) {
            RecordData data = dataFor( iter->curr() );
            if ( scanData ) {
                BSONObj b( data.data() );
                if ( !b.valid() ) {
                    DiskLoc l = iter->curr();
                    results->errors.push_back( l.toString() + " is corrupted" );
                }
            }
            iter->getNext();
        }
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


    // AFB: is there a way to force column families to be cached in rocks?
    Status RocksRecordStore::touch( OperationContext* txn, BSONObjBuilder* output ) const {
        return Status::OK();
    }

    Status RocksRecordStore::setCustomOption( OperationContext* txn,
                                              const BSONElement& option,
                                              BSONObjBuilder* info ) {
        // XXX can't do write options here as writes are dont in a write batch which is held in
        // a recovery unit
        string optionName = option.fieldName();
        if ( !option.isBoolean() ) {
            return Status( ErrorCodes::BadValue, "Invalid Value" );
        }
        if ( optionName.compare( "verify_checksums" ) == 0 ) {
            _defaultReadOptions.verify_checksums = option.boolean();
        }
        else if ( optionName.compare( "fill_cache" ) == 0 ) {
            _defaultReadOptions.fill_cache = option.boolean();
        }
        else if ( optionName.compare("tailing") == 0 ) {
            _defaultReadOptions.tailing = option.boolean();
        }
        else
            return Status( ErrorCodes::BadValue, "Invalid Option" );

        return Status::OK();
    }

    DiskLoc RocksRecordStore::_nextId() {
        boost::mutex::scoped_lock lk( _idLock );
        int ofs = _nextIdNum >> 32;
        int a = (_nextIdNum << 32 ) >> 32;
        DiskLoc loc( a, ofs );
        _nextIdNum++;
        return loc;
    }

    rocksdb::Slice RocksRecordStore::_makeKey( const DiskLoc& loc ) const {
        return rocksdb::Slice( reinterpret_cast<const char*>( &loc ), sizeof( loc ) );
    }

    RocksRecoveryUnit* RocksRecordStore::_getRecoveryUnit( OperationContext* opCtx ) {
        return dynamic_cast<RocksRecoveryUnit*>( opCtx->recoveryUnit() );
    }

    // XXX make sure these work with rollbacks (I don't think they will)
    void RocksRecordStore::_changeNumRecords( OperationContext* txn, bool insert ) {
        if ( insert ) {
            _numRecords++;
        }
        else {
            _numRecords--;
        }
        RocksRecoveryUnit* ru = _getRecoveryUnit( txn );
        char* nr_ptr = reinterpret_cast<char*>( &_numRecords );
        std::string nr_key_string = "numRecords";

        ru->writeBatch()->Put( _metadataColumnFamily,
                               rocksdb::Slice( nr_key_string ),
                               rocksdb::Slice( nr_ptr, sizeof(long long) ) );
    }


    void RocksRecordStore::_increaseDataSize( OperationContext* txn, int amount ) {
        _dataSize += amount;
        RocksRecoveryUnit* ru = _getRecoveryUnit( txn );
        char* ds_ptr = reinterpret_cast<char*>( &_dataSize );
        std::string ds_key_string = "dataSize";

        ru->writeBatch()->Put( _metadataColumnFamily,
                               rocksdb::Slice( ds_key_string ),
                               rocksdb::Slice( ds_ptr, sizeof(long long) ) );
    }

    // --------

    RocksRecordStore::Iterator::Iterator( const RocksRecordStore* rs,
                                          const CollectionScanParams::Direction& dir )
        : _rs( rs ),
          _dir( dir ),
          // XXX not using a snapshot here
          _iterator( _rs->_db->NewIterator( rs->_readOptions(),
                                            rs->_columnFamily ) ) {
        if ( _forward() )
            _iterator->SeekToFirst();
        else
            _iterator->SeekToLast();
        _checkStatus();
    }

    void RocksRecordStore::Iterator::_checkStatus() {
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

    void RocksRecordStore::Iterator::invalidate( const DiskLoc& dl ) {
        _iterator.reset( NULL );
    }

    void RocksRecordStore::Iterator::prepareToYield() {
        // TODO delete iterator, store information
    }

    bool RocksRecordStore::Iterator::recoverFromYield() {
        // TODO set iterator to same place as before, but with new snapshot
        return true;
    }

    RecordData RocksRecordStore::Iterator::dataFor( const DiskLoc& loc ) const {
        if (_iterator->Valid() && 
                *reinterpret_cast<const DiskLoc*>( _iterator->key().data() ) == loc) {
            rocksdb::Slice data_slice = _iterator->value();

            boost::shared_array<char> data( new char[data_slice.size()] );
            memcpy( data.get(), data_slice.data(), data_slice.size() );

            return RecordData( data.get(), data_slice.size(), data );
        }

        return _rs->dataFor( loc );
    }

    bool RocksRecordStore::Iterator::_forward() const {
        return _dir == CollectionScanParams::FORWARD;
    }

    void RocksRecordStore::temp_cappedTruncateAfter( OperationContext* txn,
                                                     DiskLoc end,
                                                     bool inclusive ) {
        boost::scoped_ptr<RecordIterator> iter( getIterator( txn ) );
        if ( iter->isEOF() )
            return;
        while( !iter->isEOF() ) {
            DiskLoc loc = iter->getNext();
            if ( end < loc || ( inclusive && end == loc ) )
                deleteRecord( txn, loc );
        }
    }

    rocksdb::ReadOptions RocksRecordStore::_readOptions( OperationContext* opCtx ) const {
        rocksdb::ReadOptions options( _defaultReadOptions );
        if ( opCtx ) {
            options.snapshot = _getRecoveryUnit( opCtx )->snapshot();
        }
        return options;
    }
}
