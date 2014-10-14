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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kStorage

#include "mongo/db/storage/rocks/rocks_record_store.h"

#include <memory>

#include <rocksdb/comparator.h>
#include <rocksdb/db.h>
#include <rocksdb/options.h>
#include <rocksdb/slice.h>
#include <rocksdb/utilities/write_batch_with_index.h>

#include "mongo/db/operation_context.h"
#include "mongo/db/storage/rocks/rocks_recovery_unit.h"
#include "mongo/util/log.h"
#include "mongo/util/timer.h"

namespace mongo {

    RocksRecordStore::RocksRecordStore(const StringData& ns, const StringData& id,
                                       rocksdb::DB* db,  // not owned here
                                       boost::shared_ptr<rocksdb::ColumnFamilyHandle> columnFamily,
                                       bool isCapped, int64_t cappedMaxSize, int64_t cappedMaxDocs,
                                       CappedDocumentDeleteCallback* cappedDeleteCallback)
        : RecordStore(ns),
          _db(db),
          _columnFamily(columnFamily),
          _isCapped(isCapped),
          _cappedMaxSize(cappedMaxSize),
          _cappedMaxDocs(cappedMaxDocs),
          _cappedDeleteCallback(cappedDeleteCallback),
          _dataSizeKey("datasize-" + id.toString()),
          _numRecordsKey("numrecords-" + id.toString()) {
        invariant( _db );
        invariant( _columnFamily );

        if (_isCapped) {
            invariant(_cappedMaxSize > 0);
            invariant(_cappedMaxDocs == -1 || _cappedMaxDocs > 0);
        }
        else {
            invariant(_cappedMaxSize == -1);
            invariant(_cappedMaxDocs == -1);
        }

        // Get next id
        boost::scoped_ptr<rocksdb::Iterator> iter(
            db->NewIterator(_readOptions(), columnFamily.get()));
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
        if (!_db->Get(_readOptions(), rocksdb::Slice(_numRecordsKey), &value).ok()) {
            _numRecords = 0;
            metadataPresent = false;
        }
        else {
            memcpy( &_numRecords, value.data(), sizeof( _numRecords ));
        }

        // XXX not using a Snapshot here
        if (!_db->Get(_readOptions(), rocksdb::Slice(_dataSizeKey), &value).ok()) {
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
        _db->GetApproximateSizes(_columnFamily.get(), &wholeRange, 1, &storageSize);
        return static_cast<int64_t>( storageSize );
    }

    RecordData RocksRecordStore::dataFor( OperationContext* txn, const DiskLoc& loc) const {
        return _getDataFor(_db, _columnFamily.get(), txn, loc);
    }

    void RocksRecordStore::deleteRecord( OperationContext* txn, const DiskLoc& dl ) {
        RocksRecoveryUnit* ru = RocksRecoveryUnit::getRocksRecoveryUnit( txn );

        std::string oldValue;
        _db->Get(_readOptions(txn), _columnFamily.get(), _makeKey(dl), &oldValue);
        int oldLength = oldValue.size();

        ru->writeBatch()->Delete(_columnFamily.get(), _makeKey(dl));

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
        auto ru = RocksRecoveryUnit::getRocksRecoveryUnit(txn);
        boost::scoped_ptr<rocksdb::Iterator> iter(ru->NewIterator(_columnFamily.get()));
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

        RocksRecoveryUnit* ru = RocksRecoveryUnit::getRocksRecoveryUnit( txn );

        DiskLoc loc = _nextId();

        ru->writeBatch()->Put(_columnFamily.get(), _makeKey(loc), rocksdb::Slice(data, len));

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
        RocksRecoveryUnit* ru = RocksRecoveryUnit::getRocksRecoveryUnit( txn );

        std::string old_value;
        auto status = ru->Get(_columnFamily.get(), _makeKey(loc), &old_value);

        if ( !status.ok() ) {
            return StatusWith<DiskLoc>( ErrorCodes::InternalError, status.ToString() );
        }

        int old_length = old_value.size();

        ru->writeBatch()->Put(_columnFamily.get(), _makeKey(loc), rocksdb::Slice(data, len));

        _increaseDataSize(txn, len - old_length);

        cappedDeleteAsNeeded(txn);

        return StatusWith<DiskLoc>( loc );
    }

    Status RocksRecordStore::updateWithDamages( OperationContext* txn,
                                                const DiskLoc& loc,
                                                const char* damangeSource,
                                                const mutablebson::DamageVector& damages ) {
        RocksRecoveryUnit* ru = RocksRecoveryUnit::getRocksRecoveryUnit( txn );

        rocksdb::Slice key = _makeKey( loc );

        // get original value
        std::string value;
        rocksdb::Status status;
        status = _db->Get(_readOptions(txn), _columnFamily.get(), key, &value);

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
        ru->writeBatch()->Put(_columnFamily.get(), key, value);

        return Status::OK();
    }

    RecordIterator* RocksRecordStore::getIterator( OperationContext* txn,
                                                   const DiskLoc& start,
                                                   bool tailable,
                                                   const CollectionScanParams::Direction& dir
                                                   ) const {
        return new Iterator(txn, _db, _columnFamily, tailable, dir, start);
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
        rocksdb::Status status = _db->CompactRange(_columnFamily.get(), NULL, NULL);
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
                if (full) {
                    RecordData data = dataFor(txn, iter->curr());
                    size_t dataSize;
                    const Status status = adaptor->validate(data, &dataSize);
                    if (!status.isOK()) {
                        results->valid = false;
                        if (invalidObject) {
                            results->errors.push_back("invalid object detected (see logs)");
                        }
                        invalidObject = true;
                        log() << "Invalid object detected in " << _ns << ": " << status.reason();
                    }
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
        bool valid = _db->GetProperty(_columnFamily.get(), "rocksdb.stats", &statsString);
        invariant( valid );
        result->append( "stats", statsString );
    }

    Status RocksRecordStore::touch(OperationContext* txn, BSONObjBuilder* output) const {
        Timer t;
        boost::scoped_ptr<rocksdb::Iterator> itr(
            _db->NewIterator(_readOptions(), _columnFamily.get()));
        itr->SeekToFirst();
        for (; itr->Valid(); itr->Next()) {
            invariant(itr->status().ok());
        }
        invariant(itr->status().ok());

        if (output) {
            output->append("numRanges", 1);
            output->append("millis", t.millis());
        }
        return Status::OK();
    }

    Status RocksRecordStore::setCustomOption( OperationContext* txn,
                                              const BSONElement& option,
                                              BSONObjBuilder* info ) {
        string optionName = option.fieldName();
        if ( optionName == "usePowerOf2Sizes" ) {
            return Status::OK();
        }

        return Status(ErrorCodes::InvalidOptions, "Invalid option: " + optionName);
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
        RocksRecoveryUnit* ru = RocksRecoveryUnit::getRocksRecoveryUnit( opCtx );

        boost::mutex::scoped_lock dataSizeLk( _dataSizeLock );
        ru->writeBatch()->Delete(_dataSizeKey);

        boost::mutex::scoped_lock numRecordsLk( _numRecordsLock );
        ru->writeBatch()->Delete(_numRecordsKey);
    }

    rocksdb::ReadOptions RocksRecordStore::_readOptions(OperationContext* opCtx) {
        rocksdb::ReadOptions options;
        if ( opCtx ) {
            options.snapshot = RocksRecoveryUnit::getRocksRecoveryUnit( opCtx )->snapshot();
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

    rocksdb::Slice RocksRecordStore::_makeKey(const DiskLoc& loc) {
        return rocksdb::Slice(reinterpret_cast<const char*>(&loc), sizeof(loc));
    }

    DiskLoc RocksRecordStore::_makeDiskLoc( const rocksdb::Slice& slice ) {
        return reinterpret_cast<const DiskLoc*>( slice.data() )[0];
    }

    bool RocksRecordStore::findRecord( OperationContext* txn,
                                       const DiskLoc& loc, RecordData* out ) const {
        RecordData rd = _getDataFor(_db, _columnFamily.get(), txn, loc);
        if ( rd.data() == NULL )
            return false;
        *out = rd;
        return true;
    }

    RecordData RocksRecordStore::_getDataFor(rocksdb::DB* db, rocksdb::ColumnFamilyHandle* cf,
                                             OperationContext* txn, const DiskLoc& loc) {
        rocksdb::Slice value;

        RocksRecoveryUnit* ru = RocksRecoveryUnit::getRocksRecoveryUnit(txn);
        auto key = _makeKey(loc);
        boost::shared_array<char> data;

        std::string value_storage;
        auto status = ru->Get(cf, _makeKey(loc), &value_storage);
        if (!status.ok()) {
            if (status.IsNotFound()) {
                return RecordData(nullptr, 0);
            } else {
                log() << "rocks Get failed, blowing up: " << status.ToString();
                invariant(false);
            }
        }
        data.reset(new char[value_storage.size()]);
        memcpy(data.get(), value_storage.data(), value_storage.size());
        value = rocksdb::Slice(data.get(), value_storage.size());
        return RecordData(value.data(), value.size(), data);
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
        RocksRecoveryUnit* ru = RocksRecoveryUnit::getRocksRecoveryUnit( txn );
        const char* nr_ptr = reinterpret_cast<char*>( &_numRecords );

        ru->writeBatch()->Put(rocksdb::Slice(_numRecordsKey),
                              rocksdb::Slice(nr_ptr, sizeof(long long)));
    }


    void RocksRecordStore::_increaseDataSize( OperationContext* txn, int amount ) {
        boost::mutex::scoped_lock lk( _dataSizeLock );

        _dataSize += amount;
        RocksRecoveryUnit* ru = RocksRecoveryUnit::getRocksRecoveryUnit( txn );
        const char* ds_ptr = reinterpret_cast<char*>( &_dataSize );

        ru->writeBatch()->Put(rocksdb::Slice(_dataSizeKey),
                              rocksdb::Slice(ds_ptr, sizeof(long long)));
    }

    // --------

    RocksRecordStore::Iterator::Iterator(
        OperationContext* txn, rocksdb::DB* db,
        boost::shared_ptr<rocksdb::ColumnFamilyHandle> columnFamily, bool tailable,
        const CollectionScanParams::Direction& dir, const DiskLoc& start)
        : _txn(!tailable ? txn : nullptr),
          _db(db),
          _cf(columnFamily),
          _tailable(tailable),
          _dir(dir),
          _eof(true),
          _iterator(RocksRecoveryUnit::getRocksRecoveryUnit(txn)->NewIterator(_cf.get())) {

        _locate(start);
    }

    void RocksRecordStore::Iterator::_checkStatus() {
        if ( !_iterator->status().ok() )
            log() << "Rocks Iterator Error: " << _iterator->status().ToString();
        invariant( _iterator->status().ok() );
    }

    bool RocksRecordStore::Iterator::isEOF() {
        return _eof;
    }

    DiskLoc RocksRecordStore::Iterator::curr() {
        if (_eof) {
            return DiskLoc();
        }

        return _curr;
    }

    DiskLoc RocksRecordStore::Iterator::getNext() {
        if (_eof) {
            return DiskLoc();
        }

        DiskLoc toReturn = _curr;

        if ( _forward() )
            _iterator->Next();
        else
            _iterator->Prev();

        if (_iterator->Valid()) {
            _curr = _decodeCurr();
        } else {
            _eof = true;
            // we leave _curr as it is on purpose
        }
        return toReturn;
    }

    void RocksRecordStore::Iterator::invalidate( const DiskLoc& dl ) {
        _iterator.reset( NULL );
    }

    void RocksRecordStore::Iterator::saveState() {
        _iterator.reset();
    }

    // XXX restoring state with tailable cursor will invalidate the snapshot stored inside of
    // OperationContext. It is important that while restoring state, nobody else is using the
    // OperationContext (i.e. only a single restoreState is called on a tailable cursor with a
    // single OperationContext)
    bool RocksRecordStore::Iterator::restoreState(OperationContext* txn) {
        if (_tailable) {
            // we want to read new data if the iterator is tailable
            RocksRecoveryUnit::getRocksRecoveryUnit(txn)->releaseSnapshot();
        }
        _txn = txn;
        auto ru = RocksRecoveryUnit::getRocksRecoveryUnit(txn);
        _iterator.reset(ru->NewIterator(_cf.get()));
        bool previousEOF = _eof;
        DiskLoc previousCurr = _curr;
        _locate(_curr);
        // if _curr != previousCurr that means that _curr has been deleted, so we don't need to
        // advance it, because we're already on the next document by Seek-ing
        if (previousEOF && _curr == previousCurr) {
            getNext();
        }
        return true;
    }

    RecordData RocksRecordStore::Iterator::dataFor( const DiskLoc& loc ) const {
        return RocksRecordStore::_getDataFor(_db, _cf.get(), _txn, loc);
    }

    void RocksRecordStore::Iterator::_locate(const DiskLoc& loc) {
        if (_forward()) {
            if (loc.isNull()) {
                _iterator->SeekToFirst();
            } else {
                _iterator->Seek(RocksRecordStore::_makeKey(loc));
            }
            _checkStatus();
        } else {  // backward iterator
            if (loc.isNull()) {
                _iterator->SeekToLast();
            } else {
                // lower bound on reverse iterator
                _iterator->Seek(RocksRecordStore::_makeKey(loc));
                _checkStatus();
                if (!_iterator->Valid()) {
                    _iterator->SeekToLast();
                } else if (_decodeCurr() != loc) {
                    _iterator->Prev();
                }
            }
            _checkStatus();
        }
        _eof = !_iterator->Valid();
        if (_eof) {
            _curr = loc;
        } else {
            _curr = _decodeCurr();
        }
    }

    DiskLoc RocksRecordStore::Iterator::_decodeCurr() const {
        invariant(_iterator && _iterator->Valid());
        return _makeDiskLoc(_iterator->key());
    }

    bool RocksRecordStore::Iterator::_forward() const {
        return _dir == CollectionScanParams::FORWARD;
    }

}
