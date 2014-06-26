// record_store_heap.h

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
#include <map>

#include "mongo/db/structure/capped_callback.h"
#include "mongo/db/structure/record_store.h"

namespace mongo {
    class HeapRecordIterator;

    /**
     * A RecordStore that stores all data on the heap.
     *
     * @param cappedMaxSize - required if isCapped. limit uses dataSize() in this impl.
     */
    class HeapRecordStore : public RecordStore {
    public:
        explicit HeapRecordStore(const StringData& ns,
                                 bool isCapped = false,
                                 int64_t cappedMaxSize = -1,
                                 int64_t cappedMaxDocs = -1,
                                 CappedDocumentDeleteCallback* cappedDeleteCallback = NULL);

        virtual const char* name() const;

        virtual RecordData dataFor( const DiskLoc& loc ) const;

        virtual void deleteRecord( OperationContext* txn, const DiskLoc& dl );

        virtual StatusWith<DiskLoc> insertRecord( OperationContext* txn,
                                                  const char* data,
                                                  int len,
                                                  int quotaMax );

        virtual StatusWith<DiskLoc> insertRecord( OperationContext* txn,
                                                  const DocWriter* doc,
                                                  int quotaMax );
                                                  
        virtual StatusWith<DiskLoc> updateRecord( OperationContext* txn,
                                                  const DiskLoc& oldLocation,
                                                  const char* data,
                                                  int len,
                                                  int quotaMax,
                                                  UpdateMoveNotifier* notifier );
                                                  
        virtual Status updateWithDamages( OperationContext* txn,
                                          const DiskLoc& loc,
                                          const char* damangeSource,
                                          const mutablebson::DamageVector& damages );

        virtual RecordIterator* getIterator( const DiskLoc& start, bool tailable,
                                             const CollectionScanParams::Direction& dir) const;

        virtual RecordIterator* getIteratorForRepair() const;

        virtual std::vector<RecordIterator*> getManyIterators() const;

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
                                 
        virtual void appendCustomStats( BSONObjBuilder* result, double scale ) const;

        virtual Status touch( OperationContext* txn, BSONObjBuilder* output ) const;
        
        virtual Status setCustomOption( OperationContext* txn,
                                        const BSONElement& option,
                                        BSONObjBuilder* info = NULL );

        virtual void increaseStorageSize( OperationContext* txn,  int size, int quotaMax );

        virtual int64_t storageSize(BSONObjBuilder* extraInfo = NULL, int infoLevel = 0) const;

        virtual long long dataSize() const { return _dataSize; }

        virtual long long numRecords() const { return _records.size(); }

    protected:
        virtual Record* recordFor( const DiskLoc& loc ) const;

    public:
        //
        // Not in RecordStore interface
        //

        typedef std::map<DiskLoc, boost::shared_array<char> > Records;

        bool isCapped() const { return _isCapped; }
        void setCappedDeleteCallback(CappedDocumentDeleteCallback* cb) { _cappedDeleteCallback = cb; }
        bool cappedMaxDocs() const { invariant(_isCapped); return _cappedMaxDocs; }
        bool cappedMaxSize() const { invariant(_isCapped); return _cappedMaxSize; }

    private:
        DiskLoc allocateLoc();
        bool cappedAndNeedDelete() const;
        void cappedDeleteAsNeeded(OperationContext* txn);

        // TODO figure out a proper solution to metadata
        const bool _isCapped;
        const int64_t _cappedMaxSize;
        const int64_t _cappedMaxDocs;
        CappedDocumentDeleteCallback* _cappedDeleteCallback;
        int64_t _dataSize;

        Records _records;
        int64_t _nextId;
    };

    class HeapRecordIterator : public RecordIterator {
    public:
        HeapRecordIterator(const HeapRecordStore::Records& records,
                           const HeapRecordStore& rs,
                           DiskLoc start = DiskLoc(),
                           bool tailable = false);

        virtual bool isEOF();

        virtual DiskLoc curr();

        virtual DiskLoc getNext();

        virtual void invalidate(const DiskLoc& dl);

        virtual void prepareToYield();

        virtual bool recoverFromYield();

        virtual RecordData dataFor( const DiskLoc& loc ) const;

    private:
        HeapRecordStore::Records::const_iterator _it;
        bool _tailable;
        DiskLoc _lastLoc; // only for restarting tailable
        bool _killedByInvalidate;

        const HeapRecordStore::Records& _records;
        const HeapRecordStore& _rs;
    };

    class HeapRecordReverseIterator : public RecordIterator {
    public:
        HeapRecordReverseIterator(const HeapRecordStore::Records& records,
                                  const HeapRecordStore& rs,
                                  DiskLoc start = DiskLoc());

        virtual bool isEOF();

        virtual DiskLoc curr();

        virtual DiskLoc getNext();

        virtual void invalidate(const DiskLoc& dl);

        virtual void prepareToYield();

        virtual bool recoverFromYield();

        virtual RecordData dataFor( const DiskLoc& loc ) const;

    private:
        HeapRecordStore::Records::const_reverse_iterator _it;
        bool _killedByInvalidate;

        const HeapRecordStore::Records& _records;
        const HeapRecordStore& _rs;
    };

} // namespace mongo
