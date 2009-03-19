// mmap_win.cpp

/**
*    Copyright (C) 2008 10gen Inc.
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


#include "stdafx.h"
#include "mmap.h"

namespace mongo {

    MemoryMappedFile::MemoryMappedFile() {
        fd = 0;
        maphandle = 0;
        view = 0;
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

    void* MemoryMappedFile::map(const char *_filename, int length) {
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
            out() << "CreateFile failed " << filename << endl;
            return 0;
        }
        if ( mapped > 500000000 )
            out() << "WARNING: too much mem mapped for win32" << endl;

        mapped += length;

        maphandle = CreateFileMapping(fd, NULL, PAGE_READWRITE, 0, length, NULL);
        if ( maphandle == NULL ) {
            out() << "CreateFileMapping failed " << filename << endl;
            return 0;
        }

        view = MapViewOfFile(maphandle, FILE_MAP_ALL_ACCESS, 0, 0, 0);
        if ( view == 0 ) {
            out() << "MapViewOfFile failed " << filename << " errno:";
            out() << GetLastError();
            out() << endl;
        }

        return view;
    }

    void MemoryMappedFile::flush(bool) {
    }

} 
