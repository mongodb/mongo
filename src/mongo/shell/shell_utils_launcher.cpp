/**
 *    Copyright (C) 2018-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include "mongo/shell/shell_utils_launcher.h"

#include <algorithm>
#include <array>
#include <boost/iostreams/device/file_descriptor.hpp>
#include <boost/iostreams/stream.hpp>
#include <boost/iostreams/stream_buffer.hpp>
#include <boost/iostreams/tee.hpp>
#include <csignal>
#include <fcntl.h>
#include <fmt/format.h>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <memory>
#include <vector>

#ifdef _WIN32
#include <io.h>
#define SIGKILL 9
#else
#include <csignal>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#include "mongo/base/environment_buffer.h"
#include "mongo/base/error_codes.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/util/builder.h"
#include "mongo/client/dbclient_connection.h"
#include "mongo/db/storage/named_pipe.h"
#include "mongo/db/traffic_reader.h"
#include "mongo/logv2/log.h"
#include "mongo/platform/basic.h"
#include "mongo/scripting/engine.h"
#include "mongo/shell/named_pipe_test_helper.h"
#include "mongo/shell/program_runner.h"
#include "mongo/shell/shell_options.h"
#include "mongo/shell/shell_utils.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/ctype.h"
#include "mongo/util/destructor_guard.h"
#include "mongo/util/exit.h"
#include "mongo/util/exit_code.h"
#include "mongo/util/net/hostandport.h"
#include "mongo/util/quick_exit.h"
#include "mongo/util/scopeguard.h"
#include "mongo/util/signal_win32.h"
#include "mongo/util/str.h"
#include "mongo/util/text.h"
#include "mongo/util/version/releases.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kDefault


namespace mongo {

using std::cout;
using std::endl;
using std::make_pair;
using std::map;
using std::pair;
using std::string;
using std::stringstream;
using std::unique_ptr;
using std::vector;

/**
 * These utilities are thread safe but do not provide mutually exclusive access to resources
 * identified by the caller.  Resources identified by a pid or port should not be accessed
 * by different threads.  Dependent filesystem paths should not be accessed by different
 * threads.
 */
namespace shell_utils {

namespace {

using namespace fmt::literals;

#ifdef _WIN32

constexpr auto kFilesystemErrorRetry = 10;
constexpr auto kFilesystemErrorSleepIntervalMillis = 100;

void retryWithBackOff(std::function<void(void)> func) {
    for (int i = 0; i < kFilesystemErrorRetry - 1; i++) {
        try {
            func();
            return;
        } catch (const boost::filesystem::filesystem_error& fe) {
            LOGV2_WARNING(6088701,
                          "retryWithBackOff: Filesystem error",
                          "desc"_attr = fe.what(),
                          "waitMillis"_attr = (100 * i));
        }

        // A small sleep allows Windows an opportunity to close locked file
        // handlers, and reduce false errors on remove_all
        sleepmillis(100 * i);
    }

    // Try one last time. If this still fails, we propagate the error.
    func();
}
#endif

}  // namespace

// Output up to BSONObjMaxUserSize characters of the most recent log output in order to
// avoid hitting the 16MB size limit of a BSONObject.
BSONObj RawMongoProgramOutput(const BSONObj& args, void* data) {
    auto programOutputLogger =
        ProgramRegistry::get(getGlobalServiceContext())->getProgramOutputMultiplexer();
    std::string programLog = programOutputLogger->str();
    std::size_t sz = programLog.size();
    const string& outputStr =
        sz > BSONObjMaxUserSize ? programLog.substr(sz - BSONObjMaxUserSize) : programLog;

    return BSON("" << outputStr);
}

BSONObj ClearRawMongoProgramOutput(const BSONObj& args, void* data) {
    auto programOutputLogger =
        ProgramRegistry::get(getGlobalServiceContext())->getProgramOutputMultiplexer();
    programOutputLogger->clear();
    return undefinedReturn;
}

BSONObj CheckProgram(const BSONObj& args, void* data) {
    uassert(
        ErrorCodes::BadValue, "Cannot check the program with PID = NaN", !singleArg(args).isNaN());
    ProcessId pid = ProcessId::fromNative(singleArg(args).safeNumberInt());
    int exit_code = -123456;  // sentinel value
    bool isDead = ProgramRegistry::get(getGlobalServiceContext())->isPidDead(pid, &exit_code);
    if (!isDead) {
        return BSON("" << BSON("alive" << true));
    }
    return BSON("" << BSON("alive" << false << "exitCode" << exit_code));
}

BSONObj WaitProgram(const BSONObj& a, void* data) {
    uassert(
        ErrorCodes::BadValue, "Cannot wait for the program with PID = NaN", !singleArg(a).isNaN());
    ProcessId pid = ProcessId::fromNative(singleArg(a).safeNumberInt());
    int exit_code = -123456;  // sentinel value
    ProgramRegistry::get(getGlobalServiceContext())->waitForPid(pid, true, &exit_code);
    return BSON(string("") << exit_code);
}

// Calls waitpid on a mongo process specified by a port. If there is no pid registered for the given
// port, this function returns an exit code of 0 without doing anything. Otherwise, it calls waitpid
// for the pid associated with the given port and returns its exit code.
BSONObj WaitMongoProgram(const BSONObj& a, void* data) {
    int port = singleArg(a).numberInt();
    ProcessId pid;
    int exit_code = -123456;  // sentinel value
    invariant(port >= 0);

    auto registry = ProgramRegistry::get(getGlobalServiceContext());

    if (!registry->isPortRegistered(port)) {
        LOGV2_INFO(22813, "No db started on port", "port"_attr = port);
        return BSON(string("") << 0);
    }
    pid = registry->pidForPort(port);
    registry->waitForPid(pid, true, &exit_code);
    return BSON(string("") << exit_code);
}

// This function starts a program. In its input array it accepts either all commandline tokens
// which will be executed, or a single Object which must have a field named "args" which contains
// an array with all commandline tokens. The Object may have a field named "env" which contains an
// object of Key Value pairs which will be loaded into the environment of the spawned process.
BSONObj StartMongoProgram(const BSONObj& a, void* data) {
    shellGlobalParams.nokillop = true;
    BSONObj args = a;
    BSONObj env{};
    BSONElement firstElement = args.firstElement();

    if (firstElement.ok() && firstElement.isABSONObj()) {
        BSONObj subobj = firstElement.Obj();
        BSONElement argsElem = subobj["args"];
        BSONElement envElem = subobj["env"];
        uassert(40098,
                "If StartMongoProgram is called with a BSONObj, "
                "it must contain an 'args' subobject." +
                    args.toString(),
                argsElem.ok() && argsElem.isABSONObj());

        args = argsElem.Obj();
        if (envElem.ok() && envElem.isABSONObj()) {
            env = envElem.Obj();
        }
    }

    auto registry = ProgramRegistry::get(getGlobalServiceContext());
    auto runner = registry->createProgramRunner(args, env, true);
    runner.start();
    invariant(registry->isPidRegistered(runner.pid()));
    stdx::thread t(runner, registry->getProgramOutputMultiplexer(), true /* shouldLogOutput */);
    registry->registerReaderThread(runner.pid(), std::move(t));
    return BSON(string("") << runner.pid().asLongLong());
}

BSONObj RunProgram(const BSONObj& a, void* data, bool isMongo) {
    BSONObj env{};
    auto registry = ProgramRegistry::get(getGlobalServiceContext());
    auto runner = registry->createProgramRunner(a, env, isMongo);
    runner.start();
    invariant(registry->isPidRegistered(runner.pid()));
    stdx::thread t(runner, registry->getProgramOutputMultiplexer(), true /* shouldLogOutput */);
    registry->registerReaderThread(runner.pid(), std::move(t));
    int exit_code = -123456;  // sentinel value
    registry->waitForPid(runner.pid(), true, &exit_code);
    return BSON(string("") << exit_code);
}

BSONObj RunMongoProgram(const BSONObj& a, void* data) {
    return RunProgram(a, data, true);
}

BSONObj RunNonMongoProgram(const BSONObj& a, void* data) {
    return RunProgram(a, data, false);
}

BSONObj ResetDbpath(const BSONObj& a, void* data) {
    uassert(ErrorCodes::FailedToParse, "Expected 1 field", a.nFields() == 1);
    string path = a.firstElement().str();
    if (path.empty()) {
        LOGV2_WARNING(22824, "ResetDbpath(): nothing to do, path was empty");
        return undefinedReturn;
    }

    if (boost::filesystem::exists(path)) {
        auto removeAllIfNeeded = [path]() {
            if (boost::filesystem::exists(path)) {
                boost::filesystem::remove_all(path);
            }
        };

#ifdef _WIN32
        retryWithBackOff(removeAllIfNeeded);
#else
        removeAllIfNeeded();
#endif
    }

#ifdef _WIN32
    // Removing the directory may take non-zero time since it is executed asynchronously on Windows.
    // If the directory is in process of getting removed, then the CreateDirectory fails with access
    // denied so retry create directory on any error. We only expect to see access denied but we
    // will retry on all errors.
    auto wpath = toNativeString(path.c_str());
    retryWithBackOff([wpath]() {
        if (!CreateDirectoryW(wpath.c_str(), nullptr)) {
            auto ec = lastSystemError();
            uasserted(6088702,
                      str::stream()
                          << "CreateDirectory failed with unexpected error: " << errorMessage(ec));
        }
    });
#else
    boost::filesystem::create_directory(path);
#endif

    return undefinedReturn;
}

BSONObj PathExists(const BSONObj& a, void* data) {
    uassert(ErrorCodes::FailedToParse, "Expected 1 field", a.nFields() == 1);
    string path = a.firstElement().str();
    if (path.empty()) {
        LOGV2_WARNING(22825, "PathExists(): path was empty");
        return BSON(string("") << false);
    };
    bool exists = boost::filesystem::exists(path);
    return BSON(string("") << exists);
}

void copyDir(const boost::filesystem::path& from, const boost::filesystem::path& to) {
    boost::filesystem::directory_iterator end;
    boost::filesystem::directory_iterator i(from);
    while (i != end) {
        boost::filesystem::path p = *i;
        if (p.leaf() == "metrics.interim" || p.leaf() == "metrics.interim.temp") {
            // Ignore any errors for metrics.interim* files as these may disappear during copy
            boost::system::error_code ec;
            boost::filesystem::copy_file(p, to / p.leaf(), ec);
            if (ec) {
                LOGV2_INFO(22814,
                           "Skipping copying of file from '{from}' to "
                           "'{to}' due to: {error}",
                           "Skipping copying of file due to error"
                           "from"_attr = p.generic_string(),
                           "to"_attr = (to / p.leaf()).generic_string(),
                           "error"_attr = ec.message());
            }
        } else if (p.leaf() != "mongod.lock" && p.leaf() != "WiredTiger.lock") {
            if (boost::filesystem::is_directory(p)) {
                boost::filesystem::path newDir = to / p.leaf();
                boost::filesystem::create_directory(newDir);
                copyDir(p, newDir);
            } else {
                boost::filesystem::copy_file(p, to / p.leaf());
            }
        }
        ++i;
    }
}

/**
 * Called from JS as  `copyDbpath(fromDir, toDir);`
 *
 * The destination dbpath will be cleared first.
 */
BSONObj CopyDbpath(const BSONObj& a, void* data) {
    uassert(ErrorCodes::FailedToParse, "Expected 2 fields", a.nFields() == 2);
    BSONObjIterator i(a);
    string from = i.next().str();
    string to = i.next().str();
    if (from.empty() || to.empty()) {
        LOGV2_WARNING(22826,
                      "CopyDbpath(): nothing to do, source or destination path(s) were empty");
        return undefinedReturn;
    }
    if (boost::filesystem::exists(to))
        boost::filesystem::remove_all(to);
    boost::filesystem::create_directories(to);
    copyDir(from, to);
    return undefinedReturn;
}

inline void kill_wrapper(ProcessId pid, int sig, int port, const BSONObj& opt) {
#ifdef _WIN32
    if (sig == SIGKILL || port == 0) {
        TerminateProcess(ProgramRegistry::get(getGlobalServiceContext())->getHandleForPid(pid),
                         1);  // returns failure for "zombie" processes.
        return;
    }

    std::string eventName = getShutdownSignalName(pid.asUInt32());

    HANDLE event = OpenEventA(EVENT_MODIFY_STATE, FALSE, eventName.c_str());
    if (event == nullptr) {
        auto ec = lastSystemError();
        if (ec != systemError(ERROR_FILE_NOT_FOUND)) {
            LOGV2_WARNING(22827, "kill_wrapper OpenEvent failed", "error"_attr = errorMessage(ec));
        } else {
            LOGV2_INFO(
                22815,
                "kill_wrapper OpenEvent failed to open event to the process. It "
                "has likely died already or server is running an older version. Attempting to "
                "shutdown through admin command.",
                "pid"_attr = pid.asUInt32());

            // Back-off to the old way of shutting down the server on Windows, in case we
            // are managing a pre-2.6.0rc0 service, which did not have the event.
            //
            try {
                DBClientConnection conn;
                conn.connect(
                    HostAndPort{"127.0.0.1:" + std::to_string(port)}, "MongoDB Shell", boost::none);

                BSONElement authObj = opt["auth"];

                if (!authObj.eoo()) {
                    string errMsg;
                    conn.auth("admin", authObj["user"].String(), authObj["pwd"].String(), errMsg);

                    if (!errMsg.empty()) {
                        cout << "Failed to authenticate before shutdown: " << errMsg << endl;
                    }
                }

                BSONObj info;
                BSONObjBuilder b;
                b.append("shutdown", 1);
                b.append("force", 1);
                conn.runCommand(DatabaseName::kAdmin, b.done(), info);
            } catch (...) {
                // Do nothing. This command never returns data to the client and the driver
                // doesn't like that.
                //
            }
        }
        return;
    }

    ON_BLOCK_EXIT([&] { CloseHandle(event); });

    bool result = SetEvent(event);
    if (!result) {
        auto ec = lastSystemError();
        LOGV2_ERROR(22833, "kill_wrapper SetEvent failed", "error"_attr = errorMessage(ec));
        return;
    }
#else
    if (kill(pid.toNative(), sig)) {
        auto ec = lastPosixError();
        if (ec == posixError(ESRCH)) {
        } else {
            LOGV2_INFO(22816, "Kill failed", "error"_attr = errorMessage(ec));
            uasserted(ErrorCodes::UnknownError,
                      "kill({}, {}) failed: {}"_format(pid.toNative(), sig, errorMessage(ec)));
        }
    }

#endif
}

int killDb(int port, ProcessId _pid, int signal, const BSONObj& opt, bool waitPid = true) {
    ProcessId pid;
    auto registry = ProgramRegistry::get(getGlobalServiceContext());
    if (port > 0) {
        if (!registry->isPortRegistered(port)) {
            LOGV2_INFO(22817, "No db started on port", "port"_attr = port);
            return 0;
        }
        pid = registry->pidForPort(port);
    } else {
        pid = _pid;
    }

    kill_wrapper(pid, signal, port, opt);

    // If we are not waiting for the process to end, then return immediately.
    if (!waitPid) {
        LOGV2_INFO(22818, "Skip waiting for process to terminate", "pid"_attr = pid);
        return 0;
    }

    int exitCode = static_cast<int>(ExitCode::fail);
    try {
        LOGV2_INFO(22819, "Waiting for process to terminate.", "pid"_attr = pid);
        registry->waitForPid(pid, true, &exitCode);
    } catch (...) {
        LOGV2_WARNING(22828, "Process failed to terminate.", "pid"_attr = pid);
        return static_cast<int>(ExitCode::fail);
    }

    if (signal == SIGKILL) {
        sleepmillis(4000);  // allow operating system to reclaim resources
    }

    return exitCode;
}

int killDb(int port, ProcessId _pid, int signal) {
    BSONObj dummyOpt;
    return killDb(port, _pid, signal, dummyOpt);
}

int getSignal(const BSONObj& a) {
    int ret = SIGTERM;
    if (a.nFields() >= 2) {
        BSONObjIterator i(a);
        i.next();
        BSONElement e = i.next();
        uassert(ErrorCodes::BadValue, "Expected a signal number", e.isNumber());
        ret = int(e.number());
    }
    return ret;
}

BSONObj getStopMongodOpts(const BSONObj& a) {
    if (a.nFields() == 3) {
        BSONObjIterator i(a);
        i.next();
        i.next();
        BSONElement e = i.next();

        if (e.isABSONObj()) {
            return e.embeddedObject();
        }
    }

    return BSONObj();
}

bool getWaitPid(const BSONObj& a) {
    if (a.nFields() == 4) {
        BSONObjIterator i(a);
        i.next();
        i.next();
        i.next();
        BSONElement e = i.next();
        if (e.isBoolean()) {
            return e.boolean();
        }
    }
    // Default to wait for pid.
    return true;
}

/** stopMongoProgram(port[, signal]) */
BSONObj StopMongoProgram(const BSONObj& a, void* data) {
    int nFields = a.nFields();
    uassert(ErrorCodes::FailedToParse, "wrong number of arguments", nFields >= 1 && nFields <= 4);
    uassert(ErrorCodes::BadValue, "stopMongoProgram needs a number", a.firstElement().isNumber());
    int port = int(a.firstElement().number());
    LOGV2_INFO(22820, "shell: Stopping mongo program", "waitpid"_attr = getWaitPid(a));
    int code =
        killDb(port, ProcessId::fromNative(0), getSignal(a), getStopMongodOpts(a), getWaitPid(a));
    LOGV2_INFO(22821, "shell: Stopped mongo program on port", "port"_attr = port);
    return BSON("" << (double)code);
}

BSONObj StopMongoProgramByPid(const BSONObj& a, void* data) {
    int nFields = a.nFields();
    uassert(ErrorCodes::FailedToParse, "wrong number of arguments", nFields >= 1 && nFields <= 3);
    uassert(
        ErrorCodes::BadValue, "stopMongoProgramByPid needs a number", a.firstElement().isNumber());
    ProcessId pid = ProcessId::fromNative(int(a.firstElement().number()));
    int code = killDb(0, pid, getSignal(a), getStopMongodOpts(a));
    LOGV2_INFO(22822, "shell: Stopped mongo program with pid", "pid"_attr = pid);
    return BSON("" << (double)code);
}

BSONObj ConvertTrafficRecordingToBSON(const BSONObj& a, void* data) {
    int nFields = a.nFields();
    uassert(ErrorCodes::FailedToParse, "wrong number of arguments", nFields == 1);

    auto arr = trafficRecordingFileToBSONArr(a.firstElement().String());
    return BSON("" << arr);
}

int KillMongoProgramInstances() {
    vector<ProcessId> pids;
    auto registry = ProgramRegistry::get(getGlobalServiceContext());
    registry->getRegisteredPids(pids);
    int returnCode = static_cast<int>(ExitCode::clean);
    for (auto&& pid : pids) {
        int port = registry->portForPid(pid);
        int code = killDb(port != -1 ? port : 0, pid, SIGTERM);
        if (code != static_cast<int>(ExitCode::clean)) {
            LOGV2_INFO(
                22823, "Process exited with error code", "pid"_attr = pid, "code"_attr = code);
            returnCode = code;
        }
    }
    return returnCode;
}

/**
 * Reads a set of test named pipes. 'args' BSONObj should contain one or more fields like:
 *   "0": string; relative path of the first pipe
 *   "1": string; relative path of the second pipe
 *   ...
 * Any field names not sequentially numbered from 0 will be ignored.
 */
BSONObj ReadTestPipes(const BSONObj& args, void* unused) {
    int fieldNum = 0;                            // next field name in numeric form
    BSONElement pipePathElem;                    // next pipe relative path
    std::vector<std::string> pipeRelativePaths;  // all pipe relative paths

    do {
        pipePathElem = BSONElement(args.getField(std::to_string(fieldNum)));
        if (pipePathElem.type() == BSONType::String) {
            pipeRelativePaths.emplace_back(pipePathElem.str());
        } else if (pipePathElem.type() != BSONType::EOO) {
            uasserted(ErrorCodes::FailedToParse,
                      "Argument {} (pipe path) must be a string"_format(fieldNum));
        }
        ++fieldNum;
    } while (pipePathElem.type() != BSONType::EOO);

    if (pipeRelativePaths.size() > 0) {
        return NamedPipeHelper::readFromPipes(pipeRelativePaths);
    }
    return {};
}

/**
 * Writes a test named pipe of generated BSONobj's. 'args' BSONObj should contain fields:
 *   "0": string; relative path of the pipe
 *   "1": number; number of BSON objects to write to the pipe
 *   "2": OPTIONAL number; lower bound on size of "string" field in generated object (default 0)
 *   "3": OPTIONAL number; upper bound on size of "string" field in generated object (default 2048)
 *     capped at 16,750,000 (slightly less than BSON object maximum of 16 MB)
 *   "4": OPTIONAL string; absolute path to the directory where named pipes exist. If not given,
 *        'kDefaultPipePath' is used.
 */
BSONObj WriteTestPipe(const BSONObj& args, void* unused) {
    int nFields = args.nFields();
    uassert(ErrorCodes::FailedToParse,
            "wrong number of arguments"_format(nFields),
            nFields >= 2 && nFields <= 5);

    const long kStringMaxSize = 16750000;  // max allowed size for generated object's "string" field
    BSONElement pipePathElem(args.getField("0"));
    BSONElement objectsElem(args.getField("1"));
    BSONElement stringMinSizeStr(args.getField("2"));
    BSONElement stringMaxSizeStr(args.getField("3"));
    long stringMinSize = 0;     // default "string" field minimum size
    long stringMaxSize = 2048;  // default "string" field maximum size

    uassert(ErrorCodes::FailedToParse,
            "First argument (pipe path) must be a string",
            pipePathElem.type() == BSONType::String);
    uassert(ErrorCodes::FailedToParse,
            "Second argument (number of objects) must be a number",
            objectsElem.isNumber());
    if (stringMinSizeStr.isNumber()) {  // optional
        stringMinSize = stringMinSizeStr.numberLong();
        if (stringMinSize < 0) {
            stringMinSize = 0;
        }
        if (stringMinSize > kStringMaxSize) {
            stringMinSize = kStringMaxSize;
        }
    }
    if (stringMaxSizeStr.isNumber()) {  // optional
        stringMaxSize = stringMaxSizeStr.numberLong();
        if (stringMaxSize < 0) {
            stringMaxSize = 0;
        }
        if (stringMaxSize > kStringMaxSize) {
            stringMaxSize = kStringMaxSize;
        }
    }
    uassert(ErrorCodes::FailedToParse,
            "Third argument (string min size) must be <= fourth argument (string max size)",
            stringMinSize <= stringMaxSize);

    std::string pipeDir = [&] {
        if (nFields == 5) {
            BSONElement pipeDirElem(args.getField("4"));
            uassert(ErrorCodes::FailedToParse,
                    "Fifth argument (pipe dir) must be a string",
                    pipeDirElem.type() == BSONType::String);
            return pipeDirElem.str();
        } else {
            return kDefaultPipePath.toString();
        }
    }();

    NamedPipeHelper::writeToPipeAsync(std::move(pipeDir),
                                      pipePathElem.str(),
                                      objectsElem.numberLong(),
                                      stringMinSize,
                                      stringMaxSize);

    return {};
}

namespace {

/**
 * Attempts to read the requested number of bytes from the given input stream to the given buffer
 * and returns the number of bytes actually read.
 */
int32_t readBytes(char* buf, int32_t count, std::ifstream& ifs) {
    ifs.read(buf, count);
    return ifs.gcount();
}

}  // namespace

/**
 * Writes a test named pipe of BSONobj's that are first read into memory from a BSON file, then
 * round-robinned into the pipe up to the requested number of objects. This is the same as function
 * WriteTestPipeObjects except the objects are read from a file instead of passed in as a BSONArray.
 *
 * args:
 *   "0": string; relative path of the pipe
 *   "1": number; number of BSON objects to write to the pipe
 *   "2": string; relative path to the file of BSON objects; these must fit in memory
 *   "3": OPTIONAL string; absolute path to the directory where named pipes exist. If not given,
 *        'kDefaultPipePath' is used.
 *
 * async: true, write asynchronously; false, write synchronously
 */
BSONObj writeTestPipeBsonFileHelper(const BSONObj& args, bool async) {
    int nFields = args.nFields();
    uassert(ErrorCodes::FailedToParse,
            "Function requires 3 or 4 arguments but {} were given"_format(nFields),
            nFields == 3 || nFields == 4);

    BSONElement pipePathElem(args.getField("0"));
    BSONElement objectsElem(args.getField("1"));
    BSONElement bsonFilePathElem(args.getField("2"));

    uassert(ErrorCodes::FailedToParse,
            "First argument (pipe path) must be a string",
            pipePathElem.type() == BSONType::String);
    uassert(ErrorCodes::FailedToParse,
            "Second argument (number of objects) must be a number",
            objectsElem.isNumber());
    uassert(ErrorCodes::FailedToParse,
            "Third argument (BSON file path) must be a string",
            bsonFilePathElem.type() == BSONType::String);

    std::string pipeDir = [&] {
        if (nFields == 4) {
            BSONElement pipeDirElem(args.getField("3"));
            uassert(ErrorCodes::FailedToParse,
                    "Fourth argument (pipe dir) must be a string",
                    pipeDirElem.type() == BSONType::String);
            return pipeDirElem.str();
        } else {
            return kDefaultPipePath.toString();
        }
    }();

    // Open the BSON object file.
    std::ifstream ifs(bsonFilePathElem.str(), std::ios::binary | std::ios::in);
    uassert(
        ErrorCodes::FileOpenFailed,
        "Failed to open '{}': {}"_format(bsonFilePathElem.str(), errorMessage(lastSystemError())),
        ifs.is_open());

    // Read the BSON object file into a vector of BSONObj.
    const int32_t kSizeSize = sizeof(int32_t);
    std::vector<BSONObj> bsonObjs;
    char sizeBuf[kSizeSize];  // buffer to read size of next BSONObj into
    bool eof = false;
    while (!eof) {
        int32_t nBytes = readBytes(sizeBuf, kSizeSize, ifs);
        if (nBytes == kSizeSize) {
            int32_t size = ConstDataView(sizeBuf).read<LittleEndian<int32_t>>();
            SharedBuffer buf = SharedBuffer::allocate(size);  // buffer for the full BSONObj
            invariant(buf.get());

            memcpy(buf.get(), sizeBuf, kSizeSize);
            int32_t totalRead = kSizeSize;
            while ((nBytes = readBytes(buf.get() + totalRead, size - totalRead, ifs)) > 0) {
                totalRead += nBytes;
                if (totalRead == size) {
                    break;
                }
            }
            uassert(ErrorCodes::InvalidBSON,
                    "Expected {} bytes in BSON object but got {}"_format(size, totalRead),
                    totalRead == size);

            bsonObjs.emplace_back(buf);
        } else {
            eof = true;
            uassert(ErrorCodes::InvalidBSON,
                    "Expected {} bytes in size field but got {}"_format(kSizeSize, nBytes),
                    nBytes == 0);  // 0 is normal EOF
        }
    }  // while !eof

    // Write the pipe.
    if (async) {
        NamedPipeHelper::writeToPipeObjectsAsync(
            std::move(pipeDir), pipePathElem.str(), objectsElem.numberLong(), std::move(bsonObjs));
    } else {
        NamedPipeHelper::writeToPipeObjects(
            std::move(pipeDir), pipePathElem.str(), objectsElem.numberLong(), std::move(bsonObjs));
    }

    return {};
}

/**
 * Asynchronously writes a test named pipe of BSONobj's that are first read into memory from a BSON
 * file. See writeTestPipeBsonFileHelper() header for more info.
 */
BSONObj WriteTestPipeBsonFile(const BSONObj& args, void* unused) {
    return writeTestPipeBsonFileHelper(args, true);
}

/**
 * Synchronously writes a test named pipe of BSONobj's that are first read into memory from a BSON
 * file. See writeTestPipeBsonFileHelper() header for more info.
 */
BSONObj WriteTestPipeBsonFileSync(const BSONObj& args, void* unused) {
    return writeTestPipeBsonFileHelper(args, false);
}

/**
 * Writes a test named pipe by round-robinning caller-provided objects to the pipe. 'args' BSONObj
 * should contain fields:
 *   "0": string; relative path of the pipe
 *   "1": number; number of BSON objects to write to the pipe
 *   "2": BSONArray; array of objects to round-robin write to the pipe
 *   "3": OPTIONAL string; absolute path to the directory where named pipes exist. If not given,
 *        'kDefaultPipePath' is used.
 */
BSONObj WriteTestPipeObjects(const BSONObj& args, void* unused) {
    int nFields = args.nFields();
    uassert(ErrorCodes::FailedToParse,
            "Function requires 3 or 4 arguments but {} were given"_format(nFields),
            nFields == 3 || nFields == 4);

    BSONElement pipePathElem(args.getField("0"));
    BSONElement objectsElem(args.getField("1"));
    BSONElement bsonElems(args.getField("2"));

    uassert(ErrorCodes::FailedToParse,
            "First argument (pipe path) must be a string",
            pipePathElem.type() == BSONType::String);
    uassert(ErrorCodes::FailedToParse,
            "Second argument (number of objects) must be a number",
            objectsElem.isNumber());
    uassert(ErrorCodes::FailedToParse,
            "Third argument must be an array of objects to round-robin over",
            bsonElems.type() == mongo::Array);

    std::string pipeDir = [&] {
        if (nFields == 4) {
            BSONElement pipeDirElem(args.getField("3"));
            uassert(ErrorCodes::FailedToParse,
                    "Fourth argument (pipe dir) must be a string",
                    pipeDirElem.type() == BSONType::String);
            return pipeDirElem.str();
        } else {
            return kDefaultPipePath.toString();
        }
    }();

    // Convert bsonElems into bsonObjs as the former are pointers into local stack memory that will
    // become invalid when this method returns, but they are needed by the async writer thread.
    std::vector<BSONElement> bsonElemsVector = bsonElems.Array();
    std::vector<BSONObj> bsonObjs;
    for (BSONElement bsonElem : bsonElemsVector) {
        bsonObjs.emplace_back(bsonElem.Obj().getOwned());
    }

    // Write the pipe asynchronously.
    NamedPipeHelper::writeToPipeObjectsAsync(
        std::move(pipeDir), pipePathElem.str(), objectsElem.numberLong(), std::move(bsonObjs));

    return {};
}

std::vector<ProcessId> getRunningMongoChildProcessIds() {
    std::vector<ProcessId> registeredPids, outPids;
    auto registry = ProgramRegistry::get(getGlobalServiceContext());
    registry->getRegisteredPids(registeredPids);
    // Only return processes that are still alive. A client may have started a program using a mongo
    // helper but terminated another way. E.g. if a mongod is started with MongoRunner.startMongod
    // but exited with db.shutdownServer.
    std::copy_if(registeredPids.begin(),
                 registeredPids.end(),
                 std::back_inserter(outPids),
                 [registry](const ProcessId& pid) {
                     bool isDead = registry->isPidDead(pid);
                     return !isDead;
                 });
    return outPids;
}

BSONObj RunningMongoChildProcessIds(const BSONObj&, void*) {
    std::vector<ProcessId> pids = getRunningMongoChildProcessIds();
    BSONObjBuilder bob;
    BSONArrayBuilder pidArr(bob.subarrayStart("runningPids"));
    for (const auto& pid : pids) {
        pidArr << pid.asInt64();
    }
    pidArr.done();
    return bob.obj();
}

// (Generic FCV reference): Propagate generic FCV constants to the shell.
BSONObj GetFCVConstants(const BSONObj&, void*) {
    BSONObjBuilder bob;
    BSONObjBuilder subObj(bob.subobjStart(""));

    subObj.append("latest", multiversion::toString(multiversion::GenericFCV::kLatest));
    subObj.append("lastContinuous",
                  multiversion::toString(multiversion::GenericFCV::kLastContinuous));
    subObj.append("lastLTS", multiversion::toString(multiversion::GenericFCV::kLastLTS));
    subObj.append("numSinceLastLTS", static_cast<int>(multiversion::kSinceLastLTS));
    subObj.done();

    return bob.obj();
}

MongoProgramScope::~MongoProgramScope() {
    DESTRUCTOR_GUARD(KillMongoProgramInstances(); ClearRawMongoProgramOutput(BSONObj(), nullptr))
}

/**
 * Defines (funcName, CallbackFunction) pairs where funcName becomes the name of a function in the
 * mongo test shell and CallbackFunction is its C++ callback (handler). The callbacks must all have
 * signatures like
 *    BSONObj CallbackFunction(const BSONObj& args, void* data)
 * (contract from injectNative()), though nobody is using the data parameter at time of writing.
 *
 * The BSONObj they return must put the result into field "" such as
 *   return BSON("" << true);
 * or
 *   return BSON("" << BSON("resultInfo1" << resultValue1 << "resultInfo2" << resultValue2));
 *
 * In the shell these are called like
 *   funcName(arg1, arg2, ...)
 * for example
 *   _writeTestPipe("my_pipe_file", 1234)
 * The args will come in as the BSONObj first parameter of the callback with fields named
 * sequentially from "0", e.g. for the above:
 *   {"0": "my_pipe_file", "1": 1234}
 */
void installShellUtilsLauncher(Scope& scope) {
    scope.injectNative("_startMongoProgram", StartMongoProgram);
    scope.injectNative("_runningMongoChildProcessIds", RunningMongoChildProcessIds);
    scope.injectNative("runProgram", RunMongoProgram);
    scope.injectNative("run", RunMongoProgram);
    scope.injectNative("_runMongoProgram", RunMongoProgram);
    scope.injectNative("runNonMongoProgram", RunNonMongoProgram);
    scope.injectNative("_stopMongoProgram", StopMongoProgram);
    scope.injectNative("stopMongoProgramByPid", StopMongoProgramByPid);
    scope.injectNative("rawMongoProgramOutput", RawMongoProgramOutput);
    scope.injectNative("clearRawMongoProgramOutput", ClearRawMongoProgramOutput);
    scope.injectNative("waitProgram", WaitProgram);
    scope.injectNative("waitMongoProgram", WaitMongoProgram);
    scope.injectNative("checkProgram", CheckProgram);
    scope.injectNative("resetDbpath", ResetDbpath);
    scope.injectNative("pathExists", PathExists);
    scope.injectNative("copyDbpath", CopyDbpath);
    scope.injectNative("convertTrafficRecordingToBSON", ConvertTrafficRecordingToBSON);
    scope.injectNative("getFCVConstants", GetFCVConstants);
    scope.injectNative("_readTestPipes", ReadTestPipes);
    scope.injectNative("_writeTestPipe", WriteTestPipe);
    scope.injectNative("_writeTestPipeBsonFile", WriteTestPipeBsonFile);
    scope.injectNative("_writeTestPipeBsonFileSync", WriteTestPipeBsonFileSync);
    scope.injectNative("_writeTestPipeObjects", WriteTestPipeObjects);
}
}  // namespace shell_utils
}  // namespace mongo
