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

#include <string>

#include "mongo/db/structure/record_store.h"

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
        RocksRecordStore( const StringData& ns,
                          rocksdb::DB* db,
                          rocksdb::ColumnFamilyHandle* columnFamily );

        virtual ~RocksRecordStore();

        // name of the RecordStore implementation
        virtual const char* name() const { return "rocks"; }

        virtual long long dataSize() const;

        virtual long long numRecords() const;

        virtual bool isCapped() const;

        virtual int64_t storageSize( BSONObjBuilder* extraInfo = NULL, int infoLevel = 0 ) const;

        // CRUD related

        virtual RecordData dataFor( const DiskLoc& loc) const;

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
                                          const char* damangeSource,
                                          const mutablebson::DamageVector& damages );

        virtual RecordIterator* getIterator( const DiskLoc& start = DiskLoc(),
                                             bool tailable = false,
                                             const CollectionScanParams::Direction& dir =
                                             CollectionScanParams::FORWARD
                                             ) const;

        virtual RecordIterator* getIteratorForRepair() const;

        virtual std::vector<RecordIterator*> getManyIterators() const;

        virtual Status truncate( OperationContext* txn );

        virtual bool compactSupported() const { return false; }

        virtual Status compact( OperationContext* txn,
                                RecordStoreCompactAdaptor* adaptor,
                                const CompactOptions* options,
                                CompactStats* stats );

        virtual Status validate( OperationContext* txn,
                                 bool full, bool scanData,
                                 ValidateAdaptor* adaptor,
                                 ValidateResults* results, BSONObjBuilder* output ) const;

        virtual void appendCustomStats( BSONObjBuilder* result, double scale ) const;

        virtual Status touch( OperationContext* txn, BSONObjBuilder* output ) const;

        virtual Status setCustomOption( OperationContext* txn,
                                        const BSONElement& option,
                                        BSONObjBuilder* info = NULL );

        virtual void temp_cappedTruncateAfter(OperationContext* txn,
                                              DiskLoc end,
                                              bool inclusive);
    private:

        class Iterator : public RecordIterator {
        public:
            Iterator( const RocksRecordStore* rs, const CollectionScanParams::Direction& dir );

            virtual bool isEOF();
            virtual DiskLoc curr();
            virtual DiskLoc getNext();
            virtual void invalidate(const DiskLoc& dl);
            virtual void prepareToYield();
            virtual bool recoverFromYield();
            virtual RecordData dataFor( const DiskLoc& loc ) const;

        private:
            bool _forward() const;
            void _checkStatus();

            const RocksRecordStore* _rs;
            CollectionScanParams::Direction _dir;
            boost::scoped_ptr<rocksdb::Iterator> _iterator;
        };

        RocksRecoveryUnit* _getRecoveryUnit( OperationContext* opCtx ) const;

        DiskLoc _nextId();
        rocksdb::Slice _makeKey( const DiskLoc& loc ) const;

        rocksdb::DB* _db; // not owned
        rocksdb::ColumnFamilyHandle* _columnFamily; // not owned

        int _tempIncrement;
    };
}
