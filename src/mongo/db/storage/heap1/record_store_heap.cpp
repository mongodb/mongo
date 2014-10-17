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

#include "mongo/db/storage/heap1/record_store_heap.h"

#include "mongo/db/operation_context.h"
#include "mongo/db/storage/recovery_unit.h"
#include "mongo/util/log.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {
    class HeapRecordStore::InsertChange : public RecoveryUnit::Change {
    public:
        InsertChange(Data* data, DiskLoc loc) :_data(data), _loc(loc) {}
        virtual void commit() {}
        virtual void rollback() {
            Records::iterator it = _data->records.find(_loc);
            if (it != _data->records.end()) {
                _data->dataSize -= it->second.size;
                _data->records.erase(it);
            }
        }

    private:
        Data* const _data;
        const DiskLoc _loc;
    };

    // Works for both removes and updates
    class HeapRecordStore::RemoveChange : public RecoveryUnit::Change {
    public:
        RemoveChange(Data* data, DiskLoc loc, const HeapRecord& rec)
            :_data(data), _loc(loc), _rec(rec)
        {}

        virtual void commit() {}
        virtual void rollback() {
            Records::iterator it = _data->records.find(_loc);
            if (it != _data->records.end()) {
                _data->dataSize -= it->second.size;
            }

            _data->dataSize += _rec.size;
            _data->records[_loc] = _rec;
        }

    private:
        Data* const _data;
        const DiskLoc _loc;
        const HeapRecord _rec;
    };

    //
    // RecordStore
    //

    HeapRecordStore::HeapRecordStore(const StringData& ns,
                                     boost::shared_ptr<void>* dataInOut,
                                     bool isCapped,
                                     int64_t cappedMaxSize,
                                     int64_t cappedMaxDocs,
                                     CappedDocumentDeleteCallback* cappedDeleteCallback)
            : RecordStore(ns),
              _isCapped(isCapped),
              _cappedMaxSize(cappedMaxSize),
              _cappedMaxDocs(cappedMaxDocs),
              _cappedDeleteCallback(cappedDeleteCallback),
              _data(*dataInOut ? static_cast<Data*>(dataInOut->get()) : new Data()) {

        if (!*dataInOut) {
            dataInOut->reset(_data); // takes ownership
        }

        if (_isCapped) {
            invariant(_cappedMaxSize > 0);
            invariant(_cappedMaxDocs == -1 || _cappedMaxDocs > 0);
        }
        else {
            invariant(_cappedMaxSize == -1);
            invariant(_cappedMaxDocs == -1);
        }
    }

    const char* HeapRecordStore::name() const { return "heap"; }

    RecordData HeapRecordStore::dataFor( OperationContext* txn, const DiskLoc& loc ) const {
        return recordFor(loc)->toRecordData();
    }

    const HeapRecordStore::HeapRecord* HeapRecordStore::recordFor(const DiskLoc& loc) const {
        Records::const_iterator it = _data->records.find(loc);
        if ( it == _data->records.end() ) {
            error() << "HeapRecordStore::recordFor cannot find record for " << ns() << ":" << loc;
        }
        invariant(it != _data->records.end());
        return &it->second;
    }

    HeapRecordStore::HeapRecord* HeapRecordStore::recordFor(const DiskLoc& loc) {
        Records::iterator it = _data->records.find(loc);
        if ( it == _data->records.end() ) {
            error() << "HeapRecordStore::recordFor cannot find record for " << ns() << ":" << loc;
        }
        invariant(it != _data->records.end());
        return &it->second;
    }

    bool HeapRecordStore::findRecord( OperationContext* txn,
                                      const DiskLoc& loc, RecordData* rd ) const {
        Records::const_iterator it = _data->records.find(loc);
        if ( it == _data->records.end() ) {
            return false;
        }
        *rd = it->second.toRecordData();
        return true;
    }

    void HeapRecordStore::deleteRecord(OperationContext* txn, const DiskLoc& loc) {
        HeapRecord* rec = recordFor(loc);
        txn->recoveryUnit()->registerChange(new RemoveChange(_data, loc, *rec));
        _data->dataSize -= rec->size;
        invariant(_data->records.erase(loc) == 1);
    }

    bool HeapRecordStore::cappedAndNeedDelete(OperationContext* txn) const {
        if (!_isCapped)
            return false;

        if (_data->dataSize > _cappedMaxSize)
            return true;

        if ((_cappedMaxDocs != -1) && (numRecords(txn) > _cappedMaxDocs))
            return true;

        return false;
    }

    void HeapRecordStore::cappedDeleteAsNeeded(OperationContext* txn) {
        while (cappedAndNeedDelete(txn)) {
            invariant(!_data->records.empty());

            DiskLoc oldest = _data->records.begin()->first;

            if (_cappedDeleteCallback)
                uassertStatusOK(_cappedDeleteCallback->aboutToDeleteCapped(txn, oldest));

            deleteRecord(txn, oldest);
        }
    }

    StatusWith<DiskLoc> HeapRecordStore::insertRecord(OperationContext* txn,
                                                      const char* data,
                                                      int len,
                                                      bool enforceQuota) {
        if (_isCapped && len > _cappedMaxSize) {
            // We use dataSize for capped rollover and we don't want to delete everything if we know
            // this won't fit.
            return StatusWith<DiskLoc>(ErrorCodes::BadValue,
                                       "object to insert exceeds cappedMaxSize");
        }

        HeapRecord rec(len);
        memcpy(rec.data.get(), data, len);

        const DiskLoc loc = allocateLoc();
        txn->recoveryUnit()->registerChange(new InsertChange(_data, loc));
        _data->dataSize += len;
        _data->records[loc] = rec;

        cappedDeleteAsNeeded(txn);

        return StatusWith<DiskLoc>(loc);
    }

    StatusWith<DiskLoc> HeapRecordStore::insertRecord(OperationContext* txn,
                                                      const DocWriter* doc,
                                                      bool enforceQuota) {
        const int len = doc->documentSize();
        if (_isCapped && len > _cappedMaxSize) {
            // We use dataSize for capped rollover and we don't want to delete everything if we know
            // this won't fit.
            return StatusWith<DiskLoc>(ErrorCodes::BadValue,
                                       "object to insert exceeds cappedMaxSize");
        }

        HeapRecord rec(len);
        doc->writeDocument(rec.data.get());

        const DiskLoc loc = allocateLoc();
        txn->recoveryUnit()->registerChange(new InsertChange(_data, loc));
        _data->dataSize += len;
        _data->records[loc] = rec;

        cappedDeleteAsNeeded(txn);

        return StatusWith<DiskLoc>(loc);
    }

    StatusWith<DiskLoc> HeapRecordStore::updateRecord(OperationContext* txn,
                                                      const DiskLoc& loc,
                                                      const char* data,
                                                      int len,
                                                      bool enforceQuota,
                                                      UpdateMoveNotifier* notifier ) {
        HeapRecord* oldRecord = recordFor( loc );
        int oldLen = oldRecord->size;

        if (_isCapped && len > oldLen) {
            return StatusWith<DiskLoc>( ErrorCodes::InternalError,
                                        "failing update: objects in a capped ns cannot grow",
                                        10003 );
        }

        HeapRecord newRecord(len);
        memcpy(newRecord.data.get(), data, len);

        txn->recoveryUnit()->registerChange(new RemoveChange(_data, loc, *oldRecord));
        _data->dataSize += len - oldLen;
        *oldRecord = newRecord;

        cappedDeleteAsNeeded(txn);

        return StatusWith<DiskLoc>(loc);
    }

    Status HeapRecordStore::updateWithDamages( OperationContext* txn,
                                               const DiskLoc& loc,
                                               const RecordData& oldRec,
                                               const char* damageSource,
                                               const mutablebson::DamageVector& damages ) {
        HeapRecord* oldRecord = recordFor( loc );
        const int len = oldRecord->size;

        HeapRecord newRecord(len);
        memcpy(newRecord.data.get(), oldRecord->data.get(), len);

        txn->recoveryUnit()->registerChange(new RemoveChange(_data, loc, *oldRecord));
        *oldRecord = newRecord;

        cappedDeleteAsNeeded(txn);

        char* root = newRecord.data.get();
        mutablebson::DamageVector::const_iterator where = damages.begin();
        const mutablebson::DamageVector::const_iterator end = damages.end();
        for( ; where != end; ++where ) {
            const char* sourcePtr = damageSource + where->sourceOffset;
            char* targetPtr = root + where->targetOffset;
            std::memcpy(targetPtr, sourcePtr, where->size);
        }

        *oldRecord = newRecord;

        return Status::OK();
    }

    RecordIterator* HeapRecordStore::getIterator(OperationContext* txn,
                                                 const DiskLoc& start,
                                                 const CollectionScanParams::Direction& dir) const {

        if (dir == CollectionScanParams::FORWARD) {
            return new HeapRecordIterator(txn, _data->records, *this, start, false);
        }
        else {
            return new HeapRecordReverseIterator(txn, _data->records, *this, start);
        }
    }

    RecordIterator* HeapRecordStore::getIteratorForRepair(OperationContext* txn) const {
        // TODO maybe make different from HeapRecordIterator
        return new HeapRecordIterator(txn, _data->records, *this);
    }

    std::vector<RecordIterator*> HeapRecordStore::getManyIterators(OperationContext* txn) const {
        std::vector<RecordIterator*> out;
        // TODO maybe find a way to return multiple iterators.
        out.push_back(new HeapRecordIterator(txn, _data->records, *this));
        return out;
    }

    Status HeapRecordStore::truncate(OperationContext* txn) {
        _data->records.clear();
        _data->dataSize = 0;
        return Status::OK();
    }

    void HeapRecordStore::temp_cappedTruncateAfter(OperationContext* txn,
                                                   DiskLoc end,
                                                   bool inclusive) {
        Records::iterator it = inclusive ? _data->records.lower_bound(end)
                                         : _data->records.upper_bound(end);
        while(it != _data->records.end()) {
            txn->recoveryUnit()->registerChange(new RemoveChange(_data, it->first, it->second));
            _data->dataSize -= it->second.size;
            _data->records.erase(it++);
        }
    }

    bool HeapRecordStore::compactSupported() const {
        return false;
    }
    Status HeapRecordStore::compact(OperationContext* txn,
                                    RecordStoreCompactAdaptor* adaptor,
                                    const CompactOptions* options,
                                    CompactStats* stats) {
        // TODO might be possible to do something here
        invariant(!"compact not yet implemented");
    }

    Status HeapRecordStore::validate(OperationContext* txn,
                                     bool full,
                                     bool scanData,
                                     ValidateAdaptor* adaptor,
                                     ValidateResults* results,
                                     BSONObjBuilder* output) const {
        results->valid = true;
        if (scanData && full) {
            for (Records::const_iterator it = _data->records.begin(); it != _data->records.end(); ++it) {
                const HeapRecord& rec = it->second;
                size_t dataSize;
                const Status status = adaptor->validate(rec.toRecordData(), &dataSize);
                if (!status.isOK()) {
                    results->valid = false;
                    results->errors.push_back("invalid object detected (see logs)");
                    log() << "Invalid object detected in " << _ns << ": " << status.reason();
                }
            }
        }

        output->appendNumber( "nrecords", _data->records.size() );

        return Status::OK();

    }

    void HeapRecordStore::appendCustomStats( OperationContext* txn,
                                             BSONObjBuilder* result,
                                             double scale ) const {
        result->appendBool( "capped", _isCapped );
        if ( _isCapped ) {
            result->appendIntOrLL( "max", _cappedMaxDocs );
            result->appendIntOrLL( "maxSize", _cappedMaxSize );
        }
    }

    Status HeapRecordStore::touch(OperationContext* txn, BSONObjBuilder* output) const {
        if (output) {
            output->append("numRanges", 1);
            output->append("millis", 0);
        }
        return Status::OK();
    }

    Status HeapRecordStore::setCustomOption(
                OperationContext* txn, const BSONElement& option, BSONObjBuilder* info) {
        StringData name = option.fieldName();
        if ( name == "usePowerOf2Sizes" ) {
            // we ignore, so just say ok
            return Status::OK();
        }

        return Status( ErrorCodes::InvalidOptions,
                       mongoutils::str::stream()
                       << "unknown custom option to HeapRecordStore: "
                       << name );
    }

    void HeapRecordStore::increaseStorageSize(OperationContext* txn,  int size, bool enforceQuota) {
        // unclear what this would mean for this class. For now, just error if called.
        invariant(!"increaseStorageSize not yet implemented");
    }

    int64_t HeapRecordStore::storageSize(OperationContext* txn,
                                         BSONObjBuilder* extraInfo,
                                         int infoLevel) const {
        // Note: not making use of extraInfo or infoLevel since we don't have extents
        const int64_t recordOverhead = numRecords(txn) * sizeof(HeapRecord);
        return _data->dataSize + recordOverhead;
    }

    DiskLoc HeapRecordStore::allocateLoc() {
        const int64_t id = _data->nextId++;
        // This is a hack, but both the high and low order bits of DiskLoc offset must be 0, and the
        // file must fit in 23 bits. This gives us a total of 30 + 23 == 53 bits.
        invariant(id < (1LL << 53));
        return DiskLoc(int(id >> 30), int((id << 1) & ~(1<<31)));
    }

    //
    // Forward Iterator
    //

    HeapRecordIterator::HeapRecordIterator(OperationContext* txn,
                                           const HeapRecordStore::Records& records,
                                           const HeapRecordStore& rs,
                                           DiskLoc start,
                                           bool tailable)
            : _txn(txn),
              _tailable(tailable),
              _lastLoc(minDiskLoc),
              _killedByInvalidate(false),
              _records(records),
              _rs(rs) {
        if (start.isNull()) {
            _it = _records.begin();
        }
        else {
            _it = _records.find(start);
            invariant(_it != _records.end());
        }
    }

    bool HeapRecordIterator::isEOF() {
        return _it == _records.end();
    }

    DiskLoc HeapRecordIterator::curr() {
        if (isEOF())
            return DiskLoc();
        return _it->first;
    }

    DiskLoc HeapRecordIterator::getNext() {
        if (isEOF()) {
            if (!_tailable)
                return DiskLoc();

            if (_records.empty())
                return DiskLoc();

            invariant(!_killedByInvalidate);

            // recover to last returned record
            invariant(!_lastLoc.isNull());
            _it = _records.find(_lastLoc);
            invariant(_it != _records.end());

            if (++_it == _records.end())
                return DiskLoc();
        }

        const DiskLoc out = _it->first;
        ++_it;
        if (_tailable && _it == _records.end())
            _lastLoc = out;
        return out;
    }

    void HeapRecordIterator::invalidate(const DiskLoc& loc) {
        if (_rs.isCapped()) {
            // Capped iterators die on invalidation rather than advancing.
            if (isEOF()) {
                if (_lastLoc == loc) {
                    _killedByInvalidate = true;
                }
            } 
            else if (_it->first == loc) {
                _killedByInvalidate = true;
            }

            return;
        }

        if (_it != _records.end() && _it->first == loc)
            ++_it;
    }

    void HeapRecordIterator::saveState() {
    }

    bool HeapRecordIterator::restoreState(OperationContext* txn) {
        _txn = txn;
        return !_killedByInvalidate;
    }

    RecordData HeapRecordIterator::dataFor(const DiskLoc& loc) const {
        return _rs.dataFor(_txn, loc);
    }

    //
    // Reverse Iterator
    //

    HeapRecordReverseIterator::HeapRecordReverseIterator(OperationContext* txn,
                                                         const HeapRecordStore::Records& records,
                                                         const HeapRecordStore& rs,
                                                         DiskLoc start)
            : _txn(txn),
              _killedByInvalidate(false),
              _records(records),
              _rs(rs) {
        if (start.isNull()) {
            _it = _records.rbegin();
        }
        else {
            // The reverse iterator will point to the preceding element, so we
            // increment the base iterator to make it point past the found element
            HeapRecordStore::Records::const_iterator baseIt(++_records.find(start));
            _it = HeapRecordStore::Records::const_reverse_iterator(baseIt);
            invariant(_it != _records.rend());
        }
    }

    bool HeapRecordReverseIterator::isEOF() {
        return _it == _records.rend();
    }

    DiskLoc HeapRecordReverseIterator::curr() {
        if (isEOF())
            return DiskLoc();
        return _it->first;
    }

    DiskLoc HeapRecordReverseIterator::getNext() {
        if (isEOF())
            return DiskLoc();

        const DiskLoc out = _it->first;
        ++_it;
        return out;
    }

    void HeapRecordReverseIterator::invalidate(const DiskLoc& loc) {
        if (_killedByInvalidate)
            return;

        if (_savedLoc == loc) {
            if (_rs.isCapped()) {
                // Capped iterators die on invalidation rather than advancing.
                _killedByInvalidate = true;
                return;
            }

            restoreState(_txn);
            invariant(_it->first == _savedLoc);
            ++_it;
            saveState();
        }
    }

    void HeapRecordReverseIterator::saveState() {
        if (isEOF()) {
            _savedLoc = DiskLoc();
        }
        else {
            _savedLoc = _it->first;
        }
    }

    bool HeapRecordReverseIterator::restoreState(OperationContext* txn) {
        if (_savedLoc.isNull()) {
            _it = _records.rend();
        }
        else {
            _it = HeapRecordStore::Records::const_reverse_iterator(++_records.find(_savedLoc));
        }
        return !_killedByInvalidate;
    }

    RecordData HeapRecordReverseIterator::dataFor(const DiskLoc& loc) const {
        return _rs.dataFor(_txn, loc);
    }

} // namespace mongo
