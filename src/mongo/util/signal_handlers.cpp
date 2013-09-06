// signal_handlers.cpp

/**
*    Copyright (C) 2010 10gen Inc.
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
*
*    As a special exception, the copyright holders give permission to link the
*    code of portions of this program with the OpenSSL library under certain
*    conditions as described in each individual source file and distribute
*    linked combinations including the program with the OpenSSL library. You
*    must comply with the GNU Affero General Public License in all respects
*    for all of the code used other than as permitted herein. If you modify
*    file(s) with this exception, you may extend this exception to your
*    version of the file(s), but you are not obligated to do so. If you do not
*    wish to do so, delete this exception statement from your version. If you
*    delete this exception statement from all source files in the program,
*    then also delete it in the license file.
*/

#include "mongo/pch.h"

#include <cstdarg>
#include <cstdio>
#include <cstdlib>

#if !defined(_WIN32)  // TODO: windows support
#include <unistd.h>
#endif

#include "mongo/platform/backtrace.h"
#include "mongo/util/log.h"
#include "mongo/util/signal_handlers.h"

namespace mongo {

    /*
     * WARNING: PLEASE READ BEFORE CHANGING THIS MODULE
     *
     * All code in this module must be signal-friendly. Before adding any system
     * call or other dependency, please make sure that this still holds.
     *
     */

    static int rawWrite( int fd , char* c , int size ) {
#if !defined(_WIN32)

        int toWrite = size;
        int writePos = 0;
        int wrote;
        while ( toWrite > 0 ) {
            wrote = write( fd , &c[writePos] , toWrite );
            if ( wrote < 1 ) break;
            toWrite -= wrote;
            writePos += wrote;
        }
        return writePos;

#else

        return -1;

#endif
    }

    static int formattedWrite( int fd , const char* format, ... ) {
        const int MAX_ENTRY = 256;
        static char entryBuf[MAX_ENTRY];

        va_list ap;
        va_start( ap , format );
        int entrySize = vsnprintf( entryBuf , MAX_ENTRY-1 , format , ap );
        if ( entrySize < 0 ) {
            return -1;
        }

        if ( rawWrite( fd , entryBuf , entrySize ) < 0 ) {
            return -1;
        }

        return 0;
    }

    static void formattedBacktrace( int fd ) {

#if !defined(_WIN32)

        int numFrames;
        const int MAX_DEPTH = 20;
        void* stackFrames[MAX_DEPTH];

        numFrames = backtrace( stackFrames , 20 );
        for ( int i = 0; i < numFrames; i++ ) {
            formattedWrite( fd , "%p " , stackFrames[i] );
        }
        formattedWrite( fd , "\n" );

        backtrace_symbols_fd( stackFrames , numFrames , fd );

#else

        formattedWrite( fd, "backtracing not implemented for this platform yet\n" );

#endif

    }

    void printStackAndExit( int signalNum ) {
        const int fd = 0;

        formattedWrite( fd , "Received signal %d\n" , signalNum );
        formattedWrite( fd , "Backtrace: " );
        formattedBacktrace( fd );
        formattedWrite( fd , "===\n" );

        ::_exit( EXIT_ABRUPT );
    }

} // namespace mongo
