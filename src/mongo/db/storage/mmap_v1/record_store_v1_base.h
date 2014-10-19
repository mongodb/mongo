// record_store_v1_base.h

/**
*    Copyright (C) 2013-2014 MongoDB Inc.
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

#include "mongo/db/diskloc.h"
#include "mongo/db/storage/record_store.h"

namespace mongo {

    class DeletedRecord;
    class DocWriter;
    class ExtentManager;
    class Record;
    class OperationContext;

    struct Extent;

    class RecordStoreV1MetaData {
    public:
        virtual ~RecordStoreV1MetaData(){}

        virtual const DiskLoc& capExtent() const = 0;
        virtual void setCapExtent( OperationContext* txn, const DiskLoc& loc ) = 0;

        virtual const DiskLoc& capFirstNewRecord() const = 0;
        virtual void setCapFirstNewRecord( OperationContext* txn, const DiskLoc& loc ) = 0;

        bool capLooped() const { return capFirstNewRecord().isValid(); }

        virtual long long dataSize() const = 0;
        virtual long long numRecords() const = 0;

        virtual void incrementStats( OperationContext* txn,
                                     long long dataSizeIncrement,
                                     long long numRecordsIncrement ) = 0;

        virtual void setStats( OperationContext* txn,
                               long long dataSize,
                               long long numRecords ) = 0;

        virtual DiskLoc deletedListEntry( int bucket ) const = 0;
        virtual void setDeletedListEntry( OperationContext* txn,
                                          int bucket,
                                          const DiskLoc& loc ) = 0;

        virtual DiskLoc deletedListLegacyGrabBag() const = 0;
        virtual void setDeletedListLegacyGrabBag(OperationContext* txn, const DiskLoc& loc) = 0;

        virtual void orphanDeletedList(OperationContext* txn) = 0;

        virtual const DiskLoc& firstExtent( OperationContext* txn ) const = 0;
        virtual void setFirstExtent( OperationContext* txn, const DiskLoc& loc ) = 0;

        virtual const DiskLoc& lastExtent( OperationContext* txn ) const = 0;
        virtual void setLastExtent( OperationContext* txn, const DiskLoc& loc ) = 0;

        virtual bool isCapped() const = 0;

        virtual bool isUserFlagSet( int flag ) const = 0;
        virtual int userFlags() const = 0;
        virtual bool setUserFlag( OperationContext* txn, int flag ) = 0;
        virtual bool clearUserFlag( OperationContext* txn, int flag ) = 0;
        virtual bool replaceUserFlags( OperationContext* txn, int flags ) = 0;

        virtual int lastExtentSize( OperationContext* txn) const = 0;
        virtual void setLastExtentSize( OperationContext* txn, int newMax ) = 0;

        virtual long long maxCappedDocs() const = 0;

    };

    class RecordStoreV1Base : public RecordStore {
    public:

        static const int Buckets = 26;
        static const int MaxAllowedAllocation = 16*1024*1024 + 512*1024;

        static const int bucketSizes[];

        enum UserFlags {
            Flag_UsePowerOf2Sizes = 1 << 0,
            Flag_NoPadding = 1 << 1,
        };

        // ------------

        class IntraExtentIterator;

        /**
         * @param details - takes ownership
         * @param em - does NOT take ownership
         */
        RecordStoreV1Base( const StringData& ns,
                           RecordStoreV1MetaData* details,
                           ExtentManager* em,
                           bool isSystemIndexes );

        virtual ~RecordStoreV1Base();

        virtual long long dataSize( OperationContext* txn ) const { return _details->dataSize(); }
        virtual long long numRecords( OperationContext* txn ) const { return _details->numRecords(); }

        virtual int64_t storageSize( OperationContext* txn,
                                     BSONObjBuilder* extraInfo = NULL,
                                     int level = 0 ) const;

        virtual RecordData dataFor( OperationContext* txn, const DiskLoc& loc ) const;

        virtual bool findRecord( OperationContext* txn, const DiskLoc& loc, RecordData* rd ) const;

        void deleteRecord( OperationContext* txn,
                           const DiskLoc& dl );

        StatusWith<DiskLoc> insertRecord( OperationContext* txn,
                                          const char* data,
                                          int len,
                                          bool enforceQuota );

        StatusWith<DiskLoc> insertRecord( OperationContext* txn,
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

        virtual RecordIterator* getIteratorForRepair( OperationContext* txn ) const;

        void increaseStorageSize( OperationContext* txn, int size, bool enforceQuota );

        virtual Status validate( OperationContext* txn,
                                 bool full, bool scanData,
                                 ValidateAdaptor* adaptor,
                                 ValidateResults* results, BSONObjBuilder* output ) const;

        virtual void appendCustomStats( OperationContext* txn,
                                        BSONObjBuilder* result,
                                        double scale ) const;

        virtual Status touch( OperationContext* txn, BSONObjBuilder* output ) const;

        const RecordStoreV1MetaData* details() const { return _details.get(); }

        DiskLoc getExtentLocForRecord( OperationContext* txn, const DiskLoc& loc ) const;

        DiskLoc getNextRecord( OperationContext* txn, const DiskLoc& loc ) const;
        DiskLoc getPrevRecord( OperationContext* txn, const DiskLoc& loc ) const;

        DiskLoc getNextRecordInExtent( OperationContext* txn, const DiskLoc& loc ) const;
        DiskLoc getPrevRecordInExtent( OperationContext* txn, const DiskLoc& loc ) const;

        /**
         * Quantize 'minSize' to the nearest allocation size.
         */
        static int quantizeAllocationSpace(int minSize);

        static bool isQuantized(int recordSize);

        /* return which "deleted bucket" for this size object */
        static int bucket(int size);

        virtual Status setCustomOption( OperationContext* txn,
                                        const BSONElement& option,
                                        BSONObjBuilder* info = NULL );
    protected:

        virtual Record* recordFor( const DiskLoc& loc ) const;

        const DeletedRecord* deletedRecordFor( const DiskLoc& loc ) const;

        virtual bool isCapped() const = 0;

        virtual bool shouldPadInserts() const = 0;

        virtual StatusWith<DiskLoc> allocRecord( OperationContext* txn,
                                                 int lengthWithHeaders,
                                                 bool enforceQuota ) = 0;

        // TODO: document, remove, what have you
        virtual void addDeletedRec( OperationContext* txn, const DiskLoc& dloc) = 0;

        // TODO: another sad one
        virtual DeletedRecord* drec( const DiskLoc& loc ) const;

        // just a wrapper for _extentManager->getExtent( loc );
        Extent* _getExtent( OperationContext* txn, const DiskLoc& loc ) const;

        DiskLoc _getExtentLocForRecord( OperationContext* txn, const DiskLoc& loc ) const;

        DiskLoc _getNextRecord( OperationContext* txn, const DiskLoc& loc ) const;
        DiskLoc _getPrevRecord( OperationContext* txn, const DiskLoc& loc ) const;

        DiskLoc _getNextRecordInExtent( OperationContext* txn, const DiskLoc& loc ) const;
        DiskLoc _getPrevRecordInExtent( OperationContext* txn, const DiskLoc& loc ) const;

        /**
         * finds the first suitable DiskLoc for data
         * will return the DiskLoc of a newly created DeletedRecord
         */
        DiskLoc _findFirstSpot( OperationContext* txn, const DiskLoc& extDiskLoc, Extent* e );

        /** add a record to the end of the linked list chain within this extent.
            require: you must have already declared write intent for the record header.
        */
        void _addRecordToRecListInExtent(OperationContext* txn, Record* r, DiskLoc loc);

        /**
         * internal
         * doesn't check inputs or change padding
         */
        StatusWith<DiskLoc> _insertRecord( OperationContext* txn,
                                           const char* data,
                                           int len,
                                           bool enforceQuota );

        scoped_ptr<RecordStoreV1MetaData> _details;
        ExtentManager* _extentManager;
        bool _isSystemIndexes;

        friend class RecordStoreV1RepairIterator;
    };

    /**
     * Iterates over all records within a single extent.
     *
     * EOF at end of extent, even if there are more extents.
     */
    class RecordStoreV1Base::IntraExtentIterator : public RecordIterator {
    public:
        IntraExtentIterator(OperationContext* txn,
                            DiskLoc start,
                            const RecordStoreV1Base* rs,
                            bool forward = true)
            : _txn(txn), _curr(start), _rs(rs), _forward(forward) {}

        virtual bool isEOF() { return _curr.isNull(); }

        virtual DiskLoc curr() { return _curr; }

        virtual DiskLoc getNext( );

        virtual void invalidate(const DiskLoc& dl);

        virtual void saveState() {}

        virtual bool restoreState(OperationContext* txn) { return true; }

        virtual RecordData dataFor( const DiskLoc& loc ) const { return _rs->dataFor(_txn, loc); }

    private:
        virtual const Record* recordFor( const DiskLoc& loc ) const { return _rs->recordFor(loc); }
        OperationContext* _txn;
        DiskLoc _curr;
        const RecordStoreV1Base* _rs;
        bool _forward;
    };

}
