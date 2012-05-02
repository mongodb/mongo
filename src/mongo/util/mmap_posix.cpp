// mmap_posix.cpp

/*    Copyright 2009 10gen Inc.
 *
 *    Licensed under the Apache License, Version 2.0 (the "License");
 *    you may not use this file except in compliance with the License.
 *    You may obtain a copy of the License at
 *
 *    http://www.apache.org/licenses/LICENSE-2.0
 *
 *    Unless required by applicable law or agreed to in writing, software
 *    distributed under the License is distributed on an "AS IS" BASIS,
 *    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *    See the License for the specific language governing permissions and
 *    limitations under the License.
 */

#include "pch.h"
#include "mmap.h"
#include "file_allocator.h"
#include "../db/d_concurrency.h"
#include <errno.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include "../util/processinfo.h"
#include "mongoutils/str.h"
using namespace mongoutils;

namespace mongo {

    MemoryMappedFile::MemoryMappedFile() {
        fd = 0;
        maphandle = 0;
        len = 0;
        created();
    }

    void MemoryMappedFile::close() {
        LockMongoFilesShared::assertExclusivelyLocked();
        for( vector<void*>::iterator i = views.begin(); i != views.end(); i++ ) {
            munmap(*i,len);
        }
        views.clear();

        if ( fd )
            ::close(fd);
        fd = 0;
        destroyed(); // cleans up from the master list of mmaps
    }

#ifndef O_NOATIME
#define O_NOATIME (0)
#endif

#ifndef MAP_NORESERVE
#define MAP_NORESERVE (0)
#endif

#if defined(__sunos__)
    MAdvise::MAdvise(void *,unsigned, Advice) { }
    MAdvise::~MAdvise() { }
#else
    MAdvise::MAdvise(void *p, unsigned len, Advice a) {
        
        static long pageSize = 0;
        if ( pageSize == 0 ) {
            pageSize = sysconf( _SC_PAGESIZE );
        }
        _p = (void*)((long)p & ~(pageSize-1));
        
        _len = len +((unsigned long long)p-(unsigned long long)_p);
        
        int advice = 0;
        switch ( a ) {
        case Sequential: advice = MADV_SEQUENTIAL; break;
        case Random: advice = MADV_RANDOM; break;
        default: verify(0);
        }
        
        if ( madvise(_p,_len,advice ) ) {
            error() << "madvise failed: " << errnoWithDescription() << endl;
        }
        
    }
    MAdvise::~MAdvise() { 
        madvise(_p,_len,MADV_NORMAL);
    }
#endif

    void* MemoryMappedFile::map(const char *filename, unsigned long long &length, int options) {
        // length may be updated by callee.
        setFilename(filename);
        FileAllocator::get()->allocateAsap( filename, length );
        len = length;

        massert( 10446 , str::stream() << "mmap: can't map area of size 0 file: " << filename, length > 0 );

        fd = open(filename, O_RDWR | O_NOATIME);
        if ( fd <= 0 ) {
            log() << "couldn't open " << filename << ' ' << errnoWithDescription() << endl;
            fd = 0; // our sentinel for not opened
            return 0;
        }

        unsigned long long filelen = lseek(fd, 0, SEEK_END);
        uassert(10447,  str::stream() << "map file alloc failed, wanted: " << length << " filelen: " << filelen << ' ' << sizeof(size_t), filelen == length );
        lseek( fd, 0, SEEK_SET );

        void * view = mmap(NULL, length, PROT_READ|PROT_WRITE, MAP_SHARED, fd, 0);
        if ( view == MAP_FAILED ) {
            error() << "  mmap() failed for " << filename << " len:" << length << " " << errnoWithDescription() << endl;
            if ( errno == ENOMEM ) {
                if( sizeof(void*) == 4 )
                    error() << "mmap failed with out of memory. You are using a 32-bit build and probably need to upgrade to 64" << endl;
                else
                    error() << "mmap failed with out of memory. (64 bit build)" << endl;
            }
            return 0;
        }


#if defined(__sunos__)
#warning madvise not supported on solaris yet
#else
        if ( options & SEQUENTIAL ) {
            if ( madvise( view , length , MADV_SEQUENTIAL ) ) {
                warning() << "map: madvise failed for " << filename << ' ' << errnoWithDescription() << endl;
            }
        }
#endif

        views.push_back( view );

        return view;
    }

    void* MemoryMappedFile::createReadOnlyMap() {
        void * x = mmap( /*start*/0 , len , PROT_READ , MAP_SHARED , fd , 0 );
        if( x == MAP_FAILED ) {
            if ( errno == ENOMEM ) {
                if( sizeof(void*) == 4 )
                    error() << "mmap ro failed with out of memory. You are using a 32-bit build and probably need to upgrade to 64" << endl;
                else
                    error() << "mmap ro failed with out of memory. (64 bit build)" << endl;
            }
            return 0;
        }
        return x;
    }

    void* MemoryMappedFile::createPrivateMap() {
        void * x = mmap( /*start*/0 , len , PROT_READ|PROT_WRITE , MAP_PRIVATE|MAP_NORESERVE , fd , 0 );
        if( x == MAP_FAILED ) {
            if ( errno == ENOMEM ) {
                if( sizeof(void*) == 4 ) {
                    error() << "mmap private failed with out of memory. You are using a 32-bit build and probably need to upgrade to 64" << endl;
                }
                else {
                    error() << "mmap private failed with out of memory. (64 bit build)" << endl;
                }
            }
            else { 
                error() << "mmap private failed " << errnoWithDescription() << endl;
            }
            return 0;
        }

        views.push_back(x);
        return x;
    }

    void* MemoryMappedFile::remapPrivateView(void *oldPrivateAddr) {
        // don't unmap, just mmap over the old region
        void * x = mmap( oldPrivateAddr, len , PROT_READ|PROT_WRITE , MAP_PRIVATE|MAP_NORESERVE|MAP_FIXED , fd , 0 );
        if( x == MAP_FAILED ) {
            int err = errno;
            error()  << "13601 Couldn't remap private view: " << errnoWithDescription(err) << endl;
            log() << "aborting" << endl;
            printMemInfo();
            abort();
        }
        verify( x == oldPrivateAddr );
        return x;
    }

    void MemoryMappedFile::flush(bool sync) {
        if ( views.empty() || fd == 0 )
            return;
        if ( msync(viewForFlushing(), len, sync ? MS_SYNC : MS_ASYNC) )
            problem() << "msync " << errnoWithDescription() << endl;
    }

    class PosixFlushable : public MemoryMappedFile::Flushable {
    public:
        PosixFlushable( void * view , HANDLE fd , long len )
            : _view( view ) , _fd( fd ) , _len(len) {
        }

        void flush() {
            if ( _view && _fd )
                if ( msync(_view, _len, MS_SYNC ) )
                    problem() << "msync " << errnoWithDescription() << endl;

        }

        void * _view;
        HANDLE _fd;
        long _len;
    };

    MemoryMappedFile::Flushable * MemoryMappedFile::prepareFlush() {
        return new PosixFlushable( viewForFlushing() , fd , len );
    }


} // namespace mongo

