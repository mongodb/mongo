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
#include "unittest.h"

using namespace mongoutils;

namespace mongo {
    struct LogfileTest : public UnitTest {
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

    LogFile::LogFile(string name) : _name(name) {
        _fd = CreateFile(
                  toNativeString(name.c_str()).c_str(),
                  GENERIC_WRITE,
                  FILE_SHARE_READ,
                  NULL,
                  CREATE_NEW, //OPEN_ALWAYS,
                  FILE_FLAG_NO_BUFFERING | FILE_FLAG_WRITE_THROUGH,
                  NULL);
        if( _fd == INVALID_HANDLE_VALUE ) {
            DWORD e = GetLastError();
            uasserted(13518, str::stream() << "couldn't open file " << name << " for writing " << errnoWithDescription(e));
        }
        SetFilePointer(_fd, 0, 0, FILE_END);
    }

    LogFile::~LogFile() {
        if( _fd != INVALID_HANDLE_VALUE )
            CloseHandle(_fd);
    }

    void LogFile::synchronousAppend(const void *buf, size_t len) {
        assert(_fd);
        DWORD written;
        if( !WriteFile(_fd, buf, len, &written, NULL) ) {
            DWORD e = GetLastError();
            if( e == 87 )
                massert(13519, "error appending to file - misaligned direct write?", false);
            else
                uasserted(13517, str::stream() << "error appending to file " << errnoWithDescription(e));
        }
        else {
            dassert( written == len );
        }
    }

}

#else

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

namespace mongo {

    LogFile::LogFile(string name) : _name(name) {
        _fd = open(name.c_str(),
                   O_APPEND
                   | O_CREAT | O_EXCL
                   | O_RDWR
#if defined(O_DIRECT)
                   | O_DIRECT
#endif
#if defined(O_NOATIME)
                   | O_NOATIME
#endif
#if defined(O_SYNC)
                   | O_SYNC
#endif
                   ,
                   S_IRUSR | S_IWUSR);
        if( _fd < 0 ) {
            uasserted(13516, str::stream() << "couldn't open file " << name << " for writing " << errnoWithDescription());
        }
    }

    LogFile::~LogFile() {
        if( _fd >= 0 )
            close(_fd);
        _fd = -1;
    }

    void LogFile::synchronousAppend(const void *b, size_t len) {
        const char *buf = (char *) b;
        assert(_fd);
        assert(((size_t)buf)%4096==0); // aligned
        if( len % 4096 != 0 ) {
            log() << len << ' ' << len % 4096 << endl;
            assert(false);
        }
        ssize_t written = write(_fd, buf, len);
        if( written != (ssize_t) len ) {
            log() << "write fails written:" << written << " len:" << len << " errno:" << errno << endl;
            uasserted(13515, str::stream() << "error appending to file " << _fd << errnoWithDescription());
        }
#if !defined(O_SYNC)
        if( fdatasync(_fd) < 0 ) {
            uasserted(13514, str::stream() << "error appending to file on fsync " << errnoWithDescription());
        }
#endif
    }

}

#endif
