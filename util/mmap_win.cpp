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

#include "stdafx.h"
#include "mmap.h"
#include <windows.h>

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
            UnmapViewOfFile(view);
        view = 0;
        if ( maphandle )
            CloseHandle(maphandle);
        maphandle = 0;
        if ( fd )
            CloseHandle(fd);
        fd = 0;
    }

    std::wstring toWideString(const char *s) {
        std::basic_ostringstream<TCHAR> buf;
        buf << s;
        return buf.str();
    }

    unsigned mapped = 0;

    void* MemoryMappedFile::map(const char *_filename, long &length) {
        /* big hack here: Babble uses db names with colons.  doesn't seem to work on windows.  temporary perhaps. */
        char filename[256];
        strncpy(filename, _filename, 255);
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
        std::wstring filenamew = toWideString(filename);

        fd = CreateFile(
                 filenamew.c_str(), GENERIC_WRITE | GENERIC_READ, FILE_SHARE_READ,
                 NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if ( fd == INVALID_HANDLE_VALUE ) {
            out() << "Create/OpenFile failed " << filename << ' ' << GetLastError() << endl;
            return 0;
        }

        mapped += length;

        maphandle = CreateFileMapping(fd, NULL, PAGE_READWRITE, 0, length, NULL);
        if ( maphandle == NULL ) {
            out() << "CreateFileMapping failed " << filename << ' ' << GetLastError() << endl;
            return 0;
        }

        view = MapViewOfFile(maphandle, FILE_MAP_ALL_ACCESS, 0, 0, 0);
        if ( view == 0 ) {
            out() << "MapViewOfFile failed " << filename << " errno:";
            out() << GetLastError();
            out() << endl;
        }
        len = length;
        return view;
    }

    void MemoryMappedFile::flush(bool) {
    }

} 
