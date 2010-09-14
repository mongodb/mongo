// file.h

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

#pragma once

#if !defined(_WIN32)
#include "errno.h"
#include <sys/mman.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#else
#include <windows.h>
#endif

#include "text.h"

namespace mongo { 

#ifndef __sunos__
typedef uint64_t fileofs;
#else
typedef boost::uint64_t fileofs;
#endif

class FileInterface { 
public:
    void open(const char *fn) {}
    void write(fileofs o, const char *data, unsigned len) {}
    void read(fileofs o, char *data, unsigned len) {}
    bool bad() {return false;}
    bool is_open() {return false;}
    fileofs len() { return 0; }
};

#if defined(_WIN32) 
#include <io.h>

class File : public FileInterface { 
    HANDLE fd;
    bool _bad;
    void err(BOOL b=false) { /* false = error happened */
        if( !b && !_bad ) { 
            _bad = true;
            log() << "File I/O error " << GetLastError() << '\n';
        }
    }
public:
    File() { 
        fd = INVALID_HANDLE_VALUE; 
        _bad = true; 
    }
    ~File() { 
        if( is_open() ) CloseHandle(fd);
        fd = INVALID_HANDLE_VALUE; 
    }
    void open(const char *filename, bool readOnly=false ) {
        fd = CreateFile(
                 toNativeString(filename).c_str(),
                 ( readOnly ? 0 : GENERIC_WRITE ) | GENERIC_READ, FILE_SHARE_READ,
                 NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if( !is_open() ) {
             out() << "CreateFile failed " << filename << endl;
        }
        else 
            _bad = false;
    }
    void write(fileofs o, const char *data, unsigned len) {
        LARGE_INTEGER li;
        li.QuadPart = o;
        SetFilePointerEx(fd, li, NULL, FILE_BEGIN);
        DWORD written;
        err( WriteFile(fd, data, len, &written, NULL) );
    }
    void read(fileofs o, char *data, unsigned len) {
        DWORD read;
        LARGE_INTEGER li;
        li.QuadPart = o;
        SetFilePointerEx(fd, li, NULL, FILE_BEGIN);
        int ok = ReadFile(fd, data, len, &read, 0);
        if( !ok ) 
            err(ok);
        else
            massert( 10438 , "ReadFile error - truncated file?", read == len);
    }
    bool bad() { return _bad; }
    bool is_open() { return fd != INVALID_HANDLE_VALUE; }
    fileofs len() {
        LARGE_INTEGER li;
        li.LowPart = GetFileSize(fd, (DWORD *) &li.HighPart);
        if( li.HighPart == 0 && li.LowPart == INVALID_FILE_SIZE ) {
            err( false );
            return 0;
        }
        return li.QuadPart;
    }
    void fsync() { FlushFileBuffers(fd); }
};

#else

class File : public FileInterface { 
    int fd;
    bool _bad;
    void err(bool ok) {
        if( !ok && !_bad ) { 
            _bad = true;
            log() << "File I/O " << errnoWithDescription() << '\n';
        }
    }
public:
    File() { 
        fd = -1;
        _bad = true; 
    }
    ~File() { 
        if( is_open() ) ::close(fd);
        fd = -1;
    }

#ifndef O_NOATIME
#define O_NOATIME 0
#define lseek64 lseek
#endif

    void open(const char *filename, bool readOnly=false ) {
        fd = ::open(filename, 
                    O_CREAT | ( readOnly ? 0 : ( O_RDWR | O_NOATIME ) ) ,
                    S_IRUSR | S_IWUSR);
        if ( fd <= 0 ) {
            out() << "couldn't open " << filename << ' ' << errnoWithDescription() << endl;
            return;
        }
        _bad = false;
    }
    void write(fileofs o, const char *data, unsigned len) {
        lseek64(fd, o, SEEK_SET);
        err( ::write(fd, data, len) == (int) len );
    }
    void read(fileofs o, char *data, unsigned len) {
        lseek(fd, o, SEEK_SET);
        err( ::read(fd, data, len) == (int) len );
    }
    bool bad() { return _bad; }
    bool is_open() { return fd > 0; }
    fileofs len() {
        return lseek(fd, 0, SEEK_END);
    }
    void fsync() { ::fsync(fd); }
};


#endif


}

