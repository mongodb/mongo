// processinfo_linux2.cpp

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

#include <malloc.h>
#include <iostream>
#include <stdio.h>
#include <unistd.h>
#include <sys/mman.h>
#include <gnu/libc-version.h>
#include <sys/utsname.h>

#include "processinfo.h"
#include "boost/filesystem.hpp"
#include <util/file.h>

using namespace std;

#define KLONG long
#define KLF "l"

namespace mongo {

    class LinuxProc {
    public:
        LinuxProc( pid_t pid = getpid() ) {
            char name[128];
            sprintf( name , "/proc/%d/stat"  , pid );

            FILE * f = fopen( name , "r");
            if ( ! f ) {
                stringstream ss;
                ss << "couldn't open [" << name << "] " << errnoWithDescription();
                string s = ss.str();
                // help the assert# control uasserted( 13538 , s.c_str() );
                msgassertedNoTrace( 13538 , s.c_str() );
            }
            int found = fscanf(f,
                               "%d %s %c "
                               "%d %d %d %d %d "
                               "%lu %lu %lu %lu %lu "
                               "%lu %lu %ld %ld "  /* utime stime cutime cstime */
                               "%ld %ld "
                               "%ld "
                               "%ld "
                               "%lu "  /* start_time */
                               "%lu "
                               "%ld " // rss
                               "%lu %"KLF"u %"KLF"u %"KLF"u %"KLF"u %"KLF"u "
                               /*
                                 "%*s %*s %*s %*s "
                                 "%"KLF"u %*lu %*lu "
                                 "%d %d "
                                 "%lu %lu"
                               */

                               ,

                               &_pid,
                               _comm,
                               &_state,
                               &_ppid, &_pgrp, &_session, &_tty, &_tpgid,
                               &_flags, &_min_flt, &_cmin_flt, &_maj_flt, &_cmaj_flt,
                               &_utime, &_stime, &_cutime, &_cstime,
                               &_priority, &_nice,
                               &_nlwp,
                               &_alarm,
                               &_start_time,
                               &_vsize,
                               &_rss,
                               &_rss_rlim, &_start_code, &_end_code, &_start_stack, &_kstk_esp, &_kstk_eip

                               /*
                                 &_wchan,
                                 &_exit_signal, &_processor,
                                 &_rtprio, &_sched
                               */
                              );
            if ( found == 0 ) {
                cout << "system error: reading proc info" << endl;
            }
            fclose( f );
        }

        unsigned long getVirtualMemorySize() {
            return _vsize;
        }

        unsigned long getResidentSize() {
            return (unsigned long)_rss * 4 * 1024;
        }

        int _pid;
        // The process ID.

        char _comm[128];
        // The filename of the executable, in parentheses.  This is visible whether or not the executable is swapped out.

        char _state;
        //One character from the string "RSDZTW" where R is running, S is sleeping in an interruptible wait, D is waiting  in  uninterruptible
        //  disk sleep, Z is zombie, T is traced or stopped (on a signal), and W is paging.

        int _ppid;
        // The PID of the parent.

        int _pgrp;
        // The process group ID of the process.

        int _session;
        // The session ID of the process.

        int _tty;
        // The tty the process uses.

        int _tpgid;
        // The process group ID of the process which currently owns the tty that the process is connected to.

        unsigned long _flags; // %lu
        // The  kernel flags word of the process. For bit meanings, see the PF_* defines in <linux/sched.h>.  Details depend on the kernel version.

        unsigned long _min_flt; // %lu
        // The number of minor faults the process has made which have not required loading a memory page from disk.

        unsigned long _cmin_flt; // %lu
        // The number of minor faults that the process

        unsigned long _maj_flt; // %lu
        // The number of major faults the process has made which have required loading a memory page from disk.

        unsigned long _cmaj_flt; // %lu
        // The number of major faults that the process

        unsigned long _utime; // %lu
        // The number of jiffies that this process has been scheduled in user mode.

        unsigned long _stime; //  %lu
        // The number of jiffies that this process has been scheduled in kernel mode.

        long _cutime; // %ld
        // The number of jiffies that this removed field.

        long _cstime; // %ld

        long _priority;
        long _nice;

        long _nlwp; // %ld
        // number of threads

        unsigned long _alarm;
        // The time in jiffies before the next SIGALRM is sent to the process due to an interval timer. (unused since 2.6.17)

        unsigned long _start_time; // %lu
        // The time in jiffies the process started after system boot.

        unsigned long _vsize; // %lu
        // Virtual memory size in bytes.

        long _rss; // %ld
        // Resident Set Size: number of pages the process has in real memory, minus 3 for administrative purposes. This is just the pages which
        // count  towards  text,  data, or stack space.  This does not include pages which have not been demand-loaded in, or which are swapped out

        unsigned long _rss_rlim; // %lu
        // Current limit in bytes on the rss of the process (usually 4294967295 on i386).

        unsigned long _start_code; // %lu
        // The address above which program text can run.

        unsigned long _end_code; // %lu
        // The address below which program text can run.

        unsigned long _start_stack; // %lu
        // The address of the start of the stack.

        unsigned long _kstk_esp; // %lu
        // The current value of esp (stack pointer), as found in the kernel stack page for the process.

        unsigned long _kstk_eip; // %lu
        // The current EIP (instruction pointer).



    };


    class LinuxSysHelper {
    public:

        /**
        * Read the first 1023 bytes from a file
        */
        static string readLineFromFile( const char* fname ) {
            FILE* f;
            char fstr[1024] = { 0 };

            f = fopen( fname, "r" );
            if ( f != NULL ) {
                if ( fgets( fstr, 1023, f ) != NULL )
                    fstr[strlen( fstr ) < 1 ? 0 : strlen( fstr ) - 1] = '\0';
                fclose( f );
            }
            return fstr;
        }

        /**
        * Get some details about the CPU
        */
        static void getCpuInfo(int& procCount, string& freq, string& features) {
            FILE* f;
            char fstr[1024] = { 0 };
            procCount = 0;

            f = fopen( "/proc/cpuinfo", "r" );
            if ( f == NULL )
                return;

            while ( fgets( fstr, 1023, f ) != NULL && !feof(f) ) {
                // until the end of the file
                fstr[strlen( fstr ) < 1 ? 0 : strlen( fstr ) - 1] = '\0';                    
                if (strncmp(fstr, "processor\t:", 11) == 0)
                    ++procCount;
                if (strncmp(fstr, "cpu MHz\t\t:", 10) == 0)
                    freq = fstr + 11;
                if (strncmp(fstr, "flags\t\t:", 8) == 0)
                    features = fstr + 9;
            }

            fclose( f );
        }

        /**
        * Determine linux distro and version
        */
        static void getLinuxDistro( string& name, string& version ) {
            char buf[4096] = { 0 };

            // try lsb file first
            if ( boost::filesystem::exists( "/etc/lsb-release" ) ) {
                File f;
                f.open( "/etc/lsb-release", true );
                if ( ! f.is_open() || f.bad() )
                    return;
                f.read( 0, buf, f.len() > 4095 ? 4095 : f.len() );

                // find the distribution name and version in the contents.
                // format:  KEY=VAL\n
                string contents = buf;
                unsigned lineCnt = 0;
                try {
                    while ( lineCnt < contents.length() - 1 && contents.substr( lineCnt ).find( '\n' ) != string::npos ) {
                        // until we hit the last newline or eof
                        string line = contents.substr( lineCnt, contents.substr( lineCnt ).find( '\n' ) );
                        lineCnt += contents.substr( lineCnt ).find( '\n' ) + 1;
                        size_t delim = line.find( '=' );
                        string key = line.substr( 0, delim );
                        string val = line.substr( delim + 1 );  // 0-based offset of delim
                        if ( key.compare( "DISTRIB_ID" ) == 0 )
                            name = val;
                        if ( string(key).compare( "DISTRIB_RELEASE" ) == 0 )
                            version = val;
                    }
                }
                catch (const std::out_of_range &e) {
                    // attempted to get invalid substr
                }
                return; // return with lsb-relase data
            }

            // try known flat-text file locations
            // format: Slackware-x86_64 13.0, Red Hat Enterprise Linux Server release 5.6 (Tikanga), etc.
            typedef vector <string> pathvec;
            pathvec paths;
            pathvec::const_iterator i;
            bool found = false;
            paths.push_back( "/etc/system-release" );
            paths.push_back( "/etc/redhat-release" );
            paths.push_back( "/etc/gentoo-release" );
            paths.push_back( "/etc/novell-release" );
            paths.push_back( "/etc/gentoo-release" );
            paths.push_back( "/etc/SuSE-release" );
            paths.push_back( "/etc/SUSE-release" );
            paths.push_back( "/etc/sles-release" );
            paths.push_back( "/etc/debian_release" );
            paths.push_back( "/etc/slackware-version" );
        
            for ( i = paths.begin(); i != paths.end(); ++i ) {
                // for each path
                if ( boost::filesystem::exists( *i ) ) {
                    // if the file exists, break
                    found = true;
                    break;
                }
            }

            if ( found ) {
                // found a file
                File f;
                f.open( i->c_str(), true );
                if ( ! f.is_open() || f.bad() )
                    // file exists but can't be opened
                    return;

                // read up to 512 bytes
                int len = f.len() > 512 ? 512 : f.len();
                f.read( 0, buf, len );
                buf[ len ] = '\0';
                name = buf;
                size_t nl = 0;
                if ( ( nl = name.find( '\n', nl ) ) != string::npos )
                    // stop at first newline
                    name.erase( nl );
                // no standard format for name and version.  use kernel version
                version = "Kernel ";
                version += LinuxSysHelper::readLineFromFile("/proc/sys/kernel/osrelease");
            }
        }

        /**
        * Get system memory total
        */
        static unsigned long long getSystemMemorySize() {
            string meminfo = readLineFromFile( "/proc/meminfo" );
            size_t lineOff= 0;
            if ( !meminfo.empty() && ( lineOff = meminfo.find( "MemTotal" ) ) != string::npos ) {
                // found MemTotal line.  capture everything between 'MemTotal:' and ' kB'.
                lineOff = meminfo.substr( lineOff ).find( ':' ) + 1;
                meminfo = meminfo.substr( lineOff, meminfo.substr( lineOff ).find( "kB" ) - 1);
                lineOff = 0;

                // trim whitespace and append 000 to replace kB.
                while ( isspace( meminfo.at( lineOff ) ) ) lineOff++;
                meminfo = meminfo.substr( lineOff );
            }
            else {
                meminfo = "";
            }
            return atoll(meminfo.c_str()) * 1024;   // convert from kB to bytes
        }

    };


    ProcessInfo::ProcessInfo( pid_t pid ) : _pid( pid ) {
    }

    ProcessInfo::~ProcessInfo() {
    }

    bool ProcessInfo::supported() {
        return true;
    }

    int ProcessInfo::getVirtualMemorySize() {
        LinuxProc p(_pid);
        return (int)( p.getVirtualMemorySize() / ( 1024.0 * 1024 ) );
    }

    int ProcessInfo::getResidentSize() {
        LinuxProc p(_pid);
        return (int)( p.getResidentSize() / ( 1024.0 * 1024 ) );
    }

    void ProcessInfo::getExtraInfo( BSONObjBuilder& info ) {
        // [dm] i don't think mallinfo works. (64 bit.)  ??
        struct mallinfo malloc_info = mallinfo(); // structure has same name as function that returns it. (see malloc.h)
        info.append("heap_usage_bytes", malloc_info.uordblks/*main arena*/ + malloc_info.hblkhd/*mmap blocks*/);
        //docs claim hblkhd is included in uordblks but it isn't

        LinuxProc p(_pid);
        info.append("page_faults", (int)p._maj_flt);
    }

    /**
    * Save a BSON obj representing the host system's details
    */
    void ProcessInfo::SystemInfo::collectSystemInfo() {
        utsname unameData;
        string distroName, distroVersion;
        string cpuFreq, cpuFeatures;
        int cpuCount;

        string verSig = LinuxSysHelper::readLineFromFile( "/proc/version_signature" );
        LinuxSysHelper::getCpuInfo(cpuCount, cpuFreq, cpuFeatures);
        LinuxSysHelper::getLinuxDistro( distroName, distroVersion );

        if ( uname( &unameData ) == -1 ) {
            log() << "Unable to collect detailed system information: " << strerror( errno ) << endl;
        }

        osType = "Linux";
        osName = distroName;
        osVersion = distroVersion;
        memSize = LinuxSysHelper::getSystemMemorySize();
        addrSize = (string( unameData.machine ).find( "x86_64" ) != string::npos ? 64 : 32);
        numCores = cpuCount;
        cpuArch = unameData.machine;
        hasNuma = checkNumaEnabled();
        
        BSONObjBuilder bExtra;
        bExtra.append( "versionString", LinuxSysHelper::readLineFromFile( "/proc/version" ) );
        bExtra.append( "libcVersion", gnu_get_libc_version() );
        if (!verSig.empty())
            // optional
            bExtra.append( "versionSignature", verSig );

        bExtra.append( "kernelVersion", unameData.release );
        bExtra.append( "cpuFrequencyMHz", cpuFreq);
        bExtra.append( "cpuFeatures", cpuFeatures);
        bExtra.append( "pageSize", static_cast< int >(sysconf( _SC_PAGESIZE ) ) );
        bExtra.append( "numPages", static_cast< int >(sysconf( _SC_PHYS_PAGES ) ) );
        bExtra.append( "maxOpenFiles", static_cast< int >(sysconf( _SC_OPEN_MAX ) ) );

        _extraStats = bExtra.obj();

    }

    /**
    * Determine if the process is running with (cc)NUMA
    */
    bool ProcessInfo::checkNumaEnabled() {
        if ( boost::filesystem::exists( "/sys/devices/system/node/node1" ) && 
             boost::filesystem::exists( "/proc/self/numa_maps" ) ) {
            // proc is populated with numa entries

            // read the second column of first line to determine numa state
            // ('default' = enabled, 'interleave' = disabled).  Logic from version.cpp's warnings.
            string line = LinuxSysHelper::readLineFromFile( "/proc/self/numa_maps" ).append( " \0" );
            size_t pos = line.find(' ');
            if ( pos != string::npos && line.substr( pos+1, 10 ).find( "interleave" ) == string::npos )
                // interleave not found; 
                return true;
        }
        return false;
    }

    bool ProcessInfo::blockCheckSupported() {
        return true;
    }

    bool ProcessInfo::blockInMemory( char * start ) {
        static long pageSize = 0;
        if ( pageSize == 0 ) {
            pageSize = sysconf( _SC_PAGESIZE );
        }
        start = start - ( (unsigned long long)start % pageSize );
        unsigned char x = 0;
        if ( mincore( start , 128 , &x ) ) {
            log() << "mincore failed: " << errnoWithDescription() << endl;
            return 1;
        }
        return x & 0x1;
    }


}
