// record_store_heap.cpp

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

#include "mongo/db/structure/record_store_heap.h"

#include "mongo/db/storage/record.h"

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

    Record* HeapRecordStore::recordFor(const DiskLoc& loc) const {
        Records::const_iterator it = _records.find(loc);
        invariant(it != _records.end());
        return reinterpret_cast<Record*>(it->second.get());
    }

    void HeapRecordStore::deleteRecord(OperationContext* txn, const DiskLoc& loc) {
        Record* rec = recordFor(loc);
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
                                                      int quotaMax) {
        if (_isCapped && len > _cappedMaxSize) {
            // We use dataSize for capped rollover and we don't want to delete everything if we know
            // this won't fit.
            return StatusWith<DiskLoc>(ErrorCodes::BadValue,
                                       "object to insert exceeds cappedMaxSize");
        }

        // TODO padding?
        const int lengthWithHeaders = len + Record::HeaderSize;
        boost::shared_array<char> buf(new char[lengthWithHeaders]);
        Record* rec = reinterpret_cast<Record*>(buf.get());
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
                                                      int quotaMax) {
        const int len = doc->documentSize();
        if (_isCapped && len > _cappedMaxSize) {
            // We use dataSize for capped rollover and we don't want to delete everything if we know
            // this won't fit.
            return StatusWith<DiskLoc>(ErrorCodes::BadValue,
                                       "object to insert exceeds cappedMaxSize");
        }

        // TODO padding?
        const int lengthWithHeaders = len + Record::HeaderSize;
        boost::shared_array<char> buf(new char[lengthWithHeaders]);
        Record* rec = reinterpret_cast<Record*>(buf.get());
        rec->lengthWithHeaders() = lengthWithHeaders;
        doc->writeDocument(rec->data());

        const DiskLoc loc = allocateLoc();
        _records[loc] = buf;
        _dataSize += len;

        cappedDeleteAsNeeded(txn);

        return StatusWith<DiskLoc>(loc);
    }

    RecordIterator* HeapRecordStore::getIterator(const DiskLoc& start,
                                                 bool tailable,
                                                 const CollectionScanParams::Direction& dir) const {
        if (tailable)
            invariant(_isCapped && dir == CollectionScanParams::FORWARD);

        if (dir == CollectionScanParams::FORWARD) {
            return new HeapRecordIterator(_records, *this, start, tailable);
        }
        else {
            return new HeapRecordIterator(_records, *this, start);
        }
    }

    RecordIterator* HeapRecordStore::getIteratorForRepair() const {
        // TODO maybe make different from HeapRecordIterator
        return new HeapRecordIterator(_records, *this);
    }

    std::vector<RecordIterator*> HeapRecordStore::getManyIterators() const {
        std::vector<RecordIterator*> out;
        // TODO maybe find a way to return multiple iterators.
        out.push_back(new HeapRecordIterator(_records, *this));
        return out;
    }

    Status HeapRecordStore::truncate(OperationContext* txn) {
        _records.clear();
        _dataSize = 0;
        return Status::OK();
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
        // TODO put stuff in output

        results->valid = true;
        if (scanData && full) {
            for (Records::const_iterator it = _records.begin(); it != _records.end(); ++it) {
                Record* rec = reinterpret_cast<Record*>(it->second.get());
                size_t dataSize;
                const Status status = adaptor->validate(rec, &dataSize);
                if (!status.isOK()) {
                    results->valid = false;
                    results->errors.push_back("invalid object detected (see logs)");
                    log() << "Invalid object detected in " << _ns << ": " << status.reason();
                }
            }
        }

        return Status::OK();

    }

    Status HeapRecordStore::touch(OperationContext* txn, BSONObjBuilder* output) const {
        if (output) {
            output->append("numRanges", 1);
            output->append("millis", 0);
        }
        return Status::OK();
    }

    void HeapRecordStore::increaseStorageSize(OperationContext* txn,  int size, int quotaMax) {
        // unclear what this would mean for this class. For now, just error if called.
        invariant(!"increaseStorageSize not yet implemented");
    }

    int64_t HeapRecordStore::storageSize(BSONObjBuilder* extraInfo, int infoLevel) const {
        // Note: not making use of extraInfo or infoLevel since we don't have extents
        const int64_t recordOverhead = numRecords() * Record::HeaderSize;
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

    HeapRecordIterator::HeapRecordIterator(const HeapRecordStore::Records& records,
                                           const HeapRecordStore& rs,
                                           DiskLoc start,
                                           bool tailable)
            : _tailable(tailable),
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

        if (_it->first == loc)
            ++_it;
    }

    void HeapRecordIterator::prepareToYield() {
    }

    bool HeapRecordIterator::recoverFromYield() {
        return !_killedByInvalidate;
    }

    const Record* HeapRecordIterator::recordFor(const DiskLoc& loc) const {
        return _rs.recordFor(loc);
    }

    //
    // Reverse Iterator
    //

    HeapRecordReverseIterator::HeapRecordReverseIterator(const HeapRecordStore::Records& records,
                                                         const HeapRecordStore& rs,
                                                         DiskLoc start)
            : _killedByInvalidate(false),
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

    void HeapRecordReverseIterator::prepareToYield() {
    }

    bool HeapRecordReverseIterator::recoverFromYield() {
        return !_killedByInvalidate;
    }

    const Record* HeapRecordReverseIterator::recordFor(const DiskLoc& loc) const {
        return _rs.recordFor(loc);
    }
} // namespace mongo
