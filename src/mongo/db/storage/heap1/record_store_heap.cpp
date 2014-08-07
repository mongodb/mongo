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

#include "mongo/db/storage/heap1/record_store_heap.h"

#include "mongo/util/mongoutils/str.h"

namespace mongo {

    //
    // RecordStore
    //

    HeapRecordStore::HeapRecordStore(const StringData& ns,
                                     bool isCapped,
                                     int64_t cappedMaxSize,
                                     int64_t cappedMaxDocs,
                                     CappedDocumentDeleteCallback* cappedDeleteCallback)
            : RecordStore(ns),
              _isCapped(isCapped),
              _cappedMaxSize(cappedMaxSize),
              _cappedMaxDocs(cappedMaxDocs),
              _cappedDeleteCallback(cappedDeleteCallback),
              _dataSize(0),
              _nextId(1) { // DiskLoc(0,0) isn't valid for records.

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

    RecordData HeapRecordStore::dataFor( const DiskLoc& loc ) const {
        return recordFor(loc)->toRecordData();
    }

    HeapRecordStore::HeapRecord* HeapRecordStore::recordFor(const DiskLoc& loc) const {
        Records::const_iterator it = _records.find(loc);
        invariant(it != _records.end());
        return reinterpret_cast<HeapRecord*>(it->second.get());
    }

    void HeapRecordStore::deleteRecord(OperationContext* txn, const DiskLoc& loc) {
        HeapRecord* rec = recordFor(loc);
        _dataSize -= rec->netLength();
        invariant(_records.erase(loc) == 1);
    }

    bool HeapRecordStore::cappedAndNeedDelete() const {
        if (!_isCapped)
            return false;

        if (_dataSize > _cappedMaxSize)
            return true;

        if ((_cappedMaxDocs != -1) && (numRecords() > _cappedMaxDocs))
            return true;

        return false;
    }

    void HeapRecordStore::cappedDeleteAsNeeded(OperationContext* txn) {
        while (cappedAndNeedDelete()) {
            invariant(!_records.empty());

            DiskLoc oldest = _records.begin()->first;

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

        // TODO padding?
        const int lengthWithHeaders = len + HeapRecord::HeaderSize;
        boost::shared_array<char> buf(new char[lengthWithHeaders]);
        HeapRecord* rec = reinterpret_cast<HeapRecord*>(buf.get());
        rec->lengthWithHeaders() = lengthWithHeaders;
        memcpy(rec->data(), data, len);

        const DiskLoc loc = allocateLoc();
        _records[loc] = buf;
        _dataSize += len;

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

        // TODO padding?
        const int lengthWithHeaders = len + HeapRecord::HeaderSize;
        boost::shared_array<char> buf(new char[lengthWithHeaders]);
        HeapRecord* rec = reinterpret_cast<HeapRecord*>(buf.get());
        rec->lengthWithHeaders() = lengthWithHeaders;
        doc->writeDocument(rec->data());

        const DiskLoc loc = allocateLoc();
        _records[loc] = buf;
        _dataSize += len;

        cappedDeleteAsNeeded(txn);

        return StatusWith<DiskLoc>(loc);
    }

    StatusWith<DiskLoc> HeapRecordStore::updateRecord(OperationContext* txn,
                                                      const DiskLoc& oldLocation,
                                                      const char* data,
                                                      int len,
                                                      bool enforceQuota,
                                                      UpdateMoveNotifier* notifier ) {
        HeapRecord* oldRecord = recordFor( oldLocation );
        int oldLen = oldRecord->netLength();

        // If the length of the new data is <= the length of the old data then just
        // memcopy into the old space
        if ( len <= oldLen) {
            memcpy(oldRecord->data(), data, len);
            _dataSize += len - oldLen;
            return StatusWith<DiskLoc>(oldLocation);
        }

        if ( _isCapped ) {
            return StatusWith<DiskLoc>( ErrorCodes::InternalError,
                                        "failing update: objects in a capped ns cannot grow",
                                        10003 );
        }

        // If the length of the new data exceeds the size of the old Record, we need to allocate
        // a new Record, and delete the old one

        const int lengthWithHeaders = len + HeapRecord::HeaderSize;
        boost::shared_array<char> buf(new char[lengthWithHeaders]);
        HeapRecord* rec = reinterpret_cast<HeapRecord*>(buf.get());
        rec->lengthWithHeaders() = lengthWithHeaders;
        memcpy(rec->data(), data, len);

        _records[oldLocation] = buf;
        _dataSize += len - oldLen;

        cappedDeleteAsNeeded(txn);

        return StatusWith<DiskLoc>(oldLocation);
    }

    Status HeapRecordStore::updateWithDamages( OperationContext* txn,
                                               const DiskLoc& loc,
                                               const char* damangeSource,
                                               const mutablebson::DamageVector& damages ) {
        HeapRecord* rec = recordFor( loc );
        char* root = rec->data();

        // All updates were in place. Apply them via durability and writing pointer.
        mutablebson::DamageVector::const_iterator where = damages.begin();
        const mutablebson::DamageVector::const_iterator end = damages.end();
        for( ; where != end; ++where ) {
            const char* sourcePtr = damangeSource + where->sourceOffset;
            char* targetPtr = root + where->targetOffset;
            std::memcpy(targetPtr, sourcePtr, where->size);
        }

        return Status::OK();
    }

    RecordIterator* HeapRecordStore::getIterator(OperationContext* txn,
                                                 const DiskLoc& start,
                                                 bool tailable,
                                                 const CollectionScanParams::Direction& dir) const {
        if (tailable)
            invariant(_isCapped && dir == CollectionScanParams::FORWARD);

        if (dir == CollectionScanParams::FORWARD) {
            return new HeapRecordIterator(txn, _records, *this, start, tailable);
        }
        else {
            return new HeapRecordReverseIterator(txn, _records, *this, start);
        }
    }

    RecordIterator* HeapRecordStore::getIteratorForRepair(OperationContext* txn) const {
        // TODO maybe make different from HeapRecordIterator
        return new HeapRecordIterator(txn, _records, *this);
    }

    std::vector<RecordIterator*> HeapRecordStore::getManyIterators(OperationContext* txn) const {
        std::vector<RecordIterator*> out;
        // TODO maybe find a way to return multiple iterators.
        out.push_back(new HeapRecordIterator(txn, _records, *this));
        return out;
    }

    Status HeapRecordStore::truncate(OperationContext* txn) {
        _records.clear();
        _dataSize = 0;
        return Status::OK();
    }

    void HeapRecordStore::temp_cappedTruncateAfter(OperationContext* txn,
                                                   DiskLoc end,
                                                   bool inclusive) {
        Records::iterator it = inclusive ? _records.lower_bound(end)
                                         : _records.upper_bound(end);
        while(it != _records.end()) {
            _dataSize -= reinterpret_cast<HeapRecord*>(it->second.get())->netLength();
            _records.erase(it++);
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
            for (Records::const_iterator it = _records.begin(); it != _records.end(); ++it) {
                HeapRecord* rec = reinterpret_cast<HeapRecord*>(it->second.get());
                size_t dataSize;
                const Status status = adaptor->validate(rec->toRecordData(), &dataSize);
                if (!status.isOK()) {
                    results->valid = false;
                    results->errors.push_back("invalid object detected (see logs)");
                    log() << "Invalid object detected in " << _ns << ": " << status.reason();
                }
            }
        }

        output->appendNumber( "nrecords", _records.size() );

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

        return Status( ErrorCodes::BadValue,
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
        const int64_t recordOverhead = numRecords() * HeapRecord::HeaderSize;
        return _dataSize + recordOverhead;
    }

    DiskLoc HeapRecordStore::allocateLoc() {
        const int64_t id = _nextId++;
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

    bool HeapRecordIterator::restoreState() {
        return !_killedByInvalidate;
    }

    RecordData HeapRecordIterator::dataFor(const DiskLoc& loc) const {
        return _rs.dataFor(loc);
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
            _it = HeapRecordStore::Records::const_reverse_iterator(_records.find(start));
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
        if (isEOF())
            return;

        if (_it->first == loc) {
            if (_rs.isCapped()) {
                // Capped iterators die on invalidation rather than advancing.
                _killedByInvalidate = true;
                return;
            }
            ++_it;
        }
    }

    void HeapRecordReverseIterator::saveState() {
    }

    bool HeapRecordReverseIterator::restoreState() {
        return !_killedByInvalidate;
    }

    RecordData HeapRecordReverseIterator::dataFor(const DiskLoc& loc) const {
        return _rs.dataFor(loc);
    }

} // namespace mongo
