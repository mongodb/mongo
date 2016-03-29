/**
 * Copyright (C) 2015 MongoDB Inc.
 *
 * This program is free software: you can redistribute it and/or  modify
 * it under the terms of the GNU Affero General Public License, version 3,
 * as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * As a special exception, the copyright holders give permission to link the
 * code of portions of this program with the OpenSSL library under certain
 * conditions as described in each individual source file and distribute
 * linked combinations including the program with the OpenSSL library. You
 * must comply with the GNU Affero General Public License in all respects
 * for all of the code used other than as permitted herein. If you modify
 * file(s) with this exception, you may extend this exception to your
 * version of the file(s), but you are not obligated to do so. If you do not
 * wish to do so, delete this exception statement from your version. If you
 * delete this exception statement from all source files in the program,
 * then also delete it in the license file.
 */

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kFTDC

#include "mongo/platform/basic.h"

#include "mongo/db/ftdc/controller.h"

#include "mongo/db/client.h"
#include "mongo/db/ftdc/collector.h"
#include "mongo/db/ftdc/util.h"
#include "mongo/db/jsobj.h"
#include "mongo/stdx/condition_variable.h"
#include "mongo/stdx/memory.h"
#include "mongo/stdx/mutex.h"
#include "mongo/stdx/thread.h"
#include "mongo/util/exit.h"
#include "mongo/util/log.h"
#include "mongo/util/time_support.h"

namespace mongo {

void FTDCController::setEnabled(bool enabled) {
    stdx::lock_guard<stdx::mutex> lock(_mutex);
    _configTemp.enabled = enabled;
}

void FTDCController::setPeriod(Milliseconds millis) {
    stdx::lock_guard<stdx::mutex> lock(_mutex);
    _configTemp.period = millis;
    _condvar.notify_one();
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

void FTDCController::addPeriodicCollector(std::unique_ptr<FTDCCollectorInterface> collector) {
    {
        stdx::lock_guard<stdx::mutex> lock(_mutex);
        invariant(_state == State::kNotStarted);

        _periodicCollectors.add(std::move(collector));
    }
}

void FTDCController::addOnRotateCollector(std::unique_ptr<FTDCCollectorInterface> collector) {
    {
        stdx::lock_guard<stdx::mutex> lock(_mutex);
        invariant(_state == State::kNotStarted);

        _rotateCollectors.add(std::move(collector));
    }
}

void FTDCController::start() {
    log() << "Initializing full-time diagnostic data capture with directory '"
          << _path.generic_string() << "'";

    // Start the thread
    _thread = stdx::thread(stdx::bind(&FTDCController::doLoop, this));

    {
        stdx::lock_guard<stdx::mutex> lock(_mutex);

        invariant(_state == State::kNotStarted);
        _state = State::kStarted;
    }
}

void FTDCController::stop() {
    log() << "Shutting down full-time diagnostic data capture";

    {
        stdx::lock_guard<stdx::mutex> lock(_mutex);

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

    _state = State::kDone;

    if (_mgr) {
        auto s = _mgr->close();
        if (!s.isOK()) {
            log() << "Failed to close full-time diagnostic data capture file manager: " << s;
        }
    }
}

void FTDCController::doLoop() {
    try {
        // Update config
        {
            stdx::lock_guard<stdx::mutex> lock(_mutex);
            _config = _configTemp;
        }

        Client::initThread("ftdc");
        Client* client = &cc();

        while (true) {
            // Compute the next interval to run regardless of how we were woken up
            // Skipping an interval due to a race condition with a config signal is harmless.
            auto now = getGlobalServiceContext()->getPreciseClockSource()->now();

            // Get next time to run at
            auto next_time = FTDCUtil::roundTime(now, _config.period);

            // Wait for the next run or signal to shutdown
            {
                stdx::unique_lock<stdx::mutex> lock(_mutex);

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
            // for now, we just keep an idle thread as it is simplier
            if (_config.enabled) {
                // Delay initialization of FTDCFileManager until we are sure the user has enabled
                // FTDC
                if (!_mgr) {
                    auto swMgr =
                        FTDCFileManager::create(&_config, _path, &_rotateCollectors, client);

                    _mgr = uassertStatusOK(std::move(swMgr));
                }

                auto collectSample = _periodicCollectors.collect(client);

                Status s = _mgr->writeSampleAndRotateIfNeeded(
                    client, std::get<0>(collectSample), std::get<1>(collectSample));

                uassertStatusOK(s);
            }
        }
    } catch (...) {
        warning() << "Uncaught exception in '" << exceptionToStatus()
                  << "' in full-time diagnostic data capture subsystem. Shutting down the "
                     "full-time diagnostic data capture subsystem.";
    }
}

}  // namespace mongo
