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

#pragma once

#include <boost/filesystem/path.hpp>
#include <cstdint>
#include <memory>

#include "mongo/base/disallow_copying.h"
#include "mongo/db/ftdc/collector.h"
#include "mongo/db/ftdc/config.h"
#include "mongo/db/ftdc/file_manager.h"
#include "mongo/db/jsobj.h"
#include "mongo/stdx/condition_variable.h"
#include "mongo/stdx/mutex.h"
#include "mongo/stdx/thread.h"

namespace mongo {

/**
 * Responsible for periodic collection of samples, writing them to disk,
 * and rotation.
 *
 * Exposes an methods to response to configuration changes in a thread-safe manner.
 */
class FTDCController {
    MONGO_DISALLOW_COPYING(FTDCController);

public:
    FTDCController(const boost::filesystem::path path, FTDCConfig config)
        : _path(path), _config(std::move(config)), _configTemp(_config) {}

    ~FTDCController() = default;

    /*
     * Set whether the controller is enabled, and collects data.
     */
    void setEnabled(bool enabled);

    /**
     * Set the period for data collection.
     */
    void setPeriod(Milliseconds millis);

    /**
     * Set the maximum directory size in bytes.
     */
    void setMaxDirectorySizeBytes(std::uint64_t size);

    /**
     * Set the maximum file size in bytes.
     */
    void setMaxFileSizeBytes(std::uint64_t size);

    /**
     * Set the maximum number of samples to store in a metric chunk. Larger numbers enable better
     * compression at the cost of allowing more data to be lost in the event of a crash.
     */
    void setMaxSamplesPerArchiveMetricChunk(size_t size);

    /**
     * Set the maximum number of samples to store in a interim chunk to minimize the number of
     * samples not stored in the archive log in the event of a crash.
     *
     * Smaller numbers will create unnecessary I/O.
     */
    void setMaxSamplesPerInterimMetricChunk(size_t size);

    /**
     * Add a metric collector to collect periodically. i.e., serverStatus
     */
    void addPeriodicCollector(std::unique_ptr<FTDCCollectorInterface> collector);

    /**
     * Add a collector to collect on server start, and file rotation. i.e. hostInfo
     *
     * This is for machine configuration settings, not metrics.
     */
    void addOnRotateCollector(std::unique_ptr<FTDCCollectorInterface> collector);

    /**
     * Start the controller.
     *
     * Spawns a new thread.
     */
    void start();

    /**
     * Stop the controller.
     *
     * Does not require start to be called to support early exit by mongod.
     */
    void stop();

private:
    /**
     * Do periodic statistics collection, and all other work on the background thread.
     */
    void doLoop();

private:
    /**
    * Private enum to track state.
    *
    *   +-----------------------------------------------------------+
    *   |                                                           v
    * +-------------+     +----------+     +----------------+     +-------+
    * | kNotStarted | --> | kStarted | --> | kStopRequested | --> | kDone |
    * +-------------+     +----------+     +----------------+     +-------+
    */
    enum class State {
        /**
         * Initial state. Either start() or stop() can be called next.
         */
        kNotStarted,

        /**
         * start() has been called. stop() should be called next.
         */
        kStarted,

        /**
         * stop() has been called, and the background thread is in progress of shutting down
         */
        kStopRequested,

        /**
         * Controller has been stopped.
         */
        kDone,
    };

    // state
    State _state{State::kNotStarted};

    // Directory to store files
    const boost::filesystem::path _path;

    // Mutex to protect the condvar, and configuration changes.
    stdx::mutex _mutex;
    stdx::condition_variable _condvar;

    // Config settings that are used by controller, file manager, and all other classes.
    // Copied from _configTemp periodically to get a consistent snapshot.
    FTDCConfig _config;

    // Config settings that are manipulated by setters via setParameter.
    FTDCConfig _configTemp;

    // Set of periodic collectors
    FTDCCollectorCollection _periodicCollectors;

    // Set of file rotation collectors
    FTDCCollectorCollection _rotateCollectors;

    // File manager that manages file rotation, and logging
    std::unique_ptr<FTDCFileManager> _mgr;

    // Background collection and writing thread
    stdx::thread _thread;
};

}  // namespace mongo
