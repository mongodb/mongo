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


#include "mongo/platform/basic.h"

#include "mongo/db/initialize_server_global_state.h"
#include "mongo/db/initialize_server_global_state_gen.h"

#include <boost/filesystem/operations.hpp>
#include <fmt/format.h>
#include <iostream>
#include <memory>

#ifndef _WIN32
#include <sys/stat.h>
#include <sys/wait.h>
#include <syslog.h>
#include <unistd.h>
#endif

#include "mongo/base/init.h"
#include "mongo/config.h"
#include "mongo/db/server_options.h"
#include "mongo/logv2/log.h"
#include "mongo/logv2/log_domain_global.h"
#include "mongo/platform/process_id.h"
#include "mongo/util/exit_code.h"
#include "mongo/util/processinfo.h"
#include "mongo/util/quick_exit.h"
#include "mongo/util/str.h"
#include "mongo/util/testing_proctor.h"

#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kControl


namespace mongo::initialize_server_global_state {

#ifndef _WIN32
static void croak(StringData prefix, int savedErr = errno) {
    std::cout << prefix << ": " << errorMessage(posixError(savedErr)) << std::endl;
    quickExit(ExitCode::abrupt);
}

void signalForkSuccess() {
    if (!serverGlobalParams.doFork)
        return;
    int* f = &serverGlobalParams.forkReadyFd;
    if (*f == -1)
        return;
    while (true) {
        const char c = 1;
        if (ssize_t nw = write(*f, &c, 1); nw == -1) {
            int savedErr = errno;
            if (savedErr == EINTR)
                continue;
            if (savedErr == EPIPE)
                break;  // The pipe read side has closed.
            else {
                auto ec = posixError(savedErr);
                LOGV2_WARNING(4656300,
                              "Write to child pipe failed",
                              "errno"_attr = ec.value(),
                              "errnoDesc"_attr = errorMessage(ec));
                quickExit(ExitCode::fail);
            }
        } else if (nw == 0) {
            continue;
        } else {
            break;
        }
    }
    if (close(*f) == -1) {
        auto ec = lastPosixError();
        LOGV2_WARNING(4656301,
                      "Closing write pipe failed",
                      "errno"_attr = ec.value(),
                      "errnoDesc"_attr = errorMessage(ec));
    }
    *f = -1;
}
#endif

/**
 * "Double fork" idiom to decouple mongod from the launcher process group (job) and terminal
 * session. We ensure that the daemon runs in a leaderless session. This protects it
 * from accidentally acquiring a controlling terminal should it open a terminal device
 * file.
 *
 * https://pubs.opengroup.org/onlinepubs/9699919799/basedefs/V1_chap11.html#tag_11_01_03
 *
 * Original process is <launcher>, which forks <middle>, which in turn forks <daemon>.
 *
 *       <launcher>                 // pid: <launcher>, pgid: <launcher>, sid: <?>
 *         |                        // [pid==pgid, so <launcher> is group leader]
 *         fork():
 *             + <launcher>
 *             |   | waitpid(<middle>)
 *             |   | exit with <middle>'s exit code
 *             |
 *             + <middle>           // pid: <middle>,   pgid: <launcher>, sid: <?>
 *                 |                // [<middle> is NOT group leader, thus it can `setsid()`]
 *                 setsid()         // pid: <middle>,   pgid: <middle>,   sid: <middle>
 *                 |                // [<middle> is leader of its own session and group]
 *                 pipe()
 *                 fork():
 *                     + <middle>
 *                     |  |read 1 byte from pipe
 *                     |  |if the read fails:
 *                     |  |    waitpid(<daemon>)
 *                     |  |    exit with <daemon>'s exit code
 *                     |  |exit successfully if the read succeeds
 *                     |
 *                     + <daemon>   // pid: <daemon>,   pgid: <middle>,   sid: <middle>
 *                        |         // [<daemon> leads neither its session nor its group]
 *                        |...
 *                        |(continue initializing)
 *                        |READY to serve:
 *                        |    write 1 byte to pipe
 *                        |(run forever)
 *                        |...
 *
 * The first fork creates a <middle> process. The important thing about <middle> is that
 * it is not a process group (job) leader, and is therefore not being controlled by its
 * session's terminal. This property allows <middle> to call `setsid()` and create a new
 * session, of which it will be the de facto leader. Note that `setsid()` FAILS if
 * called by a process group leader. Process group leaders are not allowed to disconnect
 * from their session, and so the fork to create <middle> is necessary.  This new
 * session will have no controlling terminal, because <middle>, with its simple code
 * path, does not open any terminal devices.
 *
 * The second fork, from <middle>, creates the <daemon> process, which will be member of
 * the <middle> process group and the newly created and unconnected <middle> session.
 * Because the <daemon> is not the originator of its session, it will can never be
 * controlled by a terminal, even if it opens a terminal device.
 *
 * Another side effect of this idiom is that the <daemon> has no parent, so it leaves no
 * zombie when it dies (it is reaped by the pid 1 init process). Only one fork is
 * required to achieve this property, however. The double fork is only necessary because
 * of the controlling terminal issue.
 *
 * Care is taken that the <launcher> process waits until <daemon> reports that it is
 * ready (serving), and that if <daemon> dies before signalling readiness, its exit code
 * is propagated through <middle> to become the exit code of the <launcher>.
 *
 * The idiom is explained in APUE (Stevens).
 */
static bool forkServer() {
#if defined(_WIN32) || (defined(__APPLE__) && TARGET_OS_TV)
    return true;
#else
    if (!serverGlobalParams.doFork)
        return true;

    fassert(16447, !serverGlobalParams.logpath.empty() || serverGlobalParams.logWithSyslog);

    std::cout.flush();
    std::cerr.flush();

    std::cout << "about to fork child process, waiting until server is ready for connections."
              << std::endl;

    auto waitAndPropagate = [&](pid_t pid, ExitCode signalCode, bool verbose) {
        int pstat;
        if (waitpid(pid, &pstat, 0) == -1)
            croak("waitpid");
        if (!WIFEXITED(pstat))
            quickExit(signalCode);
        if (int ec = WEXITSTATUS(pstat)) {
            if (verbose)
                std::cout << "ERROR: child process failed, exited with " << ec << std::endl
                          << "To see additional information in this output, start without "
                          << "the \"--fork\" option." << std::endl;
            quickExit(ExitCode::fail);
        }
        if (verbose)
            std::cout << "child process started successfully, parent exiting" << std::endl;
        quickExit(ExitCode::clean);
    };

    // Start in the <launcher> process.
    switch (pid_t middle = fork()) {
        case -1:
            croak("ERROR: stage 1 fork() failed");
            break;
        default:
            // In the <launcher> process
            waitAndPropagate(middle, ExitCode::launcherMiddleError, true);
            break;
        case 0:
            break;
    }

    // In the <middle> process

    if (chdir("/") < 0)
        croak("Cannot chdir() while forking server process");

    if (setsid() == -1)
        croak("setsid");

    int readyPipe[2];
    if (pipe(readyPipe) != 0)
        croak("pipe");

    switch (pid_t daemon = fork()) {
        case -1:
            croak("ERROR: stage 2 fork() failed");
            break;
        default: {
            // In the <middle> process
            if (close(readyPipe[1]) == -1)  // <middle> does not write pipe
                croak("closing write side of pipe failed");
            char c;
            ssize_t nr;
            while ((nr = read(readyPipe[0], &c, 1)) == -1 && errno == EINTR) {
            }
            if (nr == -1)
                croak("pipe read failed");
            if (nr == 0)
                // pipe reached eof without the daemon signalling readiness.
                // Wait for <daemon> to exit, and exit with its exit code.
                waitAndPropagate(daemon, ExitCode::launcherError, false);
            quickExit(ExitCode::clean);
        } break;
        case 0:
            break;
    }

    // In the <daemon> process (i.e. the server)
    if (close(readyPipe[0]) == -1)  // <daemon> does not read pipe
        croak("closing read side of pipe failed");
    serverGlobalParams.forkReadyFd = readyPipe[1];

    std::cout << format(FMT_STRING("forked process: {}"), getpid()) << std::endl;

    auto stdioDetach = [](FILE* fp, const char* mode, StringData name) {
        if (!freopen("/dev/null", mode, fp)) {
            int saved = errno;
            std::cout << format(FMT_STRING("Cannot reassign {} while forking server process: {}"),
                                name,
                                strerror(saved))
                      << std::endl;
            return false;
        }
        return true;
    };
    if (!stdioDetach(stdin, "r", "stdin"))
        return false;
    if (!stdioDetach(stderr, "w", "stderr"))
        return false;
    if (!stdioDetach(stdout, "w", "stdout"))
        return false;
    return true;
#endif  // !defined(_WIN32)
}

void forkServerOrDie() {
    if (!forkServer())
        quickExit(ExitCode::fail);
}

namespace {

bool checkAndMoveLogFile(const std::string& absoluteLogpath) {
    bool exists;

    try {
        exists = boost::filesystem::exists(absoluteLogpath);
    } catch (boost::filesystem::filesystem_error& e) {
        uasserted(ErrorCodes::FileNotOpen,
                  str::stream() << "Failed probe for \"" << absoluteLogpath
                                << "\": " << e.code().message());
    }

    if (exists) {
        if (boost::filesystem::is_directory(absoluteLogpath)) {
            uasserted(ErrorCodes::FileNotOpen,
                      str::stream() << "logpath \"" << absoluteLogpath
                                    << "\" should name a file, not a directory.");
        }

        if (!serverGlobalParams.logAppend && boost::filesystem::is_regular(absoluteLogpath)) {
            std::string renameTarget = absoluteLogpath + "." + terseCurrentTimeForFilename();
            boost::system::error_code ec;
            boost::filesystem::rename(absoluteLogpath, renameTarget, ec);
            if (!ec) {
                LOGV2(20697,
                      "Moving existing log file \"{oldLogPath}\" to \"{newLogPath}\"",
                      "Renamed existing log file",
                      "oldLogPath"_attr = absoluteLogpath,
                      "newLogPath"_attr = renameTarget);
            } else {
                uasserted(ErrorCodes::FileRenameFailed,
                          str::stream() << "Could not rename preexisting log file \""
                                        << absoluteLogpath << "\" to \"" << renameTarget
                                        << "\"; run with --logappend or manually remove file: "
                                        << ec.message());
            }
        }
    }
    return exists;
}
}  // namespace

MONGO_INITIALIZER_GENERAL(ServerLogRedirection,
                          ("EndStartupOptionHandling", "ForkServer", "TestingDiagnostics"),
                          ("default"))
(InitializerContext*) {
    // Hook up this global into our logging encoder
    auto& lv2Manager = logv2::LogManager::global();
    logv2::LogDomainGlobal::ConfigurationOptions lv2Config;
    lv2Config.maxAttributeSizeKB = &gMaxLogAttributeSizeKB;
    bool writeServerRestartedAfterLogConfig = false;

    if (serverGlobalParams.logWithSyslog) {
#ifdef _WIN32
        uasserted(ErrorCodes::InternalError,
                  "Syslog requested in Windows build; command line processor logic error");
#else
        lv2Config.consoleEnabled = false;
        lv2Config.syslogEnabled = true;
        lv2Config.syslogFacility = serverGlobalParams.syslogFacility;
#endif  // defined(_WIN32)
    } else if (!serverGlobalParams.logpath.empty()) {
        fassert(16448, !serverGlobalParams.logWithSyslog);
        std::string absoluteLogpath =
            boost::filesystem::absolute(serverGlobalParams.logpath, serverGlobalParams.cwd)
                .string();

        bool exists = checkAndMoveLogFile(absoluteLogpath);

        lv2Config.consoleEnabled = false;
        lv2Config.fileEnabled = true;
        lv2Config.filePath = absoluteLogpath;
        lv2Config.fileRotationMode = serverGlobalParams.logRenameOnRotate
            ? logv2::LogDomainGlobal::ConfigurationOptions::RotationMode::kRename
            : logv2::LogDomainGlobal::ConfigurationOptions::RotationMode::kReopen;
        lv2Config.fileOpenMode = serverGlobalParams.logAppend
            ? logv2::LogDomainGlobal::ConfigurationOptions::OpenMode::kAppend
            : logv2::LogDomainGlobal::ConfigurationOptions::OpenMode::kTruncate;

        if (serverGlobalParams.logAppend && exists) {
            writeServerRestartedAfterLogConfig = true;
        }
    }

    if (TestingProctor::instance().isEnabled() && !gBacktraceLogFile.empty()) {
        std::string absoluteLogpath =
            boost::filesystem::absolute(gBacktraceLogFile, serverGlobalParams.cwd).string();

        /* ignore */ checkAndMoveLogFile(absoluteLogpath);

        lv2Config.backtraceFilePath = absoluteLogpath;
    }

    lv2Config.timestampFormat = serverGlobalParams.logTimestampFormat;
    Status result = lv2Manager.getGlobalDomainInternal().configure(lv2Config);
    if (result.isOK() && writeServerRestartedAfterLogConfig) {
        LOGV2(20698, "***** SERVER RESTARTED *****");
    }
    uassertStatusOK(result);
}

/**
 * atexit handler to terminate the process before static destructors run.
 *
 * Mongo server processes cannot safely call ::exit() or std::exit(), but
 * some third-party libraries may call one of those functions.  In that
 * case, to avoid static-destructor problems in the server, this exits the
 * process immediately with code ExitCode::fail.
 *
 * TODO: Remove once exit() executes safely in mongo server processes.
 */
static void shortCircuitExit() {
    quickExit(ExitCode::fail);
}

MONGO_INITIALIZER(RegisterShortCircuitExitHandler)(InitializerContext*) {
    if (std::atexit(&shortCircuitExit) != 0)
        uasserted(ErrorCodes::InternalError, "Failed setting short-circuit exit handler.");
}

bool checkSocketPath() {
#ifndef _WIN32
    if (!serverGlobalParams.noUnixSocket &&
        !boost::filesystem::is_directory(serverGlobalParams.socket)) {
        std::cout << serverGlobalParams.socket << " must be a directory" << std::endl;
        return false;
    }
#endif

    return true;
}

bool writePidFile() {
    return serverGlobalParams.pidFile.empty() ? true
                                              : mongo::writePidFile(serverGlobalParams.pidFile);
}

#ifndef _WIN32
namespace {
// Handling for `honorSystemUmask` and `processUmask` setParameters.
// Non-Windows platforms only.
//
// If honorSystemUmask is true, processUmask may not be set
// and the umask will be left exactly as set by the OS.
//
// If honorSystemUmask is false, then we will still honor the 'user'
// portion of the current umask, but the group/other bits will be
// set to 1, or whatever value is provided by processUmask if specified.

// processUmask set parameter may only override group/other bits.
constexpr mode_t kValidUmaskBits = S_IRWXG | S_IRWXO;

// By default, honorSystemUmask==false masks all group/other bits.
constexpr mode_t kDefaultProcessUmask = S_IRWXG | S_IRWXO;

bool honorSystemUmask = false;
boost::optional<mode_t> umaskOverride;

mode_t getUmaskOverride() {
    return umaskOverride ? *umaskOverride : kDefaultProcessUmask;
}

// We need to set our umask before opening any log files.
MONGO_INITIALIZER_GENERAL(MungeUmask, ("EndStartupOptionHandling"), ("ServerLogRedirection"))
(InitializerContext*) {
    if (!honorSystemUmask) {
        // POSIX does not provide a mechanism for reading the current umask
        // without modifying it.
        // Do this conservatively by setting a short-lived umask of 0777
        // in order to pull out the user portion of the current umask.
        umask((umask(S_IRWXU | S_IRWXG | S_IRWXO) & S_IRWXU) | getUmaskOverride());
    }
}
}  // namespace
#endif

// --setParameter honorSystemUmask
Status HonorSystemUMaskServerParameter::setFromString(const std::string& value) {
#ifndef _WIN32
    if ((value == "0") || (value == "false")) {
        // false may be specified with processUmask
        // since it defines precisely how we're not honoring system umask.
        honorSystemUmask = false;
        return Status::OK();
    }

    if ((value == "1") || (value == "true")) {
        if (umaskOverride) {
            return {ErrorCodes::BadValue,
                    "honorSystemUmask and processUmask may not be specified together"};
        } else {
            honorSystemUmask = true;
            return Status::OK();
        }
    }

    return {ErrorCodes::BadValue, "honorSystemUmask must be 'true' or 'false'"};
#else
    return {ErrorCodes::InternalError, "honerSystemUmask is not available on windows"};
#endif
}

void HonorSystemUMaskServerParameter::append(OperationContext*,
                                             BSONObjBuilder& b,
                                             const std::string& name) {
#ifndef _WIN32
    b << name << honorSystemUmask;
#endif
}

// --setParameter processUmask
Status ProcessUMaskServerParameter::setFromString(const std::string& value) {
#ifndef _WIN32
    if (honorSystemUmask) {
        return {ErrorCodes::BadValue,
                "honorSystemUmask and processUmask may not be specified together"};
    }

    // Convert base from octal
    const char* val = value.c_str();
    char* end = nullptr;

    auto mask = std::strtoul(val, &end, 8);
    if (end && (end != (val + value.size()))) {
        return {ErrorCodes::BadValue,
                str::stream() << "'" << value << "' is not a valid octal value"};
    }

    if ((mask & kValidUmaskBits) != mask) {
        return {ErrorCodes::BadValue,
                str::stream() << "'" << value << "' attempted to set invalid umask bits"};
    }

    umaskOverride = static_cast<mode_t>(mask);
    return Status::OK();
#else
    return {ErrorCodes::InternalError, "processUmask is not available on windows"};
#endif
}

void ProcessUMaskServerParameter::append(OperationContext*,
                                         BSONObjBuilder& b,
                                         const std::string& name) {
#ifndef _WIN32
    b << name << static_cast<int>(getUmaskOverride());
#endif
}

}  // namespace mongo::initialize_server_global_state
