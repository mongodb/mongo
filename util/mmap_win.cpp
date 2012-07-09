// mmap_win.cpp

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
#include "text.h"
#include "../db/mongommf.h"
#include "../db/concurrency.h"
#include "timer.h"

namespace mongo {

    mutex mapViewMutex("mapView");
    ourbitset writable;

    MAdvise::MAdvise(void *,unsigned, Advice) { }
    MAdvise::~MAdvise() { }

    static unsigned long long _nextMemoryMappedFileLocation = 256LL * 1024LL * 1024LL * 1024LL;
    static SimpleMutex _nextMemoryMappedFileLocationMutex( "nextMemoryMappedFileLocationMutex" );

    void* getNextMemoryMappedFileLocation( unsigned long long mmfSize ) {
        if ( 4 == sizeof(void*) ) {
            return 0;
        }
        SimpleMutex::scoped_lock lk( _nextMemoryMappedFileLocationMutex );
        static unsigned long long granularity = 0;
        if ( 0 == granularity ) {
            SYSTEM_INFO systemInfo;
            GetSystemInfo( &systemInfo );
            granularity = static_cast<unsigned long long>( systemInfo.dwAllocationGranularity );
        }
        unsigned long long thisMemoryMappedFileLocation = _nextMemoryMappedFileLocation;
        mmfSize = ( mmfSize + granularity - 1) & ~( granularity - 1 );
        _nextMemoryMappedFileLocation += mmfSize;
        return reinterpret_cast<void*>( static_cast<uintptr_t>( thisMemoryMappedFileLocation ) );
    }

    /** notification on unmapping so we can clear writable bits */
    void MemoryMappedFile::clearWritableBits(void *p) {
        for( unsigned i = ((size_t)p)/ChunkSize; i <= (((size_t)p)+len)/ChunkSize; i++ ) {
            writable.clear(i);
            assert( !writable.get(i) );
        }
    }

    MemoryMappedFile::MemoryMappedFile()
        : _flushMutex(new mutex("flushMutex")) {
        fd = 0;
        maphandle = 0;
        len = 0;
        created();
    }

    void MemoryMappedFile::close() {
        mmmutex.assertExclusivelyLocked();
        for( vector<void*>::iterator i = views.begin(); i != views.end(); i++ ) {
            clearWritableBits(*i);
            UnmapViewOfFile(*i);
        }
        views.clear();
        if ( maphandle )
            CloseHandle(maphandle);
        maphandle = 0;
        if ( fd )
            CloseHandle(fd);
        fd = 0;
        destroyed(); // cleans up from the master list of mmaps
    }

    unsigned long long mapped = 0;

    void* MemoryMappedFile::createReadOnlyMap() {
        assert( maphandle );
        scoped_lock lk(mapViewMutex);
        LPVOID thisAddress = getNextMemoryMappedFileLocation( len );
        void* readOnlyMapAddress = MapViewOfFileEx(
                maphandle,          // file mapping handle
                FILE_MAP_READ,      // access
                0, 0,               // file offset, high and low
                0,                  // bytes to map, 0 == all
                thisAddress );      // address to place file
        if ( 0 == readOnlyMapAddress ) {
            DWORD dosError = GetLastError();
            log() << "MapViewOfFileEx for " << filename()
                    << " failed with " << errnoWithDescription( dosError )
                    << " (file size is " << len << ")"
                    << " in MemoryMappedFile::createReadOnlyMap, terminating"
                    << endl;
            ::abort();
        }
        views.push_back( readOnlyMapAddress );
        return readOnlyMapAddress;
    }

    void* MemoryMappedFile::map(const char *filenameIn, unsigned long long &length, int options) {
        assert( fd == 0 && len == 0 ); // can't open more than once
        setFilename(filenameIn);
        /* big hack here: Babble uses db names with colons.  doesn't seem to work on windows.  temporary perhaps. */
        char filename[256];
        strncpy(filename, filenameIn, 255);
        filename[255] = 0;
        {
            size_t len = strlen( filename );
            for ( size_t i=len-1; i>=0; i-- ) {
                if ( filename[i] == '/' ||
                        filename[i] == '\\' )
                    break;

                if ( filename[i] == ':' )
                    filename[i] = '_';
            }
        }

        updateLength( filename, length );

        {
            DWORD createOptions = FILE_ATTRIBUTE_NORMAL;
            if ( options & SEQUENTIAL )
                createOptions |= FILE_FLAG_SEQUENTIAL_SCAN;
            DWORD rw = GENERIC_READ | GENERIC_WRITE;
            fd = CreateFile(
                     toNativeString(filename).c_str(),
                     rw, // desired access
                     FILE_SHARE_WRITE | FILE_SHARE_READ, // share mode
                     NULL, // security
                     OPEN_ALWAYS, // create disposition
                     createOptions , // flags
                     NULL); // hTempl
            if ( fd == INVALID_HANDLE_VALUE ) {
                DWORD e = GetLastError();
                log() << "Create/OpenFile failed " << filename << " errno:" << e << endl;
                return 0;
            }
        }

        mapped += length;

        {
            DWORD flProtect = PAGE_READWRITE; //(options & READONLY)?PAGE_READONLY:PAGE_READWRITE;
            maphandle = CreateFileMapping(fd, NULL, flProtect,
                                          length >> 32 /*maxsizehigh*/,
                                          (unsigned) length /*maxsizelow*/,
                                          NULL/*lpName*/);
            if ( maphandle == NULL ) {
                DWORD e = GetLastError(); // log() call was killing lasterror before we get to that point in the stream
                log() << "CreateFileMapping failed " << filename << ' ' << errnoWithDescription(e) << endl;
                close();
                return 0;
            }
        }

        void *view = 0;
        {
            scoped_lock lk(mapViewMutex);
            DWORD access = ( options & READONLY ) ? FILE_MAP_READ : FILE_MAP_ALL_ACCESS;
            LPVOID thisAddress = getNextMemoryMappedFileLocation( length );
            view = MapViewOfFileEx(
                    maphandle,      // file mapping handle
                    access,         // access
                    0, 0,           // file offset, high and low
                    0,              // bytes to map, 0 == all
                    thisAddress );  // address to place file
        }
        {
            if ( view == 0 ) {
                DWORD dosError = GetLastError();
                log() << "MapViewOfFileEx for " << filename
                        << " failed with " << errnoWithDescription( dosError )
                        << " (file size is " << length << ")"
                        << " in MemoryMappedFile::map, terminating"
                        << endl;
                close();
                ::abort();
            }
        }
        views.push_back(view);
        len = length;

        return view;
    }

    class WindowsFlushable : public MemoryMappedFile::Flushable {
    public:
        WindowsFlushable( void * view , HANDLE fd , string filename , boost::shared_ptr<mutex> flushMutex )
            : _view(view) , _fd(fd) , _filename(filename) , _flushMutex(flushMutex)
        {}

        void flush() {
            if (!_view || !_fd)
                return;

            scoped_lock lk(*_flushMutex);

            int loopCount = 0;
            bool success = false;
            bool timeout = false;
            int dosError = ERROR_SUCCESS;
            const int maximumTimeInSeconds = 60 * 15;
            Timer t;
            while ( !success && !timeout ) {
                ++loopCount;
                success = FALSE != FlushViewOfFile( _view, 0 );
                if ( !success ) {
                    dosError = GetLastError();
                    if ( dosError != ERROR_LOCK_VIOLATION ) {
                        break;
                    }
                    timeout = t.seconds() > maximumTimeInSeconds;
                }
            }
            if ( success && loopCount > 1 ) {
                log() << "FlushViewOfFile for " << _filename
                        << " succeeded after " << loopCount
                        << " attempts taking " << t.millis()
                        << " ms" << endl;
            }
            else if ( !success ) {
                log() << "FlushViewOfFile for " << _filename
                        << " failed with error " << dosError
                        << " after " << loopCount
                        << " attempts taking " << t.millis()
                        << " ms" << endl;
                // Abort here to avoid data corruption
                fassert(16387, false);
            }

            success = FALSE != FlushFileBuffers(_fd);
            if (!success) {
                int err = GetLastError();
                out() << "FlushFileBuffers failed " << err << " file: " << _filename << endl;
                fassert(16388, false);
            }
        }

        void * _view;
        HANDLE _fd;
        string _filename;
        boost::shared_ptr<mutex> _flushMutex;
    };

    void MemoryMappedFile::flush(bool sync) {
        uassert(13056, "Async flushing not supported on windows", sync);
        if( !views.empty() ) {
            WindowsFlushable f( viewForFlushing() , fd , filename() , _flushMutex);
            f.flush();
        }
    }

    MemoryMappedFile::Flushable * MemoryMappedFile::prepareFlush() {
        return new WindowsFlushable( viewForFlushing() , fd , filename() , _flushMutex );
    }
    void MemoryMappedFile::_lock() {}
    void MemoryMappedFile::_unlock() {}

}
