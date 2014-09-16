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

#include <rocksdb/comparator.h>
#include <rocksdb/db.h>
#include <rocksdb/options.h>
#include <rocksdb/slice.h>

#include "mongo/util/log.h"

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
          _dataSizeKey( ns.toString() + "-dataSize" ),
          _numRecordsKey( ns.toString() + "-numRecords" ) {
        invariant( _db );
        invariant( _columnFamily );
        invariant( _metadataColumnFamily );
        invariant( _columnFamily != _metadataColumnFamily );

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
            DiskLoc lastLoc = _makeDiskLoc( lastSlice );
            _nextIdNum.store( lastLoc.getOfs() + ( uint64_t( lastLoc.a() ) << 32 ) + 1) ;
        }
        else {
            // Need to start at 1 so we are always higher than minDiskLoc
            _nextIdNum.store( 1 );
        }

        // load metadata
        std::string value;
        bool metadataPresent = true;
        // XXX not using a Snapshot here
        if (!_db->Get( _readOptions(),
                       _metadataColumnFamily,
                       rocksdb::Slice( _numRecordsKey ),
                       &value ).ok()) {
            _numRecords = 0;
            metadataPresent = false;
        }
        else {
            memcpy( &_numRecords, value.data(), sizeof( _numRecords ));
        }

        // XXX not using a Snapshot here
        if (!_db->Get( _readOptions(),
                       _metadataColumnFamily,
                       rocksdb::Slice( _dataSizeKey ),
                       &value ).ok()) {
            _dataSize = 0;
            invariant(!metadataPresent);
        }
        else {
            memcpy( &_dataSize, value.data(), sizeof( _dataSize ));
            invariant( _dataSize >= 0 );
        }
    }

    int64_t RocksRecordStore::storageSize( OperationContext* txn,
                                           BSONObjBuilder* extraInfo,
                                           int infoLevel ) const {
        uint64_t storageSize;
        rocksdb::Range wholeRange( _makeKey( minDiskLoc ), _makeKey( maxDiskLoc ) );
        _db->GetApproximateSizes( _columnFamily, &wholeRange, 1, &storageSize);
        return static_cast<int64_t>( storageSize );
    }

    RecordData RocksRecordStore::dataFor( OperationContext* txn, const DiskLoc& loc) const {
        // TODO investigate using cursor API to get a Slice and avoid double copying.
        std::string value;

        // XXX not using a Snapshot here
        rocksdb::Status status = _db->Get( _readOptions(), _columnFamily, _makeKey( loc ), &value );

        if ( !status.ok() ) {
            log() << "rocks Get failed, blowing up: " << status.ToString();
            invariant( false );
        }

        boost::shared_array<char> data( new char[value.size()] );
        memcpy( data.get(), value.data(), value.size() );

        return RecordData( data.get(), value.size(), data );
    }

    void RocksRecordStore::deleteRecord( OperationContext* txn, const DiskLoc& dl ) {
        RocksRecoveryUnit* ru = _getRecoveryUnit( txn );

        std::string oldValue;
        _db->Get( _readOptions( txn ), _columnFamily, _makeKey( dl ), &oldValue );
        int oldLength = oldValue.size();

        ru->writeBatch()->Delete( _columnFamily, _makeKey( dl ) );

        _changeNumRecords(txn, false);
        _increaseDataSize(txn, -oldLength);
    }

    bool RocksRecordStore::cappedAndNeedDelete() const {
        if (!_isCapped)
            return false;

        if (_dataSize > _cappedMaxSize)
            return true;

        if ((_cappedMaxDocs != -1) && (_numRecords > _cappedMaxDocs))
            return true;

        return false;
    }

    void RocksRecordStore::cappedDeleteAsNeeded(OperationContext* txn) {
        if (!cappedAndNeedDelete())
            return;
        // This persistent iterator is necessary since you can't read your own writes
        boost::scoped_ptr<rocksdb::Iterator> iter( _db->NewIterator( _readOptions( txn ),
                                                                     _columnFamily ) );
        iter->SeekToFirst();

        // XXX TODO there is a bug here where if the size of the write batch exceeds the cap size
        // then iter will not be valid and it will crash. To fix this we need the ability to
        // query the write batch, and delete the oldest record in the write batch until the
        // size of the write batch is less than the cap

        // XXX PROBLEMS
        // 2 threads could delete the same document
        // multiple inserts using the same snapshot will delete the same document
        while ( cappedAndNeedDelete() && iter->Valid() ) {
            invariant(_numRecords > 0);

            rocksdb::Slice slice = iter->key();
            DiskLoc oldest = _makeDiskLoc( slice );

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
        boost::scoped_array<char> buf( new char[len] );
        doc->writeDocument( buf.get() );

        return insertRecord( txn, buf.get(), len, enforceQuota );
    }

    StatusWith<DiskLoc> RocksRecordStore::updateRecord( OperationContext* txn,
                                                        const DiskLoc& loc,
                                                        const char* data,
                                                        int len,
                                                        bool enforceQuota,
                                                        UpdateMoveNotifier* notifier ) {
        RocksRecoveryUnit* ru = _getRecoveryUnit( txn );

        std::string old_value;
        // XXX Be sure to also first query the write batch once Facebook implements that
        rocksdb::Status status = _db->Get( _readOptions( txn ),
                                           _columnFamily,
                                           _makeKey( loc ),
                                           &old_value );

        if ( !status.ok() ) {
            return StatusWith<DiskLoc>( ErrorCodes::InternalError, status.ToString() );
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
                return Status( ErrorCodes::InternalError, "doc not found for in-place update" );

            log() << "rocks Get failed, blowing up: " << status.ToString();
            invariant( false );
        }

        // apply changes to our copy
        for( size_t i = 0; i < damages.size(); i++ ) {
            mutablebson::DamageEvent event = damages[i];
            const char* sourcePtr = damangeSource + event.sourceOffset;

            invariant( event.targetOffset + event.size < value.length() );
            value.replace( event.targetOffset, event.size, sourcePtr, event.size );
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
        invariant( !tailable );

        return new Iterator( txn, this, dir, start );
    }


    RecordIterator* RocksRecordStore::getIteratorForRepair( OperationContext* txn ) const {
        return getIterator( txn );
    }

    std::vector<RecordIterator*> RocksRecordStore::getManyIterators( OperationContext* txn ) const {
        // AFB: any way to get the split point keys for the bottom layer of the lsm tree?
        std::vector<RecordIterator*> iterators;
        iterators.push_back( getIterator( txn ) );
        return iterators;
    }

    Status RocksRecordStore::truncate( OperationContext* txn ) {
        // XXX once we have readable WriteBatch, also delete outstanding writes to
        // this collection in the WriteBatch
        //AFB add Clear(ColumnFamilyHandle*)
        boost::scoped_ptr<RecordIterator> iter( getIterator( txn ) );
        while( !iter->isEOF() ) {
            DiskLoc loc = iter->getNext();
            deleteRecord( txn, loc );
        }

        return Status::OK();
    }

    Status RocksRecordStore::compact( OperationContext* txn,
                                      RecordStoreCompactAdaptor* adaptor,
                                      const CompactOptions* options,
                                      CompactStats* stats ) {
        rocksdb::Status status = _db->CompactRange( _columnFamily, NULL, NULL );
        if ( status.ok() )
            return Status::OK();
        else
            return Status( ErrorCodes::InternalError, status.ToString() );
    }

    Status RocksRecordStore::validate( OperationContext* txn,
                                       bool full,
                                       bool scanData,
                                       ValidateAdaptor* adaptor,
                                       ValidateResults* results,
                                       BSONObjBuilder* output ) const {
        // TODO validate that _numRecords and _dataSize are correct in scanData mode
        if ( scanData ) {
            bool invalidObject = false;
            size_t numRecords = 0;
            boost::scoped_ptr<RecordIterator> iter( getIterator( txn ) );
            while( !iter->isEOF() ) {
                numRecords++;
                RecordData data = dataFor( txn, iter->curr() );
                size_t dataSize;
                const Status status = adaptor->validate( data, &dataSize );
                if (!status.isOK()) {
                    results->valid = false;
                    if ( invalidObject ) {
                        results->errors.push_back("invalid object detected (see logs)");
                    }
                    invalidObject = true;
                    log() << "Invalid object detected in " << _ns << ": " << status.reason();
                }
                iter->getNext();
            }
            output->appendNumber("nrecords", numRecords);
        }
        else
            output->appendNumber("nrecords", _numRecords);

        return Status::OK();
    }

    void RocksRecordStore::appendCustomStats( OperationContext* txn,
                                              BSONObjBuilder* result,
                                              double scale ) const {
        string statsString;
        result->appendBool("capped", _isCapped);
        if (_isCapped) {
            result->appendIntOrLL("max", _cappedMaxDocs);
            result->appendIntOrLL("maxSize", _cappedMaxSize);
        }
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
        string optionName = option.fieldName();
        if ( optionName == "usePowerOf2Sizes" ) {
            return Status::OK();
        }

        return Status( ErrorCodes::BadValue, "Invalid option: " + optionName );
    }

    namespace {
        class RocksCollectionComparator : public rocksdb::Comparator {
            public:
                RocksCollectionComparator() { }
                virtual ~RocksCollectionComparator() { }

                virtual int Compare( const rocksdb::Slice& a, const rocksdb::Slice& b ) const {
                    DiskLoc lhs = reinterpret_cast<const DiskLoc*>( a.data() )[0];
                    DiskLoc rhs = reinterpret_cast<const DiskLoc*>( b.data() )[0];
                    return lhs.compare( rhs );
                }

                virtual const char* Name() const {
                    return "mongodb.RocksCollectionComparator";
                }

                /**
                 * From the RocksDB comments: "an implementation of this method that does nothing is
                 * correct"
                 */
                virtual void FindShortestSeparator( std::string* start,
                        const rocksdb::Slice& limit) const { }

                /**
                 * From the RocksDB comments: "an implementation of this method that does nothing is
                 * correct.
                 */
                virtual void FindShortSuccessor(std::string* key) const { }
        };
    }

    rocksdb::Comparator* RocksRecordStore::newRocksCollectionComparator() {
        return new RocksCollectionComparator();
    }

    void RocksRecordStore::temp_cappedTruncateAfter( OperationContext* txn,
                                                     DiskLoc end,
                                                     bool inclusive ) {
        boost::scoped_ptr<RecordIterator> iter(
                getIterator( txn, maxDiskLoc, false, CollectionScanParams::BACKWARD ) );

        while( !iter->isEOF() ) {
            WriteUnitOfWork wu( txn );
            DiskLoc loc = iter->getNext();
            if ( loc < end || ( !inclusive && loc == end))
                return;

            deleteRecord( txn, loc );
            wu.commit();
        }
    }

    void RocksRecordStore::dropRsMetaData( OperationContext* opCtx ) {
        RocksRecoveryUnit* ru = _getRecoveryUnit( opCtx );

        boost::mutex::scoped_lock dataSizeLk( _dataSizeLock );
        ru->writeBatch()->Delete( _metadataColumnFamily, _dataSizeKey );

        boost::mutex::scoped_lock numRecordsLk( _numRecordsLock );
        ru->writeBatch()->Delete( _metadataColumnFamily, _numRecordsKey );
    }

    rocksdb::ReadOptions RocksRecordStore::_readOptions( OperationContext* opCtx ) const {
        rocksdb::ReadOptions options;
        if ( opCtx ) {
            options.snapshot = _getRecoveryUnit( opCtx )->snapshot();
        }
        return options;
    }

    DiskLoc RocksRecordStore::_nextId() {
        const uint64_t myId = _nextIdNum.fetchAndAdd(1);
        int a = myId >> 32;
        // This masks the lowest 4 bytes of myId
        int ofs = myId & 0x00000000FFFFFFFF;
        DiskLoc loc( a, ofs );
        return loc;
    }

    rocksdb::Slice RocksRecordStore::_makeKey( const DiskLoc& loc ) {
        return rocksdb::Slice( reinterpret_cast<const char*>( &loc ), sizeof( loc ) );
    }

    RocksRecoveryUnit* RocksRecordStore::_getRecoveryUnit( OperationContext* opCtx ) {
        return dynamic_cast<RocksRecoveryUnit*>( opCtx->recoveryUnit() );
    }

    DiskLoc RocksRecordStore::_makeDiskLoc( const rocksdb::Slice& slice ) {
        return reinterpret_cast<const DiskLoc*>( slice.data() )[0];
    }

    // XXX make sure these work with rollbacks (I don't think they will)
    void RocksRecordStore::_changeNumRecords( OperationContext* txn, bool insert ) {
        boost::mutex::scoped_lock lk( _numRecordsLock );

        if ( insert ) {
            _numRecords++;
        }
        else {
            _numRecords--;
        }
        RocksRecoveryUnit* ru = _getRecoveryUnit( txn );
        const char* nr_ptr = reinterpret_cast<char*>( &_numRecords );

        ru->writeBatch()->Put( _metadataColumnFamily,
                               rocksdb::Slice( _numRecordsKey ),
                               rocksdb::Slice( nr_ptr, sizeof(long long) ) );
    }


    void RocksRecordStore::_increaseDataSize( OperationContext* txn, int amount ) {
        boost::mutex::scoped_lock lk( _dataSizeLock );

        _dataSize += amount;
        RocksRecoveryUnit* ru = _getRecoveryUnit( txn );
        const char* ds_ptr = reinterpret_cast<char*>( &_dataSize );

        ru->writeBatch()->Put( _metadataColumnFamily,
                               rocksdb::Slice( _dataSizeKey ),
                               rocksdb::Slice( ds_ptr, sizeof(long long) ) );
    }

    // --------

    RocksRecordStore::Iterator::Iterator( OperationContext* txn,
                                          const RocksRecordStore* rs,
                                          const CollectionScanParams::Direction& dir,
                                          const DiskLoc& start )
        : _txn( txn ),
          _rs( rs ),
          _dir( dir ),
          _reseekKeyValid( false ),
          // XXX not using a snapshot here
          _iterator( _rs->_db->NewIterator( rs->_readOptions(), rs->_columnFamily ) ) {
        if (start.isNull()) {
            if ( _forward() )
                _iterator->SeekToFirst();
            else
                _iterator->SeekToLast();
        }
        else {
            _iterator->Seek( rs->_makeKey( start ) );

            if ( !_forward() && !_iterator->Valid() )
                _iterator->SeekToLast();
            else if ( !_forward() && _iterator->Valid() &&
                      _makeDiskLoc( _iterator->key() ) != start )
                _iterator->Prev();
        }

        _checkStatus();
    }

    void RocksRecordStore::Iterator::_checkStatus() {
        if ( !_iterator->status().ok() )
            log() << "Rocks Iterator Error: " << _iterator->status().ToString();
        invariant( _iterator->status().ok() );
    }

    bool RocksRecordStore::Iterator::isEOF() {
        return !_iterator || !_iterator->Valid();
    }

    DiskLoc RocksRecordStore::Iterator::curr() {
        if ( !_iterator->Valid() )
            return DiskLoc();

        rocksdb::Slice slice = _iterator->key();
        return _makeDiskLoc( slice );
    }

    DiskLoc RocksRecordStore::Iterator::getNext() {
        if ( !_iterator->Valid() ) {
            return DiskLoc();
        }

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

    void RocksRecordStore::Iterator::saveState() {
        if ( !_iterator ) {
            return;
        }
        if ( _iterator->Valid() ) {
            _reseekKey = _iterator->key().ToString();
            _reseekKeyValid = true;
        }
    }

    bool RocksRecordStore::Iterator::restoreState(OperationContext* txn) {
        _txn = txn;
        if ( !_reseekKeyValid ) {
          _iterator.reset( NULL );
          return true;
        }
        _iterator.reset( _rs->_db->NewIterator( _rs->_readOptions(),
                                                _rs->_columnFamily ) );
        _checkStatus();
        _iterator->Seek( _reseekKey );
        _checkStatus();
        _reseekKeyValid = false;

        return true;
    }

    RecordData RocksRecordStore::Iterator::dataFor( const DiskLoc& loc ) const {
        return _rs->dataFor( _txn, loc );
    }

    bool RocksRecordStore::Iterator::_forward() const {
        return _dir == CollectionScanParams::FORWARD;
    }
}
