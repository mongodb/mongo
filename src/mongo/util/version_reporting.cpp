// @file version.cpp

/*    Copyright 2009 10gen Inc.
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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kControl

#include "mongo/config.h"

#include "mongo/platform/basic.h"

#include "mongo/util/version_reporting.h"

#include <boost/version.hpp>
#include <sstream>
#include <string>

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/global_environment_experiment.h"
#include "mongo/db/jsobj.h"
#include "mongo/util/debug_util.h"
#include "mongo/util/log.h"
#include "mongo/util/net/ssl_manager.h"
#include "mongo/util/version.h"


namespace mongo {

    using std::endl;
    using std::string;
    using std::stringstream;

    void printGitVersion() { log() << "git version: " << gitVersion() << endl; }

    const std::string openSSLVersion(const std::string &prefix, const std::string &suffix) {
        return getSSLVersion(prefix, suffix);
    }

    void printOpenSSLVersion() {
#ifdef MONGO_CONFIG_SSL
        log() << openSSLVersion("OpenSSL version: ") << endl;
#endif
    }

    BSONArray storageEngineList() {
        if (!hasGlobalEnvironment())
            return BSONArray();

        boost::scoped_ptr<StorageFactoriesIterator> sfi(
            getGlobalEnvironment()->makeStorageFactoriesIterator());

        if (!sfi)
            return BSONArray();

        BSONArrayBuilder engineArrayBuilder;

        while (sfi->more()) {
            engineArrayBuilder.append(sfi->next()->getCanonicalName());
        }

        return engineArrayBuilder.arr();
    }

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
              << "storageEngines" << storageEngineList()
              << "versionArray" << versionArray
              << "javascriptEngine" << compiledJSEngine()
/*TODO: add this back once the module system is in place -- maybe once we do something like serverstatus with callbacks*/
//              << "interpreterVersion" << globalScriptEngine->getInterpreterVersionString()
              << "bits" << ( sizeof( int* ) == 4 ? 32 : 64 );
       result.appendBool( "debug" , kDebugBuild );
       result.appendNumber("maxBsonObjectSize", BSONObjMaxUserSize);
    }
}
