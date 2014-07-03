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
                                        rocksdb::ColumnFamilyHandle* metadataColumnFamily )
        : RecordStore( ns ),
          _db( db ),
          _columnFamily( columnFamily ),
          _metadataColumnFamily( metadataColumnFamily ) {
        invariant( _db );
        invariant( _columnFamily );
        invariant( _metadataColumnFamily );

        // Get next id
        boost::scoped_ptr<rocksdb::Iterator> iter( db->NewIterator( rocksdb::ReadOptions(),
                                            columnFamily ) );
        iter->SeekToLast();
        if (iter->Valid()) {
            rocksdb::Slice last_slice = iter->key();
            DiskLoc last_loc = reinterpret_cast<DiskLoc*>( const_cast<char*>( 
                                                            last_slice.data() ) )[0];
            _nextIdNum = last_loc.a() + ( (uint64_t) last_loc.getOfs() << 32 );
        }
        else
            _nextIdNum = 1;

        // load metadata
        std::string value;
        if (!_db->Get( rocksdb::ReadOptions(),
                           _metadataColumnFamily,
                           rocksdb::Slice("numRecords"),
                           &value ).ok()) {
            _numRecords = 0;
        }
        else
            _numRecords = *((long long *) value.data());
        if (!_db->Get( rocksdb::ReadOptions(),
                           _metadataColumnFamily,
                           rocksdb::Slice("dataSize"),
                           &value ).ok()) {
            _dataSize = 0;
        }
        else
            _dataSize = *((long long *) value.data());
    }

    RocksRecordStore::~RocksRecordStore() {
    }

    long long RocksRecordStore::dataSize() const {
        return _dataSize;
    }

    long long RocksRecordStore::numRecords() const {
        return _numRecords;
    }

    bool RocksRecordStore::isCapped() const {
        return false;
    }

    int64_t RocksRecordStore::storageSize( BSONObjBuilder* extraInfo, int infoLevel ) const {
        return dataSize(); // todo: this isn't very good
    }

    RecordData RocksRecordStore::dataFor( const DiskLoc& loc) const {
        std::string* value = new std::string();
        rocksdb::Status status;
        status = _db->Get( rocksdb::ReadOptions(),
                           _columnFamily,
                           _makeKey( loc ),
                           value );

        if ( !status.ok() ) {
            if ( status.IsNotFound() )
                return RecordData( NULL, 0 );

            log() << "rocks Get failed, blowing up: " << status.ToString();
            invariant( false );
        }

        boost::shared_array<char> data( reinterpret_cast<char*>(value) );

        return RecordData( value->data(), value->size(), data );
    }

    void RocksRecordStore::deleteRecord( OperationContext* txn, const DiskLoc& dl ) {
        RocksRecoveryUnit* ru = _getRecoveryUnit( txn );

        std::string old_value;
        _db->Get(rocksdb::ReadOptions(),
                           _columnFamily,
                           _makeKey( dl ),
                           &old_value );
        int old_length = old_value.size();

        ru->writeBatch()->Delete( _columnFamily,
                                  _makeKey( dl ) );

        _changeNumRecords(txn, false);
        _increaseDataSize(txn, -old_length);
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

        _changeNumRecords(txn, true);
        _increaseDataSize(txn, len);

        return StatusWith<DiskLoc>( loc );
    }

    StatusWith<DiskLoc> RocksRecordStore::insertRecord( OperationContext* txn,
                                                        const DocWriter* doc,
                                                        bool enforceQuota ) {

        const int len = doc->documentSize();

        boost::shared_array<char> buf(new char[len]);
        doc->writeDocument(buf.get());

        RocksRecoveryUnit* ru = _getRecoveryUnit( txn );

        DiskLoc loc = _nextId();

        ru->writeBatch()->Put( _columnFamily,
                               _makeKey( loc ),
                               rocksdb::Slice( buf.get(), len ) );

        _changeNumRecords(txn, true);
        _increaseDataSize(txn, len);

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
        _db->Get(rocksdb::ReadOptions(),
                           _columnFamily,
                           _makeKey( loc ),
                           &old_value );
        int old_length = old_value.size();

        ru->writeBatch()->Put( _columnFamily,
                               _makeKey( loc ),
                               rocksdb::Slice( data, len ) );

        _increaseDataSize(txn, len - old_length);

        return StatusWith<DiskLoc>( loc );
    }

    // Wrote this whole implementation of updateWithDamages, only to find out merge_operator
    // is not compatible with backward_iterator in rocks. Since we need backward_iterators we
    // can't use the rocks merge_operator functionality... :(
    /* 
    // expects the first sizeof(char*) bytes of value to be damageSource, and the following
    // bytes to be damages
    class UpdateWithDamagesOperator : public AssociativeMergeOperator {
     public:
      virtual bool Merge(
        const Slice& key,
        const Slice* existing_value,
        const Slice& value,
        std::string* new_value,
        Logger* logger) const override {
        const mutablebson::DamageVector* damages = 
            reinterpret_cast<const mutablebson::DamageVector*>( value.data() 
                                            + sizeof(char) * sizeof(char*) );
        const char* damageSource = *reinterpret_cast<char**>( value.data() );

        std::string valueCopy(existing_value.get(), existing_value.size());
       
        char* root = const_cast<char*>( valueCopy.data() );
        for( size_t i = 0; i < damages.size(); i++ ) {
            mutablebson::DamageEvent event = damages[i];
            const char* sourcePtr = damageSource + event.sourceOffset;
            char* targetPtr = root + event.targetOffset;
            std::memcpy(targetPtr, sourcePtr, event.size);
        }

        *new_value = valueCopy;
        return true;
      }

      virtual const char* Name() const override {
        return "UpdateWithDamagesOperator";
       }
    };

    Status RocksRecordStore::updateWithDamages( OperationContext* txn,
                                                const DiskLoc& loc,
                                                const char* damageSource,
                                                const mutablebson::DamageVector& damages ) {
        std::string damages_value(reinterpret_cast<char *>( &damageSource ), sizeof(char*));
        const char* damages_ptr = reinterpret_cast<const char*>( &damages );
        damages_value.append(damages_ptr, sizeof(damages));
        rocksdb::Slice value(damages_value);

        rocksdb::Status status = _db->Merge( rocksdb::WriteOptions(),
                           _columnFamily,
                           _makeKey( loc ),
                           value );
        invariant( status.ok() );
        return Status::OK();
    }
    */

    Status RocksRecordStore::updateWithDamages( OperationContext* txn,
                                                const DiskLoc& loc,
                                                const char* damangeSource,
                                                const mutablebson::DamageVector& damages ) {
        RocksRecoveryUnit* ru = _getRecoveryUnit( txn );

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
        ru->writeBatch()->Put(_columnFamily,
                           key,
                           value );

        return Status::OK();
    }

    RecordIterator* RocksRecordStore::getIterator( const DiskLoc& start,
                                                   bool tailable,
                                                   const CollectionScanParams::Direction& dir
                                                   ) const {
        invariant( start == DiskLoc() );
        invariant( !tailable );

        return new Iterator( this, dir );
    }


    RecordIterator* RocksRecordStore::getIteratorForRepair() const {
        return getIterator();
    }

    std::vector<RecordIterator*> RocksRecordStore::getManyIterators() const {
        // XXX do we want this to actually return a set of iterators?

        std::vector<RecordIterator*> iterators;
        iterators.push_back(getIterator());
        return iterators;
    }

    Status RocksRecordStore::truncate( OperationContext* txn ) {
        RecordIterator* iter = getIterator();
        while(!iter->isEOF()) {
            DiskLoc loc = iter->getNext();
            deleteRecord(txn, loc);
        }

        return Status::OK();
    }

    Status RocksRecordStore::compact( OperationContext* txn,
                                      RecordStoreCompactAdaptor* adaptor,
                                      const CompactOptions* options,
                                      CompactStats* stats ) {
        rocksdb::Status status = _db->CompactRange(_columnFamily, nullptr, nullptr);
        invariant (status.ok());
        return Status::OK();
    }

    Status RocksRecordStore::validate( OperationContext* txn,
                                       bool full, bool scanData,
                                       ValidateAdaptor* adaptor,
                                       ValidateResults* results, BSONObjBuilder* output ) const {
        output->append( "rocks", "no validate yet" );
        return Status::OK();
    }

    void RocksRecordStore::appendCustomStats( BSONObjBuilder* result, double scale ) const {
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
        boost::mutex::scoped_lock lk( _idLock );
        int ofs = _nextIdNum >> 32;
        int a = (_nextIdNum << 32) >> 32;
        DiskLoc loc( ofs, a );
        _nextIdNum++;
        return loc;
    }

    rocksdb::Slice RocksRecordStore::_makeKey( const DiskLoc& loc ) const {
        return rocksdb::Slice( reinterpret_cast<const char*>( &loc ), sizeof( loc ) );
    }

    RocksRecoveryUnit* RocksRecordStore::_getRecoveryUnit( OperationContext* opCtx ) const {
        return dynamic_cast<RocksRecoveryUnit*>( opCtx->recoveryUnit() );
    }

    void RocksRecordStore::_changeNumRecords(OperationContext* txn, bool insert) {
        if (insert) {
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


    void RocksRecordStore::_increaseDataSize(OperationContext* txn, int amount) {
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

    // XXX mmapv1 just leaves these 2 blank... so is it ok to also leave them like this?
    void RocksRecordStore::Iterator::prepareToYield() {
    }

    bool RocksRecordStore::Iterator::recoverFromYield() {
        return true;
    }

    RecordData RocksRecordStore::Iterator::dataFor( const DiskLoc& loc ) const {
        // XXX is this actually an optimization?
        if (*reinterpret_cast<const DiskLoc*>( _iterator->key().data() ) == loc) {
            rocksdb::Slice data_slice = _iterator->value();

            boost::shared_array<char> data( new char[data_slice.size()] );
            memcpy(data.get(), data_slice.data(), data_slice.size());

            return RecordData( data.get(), data_slice.size(), data );
        }

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
