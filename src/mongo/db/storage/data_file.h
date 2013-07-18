// data_file.h

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
*/

#pragma once

#include "mongo/db/diskloc.h"
#include "mongo/db/pdfile_version.h"
#include "mongo/db/storage/durable_mapped_file.h"

namespace mongo {

    /*  a datafile - i.e. the "dbname.<#>" files :

          ----------------------
          DataFileHeader
          ----------------------
          Extent (for a particular namespace)
            Record
            ...
            Record (some chained for unused space)
          ----------------------
          more Extents...
          ----------------------
    */
#pragma pack(1)
    class DataFileHeader {
    public:
        int version;
        int versionMinor;
        int fileLength;
        DiskLoc unused; /* unused is the portion of the file that doesn't belong to any allocated extents. -1 = no more */
        int unusedLength;
        char reserved[8192 - 4*4 - 8];

        char data[4]; // first extent starts here

        enum { HeaderSize = 8192 };

        bool isCurrentVersion() const {
            return version == PDFILE_VERSION && ( versionMinor == PDFILE_VERSION_MINOR_22_AND_OLDER
                                               || versionMinor == PDFILE_VERSION_MINOR_24_AND_NEWER
                                                );
        }

        bool uninitialized() const { return version == 0; }

        void init(int fileno, int filelength, const char* filename);

        bool isEmpty() const {
            return uninitialized() || ( unusedLength == fileLength - HeaderSize - 16 );
        }
    };
#pragma pack()


    class DataFile {
        friend class DataFileMgr;
        friend class BasicCursor;
    public:
        DataFile(int fn) : _mb(0), fileNo(fn) { }

        /** @return true if found and opened. if uninitialized (prealloc only) does not open. */
        Status openExisting( const char *filename );

        /** creates if DNE */
        void open(const char *filename, int requestedDataSize = 0, bool preallocateOnly = false);

        /* allocate a new extent from this datafile.
           @param capped - true if capped collection
           @param loops is our recursion check variable - you want to pass in zero
        */
        Extent* createExtent(const char *ns, int approxSize, bool capped = false, int loops = 0);

        DataFileHeader *getHeader() { return header(); }
        HANDLE getFd() { return mmf.getFd(); }
        unsigned long long length() const { return mmf.length(); }

        /* return max size an extent may be */
        static int maxSize();

        /** fsync */
        void flush( bool sync );

        /** only use fore debugging */
        Extent* debug_getExtent(DiskLoc loc) { return _getExtent( loc ); }

        Extent* getExtent(DiskLoc loc) const;
        Record* recordAt(DiskLoc dl) const;
    private:
        void badOfs(int) const;
        void badOfs2(int) const;
        int defaultSize( const char *filename ) const;

        Extent* _getExtent(DiskLoc loc) const;

        void grow(DiskLoc dl, int size);

        char* p() const { return (char *) _mb; }
        DataFileHeader* header() { return (DataFileHeader*) _mb; }

        DurableMappedFile mmf;
        void *_mb; // the memory mapped view
        int fileNo;
    };


}
