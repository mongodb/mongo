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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kDefault

#include "mongo/platform/basic.h"

#include "mongo/shell/shell_utils.h"

#include "mongo/client/dbclientinterface.h"
#include "mongo/client/replica_set_monitor.h"
#include "mongo/db/hasher.h"
#include "mongo/platform/random.h"
#include "mongo/scripting/engine.h"
#include "mongo/shell/bench.h"
#include "mongo/shell/shell_options.h"
#include "mongo/shell/shell_utils_extended.h"
#include "mongo/shell/shell_utils_launcher.h"
#include "mongo/util/fail_point_service.h"
#include "mongo/util/log.h"
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
thread_local unsigned int _randomSeed = 0;
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
    _randomSeed = seed;
#else
    srand(seed);
#endif
    return BSON("" << static_cast<double>(seed));
}

BSONObj JSRand(const BSONObj& a, void* data) {
    uassert(12519, "rand accepts no arguments", a.nFields() == 0);
    unsigned r;
#if !defined(_WIN32)
    r = rand_r(&_randomSeed);
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

BSONObj getBuildInfo(const BSONObj& a, void* data) {
    uassert(16822, "getBuildInfo accepts no arguments", a.nFields() == 0);
    BSONObjBuilder b;
    VersionInfoInterface::instance().appendBuildInfo(&b);
    return BSON("" << b.done());
}

BSONObj _setShellFailPoint(const BSONObj& a, void* data) {
    if (a.nFields() != 1) {
        uasserted(ErrorCodes::BadValue,
                  str::stream() << "_setShellFailPoint takes exactly 1 argument, but was given "
                                << a.nFields());
    }

    if (!a.firstElement().isABSONObj()) {
        uasserted(ErrorCodes::BadValue,
                  str::stream() << "_setShellFailPoint given a non-object as an argument.");
    }

    auto cmdObj = a.firstElement().Obj();
    setGlobalFailPoint(cmdObj.firstElement().str(), cmdObj);

    return BSON("" << true);
}

BSONObj computeSHA256Block(const BSONObj& a, void* data) {
    std::vector<ConstDataRange> blocks;

    auto ele = a[0];

    BSONObjBuilder bob;
    switch (ele.type()) {
        case BinData: {
            int len;
            const char* ptr = ele.binData(len);
            SHA256Block::computeHash({ConstDataRange(ptr, len)}).appendAsBinData(bob, ""_sd);

            break;
        }
        case String: {
            auto str = ele.valueStringData();
            SHA256Block::computeHash({ConstDataRange(str.rawData(), str.size())})
                .appendAsBinData(bob, ""_sd);
            break;
        }
        default:
            uasserted(ErrorCodes::BadValue, "Can only computeSHA256Block of strings and bindata");
    }

    return bob.obj();
}

/**
 * This function computes a hash value for a document.
 * Specifically, this is the same hash function that is used to form a hashed index,
 * and thus used to generate shard keys for a collection.
 *
 * e.g.
 * > // For a given collection prepared like so:
 * > use mydb
 * > db.mycollection.createIndex({ x: "hashed" })
 * > sh.shardCollection("mydb.mycollection", { x: "hashed" })
 * > // And a sample object like so:
 * > var obj = { x: "Whatever key", y: 2, z: 10.0 }
 * > // The hashed value of the shard key can be acquired from the shard key-value pair like so:
 * > convertShardKeyToHashed({x: "Whatever key"})
 */
BSONObj convertShardKeyToHashed(const BSONObj& a, void* data) {
    const auto& objEl = a[0];

    uassert(10151,
            "convertShardKeyToHashed accepts either 1 or 2 arguments",
            a.nFields() >= 1 && a.nFields() <= 2);

    // It looks like the seed is always default right now.
    // But no reason not to allow for the future
    auto seed = BSONElementHasher::DEFAULT_HASH_SEED;
    if (a.nFields() > 1) {
        auto seedEl = a[1];

        uassert(10159, "convertShardKeyToHashed seed value should be a number", seedEl.isNumber());
        seed = seedEl.numberInt();
    }

    auto key = BSONElementHasher::hash64(objEl, seed);
    return BSON("" << key);
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

BSONObj shouldRetryWrites(const BSONObj&, void* data) {
    return BSON("" << shellGlobalParams.shouldRetryWrites);
}

BSONObj shouldUseImplicitSessions(const BSONObj&, void* data) {
    return BSON("" << true);
}

BSONObj interpreterVersion(const BSONObj& a, void* data) {
    uassert(16453, "interpreterVersion accepts no arguments", a.nFields() == 0);
    return BSON("" << getGlobalScriptEngine()->getInterpreterVersionString());
}

BSONObj fileExistsJS(const BSONObj& a, void*) {
    uassert(40678,
            "fileExists expects one string argument",
            a.nFields() == 1 && a.firstElement().type() == String);
    return BSON("" << fileExists(a.firstElement().valuestrsafe()));
}

void installShellUtils(Scope& scope) {
    scope.injectNative("getMemInfo", JSGetMemInfo);
    scope.injectNative("_replMonitorStats", replMonitorStats);
    scope.injectNative("_srand", JSSrand);
    scope.injectNative("_rand", JSRand);
    scope.injectNative("_isWindows", isWindows);
    scope.injectNative("_setShellFailPoint", _setShellFailPoint);
    scope.injectNative("interpreterVersion", interpreterVersion);
    scope.injectNative("getBuildInfo", getBuildInfo);
    scope.injectNative("computeSHA256Block", computeSHA256Block);
    scope.injectNative("convertShardKeyToHashed", convertShardKeyToHashed);
    scope.injectNative("fileExists", fileExistsJS);

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
    scope.injectNative("_shouldRetryWrites", shouldRetryWrites);
    scope.injectNative("_shouldUseImplicitSessions", shouldUseImplicitSessions);
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

void ConnectionRegistry::registerConnection(DBClientBase& client) {
    BSONObj info;
    if (client.runCommand("admin", BSON("whatsmyuri" << 1), info)) {
        string connstr = client.getServerAddress();
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
        std::unique_ptr<DBClientBase> conn(cs.connect("MongoDB Shell", errmsg));
        if (!conn) {
            continue;
        }

        const set<string>& uris = i->second;

        BSONObj currentOpRes;
        conn->runPseudoCommand("admin", "currentOp", "$cmd.sys.inprog", {}, currentOpRes);
        if (!currentOpRes["inprog"].isABSONObj()) {
            // We don't have permissions (or the call didn't succeed) - go to the next connection.
            continue;
        }
        auto inprog = currentOpRes["inprog"].embeddedObject();
        for (const auto op : inprog) {
            // For sharded clusters, `client_s` is used instead and `client` is not present.
            string client;
            if (auto elem = op["client"]) {
                // mongod currentOp client
                if (elem.type() != String) {
                    warning() << "Ignoring operation " << op["opid"].toString(false)
                              << "; expected 'client' field in currentOp response to have type "
                                 "string, but found "
                              << typeName(elem.type());
                    continue;
                }
                client = elem.str();
            } else if (auto elem = op["client_s"]) {
                // mongos currentOp client
                if (elem.type() != String) {
                    warning() << "Ignoring operation " << op["opid"].toString(false)
                              << "; expected 'client_s' field in currentOp response to have type "
                                 "string, but found "
                              << typeName(elem.type());
                    continue;
                }
                client = elem.str();
            } else {
                // Internal operation, like TTL index.
                continue;
            }
            if (uris.count(client)) {
                if (!withPrompt || prompter.confirm()) {
                    BSONObjBuilder cmdBob;
                    BSONObj info;
                    cmdBob.appendAs(op["opid"], "op");
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
void onConnect(DBClientBase& c) {
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
}  // namespace mongo
