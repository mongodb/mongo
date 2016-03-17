// extent_manager.h

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

#include <memory>
#include <string>
#include <vector>

#include "mongo/base/status.h"
#include "mongo/base/string_data.h"
#include "mongo/db/storage/mmap_v1/diskloc.h"

namespace mongo {

class DataFile;
class DataFileVersion;
class MmapV1RecordHeader;
class RecordFetcher;
class OperationContext;

struct Extent;

/**
 * ExtentManager basics
 *  - one per database
 *  - responsible for managing <db>.# files
 *  - NOT responsible for .ns file
 *  - gives out extents
 *  - responsible for figuring out how to get a new extent
 *  - can use any method it wants to do so
 *  - this structure is NOT stored on disk
 *  - files will not be removed from the EM
 *  - extent size and loc are immutable
 *  - this class is thread safe, once constructed and init()-ialized
 */
class ExtentManager {
    MONGO_DISALLOW_COPYING(ExtentManager);

public:
    ExtentManager() {}

    class Factory {
    public:
        virtual ~Factory() = default;
        virtual std::unique_ptr<ExtentManager> create(StringData dbname,
                                                      StringData path,
                                                      bool directoryPerDB) = 0;
    };

    virtual ~ExtentManager() {}

    /**
     * opens all current files
     */
    virtual Status init(OperationContext* txn) = 0;

    virtual int numFiles() const = 0;
    virtual long long fileSize() const = 0;

    // must call Extent::reuse on the returned extent
    virtual DiskLoc allocateExtent(OperationContext* txn,
                                   bool capped,
                                   int size,
                                   bool enforceQuota) = 0;

    /**
     * firstExt has to be == lastExt or a chain
     */
    virtual void freeExtents(OperationContext* txn, DiskLoc firstExt, DiskLoc lastExt) = 0;

    /**
     * frees a single extent
     * ignores all fields in the Extent except: magic, myLoc, length
     */
    virtual void freeExtent(OperationContext* txn, DiskLoc extent) = 0;

    /**
     * Retrieve statistics on the the free list managed by this ExtentManger.
     * @param numExtents - non-null pointer to an int that will receive the number of extents
     * @param totalFreeSizeBytes - non-null pointer to an int64_t receiving the total free
     *                             space in the free list.
     */
    virtual void freeListStats(OperationContext* txn,
                               int* numExtents,
                               int64_t* totalFreeSizeBytes) const = 0;

    /**
     * @param loc - has to be for a specific MmapV1RecordHeader
     * Note(erh): this sadly cannot be removed.
     * A MmapV1RecordHeader DiskLoc has an offset from a file, while a RecordStore really wants an
     * offset from an extent.  This intrinsically links an original record store to the original
     * extent manager.
     */
    virtual MmapV1RecordHeader* recordForV1(const DiskLoc& loc) const = 0;

    /**
     * The extent manager tracks accesses to DiskLocs. This returns non-NULL if the DiskLoc has
     * been recently accessed, and therefore has likely been paged into physical memory.
     * Returns nullptr if the DiskLoc is Null.
     *
     */
    virtual std::unique_ptr<RecordFetcher> recordNeedsFetch(const DiskLoc& loc) const = 0;

    /**
     * @param loc - has to be for a specific MmapV1RecordHeader (not an Extent)
     * Note(erh) see comment on recordFor
     */
    virtual Extent* extentForV1(const DiskLoc& loc) const = 0;

    /**
     * @param loc - has to be for a specific MmapV1RecordHeader (not an Extent)
     * Note(erh) see comment on recordFor
     */
    virtual DiskLoc extentLocForV1(const DiskLoc& loc) const = 0;

    /**
     * @param loc - has to be for a specific Extent
     */
    virtual Extent* getExtent(const DiskLoc& loc, bool doSanityCheck = true) const = 0;

    /**
     * @return maximum size of an Extent
     */
    virtual int maxSize() const = 0;

    /**
     * @return minimum size of an Extent
     */
    virtual int minSize() const {
        return 0x1000;
    }

    /**
     * @param recordLen length of record we need
     * @param lastExt size of last extent which is a factor in next extent size
     */
    virtual int followupSize(int recordLen, int lastExtentLen) const;

    /** get a suggested size for the first extent in a namespace
     *  @param recordLen length of record we need to insert
     */
    virtual int initialSize(int recordLen) const;

    /**
     * quantizes extent size to >= min + page boundary
     */
    virtual int quantizeExtentSize(int size) const;

    // see cacheHint methods
    enum HintType { Sequential, Random };
    class CacheHint {
    public:
        virtual ~CacheHint() {}
    };
    /**
     * Tell the system that for this extent, it will have this kind of disk access.
     * Caller takes owernship of CacheHint
     */
    virtual CacheHint* cacheHint(const DiskLoc& extentLoc, const HintType& hint) = 0;

    virtual DataFileVersion getFileFormat(OperationContext* txn) const = 0;
    virtual void setFileFormat(OperationContext* txn, DataFileVersion newVersion) = 0;

    virtual const DataFile* getOpenFile(int n) const = 0;
};

}  // namespace mongo
