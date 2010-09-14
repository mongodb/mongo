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
#include "../db/concurrency.h"

#include <errno.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

namespace mongo {

    MemoryMappedFile::MemoryMappedFile() {
        fd = 0;
        maphandle = 0;
        view = 0;
        len = 0;
        created();
    }

    void MemoryMappedFile::close() {
        if ( view )
            munmap(view, len);
        view = 0;

        if ( fd )
            ::close(fd);
        fd = 0;
    }

#ifndef O_NOATIME
#define O_NOATIME 0
#endif

    void* MemoryMappedFile::map(const char *filename, unsigned long long &length, int options) {
        // length may be updated by callee.
        _filename = filename;
        theFileAllocator().allocateAsap( filename, length );
        len = length;

        massert( 10446 ,  (string)"mmap() can't map area of size 0 [" + filename + "]" , length > 0 );

        
        fd = open(filename, O_RDWR | O_NOATIME);
        if ( fd <= 0 ) {
            out() << "couldn't open " << filename << ' ' << errnoWithDescription() << endl;
            return 0;
        }

        unsigned long long filelen = lseek(fd, 0, SEEK_END);
        if ( filelen != length ){
            cout << "wanted length: " << length << " filelen: " << filelen << endl;
            cout << sizeof(size_t) << endl;
            massert( 10447 ,  "file size allocation failed", filelen == length );
        }
        lseek( fd, 0, SEEK_SET );
        
        view = mmap(NULL, length, PROT_READ|PROT_WRITE, MAP_SHARED, fd, 0);
        if ( view == MAP_FAILED ) {
            out() << "  mmap() failed for " << filename << " len:" << length << " " << errnoWithDescription() << endl;
            if ( errno == ENOMEM ){
                out() << "mmap failed with out of memory, if you're using 32-bits, then you probably need to upgrade to 64" << endl;
            }
            return 0;
        }


#if defined(__sunos__)
#warning madvise not supported on solaris yet
#else
        if ( options & SEQUENTIAL ){
            if ( madvise( view , length , MADV_SEQUENTIAL ) ){
                out() << " madvise failed for " << filename << " " << errnoWithDescription() << endl;
            }
        }
#endif

        DEV if (! dbMutex.info().isLocked()){
            _unlock();
        }

        return view;
    }
    
    void* MemoryMappedFile::testGetCopyOnWriteView(){
        void * x = mmap( NULL , len , PROT_READ | PROT_WRITE , MAP_PRIVATE , fd , 0 );
        assert( x );
        return x;
    }
    
    void  MemoryMappedFile::testCloseCopyOnWriteView(void * x ){
        munmap(x,len);
    }
    
    void MemoryMappedFile::flush(bool sync) {
        if ( view == 0 || fd == 0 )
            return;
        if ( msync(view, len, sync ? MS_SYNC : MS_ASYNC) )
            problem() << "msync " << errnoWithDescription() << endl;
    }
    
    class PosixFlushable : public MemoryMappedFile::Flushable {
    public:
        PosixFlushable( void * view , HANDLE fd , long len )
            : _view( view ) , _fd( fd ) , _len(len){
        }

        void flush(){
            if ( _view && _fd )
                if ( msync(_view, _len, MS_SYNC ) )
                    problem() << "msync " << errnoWithDescription() << endl;
            
        }

        void * _view;
        HANDLE _fd;
        long _len;
    };

    MemoryMappedFile::Flushable * MemoryMappedFile::prepareFlush(){
        return new PosixFlushable( view , fd , len );
    }

    void MemoryMappedFile::_lock() {
        if (view) assert(mprotect(view, len, PROT_READ | PROT_WRITE) == 0);
    }

    void MemoryMappedFile::_unlock() {
        if (view) assert(mprotect(view, len, PROT_READ) == 0);
    }

} // namespace mongo

