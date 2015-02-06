// in_memory_record_store.cpp

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

#include "mongo/db/storage/in_memory/in_memory_record_store.h"

#include <boost/shared_ptr.hpp>

#include "mongo/db/jsobj.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/storage/oplog_hack.h"
#include "mongo/db/storage/recovery_unit.h"
#include "mongo/util/log.h"
#include "mongo/db/namespace_string.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {

    using boost::shared_ptr;

    class InMemoryRecordStore::InsertChange : public RecoveryUnit::Change {
    public:
        InsertChange(Data* data, RecordId loc) :_data(data), _loc(loc) {}
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
        const RecordId _loc;
    };

    // Works for both removes and updates
    class InMemoryRecordStore::RemoveChange : public RecoveryUnit::Change {
    public:
        RemoveChange(Data* data, RecordId loc, const InMemoryRecord& rec)
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
        const RecordId _loc;
        const InMemoryRecord _rec;
    };

    class InMemoryRecordStore::TruncateChange : public RecoveryUnit::Change {
    public:
        TruncateChange(Data* data) : _data(data), _dataSize(0) {
            using std::swap;
            swap(_dataSize, _data->dataSize);
            swap(_records, _data->records);
        }

        virtual void commit() {}
        virtual void rollback() {
            using std::swap;
            swap(_dataSize, _data->dataSize);
            swap(_records, _data->records);
        }

    private:
        Data* const _data;
        int64_t _dataSize;
        Records _records;
    };

    //
    // RecordStore
    //

    InMemoryRecordStore::InMemoryRecordStore(StringData ns,
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
              _data(*dataInOut ? static_cast<Data*>(dataInOut->get())
                               : new Data(NamespaceString::oplog(ns))) {
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

    const char* InMemoryRecordStore::name() const { return "InMemory"; }

    RecordData InMemoryRecordStore::dataFor( OperationContext* txn, const RecordId& loc ) const {
        return recordFor(loc)->toRecordData();
    }

    const InMemoryRecordStore::InMemoryRecord* InMemoryRecordStore::recordFor(
            const RecordId& loc) const {
        Records::const_iterator it = _data->records.find(loc);
        if ( it == _data->records.end() ) {
            error() << "InMemoryRecordStore::recordFor cannot find record for " << ns()
                    << ":" << loc;
        }
        invariant(it != _data->records.end());
        return &it->second;
    }

    InMemoryRecordStore::InMemoryRecord* InMemoryRecordStore::recordFor(const RecordId& loc) {
        Records::iterator it = _data->records.find(loc);
        if ( it == _data->records.end() ) {
            error() << "InMemoryRecordStore::recordFor cannot find record for " << ns()
                    << ":" << loc;
        }
        invariant(it != _data->records.end());
        return &it->second;
    }

    bool InMemoryRecordStore::findRecord( OperationContext* txn,
                                          const RecordId& loc, RecordData* rd ) const {
        Records::const_iterator it = _data->records.find(loc);
        if ( it == _data->records.end() ) {
            return false;
        }
        *rd = it->second.toRecordData();
        return true;
    }

    void InMemoryRecordStore::deleteRecord(OperationContext* txn, const RecordId& loc) {
        InMemoryRecord* rec = recordFor(loc);
        txn->recoveryUnit()->registerChange(new RemoveChange(_data, loc, *rec));
        _data->dataSize -= rec->size;
        invariant(_data->records.erase(loc) == 1);
    }

    bool InMemoryRecordStore::cappedAndNeedDelete(OperationContext* txn) const {
        if (!_isCapped)
            return false;

        if (_data->dataSize > _cappedMaxSize)
            return true;

        if ((_cappedMaxDocs != -1) && (numRecords(txn) > _cappedMaxDocs))
            return true;

        return false;
    }

    void InMemoryRecordStore::cappedDeleteAsNeeded(OperationContext* txn) {
        while (cappedAndNeedDelete(txn)) {
            invariant(!_data->records.empty());

            Records::iterator oldest = _data->records.begin();
            RecordId id = oldest->first;
            RecordData data = oldest->second.toRecordData();

            if (_cappedDeleteCallback)
                uassertStatusOK(_cappedDeleteCallback->aboutToDeleteCapped(txn, id, data));

            deleteRecord(txn, id);
        }
    }

    StatusWith<RecordId> InMemoryRecordStore::extractAndCheckLocForOplog(const char* data,
                                                                        int len) const {
        StatusWith<RecordId> status = oploghack::extractKey(data, len);
        if (!status.isOK())
            return status;

        if (!_data->records.empty() && status.getValue() <= _data->records.rbegin()->first)
            return StatusWith<RecordId>(ErrorCodes::BadValue, "ts not higher than highest");

        return status;
    }

    StatusWith<RecordId> InMemoryRecordStore::insertRecord(OperationContext* txn,
                                                          const char* data,
                                                          int len,
                                                          bool enforceQuota) {
        if (_isCapped && len > _cappedMaxSize) {
            // We use dataSize for capped rollover and we don't want to delete everything if we know
            // this won't fit.
            return StatusWith<RecordId>(ErrorCodes::BadValue,
                                       "object to insert exceeds cappedMaxSize");
        }

        InMemoryRecord rec(len);
        memcpy(rec.data.get(), data, len);

        RecordId loc;
        if (_data->isOplog) {
            StatusWith<RecordId> status = extractAndCheckLocForOplog(data, len);
            if (!status.isOK())
                return status;
            loc = status.getValue();
        }
        else {
            loc = allocateLoc();
        }

        txn->recoveryUnit()->registerChange(new InsertChange(_data, loc));
        _data->dataSize += len;
        _data->records[loc] = rec;

        cappedDeleteAsNeeded(txn);

        return StatusWith<RecordId>(loc);
    }

    StatusWith<RecordId> InMemoryRecordStore::insertRecord(OperationContext* txn,
                                                          const DocWriter* doc,
                                                          bool enforceQuota) {
        const int len = doc->documentSize();
        if (_isCapped && len > _cappedMaxSize) {
            // We use dataSize for capped rollover and we don't want to delete everything if we know
            // this won't fit.
            return StatusWith<RecordId>(ErrorCodes::BadValue,
                                       "object to insert exceeds cappedMaxSize");
        }

        InMemoryRecord rec(len);
        doc->writeDocument(rec.data.get());

        RecordId loc;
        if (_data->isOplog) {
            StatusWith<RecordId> status = extractAndCheckLocForOplog(rec.data.get(), len);
            if (!status.isOK())
                return status;
            loc = status.getValue();
        }
        else {
            loc = allocateLoc();
        }

        txn->recoveryUnit()->registerChange(new InsertChange(_data, loc));
        _data->dataSize += len;
        _data->records[loc] = rec;

        cappedDeleteAsNeeded(txn);

        return StatusWith<RecordId>(loc);
    }

    StatusWith<RecordId> InMemoryRecordStore::updateRecord(OperationContext* txn,
                                                          const RecordId& loc,
                                                          const char* data,
                                                          int len,
                                                          bool enforceQuota,
                                                          UpdateNotifier* notifier ) {
        InMemoryRecord* oldRecord = recordFor( loc );
        int oldLen = oldRecord->size;

        if (_isCapped && len > oldLen) {
            return StatusWith<RecordId>( ErrorCodes::InternalError,
                                        "failing update: objects in a capped ns cannot grow",
                                        10003 );
        }

        if (notifier) {
            // The in-memory KV engine uses the invalidation framework (does not support
            // doc-locking), and therefore must notify that it is updating a document.
            Status callbackStatus = notifier->recordStoreGoingToUpdateInPlace(txn, loc);
            if (!callbackStatus.isOK()) {
                return StatusWith<RecordId>(callbackStatus);
            }
        }

        InMemoryRecord newRecord(len);
        memcpy(newRecord.data.get(), data, len);

        txn->recoveryUnit()->registerChange(new RemoveChange(_data, loc, *oldRecord));
        _data->dataSize += len - oldLen;
        *oldRecord = newRecord;

        cappedDeleteAsNeeded(txn);

        return StatusWith<RecordId>(loc);
    }

    bool InMemoryRecordStore::updateWithDamagesSupported() const {
        // TODO: Currently the UpdateStage assumes that updateWithDamages will apply the
        // damages directly to the unowned BSONObj containing the record to be modified.
        // The implementation of updateWithDamages() below copies the old record to a
        // a new one and then applies the damages.
        //
        // We should be able to enable updateWithDamages() here once this assumption is
        // relaxed.
        return false;
    }

    Status InMemoryRecordStore::updateWithDamages( OperationContext* txn,
                                                   const RecordId& loc,
                                                   const RecordData& oldRec,
                                                   const char* damageSource,
                                                   const mutablebson::DamageVector& damages ) {
        InMemoryRecord* oldRecord = recordFor( loc );
        const int len = oldRecord->size;

        InMemoryRecord newRecord(len);
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

    RecordIterator* InMemoryRecordStore::getIterator(
            OperationContext* txn,
            const RecordId& start,
            const CollectionScanParams::Direction& dir) const {

        if (dir == CollectionScanParams::FORWARD) {
            return new InMemoryRecordIterator(txn, _data->records, *this, start, false);
        }
        else {
            return new InMemoryRecordReverseIterator(txn, _data->records, *this, start);
        }
    }

    RecordIterator* InMemoryRecordStore::getIteratorForRepair(OperationContext* txn) const {
        // TODO maybe make different from InMemoryRecordIterator
        return new InMemoryRecordIterator(txn, _data->records, *this);
    }

    std::vector<RecordIterator*> InMemoryRecordStore::getManyIterators(
            OperationContext* txn) const {
        std::vector<RecordIterator*> out;
        // TODO maybe find a way to return multiple iterators.
        out.push_back(new InMemoryRecordIterator(txn, _data->records, *this));
        return out;
    }

    Status InMemoryRecordStore::truncate(OperationContext* txn) {
        // Unlike other changes, TruncateChange mutates _data on construction to perform the
        // truncate
        txn->recoveryUnit()->registerChange(new TruncateChange(_data));
        return Status::OK();
    }

    void InMemoryRecordStore::temp_cappedTruncateAfter(OperationContext* txn,
                                                       RecordId end,
                                                       bool inclusive) {
        Records::iterator it = inclusive ? _data->records.lower_bound(end)
                                         : _data->records.upper_bound(end);
        while(it != _data->records.end()) {
            txn->recoveryUnit()->registerChange(new RemoveChange(_data, it->first, it->second));
            _data->dataSize -= it->second.size;
            _data->records.erase(it++);
        }
    }

    Status InMemoryRecordStore::validate(OperationContext* txn,
                                         bool full,
                                         bool scanData,
                                         ValidateAdaptor* adaptor,
                                         ValidateResults* results,
                                         BSONObjBuilder* output) {
        results->valid = true;
        if (scanData && full) {
            for (Records::const_iterator it = _data->records.begin();
                        it != _data->records.end(); ++it) {
                const InMemoryRecord& rec = it->second;
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

    void InMemoryRecordStore::appendCustomStats( OperationContext* txn,
                                                 BSONObjBuilder* result,
                                                 double scale ) const {
        result->appendBool( "capped", _isCapped );
        if ( _isCapped ) {
            result->appendIntOrLL( "max", _cappedMaxDocs );
            result->appendIntOrLL( "maxSize", _cappedMaxSize / scale );
        }
    }

    Status InMemoryRecordStore::touch(OperationContext* txn, BSONObjBuilder* output) const {
        if (output) {
            output->append("numRanges", 1);
            output->append("millis", 0);
        }
        return Status::OK();
    }

    Status InMemoryRecordStore::setCustomOption(
                OperationContext* txn, const BSONElement& option, BSONObjBuilder* info) {
        StringData name = option.fieldName();
        if ( name == "usePowerOf2Sizes" ) {
            // we ignore, so just say ok
            return Status::OK();
        }

        return Status( ErrorCodes::InvalidOptions,
                       mongoutils::str::stream()
                       << "unknown custom option to InMemoryRecordStore: "
                       << name );
    }

    void InMemoryRecordStore::increaseStorageSize(OperationContext* txn,
                                                  int size, bool enforceQuota) {
        // unclear what this would mean for this class. For now, just error if called.
        invariant(!"increaseStorageSize not yet implemented");
    }

    int64_t InMemoryRecordStore::storageSize(OperationContext* txn,
                                             BSONObjBuilder* extraInfo,
                                             int infoLevel) const {
        // Note: not making use of extraInfo or infoLevel since we don't have extents
        const int64_t recordOverhead = numRecords(txn) * sizeof(InMemoryRecord);
        return _data->dataSize + recordOverhead;
    }

    RecordId InMemoryRecordStore::allocateLoc() {
        RecordId out = RecordId(_data->nextId++);
        invariant(out < RecordId::max());
        return out;
    }

    boost::optional<RecordId> InMemoryRecordStore::oplogStartHack(
            OperationContext* txn,
            const RecordId& startingPosition) const {

        if (!_data->isOplog)
            return boost::none;

        const Records& records = _data->records;

        if (records.empty())
            return RecordId();

        Records::const_iterator it = records.lower_bound(startingPosition);
        if (it == records.end() || it->first > startingPosition)
            --it;

        return it->first;
    }

    //
    // Forward Iterator
    //

    InMemoryRecordIterator::InMemoryRecordIterator(OperationContext* txn,
                                                   const InMemoryRecordStore::Records& records,
                                                   const InMemoryRecordStore& rs,
                                                   RecordId start,
                                                   bool tailable)
            : _txn(txn),
              _tailable(tailable),
              _lastLoc(RecordId::min()),
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

    bool InMemoryRecordIterator::isEOF() {
        return _it == _records.end();
    }

    RecordId InMemoryRecordIterator::curr() {
        if (isEOF())
            return RecordId();
        return _it->first;
    }

    RecordId InMemoryRecordIterator::getNext() {
        if (isEOF()) {
            if (!_tailable)
                return RecordId();

            if (_records.empty())
                return RecordId();

            invariant(!_killedByInvalidate);

            // recover to last returned record
            invariant(!_lastLoc.isNull());
            _it = _records.find(_lastLoc);
            invariant(_it != _records.end());

            if (++_it == _records.end())
                return RecordId();
        }

        const RecordId out = _it->first;
        ++_it;
        if (_tailable && _it == _records.end())
            _lastLoc = out;
        return out;
    }

    void InMemoryRecordIterator::invalidate(const RecordId& loc) {
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

    void InMemoryRecordIterator::saveState() {
    }

    bool InMemoryRecordIterator::restoreState(OperationContext* txn) {
        _txn = txn;
        return !_killedByInvalidate;
    }

    RecordData InMemoryRecordIterator::dataFor(const RecordId& loc) const {
        return _rs.dataFor(_txn, loc);
    }

    //
    // Reverse Iterator
    //

    InMemoryRecordReverseIterator::InMemoryRecordReverseIterator(
            OperationContext* txn,
            const InMemoryRecordStore::Records& records,
            const InMemoryRecordStore& rs,
            RecordId start) : _txn(txn),
                             _killedByInvalidate(false),
                             _records(records),
                             _rs(rs) {

        if (start.isNull()) {
            _it = _records.rbegin();
        }
        else {
            // The reverse iterator will point to the preceding element, so we
            // increment the base iterator to make it point past the found element
            InMemoryRecordStore::Records::const_iterator baseIt(++_records.find(start));
            _it = InMemoryRecordStore::Records::const_reverse_iterator(baseIt);
            invariant(_it != _records.rend());
        }
    }

    bool InMemoryRecordReverseIterator::isEOF() {
        return _it == _records.rend();
    }

    RecordId InMemoryRecordReverseIterator::curr() {
        if (isEOF())
            return RecordId();
        return _it->first;
    }

    RecordId InMemoryRecordReverseIterator::getNext() {
        if (isEOF())
            return RecordId();

        const RecordId out = _it->first;
        ++_it;
        return out;
    }

    void InMemoryRecordReverseIterator::invalidate(const RecordId& loc) {
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

    void InMemoryRecordReverseIterator::saveState() {
        if (isEOF()) {
            _savedLoc = RecordId();
        }
        else {
            _savedLoc = _it->first;
        }
    }

    bool InMemoryRecordReverseIterator::restoreState(OperationContext* txn) {
        if (_savedLoc.isNull()) {
            _it = _records.rend();
        }
        else {
            _it = InMemoryRecordStore::Records::const_reverse_iterator(++_records.find(_savedLoc));
        }
        return !_killedByInvalidate;
    }

    RecordData InMemoryRecordReverseIterator::dataFor(const RecordId& loc) const {
        return _rs.dataFor(_txn, loc);
    }

} // namespace mongo
