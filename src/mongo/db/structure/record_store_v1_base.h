// record_store_v1_base.h

/**
*    Copyright (C) 2013 10gen Inc.
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
#include "mongo/db/structure/record_store.h"

namespace mongo {

    class DocWriter;
    class ExtentManager;
    class Record;
    class TransactionExperiment;

    class RecordStoreV1MetaData {
    public:
        virtual ~RecordStoreV1MetaData(){}

        virtual int bucket(int size) const = 0;

        virtual const DiskLoc& capExtent() const = 0;
        virtual void setCapExtent( TransactionExperiment* txn, const DiskLoc& loc ) = 0;

        virtual const DiskLoc& capFirstNewRecord() const = 0;
        virtual void setCapFirstNewRecord( TransactionExperiment* txn, const DiskLoc& loc ) = 0;

        virtual bool capLooped() const = 0;
        virtual void clearSystemFlags(TransactionExperiment* txn) = 0;

        virtual long long dataSize() const = 0;
        virtual long long numRecords() const = 0;

        virtual void incrementStats( TransactionExperiment* txn,
                                     long long dataSizeIncrement,
                                     long long numRecordsIncrement ) = 0;

        virtual void setStats( TransactionExperiment* txn,
                               long long dataSizeIncrement,
                               long long numRecordsIncrement ) = 0;

        virtual const DiskLoc& deletedListEntry( int bucket ) const = 0;
        virtual void setDeletedListEntry( TransactionExperiment* txn,
                                          int bucket,
                                          const DiskLoc& loc ) = 0;
        virtual void orphanDeletedList(TransactionExperiment* txn) = 0;

        virtual const DiskLoc& firstExtent() const = 0;
        virtual void setFirstExtent( TransactionExperiment* txn, const DiskLoc& loc ) = 0;

        virtual const DiskLoc& lastExtent() const = 0;
        virtual void setLastExtent( TransactionExperiment* txn, const DiskLoc& loc ) = 0;

        virtual bool isCapped() const = 0;

        virtual bool isUserFlagSet( int flag ) const = 0;

        virtual int lastExtentSize() const = 0;
        virtual void setLastExtentSize( TransactionExperiment* txn, int newMax ) = 0;

        virtual long long maxCappedDocs() const = 0;

        virtual double paddingFactor() const = 0;

        virtual void setPaddingFactor( TransactionExperiment* txn, double paddingFactor ) = 0;

        virtual int quantizePowerOf2AllocationSpace(int allocSize) const = 0;

    };

    class RecordStoreV1Base : public RecordStore {
    public:
        class IntraExtentIterator;

        RecordStoreV1Base( const StringData& ns,
                           RecordStoreV1MetaData* details,
                           ExtentManager* em,
                           bool isSystemIndexes );

        virtual ~RecordStoreV1Base();

        Record* recordFor( const DiskLoc& loc ) const;

        void deleteRecord( TransactionExperiment* txn,
                           const DiskLoc& dl );

        StatusWith<DiskLoc> insertRecord( TransactionExperiment* txn,
                                          const char* data,
                                          int len,
                                          int quotaMax );

        StatusWith<DiskLoc> insertRecord( TransactionExperiment* txn,
                                          const DocWriter* doc,
                                          int quotaMax );

        virtual RecordIterator* getIteratorForRepair() const;

        void increaseStorageSize( TransactionExperiment* txn, int size, int quotaMax );

        virtual Status validate( TransactionExperiment* txn,
                                 bool full, bool scanData,
                                 ValidateAdaptor* adaptor,
                                 ValidateResults* results, BSONObjBuilder* output ) const;

        virtual Status touch( TransactionExperiment* txn, BSONObjBuilder* output ) const;

        // TODO: another sad one
        virtual const DeletedRecord* deletedRecordFor( const DiskLoc& loc ) const;

        const RecordStoreV1MetaData* details() const { return _details.get(); }

        /**
         * @return the actual size to create
         *         will be >= oldRecordSize
         *         based on padding and any other flags
         */
        int getRecordAllocationSize( int minRecordSize ) const;

        DiskLoc getExtentLocForRecord( const DiskLoc& loc ) const;

        DiskLoc getNextRecord( const DiskLoc& loc ) const;
        DiskLoc getPrevRecord( const DiskLoc& loc ) const;

        DiskLoc getNextRecordInExtent( const DiskLoc& loc ) const;
        DiskLoc getPrevRecordInExtent( const DiskLoc& loc ) const;

    protected:

        virtual bool isCapped() const = 0;

        virtual StatusWith<DiskLoc> allocRecord( TransactionExperiment* txn,
                                                 int lengthWithHeaders,
                                                 int quotaMax ) = 0;

        // TODO: document, remove, what have you
        virtual void addDeletedRec( TransactionExperiment* txn, const DiskLoc& dloc) = 0;

        // TODO: another sad one
        virtual DeletedRecord* drec( const DiskLoc& loc ) const;

        // just a wrapper for _extentManager->getExtent( loc );
        Extent* _getExtent( const DiskLoc& loc ) const;

        DiskLoc _getExtentLocForRecord( const DiskLoc& loc ) const;

        DiskLoc _getNextRecord( const DiskLoc& loc ) const;
        DiskLoc _getPrevRecord( const DiskLoc& loc ) const;

        DiskLoc _getNextRecordInExtent( const DiskLoc& loc ) const;
        DiskLoc _getPrevRecordInExtent( const DiskLoc& loc ) const;

        /** add a record to the end of the linked list chain within this extent.
            require: you must have already declared write intent for the record header.
        */
        void _addRecordToRecListInExtent(TransactionExperiment* txn, Record* r, DiskLoc loc);

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
        IntraExtentIterator(DiskLoc start, const RecordStore* rs, bool forward = true)
            : _curr(start), _rs(rs), _forward(forward) {}

        virtual bool isEOF() { return _curr.isNull(); }

        virtual DiskLoc curr() { return _curr; }

        virtual DiskLoc getNext();

        virtual void invalidate(const DiskLoc& dl);

        virtual void prepareToYield() {}

        virtual bool recoverFromYield() { return true; }

        virtual const Record* recordFor( const DiskLoc& loc ) const { return _rs->recordFor(loc); }

    private:
        DiskLoc _curr;
        const RecordStore* _rs;
        bool _forward;
    };

}
