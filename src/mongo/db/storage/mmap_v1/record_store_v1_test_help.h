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

#include "mongo/db/storage/mmap_v1/data_file.h"
#include "mongo/db/storage/mmap_v1/extent_manager.h"
#include "mongo/db/storage/mmap_v1/record_store_v1_base.h"

namespace mongo {

class DummyRecordStoreV1MetaData : public RecordStoreV1MetaData {
public:
    DummyRecordStoreV1MetaData(bool capped, int userFlags);
    virtual ~DummyRecordStoreV1MetaData() {}

    virtual const DiskLoc& capExtent() const;
    virtual void setCapExtent(OperationContext* txn, const DiskLoc& loc);

    virtual const DiskLoc& capFirstNewRecord() const;
    virtual void setCapFirstNewRecord(OperationContext* txn, const DiskLoc& loc);

    virtual long long dataSize() const;
    virtual long long numRecords() const;

    virtual void incrementStats(OperationContext* txn,
                                long long dataSizeIncrement,
                                long long numRecordsIncrement);

    virtual void setStats(OperationContext* txn, long long dataSize, long long numRecords);

    virtual DiskLoc deletedListEntry(int bucket) const;
    virtual void setDeletedListEntry(OperationContext* txn, int bucket, const DiskLoc& loc);

    virtual DiskLoc deletedListLegacyGrabBag() const;
    virtual void setDeletedListLegacyGrabBag(OperationContext* txn, const DiskLoc& loc);

    virtual void orphanDeletedList(OperationContext* txn);

    virtual const DiskLoc& firstExtent(OperationContext* txn) const;
    virtual void setFirstExtent(OperationContext* txn, const DiskLoc& loc);

    virtual const DiskLoc& lastExtent(OperationContext* txn) const;
    virtual void setLastExtent(OperationContext* txn, const DiskLoc& loc);

    virtual bool isCapped() const;

    virtual bool isUserFlagSet(int flag) const;
    virtual int userFlags() const {
        return _userFlags;
    }
    virtual bool setUserFlag(OperationContext* txn, int flag);
    virtual bool clearUserFlag(OperationContext* txn, int flag);
    virtual bool replaceUserFlags(OperationContext* txn, int flags);


    virtual int lastExtentSize(OperationContext* txn) const;
    virtual void setLastExtentSize(OperationContext* txn, int newMax);

    virtual long long maxCappedDocs() const;

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
    DiskLoc _deletedListLegacyGrabBag;
};

class DummyExtentManager : public ExtentManager {
public:
    virtual ~DummyExtentManager();

    virtual Status init(OperationContext* txn);

    virtual int numFiles() const;
    virtual long long fileSize() const;

    virtual DiskLoc allocateExtent(OperationContext* txn, bool capped, int size, bool enforceQuota);

    virtual void freeExtents(OperationContext* txn, DiskLoc firstExt, DiskLoc lastExt);

    virtual void freeExtent(OperationContext* txn, DiskLoc extent);

    virtual void freeListStats(OperationContext* txn,
                               int* numExtents,
                               int64_t* totalFreeSizeBytes) const;

    virtual MmapV1RecordHeader* recordForV1(const DiskLoc& loc) const;

    virtual std::unique_ptr<RecordFetcher> recordNeedsFetch(const DiskLoc& loc) const final;

    virtual Extent* extentForV1(const DiskLoc& loc) const;

    virtual DiskLoc extentLocForV1(const DiskLoc& loc) const;

    virtual Extent* getExtent(const DiskLoc& loc, bool doSanityCheck = true) const;

    virtual int maxSize() const;

    virtual CacheHint* cacheHint(const DiskLoc& extentLoc, const HintType& hint);

    DataFileVersion getFileFormat(OperationContext* txn) const final;

    virtual void setFileFormat(OperationContext* txn, DataFileVersion newVersion) final;

    const DataFile* getOpenFile(int n) const final;


protected:
    struct ExtentInfo {
        char* data;
        size_t length;
    };

    std::vector<ExtentInfo> _extents;
};

struct LocAndSize {
    DiskLoc loc;
    int size;  // with headers
};

/**
 * Creates a V1 storage/mmap_v1 with the passed in records and DeletedRecords (drecs).
 *
 * List of LocAndSize are terminated by a Null DiskLoc. Passing a NULL pointer is shorthand for
 * an empty list. Each extent gets it's own DiskLoc file number. DiskLoc Offsets must be > 1000.
 *
 * records must be sorted by extent/file. offsets within an extent can be in any order.
 *
 * In a simple RS, drecs must be grouped into size-buckets, but the ordering within the size
 * buckets is up to you.
 *
 * In a capped collection, all drecs form a single list and must be grouped by extent, with each
 * extent having at least one drec. capFirstNewRecord() and capExtent() *must* be correctly set
 * on md before calling.
 *
 * You are responsible for ensuring the records and drecs don't overlap.
 *
 * ExtentManager and MetaData must both be empty.
 */
void initializeV1RS(OperationContext* txn,
                    const LocAndSize* records,
                    const LocAndSize* drecs,
                    const LocAndSize* legacyGrabBag,
                    DummyExtentManager* em,
                    DummyRecordStoreV1MetaData* md);

/**
 * Asserts that the V1RecordStore defined by md has the passed in records and drecs in the
 * correct order.
 *
 * List of LocAndSize are terminated by a Null DiskLoc. Passing a NULL pointer means don't check
 * that list.
 */
void assertStateV1RS(OperationContext* txn,
                     const LocAndSize* records,
                     const LocAndSize* drecs,
                     const LocAndSize* legacyGrabBag,
                     const ExtentManager* em,
                     const DummyRecordStoreV1MetaData* md);

}  // namespace mongo
