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
#include "unittest.h"
#include "version.h"
#include "file.h"

namespace mongo {

    const char versionString[] = "1.8.2-rc1-pre-";

    string mongodVersion() {
        stringstream ss;
        ss << "db version v" << versionString << ", pdfile version " << VERSION << "." << VERSION_MINOR;
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
        log() << "build sys info: " << sysInfo() << endl;
    }

    //
    // 32 bit systems warning
    //
    void show_warnings() {
        // each message adds a leading but not a trailing newline

        bool warned = false;
        {
            const char * foo = strchr( versionString , '.' ) + 1;
            int bar = atoi( foo );
            if ( ( 2 * ( bar / 2 ) ) != bar ) {
                cout << "\n** NOTE: This is a development version (" << versionString << ") of MongoDB.";
                cout << "\n**       Not recommended for production." << endl;
                warned = true;
            }
        }

        if ( sizeof(int*) == 4 ) {
            cout << endl;
            cout << "** NOTE: when using MongoDB 32 bit, you are limited to about 2 gigabytes of data" << endl;
            cout << "**       see http://blog.mongodb.org/post/137788967/32-bit-limitations" << endl;
            cout << "**       with --dur, the limit is lower" << endl;
            warned = true;
        }

#ifdef __linux__
        if (boost::filesystem::exists("/proc/vz") && !boost::filesystem::exists("/proc/bc")) {
            cout << endl;
            cout << "** WARNING: You are running in OpenVZ. This is known to be broken!!!" << endl;
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
            char line[100]; //we only need the first line
            f.read(0, line, sizeof(line));

            // just in case...
            line[98] = ' ';
            line[99] = '\0';

            // skip over pointer
            const char* space = strchr(line, ' ');

            if (!startsWith(space+1, "interleave")){
                cout << endl;
                cout << "** WARNING: You are running in on a NUMA machine." << endl;
                cout << "**          We suggest launching mongod like this to avoid performance problems:" << endl;
                cout << "**              numactl --interleave=all mongod [other options]" << endl;
                warned = true;
            }
        }
#endif

        if (warned)
            cout << endl;
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

        return lexNumCmp(rhs.data(), lhs.data());
    }

    class VersionCmpTest : public UnitTest {
    public:
        void run() {
            assert( versionCmp("1.2.3", "1.2.3") == 0 );
            assert( versionCmp("1.2.3", "1.2.4") < 0 );
            assert( versionCmp("1.2.3", "1.2.20") < 0 );
            assert( versionCmp("1.2.3", "1.20.3") < 0 );
            assert( versionCmp("2.2.3", "10.2.3") < 0 );
            assert( versionCmp("1.2.3", "1.2.3-") > 0 );
            assert( versionCmp("1.2.3", "1.2.3-pre") > 0 );
            assert( versionCmp("1.2.3", "1.2.4-") < 0 );
            assert( versionCmp("1.2.3-", "1.2.3") < 0 );
            assert( versionCmp("1.2.3-pre", "1.2.3") < 0 );

            log(1) << "versionCmpTest passed" << endl;
        }
    } versionCmpTest;
}
