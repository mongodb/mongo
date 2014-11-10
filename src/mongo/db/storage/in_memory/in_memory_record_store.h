// in_memory_record_store.h

/**
*    Copyright (C) 2014 MongoDB Inc.
*
*    This program is free software: you can redistribute it and/or  modify
*    it under the terms of the GNU Affero General Public License, version 3,
*    as published by the Free Software Foundation.
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

#pragma once

#include <boost/shared_array.hpp>
#include <boost/shared_ptr.hpp>
#include <map>

#include "mongo/db/storage/capped_callback.h"
#include "mongo/db/storage/record_store.h"

namespace mongo {

    class InMemoryRecordIterator;

    /**
     * A RecordStore that stores all data in-memory.
     *
     * @param cappedMaxSize - required if isCapped. limit uses dataSize() in this impl.
     */
    class InMemoryRecordStore : public RecordStore {
    public:
        explicit InMemoryRecordStore(const StringData& ns,
                                     boost::shared_ptr<void>* dataInOut,
                                     bool isCapped = false,
                                     int64_t cappedMaxSize = -1,
                                     int64_t cappedMaxDocs = -1,
                                     CappedDocumentDeleteCallback* cappedDeleteCallback = NULL);

        virtual const char* name() const;

        virtual RecordData dataFor( OperationContext* txn, const DiskLoc& loc ) const;

        virtual bool findRecord( OperationContext* txn, const DiskLoc& loc, RecordData* rd ) const;

        virtual void deleteRecord( OperationContext* txn, const DiskLoc& dl );

        virtual StatusWith<DiskLoc> insertRecord( OperationContext* txn,
                                                  const char* data,
                                                  int len,
                                                  bool enforceQuota );

        virtual StatusWith<DiskLoc> insertRecord( OperationContext* txn,
                                                  const DocWriter* doc,
                                                  bool enforceQuota );

        virtual StatusWith<DiskLoc> updateRecord( OperationContext* txn,
                                                  const DiskLoc& oldLocation,
                                                  const char* data,
                                                  int len,
                                                  bool enforceQuota,
                                                  UpdateMoveNotifier* notifier );

        virtual Status updateWithDamages( OperationContext* txn,
                                          const DiskLoc& loc,
                                          const RecordData& oldRec,
                                          const char* damageSource,
                                          const mutablebson::DamageVector& damages );

        virtual RecordIterator* getIterator( OperationContext* txn,
                                             const DiskLoc& start,
                                             const CollectionScanParams::Direction& dir) const;

        virtual RecordIterator* getIteratorForRepair( OperationContext* txn ) const;

        virtual std::vector<RecordIterator*> getManyIterators( OperationContext* txn ) const;

        virtual Status truncate( OperationContext* txn );

        virtual void temp_cappedTruncateAfter( OperationContext* txn, DiskLoc end, bool inclusive );

        virtual bool compactSupported() const;
        virtual Status compact( OperationContext* txn,
                                RecordStoreCompactAdaptor* adaptor,
                                const CompactOptions* options,
                                CompactStats* stats );

        virtual Status validate( OperationContext* txn,
                                 bool full,
                                 bool scanData,
                                 ValidateAdaptor* adaptor,
                                 ValidateResults* results, BSONObjBuilder* output ) const;

        virtual void appendCustomStats( OperationContext* txn,
                                        BSONObjBuilder* result,
                                        double scale ) const;

        virtual Status touch( OperationContext* txn, BSONObjBuilder* output ) const;

        virtual Status setCustomOption( OperationContext* txn,
                                        const BSONElement& option,
                                        BSONObjBuilder* info = NULL );

        virtual void increaseStorageSize( OperationContext* txn,  int size, bool enforceQuota );

        virtual int64_t storageSize( OperationContext* txn,
                                     BSONObjBuilder* extraInfo = NULL,
                                     int infoLevel = 0) const;

        virtual long long dataSize( OperationContext* txn ) const { return _data->dataSize; }

        virtual long long numRecords( OperationContext* txn ) const {
            return _data->records.size();
        }

        virtual DiskLoc oplogStartHack(OperationContext* txn,
                                       const DiskLoc& startingPosition) const;

    protected:
        struct InMemoryRecord {
            InMemoryRecord() :size(0) {}
            InMemoryRecord(int size) :size(size), data(new char[size]) {}

            RecordData toRecordData() const { return RecordData(data.get(), size); }

            int size;
            boost::shared_array<char> data;
        };

        virtual const InMemoryRecord* recordFor( const DiskLoc& loc ) const;
        virtual InMemoryRecord* recordFor( const DiskLoc& loc );

    public:
        //
        // Not in RecordStore interface
        //

        typedef std::map<DiskLoc, InMemoryRecord> Records;

        bool isCapped() const { return _isCapped; }
        void setCappedDeleteCallback(CappedDocumentDeleteCallback* cb) {
            _cappedDeleteCallback = cb;
        }
        bool cappedMaxDocs() const { invariant(_isCapped); return _cappedMaxDocs; }
        bool cappedMaxSize() const { invariant(_isCapped); return _cappedMaxSize; }

    private:
        class InsertChange;
        class RemoveChange;
        class TruncateChange;

        StatusWith<DiskLoc> extractAndCheckLocForOplog(const char* data, int len) const;

        DiskLoc allocateLoc();
        bool cappedAndNeedDelete(OperationContext* txn) const;
        void cappedDeleteAsNeeded(OperationContext* txn);

        // TODO figure out a proper solution to metadata
        const bool _isCapped;
        const int64_t _cappedMaxSize;
        const int64_t _cappedMaxDocs;
        CappedDocumentDeleteCallback* _cappedDeleteCallback;

        // This is the "persistent" data.
        struct Data {
            Data(bool isOplog) :dataSize(0), nextId(1), isOplog(isOplog) {}

            int64_t dataSize;
            Records records;
            int64_t nextId;
            const bool isOplog;
        };

        Data* const _data;
    };

    class InMemoryRecordIterator : public RecordIterator {
    public:
        InMemoryRecordIterator(OperationContext* txn,
                               const InMemoryRecordStore::Records& records,
                               const InMemoryRecordStore& rs,
                               DiskLoc start = DiskLoc(),
                               bool tailable = false);

        virtual bool isEOF();

        virtual DiskLoc curr();

        virtual DiskLoc getNext();

        virtual void invalidate(const DiskLoc& dl);

        virtual void saveState();

        virtual bool restoreState(OperationContext* txn);

        virtual RecordData dataFor( const DiskLoc& loc ) const;

    private:
        OperationContext* _txn; // not owned
        InMemoryRecordStore::Records::const_iterator _it;
        bool _tailable;
        DiskLoc _lastLoc; // only for restarting tailable
        bool _killedByInvalidate;

        const InMemoryRecordStore::Records& _records;
        const InMemoryRecordStore& _rs;
    };

    class InMemoryRecordReverseIterator : public RecordIterator {
    public:
        InMemoryRecordReverseIterator(OperationContext* txn,
                                      const InMemoryRecordStore::Records& records,
                                      const InMemoryRecordStore& rs,
                                      DiskLoc start = DiskLoc());

        virtual bool isEOF();

        virtual DiskLoc curr();

        virtual DiskLoc getNext();

        virtual void invalidate(const DiskLoc& dl);

        virtual void saveState();

        virtual bool restoreState(OperationContext* txn);

        virtual RecordData dataFor( const DiskLoc& loc ) const;

    private:
        OperationContext* _txn; // not owned
        InMemoryRecordStore::Records::const_reverse_iterator _it;
        bool _killedByInvalidate;
        DiskLoc _savedLoc; // isNull if saved at EOF

        const InMemoryRecordStore::Records& _records;
        const InMemoryRecordStore& _rs;
    };

} // namespace mongo
