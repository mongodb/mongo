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


#include "mongo/watchdog/watchdog.h"

#include <cerrno>
#include <cstring>
#include <mutex>
#include <ratio>
#include <system_error>
#include <utility>

#include <boost/align.hpp>  // IWYU pragma: keep
#include <boost/align/align_up.hpp>
// IWYU pragma: no_include "boost/align/detail/aligned_alloc_posix.hpp"
#include <boost/filesystem/operations.hpp>
#include <boost/filesystem/path.hpp>
// IWYU pragma: no_include "boost/system/detail/error_code.hpp"

#ifndef _WIN32
#include <fcntl.h>

#include <sys/stat.h>
#endif

#include "mongo/base/error_codes.h"
#include "mongo/config.h"  // IWYU pragma: keep
#include "mongo/db/client.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"
#include "mongo/logv2/log.h"
#include "mongo/platform/process_id.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/clock_source.h"
#include "mongo/util/concurrency/idle_thread_block.h"
#include "mongo/util/errno_util.h"
#include "mongo/util/exit.h"
#include "mongo/util/exit_code.h"
#include "mongo/util/str.h"
#include "mongo/util/testing_proctor.h"
#include "mongo/util/time_support.h"
#include "mongo/util/timer.h"

#if defined(MONGO_CONFIG_HAVE_HEADER_UNISTD_H)
#include <unistd.h>
#endif

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kControl


namespace mongo {

namespace {

const auto getWatchdogMonitorInterface =
    ServiceContext::declareDecoration<std::unique_ptr<WatchdogMonitorInterface>>();

}  // namespace

WatchdogPeriodicThread::WatchdogPeriodicThread(Milliseconds period, StringData threadName)
    : _period(period), _enabled(true), _threadName(std::string{threadName}) {}

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

    {
        stdx::lock_guard<stdx::mutex> lock(_mutex);
        invariant(_state == State::kShutdownRequested);
        _state = State::kDone;
    }
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
    // TODO(SERVER-74659): Please revisit if this thread could be made killable.
    Client::initThread(_threadName,
                       getGlobalServiceContext()->getService(ClusterRole::ShardServer),
                       Client::noSession(),
                       ClientOperationKillableByStepdown{false});

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

            while ((startTime + _period) > preciseClockSource->now()) {
                // We are signalled on period changes at which point we may be done waiting or need
                // to wait longer. If the period changes and we still need to wait longer, it's
                // important that we call waitForConditionOrInterruptUntil again, as it will
                // otherwise continue to use a deadline based on the old period.
                auto oldPeriod = _period;
                try {
                    opCtx->waitForConditionOrInterruptUntil(
                        _condvar, lock, startTime + _period, [&] {
                            return oldPeriod != _period || _state == State::kShutdownRequested;
                        });
                } catch (const ExceptionFor<ErrorCodes::InterruptedDueToStorageChange>&) {
                    LOGV2_DEBUG(
                        6644400, 1, "Watchdog interrupted due to storage change. Retrying.");
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

                    // This interruption ends the WatchdogPeriodicThread. This means it is possible
                    // to killOp this operation and stop it for the lifetime of the process.
                    LOGV2_DEBUG(
                        23406, 1, "WatchdogPeriodicThread interrupted by: {e}", "e"_attr = e);
                    return;
                }

                // Are we done running?
                if (_state == State::kShutdownRequested) {
                    return;
                }
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

void WatchdogCheckThread::setShouldRunChecks(const bool shouldRunChecks) {
    _shouldRunChecks.store(shouldRunChecks);
}

void WatchdogCheckThread::run(OperationContext* opCtx) {
    for (auto& check : _checks) {
        Timer timer(opCtx->getServiceContext()->getTickSource());

        if (_shouldRunChecks.load()) {
            check->run(opCtx);
            Microseconds micros = timer.elapsed();

            LOGV2_DEBUG(8350803,
                        1,
                        "Watchdog test checked '{check_getDescriptionForLogging}' took "
                        "{duration_cast_Milliseconds_micros}",
                        "check_getDescriptionForLogging"_attr = check->getDescriptionForLogging(),
                        "duration_cast_Milliseconds_micros"_attr =
                            duration_cast<Milliseconds>(micros));
        } else {
            LOGV2_DEBUG(8350802,
                        1,
                        "Watchdog skipping running check",
                        "check_getDescriptionForLogging"_attr = check->getDescriptionForLogging());
        }

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
    _monitorGeneration.fetchAndAdd(1);
    auto currentGeneration = _checkThread->getGeneration();

    if (currentGeneration != _lastSeenGeneration) {
        _lastSeenGeneration = currentGeneration;
    } else {
        _callback();
    }
}

WatchdogMonitorInterface* WatchdogMonitorInterface::get(ServiceContext* service) {
    return getWatchdogMonitorInterface(service).get();
}

WatchdogMonitorInterface* WatchdogMonitorInterface::get(OperationContext* ctx) {
    return getWatchdogMonitorInterface(ctx->getClient()->getServiceContext()).get();
}

WatchdogMonitorInterface* WatchdogMonitorInterface::getGlobalWatchdogMonitorInterface() {
    if (!hasGlobalServiceContext()) {
        return nullptr;
    }
    return getWatchdogMonitorInterface(getGlobalServiceContext()).get();
};

void WatchdogMonitorInterface::set(
    ServiceContext* service, std::unique_ptr<WatchdogMonitorInterface> watchdogMonitorInterface) {
    auto& coordinator = getWatchdogMonitorInterface(service);
    coordinator = std::move(watchdogMonitorInterface);
}


WatchdogMonitor::WatchdogMonitor(std::vector<std::unique_ptr<WatchdogCheck>> checks,
                                 Milliseconds checkPeriod,
                                 Milliseconds monitorPeriod,
                                 WatchdogDeathCallback callback)
    : WatchdogMonitorInterface(),
      _checkPeriod(checkPeriod),
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
        stdx::lock_guard<stdx::mutex> lock(_mutex);

        invariant(_state == State::kNotStarted);
        _state = State::kStarted;
    }
}

void WatchdogMonitor::pauseChecks() {
    {
        stdx::lock_guard<stdx::mutex> lock(_mutex);
        if (_state == State::kStarted || _state == State::kShutdownRequested) {
            LOGV2(8350800, "WatchdogMonitor pausing watchdog checks");
            _watchdogCheckThread.setShouldRunChecks(false);
        }
    }
}

void WatchdogMonitor::unpauseChecks() {
    {
        stdx::lock_guard<stdx::mutex> lock(_mutex);
        if (_state == State::kStarted || _state == State::kShutdownRequested) {
            LOGV2(8350801, "WatchdogMonitor unpausing watchdog checks");
            _watchdogCheckThread.setShouldRunChecks(true);
        }
    }
}

bool WatchdogMonitor::getShouldRunChecks_forTest() {
    MONGO_UNREACHABLE;
}

void WatchdogMonitor::setPeriod(Milliseconds duration) {
    {
        stdx::lock_guard<stdx::mutex> lock(_mutex);

        if (duration > Milliseconds(0)) {
            dassert(duration >= Milliseconds(1));

            // Make sure that we monitor runs more frequently then checks
            // 2 feels like an arbitrary good minimum.
            invariant((TestingProctor::instance().isInitialized() &&
                       TestingProctor::instance().isEnabled()) ||
                      duration >= 2 * _checkPeriod);

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

    {
        stdx::lock_guard<stdx::mutex> lock(_mutex);
        invariant(_state == State::kShutdownRequested);
        _state = State::kDone;
    }
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

// Apple does not support O_DIRECT, so instead we use fcntl to enable the F_NOCACHE flag later.
#if defined(__APPLE__)
    int fd = open(file.generic_string().c_str(), O_RDWR | O_CREAT, S_IRUSR | S_IWUSR);
#else
    int fd = open(file.generic_string().c_str(), O_RDWR | O_CREAT | O_DIRECT, S_IRUSR | S_IWUSR);
#endif

    if (fd == -1) {
        auto ec = lastSystemError();
        LOGV2_FATAL_CONTINUE(23424,
                             "open failed in checkFile",
                             "filepath"_attr = file.generic_string(),
                             "error"_attr = errorMessage(ec));
        fassertNoTrace(4080, !ec);
    }

#if defined(__APPLE__)
    if (-1 == fcntl(fd, F_NOCACHE, 1)) {
        auto ec = lastSystemError();
        LOGV2_FATAL_CONTINUE(6319301,
                             "fcntl failed in checkFile",
                             "filepath"_attr = file.generic_string(),
                             "error"_attr = errorMessage(ec));
        fassertNoTrace(6319302, !ec);
    }
#endif

    struct stat st;
    if (fstat(fd, &st) < 0) {
        auto ec = lastSystemError();
        LOGV2(6319300, "fstat failed in checkFile", "error"_attr = errorMessage(ec));
        // default to reasonable power of two
        st.st_blksize = 4096;
    }

    unsigned long alignment = st.st_blksize;
    unsigned long alignedSize = boost::alignment::align_up(nowStr.size(), alignment);
    char* alignedBuf = static_cast<char*>(boost::alignment::aligned_alloc(alignment, alignedSize));
    ScopeGuard cleanupBuf([alignedBuf]() { boost::alignment::aligned_free(alignedBuf); });

    memset(alignedBuf, 0, alignedSize);
    memcpy(alignedBuf, nowStr.c_str(), nowStr.size());

    ssize_t bytesWrittenInWrite = -1;
    while (bytesWrittenInWrite == -1) {
        bytesWrittenInWrite = write(fd, alignedBuf, alignedSize);
        if (bytesWrittenInWrite == -1) {
            auto ec = lastSystemError();
            if (ec != systemError(EINTR)) {
                LOGV2_FATAL_CONTINUE(23425,
                                     "write failed in checkFile",
                                     "filepath"_attr = file.generic_string(),
                                     "error"_attr = errorMessage(ec));
                fassertNoTrace(4081, !ec);
            }
        }
    }
    if (static_cast<size_t>(bytesWrittenInWrite) != alignedSize) {
        LOGV2_FATAL_NOTRACE(23412,
                            "partial or EOF write in checkFile",
                            "filepath"_attr = file.generic_string(),
                            "alignedSize"_attr = alignedSize,
                            "bytesWritten"_attr = bytesWrittenInWrite);
    }

    if (fsync(fd)) {
        auto ec = lastSystemError();
        LOGV2_FATAL_CONTINUE(23426,
                             "fsync failed in checkFile",
                             "filepath"_attr = file.generic_string(),
                             "error"_attr = errorMessage(ec));
        fassertNoTrace(4082, !ec);
    }

    ssize_t bytesReadInRead = -1;
    while (bytesReadInRead == -1) {
        bytesReadInRead = pread(fd, alignedBuf, alignedSize, 0);
        if (bytesReadInRead == -1) {
            auto ec = lastSystemError();
            if (ec != systemError(EINTR)) {
                LOGV2_FATAL_CONTINUE(23427,
                                     "read failed in checkFile",
                                     "filepath"_attr = file.generic_string(),
                                     "error"_attr = errorMessage(ec));
                fassertNoTrace(4083, !ec);
            }
        }
    }
    if (static_cast<size_t>(bytesReadInRead) != alignedSize) {
        LOGV2_FATAL_NOTRACE(23413,
                            "partial or EOF read in checkFile",
                            "filepath"_attr = file.generic_string(),
                            "alignedSize"_attr = alignedSize,
                            "bytesRead"_attr = bytesReadInRead);
    }

    if (memcmp(nowStr.c_str(), alignedBuf, nowStr.size()) != 0) {
        LOGV2_FATAL_NOTRACE(
            50718, "Read wrong string in checkFile", "filepath"_attr = file.generic_string());
    }

    if (close(fd)) {
        auto ec = lastSystemError();
        LOGV2_FATAL_CONTINUE(23430,
                             "close failed in checkFile",
                             "filepath"_attr = file.generic_string(),
                             "error"_attr = errorMessage(ec));
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
    file /= std::string{kProbeFileName};
    file += ProcessId::getCurrent().toString();
    file += std::string{kProbeFileNameExt};

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
    return str::stream() << " directory '" << _directory.generic_string() << "'";
}

}  // namespace mongo
