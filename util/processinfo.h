// processinfo.h

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

#include <sys/types.h>
#include <string>

#ifndef _WIN32
#include <unistd.h>
#else
typedef int pid_t;
int getpid();
#endif

namespace mongo {

    class BSONObjBuilder;

    class ProcessInfo {
    public:
        ProcessInfo( pid_t pid = getpid() );
        ~ProcessInfo();

        /**
         * @return mbytes
         */
        int getVirtualMemorySize();

        /**
         * @return mbytes
         */
        int getResidentSize();

        /**
         * Append platform-specific data to obj
         */
        void getExtraInfo(BSONObjBuilder& info);

        bool supported();

        static bool blockCheckSupported();
        static bool blockInMemory( char * start );

    private:
        pid_t _pid;
    };

    void writePidFile( const std::string& path );

    void printMemInfo( const char * whereContextStr = 0 );

}
