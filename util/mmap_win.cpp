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
#include <windows.h>

namespace mongo {

    MemoryMappedFile::MemoryMappedFile()
        : _flushMutex(new mutex("flushMutex"))
    {
        fd = 0;
        maphandle = 0;
        view = 0;
        len = 0;
        created();
    }

    void MemoryMappedFile::close() {
        if ( view )
            UnmapViewOfFile(view);
        view = 0;
        if ( maphandle )
            CloseHandle(maphandle);
        maphandle = 0;
        if ( fd )
            CloseHandle(fd);
        fd = 0;
    }
    
    unsigned long long mapped = 0;

    void* MemoryMappedFile::map(const char *filenameIn, unsigned long long &length, int options) {
        _filename = filenameIn;
        /* big hack here: Babble uses db names with colons.  doesn't seem to work on windows.  temporary perhaps. */
        char filename[256];
        strncpy(filename, filenameIn, 255);
        filename[255] = 0;
        { 
            size_t len = strlen( filename );
            for ( size_t i=len-1; i>=0; i-- ){
                if ( filename[i] == '/' ||
                     filename[i] == '\\' )
                    break;
                
                if ( filename[i] == ':' )
                    filename[i] = '_';
            }
        }

        updateLength( filename, length );

        DWORD createOptions = FILE_ATTRIBUTE_NORMAL;
        if ( options & SEQUENTIAL )
            createOptions |= FILE_FLAG_SEQUENTIAL_SCAN;
        DWORD rw = GENERIC_READ | GENERIC_WRITE;
        //if ( options & READONLY ) 
        //    rw = GENERIC_READ;

        fd = CreateFile(
                 toNativeString(filename).c_str(),
                 rw, // desired access
                 FILE_SHARE_READ, // share mode
                 NULL, // security
                 OPEN_ALWAYS, // create disposition
                 createOptions , // flags
                 NULL); // hTempl
        if ( fd == INVALID_HANDLE_VALUE ) {
            log() << "Create/OpenFile failed " << filename << ' ' << GetLastError() << endl;
            return 0;
        }

        mapped += length;

        DWORD flProtect = PAGE_READWRITE; //(options & READONLY)?PAGE_READONLY:PAGE_READWRITE;
        maphandle = CreateFileMapping(fd, NULL, flProtect, 
            length >> 32 /*maxsizehigh*/, 
            (unsigned) length /*maxsizelow*/, 
            NULL/*lpName*/);
        if ( maphandle == NULL ) {
            DWORD e = GetLastError(); // log() call was killing lasterror before we get to that point in the stream
            log() << "CreateFileMapping failed " << filename << ' ' << errnoWithDescription(e) << endl;
            return 0;
        }

        {
            DWORD access = (options&READONLY)? FILE_MAP_READ : FILE_MAP_ALL_ACCESS;
            view = MapViewOfFile(maphandle, access, /*f ofs hi*/0, /*f ofs lo*/ 0, /*dwNumberOfBytesToMap 0 means to eof*/0);
        }
        if ( view == 0 ) {
            DWORD e = GetLastError();
            log() << "MapViewOfFile failed " << filename << " " << errnoWithDescription(e) << endl;
        }
        len = length;
        return view;
    }

    class WindowsFlushable : public MemoryMappedFile::Flushable {
    public:
        WindowsFlushable( void * view , HANDLE fd , string filename , boost::shared_ptr<mutex> flushMutex )
            : _view(view) , _fd(fd) , _filename(filename) , _flushMutex(flushMutex)
        {}
        
        void flush(){
            if (!_view || !_fd) 
                return;

            scoped_lock lk(*_flushMutex);

            bool success = FlushViewOfFile(_view, 0); // 0 means whole mapping
            if (!success){
                int err = GetLastError();
                out() << "FlushViewOfFile failed " << err << " file: " << _filename << endl;
            }
            
            success = FlushFileBuffers(_fd);
            if (!success){
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
        
        WindowsFlushable f( view , fd , _filename , _flushMutex);
        f.flush();
    }

    MemoryMappedFile::Flushable * MemoryMappedFile::prepareFlush(){
        return new WindowsFlushable( view , fd , _filename , _flushMutex );
    }
    void MemoryMappedFile::_lock() {}
    void MemoryMappedFile::_unlock() {}

} 
