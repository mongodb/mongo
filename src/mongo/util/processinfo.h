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

#include <db/jsobj.h>

namespace mongo {

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
         * Get the type of os (e.g. Windows, Linux, Mac OS)
         */
        const string& getOsType() const { return sysInfo().osType; }

        /**
         * Get the os Name (e.g. Ubuntu, Gentoo, Windows Server 2008)
         */
        const string& getOsName() const { return sysInfo().osName; }

        /**
         * Get the os version (e.g. 10.04, 11.3.0, 6.1 (build 7600))
         */
        const string& getOsVersion() const { return sysInfo().osVersion; }

        /**
         * Get the cpu address size (e.g. 32, 36, 64)
         */
        const unsigned getAddrSize() const { return sysInfo().addrSize; }

        /**
         * Get the total amount of system memory in MB
         */
        const unsigned long long getMemSizeMB() const { return sysInfo().memSize / (1024 * 1024); }

        /**
         * Get the number of CPUs
         */
        const unsigned getNumCores() const { return sysInfo().numCores; }

        /**
         * Get the CPU architecture (e.g. x86, x86_64)
         */
        const string& getArch() const { return sysInfo().cpuArch; }

        /**
         * Determine if NUMA is enabled (interleaved) for this process
         */
        bool hasNumaEnabled() const { return sysInfo().hasNuma; }

        /**
         * Get extra system stats
         */
        void appendSystemDetails( BSONObjBuilder& details ) const {
            details.append( StringData("extra"), sysInfo()._extraStats.copy() );
        }

        /**
         * Append platform-specific data to obj
         */
        void getExtraInfo( BSONObjBuilder& info );

        bool supported();

        static bool blockCheckSupported();

        static bool blockInMemory( char * start );

    private:
        /**
         * Host and operating system info.  Does not change over time.
         */
        class SystemInfo {
        public:
            string osType;
            string osName;
            string osVersion;
            unsigned addrSize;
            unsigned long long memSize;
            unsigned numCores;
            string cpuArch;
            bool hasNuma;
            BSONObj _extraStats;
            SystemInfo() :
                    addrSize( 0 ),
                    memSize( 0 ),
                    numCores( 0 ),
                    hasNuma( false ) { 
                // populate SystemInfo during construction
                collectSystemInfo();
            }
        private:
            /** Collect host system info */
            void collectSystemInfo();
        };

        pid_t _pid;
        static mongo::mutex _sysInfoLock;

        static bool checkNumaEnabled();

        const SystemInfo& sysInfo() const {
            // initialize and collect sysInfo on first call
            // TODO: SERVER-5112
            static ProcessInfo::SystemInfo *initSysInfo = NULL;
            if ( ! initSysInfo ) {
                scoped_lock lk( _sysInfoLock );
                if ( ! initSysInfo ) {
                    initSysInfo = new SystemInfo();
                }
            }
            return *initSysInfo;
        }

    };

    void writePidFile( const std::string& path );

    void printMemInfo( const char * whereContextStr = 0 );

}
