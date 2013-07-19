// data_file.cpp

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

#include "mongo/pch.h"

#include "mongo/db/storage/data_file.h"

#include <boost/filesystem/operations.hpp>

#include "mongo/db/cmdline.h"
#include "mongo/db/d_concurrency.h"
#include "mongo/db/dur.h"
#include "mongo/db/lockstate.h"
#include "mongo/db/pdfile.h"
#include "mongo/db/storage/extent.h"
#include "mongo/util/file_allocator.h"

namespace mongo {


    // XXX-ERH
    void addNewExtentToNamespace(const char *ns, Extent *e, DiskLoc eloc, DiskLoc emptyLoc, bool capped);

    static void data_file_check(void *_mb) { 
        if( sizeof(char *) == 4 )
            uassert( 10084 , "can't map file memory - mongo requires 64 bit build for larger datasets", _mb != 0);
        else
            uassert( 10085 , "can't map file memory", _mb != 0);
    }

    int DataFile::maxSize() {
        if ( sizeof( int* ) == 4 ) {
            return 512 * 1024 * 1024;
        }
        else if ( cmdLine.smallfiles ) {
            return 0x7ff00000 >> 2;
        }
        else {
            return 0x7ff00000;
        }
    }

    NOINLINE_DECL void DataFile::badOfs2(int ofs) const {
        stringstream ss;
        ss << "bad offset:" << ofs << " accessing file: " << mmf.filename() << " - consider repairing database";
        uasserted(13441, ss.str());
    }

    NOINLINE_DECL void DataFile::badOfs(int ofs) const {
        stringstream ss;
        ss << "bad offset:" << ofs << " accessing file: " << mmf.filename() << " - consider repairing database";
        uasserted(13440, ss.str());
    }

    int DataFile::defaultSize( const char *filename ) const {
        int size;
        if ( fileNo <= 4 )
            size = (64*1024*1024) << fileNo;
        else
            size = 0x7ff00000;
        if ( cmdLine.smallfiles ) {
            size = size >> 2;
        }
        return size;
    }

    /** @return true if found and opened. if uninitialized (prealloc only) does not open. */
    Status DataFile::openExisting( const char *filename ) {
        verify( _mb == 0 );
        if( !boost::filesystem::exists(filename) )
            return Status( ErrorCodes::InvalidPath, "DataFile::openExisting - file does not exist" );

        if( !mmf.open(filename,false) ) {
            MONGO_DLOG(2) << "info couldn't open " << filename << " probably end of datafile list" << endl;
            return Status( ErrorCodes::InternalError, "DataFile::openExisting - mmf.open failed" );
        }
        _mb = mmf.getView(); verify(_mb);
        unsigned long long sz = mmf.length();
        verify( sz <= 0x7fffffff );
        verify( sz % 4096 == 0 );
        if( sz < 64*1024*1024 && !cmdLine.smallfiles ) {
            if( sz >= 16*1024*1024 && sz % (1024*1024) == 0 ) {
                log() << "info openExisting file size " << sz << " but cmdLine.smallfiles=false: "
                      << filename << endl;
            }
            else {
                log() << "openExisting size " << sz << " less then minimum file size expectation "
                      << filename << endl;
                verify(false);
            }
        }
        data_file_check(_mb);
        return Status::OK();
    }

    void DataFile::open( const char *filename, int minSize, bool preallocateOnly ) {
        long size = defaultSize( filename );
        while ( size < minSize ) {
            if ( size < maxSize() / 2 )
                size *= 2;
            else {
                size = maxSize();
                break;
            }
        }
        if ( size > maxSize() )
            size = maxSize();

        verify( size >= 64*1024*1024 || cmdLine.smallfiles );
        verify( size % 4096 == 0 );

        if ( preallocateOnly ) {
            if ( cmdLine.prealloc ) {
                FileAllocator::get()->requestAllocation( filename, size );
            }
            return;
        }

        {
            verify( _mb == 0 );
            unsigned long long sz = size;
            if( mmf.create(filename, sz, false) )
                _mb = mmf.getView();
            verify( sz <= 0x7fffffff );
            size = (int) sz;
        }
        data_file_check(_mb);
        header()->init(fileNo, size, filename);
    }

    void DataFile::flush( bool sync ) {
        mmf.flush( sync );
    }

    Extent* DataFile::createExtent(const char *ns, int approxSize, bool newCapped, int loops) {
        approxSize = ExtentManager::quantizeExtentSize( approxSize );

        massert( 10357 ,  "shutdown in progress", ! inShutdown() );
        massert( 10358 ,  "bad new extent size", approxSize >= Extent::minSize() && approxSize <= Extent::maxSize() );
        massert( 10359 ,  "header==0 on new extent: 32 bit mmap space exceeded?", header() ); // null if file open failed
        int ExtentSize = min(header()->unusedLength, approxSize);

        verify( ExtentSize >= Extent::minSize() ); // TODO: maybe return NULL

        int offset = header()->unused.getOfs();

        DataFileHeader *h = header();
        h->unused.writing().set( fileNo, offset + ExtentSize );
        getDur().writingInt(h->unusedLength) = h->unusedLength - ExtentSize;

        DiskLoc loc;
        loc.set(fileNo, offset);
        Extent *e = _getExtent(loc);
        DiskLoc emptyLoc = getDur().writing(e)->init(ns, ExtentSize, fileNo, offset, newCapped);

        addNewExtentToNamespace(ns, e, loc, emptyLoc, newCapped);

        DEV {
            MONGO_TLOG(1) << "new extent " << ns << " size: 0x" << hex << ExtentSize << " loc: 0x"
                          << hex << offset << " emptyLoc:" << hex << emptyLoc.getOfs() << dec
                          << endl;
        }
        return e;
    }

    // -------------------------------------------------------------------------------

    void DataFileHeader::init(int fileno, int filelength, const char* filename) {
        if ( uninitialized() ) {
            DEV log() << "datafileheader::init initializing " << filename << " n:" << fileno << endl;
            if( !(filelength > 32768 ) ) {
                massert(13640, str::stream() << "DataFileHeader looks corrupt at file open filelength:" << filelength << " fileno:" << fileno, false);
            }

            {
                // "something" is too vague, but we checked for the right db to be locked higher up the call stack
                if( !Lock::somethingWriteLocked() ) {
                    LockState::Dump();
                    log() << "*** TEMP NOT INITIALIZING FILE " << filename << ", not in a write lock." << endl;
                    log() << "temp bypass until more elaborate change - case that is manifesting is benign anyway" << endl;
                    return;
                    /**
                       log() << "ERROR can't create outside a write lock" << endl;
                       printStackTrace();
                       ::abort();
                    **/
                }
            }

            getDur().createdFile(filename, filelength);
            verify( HeaderSize == 8192 );
            DataFileHeader *h = getDur().writing(this);
            h->fileLength = filelength;
            h->version = PDFILE_VERSION;
            h->versionMinor = PDFILE_VERSION_MINOR_22_AND_OLDER; // All dbs start like this
            h->unused.set( fileno, HeaderSize );
            verify( (data-(char*)this) == HeaderSize );
            h->unusedLength = fileLength - HeaderSize - 16;
        }
    }

}
