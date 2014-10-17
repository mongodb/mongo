// rocks_record_store.h

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

#pragma once

#include <atomic>
#include <string>
#include <memory>

#include <rocksdb/options.h>

#include "mongo/db/storage/capped_callback.h"
#include "mongo/db/storage/record_store.h"
#include "mongo/platform/atomic_word.h"

namespace rocksdb {
    class ColumnFamilyHandle;
    class DB;
    class Iterator;
    class Slice;
}

namespace mongo {

    class RocksRecoveryUnit;

    class RocksRecordStore : public RecordStore {
    public:
        RocksRecordStore(const StringData& ns, const StringData& id, rocksdb::DB* db,
                         boost::shared_ptr<rocksdb::ColumnFamilyHandle> columnFamily,
                         bool isCapped = false, int64_t cappedMaxSize = -1,
                         int64_t cappedMaxDocs = -1,
                         CappedDocumentDeleteCallback* cappedDeleteCallback = NULL);

        virtual ~RocksRecordStore() { }

        // name of the RecordStore implementation
        virtual const char* name() const { return "rocks"; }

        virtual long long dataSize( OperationContext* txn ) const { return _dataSize; }

        virtual long long numRecords( OperationContext* txn ) const;

        virtual bool isCapped() const { return _isCapped; }

        virtual int64_t storageSize( OperationContext* txn,
                                     BSONObjBuilder* extraInfo = NULL,
                                     int infoLevel = 0 ) const;

        // CRUD related

        virtual RecordData dataFor( OperationContext* txn, const DiskLoc& loc ) const;

        virtual bool findRecord( OperationContext* txn,
                                 const DiskLoc& loc,
                                 RecordData* out ) const;

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
                                             const DiskLoc& start = DiskLoc(),
                                             bool tailable = false,
                                             const CollectionScanParams::Direction& dir =
                                             CollectionScanParams::FORWARD ) const;

        virtual RecordIterator* getIteratorForRepair( OperationContext* txn ) const;

        virtual std::vector<RecordIterator*> getManyIterators( OperationContext* txn ) const;

        virtual Status truncate( OperationContext* txn );

        virtual bool compactSupported() const { return true; }

        virtual Status compact( OperationContext* txn,
                                RecordStoreCompactAdaptor* adaptor,
                                const CompactOptions* options,
                                CompactStats* stats );

        virtual Status validate( OperationContext* txn,
                                 bool full, bool scanData,
                                 ValidateAdaptor* adaptor,
                                 ValidateResults* results, BSONObjBuilder* output ) const;

        virtual void appendCustomStats( OperationContext* txn,
                                        BSONObjBuilder* result,
                                        double scale ) const;

        virtual Status touch( OperationContext* txn, BSONObjBuilder* output ) const;

        virtual Status setCustomOption( OperationContext* txn,
                                        const BSONElement& option,
                                        BSONObjBuilder* info = NULL );

        virtual void temp_cappedTruncateAfter(OperationContext* txn,
                                              DiskLoc end,
                                              bool inclusive);

        void setCappedDeleteCallback(CappedDocumentDeleteCallback* cb) {
          _cappedDeleteCallback = cb;
        }
        bool cappedMaxDocs() const { invariant(_isCapped); return _cappedMaxDocs; }
        bool cappedMaxSize() const { invariant(_isCapped); return _cappedMaxSize; }

        /**
         * Drops metadata held by the record store
         */
        void dropRsMetaData( OperationContext* opCtx );

        static rocksdb::Comparator* newRocksCollectionComparator();
    private:

        // NOTE: RecordIterator might outlive the RecordStore
        class Iterator : public RecordIterator {
        public:
            Iterator(OperationContext* txn, rocksdb::DB* db,
                     boost::shared_ptr<rocksdb::ColumnFamilyHandle> columnFamily, bool tailable,
                     const CollectionScanParams::Direction& dir, const DiskLoc& start);

            virtual bool isEOF();
            virtual DiskLoc curr();
            virtual DiskLoc getNext();
            virtual void invalidate(const DiskLoc& dl);
            virtual void saveState();
            virtual bool restoreState(OperationContext* txn);
            virtual RecordData dataFor( const DiskLoc& loc ) const;

        private:
            void _locate(const DiskLoc& loc);
            DiskLoc _decodeCurr() const;
            bool _forward() const;
            void _checkStatus();

            OperationContext* _txn;
            rocksdb::DB* _db; // not owned
            boost::shared_ptr<rocksdb::ColumnFamilyHandle> _cf;
            bool _tailable;
            CollectionScanParams::Direction _dir;
            bool _eof;
            DiskLoc _curr;
            boost::scoped_ptr<rocksdb::Iterator> _iterator;
        };

        /**
         * Returns a new ReadOptions struct, containing the snapshot held in opCtx, if opCtx is not
         * null
         */
        static rocksdb::ReadOptions _readOptions(OperationContext* opCtx = NULL);

        static DiskLoc _makeDiskLoc( const rocksdb::Slice& slice );

        static RecordData _getDataFor(rocksdb::DB* db, rocksdb::ColumnFamilyHandle* cf,
                                      OperationContext* txn, const DiskLoc& loc);

        DiskLoc _nextId();
        bool cappedAndNeedDelete(OperationContext* txn) const;
        void cappedDeleteAsNeeded(OperationContext* txn);

        // The use of this function requires that the passed in DiskLoc outlives the returned Slice
        // TODO possibly make this safer in the future
        static rocksdb::Slice _makeKey( const DiskLoc& loc );
        void _changeNumRecords(OperationContext* txn, bool insert);
        void _increaseDataSize(OperationContext* txn, int amount);

        rocksdb::DB* _db; // not owned
        boost::shared_ptr<rocksdb::ColumnFamilyHandle> _columnFamily;

        const bool _isCapped;
        const int64_t _cappedMaxSize;
        const int64_t _cappedMaxDocs;
        CappedDocumentDeleteCallback* _cappedDeleteCallback;

        AtomicUInt64 _nextIdNum;
        long long _dataSize;
        std::atomic<long long> _numRecords;

        const string _dataSizeKey;
        const string _numRecordsKey;

        // locks
        boost::mutex _dataSizeLock;
    };
}
