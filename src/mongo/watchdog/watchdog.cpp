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
#include "mongo/logv2/log.h"
#include "mongo/platform/process_id.h"
#include "mongo/util/concurrency/idle_thread_block.h"
#include "mongo/util/errno_util.h"
#include "mongo/util/exit.h"
#include "mongo/util/exit_code.h"
#include "mongo/util/hex.h"
#include "mongo/util/timer.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kControl


namespace mongo {

WatchdogPeriodicThread::WatchdogPeriodicThread(Milliseconds period, StringData threadName)
    : _period(period), _enabled(true), _threadName(threadName.toString()) {}

void WatchdogPeriodicThread::start() {
    {
        stdx::lock_guard<Latch> lock(_mutex);

        invariant(_state == State::kNotStarted);
        _state = State::kStarted;

        // Start the thread.
        _thread = stdx::thread([this] { this->doLoop(); });
    }
}

void WatchdogPeriodicThread::shutdown() {

    stdx::thread thread;

    {
        stdx::lock_guard<Latch> lock(_mutex);

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
    stdx::lock_guard<Latch> lock(_mutex);

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
        stdx::lock_guard<Latch> lock(_mutex);

        // Ensure state is starting from a clean slate.
        resetState();
    }

    while (true) {
        // Wait for the next run or signal to shutdown.

        auto opCtx = client->makeOperationContext();

        Date_t startTime = preciseClockSource->now();

        {
            stdx::unique_lock<Latch> lock(_mutex);
            MONGO_IDLE_THREAD_BLOCK;


            // Check if the period is different?
            // We are signalled on period changes at which point we may be done waiting or need to
            // wait longer.
            try {
                opCtx->waitForConditionOrInterruptUntil(_condvar, lock, startTime + _period, [&] {
                    return (startTime + _period) <= preciseClockSource->now() ||
                        _state == State::kShutdownRequested;
                });
            } catch (const ExceptionFor<ErrorCodes::InterruptedDueToStorageChange>&) {
                LOGV2_DEBUG(6644400, 1, "Watchdog interrupted due to storage change. Retrying.");
                continue;
            } catch (const DBException& e) {
                // The only bad status is when we are in shutdown
                if (!opCtx->getServiceContext()->getKillAllOperations()) {
                    LOGV2_FATAL_CONTINUE(
                        23415,
                        "Watchdog was interrupted, shutting down, reason: {e_toStatus}",
                        "e_toStatus"_attr = e.toStatus());
                    exitCleanly(ExitCode::abrupt);
                }

                // This interruption ends the WatchdogPeriodicThread. This means it is possible to
                // killOp this operation and stop it for the lifetime of the process.
                LOGV2_DEBUG(23406, 1, "WatchdogPeriodicThread interrupted by: {e}", "e"_attr = e);
                return;
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

        LOGV2_DEBUG(23407,
                    1,
                    "Watchdog test '{check_getDescriptionForLogging}' took "
                    "{duration_cast_Milliseconds_micros}",
                    "check_getDescriptionForLogging"_attr = check->getDescriptionForLogging(),
                    "duration_cast_Milliseconds_micros"_attr = duration_cast<Milliseconds>(micros));

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
    LOGV2(23408, "Starting Watchdog Monitor");

    // Start the threads.
    _watchdogCheckThread.start();

    _watchdogMonitorThread.start();

    {
        stdx::lock_guard<Latch> lock(_mutex);

        invariant(_state == State::kNotStarted);
        _state = State::kStarted;
    }
}

void WatchdogMonitor::setPeriod(Milliseconds duration) {
    {
        stdx::lock_guard<Latch> lock(_mutex);

        if (duration > Milliseconds(0)) {
            dassert(duration >= Milliseconds(1));

            // Make sure that we monitor runs more frequently then checks
            // 2 feels like an arbitrary good minimum.
            invariant(duration >= 2 * _checkPeriod);

            _watchdogCheckThread.setPeriod(_checkPeriod);
            _watchdogMonitorThread.setPeriod(duration);

            LOGV2(23409,
                  "WatchdogMonitor period changed to {duration_cast_Seconds_duration}",
                  "duration_cast_Seconds_duration"_attr = duration_cast<Seconds>(duration));
        } else {
            _watchdogMonitorThread.setPeriod(duration);
            _watchdogCheckThread.setPeriod(duration);

            LOGV2(23410, "WatchdogMonitor disabled");
        }
    }
}

void WatchdogMonitor::shutdown() {
    {
        stdx::lock_guard<Latch> lock(_mutex);

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
        auto ec = lastSystemError();
        LOGV2_FATAL_CONTINUE(23416,
                             "CreateFile failed for '{file_generic_string}' with error: "
                             "{errnoWithDescription_gle}",
                             "file_generic_string"_attr = file.generic_string(),
                             "errnoWithDescription_gle"_attr = errorMessage(ec));
        fassertNoTrace(4074, !ec);
    }

    DWORD bytesWrittenTotal;
    if (!WriteFile(hFile, nowStr.c_str(), nowStr.size(), &bytesWrittenTotal, NULL)) {
        auto ec = lastSystemError();
        LOGV2_FATAL_CONTINUE(
            23417,
            "WriteFile failed for '{file_generic_string}' with error: {errnoWithDescription_gle}",
            "file_generic_string"_attr = file.generic_string(),
            "errnoWithDescription_gle"_attr = errorMessage(ec));
        fassertNoTrace(4075, !ec);
    }

    if (bytesWrittenTotal != nowStr.size()) {
        LOGV2_WARNING(23411,
                      "partial write for '{file_generic_string}' expected {nowStr_size} bytes but "
                      "wrote {bytesWrittenTotal} bytes",
                      "file_generic_string"_attr = file.generic_string(),
                      "nowStr_size"_attr = nowStr.size(),
                      "bytesWrittenTotal"_attr = bytesWrittenTotal);
    } else {

        if (!FlushFileBuffers(hFile)) {
            auto ec = lastSystemError();
            LOGV2_FATAL_CONTINUE(23418,
                                 "FlushFileBuffers failed for '{file_generic_string}' with error: "
                                 "{errnoWithDescription_gle}",
                                 "file_generic_string"_attr = file.generic_string(),
                                 "errnoWithDescription_gle"_attr = errorMessage(ec));
            fassertNoTrace(4076, !ec);
        }

        DWORD newOffset = SetFilePointer(hFile, 0, 0, FILE_BEGIN);
        if (newOffset != 0) {
            auto ec = lastSystemError();
            LOGV2_FATAL_CONTINUE(23419,
                                 "SetFilePointer failed for '{file_generic_string}' with error: "
                                 "{errnoWithDescription_gle}",
                                 "file_generic_string"_attr = file.generic_string(),
                                 "errnoWithDescription_gle"_attr = errorMessage(ec));
            fassertNoTrace(4077, !ec);
        }

        DWORD bytesRead;
        auto readBuffer = std::make_unique<char[]>(nowStr.size());
        if (!ReadFile(hFile, readBuffer.get(), nowStr.size(), &bytesRead, NULL)) {
            auto ec = lastSystemError();
            LOGV2_FATAL_CONTINUE(23420,
                                 "ReadFile failed for '{file_generic_string}' with error: "
                                 "{errnoWithDescription_gle}",
                                 "file_generic_string"_attr = file.generic_string(),
                                 "errnoWithDescription_gle"_attr = errorMessage(ec));
            fassertNoTrace(4078, !ec);
        }

        if (bytesRead != bytesWrittenTotal) {
            LOGV2_FATAL_NOTRACE(50724,
                                "Read wrong number of bytes for '{file_generic_string}' expected "
                                "{bytesWrittenTotal} bytes but read {bytesRead} bytes",
                                "file_generic_string"_attr = file.generic_string(),
                                "bytesWrittenTotal"_attr = bytesWrittenTotal,
                                "bytesRead"_attr = bytesRead);
        }

        if (memcmp(nowStr.c_str(), readBuffer.get(), nowStr.size()) != 0) {
            LOGV2_FATAL_NOTRACE(
                50717,
                "Read wrong string from file '{file_generic_string}{nowStr_size} bytes (in "
                "hex) '{toHexLower_nowStr_c_str_nowStr_size}' but read bytes "
                "'{toHexLower_readBuffer_get_bytesRead}'",
                "file_generic_string"_attr = file.generic_string(),
                "nowStr_size"_attr = nowStr.size(),
                "toHexLower_nowStr_c_str_nowStr_size"_attr = hexblob::encodeLower(nowStr),
                "toHexLower_readBuffer_get_bytesRead"_attr =
                    hexblob::encodeLower(readBuffer.get(), bytesRead));
        }
    }

    if (!CloseHandle(hFile)) {
        auto ec = lastSystemError();
        LOGV2_FATAL_CONTINUE(
            23423,
            "CloseHandle failed for '{file_generic_string}' with error: {errnoWithDescription_gle}",
            "file_generic_string"_attr = file.generic_string(),
            "errnoWithDescription_gle"_attr = errorMessage(ec));
        fassertNoTrace(4079, !ec);
    }
}

void watchdogTerminate() {
    ::TerminateProcess(::GetCurrentProcess(), static_cast<UINT>(ExitCode::watchdog));
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
        auto ec = lastSystemError();
        LOGV2_FATAL_CONTINUE(
            23424,
            "open failed for '{file_generic_string}' with error: {errnoWithDescription_err}",
            "file_generic_string"_attr = file.generic_string(),
            "errnoWithDescription_err"_attr = errorMessage(ec));
        fassertNoTrace(4080, !ec);
    }

    size_t bytesWrittenTotal = 0;
    while (bytesWrittenTotal < nowStr.size()) {
        ssize_t bytesWrittenInWrite =
            write(fd, nowStr.c_str() + bytesWrittenTotal, nowStr.size() - bytesWrittenTotal);
        if (bytesWrittenInWrite == -1) {
            auto ec = lastSystemError();
            if (ec == systemError(EINTR)) {
                continue;
            }

            LOGV2_FATAL_CONTINUE(
                23425,
                "write failed for '{file_generic_string}' with error: {errnoWithDescription_err}",
                "file_generic_string"_attr = file.generic_string(),
                "errnoWithDescription_err"_attr = errorMessage(ec));
            fassertNoTrace(4081, !ec);
        }

        // Warn if the write was incomplete
        if (bytesWrittenTotal == 0 && static_cast<size_t>(bytesWrittenInWrite) != nowStr.size()) {
            LOGV2_WARNING(23412,
                          "parital write for '{file_generic_string}' expected {nowStr_size} bytes "
                          "but wrote {bytesWrittenInWrite} bytes",
                          "file_generic_string"_attr = file.generic_string(),
                          "nowStr_size"_attr = nowStr.size(),
                          "bytesWrittenInWrite"_attr = bytesWrittenInWrite);
        }

        bytesWrittenTotal += bytesWrittenInWrite;
    }

    if (fsync(fd)) {
        auto ec = lastSystemError();
        LOGV2_FATAL_CONTINUE(
            23426,
            "fsync failed for '{file_generic_string}' with error: {errnoWithDescription_err}",
            "file_generic_string"_attr = file.generic_string(),
            "errnoWithDescription_err"_attr = errorMessage(ec));
        fassertNoTrace(4082, !ec);
    }

    auto readBuffer = std::make_unique<char[]>(nowStr.size());
    size_t bytesReadTotal = 0;
    while (bytesReadTotal < nowStr.size()) {
        ssize_t bytesReadInRead = pread(
            fd, readBuffer.get() + bytesReadTotal, nowStr.size() - bytesReadTotal, bytesReadTotal);
        if (bytesReadInRead == -1) {
            auto ec = lastSystemError();
            if (ec == systemError(EINTR)) {
                continue;
            }

            LOGV2_FATAL_CONTINUE(
                23427,
                "read failed for '{file_generic_string}' with error: {errnoWithDescription_err}",
                "file_generic_string"_attr = file.generic_string(),
                "errnoWithDescription_err"_attr = errorMessage(ec));
            fassertNoTrace(4083, !ec);
        } else if (bytesReadInRead == 0) {
            LOGV2_FATAL_NOTRACE(
                50719,
                "read failed for '{file_generic_string}' with unexpected end of file",
                "file_generic_string"_attr = file.generic_string());
        }

        // Warn if the read was incomplete
        if (bytesReadTotal == 0 && static_cast<size_t>(bytesReadInRead) != nowStr.size()) {
            LOGV2_WARNING(23413,
                          "partial read for '{file_generic_string}' expected {nowStr_size} bytes "
                          "but read {bytesReadInRead} bytes",
                          "file_generic_string"_attr = file.generic_string(),
                          "nowStr_size"_attr = nowStr.size(),
                          "bytesReadInRead"_attr = bytesReadInRead);
        }

        bytesReadTotal += bytesReadInRead;
    }

    if (memcmp(nowStr.c_str(), readBuffer.get(), nowStr.size()) != 0) {
        LOGV2_FATAL_NOTRACE(
            50718,
            "Read wrong string from file '{file_generic_string}' expected {nowStr_size} "
            "bytes (in hex) '{toHexLower_nowStr_c_str_nowStr_size}' but read bytes "
            "'{toHexLower_readBuffer_get_bytesReadTotal}'",
            "file_generic_string"_attr = file.generic_string(),
            "nowStr_size"_attr = nowStr.size(),
            "toHexLower_nowStr_c_str_nowStr_size"_attr = hexblob::encodeLower(nowStr),
            "toHexLower_readBuffer_get_bytesReadTotal"_attr =
                hexblob::encodeLower(readBuffer.get(), bytesReadTotal));
    }

    if (close(fd)) {
        auto ec = lastSystemError();
        LOGV2_FATAL_CONTINUE(
            23430,
            "close failed for '{file_generic_string}' with error: {errnoWithDescription_err}",
            "file_generic_string"_attr = file.generic_string(),
            "errnoWithDescription_err"_attr = errorMessage(ec));
        fassertNoTrace(4084, !ec);
    }
}

void watchdogTerminate() {
    // This calls the exit_group syscall on Linux
    ::_exit(static_cast<int>(ExitCode::watchdog));
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
        LOGV2_WARNING(23414,
                      "Failed to delete file '{file_generic_string}', error: {ec_message}",
                      "file_generic_string"_attr = file.generic_string(),
                      "ec_message"_attr = ec.message());
    }
}

std::string DirectoryCheck::getDescriptionForLogging() {
    return str::stream() << "checked directory '" << _directory.generic_string() << "'";
}

}  // namespace mongo
