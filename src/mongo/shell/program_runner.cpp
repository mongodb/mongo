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

#include "mongo/shell/program_runner.h"

#include "boost/filesystem/operations.hpp"
#include <boost/iostreams/device/file_descriptor.hpp>
#include <boost/iostreams/stream.hpp>
#include <boost/iostreams/stream_buffer.hpp>
#include <boost/iostreams/tee.hpp>
#include <fcntl.h>

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
#include "mongo/base/parse_number.h"
#include "mongo/logv2/log.h"
#include "mongo/util/errno_util.h"
#include "mongo/util/text.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kDefault

namespace mongo::shell_utils {

using std::cout;
using std::endl;
using std::make_pair;
using std::pair;
using std::string;
using std::stringstream;
using std::vector;

#ifdef _WIN32
inline int close(int fd) {
    return _close(fd);
}
inline int read(int fd, void* buf, size_t size) {
    return _read(fd, buf, size);
}
inline int pipe(int fds[2]) {
    return _pipe(fds, 4096, _O_TEXT | _O_NOINHERIT);
}
#endif

namespace {
using namespace fmt::literals;

void safeClose(int fd) {
#ifndef _WIN32
    struct ScopedSignalBlocker {
        ScopedSignalBlocker() {
            sigset_t mask;
            sigfillset(&mask);
            pthread_sigmask(SIG_SETMASK, &mask, &_oldMask);
        }

        ~ScopedSignalBlocker() {
            pthread_sigmask(SIG_SETMASK, &_oldMask, nullptr);
        }

    private:
        sigset_t _oldMask;
    };
    const ScopedSignalBlocker block;
#endif
    if (close(fd) != 0) {
        auto ec = lastPosixError();
        LOGV2_ERROR(22829, "Failed to close fd", "fd"_attr = fd, "error"_attr = errorMessage(ec));
        fassertFailed(40318);
    }
}

const auto getProgramRegistry =
    ServiceContext::declareDecoration<std::unique_ptr<ProgramRegistry>>();
constexpr auto kParsingPortNumber = -2;
}  // namespace

void ProgramRegistry::create(ServiceContext* serviceContext) {
    fassert(7095700, !getProgramRegistry(serviceContext));
    getProgramRegistry(serviceContext) = std::make_unique<ProgramRegistry>();
}

ProgramRegistry* ProgramRegistry::get(ServiceContext* serviceContext) {
    return getProgramRegistry(serviceContext).get();
}

ProgramOutputMultiplexer* ProgramRegistry::getProgramOutputMultiplexer() {
    return &_programOutputMultiplexer;
}

ProgramRunner ProgramRegistry::createProgramRunner(BSONObj args, BSONObj env, bool isMongo) {
    return ProgramRunner(args, env, isMongo, this);
}

bool ProgramRegistry::isPortRegistered(int port) const {
    stdx::lock_guard<stdx::recursive_mutex> lk(_mutex);
    return _portToPidMap.count(port) == 1;
}

ProcessId ProgramRegistry::pidForPort(int port) const {
    stdx::lock_guard<stdx::recursive_mutex> lk(_mutex);
    invariant(isPortRegistered(port));
    return _portToPidMap.find(port)->second;
}

int ProgramRegistry::portForPid(ProcessId pid) const {
    stdx::lock_guard<stdx::recursive_mutex> lk(_mutex);
    for (const auto& portPid : _portToPidMap) {
        if (portPid.second == pid)
            return portPid.first;
    }
    return -1;
}

void ProgramRegistry::registerProgram(ProcessId pid, int port) {
    stdx::lock_guard<stdx::recursive_mutex> lk(_mutex);
    uassert(7095701,
            str::stream() << "Program cannot be reigstered because pid " << pid.toString()
                          << " is already in use",
            !isPidRegistered(pid));
    _registeredPids.emplace(pid);
    if (port != -1) {
        _portToPidMap.emplace(port, pid);
    }
}

void ProgramRegistry::unregisterProgram(ProcessId pid) {
    stdx::lock_guard<stdx::recursive_mutex> lk(_mutex);
    if (isPidRegistered(pid)) {
        // Some instances of ProgramRunner are constructed to read output synchronously. These
        // instances never created a separate stdx::thread and never called registerReaderThread().
        if (_outputReaderThreads.contains(pid)) {
            _outputReaderThreads[pid].join();
        }

        // Remove the PID from the registry.
        _outputReaderThreads.erase(pid);
        _portToPidMap.erase(portForPid(pid));
        _registeredPids.erase(pid);
    }
}

void ProgramRegistry::registerReaderThread(ProcessId pid, stdx::thread reader) {
    stdx::lock_guard<stdx::recursive_mutex> lk(_mutex);
    invariant(isPidRegistered(pid));
    invariant(_outputReaderThreads.count(pid) == 0);
    _outputReaderThreads.emplace(pid, std::move(reader));
}

void ProgramRegistry::updatePidExitCode(ProcessId pid, int exitCode) {
    stdx::lock_guard<stdx::recursive_mutex> lk(_mutex);
    _pidToExitCode[pid] = exitCode;
}

bool ProgramRegistry::waitForPid(const ProcessId pid, const bool block, int* const exit_code) {
    {
        // Be careful not to hold the lock while waiting for the pid to finish
        stdx::lock_guard<stdx::recursive_mutex> lk(_mutex);

        // unregistered pids are dead
        if (!this->isPidRegistered(pid)) {
            if (exit_code) {
                const auto code = _pidToExitCode.find(pid);
                if (code != _pidToExitCode.end()) {
                    *exit_code = code->second;
                } else {
                    // If you hit this invariant, you're waiting on a PID that was
                    // never a child of this process.
                    MONGO_UNREACHABLE;
                }
            }
            return true;
        }
    }
#ifdef _WIN32
    HANDLE h = getHandleForPid(pid);

    // wait until the process object is signaled before getting its
    // exit code. do this even when block is false to ensure that all
    // file handles open in the process have been closed.

    DWORD ret = WaitForSingleObject(h, (block ? INFINITE : 0));
    if (ret == WAIT_TIMEOUT) {
        return false;
    } else if (ret != WAIT_OBJECT_0) {
        auto ec = lastSystemError();
        LOGV2_INFO(22811,
                   "ProgramRegistry::waitForPid: WaitForSingleObject failed",
                   "error"_attr = errorMessage(ec));
    }

    DWORD tmp;
    if (GetExitCodeProcess(h, &tmp)) {
        if (tmp == STILL_ACTIVE) {
            uassert(
                ErrorCodes::UnknownError, "Process is STILL_ACTIVE even after blocking", !block);
            return false;
        }
        CloseHandle(h);
        eraseHandleForPid(pid);
        if (exit_code)
            *exit_code = tmp;
        updatePidExitCode(pid, tmp);

        unregisterProgram(pid);
        return true;
    } else {
        auto ec = lastSystemError();
        LOGV2_INFO(22812, "GetExitCodeProcess failed", "error"_attr = errorMessage(ec));
        return false;
    }
#else
    int status;
    int ret;
    do {
        errno = 0;
        ret = waitpid(pid.toNative(), &status, (block ? 0 : WNOHANG));
    } while (ret == -1 && errno == EINTR);
    if (ret) {
        // It's possible for waitpid to return -1 if the waidpid was already
        // run on the pid. We're not sure if this issue can actually happen
        // due to the locking/single threaded nature of JS, so we're adding
        // this invariant to trigger a failure if this ever happens.
        // See SERVER-63022.
        invariant(ret > 0);
        int code;
        if (WIFEXITED(status)) {
            code = WEXITSTATUS(status);
        } else if (WIFSIGNALED(status)) {
            code = WTERMSIG(status);
        } else {
            MONGO_UNREACHABLE;
        }
        updatePidExitCode(pid, code);
        if (exit_code) {
            *exit_code = code;
        }
    }
    if (ret) {
        unregisterProgram(pid);
    } else if (block) {
        uasserted(ErrorCodes::UnknownError, "Process did not exit after blocking");
    }
    return ret == pid.toNative();
#endif
}

bool ProgramRegistry::isPidDead(const ProcessId pid, int* const exit_code) {
    return this->waitForPid(pid, false, exit_code);
}

void ProgramRegistry::getRegisteredPorts(vector<int>& ports) {
    stdx::lock_guard<stdx::recursive_mutex> lk(_mutex);
    for (const auto& portPid : _portToPidMap) {
        ports.push_back(portPid.first);
    }
}

bool ProgramRegistry::isPidRegistered(ProcessId pid) const {
    stdx::lock_guard<stdx::recursive_mutex> lk(_mutex);
    return _registeredPids.count(pid) == 1;
}

void ProgramRegistry::getRegisteredPids(vector<ProcessId>& pids) {
    stdx::lock_guard<stdx::recursive_mutex> lk(_mutex);
    for (const auto& pid : _registeredPids) {
        pids.emplace_back(pid);
    }
}

#ifdef _WIN32
HANDLE ProgramRegistry::getHandleForPid(ProcessId pid) const {
    stdx::lock_guard<stdx::recursive_mutex> lk(_mutex);

    auto iter = _handles.find(pid);
    uassert(
        ErrorCodes::BadValue, "Unregistered pid {}"_format(pid.toNative()), iter != _handles.end());
    return iter->second;
}

void ProgramRegistry::eraseHandleForPid(ProcessId pid) {
    stdx::lock_guard<stdx::recursive_mutex> lk(_mutex);

    _handles.erase(pid);
}

void ProgramRegistry::insertHandleForPid(ProcessId pid, HANDLE handle) {
    stdx::lock_guard<stdx::recursive_mutex> lk(_mutex);

    _handles.insert(make_pair(pid, handle));
}

#endif

void ProgramOutputMultiplexer::appendLine(
    int port, ProcessId pid, const std::string& name, const std::string& line, bool shouldLog) {
    stdx::lock_guard<Latch> lk(_mongoProgramOutputMutex);
    auto sinkProgramOutput = [&](auto& sink) {
        if (port > 0) {
            sink << name << port << "| " << line << endl;
        } else {
            sink << name << pid << "| " << line << endl;
        }
    };

    sinkProgramOutput(_buffer);

    if (shouldLog) {
        std::ostringstream ss;
        sinkProgramOutput(ss);
        LOGV2_INFO_OPTIONS(4615640,
                           logv2::LogOptions(logv2::LogTag::kPlainShell |
                                                 logv2::LogTag::kAllowDuringPromptingShell,
                                             logv2::LogTruncation::Disabled),
                           "{message}",
                           "message"_attr = ss.str());
    }
}

string ProgramOutputMultiplexer::str() const {
    stdx::lock_guard<Latch> lk(_mongoProgramOutputMutex);
    return _buffer.str();
}

void ProgramOutputMultiplexer::clear() {
    stdx::lock_guard<Latch> lk(_mongoProgramOutputMutex);
    _buffer.str("");
}

ProgramRunner::ProgramRunner(BSONObj args, BSONObj env, bool isMongo, ProgramRegistry* registry)
    : _parentRegistry(registry) {
    uassert(ErrorCodes::FailedToParse,
            "cannot pass an empty argument to ProgramRunner",
            !args.isEmpty());

    string program(args.firstElement().str());
    uassert(ErrorCodes::FailedToParse,
            "invalid program name passed to ProgramRunner",
            !program.empty());
    boost::filesystem::path programPath = findProgram(program);
    boost::filesystem::path programName = programPath.stem();

    _pipe = -1;
    _port = -1;

    string prefix("mongod-");
    bool isMongodProgram = isMongo &&
        (string("mongod") == programName ||
         programName.string().compare(0, prefix.size(), prefix) == 0);
    prefix = "mongos-";
    bool isMongosProgram = isMongo &&
        (string("mongos") == programName ||
         programName.string().compare(0, prefix.size(), prefix) == 0);
    prefix = "mongoqd-";
    bool isMongoqProgram = isMongo &&
        (string("mongoqd") == programName ||
         programName.string().compare(0, prefix.size(), prefix) == 0);

    parseName(isMongo, isMongodProgram, isMongosProgram, isMongoqProgram, programName);

    _argv.push_back(programPath.string());

    parseArgs(args, isMongo, isMongodProgram);
    loadEnvironmentVariables(env);

    bool needsPort = isMongo &&
        (isMongodProgram || isMongosProgram || isMongoqProgram || (programName == "mongobridge"));
    if (!needsPort) {
        _port = -1;
    }

    uassert(ErrorCodes::FailedToParse,
            str::stream() << "a port number is expected when running " << program
                          << " from the shell",
            !needsPort || _port >= 0);

    uassert(ErrorCodes::BadValue,
            str::stream() << "can't start " << program << ", port " << _port << " already in use",
            _port < 0 || !_parentRegistry->isPortRegistered(_port));
}

void ProgramRunner::start() {
    int pipeEnds[2];

    {
        // NOTE(JCAREY):
        //
        // We take this lock from before our call to pipe until after we close the write side (in
        // the parent) to avoid leaking fds from threads racing around fork().  I.e.
        //
        // Thread A: calls pipe()
        // Thread B: calls fork()
        // A: sets cloexec on read and write sides
        // B: has a forked child with open fds
        // A: spawns a child thread to read it's child process's stdout
        // A: A's child process exits
        // A: wait's on A's reader thread in de-register
        // A: deadlocks forever (because the child reader thread stays in read() because of the open
        //    fd in B)
        //
        // Holding the lock for the duration of those events prevents the leaks and thus the
        // associated deadlocks.
        stdx::lock_guard<Latch> lk(_parentRegistry->_createProcessMtx);
        if (pipe(pipeEnds)) {
            auto ec = lastPosixError();
            LOGV2_ERROR(22830, "Failed to create pipe", "error"_attr = errorMessage(ec));
            fassertFailed(16701);
        }
#ifndef _WIN32
        // The calls to fcntl to set CLOEXEC ensure that processes started by the process we are
        // about to fork do *not* inherit the file descriptors for the pipe. If grandchild processes
        // could inherit the FD for the pipe, than the pipe wouldn't close on child process exit. On
        // windows, instead the handle inherit flag is turned off after the call to CreateProcess.
        if (fcntl(pipeEnds[0], F_SETFD, FD_CLOEXEC)) {
            auto ec = lastPosixError();
            LOGV2_ERROR(
                22831, "Failed to set FD_CLOEXEC on pipe end 0", "error"_attr = errorMessage(ec));
            fassertFailed(40308);
        }
        if (fcntl(pipeEnds[1], F_SETFD, FD_CLOEXEC)) {
            auto ec = lastPosixError();
            LOGV2_ERROR(
                22832, "Failed to set FD_CLOEXEC on pipe end 1", "error"_attr = errorMessage(ec));
            fassertFailed(40317);
        }
#endif

        fflush(nullptr);

        launchProcess(pipeEnds[1]);  // sets _pid

        // Close the write end of the pipe.
        safeClose(pipeEnds[1]);
    }

    if (_port >= 0) {
        _parentRegistry->registerProgram(_pid, _port);
    } else {
        _parentRegistry->registerProgram(_pid);
    }

    _pipe = pipeEnds[0];

    LOGV2_INFO(22810,
               "shell: Started program",
               "pid"_attr = _pid,
               "port"_attr = _port,
               "argv"_attr = _argv);
}

void ProgramRunner::operator()(ProgramOutputMultiplexer* multiplexer, bool shouldLogOutput) {
    invariant(_pipe >= 0);
    // Send the never_close_handle flag so that we can handle closing the fd below with safeClose.
    boost::iostreams::stream_buffer<boost::iostreams::file_descriptor_source> fdBuf(
        _pipe, boost::iostreams::file_descriptor_flags::never_close_handle);
    std::istream fdStream(&fdBuf);

    std::string line;
    while (std::getline(fdStream, line)) {
        if (line.find('\0') != std::string::npos) {
            multiplexer->appendLine(
                _port, _pid, _name, "WARNING: mongod wrote null bytes to output", shouldLogOutput);
        }
        multiplexer->appendLine(_port, _pid, _name, line, shouldLogOutput);
    }

    // Close the read end of the pipe.
    safeClose(_pipe);
}

boost::filesystem::path ProgramRunner::findProgram(const string& prog) {
    boost::filesystem::path p = prog;

#ifdef _WIN32
    // The system programs either come versioned in the form of <utility>-<major.minor>
    // (e.g., mongorestore-2.4) or just <utility>. For windows, the appropriate extension
    // needs to be appended.
    //

    auto isExtensionValid = [](std::string e) {
        return std::all_of(e.begin(), e.end(), [](char c) { return !ctype::isDigit(c); });
    };

    if (!p.has_extension() || !isExtensionValid(p.extension().string())) {
        p = prog + ".exe";
    }
#endif

    // The file could exist if it is specified as a full path.
    if (p.is_absolute() && boost::filesystem::exists(p)) {
        return p;
    }

    // Check if the binary exists in the current working directory
    boost::filesystem::path t = boost::filesystem::current_path() / p;
    if (boost::filesystem::exists(t)) {
        return t;
    }

#ifndef _WIN32
    // On POSIX, we need to manually resolve the $PATH variable, to try and find the binary in the
    // filesystem.
    const char* cpath = getenv("PATH");
    if (!cpath) {
        // PATH was unset, so path search is implementation defined
        return t;
    }

    std::string path(cpath);
    std::vector<std::string> pathEntries;

    // PATH entries are separated by colons. Per POSIX 2013, there is no way to escape a colon in
    // an entry.
    str::splitStringDelim(path, &pathEntries, ':');

    for (const std::string& pathEntry : pathEntries) {
        boost::filesystem::path potentialBinary = boost::filesystem::path(pathEntry) / p;
        if (boost::filesystem::exists(potentialBinary) &&
            boost::filesystem::is_regular_file(potentialBinary) &&
            access(potentialBinary.c_str(), X_OK) == 0) {
            return potentialBinary;
        }
    }
#endif

    return p;
}

void ProgramRunner::launchProcess(int child_stdout) {
    std::vector<std::string> envStrings;
    for (const auto& envKeyValue : _envp) {
        envStrings.emplace_back(envKeyValue.first + '=' + envKeyValue.second);
    }

#ifdef _WIN32
    stringstream ss;
    for (unsigned i = 0; i < _argv.size(); i++) {
        if (i)
            ss << ' ';
        if (_argv[i].find(' ') == string::npos)
            ss << _argv[i];
        else {
            ss << '"';
            // Escape all embedded quotes and backslashes.
            for (size_t j = 0; j < _argv[i].size(); ++j) {
                if (_argv[i][j] == '"' || _argv[i][j] == '\\')
                    ss << '\\';
                ss << _argv[i][j];
            }
            ss << '"';
        }
    }

    std::wstring args = toNativeString(ss.str().c_str());

    // Construct the environment block which the new process will use.
    // An environment block is a NULL terminated array of NULL terminated WCHAR strings. The
    // strings are of the form "name=value\0". Because the strings are variable length, we must
    // precompute the size of the array before we may allocate it.
    size_t environmentBlockSize = 0;
    std::vector<std::wstring> nativeEnvStrings;

    // Compute the size of the environment block, in characters. Note that we have to count
    // wchar_t characters, which we'll actually be storing in the block later, rather than UTF8
    // characters we have in _envp and need to convert.
    for (const std::string& envKeyValue : envStrings) {
        std::wstring nativeKeyValue = toNativeString(envKeyValue.c_str());
        environmentBlockSize += (nativeKeyValue.size() + 1);
        nativeEnvStrings.emplace_back(std::move(nativeKeyValue));
    }

    // Reserve space for the final NULL character which terminates the environment block
    environmentBlockSize += 1;

    auto lpEnvironment = std::make_unique<wchar_t[]>(environmentBlockSize);
    size_t environmentOffset = 0;
    for (const std::wstring& envKeyValue : nativeEnvStrings) {
        // Ensure there is enough room to write the string, the string's NULL byte, and the block's
        // NULL byte
        uassert(7095702,
                str::stream() << "Insufficient space to set environment variable "
                              << toUtf8String(envKeyValue),
                environmentOffset + envKeyValue.size() + 1 + 1 <= environmentBlockSize);
        wcscpy_s(
            lpEnvironment.get() + environmentOffset, envKeyValue.size() + 1, envKeyValue.c_str());
        environmentOffset += envKeyValue.size();
        std::memset(lpEnvironment.get() + environmentOffset, 0, sizeof(wchar_t));
        environmentOffset += 1;
    }
    std::memset(lpEnvironment.get() + environmentOffset, 0, sizeof(wchar_t));

    HANDLE h = reinterpret_cast<HANDLE>(_get_osfhandle(child_stdout));
    uassert(7095703,
            str::stream() << "Handle could not be set",
            h != INVALID_HANDLE_VALUE && SetHandleInformation(h, HANDLE_FLAG_INHERIT, 1));

    STARTUPINFO si;
    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    si.hStdError = h;
    si.hStdOutput = h;
    si.dwFlags |= STARTF_USESTDHANDLES;

    PROCESS_INFORMATION pi;
    ZeroMemory(&pi, sizeof(pi));

    DWORD dwCreationFlags = 0;
    dwCreationFlags |= CREATE_UNICODE_ENVIRONMENT;

    bool success = CreateProcessW(nullptr,
                                  const_cast<LPWSTR>(args.c_str()),
                                  nullptr,
                                  nullptr,
                                  true,
                                  dwCreationFlags,
                                  lpEnvironment.get(),
                                  nullptr,
                                  &si,
                                  &pi) != 0;
    if (!success) {
        const auto ec = lastSystemError();
        ss << "couldn't start process " << _argv[0] << "; " << errorMessage(ec);
        uasserted(14042, ss.str());
    }

    CloseHandle(pi.hThread);
    uassert(7095704,
            str::stream() << "Handle could not be unset",
            SetHandleInformation(h, HANDLE_FLAG_INHERIT, 0));

    _pid = ProcessId::fromNative(pi.dwProcessId);
    _parentRegistry->insertHandleForPid(_pid, pi.hProcess);
#else

    std::string execErrMsg = str::stream() << "Unable to start program " << _argv[0];
    auto constCharStorageMaker = [](const std::vector<std::string>& in) {
        std::vector<const char*> out;
        std::transform(in.begin(), in.end(), std::back_inserter(out), [](const std::string& x) {
            return x.c_str();
        });
        out.push_back(nullptr);
        return out;
    };

    std::vector<const char*> argvStorage = constCharStorageMaker(_argv);
    std::vector<const char*> envpStorage = constCharStorageMaker(envStrings);

    pid_t nativePid = fork();
    _pid = ProcessId::fromNative(nativePid);
    // Async signal unsafe functions should not be called in the child process.

    if (nativePid == -1) {
        // Fork failed so it is time for the process to exit
        const auto ec = lastPosixError();
        cout << "ProgramRunner is unable to fork child process: " << errorMessage(ec) << endl;
        fassertFailed(34363);
    }

    if (nativePid == 0) {
        // DON'T ASSERT IN THIS BLOCK - very bad things will happen
        //
        // Also, deliberately call _exit instead of quickExit. We intended to
        // fork() and exec() here, so we never want to run any form of cleanup.
        // This includes things that quickExit calls, such as atexit leak
        // checks.

        if (dup2(child_stdout, STDOUT_FILENO) == -1 || dup2(child_stdout, STDERR_FILENO) == -1) {
            // Async signal unsafe code reporting a terminal error condition.
            perror("Unable to dup2 child output: ");
            _exit(static_cast<int>(ExitCode::fail));  // do not pass go, do not call atexit handlers
        }

        execve(argvStorage[0],
               const_cast<char**>(argvStorage.data()),
               const_cast<char**>(envpStorage.data()));

        // Async signal unsafe code reporting a terminal error condition.
        perror(execErrMsg.c_str());

        _exit(static_cast<int>(ExitCode::fail));
    }

#endif
}

void ProgramRunner::loadEnvironmentVariables(BSONObj env) {
    // Load explicitly set environment key value pairs into _envp.
    for (const BSONElement& e : env) {
        uassert(ErrorCodes::FailedToParse,
                "Environment variable values must be strings",
                e.type() == mongo::String);

        _envp.emplace(std::string(e.fieldName()), e.str());
    }

// Import this process' environment into _envp, for all keys that have not already been set.
// We need to do this so that the child process has all the PATH and locale variables, unless
// we explicitly override them.
#ifdef _WIN32
    wchar_t* processEnv = GetEnvironmentStringsW();
    ON_BLOCK_EXIT([processEnv] {
        if (processEnv)
            FreeEnvironmentStringsW(processEnv);
    });

    // Windows' GetEnvironmentStringsW returns a NULL terminated array of NULL separated
    // <key>=<value> pairs.
    while (processEnv && *processEnv) {
        std::wstring envKeyValue(processEnv);
        size_t splitPoint = envKeyValue.find('=');
        uassert(ErrorCodes::BadValue,
                str::stream() << "Environment key " << toUtf8String(envKeyValue)
                              << " does not have '=' separator",
                splitPoint != std::wstring::npos);
        std::string envKey = toUtf8String(envKeyValue.substr(0, splitPoint));
        std::string envValue = toUtf8String(envKeyValue.substr(splitPoint + 1));
        _envp.emplace(std::move(envKey), std::move(envValue));
        processEnv += envKeyValue.size() + 1;
    }
#else
    // environ is a POSIX defined array of char*s. Each char* in the array is a <key>=<value>\0
    // pair.
    char** environEntry = getEnvironPointer();
    while (*environEntry) {
        std::string envKeyValue(*environEntry);
        size_t splitPoint = envKeyValue.find('=');
        uassert(ErrorCodes::BadValue,
                str::stream() << "Environment key " << envKeyValue
                              << " does not have '=' separator",
                splitPoint != std::wstring::npos);
        std::string envKey = envKeyValue.substr(0, splitPoint);
        std::string envValue = envKeyValue.substr(splitPoint + 1);
        _envp.emplace(std::move(envKey), std::move(envValue));
        ++environEntry;
    }
#endif
}

void ProgramRunner::parseName(bool isMongo,
                              bool isMongodProgram,
                              bool isMongosProgram,
                              bool isMongoqProgram,
                              const boost::filesystem::path& programName) {
    if (!isMongo) {
        _name = "sh";
    } else if (isMongodProgram) {
        _name = "d";
    } else if (isMongosProgram) {
        _name = "s";
    } else if (isMongoqProgram) {
        _name = "q";
    } else if (programName == "mongobridge") {
        _name = "b";
    } else {
        _name = "sh";
    }
}

void ProgramRunner::parseArgs(BSONObj args, bool isMongo, bool isMongodProgram) {
    // Parse individual arguments into _argv
    BSONObjIterator j(args);
    j.next();  // skip program name (handled above)

    while (j.more()) {
        BSONElement e = j.next();
        string str;
        if (e.isNumber()) {
            stringstream ss;
            ss << e.number();
            str = ss.str();
        } else {
            uassert(ErrorCodes::FailedToParse,
                    "Program arguments must be strings",
                    e.type() == mongo::String);
            str = e.str();
        }
        if (isMongo) {
            if (str == "--port") {
                _port = kParsingPortNumber;
            } else if (_port == kParsingPortNumber) {
                if (!NumberParser::strToAny(10)(str, &_port).isOK())
                    _port = 0;  // same behavior as strtol
            } else if (isMongodProgram && str == "--configsvr") {
                _name = "c";
            }
        }
        _argv.push_back(str);
    }
}

}  // namespace mongo::shell_utils
