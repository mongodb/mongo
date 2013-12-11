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


#include <cstdlib>
#include <iostream>
#include <iomanip>
#include <sstream>
#include <string>
#include <fstream>

#include "mongo/base/parse_number.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/pdfile_version.h"
#include "mongo/db/storage_options.h"
#include "mongo/util/file.h"
#include "mongo/util/net/ssl_manager.h" 
#include "mongo/util/processinfo.h"
#include "mongo/util/ramlog.h"
#include "mongo/util/stringutils.h"
#include "mongo/util/version.h"


namespace mongo {

    /* Approved formats for versionString:
     *      1.2.3
     *      1.2.3-pre-
     *      1.2.3-rc4 (up to rc9)
     *      1.2.3-rc4-pre-
     * If you really need to do something else you'll need to fix _versionArray()
     */
    const char versionString[] = "2.5.5-pre-";

    // See unit test for example outputs
    BSONArray toVersionArray(const char* version){
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

            int num;
            if ( parseNumberFromString( curPart, &num ).isOK() ) {
                b.append(num);
            }
            else if (curPart.empty()){
                verify(*c == '\0');
                break;
            }
            else if (startsWith(curPart, "rc")){
                num = 0;
                verify( parseNumberFromString( curPart.substr(2), &num ).isOK() );
                finalPart = -10 + num;
                break;
            }
            else if (curPart == "pre"){
                finalPart = -100;
                break;
            }

            curPart = "";
        } while (*c++);

        b.append(finalPart);
        return b.arr();
    }

    bool isSameMajorVersion( const char* version ) {

        BSONArray remoteVersionArray = toVersionArray( version );

        BSONObjIterator remoteIt(remoteVersionArray);
        BSONObjIterator myIt(versionArray);

        // Compare only the first two fields of the version
        int compareLen = 2;
        while (compareLen > 0 && remoteIt.more() && myIt.more()) {
            if (remoteIt.next().numberInt() != myIt.next().numberInt()) break;
            compareLen--;
        }

        return compareLen == 0;
    }

    const BSONArray versionArray = toVersionArray(versionString);

    string mongodVersion() {
        stringstream ss;
        ss << "db version v" << versionString;
        return ss.str();
    }

#ifndef _SCONS
    // only works in scons
    const char * gitVersion() { return "not-scons"; }
    const char * compiledJSEngine() { return ""; }
    const char * allocator() { return ""; }
    const char * loaderFlags() { return ""; }
    const char * compilerFlags() { return ""; }
#endif

    void printGitVersion() { log() << "git version: " << gitVersion() << endl; }

    const std::string openSSLVersion(const std::string &prefix, const std::string &suffix) {
        return getSSLVersion(prefix, suffix);
    }

    void printOpenSSLVersion() {
#ifdef MONGO_SSL
        log() << openSSLVersion("OpenSSL version: ") << endl;
#endif
    }

#ifndef _SCONS
#if defined(_WIN32)
    string sysInfo() {
        stringstream ss;
        ss << "not-scons win";
        ss << " mscver:" << _MSC_FULL_VER << " built:" << __DATE__;
        ss << " boostver:" << BOOST_VERSION;
        ss << (sizeof(char *) == 8) ? " 64bit" : " 32bit";
        return ss.str();
    }
#else
    string sysInfo() { return ""; }

#endif
#endif

#if defined(_WIN32)
    std::string targetMinOS() {
        stringstream ss;
#if (NTDDI_VERSION >= 0x06010000)
        ss << "Windows 7/Windows Server 2008 R2";
#elif (NTDDI_VERSION >= 0x05020200)
        ss << "Windows Server 2003 SP2";
#elif (NTDDI_VERSION >= 0x05010300)
        ss << "Windows XP SP3";
#else
#error This targetted Windows version is not supported
#endif // NTDDI_VERSION
       return ss.str();
    }

    void printTargetMinOS() {
        log() << "targetMinOS: " << targetMinOS();
    }
#endif // _WIN32

    void printSysInfo() {
        log() << "build info: " << sysInfo() << endl;
    }

    void printAllocator() {
        log() << "allocator: " << allocator() << endl;
    }

    void appendBuildInfo(BSONObjBuilder& result) {
       result << "version" << versionString
              << "gitVersion" << gitVersion()
#if defined(_WIN32)
              << "targetMinOS" << targetMinOS()
#endif
              << "OpenSSLVersion" << openSSLVersion()
              << "sysInfo" << sysInfo()
              << "loaderFlags" << loaderFlags()
              << "compilerFlags" << compilerFlags()
              << "allocator" << allocator()
              << "versionArray" << versionArray
              << "javascriptEngine" << compiledJSEngine()
/*TODO: add this back once the module system is in place -- maybe once we do something like serverstatus with callbacks*/
//              << "interpreterVersion" << globalScriptEngine->getInterpreterVersionString()
              << "bits" << ( sizeof( int* ) == 4 ? 32 : 64 );
       result.appendBool( "debug" , debug );
       result.appendNumber("maxBsonObjectSize", BSONObjMaxUserSize);
    }
}
