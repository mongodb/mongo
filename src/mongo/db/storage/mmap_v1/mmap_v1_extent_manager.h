// mmap_v1_extent_manager.h

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

#include <string>

#include <boost/filesystem/path.hpp>

#include "mongo/platform/atomic_word.h"
#include "mongo/base/status.h"
#include "mongo/base/string_data.h"
#include "mongo/db/concurrency/lock_mgr_defs.h"
#include "mongo/db/diskloc.h"
#include "mongo/db/storage/mmap_v1/extent_manager.h"
#include "mongo/util/concurrency/mutex.h"

namespace mongo {

    class DataFile;
    class Record;
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
     *  - this class is thread safe, except as indicated below
     *
     * Implementation:
     *  - ExtentManager holds a preallocated list of DataFile
     *  - files will not be removed from the EM, so _files access can be lock-free
     *  - extent size and loc are immutable
     *  - Any non-const public operations on an ExtentManager will acquire an MODE_X lock on its
     *    RESOURCE_MMAPv1_EXTENT_MANAGER resource from the lock-manager, which will extend life
     *    to during WriteUnitOfWorks that might need rollback. Private methods will only
     *    be called from public ones.
     */
    class MmapV1ExtentManager : public ExtentManager {
        MONGO_DISALLOW_COPYING( MmapV1ExtentManager );
    public:
        /**
         * @param freeListDetails this is a reference into the .ns file
         *        while a bit odd, this is not a layer violation as extents
         *        are a peer to the .ns file, without any layering
         */
        MmapV1ExtentManager(const StringData& dbname, const StringData& path,
                            bool directoryPerDB);

        /**
         * opens all current files, not thread safe
         */
        Status init(OperationContext* txn);

        int numFiles() const;
        long long fileSize() const;

        // must call Extent::reuse on the returned extent
        DiskLoc allocateExtent( OperationContext* txn,
                                bool capped,
                                int size,
                                bool enforceQuota );

        /**
         * firstExt has to be == lastExt or a chain
         */
        void freeExtents( OperationContext* txn, DiskLoc firstExt, DiskLoc lastExt );

        /**
         * frees a single extent
         * ignores all fields in the Extent except: magic, myLoc, length
         */
        void freeExtent( OperationContext* txn, DiskLoc extent );

        // For debug only: not thread safe
        void printFreeList() const;

        void freeListStats(OperationContext* txn,
                           int* numExtents,
                           int64_t* totalFreeSizeBytes) const;

        /**
         * @param loc - has to be for a specific Record
         * Note(erh): this sadly cannot be removed.
         * A Record DiskLoc has an offset from a file, while a RecordStore really wants an offset
         * from an extent.  This intrinsically links an original record store to the original extent
         * manager.
         */
        Record* recordForV1( const DiskLoc& loc ) const;

        /**
         * @param loc - has to be for a specific Record (not an Extent)
         * Note(erh) see comment on recordFor
         */
        Extent* extentForV1( const DiskLoc& loc ) const;

        /**
         * @param loc - has to be for a specific Record (not an Extent)
         * Note(erh) see comment on recordFor
         */
        DiskLoc extentLocForV1( const DiskLoc& loc ) const;

        /**
         * @param loc - has to be for a specific Extent
         */
        Extent* getExtent( const DiskLoc& loc, bool doSanityCheck = true ) const;

        /**
         * Not thread safe, requires a database exclusive lock
         */
        void getFileFormat(OperationContext* txn, int* major, int* minor) const;
        void setFileFormat(OperationContext* txn, int major, int minor);

        const DataFile* getOpenFile( int n ) const { return _getOpenFile( n ); }

        virtual int maxSize() const;

        virtual CacheHint* cacheHint( const DiskLoc& extentLoc, const HintType& hint );

    private:
        /**
         * will return NULL if nothing suitable in free list
         */
        DiskLoc _allocFromFreeList( OperationContext* txn, int approxSize, bool capped );

        /* allocate a new Extent, does not check free list
        */
        DiskLoc _createExtent( OperationContext* txn, int approxSize, bool enforceQuota );

        DataFile* _addAFile( OperationContext* txn, int sizeNeeded, bool preallocateNextFile );

        DiskLoc _getFreeListStart() const;
        DiskLoc _getFreeListEnd() const;
        void _setFreeListStart( OperationContext* txn, DiskLoc loc );
        void _setFreeListEnd( OperationContext* txn, DiskLoc loc );

        const DataFile* _getOpenFile(int fileId) const;
        DataFile* _getOpenFile(int fileId);

        DiskLoc _createExtentInFile( OperationContext* txn,
                                     int fileNo,
                                     DataFile* f,
                                     int size,
                                     bool enforceQuota );

        boost::filesystem::path fileName( int n ) const;

// -----

        const std::string _dbname; // i.e. "test"
        const std::string _path; // i.e. "/data/db"
        const bool _directoryPerDB;
        const ResourceId _rid;

        /**
         * Simple wrapper around an array object to allow append-only modification of the array,
         * as well as concurrent read-accesses. This class has a minimal interface to keep
         * implementation simple and easy to modify.
         */
        class FilesArray {
        public:
            FilesArray() : _writersMutex("MmapV1ExtentManager"), _size(0) { }
            ~FilesArray();

            /**
             * Returns file at location 'n' in the array, with 'n' less than number of files added.
             * Will always return the same pointer for a given file.
             */
            DataFile* operator[](int n) const {
                invariant(n >= 0 && n < size());
                return _files[n];
            }

            /**
             * Returns true iff no files were added
             */
            bool empty() const {
                return size() == 0;
            }

            /**
             * Returns number of files added to the array
             */
            int size() const {
                return _size.load();
            }

            // Appends val to the array, taking ownership of its pointer
            void push_back(DataFile* val);

        private:
            mutex _writersMutex;
            AtomicInt32 _size; // number of files in the array
            DataFile* _files[DiskLoc::MaxFiles];
        };

        FilesArray _files;
    };
}
