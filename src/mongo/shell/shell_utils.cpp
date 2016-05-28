// mongo/shell/shell_utils.cpp
/*
 *    Copyright 2010 10gen Inc.
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

#include "mongo/platform/basic.h"

#include "mongo/shell/shell_utils.h"

#include "mongo/client/dbclientinterface.h"
#include "mongo/client/replica_set_monitor.h"
#include "mongo/db/catalog/index_key_validate.h"
#include "mongo/db/index/external_key_generator.h"
#include "mongo/platform/random.h"
#include "mongo/scripting/engine.h"
#include "mongo/shell/bench.h"
#include "mongo/shell/shell_options.h"
#include "mongo/shell/shell_utils_extended.h"
#include "mongo/shell/shell_utils_launcher.h"
#include "mongo/util/concurrency/threadlocal.h"
#include "mongo/util/processinfo.h"
#include "mongo/util/quick_exit.h"
#include "mongo/util/text.h"
#include "mongo/util/version.h"

namespace mongo {

using std::set;
using std::map;
using std::string;

namespace JSFiles {
extern const JSFile servers;
extern const JSFile shardingtest;
extern const JSFile servers_misc;
extern const JSFile replsettest;
extern const JSFile bridge;
}

namespace shell_utils {

std::string _dbConnect;
std::string _dbAuth;

const char* argv0 = 0;
void RecordMyLocation(const char* _argv0) {
    argv0 = _argv0;
}

// helpers

BSONObj makeUndefined() {
    BSONObjBuilder b;
    b.appendUndefined("");
    return b.obj();
}
const BSONObj undefinedReturn = makeUndefined();

BSONElement singleArg(const BSONObj& args) {
    uassert(12597, "need to specify 1 argument", args.nFields() == 1);
    return args.firstElement();
}

const char* getUserDir() {
#ifdef _WIN32
    return getenv("USERPROFILE");
#else
    return getenv("HOME");
#endif
}

// real methods

BSONObj JSGetMemInfo(const BSONObj& args, void* data) {
    ProcessInfo pi;
    uassert(10258, "processinfo not supported", pi.supported());

    BSONObjBuilder e;
    e.append("virtual", pi.getVirtualMemorySize());
    e.append("resident", pi.getResidentSize());

    BSONObjBuilder b;
    b.append("ret", e.obj());

    return b.obj();
}

#if !defined(_WIN32)
ThreadLocalValue<unsigned int> _randomSeed;
#endif

BSONObj JSSrand(const BSONObj& a, void* data) {
    unsigned int seed;
    // grab the least significant bits of either the supplied argument or
    // a random number from SecureRandom.
    if (a.nFields() == 1 && a.firstElement().isNumber())
        seed = static_cast<unsigned int>(a.firstElement().numberLong());
    else {
        std::unique_ptr<SecureRandom> rand(SecureRandom::create());
        seed = static_cast<unsigned int>(rand->nextInt64());
    }
#if !defined(_WIN32)
    _randomSeed.set(seed);
#else
    srand(seed);
#endif
    return BSON("" << static_cast<double>(seed));
}

BSONObj JSRand(const BSONObj& a, void* data) {
    uassert(12519, "rand accepts no arguments", a.nFields() == 0);
    unsigned r;
#if !defined(_WIN32)
    r = rand_r(&_randomSeed.getRef());
#else
    r = rand();
#endif
    return BSON("" << double(r) / (double(RAND_MAX) + 1));
}

BSONObj isWindows(const BSONObj& a, void* data) {
    uassert(13006, "isWindows accepts no arguments", a.nFields() == 0);
#ifdef _WIN32
    return BSON("" << true);
#else
    return BSON("" << false);
#endif
}

BSONObj isAddressSanitizerActive(const BSONObj& a, void* data) {
    bool isSanitized = false;
// See the following for information on how we detect address sanitizer in clang and gcc.
//
// - http://clang.llvm.org/docs/AddressSanitizer.html#has-feature-address-sanitizer
// - https://gcc.gnu.org/ml/gcc-patches/2012-11/msg01827.html
//
#if defined(__has_feature)
#if __has_feature(address_sanitizer)
    isSanitized = true;
#endif
#elif defined(__SANITIZE_ADDRESS__)
    isSanitized = true;
#endif
    return BSON("" << isSanitized);
}

BSONObj getBuildInfo(const BSONObj& a, void* data) {
    uassert(16822, "getBuildInfo accepts no arguments", a.nFields() == 0);
    BSONObjBuilder b;
    appendBuildInfo(b);
    return BSON("" << b.done());
}

BSONObj isKeyTooLarge(const BSONObj& a, void* data) {
    uassert(17428, "keyTooLarge takes exactly 2 arguments", a.nFields() == 2);
    BSONObjIterator i(a);
    BSONObj index = i.next().Obj();
    BSONObj doc = i.next().Obj();

    return BSON("" << isAnyIndexKeyTooLarge(index, doc));
}

BSONObj validateIndexKey(const BSONObj& a, void* data) {
    BSONObj key = a[0].Obj();
    Status indexValid = validateKeyPattern(key);
    if (!indexValid.isOK()) {
        return BSON("" << BSON("ok" << false << "type" << indexValid.codeString() << "errmsg"
                                    << indexValid.reason()));
    }
    return BSON("" << BSON("ok" << true));
}

BSONObj replMonitorStats(const BSONObj& a, void* data) {
    uassert(17134,
            "replMonitorStats requires a single string argument (the ReplSet name)",
            a.nFields() == 1 && a.firstElement().type() == String);

    ReplicaSetMonitorPtr rsm = ReplicaSetMonitor::get(a.firstElement().valuestrsafe());
    if (!rsm) {
        return BSON(""
                    << "no ReplSetMonitor exists by that name");
    }

    BSONObjBuilder result;
    rsm->appendInfo(result);
    return result.obj();
}

BSONObj useWriteCommandsDefault(const BSONObj& a, void* data) {
    return BSON("" << shellGlobalParams.useWriteCommandsDefault);
}

BSONObj writeMode(const BSONObj&, void*) {
    return BSON("" << shellGlobalParams.writeMode);
}

BSONObj readMode(const BSONObj&, void*) {
    return BSON("" << shellGlobalParams.readMode);
}

BSONObj interpreterVersion(const BSONObj& a, void* data) {
    uassert(16453, "interpreterVersion accepts no arguments", a.nFields() == 0);
    return BSON("" << globalScriptEngine->getInterpreterVersionString());
}

void installShellUtils(Scope& scope) {
    scope.injectNative("getMemInfo", JSGetMemInfo);
    scope.injectNative("_replMonitorStats", replMonitorStats);
    scope.injectNative("_srand", JSSrand);
    scope.injectNative("_rand", JSRand);
    scope.injectNative("_isWindows", isWindows);
    scope.injectNative("_isAddressSanitizerActive", isAddressSanitizerActive);
    scope.injectNative("interpreterVersion", interpreterVersion);
    scope.injectNative("getBuildInfo", getBuildInfo);
    scope.injectNative("isKeyTooLarge", isKeyTooLarge);
    scope.injectNative("validateIndexKey", validateIndexKey);

#ifndef MONGO_SAFE_SHELL
    // can't launch programs
    installShellUtilsLauncher(scope);
    installShellUtilsExtended(scope);
#endif
}

void initScope(Scope& scope) {
    // Need to define this method before JSFiles::utils is executed.
    scope.injectNative("_useWriteCommandsDefault", useWriteCommandsDefault);
    scope.injectNative("_writeMode", writeMode);
    scope.injectNative("_readMode", readMode);
    scope.externalSetup();
    mongo::shell_utils::installShellUtils(scope);
    scope.execSetup(JSFiles::servers);
    scope.execSetup(JSFiles::shardingtest);
    scope.execSetup(JSFiles::servers_misc);
    scope.execSetup(JSFiles::replsettest);
    scope.execSetup(JSFiles::bridge);

    scope.injectNative("benchRun", BenchRunner::benchRunSync);
    scope.injectNative("benchRunSync", BenchRunner::benchRunSync);
    scope.injectNative("benchStart", BenchRunner::benchStart);
    scope.injectNative("benchFinish", BenchRunner::benchFinish);

    if (!_dbConnect.empty()) {
        uassert(12513, "connect failed", scope.exec(_dbConnect, "(connect)", false, true, false));
    }
    if (!_dbAuth.empty()) {
        uassert(12514, "login failed", scope.exec(_dbAuth, "(auth)", true, true, false));
    }
}

Prompter::Prompter(const string& prompt) : _prompt(prompt), _confirmed() {}

bool Prompter::confirm() {
    if (_confirmed) {
        return true;
    }

    // The printf and scanf functions provide thread safe i/o.

    printf("\n%s (y/n): ", _prompt.c_str());

    char yn = '\0';
    int nScanMatches = scanf("%c", &yn);
    bool matchedY = (nScanMatches == 1 && (yn == 'y' || yn == 'Y'));

    return _confirmed = matchedY;
}

ConnectionRegistry::ConnectionRegistry() = default;

void ConnectionRegistry::registerConnection(DBClientWithCommands& client) {
    BSONObj info;
    if (client.runCommand("admin", BSON("whatsmyuri" << 1), info)) {
        string connstr = dynamic_cast<DBClientBase&>(client).getServerAddress();
        stdx::lock_guard<stdx::mutex> lk(_mutex);
        _connectionUris[connstr].insert(info["you"].str());
    }
}

void ConnectionRegistry::killOperationsOnAllConnections(bool withPrompt) const {
    Prompter prompter("do you want to kill the current op(s) on the server?");
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    for (map<string, set<string>>::const_iterator i = _connectionUris.begin();
         i != _connectionUris.end();
         ++i) {
        auto status = ConnectionString::parse(i->first);
        if (!status.isOK()) {
            continue;
        }

        const ConnectionString cs(status.getValue());

        string errmsg;
        std::unique_ptr<DBClientWithCommands> conn(cs.connect(errmsg));
        if (!conn) {
            continue;
        }

        const set<string>& uris = i->second;

        BSONObj currentOpRes;
        conn->runPseudoCommand("admin", "currentOp", "$cmd.sys.inprog", {}, currentOpRes);
        auto inprog = currentOpRes["inprog"].embeddedObject();
        BSONForEach(op, inprog) {
            if (uris.count(op["client"].String())) {
                if (!withPrompt || prompter.confirm()) {
                    BSONObjBuilder cmdBob;
                    BSONObj info;
                    cmdBob.append("op", op["opid"]);
                    auto cmdArgs = cmdBob.done();
                    conn->runPseudoCommand("admin", "killOp", "$cmd.sys.killop", cmdArgs, info);
                } else {
                    return;
                }
            }
        }
    }
}

ConnectionRegistry connectionRegistry;

bool _nokillop = false;
void onConnect(DBClientWithCommands& c) {
    if (_nokillop) {
        return;
    }

    // Only override the default rpcProtocols if they were set on the command line.
    if (shellGlobalParams.rpcProtocols) {
        c.setClientRPCProtocols(*shellGlobalParams.rpcProtocols);
    }

    connectionRegistry.registerConnection(c);
}

bool fileExists(const std::string& file) {
    try {
#ifdef _WIN32
        boost::filesystem::path p(toWideString(file.c_str()));
#else
        boost::filesystem::path p(file);
#endif
        return boost::filesystem::exists(p);
    } catch (...) {
        return false;
    }
}


stdx::mutex& mongoProgramOutputMutex(*(new stdx::mutex()));
}
}
