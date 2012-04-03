// @file logfile.cpp simple file log writing / journaling

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

#include "pch.h"
#include "logfile.h"
#include "text.h"
#include "mongoutils/str.h"
#include "mongo/util/startup_test.h"

using namespace mongoutils;

namespace mongo {
    struct LogfileTest : public StartupTest {
        LogfileTest() { }
        void run() {
            if( 0 && debug ) {
                try {
                    LogFile f("logfile_test");
                    void *p = malloc(16384);
                    char *buf = (char*) p;
                    buf += 4095;
                    buf = (char*) (((size_t)buf)&(~0xfff));
                    memset(buf, 'z', 8192);
                    buf[8190] = '\n';
                    buf[8191] = 'B';
                    buf[0] = 'A';
                    f.synchronousAppend(buf, 8192);
                    f.synchronousAppend(buf, 8192);
                    free(p);
                }
                catch(DBException& e ) {
                    log() << "logfile.cpp test failed : " << e.what() << endl;
                    throw;
                }
            }
        }
    } __test;
}

#if defined(_WIN32)

namespace mongo {

    LogFile::LogFile(string name, bool readwrite) : _name(name) {
        _fd = CreateFile(
                  toNativeString(name.c_str()).c_str(),
                  (readwrite?GENERIC_READ:0)|GENERIC_WRITE,
                  FILE_SHARE_READ,
                  NULL,
                  OPEN_ALWAYS,
                  FILE_FLAG_NO_BUFFERING | FILE_FLAG_WRITE_THROUGH,
                  NULL);
        if( _fd == INVALID_HANDLE_VALUE ) {
            DWORD e = GetLastError();
            uasserted(13518, str::stream() << "couldn't open file " << name << " for writing " << errnoWithDescription(e));
        }
        SetFilePointer(_fd, 0, 0, FILE_BEGIN);
    }

    LogFile::~LogFile() {
        if( _fd != INVALID_HANDLE_VALUE )
            CloseHandle(_fd);
    }

    void LogFile::truncate() {
        verify(_fd != INVALID_HANDLE_VALUE);

        if (!SetEndOfFile(_fd)){
            msgasserted(15871, "Couldn't truncate file: " + errnoWithDescription());
        }
    }

    void LogFile::writeAt(unsigned long long offset, const void *_buf, size_t _len) { 
// TODO 64 bit offsets
        OVERLAPPED o;
        memset(&o,0,sizeof(o));
        (unsigned long long&) o.Offset = offset; 
        BOOL ok= WriteFile(_fd, _buf, _len, 0, &o);
        verify(ok);
    }

    void LogFile::readAt(unsigned long long offset, void *_buf, size_t _len) { 
// TODO 64 bit offsets
        OVERLAPPED o;
        memset(&o,0,sizeof(o));
        (unsigned long long&) o.Offset = offset;
        DWORD nr;
        BOOL ok = ReadFile(_fd, _buf, _len, &nr, &o);
        if( !ok ) {
            string e = errnoWithDescription();
            //DWORD e = GetLastError();
            log() << "LogFile readAt(" << offset << ") len:" << _len << "errno:" << e << endl;
            verify(false);
        }
    }

    void LogFile::synchronousAppend(const void *_buf, size_t _len) {
        const size_t BlockSize = 8 * 1024 * 1024;
        verify(_fd);
        verify(_len % 4096 == 0);
        const char *buf = (const char *) _buf;
        size_t left = _len;
        while( left ) {
            size_t toWrite = min(left, BlockSize);
            DWORD written;
            if( !WriteFile(_fd, buf, toWrite, &written, NULL) ) {
                DWORD e = GetLastError();
                if( e == 87 )
                    msgasserted(13519, "error 87 appending to file - invalid parameter");
                else
                    uasserted(13517, str::stream() << "error appending to file " << _name << ' ' << _len << ' ' << toWrite << ' ' << errnoWithDescription(e));
            }
            else {
                dassert( written == toWrite );
            }
            left -= written;
            buf += written;
        }
    }

}

#else

/// posix

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include "paths.h"

namespace mongo {

    LogFile::LogFile(string name, bool readwrite) : _name(name) {
        int options = O_CREAT
                    | (readwrite?O_RDWR:O_WRONLY)
#if defined(O_DIRECT)
                    | O_DIRECT
#endif
#if defined(O_NOATIME)
                    | O_NOATIME
#endif
                    ;

        _fd = open(name.c_str(), options, S_IRUSR | S_IWUSR);

#if defined(O_DIRECT)
        _direct = true;
        if( _fd < 0 ) {
            _direct = false;
            options &= ~O_DIRECT;
            _fd = open(name.c_str(), options, S_IRUSR | S_IWUSR);
        }
#else
        _direct = false;
#endif

        if( _fd < 0 ) {
            uasserted(13516, str::stream() << "couldn't open file " << name << " for writing " << errnoWithDescription());
        }

        flushMyDirectory(name);
    }

    LogFile::~LogFile() {
        if( _fd >= 0 )
            close(_fd);
        _fd = -1;
    }

    void LogFile::truncate() {
        verify(_fd >= 0);

        BOOST_STATIC_ASSERT(sizeof(off_t) == 8); // we don't want overflow here
        const off_t pos = lseek(_fd, 0, SEEK_CUR); // doesn't actually seek
        if (ftruncate(_fd, pos) != 0){
            msgasserted(15873, "Couldn't truncate file: " + errnoWithDescription());
        }

        fsync(_fd);
    }

    void LogFile::writeAt(unsigned long long offset, const void *buf, size_t len) { 
        verify(((size_t)buf)%4096==0); // aligned
        ssize_t written = pwrite(_fd, buf, len, offset);
        if( written != (ssize_t) len ) {
            log() << "writeAt fails " << errnoWithDescription() << endl;
        }
#if defined(__linux__)
        fdatasync(_fd);
#else
        fsync(_fd);
#endif
    }

    void LogFile::readAt(unsigned long long offset, void *_buf, size_t _len) { 
        verify(((size_t)_buf)%4096==0); // aligned
        ssize_t rd = pread(_fd, _buf, _len, offset);
        verify( rd != -1 );
    }

    void LogFile::synchronousAppend(const void *b, size_t len) {

        const char *buf = static_cast<const char *>( b );
        ssize_t charsToWrite = static_cast<ssize_t>( len );

        fassert( 16144, charsToWrite >= 0 );
        fassert( 16142, _fd >= 0 );
        fassert( 16143, reinterpret_cast<ssize_t>( buf ) % 4096 == 0 );  // aligned

#ifdef POSIX_FADV_DONTNEED
        const off_t pos = lseek(_fd, 0, SEEK_CUR); // doesn't actually seek, just get current position
#endif

        while ( charsToWrite > 0 ) {
            const ssize_t written = write( _fd, buf, static_cast<size_t>( charsToWrite ) );
            if ( -1 == written ) {
                log() << "LogFile::synchronousAppend failed with " << charsToWrite
                      << " bytes unwritten out of " << len << " bytes;  b=" << b << ' '
                      << errnoWithDescription() << std::endl;
                fassertFailed( 13515 );
            }
            buf += written;
            charsToWrite -= written;
        }

        if( 
#if defined(__linux__)
           fdatasync(_fd) < 0 
#else
           fsync(_fd)
#endif
            ) {
            log() << "error appending to file on fsync " << ' ' << errnoWithDescription();
            fassertFailed( 13514 );
        }

#ifdef POSIX_FADV_DONTNEED
        if (!_direct)
            posix_fadvise(_fd, pos, len, POSIX_FADV_DONTNEED);
#endif
    }

}

#endif
