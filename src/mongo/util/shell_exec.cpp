// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/util/shell_exec.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/bson/util/builder_fwd.h"
#include "mongo/util/assert_util.h"

#include <algorithm>
#include <string_view>
#include <system_error>

#include <boost/move/utility_core.hpp>

#ifdef _WIN32
#include <processthreadsapi.h>
#include <synchapi.h>
#else
#include <cstdio>

#include <poll.h>
#endif

#include "mongo/util/errno_util.h"
#include "mongo/util/str.h"
#include "mongo/util/text.h"  // IWYU pragma: keep
#include "mongo/util/time_support.h"

namespace mongo {
namespace {
constexpr size_t kExecBufferSizeBytes = 1024;

#ifdef _WIN32
class ProcessStream {
public:
    ProcessStream(const std::string& cmd) {
        ZeroMemory(&_startup, sizeof(_startup));
        ZeroMemory(&_process, sizeof(_process));
        _startup.cb = sizeof(_startup);
        _startup.dwFlags = STARTF_USESTDHANDLES;

        SECURITY_ATTRIBUTES sa;
        ZeroMemory(&sa, sizeof(sa));
        sa.nLength = sizeof(sa);
        sa.lpSecurityDescriptor = nullptr;
        sa.bInheritHandle = true;

        // Close our end of stdin immediately to signal child we have no data.
        HANDLE dummy = nullptr;
        if (!CreatePipe(&dummy, &_startup.hStdInput, &sa, kExecBufferSizeBytes)) {
            auto ec = lastSystemError();
            uasserted(ErrorCodes::OperationFailed,
                      str::stream()
                          << "Unable to create stdin pipe for subprocess: " << errorMessage(ec));
        }
        CloseHandle(dummy);

        if (!CreatePipe(&_stdout, &_startup.hStdOutput, &sa, kExecBufferSizeBytes)) {
            auto ec = lastSystemError();
            uasserted(ErrorCodes::OperationFailed,
                      str::stream()
                          << "Unable to create stdout pipe for subprocess: " << errorMessage(ec));
        }

        if (!CreatePipe(&_stderr, &_startup.hStdError, &sa, kExecBufferSizeBytes)) {
            auto ec = lastSystemError();
            uasserted(ErrorCodes::OperationFailed,
                      str::stream()
                          << "Unable to create stderr pipe for subprocess: " << errorMessage(ec));
        }

        DWORD mode = PIPE_NOWAIT;
        if (!SetNamedPipeHandleState(_stdout, &mode, nullptr, nullptr)) {
            auto ec = lastSystemError();
            uasserted(ErrorCodes::OperationFailed,
                      str::stream()
                          << "Unable to set non-blocking for subprocess: " << errorMessage(ec));
        }

        auto wideCmd = toWideString(cmd.c_str());
        if (!CreateProcessW(nullptr,
                            const_cast<wchar_t*>(wideCmd.c_str()),
                            &sa,
                            &sa,
                            true,
                            CREATE_NO_WINDOW,
                            nullptr,
                            nullptr,
                            &_startup,
                            &_process)) {
            auto ec = lastSystemError();
            uasserted(ErrorCodes::OperationFailed,
                      str::stream() << "Unable to launch command: " << errorMessage(ec));
        }
    }

    int close() {
        if (_exitcode != STILL_ACTIVE) {
            return _exitcode;
        }

        if (!GetExitCodeProcess(_process.hProcess, &_exitcode)) {
            auto ec = lastSystemError();
            uasserted(ErrorCodes::OperationFailed,
                      str::stream()
                          << "Failed retreiving exit code from subprocess: " << errorMessage(ec));
        }

        if (_exitcode == STILL_ACTIVE) {
            if (!TerminateProcess(_process.hProcess, 1)) {
                auto ec = lastSystemError();
                uasserted(ErrorCodes::OperationFailed,
                          str::stream() << "Failed terminating subprocess: " << errorMessage(ec));
            }
            _exitcode = 1;
        }

        return _exitcode;
    }

    bool eof() {
        if (_exitcode != STILL_ACTIVE) {
            return true;
        }

        if (!GetExitCodeProcess(_process.hProcess, &_exitcode)) {
            auto ec = lastSystemError();
            uasserted(ErrorCodes::OperationFailed,
                      str::stream()
                          << "Failed retreiving status of subprocess: " << errorMessage(ec));
        }

        return _exitcode != STILL_ACTIVE;
    }

    Status wait(Milliseconds duration) {
        auto ret = WaitForSingleObject(_process.hProcess, durationCount<Milliseconds>(duration));
        if (ret == WAIT_OBJECT_0) {
            return Status::OK();
        } else if (ret == WAIT_TIMEOUT) {
            return {ErrorCodes::OperationFailed, "Timeout expired"};
        } else {
            return {ErrorCodes::OperationFailed, errorMessage(lastSystemError())};
        }
    }

    void read(StringBuilder& sb, size_t len) {
        constexpr DWORD kPipeReadyTimeoutMS = 10;
        if (!_stdout || (WAIT_OBJECT_0 != WaitForSingleObject(_stdout, kPipeReadyTimeoutMS))) {
            return;
        }

        char buf[kExecBufferSizeBytes];
        DWORD read = 0;
        if (!ReadFile(_stdout, buf, std::min<size_t>(sizeof(buf), len), &read, nullptr)) {
            auto ec = lastSystemError();
            uasserted(ErrorCodes::OperationFailed,
                      str::stream() << "Failed reading from subprocess: " << errorMessage(ec));
        }

        if (read == 0) {
            CloseHandle(_stdout);
            _stdout = nullptr;
        } else {
            sb << std::string_view(buf, read);
        }
    }

    ~ProcessStream() {
        if (_startup.hStdInput) {
            CloseHandle(_startup.hStdInput);
        }
        if (_startup.hStdOutput) {
            CloseHandle(_startup.hStdOutput);
        }
        if (_startup.hStdError) {
            CloseHandle(_startup.hStdError);
        }
        if (_stdout) {
            CloseHandle(_stdout);
        }
        if (_stderr) {
            CloseHandle(_stderr);
        }
        if (_process.hProcess) {
            CloseHandle(_process.hProcess);
        }
        if (_process.hThread) {
            CloseHandle(_process.hThread);
        }
    }

private:
    ProcessStream() = delete;
    ProcessStream(const ProcessStream&) = delete;
    ProcessStream& operator=(const ProcessStream&) = delete;

    HANDLE _stdout, _stderr;
    STARTUPINFO _startup;
    PROCESS_INFORMATION _process;
    DWORD _exitcode = STILL_ACTIVE;
};
#elif !defined(__wasi__)
class ProcessStream {
public:
    ProcessStream(const std::string& cmd) {
        _fp = ::popen(cmd.c_str(), "r");
        if (!_fp) {
            auto ec = lastSystemError();
            uasserted(ErrorCodes::OperationFailed,
                      str::stream() << "Unable to launch command: " << errorMessage(ec));
        }
        _fd = fileno(_fp);
    }

    int close() {
        if (!_fp) {
            return _exitcode;
        }
        _exitcode = ::pclose(_fp);
        _fp = nullptr;
        return _exitcode;
    }

    bool eof() {
        return feof(_fp);
    }

    Status wait(Milliseconds duration) {
        struct pollfd fds;
        fds.fd = _fd;
        fds.events = POLLIN | POLLHUP;
        fds.revents = 0;

        auto ret = poll(&fds, 1, durationCount<Milliseconds>(duration));
        if (ret < 0) {
            return {ErrorCodes::OperationFailed, errorMessage(lastSystemError())};
        } else if (ret == 0) {
            return {ErrorCodes::OperationFailed, "Timeout expired"};
        } else {
            return Status::OK();
        }
    }

    void read(StringBuilder& sb, size_t len) {
        char buf[kExecBufferSizeBytes];
        len = fread(buf, 1, std::min<size_t>(sizeof(buf), len), _fp);
        sb << std::string_view(buf, len);
    }

    ~ProcessStream() {
        if (_fp) {
            ::pclose(_fp);
        }
    }

private:
    ProcessStream() = delete;
    ProcessStream(const ProcessStream&) = delete;
    ProcessStream& operator=(const ProcessStream&) = delete;

    FILE* _fp;
    int _fd;
    int _exitcode = 1;
};
#else
// WASI doesn't support process execution
// See: https://github.com/WebAssembly/WASI/issues/414
class ProcessStream {
public:
    ProcessStream(const std::string& cmd) {
        uasserted(ErrorCodes::OperationFailed,
                  str::stream() << "Shell execution not supported in WASI environment: " << cmd);
    }

    int close() {
        return 1;
    }

    bool eof() {
        return true;
    }

    Status wait(Milliseconds duration) {
        (void)duration;
        return {ErrorCodes::OperationFailed, "Shell execution not supported in WASI"};
    }

    void read(StringBuilder& sb, size_t len) {
        (void)sb;
        (void)len;
    }

    ~ProcessStream() = default;

private:
    ProcessStream() = delete;
    ProcessStream(const ProcessStream&) = delete;
    ProcessStream& operator=(const ProcessStream&) = delete;
};
#endif
}  // namespace
}  // namespace mongo
mongo::StatusWith<std::string> mongo::shellExec(const std::string& cmd,
                                                Milliseconds timeout,
                                                size_t maxlen,
                                                bool ignoreExitCode) try {
    if (durationCount<Milliseconds>(timeout) <= 0) {
        return {ErrorCodes::OperationFailed, str::stream() << "Invalid timeout: " << timeout};
    }
    auto end = Date_t::now() + timeout;

    ProcessStream process(cmd);
    StringBuilder sb;
    while (!process.eof()) {
        auto status = process.wait(end - Date_t::now());
        if (!status.isOK()) {
            return status;
        }

        process.read(sb, maxlen - sb.len());
        if (static_cast<size_t>(sb.len()) >= maxlen) {
            // Truncate at maxlen
            break;
        }
    }

    auto exitcode = process.close();
    if (!ignoreExitCode && exitcode) {
        return {ErrorCodes::OperationFailed,
                str::stream() << "Process returned non-zero exit code: " << exitcode};
    }

    return sb.str();
} catch (...) {
    return mongo::exceptionToStatus();
}
