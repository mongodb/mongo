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
#include "../db/d_concurrency.h"
#include "../db/memconcept.h"
#include "mongo/util/timer.h"
#include "mongo/util/concurrency/remap_lock.h"

namespace mongo {

    mutex mapViewMutex("mapView");
    ourbitset writable;

    MAdvise::MAdvise(void *,unsigned, Advice) { }
    MAdvise::~MAdvise() { }

    // SERVER-2942 -- We do it this way because RemapLock is used in both mongod and mongos but
    // we need different effects.  When called in mongod it needs to be a mutex and in mongos it
    // needs to be a no-op.  This is the mongod version, the no-op mongos version is in server.cpp.
    SimpleMutex _remapLock( "remapLock" );
    RemapLock::RemapLock() {
        _remapLock.lock();
    }
    RemapLock::~RemapLock() {
        _remapLock.unlock();
    }

    /** notification on unmapping so we can clear writable bits */
    void MemoryMappedFile::clearWritableBits(void *p) {
        for( unsigned i = ((size_t)p)/ChunkSize; i <= (((size_t)p)+len)/ChunkSize; i++ ) {
            writable.clear(i);
            verify( !writable.get(i) );
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
        LockMongoFilesShared::assertExclusivelyLocked();
        for( vector<void*>::iterator i = views.begin(); i != views.end(); i++ ) {
            memconcept::invalidate(*i);
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
        verify( maphandle );
        scoped_lock lk(mapViewMutex);
        void* readOnlyMapAddress = MapViewOfFile(
                maphandle,          // file mapping handle
                FILE_MAP_READ,      // access
                0, 0,               // file offset, high and low
                0 );                // bytes to map, 0 == all
        if ( 0 == readOnlyMapAddress ) {
            DWORD dosError = GetLastError();
            log() << "MapViewOfFile for " << filename()
                    << " failed with error " << errnoWithDescription( dosError )
                    << " (file size is " << len << ")"
                    << " in MemoryMappedFile::createReadOnlyMap"
                    << endl;
            fassertFailed( 16165 );
        }
        memconcept::is( readOnlyMapAddress, memconcept::concept::other, filename() );
        views.push_back( readOnlyMapAddress );
        return readOnlyMapAddress;
    }

    void* MemoryMappedFile::map(const char *filenameIn, unsigned long long &length, int options) {
        verify( fd == 0 && len == 0 ); // can't open more than once
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
            view = MapViewOfFile(
                    maphandle,  // file mapping handle
                    access,     // access
                    0, 0,       // file offset, high and low
                    0 );        // bytes to map, 0 == all
            if ( view == 0 ) {
                DWORD dosError = GetLastError();
                log() << "MapViewOfFile for " << filename
                        << " failed with error " << errnoWithDescription( dosError )
                        << " (file size is " << len << ")"
                        << " in MemoryMappedFile::map"
                        << endl;
                close();
                fassertFailed( 16166 );
            }
        }
        views.push_back(view);
        memconcept::is(view, memconcept::concept::memorymappedfile, this->filename(), (unsigned) length);
        len = length;
        return view;
    }

    extern mutex mapViewMutex;

    __declspec(noinline) void makeChunkWritable(size_t chunkno) { 
        scoped_lock lk(mapViewMutex);

        if( writable.get(chunkno) ) // double check lock
            return;

        // remap all maps in this chunk.  common case is a single map, but could have more than one with smallfiles or .ns files
        size_t chunkStart = chunkno * MemoryMappedFile::ChunkSize;
        size_t chunkNext = chunkStart + MemoryMappedFile::ChunkSize;

        scoped_lock lk2(privateViews._mutex());
        map<void*,MongoMMF*>::iterator i = privateViews.finditer_inlock((void*) (chunkNext-1));
        while( 1 ) {
            const pair<void*,MongoMMF*> x = *(--i);
            MongoMMF *mmf = x.second;
            if( mmf == 0 )
                break;

            size_t viewStart = (size_t) x.first;
            size_t viewEnd = (size_t) (viewStart + mmf->length());
            if( viewEnd <= chunkStart )
                break;

            size_t protectStart = max(viewStart, chunkStart);
            dassert(protectStart<chunkNext);

            size_t protectEnd = min(viewEnd, chunkNext);
            size_t protectSize = protectEnd - protectStart;
            dassert(protectSize>0&&protectSize<=MemoryMappedFile::ChunkSize);

            DWORD old;
            bool ok = VirtualProtect((void*)protectStart, protectSize, PAGE_WRITECOPY, &old);
            if( !ok ) {
                DWORD e = GetLastError();
                log() << "VirtualProtect failed (mcw) " << mmf->filename() << ' ' << chunkno << hex << protectStart << ' ' << protectSize << ' ' << errnoWithDescription(e) << endl;
                verify(false);
            }
        }

        writable.set(chunkno);
    }

    void* MemoryMappedFile::createPrivateMap() {
        verify( maphandle );
        scoped_lock lk(mapViewMutex);
        void* privateMapAddress = MapViewOfFile(
                maphandle,          // file mapping handle
                FILE_MAP_READ,      // access
                0, 0,               // file offset, high and low
                0 );                // bytes to map, 0 == all
        if ( privateMapAddress == 0 ) {
            DWORD dosError = GetLastError();
            log() << "MapViewOfFile for " << filename()
                    << " failed with error " << errnoWithDescription( dosError )
                    << " (file size is " << len << ")"
                    << " in MemoryMappedFile::createPrivateMap"
                    << endl;
            fassertFailed( 16167 );
        }
        clearWritableBits( privateMapAddress );
        views.push_back( privateMapAddress );
        memconcept::is( privateMapAddress, memconcept::concept::memorymappedfile, filename() );
        return privateMapAddress;
    }

    void* MemoryMappedFile::remapPrivateView(void *oldPrivateAddr) {
        verify( Lock::isW() );

        LockMongoFilesExclusive lockMongoFiles;

        RemapLock lk;   // Interlock with PortMessageServer::acceptedMP() to stop thread creation

        clearWritableBits(oldPrivateAddr);
        if( !UnmapViewOfFile(oldPrivateAddr) ) {
            DWORD dosError = GetLastError();
            log() << "UnMapViewOfFile for " << filename()
                    << " failed with error " << errnoWithDescription( dosError )
                    << " in MemoryMappedFile::remapPrivateView"
                    << endl;
            fassertFailed( 16168 );
        }

        void* newPrivateView = MapViewOfFileEx(
                maphandle,          // file mapping handle
                FILE_MAP_READ,      // access
                0, 0,               // file offset, high and low
                0,                  // bytes to map, 0 == all
                oldPrivateAddr );   // we want the same address we had before
        if ( 0 == newPrivateView ) {
            DWORD dosError = GetLastError();
            log() << "MapViewOfFileEx for " << filename()
                    << " failed with error " << errnoWithDescription( dosError )
                    << " (file size is " << len << ")"
                    << " in MemoryMappedFile::remapPrivateView"
                    << endl;
        }
        fassert( 16148, newPrivateView == oldPrivateAddr );
        return newPrivateView;
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
            const int maximumLoopCount = 1000 * 1000;
            const int maximumTimeInSeconds = 60;
            Timer t;
            while ( !success && !timeout && loopCount < maximumLoopCount ) {
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
            }

            success = FALSE != FlushFileBuffers(_fd);
            if (!success) {
                int err = GetLastError();
                out() << "FlushFileBuffers failed " << err << " file: " << _filename << endl;
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

}
