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

using namespace mongoutils;

namespace mongo {
    struct ___Test { 
        ___Test() { 
            if( 0 ) {
                try { 
                    LogFile f("foo");
                    char *buf = (char*) malloc(16384);
                    memset(buf, 'z', 16384);
                    buf[16382] = '\n';
                    buf[16383] = 'B';
                    buf[0] = 'A';
                    f.synchronousAppend(buf, 16384);
                    f.synchronousAppend(buf, 16384);
                }
                catch(DBException& e ) { 
                    log() << e.what() << endl;
                }
            }
        }
    } __test;
}

#if defined(_WIN32)

namespace mongo { 

    LogFile::LogFile(string name) {
        _fd = CreateFile(
            toNativeString(name.c_str()).c_str(),
            GENERIC_WRITE,
            FILE_SHARE_READ,
            NULL, 
            OPEN_ALWAYS,
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

    void LogFile::synchronousAppend(void *buf, size_t len) { 
        assert(_fd);
        DWORD written;
        if( !WriteFile(_fd, buf, len, &written, NULL) ) { 
            DWORD e = GetLastError();
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

    LogFile::LogFile(string name) {
        _fd = open(name.c_str(), 
                     O_APPEND 
                   | O_CREAT
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
    }

    void LogFile::synchronousAppend(void *buf, size_t len) { 
        assert(_fd);
        ssize_t written = write(_fd, buf, len);
        if( written != (ssize_t) len ) { 
            uasserted(13515, str::stream() << "error appending to file " << errnoWithDescription());
        }
#if !defined(O_SYNC)
        if( fdatasync(_fd) < 0 ) { 
            uasserted(13514, str::stream() << "error appending to file on fsync " << errnoWithDescription());
        }
#endif
    }

}

#endif
