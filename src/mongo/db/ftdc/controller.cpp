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


#include <boost/filesystem/path.hpp>
#include <boost/move/utility_core.hpp>
// IWYU pragma: no_include "cxxabi.h"
#include "mongo/base/error_codes.h"
#include "mongo/db/client.h"
#include "mongo/db/ftdc/collector.h"
#include "mongo/db/ftdc/controller.h"
#include "mongo/db/ftdc/util.h"
#include "mongo/db/service_context.h"
#include "mongo/logv2/log.h"
#include "mongo/stdx/condition_variable.h"
#include "mongo/stdx/mutex.h"
#include "mongo/stdx/thread.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/clock_source.h"
#include "mongo/util/concurrency/idle_thread_block.h"
#include "mongo/util/str.h"
#include "mongo/util/time_support.h"

#include <memory>
#include <mutex>
#include <tuple>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kFTDC


namespace mongo {

long long FTDCController::getNumAsyncPeriodicCollectors() {
    return _numAsyncPeriodicCollectors.get();
}

Status FTDCController::setEnabled(bool enabled) {
    stdx::lock_guard<stdx::mutex> lock(_mutex);

    if (_path.empty()) {
        return Status(ErrorCodes::FTDCPathNotSet,
                      str::stream() << "FTDC cannot be enabled without setting the set parameter "
                                       "'diagnosticDataCollectionDirectoryPath' first.");
    }

    _configTemp.enabled = enabled;
    _condvar.notify_one();

    return Status::OK();
}

void FTDCController::setMetadataCaptureFrequency(std::uint64_t freq) {
    stdx::lock_guard<stdx::mutex> lock(_mutex);
    _configTemp.metadataCaptureFrequency = freq;
    _condvar.notify_one();
}

Milliseconds FTDCController::getPeriod() {
    stdx::lock_guard<stdx::mutex> lock(_mutex);
    return _configTemp.period;
}

void FTDCController::setPeriod(Milliseconds millis) {
    stdx::lock_guard<stdx::mutex> lock(_mutex);
    _configTemp.period = millis;
    _condvar.notify_one();
}

Status FTDCController::setSampleTimeout(Milliseconds newValue) {
    if (!feature_flags::gFeatureFlagGaplessFTDC.isEnabled()) {
        return Status(
            ErrorCodes::InvalidOptions,
            "The diagnosticDataCollectionSampleTimeoutMillis parameter is not available with "
            "featureFlagGaplessFTDC disabled.");
    }

    stdx::lock_guard<stdx::mutex> lock(_mutex);
    _configTemp.sampleTimeout = newValue;
    _asyncPeriodicCollectors->updateSampleTimeout(newValue);
    _condvar.notify_one();
    return Status::OK();
}

Status FTDCController::setMinThreads(size_t newValue) {
    if (!feature_flags::gFeatureFlagGaplessFTDC.isEnabled()) {
        return Status(ErrorCodes::InvalidOptions,
                      "The diagnosticDataCollectionMinThreads parameter is not available with "
                      "featureFlagGaplessFTDC disabled.");
    }

    stdx::lock_guard<stdx::mutex> lock(_mutex);
    if (newValue > _configTemp.maxThreads) {
        return Status(
            ErrorCodes::BadValue,
            fmt::format("Cannot set diagnosticDataCollectionMinThreads to {}. The value must not "
                        "exceed diagnosticDataCollectionMaxThreads, currently set to {}.",
                        newValue,
                        _configTemp.maxThreads));
    }

    _configTemp.minThreads = newValue;
    _asyncPeriodicCollectors->updateMinThreads(newValue);
    _condvar.notify_one();
    return Status::OK();
}

Status FTDCController::setMaxThreads(size_t newValue) {
    if (!feature_flags::gFeatureFlagGaplessFTDC.isEnabled()) {
        return Status(ErrorCodes::InvalidOptions,
                      "The diagnosticDataCollectionMaxThreads parameter is not available with "
                      "featureFlagGaplessFTDC disabled.");
    }

    stdx::lock_guard<stdx::mutex> lock(_mutex);
    if (newValue < _configTemp.minThreads) {
        return Status(
            ErrorCodes::BadValue,
            fmt::format("Cannot set diagnosticDataCollectionMaxThreads to {}. The value must not "
                        "fall below diagnosticDataCollectionMinThreads, currently set to {}.",
                        newValue,
                        _configTemp.minThreads));
    }

    _configTemp.maxThreads = newValue;
    _asyncPeriodicCollectors->updateMaxThreads(newValue);
    _condvar.notify_one();
    return Status::OK();
}

void FTDCController::setMaxDirectorySizeBytes(std::uint64_t size) {
    stdx::lock_guard<stdx::mutex> lock(_mutex);
    _configTemp.maxDirectorySizeBytes = size;
    _condvar.notify_one();
}

void FTDCController::setMaxFileSizeBytes(std::uint64_t size) {
    stdx::lock_guard<stdx::mutex> lock(_mutex);
    _configTemp.maxFileSizeBytes = size;
    _condvar.notify_one();
}

void FTDCController::setMaxSamplesPerArchiveMetricChunk(size_t size) {
    stdx::lock_guard<stdx::mutex> lock(_mutex);
    _configTemp.maxSamplesPerArchiveMetricChunk = size;
    _condvar.notify_one();
}

void FTDCController::setMaxSamplesPerInterimMetricChunk(size_t size) {
    stdx::lock_guard<stdx::mutex> lock(_mutex);
    _configTemp.maxSamplesPerInterimMetricChunk = size;
    _condvar.notify_one();
}

Status FTDCController::setDirectory(const boost::filesystem::path& path) {
    stdx::lock_guard<stdx::mutex> lock(_mutex);

    if (!_path.empty()) {
        return Status(ErrorCodes::FTDCPathAlreadySet,
                      str::stream() << "FTDC path has already been set to '" << _path.string()
                                    << "'. It cannot be changed.");
    }

    _path = path;

    // Do not notify for the change since it has to be enabled via setEnabled.

    return Status::OK();
}

void FTDCController::addPeriodicMetadataCollector(
    std::unique_ptr<FTDCCollectorInterface> collector) {
    stdx::lock_guard lock(_mutex);
    invariant(_state == State::kNotStarted);

    _periodicMetadataCollectors.add(std::move(collector));
}

void FTDCController::addPeriodicCollector(std::unique_ptr<FTDCCollectorInterface> collector) {
    stdx::lock_guard lock(_mutex);
    invariant(_state == State::kNotStarted);

    if (feature_flags::gFeatureFlagGaplessFTDC.isEnabled()) {
        _asyncPeriodicCollectors->add(std::move(collector));
        _numAsyncPeriodicCollectors.increment();
        return;
    }

    _periodicCollectors.add(std::move(collector));
}

void FTDCController::addOnRotateCollector(std::unique_ptr<FTDCCollectorInterface> collector) {
    stdx::lock_guard lock(_mutex);
    invariant(_state == State::kNotStarted);

    _rotateCollectors.add(std::move(collector));
}

BSONObj FTDCController::getMostRecentPeriodicDocument() {
    {
        stdx::lock_guard<stdx::mutex> lock(_mutex);
        return _mostRecentPeriodicDocument.getOwned();
    }
}

void FTDCController::triggerRotate() {
    _shouldRotateBeforeNextSample.store(true);
}

void FTDCController::start(Service* service) {
    LOGV2(20625,
          "Initializing full-time diagnostic data capture",
          "dataDirectory"_attr = _path.generic_string());

    // Start the thread
    _thread = stdx::thread([this, service] { doLoop(service); });

    {
        stdx::lock_guard lock(_mutex);

        invariant(_state == State::kNotStarted);
        _state = State::kStarted;
    }
}

void FTDCController::stop() {
    LOGV2(20626, "Shutting down full-time diagnostic data capture");

    {
        stdx::lock_guard lock(_mutex);

        bool started = (_state == State::kStarted);

        invariant(_state == State::kNotStarted || _state == State::kStarted);

        if (!started) {
            _state = State::kDone;
            return;
        }

        _configTemp.enabled = false;
        _state = State::kStopRequested;

        // Wake up the thread if sleeping so that it will check if we are done
        _condvar.notify_one();
    }

    _thread.join();

    {
        stdx::lock_guard lock(_mutex);
        _state = State::kDone;
    }

    if (_mgr) {
        auto s = _mgr->close();
        if (!s.isOK()) {
            LOGV2(20627,
                  "Failed to close full-time diagnostic data capture file manager",
                  "error"_attr = s);
        }
    }
}

void FTDCController::doLoop(Service* service) try {
    // Note: All exceptions thrown in this loop are considered process fatal.
    //
    // TODO(SERVER-74659): Please revisit if this thread could be made killable.
    Client::initThread(
        kFTDCThreadName, service, Client::noSession(), ClientOperationKillableByStepdown{false});
    Client* client = &cc();

    // Update config
    {
        stdx::lock_guard<stdx::mutex> lock(_mutex);
        _config = _configTemp;
    }

    // Periodic metadata is collected when metadataCaptureFrequencyCountdown hits 0 (which will
    // always include the first iteration). Then, the value of metadataCaptureFrequencyCountdown is
    // reset to _config.metadataCaptureFrequency and countdown starts again.
    std::uint64_t metadataCaptureFrequencyCountdown = 1;

    while (true) {
        _env->onStartLoop();

        // Compute the next interval to run regardless of how we were woken up
        // Skipping an interval due to a race condition with a config signal is harmless.
        auto now = getGlobalServiceContext()->getPreciseClockSource()->now();

        // Get next time to run at
        auto next_time = FTDCUtil::roundTime(now, _config.period);

        // Wait for the next run or signal to shutdown
        {
            stdx::unique_lock<stdx::mutex> lock(_mutex);
            MONGO_IDLE_THREAD_BLOCK;

            // We ignore spurious wakeups by just doing an iteration of the loop
            auto status = _condvar.wait_until(lock, next_time.toSystemTimePoint());

            // Are we done running?
            if (_state == State::kStopRequested) {
                break;
            }

            // Update the current configuration settings always
            // In unit tests, we may never get a signal when the timeout is 1ms on Windows since
            // MSVC 2013 converts wait_until(now() + 1ms) into ~ wait_for(0) which means it will
            // not wait for the condition variable to be signaled because it uses
            // GetFileSystemTime for now which has ~10 ms granularity.
            _config = _configTemp;

            // if we hit a timeout on the condvar, we need to do another collection
            // if we were signalled, then we have a config update only or were asked to stop
            if (status == stdx::cv_status::no_timeout) {
                continue;
            }
        }

        // TODO: consider only running this thread if we are enabled
        // for now, we just keep an idle thread as it is simpler
        if (!_config.enabled) {
            continue;
        }

        // Delay initialization of FTDCFileManager until we are sure the user has enabled
        // FTDC
        if (!_mgr) {
            auto swMgr = FTDCFileManager::create(&_config, _path, &_rotateCollectors, client);

            _mgr = uassertStatusOK(std::move(swMgr));
        }

        if (bool req = true; _shouldRotateBeforeNextSample.compareAndSwap(&req, false)) {
            iassert(_mgr->rotate(client));
        }

        auto collectSample = feature_flags::gFeatureFlagGaplessFTDC.isEnabled()
            ? _asyncPeriodicCollectors->collect(client)
            : _periodicCollectors.collect(client);

        Status s = _mgr->writeSampleAndRotateIfNeeded(
            client, std::get<0>(collectSample), std::get<1>(collectSample));

        uassertStatusOK(s);

        // Store a reference to the most recent document from the periodic collectors
        {
            stdx::lock_guard<stdx::mutex> lock(_mutex);
            _mostRecentPeriodicDocument = std::get<0>(collectSample);
        }

        if (--metadataCaptureFrequencyCountdown == 0) {
            metadataCaptureFrequencyCountdown = _config.metadataCaptureFrequency;
            auto collectSample = _periodicMetadataCollectors.collect(client);
            Status s = _mgr->writePeriodicMetadataSampleAndRotateIfNeeded(
                client, std::get<0>(collectSample), std::get<1>(collectSample));
            iassert(s);
        }
    }
} catch (...) {
    LOGV2_FATAL(9399800,
                "Exception thrown in full-time diagnostic data capture subsystem. Terminating the "
                "process because diagnostics cannot be captured.",
                "exception"_attr = exceptionToStatus());
}

}  // namespace mongo
