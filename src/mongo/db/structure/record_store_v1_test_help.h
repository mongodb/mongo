// record_store_v1_test_help.h

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

#include <vector>

#include "mongo/db/storage/extent_manager.h"
#include "mongo/db/storage/transaction.h"
#include "mongo/db/structure/record_store_v1_base.h"

namespace mongo {

    class DummyTransactionExperiment : public TransactionExperiment {
    public:
        virtual ~DummyTransactionExperiment(){}

        virtual bool commitIfNeeded(bool force = false);

        virtual bool isCommitNeeded() const;

        virtual ProgressMeter* setMessage(const char* msg,
                                          const std::string& name ,
                                          unsigned long long progressMeterTotal,
                                          int secondsBetween);

        virtual void* writingPtr(void* data, size_t len);

        virtual void createdFile(const std::string& filename, unsigned long long len);

        virtual void syncDataAndTruncateJournal();

        virtual void checkForInterrupt(bool heedMutex = true) const;

        virtual Status checkForInterruptNoAssert() const;

    };

    class DummyRecordStoreV1MetaData : public RecordStoreV1MetaData {
    public:
        DummyRecordStoreV1MetaData( bool capped, int userFlags );
        virtual ~DummyRecordStoreV1MetaData(){}

        virtual const DiskLoc& capExtent() const;
        virtual void setCapExtent( TransactionExperiment* txn, const DiskLoc& loc );

        virtual const DiskLoc& capFirstNewRecord() const;
        virtual void setCapFirstNewRecord( TransactionExperiment* txn, const DiskLoc& loc );

        virtual bool capLooped() const;

        virtual long long dataSize() const;
        virtual long long numRecords() const;

        virtual void incrementStats( TransactionExperiment* txn,
                                     long long dataSizeIncrement,
                                     long long numRecordsIncrement );

        virtual void setStats( TransactionExperiment* txn,
                               long long dataSizeIncrement,
                               long long numRecordsIncrement );

        virtual const DiskLoc& deletedListEntry( int bucket ) const;
        virtual void setDeletedListEntry( TransactionExperiment* txn,
                                          int bucket,
                                          const DiskLoc& loc );
        virtual void orphanDeletedList(TransactionExperiment* txn);

        virtual const DiskLoc& firstExtent() const;
        virtual void setFirstExtent( TransactionExperiment* txn, const DiskLoc& loc );

        virtual const DiskLoc& lastExtent() const;
        virtual void setLastExtent( TransactionExperiment* txn, const DiskLoc& loc );

        virtual bool isCapped() const;

        virtual bool isUserFlagSet( int flag ) const;

        virtual int lastExtentSize() const;
        virtual void setLastExtentSize( TransactionExperiment* txn, int newMax );

        virtual long long maxCappedDocs() const;

        virtual double paddingFactor() const;

        virtual void setPaddingFactor( TransactionExperiment* txn, double paddingFactor );

    protected:

        DiskLoc _capExtent;
        DiskLoc _capFirstNewRecord;

        long long _dataSize;
        long long _numRecords;

        DiskLoc _firstExtent;
        DiskLoc _lastExtent;

        bool _capped;
        int _userFlags;
        long long _maxCappedDocs;

        int _lastExtentSize;
        double _paddingFactor;

        std::vector<DiskLoc> _deletedLists;
    };

    class DummyExtentManager : public ExtentManager {
    public:
        virtual ~DummyExtentManager();

        virtual Status init(TransactionExperiment* txn);

        virtual size_t numFiles() const;
        virtual long long fileSize() const;

        virtual void flushFiles( bool sync );

        virtual DiskLoc allocateExtent( TransactionExperiment* txn,
                                        bool capped,
                                        int size,
                                        int quotaMax );

        virtual void freeExtents( TransactionExperiment* txn,
                                  DiskLoc firstExt, DiskLoc lastExt );

        virtual void freeExtent( TransactionExperiment* txn, DiskLoc extent );

        virtual void freeListStats( int* numExtents, int64_t* totalFreeSize ) const;

        virtual Record* recordForV1( const DiskLoc& loc ) const;

        virtual Extent* extentForV1( const DiskLoc& loc ) const;

        virtual DiskLoc extentLocForV1( const DiskLoc& loc ) const;

        virtual Extent* getExtent( const DiskLoc& loc, bool doSanityCheck = true ) const;

        virtual int maxSize() const;

        virtual CacheHint* cacheHint( const DiskLoc& extentLoc, const HintType& hint );

    protected:
        struct ExtentInfo {
            char* data;
            size_t length;
        };

        std::vector<ExtentInfo> _extents;
    };

}
