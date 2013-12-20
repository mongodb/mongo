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

#include "mongo/platform/basic.h"

#include "mongo/util/version_reporting.h"

#include <sstream>
#include <string>

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/jsobj.h"
#include "mongo/util/net/ssl_manager.h"
#include "mongo/util/version.h"


namespace mongo {

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
#if( !defined(_MT) )
#error _MT is not defined
#endif
        ss << (sizeof(char *) == 8 ? " 64bit" : " 32bit");
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
