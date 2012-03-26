// file.h cross platform basic file class. supports 64 bit offsets and such.

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
#include <sys/statvfs.h>
#endif
#include "text.h"

namespace mongo {

#ifndef __sunos__
    typedef uint64_t fileofs;
#else
    typedef boost::uint64_t fileofs;
#endif

    /* NOTE: not thread-safe. (at least the windows implementation isn't. */

    class FileInterface {
    public:
        void open(const char *fn) {}
        void write(fileofs o, const char *data, unsigned len) {}
        void read(fileofs o, char *data, unsigned len) {}
        bool bad() {return false;}
        bool is_open() {return false;}
        fileofs len() { return 0; }
        void fsync() { verify(false); }

        // shrink file to size bytes. No-op if file already smaller.
        void truncate(fileofs size);

        /** @return  -1 if error or unavailable */
        static boost::intmax_t freeSpace(const string &path) { verify(false); return -1; }
    };

#if defined(_WIN32)
#include <io.h>

    class File : public FileInterface {
        HANDLE fd;
        bool _bad;
        string _name;
        void err(BOOL b=false) { /* false = error happened */
            if( !b && !_bad ) {
                _bad = true;
                log() << "File " << _name << "I/O error " << GetLastError() << '\n';
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
        void open(const char *filename, bool readOnly=false , bool direct=false) {
            _name = filename;
            fd = CreateFile(
                     toNativeString(filename).c_str(),
                     ( readOnly ? 0 : GENERIC_WRITE ) | GENERIC_READ, FILE_SHARE_WRITE|FILE_SHARE_READ,
                     NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
            if( !is_open() ) {
                DWORD e = GetLastError();
                log() << "Create/Open File failed " << filename << ' ' << errnoWithDescription(e) << endl;
            }
            else
                _bad = false;
        }
        static boost::intmax_t freeSpace(const string &path) {
            ULARGE_INTEGER avail;
            if( GetDiskFreeSpaceEx(toNativeString(path.c_str()).c_str(), &avail, NULL, NULL) ) { 
                return avail.QuadPart;
            }
            DWORD e = GetLastError();
            log() << "GetDiskFreeSpaceEx fails errno: " << e << endl;
            return -1;
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

        void truncate(fileofs size) {
            if (len() <= size)
                return;

            LARGE_INTEGER li;
            li.QuadPart = size;
            if (SetFilePointerEx(fd, li, NULL, FILE_BEGIN) == 0){
                err(false);
                return; //couldn't seek
            }

            err(SetEndOfFile(fd));
        }
    };

#else

    class File : public FileInterface {
    public:
        int fd;
    private:
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
#endif

        void open(const char *filename, bool readOnly=false , bool direct=false) {
            fd = ::open(filename,
                        O_CREAT | ( readOnly ? 0 : ( O_RDWR | O_NOATIME ) ) 
#if defined(O_DIRECT)
                        | ( direct ? O_DIRECT : 0 ) 
#endif
                        ,
                        S_IRUSR | S_IWUSR);
            if ( fd <= 0 ) {
                out() << "couldn't open " << filename << ' ' << errnoWithDescription() << endl;
                return;
            }
            _bad = false;
        }
        void write(fileofs o, const char *data, unsigned len) {
            err( ::pwrite(fd, data, len, o) == (int) len );
        }
        void read(fileofs o, char *data, unsigned len) {
            ssize_t s = ::pread(fd, data, len, o);
            if( s == -1 ) {
                err(false);
            }
            else if( s != (int) len ) { 
                _bad = true;
                log() << "File error read:" << s << " bytes, wanted:" << len << " ofs:" << o << endl;
            }
        }
        bool bad() { return _bad; }
        bool is_open() { return fd > 0; }
        fileofs len() {
            off_t o = lseek(fd, 0, SEEK_END);
            if( o != (off_t) -1 )
                return o;
            err(false);
            return 0;
        }
        void fsync() { ::fsync(fd); }
        static boost::intmax_t freeSpace ( const string &path ) {
            struct statvfs info;
            verify( !statvfs( path.c_str() , &info ) );
            return boost::intmax_t( info.f_bavail ) * info.f_frsize;
        }

        void truncate(fileofs size) {
            if (len() <= size)
                return;

            err(ftruncate(fd, size) == 0);
        }
    };


#endif


}

