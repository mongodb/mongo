// @file version.cpp

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

#include "pch.h"
#include <cstdlib>
#include <iostream>
#include <iomanip>
#include <sstream>
#include <string>
#include <fstream>
#include "mongo/util/startup_test.h"
#include "version.h"
#include "stringutils.h"
#include "../db/jsobj.h"
#include "file.h"
#include "ramlog.h"
#include "../db/cmdline.h"
#include "processinfo.h"
#include "mongo/db/pdfile.h"

#include <boost/filesystem/operations.hpp>

namespace mongo {

    /* Approved formats for versionString:
     *      1.2.3
     *      1.2.3-pre-
     *      1.2.3-rc4 (up to rc9)
     *      1.2.3-rc4-pre-
     * If you really need to do something else you'll need to fix _versionArray()
     */
    const char versionString[] = "2.1.1-pre-";

    // See unit test for example outputs
    static BSONArray _versionArray(const char* version){
        // this is inefficient, but cached so it doesn't matter
        BSONArrayBuilder b;
        string curPart;
        const char* c = version;
        int finalPart = 0; // 0 = final release, -100 = pre, -10 to -1 = -10 + X for rcX
        do { //walks versionString including NUL byte
            if (!(*c == '.' || *c == '-' || *c == '\0')){
                curPart += *c;
                continue;
            }

            try {
                unsigned num = stringToNum(curPart.c_str());
                b.append((int) num);
            }
            catch (...){ // not a number
                if (curPart.empty()){
                    verify(*c == '\0');
                    break;
                }
                else if (startsWith(curPart, "rc")){
                    finalPart = -10 + stringToNum(curPart.c_str()+2);
                    break;
                }
                else if (curPart == "pre"){
                    finalPart = -100;
                    break;
                }
            }

            curPart = "";
        } while (*c++);

        b.append(finalPart);
        return b.arr();
    }

    const BSONArray versionArray = _versionArray(versionString);

    string mongodVersion() {
        stringstream ss;
        ss << "db version v" << versionString << ", pdfile version " << PDFILE_VERSION << "." << PDFILE_VERSION_MINOR;
        return ss.str();
    }

#ifndef _SCONS
    // only works in scons
    const char * gitVersion() { return "not-scons"; }
#endif

    void printGitVersion() { log() << "git version: " << gitVersion() << endl; }

#ifndef _SCONS
#if defined(_WIN32)
    string sysInfo() {
        stringstream ss;
        ss << "not-scons win";
        ss << " mscver:" << _MSC_FULL_VER << " built:" << __DATE__;
        ss << " boostver:" << BOOST_VERSION;
#if( !defined(_MT) )
#error _MT is not defined
#endif
        ss << (sizeof(char *) == 8) ? " 64bit" : " 32bit";
        return ss.str();
    }
#else
    string sysInfo() { return ""; }
#endif
#endif

    void printSysInfo() {
        log() << "build info: " << sysInfo() << endl;
    }


    static Tee * startupWarningsLog = new RamLog("startupWarnings"); //intentionally leaked

    //
    // system warnings
    //
    void show_warnings() {
        // each message adds a leading and a trailing newline

        bool warned = false;
        {
            const char * foo = strchr( versionString , '.' ) + 1;
            int bar = atoi( foo );
            if ( ( 2 * ( bar / 2 ) ) != bar ) {
                log() << startupWarningsLog;
                log() << "** NOTE: This is a development version (" << versionString << ") of MongoDB." << startupWarningsLog;
                log() << "**       Not recommended for production." << startupWarningsLog;
                warned = true;
            }
        }

        if ( sizeof(int*) == 4 ) {
            log() << startupWarningsLog;
            log() << "** NOTE: when using MongoDB 32 bit, you are limited to about 2 gigabytes of data" << startupWarningsLog;
            log() << "**       see http://blog.mongodb.org/post/137788967/32-bit-limitations" << startupWarningsLog;
            log() << "**       with --journal, the limit is lower" << startupWarningsLog;
            warned = true;
        }

        if ( !ProcessInfo::blockCheckSupported() ) {
            log() << startupWarningsLog;
            log() << "** NOTE: your operating system version does not support the method that MongoDB" << startupWarningsLog;
            log() << "**       uses to detect impending page faults." << startupWarningsLog;
            log() << "**       This may result in slower performance for certain use cases" << startupWarningsLog;
            warned = true;
        }
#ifdef __linux__
        if (boost::filesystem::exists("/proc/vz") && !boost::filesystem::exists("/proc/bc")) {
            log() << startupWarningsLog;
            log() << "** WARNING: You are running in OpenVZ. This is known to be broken!!!" << startupWarningsLog;
            warned = true;
        }

        if (boost::filesystem::exists("/sys/devices/system/node/node1")){
            // We are on a box with a NUMA enabled kernel and more than 1 numa node (they start at node0)
            // Now we look at the first line of /proc/self/numa_maps
            //
            // Bad example:
            // $ cat /proc/self/numa_maps
            // 00400000 default file=/bin/cat mapped=6 N4=6
            //
            // Good example:
            // $ numactl --interleave=all cat /proc/self/numa_maps
            // 00400000 interleave:0-7 file=/bin/cat mapped=6 N4=6

            File f;
            f.open("/proc/self/numa_maps", /*read_only*/true);
            if ( f.is_open() && ! f.bad() ) {
                char line[100]; //we only need the first line
                if (read(f.fd, line, sizeof(line)) < 0){
                    warning() << "failed to read from /proc/self/numa_maps: " << errnoWithDescription() << startupWarningsLog;
                    warned = true;
                }
                else {
                    // just in case...
                    line[98] = ' ';
                    line[99] = '\0';
                    
                    // skip over pointer
                    const char* space = strchr(line, ' ');
                    
                    if ( ! space ) {
                        log() << startupWarningsLog;
                        log() << "** WARNING: cannot parse numa_maps" << startupWarningsLog;
                        warned = true;
                    }
                    else if ( ! startsWith(space+1, "interleave") ) {
                        log() << startupWarningsLog;
                        log() << "** WARNING: You are running on a NUMA machine." << startupWarningsLog;
                        log() << "**          We suggest launching mongod like this to avoid performance problems:" << startupWarningsLog;
                        log() << "**              numactl --interleave=all mongod [other options]" << startupWarningsLog;
                        warned = true;
                    }
                }
            }
        }

        if (cmdLine.dur){
            fstream f ("/proc/sys/vm/overcommit_memory", ios_base::in);
            unsigned val;
            f >> val;

            if (val == 2) {
                log() << startupWarningsLog;
                log() << "** WARNING: /proc/sys/vm/overcommit_memory is " << val << startupWarningsLog;
                log() << "**          Journaling works best with it set to 0 or 1" << startupWarningsLog;
            }
        }

        if (boost::filesystem::exists("/proc/sys/vm/zone_reclaim_mode")){
            fstream f ("/proc/sys/vm/zone_reclaim_mode", ios_base::in);
            unsigned val;
            f >> val;

            if (val != 0) {
                log() << startupWarningsLog;
                log() << "** WARNING: /proc/sys/vm/zone_reclaim_mode is " << val << startupWarningsLog;
                log() << "**          We suggest setting it to 0" << startupWarningsLog;
                log() << "**          http://www.kernel.org/doc/Documentation/sysctl/vm.txt" << startupWarningsLog;
            }
        }
#endif

#if defined(RLIMIT_NPROC) && defined(RLIMIT_NOFILE)
        //Check that # of files rlmit > 1000 , and # of processes > # of files/2
        const unsigned int minNumFiles = 1000;
        const double filesToProcsRatio = 2.0;
        struct rlimit rlnproc;
        struct rlimit rlnofile;

        if(!getrlimit(RLIMIT_NPROC,&rlnproc) && !getrlimit(RLIMIT_NOFILE,&rlnofile)){
            if(rlnofile.rlim_cur < minNumFiles){
                log() << startupWarningsLog;
                log() << "** WARNING: soft rlimits too low. Number of files is "
                        << rlnofile.rlim_cur
                        << ", should be at least " << minNumFiles << startupWarningsLog;
            }
            if(rlnproc.rlim_cur < rlnofile.rlim_cur/filesToProcsRatio){
                log() << startupWarningsLog;
                log() << "** WARNING: soft rlimits too low. rlimits set to " << rlnproc.rlim_cur << " processes, "
                        << rlnofile.rlim_cur << " files. Number of processes should be at least "
                        << 1/filesToProcsRatio << " times number of files." << startupWarningsLog;
            }
        } else {
            log() << startupWarningsLog;
            log() << "** WARNING: getrlimit failed. " << errnoWithDescription() << startupWarningsLog;
        }
#endif
        if (warned) {
            log() << startupWarningsLog;
        }
    }

    int versionCmp(StringData rhs, StringData lhs) {
        if (strcmp(rhs.data(),lhs.data()) == 0)
            return 0;

        // handle "1.2.3-" and "1.2.3-pre"
        if (rhs.size() < lhs.size()) {
            if (strncmp(rhs.data(), lhs.data(), rhs.size()) == 0 && lhs.data()[rhs.size()] == '-')
                return +1;
        }
        else if (rhs.size() > lhs.size()) {
            if (strncmp(rhs.data(), lhs.data(), lhs.size()) == 0 && rhs.data()[lhs.size()] == '-')
                return -1;
        }

        return LexNumCmp::cmp(rhs.data(), lhs.data(), false);
    }

    class VersionCmpTest : public StartupTest {
    public:
        void run() {
            verify( versionCmp("1.2.3", "1.2.3") == 0 );
            verify( versionCmp("1.2.3", "1.2.4") < 0 );
            verify( versionCmp("1.2.3", "1.2.20") < 0 );
            verify( versionCmp("1.2.3", "1.20.3") < 0 );
            verify( versionCmp("2.2.3", "10.2.3") < 0 );
            verify( versionCmp("1.2.3", "1.2.3-") > 0 );
            verify( versionCmp("1.2.3", "1.2.3-pre") > 0 );
            verify( versionCmp("1.2.3", "1.2.4-") < 0 );
            verify( versionCmp("1.2.3-", "1.2.3") < 0 );
            verify( versionCmp("1.2.3-pre", "1.2.3") < 0 );

            log(1) << "versionCmpTest passed" << endl;
        }
    } versionCmpTest;

    class VersionArrayTest : public StartupTest {
    public:
        void run() {
            verify( _versionArray("1.2.3") == BSON_ARRAY(1 << 2 << 3 << 0) );
            verify( _versionArray("1.2.0") == BSON_ARRAY(1 << 2 << 0 << 0) );
            verify( _versionArray("2.0.0") == BSON_ARRAY(2 << 0 << 0 << 0) );

            verify( _versionArray("1.2.3-pre-") == BSON_ARRAY(1 << 2 << 3 << -100) );
            verify( _versionArray("1.2.0-pre-") == BSON_ARRAY(1 << 2 << 0 << -100) );
            verify( _versionArray("2.0.0-pre-") == BSON_ARRAY(2 << 0 << 0 << -100) );

            verify( _versionArray("1.2.3-rc0") == BSON_ARRAY(1 << 2 << 3 << -10) );
            verify( _versionArray("1.2.0-rc1") == BSON_ARRAY(1 << 2 << 0 << -9) );
            verify( _versionArray("2.0.0-rc2") == BSON_ARRAY(2 << 0 << 0 << -8) );

            // Note that the pre of an rc is the same as the rc itself
            verify( _versionArray("1.2.3-rc3-pre-") == BSON_ARRAY(1 << 2 << 3 << -7) );
            verify( _versionArray("1.2.0-rc4-pre-") == BSON_ARRAY(1 << 2 << 0 << -6) );
            verify( _versionArray("2.0.0-rc5-pre-") == BSON_ARRAY(2 << 0 << 0 << -5) );

            log(1) << "versionArrayTest passed" << endl;
        }
    } versionArrayTest;
}
