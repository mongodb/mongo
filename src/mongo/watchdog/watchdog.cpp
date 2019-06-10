/**
 *    Copyright (C) 2019-present MongoDB, Inc.
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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kControl

#include "mongo/platform/basic.h"

#include "mongo/watchdog/watchdog.h"

#include <boost/filesystem.hpp>

#ifndef _WIN32
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

#include "mongo/base/static_assert.h"
#include "mongo/db/client.h"
#include "mongo/db/operation_context.h"
#include "mongo/platform/process_id.h"
#include "mongo/util/concurrency/idle_thread_block.h"
#include "mongo/util/hex.h"
#include "mongo/util/log.h"
#include "mongo/util/timer.h"


namespace mongo {

WatchdogPeriodicThread::WatchdogPeriodicThread(Milliseconds period, StringData threadName)
    : _period(period), _enabled(true), _threadName(threadName.toString()) {}

void WatchdogPeriodicThread::start() {
    {
        stdx::lock_guard<stdx::mutex> lock(_mutex);

        invariant(_state == State::kNotStarted);
        _state = State::kStarted;

        // Start the thread.
        _thread = stdx::thread([this] { this->doLoop(); });
    }
}

void WatchdogPeriodicThread::shutdown() {

    stdx::thread thread;

    {
        stdx::lock_guard<stdx::mutex> lock(_mutex);

        bool started = (_state == State::kStarted);

        invariant(_state == State::kNotStarted || _state == State::kStarted);

        if (!started) {
            _state = State::kDone;
            return;
        }

        _state = State::kShutdownRequested;

        std::swap(thread, _thread);

        // Wake up the thread if sleeping so that it will check if we are done.
        _condvar.notify_one();
    }

    thread.join();

    _state = State::kDone;
}

void WatchdogPeriodicThread::setPeriod(Milliseconds period) {
    stdx::lock_guard<stdx::mutex> lock(_mutex);

    bool wasEnabled = _enabled;

    if (period < Milliseconds::zero()) {
        _enabled = false;

        // Leave the thread running but very slowly. If we set this value too high, it would
        // overflow Duration.
        _period = Hours(1);
    } else {
        _period = period;
        _enabled = true;
    }

    if (!wasEnabled && _enabled) {
        resetState();
    }

    _condvar.notify_one();
}

void WatchdogPeriodicThread::doLoop() {
    Client::initThread(_threadName);
    Client* client = &cc();

    auto preciseClockSource = client->getServiceContext()->getPreciseClockSource();

    {
        stdx::lock_guard<stdx::mutex> lock(_mutex);

        // Ensure state is starting from a clean slate.
        resetState();
    }

    while (true) {
        // Wait for the next run or signal to shutdown.

        auto opCtx = client->makeOperationContext();

        Date_t startTime = preciseClockSource->now();

        {
            stdx::unique_lock<stdx::mutex> lock(_mutex);
            MONGO_IDLE_THREAD_BLOCK;


            // Check if the period is different?
            // We are signalled on period changes at which point we may be done waiting or need to
            // wait longer.
            while (startTime + _period > preciseClockSource->now() &&
                   _state != State::kShutdownRequested) {
                auto s = opCtx->waitForConditionOrInterruptNoAssertUntil(
                    _condvar, lock, startTime + _period);

                if (!s.isOK()) {
                    // The only bad status is when we are in shutdown
                    if (!opCtx->getServiceContext()->getKillAllOperations()) {
                        error() << "Watchdog was interrupted, shuting down:, reason: "
                                << s.getStatus();
                    }

                    return;
                }
            }

            // Are we done running?
            if (_state == State::kShutdownRequested) {
                return;
            }

            // Check if the watchdog checks have been disabled
            if (!_enabled) {
                continue;
            }
        }

        run(opCtx.get());
    }
}


WatchdogCheckThread::WatchdogCheckThread(std::vector<std::unique_ptr<WatchdogCheck>> checks,
                                         Milliseconds period)
    : WatchdogPeriodicThread(period, "watchdogCheck"), _checks(std::move(checks)) {}

std::int64_t WatchdogCheckThread::getGeneration() {
    return _checkGeneration.load();
}

void WatchdogCheckThread::resetState() {}

void WatchdogCheckThread::run(OperationContext* opCtx) {
    for (auto& check : _checks) {
        Timer timer(opCtx->getServiceContext()->getTickSource());

        check->run(opCtx);
        Microseconds micros = timer.elapsed();

        LOG(1) << "Watchdog test '" << check->getDescriptionForLogging() << "' took "
               << duration_cast<Milliseconds>(micros);

        // We completed a check, bump the generation counter.
        _checkGeneration.fetchAndAdd(1);
    }
}


WatchdogMonitorThread::WatchdogMonitorThread(WatchdogCheckThread* checkThread,
                                             WatchdogDeathCallback callback,
                                             Milliseconds interval)
    : WatchdogPeriodicThread(interval, "watchdogMonitor"),
      _callback(callback),
      _checkThread(checkThread) {}

std::int64_t WatchdogMonitorThread::getGeneration() {
    return _monitorGeneration.load();
}

void WatchdogMonitorThread::resetState() {
    // Reset the generation so that if the monitor thread is run before the check thread
    // after being enabled, it does not.
    _lastSeenGeneration = -1;
}

void WatchdogMonitorThread::run(OperationContext* opCtx) {
    auto currentGeneration = _checkThread->getGeneration();

    if (currentGeneration != _lastSeenGeneration) {
        _lastSeenGeneration = currentGeneration;
    } else {
        _callback();
    }
}


WatchdogMonitor::WatchdogMonitor(std::vector<std::unique_ptr<WatchdogCheck>> checks,
                                 Milliseconds checkPeriod,
                                 Milliseconds monitorPeriod,
                                 WatchdogDeathCallback callback)
    : _checkPeriod(checkPeriod),
      _watchdogCheckThread(std::move(checks), checkPeriod),
      _watchdogMonitorThread(&_watchdogCheckThread, callback, monitorPeriod) {
    invariant(checkPeriod < monitorPeriod);
}

void WatchdogMonitor::start() {
    log() << "Starting Watchdog Monitor";

    // Start the threads.
    _watchdogCheckThread.start();

    _watchdogMonitorThread.start();

    {
        stdx::lock_guard<stdx::mutex> lock(_mutex);

        invariant(_state == State::kNotStarted);
        _state = State::kStarted;
    }
}

void WatchdogMonitor::setPeriod(Milliseconds duration) {
    {
        stdx::lock_guard<stdx::mutex> lock(_mutex);

        if (duration > Milliseconds(0)) {
            dassert(duration >= Milliseconds(1));

            // Make sure that we monitor runs more frequently then checks
            // 2 feels like an arbitrary good minimum.
            invariant(duration >= 2 * _checkPeriod);

            _watchdogCheckThread.setPeriod(_checkPeriod);
            _watchdogMonitorThread.setPeriod(duration);

            log() << "WatchdogMonitor period changed to " << duration_cast<Seconds>(duration);
        } else {
            _watchdogMonitorThread.setPeriod(duration);
            _watchdogCheckThread.setPeriod(duration);

            log() << "WatchdogMonitor disabled";
        }
    }
}

void WatchdogMonitor::shutdown() {
    {
        stdx::lock_guard<stdx::mutex> lock(_mutex);

        bool started = (_state == State::kStarted);

        invariant(_state == State::kNotStarted || _state == State::kStarted);

        if (!started) {
            _state = State::kDone;
            return;
        }

        _state = State::kShutdownRequested;
    }

    _watchdogMonitorThread.shutdown();

    _watchdogCheckThread.shutdown();

    _state = State::kDone;
}

std::int64_t WatchdogMonitor::getCheckGeneration() {
    return _watchdogCheckThread.getGeneration();
}

std::int64_t WatchdogMonitor::getMonitorGeneration() {
    return _watchdogMonitorThread.getGeneration();
}

#ifdef _WIN32
/**
 * Check a directory is ok
 * 1. Open up a direct_io to a new file
 * 2. Write to the file
 * 3. Seek to the beginning
 * 4. Read from the file
 * 5. Close file
 */
void checkFile(OperationContext* opCtx, const boost::filesystem::path& file) {
    Date_t now = opCtx->getServiceContext()->getPreciseClockSource()->now();
    std::string nowStr = now.toString();

    HANDLE hFile = CreateFileW(file.generic_wstring().c_str(),
                               GENERIC_READ | GENERIC_WRITE,
                               0,  // No Sharing
                               NULL,
                               CREATE_ALWAYS,
                               FILE_ATTRIBUTE_NORMAL,
                               NULL);
    if (hFile == INVALID_HANDLE_VALUE) {
        std::uint32_t gle = ::GetLastError();
        severe() << "CreateFile failed for '" << file.generic_string()
                 << "' with error: " << errnoWithDescription(gle);
        fassertNoTrace(4074, gle == 0);
    }

    DWORD bytesWrittenTotal;
    if (!WriteFile(hFile, nowStr.c_str(), nowStr.size(), &bytesWrittenTotal, NULL)) {
        std::uint32_t gle = ::GetLastError();
        severe() << "WriteFile failed for '" << file.generic_string()
                 << "' with error: " << errnoWithDescription(gle);
        fassertNoTrace(4075, gle == 0);
    }

    if (bytesWrittenTotal != nowStr.size()) {
        warning() << "partial write for '" << file.generic_string() << "' expected "
                  << nowStr.size() << " bytes but wrote " << bytesWrittenTotal << " bytes";
    } else {

        if (!FlushFileBuffers(hFile)) {
            std::uint32_t gle = ::GetLastError();
            severe() << "FlushFileBuffers failed for '" << file.generic_string()
                     << "' with error: " << errnoWithDescription(gle);
            fassertNoTrace(4076, gle == 0);
        }

        DWORD newOffset = SetFilePointer(hFile, 0, 0, FILE_BEGIN);
        if (newOffset != 0) {
            std::uint32_t gle = ::GetLastError();
            severe() << "SetFilePointer failed for '" << file.generic_string()
                     << "' with error: " << errnoWithDescription(gle);
            fassertNoTrace(4077, gle == 0);
        }

        DWORD bytesRead;
        auto readBuffer = std::make_unique<char[]>(nowStr.size());
        if (!ReadFile(hFile, readBuffer.get(), nowStr.size(), &bytesRead, NULL)) {
            std::uint32_t gle = ::GetLastError();
            severe() << "ReadFile failed for '" << file.generic_string()
                     << "' with error: " << errnoWithDescription(gle);
            fassertNoTrace(4078, gle == 0);
        }

        if (bytesRead != bytesWrittenTotal) {
            severe() << "Read wrong number of bytes for '" << file.generic_string() << "' expected "
                     << bytesWrittenTotal << " bytes but read " << bytesRead << " bytes";
            fassertNoTrace(50724, false);
        }

        if (memcmp(nowStr.c_str(), readBuffer.get(), nowStr.size()) != 0) {
            severe() << "Read wrong string from file '" << file.generic_string() << nowStr.size()
                     << " bytes (in hex) '" << toHexLower(nowStr.c_str(), nowStr.size())
                     << "' but read bytes '" << toHexLower(readBuffer.get(), bytesRead) << "'";
            fassertNoTrace(50717, false);
        }
    }

    if (!CloseHandle(hFile)) {
        std::uint32_t gle = ::GetLastError();
        severe() << "CloseHandle failed for '" << file.generic_string()
                 << "' with error: " << errnoWithDescription(gle);
        fassertNoTrace(4079, gle == 0);
    }
}

void watchdogTerminate() {
    ::TerminateProcess(::GetCurrentProcess(), ExitCode::EXIT_WATCHDOG);
}

#else

/**
 * Check a directory is ok
 * 1. Open up a direct_io to a new file
 * 2. Write to the file
 * 3. Read from the file
 * 4. Close file
 */
void checkFile(OperationContext* opCtx, const boost::filesystem::path& file) {
    Date_t now = opCtx->getServiceContext()->getPreciseClockSource()->now();
    std::string nowStr = now.toString();

    int fd = open(file.generic_string().c_str(), O_RDWR | O_CREAT, S_IRUSR | S_IWUSR);
    if (fd == -1) {
        auto err = errno;
        severe() << "open failed for '" << file.generic_string()
                 << "' with error: " << errnoWithDescription(err);
        fassertNoTrace(4080, err == 0);
    }

    size_t bytesWrittenTotal = 0;
    while (bytesWrittenTotal < nowStr.size()) {
        ssize_t bytesWrittenInWrite =
            write(fd, nowStr.c_str() + bytesWrittenTotal, nowStr.size() - bytesWrittenTotal);
        if (bytesWrittenInWrite == -1) {
            auto err = errno;
            if (err == EINTR) {
                continue;
            }

            severe() << "write failed for '" << file.generic_string()
                     << "' with error: " << errnoWithDescription(err);
            fassertNoTrace(4081, err == 0);
        }

        // Warn if the write was incomplete
        if (bytesWrittenTotal == 0 && static_cast<size_t>(bytesWrittenInWrite) != nowStr.size()) {
            warning() << "parital write for '" << file.generic_string() << "' expected "
                      << nowStr.size() << " bytes but wrote " << bytesWrittenInWrite << " bytes";
        }

        bytesWrittenTotal += bytesWrittenInWrite;
    }

    if (fsync(fd)) {
        auto err = errno;
        severe() << "fsync failed for '" << file.generic_string()
                 << "' with error: " << errnoWithDescription(err);
        fassertNoTrace(4082, err == 0);
    }

    auto readBuffer = std::make_unique<char[]>(nowStr.size());
    size_t bytesReadTotal = 0;
    while (bytesReadTotal < nowStr.size()) {
        ssize_t bytesReadInRead = pread(
            fd, readBuffer.get() + bytesReadTotal, nowStr.size() - bytesReadTotal, bytesReadTotal);
        if (bytesReadInRead == -1) {
            auto err = errno;
            if (err == EINTR) {
                continue;
            }

            severe() << "read failed for '" << file.generic_string()
                     << "' with error: " << errnoWithDescription(err);
            fassertNoTrace(4083, err == 0);
        } else if (bytesReadInRead == 0) {
            severe() << "read failed for '" << file.generic_string()
                     << "' with unexpected end of file";
            fassertNoTrace(50719, false);
        }

        // Warn if the read was incomplete
        if (bytesReadTotal == 0 && static_cast<size_t>(bytesReadInRead) != nowStr.size()) {
            warning() << "partial read for '" << file.generic_string() << "' expected "
                      << nowStr.size() << " bytes but read " << bytesReadInRead << " bytes";
        }

        bytesReadTotal += bytesReadInRead;
    }

    if (memcmp(nowStr.c_str(), readBuffer.get(), nowStr.size()) != 0) {
        severe() << "Read wrong string from file '" << file.generic_string() << "' expected "
                 << nowStr.size() << " bytes (in hex) '"
                 << toHexLower(nowStr.c_str(), nowStr.size()) << "' but read bytes '"
                 << toHexLower(readBuffer.get(), bytesReadTotal) << "'";
        fassertNoTrace(50718, false);
    }

    if (close(fd)) {
        auto err = errno;
        severe() << "close failed for '" << file.generic_string()
                 << "' with error: " << errnoWithDescription(err);
        fassertNoTrace(4084, err == 0);
    }
}

void watchdogTerminate() {
    // This calls the exit_group syscall on Linux
    ::_exit(ExitCode::EXIT_WATCHDOG);
}
#endif

constexpr StringData DirectoryCheck::kProbeFileName;
constexpr StringData DirectoryCheck::kProbeFileNameExt;

void DirectoryCheck::run(OperationContext* opCtx) {
    // Ensure we have unique file names if multiple processes share the same logging directory
    boost::filesystem::path file = _directory;
    file /= kProbeFileName.toString();
    file += ProcessId::getCurrent().toString();
    file += kProbeFileNameExt.toString();

    checkFile(opCtx, file);

    // Try to delete the file so it is not leaked on restart, but ignore errors
    boost::system::error_code ec;
    boost::filesystem::remove(file, ec);
    if (ec) {
        warning() << "Failed to delete file '" << file.generic_string()
                  << "', error: " << ec.message();
    }
}

std::string DirectoryCheck::getDescriptionForLogging() {
    return str::stream() << "checked directory '" << _directory.generic_string() << "'";
}

}  // namespace mongo
