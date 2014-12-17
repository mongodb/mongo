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
#include <algorithm>

#include <boost/scoped_array.hpp>

#include <rocksdb/comparator.h>
#include <rocksdb/db.h>
#include <rocksdb/options.h>
#include <rocksdb/slice.h>
#include <rocksdb/utilities/write_batch_with_index.h>

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/concurrency/write_conflict_exception.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/storage/rocks/rocks_recovery_unit.h"
#include "mongo/db/storage/oplog_hack.h"
#include "mongo/util/log.h"
#include "mongo/util/timer.h"

namespace mongo {

    namespace {

        class CappedInsertChange : public RecoveryUnit::Change {
        public:
            CappedInsertChange(CappedVisibilityManager* cappedVisibilityManager,
                               const RecordId& record)
                : _cappedVisibilityManager(cappedVisibilityManager), _record(record) {}

            virtual void commit() { _cappedVisibilityManager->dealtWithCappedRecord(_record); }

            virtual void rollback() { _cappedVisibilityManager->dealtWithCappedRecord(_record); }

        private:
            CappedVisibilityManager* _cappedVisibilityManager;
            RecordId _record;
        };
    }  // namespace

    void CappedVisibilityManager::addUncommittedRecord(OperationContext* txn,
                                                       const RecordId& record) {
        boost::mutex::scoped_lock lk(_lock);
        _addUncommittedRecord_inlock(txn, record);
    }

    void CappedVisibilityManager::_addUncommittedRecord_inlock(OperationContext* txn,
                                                               const RecordId& record) {
        // todo: make this a dassert at some point
        invariant(_uncommittedRecords.empty() || _uncommittedRecords.back() < record);
        _uncommittedRecords.push_back(record);
        txn->recoveryUnit()->registerChange(new CappedInsertChange(this, record));
        _oplog_highestSeen = record;
    }

    RecordId CappedVisibilityManager::getNextAndAddUncommittedRecord(
        OperationContext* txn, std::function<RecordId()> nextId) {
        boost::mutex::scoped_lock lk(_lock);
        RecordId record = nextId();
        _addUncommittedRecord_inlock(txn, record);
        return record;
    }

    void CappedVisibilityManager::dealtWithCappedRecord(const RecordId& record) {
        boost::mutex::scoped_lock lk(_lock);
        std::vector<RecordId>::iterator it =
            std::find(_uncommittedRecords.begin(), _uncommittedRecords.end(), record);
        invariant(it != _uncommittedRecords.end());
        _uncommittedRecords.erase(it);
    }

    bool CappedVisibilityManager::isCappedHidden(const RecordId& record) const {
        boost::mutex::scoped_lock lk(_lock);
        if (_uncommittedRecords.empty()) {
            return false;
        }
        return _uncommittedRecords.front() <= record;
    }

    void CappedVisibilityManager::updateHighestSeen(const RecordId& record) {
        if (record > _oplog_highestSeen) {
            boost::mutex::scoped_lock lk(_lock);
            if (record > _oplog_highestSeen) {
                _oplog_highestSeen = record;
            }
        }
    }

    RecordId CappedVisibilityManager::oplogStartHack() const {
        boost::mutex::scoped_lock lk(_lock);
        if (_uncommittedRecords.empty()) {
            return _oplog_highestSeen;
        } else {
            return _uncommittedRecords.front();
        }
    }

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
          _isOplog(NamespaceString::oplog(ns)),
          _oplogCounter(0),
          _cappedVisibilityManager((_isCapped || _isOplog) ? new CappedVisibilityManager()
                                                           : nullptr),
          _ident(id.toString()),
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
            RecordId lastId = _makeRecordId(lastSlice);
            if (_isOplog || _isCapped) {
                _cappedVisibilityManager->updateHighestSeen(lastId);
            }
            _nextIdNum.store(lastId.repr() + 1);
        }
        else {
            // Need to start at 1 so we are always higher than RecordId::min()
            _nextIdNum.store( 1 );
        }

        // load metadata
        std::string value;
        bool metadataPresent = true;
        // XXX not using a Snapshot here
        if (!_db->Get(_readOptions(), rocksdb::Slice(_numRecordsKey), &value).ok()) {
            _numRecords.store(0);
            metadataPresent = false;
        }
        else {
            long long numRecords = 0;
            memcpy( &numRecords, value.data(), sizeof(numRecords));
            _numRecords.store(numRecords);
        }

        // XXX not using a Snapshot here
        if (!_db->Get(_readOptions(), rocksdb::Slice(_dataSizeKey), &value).ok()) {
            _dataSize.store(0);
            invariant(!metadataPresent);
        }
        else {
            invariant(value.size() == sizeof(long long));
            long long ds;
            memcpy(&ds, value.data(), sizeof(long long));
            invariant(ds >= 0);
            _dataSize.store(ds);
        }
    }

    int64_t RocksRecordStore::storageSize( OperationContext* txn,
                                           BSONObjBuilder* extraInfo,
                                           int infoLevel ) const {
        uint64_t storageSize;
        rocksdb::Range wholeRange( _makeKey( RecordId::min() ), _makeKey( RecordId::max() ) );
        _db->GetApproximateSizes(_columnFamily.get(), &wholeRange, 1, &storageSize);
        return static_cast<int64_t>( storageSize );
    }

    RecordData RocksRecordStore::dataFor( OperationContext* txn, const RecordId& loc) const {
        return _getDataFor(_db, _columnFamily.get(), txn, loc);
    }

    void RocksRecordStore::deleteRecord( OperationContext* txn, const RecordId& dl ) {
        RocksRecoveryUnit* ru = RocksRecoveryUnit::getRocksRecoveryUnit( txn );
        if (!ru->transaction()->registerWrite(_getTransactionID(dl))) {
            throw WriteConflictException();
        }

        std::string oldValue;
        ru->Get(_columnFamily.get(), _makeKey(dl), &oldValue);
        int oldLength = oldValue.size();

        ru->writeBatch()->Delete(_columnFamily.get(), _makeKey(dl));

        _changeNumRecords(txn, false);
        _increaseDataSize(txn, -oldLength);
    }

    long long RocksRecordStore::numRecords(OperationContext* txn) const {
        RocksRecoveryUnit* ru = RocksRecoveryUnit::getRocksRecoveryUnit( txn );
        return _numRecords.load(std::memory_order::memory_order_relaxed) +
            ru->getDeltaCounter(_numRecordsKey);
    }

    bool RocksRecordStore::cappedAndNeedDelete(long long dataSizeDelta,
                                               long long numRecordsDelta) const {
        if (!_isCapped)
            return false;

        if (_dataSize.load() + dataSizeDelta > _cappedMaxSize)
            return true;

        if ((_cappedMaxDocs != -1) && (_numRecords.load() + numRecordsDelta > _cappedMaxDocs))
            return true;

        return false;
    }

    void RocksRecordStore::cappedDeleteAsNeeded(OperationContext* txn,
                                                const RecordId& justInserted) {
        if (_isOplog) {
            if (_oplogCounter++ % 100 > 0) {
                return;
            }
        }

        long long dataSizeDelta = 0, numRecordsDelta = 0;
        if (!_isOplog) {
            auto ru = RocksRecoveryUnit::getRocksRecoveryUnit(txn);
            dataSizeDelta = ru->getDeltaCounter(_dataSizeKey);
            numRecordsDelta = ru->getDeltaCounter(_numRecordsKey);
        }

        if (!cappedAndNeedDelete(dataSizeDelta, numRecordsDelta)) {
            return;
        }

        // ensure only one thread at a time can do deletes, otherwise they'll conflict.
        boost::mutex::scoped_lock lock(_cappedDeleterMutex, boost::try_to_lock);
        if (!lock) {
            return;
        }

        // we do this is a sub transaction in case it aborts
        RocksRecoveryUnit* realRecoveryUnit =
            dynamic_cast<RocksRecoveryUnit*>(txn->releaseRecoveryUnit());
        invariant(realRecoveryUnit);
        txn->setRecoveryUnit(realRecoveryUnit->newRocksRecoveryUnit());

        try {
            auto ru = RocksRecoveryUnit::getRocksRecoveryUnit(txn);
            boost::scoped_ptr<rocksdb::Iterator> iter(ru->NewIterator(_columnFamily.get()));
            iter->SeekToFirst();

            while (cappedAndNeedDelete(dataSizeDelta, numRecordsDelta) && iter->Valid()) {
                WriteUnitOfWork wuow(txn);

                invariant(_numRecords.load() > 0);

                rocksdb::Slice slice = iter->key();
                RecordId oldest = _makeRecordId(slice);

                if (oldest >= justInserted) {
                    break;
                }

                if (_cappedDeleteCallback) {
                    uassertStatusOK(_cappedDeleteCallback->aboutToDeleteCapped(txn, oldest));
                }

                deleteRecord(txn, oldest);

                iter->Next();

                // We need to commit here to reflect updates on _numRecords and _dataSize.
                // TODO: investigate if we should reflect changes to _numRecords and _dataSize
                // immediately (read uncommitted).
                wuow.commit();
            }
        }
        catch (const WriteConflictException& wce) {
            delete txn->releaseRecoveryUnit();
            txn->setRecoveryUnit(realRecoveryUnit);
            if (_isOplog) {
                log() << "got conflict purging oplog, ignoring";
                return;
            }
            throw;
        }
        catch (...) {
            delete txn->releaseRecoveryUnit();
            txn->setRecoveryUnit(realRecoveryUnit);
            throw;
        }

        delete txn->releaseRecoveryUnit();
        txn->setRecoveryUnit(realRecoveryUnit);
    }

    StatusWith<RecordId> RocksRecordStore::insertRecord( OperationContext* txn,
                                                        const char* data,
                                                        int len,
                                                        bool enforceQuota ) {
        if ( _isCapped && len > _cappedMaxSize ) {
            return StatusWith<RecordId>( ErrorCodes::BadValue,
                                       "object to insert exceeds cappedMaxSize" );
        }

        RocksRecoveryUnit* ru = RocksRecoveryUnit::getRocksRecoveryUnit( txn );

        RecordId loc;
        if (_isOplog) {
            StatusWith<RecordId> status = oploghack::extractKey(data, len);
            if (!status.isOK()) {
                return status;
            }
            loc = status.getValue();
            _cappedVisibilityManager->updateHighestSeen(loc);
        } else if (_isCapped) {
            loc = _cappedVisibilityManager->getNextAndAddUncommittedRecord(
                txn, [&]() { return _nextId(); });
        } else {
            loc = _nextId();
        }

        // XXX it might be safe to remove this, since we just allocated new unique RecordId.
        // However, we need to check if any other transaction can start modifying this RecordId
        // before our transaction is committed
        if (!ru->transaction()->registerWrite(_getTransactionID(loc))) {
            throw WriteConflictException();
        }
        ru->writeBatch()->Put(_columnFamily.get(), _makeKey(loc), rocksdb::Slice(data, len));

        _changeNumRecords( txn, true );
        _increaseDataSize( txn, len );

        cappedDeleteAsNeeded(txn, loc);

        return StatusWith<RecordId>( loc );
    }

    StatusWith<RecordId> RocksRecordStore::insertRecord( OperationContext* txn,
                                                        const DocWriter* doc,
                                                        bool enforceQuota ) {
        const int len = doc->documentSize();
        boost::scoped_array<char> buf( new char[len] );
        doc->writeDocument( buf.get() );

        return insertRecord( txn, buf.get(), len, enforceQuota );
    }

    StatusWith<RecordId> RocksRecordStore::updateRecord( OperationContext* txn,
                                                        const RecordId& loc,
                                                        const char* data,
                                                        int len,
                                                        bool enforceQuota,
                                                        UpdateMoveNotifier* notifier ) {
        RocksRecoveryUnit* ru = RocksRecoveryUnit::getRocksRecoveryUnit( txn );
        if (!ru->transaction()->registerWrite(_getTransactionID(loc))) {
            throw WriteConflictException();
        }

        std::string old_value;
        auto status = ru->Get(_columnFamily.get(), _makeKey(loc), &old_value);

        if ( !status.ok() ) {
            return StatusWith<RecordId>( ErrorCodes::InternalError, status.ToString() );
        }

        int old_length = old_value.size();

        ru->writeBatch()->Put(_columnFamily.get(), _makeKey(loc), rocksdb::Slice(data, len));

        _increaseDataSize(txn, len - old_length);

        cappedDeleteAsNeeded(txn, loc);

        return StatusWith<RecordId>( loc );
    }

    Status RocksRecordStore::updateWithDamages( OperationContext* txn,
                                                const RecordId& loc,
                                                const RecordData& oldRec,
                                                const char* damageSource,
                                                const mutablebson::DamageVector& damages ) {
        RocksRecoveryUnit* ru = RocksRecoveryUnit::getRocksRecoveryUnit( txn );
        if (!ru->transaction()->registerWrite(_getTransactionID(loc))) {
            throw WriteConflictException();
        }

        rocksdb::Slice key = _makeKey( loc );

        // get original value
        std::string value;
        rocksdb::Status status;
        status = ru->Get(_columnFamily.get(), key, &value);

        if ( !status.ok() ) {
            if ( status.IsNotFound() )
                return Status( ErrorCodes::InternalError, "doc not found for in-place update" );

            log() << "rocks Get failed, blowing up: " << status.ToString();
            invariant( false );
        }

        // apply changes to our copy
        for( size_t i = 0; i < damages.size(); i++ ) {
            mutablebson::DamageEvent event = damages[i];
            const char* sourcePtr = damageSource + event.sourceOffset;

            invariant( event.targetOffset + event.size < value.length() );
            value.replace( event.targetOffset, event.size, sourcePtr, event.size );
        }

        // write back
        ru->writeBatch()->Put(_columnFamily.get(), key, value);

        return Status::OK();
    }

    RecordIterator* RocksRecordStore::getIterator(OperationContext* txn, const RecordId& start,
                                                  const CollectionScanParams::Direction& dir)
        const {
        if (_isOplog && dir == CollectionScanParams::FORWARD) {
            auto ru = RocksRecoveryUnit::getRocksRecoveryUnit(txn);
            if (!ru->hasSnapshot() || ru->getOplogReadTill().isNull()) {
                // we don't have snapshot, we can update our view
                ru->setOplogReadTill(_cappedVisibilityManager->oplogStartHack());
            }
        }

        return new Iterator(txn, _db, _columnFamily, _cappedVisibilityManager, dir, start);
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
            RecordId loc = iter->getNext();
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
            output->appendNumber("nrecords", numRecords(txn));

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
        // no need to use snapshot here, since we're just loading records into memory
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

    Status RocksRecordStore::oplogDiskLocRegister(OperationContext* txn, const OpTime& opTime) {
        invariant(_isOplog);
        StatusWith<RecordId> record = oploghack::keyForOptime(opTime);
        if (record.isOK()) {
            _cappedVisibilityManager->addUncommittedRecord(txn, record.getValue());
        }

        return record.getStatus();
    }

    /**
     * Return the RecordId of an oplog entry as close to startingPosition as possible without
     * being higher. If there are no entries <= startingPosition, return RecordId().
     */
    boost::optional<RecordId> RocksRecordStore::oplogStartHack(
        OperationContext* txn, const RecordId& startingPosition) const {

        if (!_isOplog) {
            return boost::none;
        }

        auto ru = RocksRecoveryUnit::getRocksRecoveryUnit(txn);
        ru->setOplogReadTill(_cappedVisibilityManager->oplogStartHack());

        boost::scoped_ptr<rocksdb::Iterator> iter(ru->NewIterator(_columnFamily.get()));
        iter->Seek(_makeKey(startingPosition));
        if (!iter->Valid()) {
            iter->SeekToLast();
            if (iter->Valid()) {
                // startingPosition is bigger than everything else
                return _makeRecordId(iter->key());
            } else {
                // record store is empty
                return RecordId();
            }
        }

        // We're at or past target:
        // 1) if we're at -- return
        // 2) if we're past -- do a prev()
        RecordId foundKey = reinterpret_cast<const RecordId*>(iter->key().data())[0];
        int cmp = startingPosition.compare(foundKey);
        if (cmp != 0) {
            // RocksDB invariant -- iterator needs to land at or past target when Seek-ing
            invariant(cmp < 0);
            // we're past target -- prev()
            iter->Prev();
        }

        if (!iter->Valid()) {
            // there are no entries <= startingPosition
            return RecordId();
        }

        return _makeRecordId(iter->key());
    }

    namespace {
        class RocksCollectionComparator : public rocksdb::Comparator {
            public:
                RocksCollectionComparator() { }
                virtual ~RocksCollectionComparator() { }

                virtual int Compare( const rocksdb::Slice& a, const rocksdb::Slice& b ) const {
                    RecordId lhs = reinterpret_cast<const RecordId*>( a.data() )[0];
                    RecordId rhs = reinterpret_cast<const RecordId*>( b.data() )[0];
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
                                                     RecordId end,
                                                     bool inclusive ) {
        // copied from WiredTigerRecordStore::temp_cappedTruncateAfter()
        WriteUnitOfWork wuow(txn);
        boost::scoped_ptr<RecordIterator> iter( getIterator( txn, end ) );
        while( !iter->isEOF() ) {
            RecordId loc = iter->getNext();
            if ( end < loc || ( inclusive && end == loc ) ) {
                deleteRecord( txn, loc );
            }
        }
        wuow.commit();
    }

    rocksdb::ReadOptions RocksRecordStore::_readOptions(OperationContext* opCtx) {
        rocksdb::ReadOptions options;
        if ( opCtx ) {
            options.snapshot = RocksRecoveryUnit::getRocksRecoveryUnit( opCtx )->snapshot();
        }
        return options;
    }

    RecordId RocksRecordStore::_nextId() {
        invariant(!_isOplog);
        return RecordId(_nextIdNum.fetchAndAdd(1));
    }

    rocksdb::Slice RocksRecordStore::_makeKey(const RecordId& loc) {
        return rocksdb::Slice(reinterpret_cast<const char*>(&loc), sizeof(loc));
    }

    RecordId RocksRecordStore::_makeRecordId( const rocksdb::Slice& slice ) {
        return reinterpret_cast<const RecordId*>( slice.data() )[0];
    }

    bool RocksRecordStore::findRecord( OperationContext* txn,
                                       const RecordId& loc, RecordData* out ) const {
        RecordData rd = _getDataFor(_db, _columnFamily.get(), txn, loc);
        if ( rd.data() == NULL )
            return false;
        *out = rd;
        return true;
    }

    RecordData RocksRecordStore::_getDataFor(rocksdb::DB* db, rocksdb::ColumnFamilyHandle* cf,
                                             OperationContext* txn, const RecordId& loc) {
        RocksRecoveryUnit* ru = RocksRecoveryUnit::getRocksRecoveryUnit(txn);

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

        SharedBuffer data = SharedBuffer::allocate(value_storage.size());
        memcpy(data.get(), value_storage.data(), value_storage.size());
        return RecordData(data.moveFrom(), value_storage.size());
    }

    // XXX make sure these work with rollbacks (I don't think they will)
    void RocksRecordStore::_changeNumRecords( OperationContext* txn, bool insert ) {
        RocksRecoveryUnit* ru = RocksRecoveryUnit::getRocksRecoveryUnit( txn );
        if ( insert ) {
            ru->incrementCounter(_numRecordsKey, &_numRecords, 1);
        }
        else {
            ru->incrementCounter(_numRecordsKey, &_numRecords,  -1);
        }
    }

    void RocksRecordStore::_increaseDataSize( OperationContext* txn, int amount ) {
        RocksRecoveryUnit* ru = RocksRecoveryUnit::getRocksRecoveryUnit( txn );
        ru->incrementCounter(_dataSizeKey, &_dataSize, amount);
    }

    std::string RocksRecordStore::_getTransactionID(const RecordId& rid) const {
        // TODO -- optimize in the future
        return _ident + std::string(reinterpret_cast<const char*>(&rid), sizeof(rid));
    }

    // --------

    RocksRecordStore::Iterator::Iterator(
        OperationContext* txn, rocksdb::DB* db,
        boost::shared_ptr<rocksdb::ColumnFamilyHandle> columnFamily,
        boost::shared_ptr<CappedVisibilityManager> cappedVisibilityManager,
        const CollectionScanParams::Direction& dir, const RecordId& start)
        : _txn(txn),
          _db(db),
          _cf(columnFamily),
          _cappedVisibilityManager(cappedVisibilityManager),
          _dir(dir),
          _eof(true),
          _readUntilForOplog(RocksRecoveryUnit::getRocksRecoveryUnit(txn)->getOplogReadTill()),
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

    RecordId RocksRecordStore::Iterator::curr() {
        if (_eof) {
            return RecordId();
        }

        return _curr;
    }

    RecordId RocksRecordStore::Iterator::getNext() {
        if (_eof) {
            return RecordId();
        }

        RecordId toReturn = _curr;

        if ( _forward() )
            _iterator->Next();
        else
            _iterator->Prev();

        if (_iterator->Valid()) {
            _curr = _decodeCurr();
            if (_cappedVisibilityManager.get()) {  // isCapped?
                if (_readUntilForOplog.isNull()) {
                    // this is the normal capped case
                    if (_cappedVisibilityManager->isCappedHidden(_curr)) {
                        _eof = true;
                    }
                } else {
                    // this is for oplogs
                    if (_curr > _readUntilForOplog ||
                        (_curr == _readUntilForOplog &&
                         _cappedVisibilityManager->isCappedHidden(_curr))) {
                        _eof = true;
                    }
                }
            }  // isCapped?
        } else {
            _eof = true;
            // we leave _curr as it is on purpose
        }

        _checkStatus();
        return toReturn;
    }

    void RocksRecordStore::Iterator::invalidate( const RecordId& dl ) {
        // this should never be called
    }

    void RocksRecordStore::Iterator::saveState() {
        _iterator.reset();
    }

    bool RocksRecordStore::Iterator::restoreState(OperationContext* txn) {
        _txn = txn;
        if (_eof) {
          return true;
        }

        auto ru = RocksRecoveryUnit::getRocksRecoveryUnit(txn);
        _iterator.reset(ru->NewIterator(_cf.get()));
        _locate(_curr);
        return true;
    }

    RecordData RocksRecordStore::Iterator::dataFor(const RecordId& loc) const {
        if (!_eof && loc == _curr && _iterator->Valid() && _iterator->status().ok()) {
            SharedBuffer data = SharedBuffer::allocate(_iterator->value().size());
            memcpy(data.get(), _iterator->value().data(), _iterator->value().size());
            return RecordData(data.moveFrom(), _iterator->value().size());
        }
        return RocksRecordStore::_getDataFor(_db, _cf.get(), _txn, loc);
    }

    void RocksRecordStore::Iterator::_locate(const RecordId& loc) {
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

    RecordId RocksRecordStore::Iterator::_decodeCurr() const {
        invariant(_iterator && _iterator->Valid());
        return _makeRecordId(_iterator->key());
    }

    bool RocksRecordStore::Iterator::_forward() const {
        return _dir == CollectionScanParams::FORWARD;
    }

}
