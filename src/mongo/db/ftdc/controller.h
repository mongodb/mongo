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

#pragma once

#include <boost/filesystem/path.hpp>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <utility>

#include "mongo/base/status.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/ftdc/collector.h"
#include "mongo/db/ftdc/config.h"
#include "mongo/db/ftdc/file_manager.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/service_context.h"
#include "mongo/platform/mutex.h"
#include "mongo/stdx/condition_variable.h"
#include "mongo/stdx/thread.h"
#include "mongo/util/duration.h"

namespace mongo {

/**
 * Responsible for periodic collection of samples, writing them to disk,
 * and rotation.
 *
 * Exposes an methods to response to configuration changes in a thread-safe manner.
 */
class FTDCController {
    FTDCController(const FTDCController&) = delete;
    FTDCController& operator=(const FTDCController&) = delete;

public:
    FTDCController(const boost::filesystem::path path, FTDCConfig config)
        : _path(path), _config(std::move(config)), _configTemp(_config) {}

    ~FTDCController() = default;

    /*
     * Set whether the controller is enabled, and collects data.
     *
     * Returns ErrorCodes::FTDCPathNotSet if no log path has been specified for FTDC. This occurs
     * in MongoS in some situations since MongoS is not required to have a storage directory like
     * MongoD does.
     */
    Status setEnabled(bool enabled);

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

    /*
     * Set the path to store FTDC files if not already set.
     *
     * Returns ErrorCodes::FTDCPathAlreadySet if the path has already been set.
     */
    Status setDirectory(const boost::filesystem::path& path);

    /**
     * Add a metric collector to collect periodically (e.g., serverStatus).
     *
     * `role` is used to disambiguate role-specific collectors with colliding names.
     * It must be `ClusterRole::ShardServer`, `ClusterRole::RouterServer`, or `ClusterRole::None`.
     */
    void addPeriodicCollector(std::unique_ptr<FTDCCollectorInterface> collector, ClusterRole role);

    /**
     * Add a collector that gathers machine or process configuration settings (not metrics).
     * These are emitted at the top of every log file the server produces, which is
     * why the "on rotate" terminology is used.
     *
     * `role` is used to disambiguate role-specific collectors with colliding names.
     * It must be `ClusterRole::ShardServer`, `ClusterRole::RouterServer`, or `ClusterRole::None`.
     */
    void addOnRotateCollector(std::unique_ptr<FTDCCollectorInterface> collector, ClusterRole role);

    /**
     * Start the controller.
     *
     * Spawns a new thread.
     */
    void start(Service* service);

    /**
     * Stop the controller.
     *
     * Does not require start to be called to support early exit by mongod.
     */
    void stop();

    /**
     * Get the FTDCController from Service.
     */
    static FTDCController* get(Service* service);

    /**
     * Get a reference to most recent document from the periodic collectors.
     */
    BSONObj getMostRecentPeriodicDocument();

private:
    /**
     * Do periodic statistics collection, and all other work on the background thread.
     */
    void doLoop(Service* service) noexcept;

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
    boost::filesystem::path _path;

    // Mutex to protect the condvar, configuration changes, and most recent periodic document.
    Mutex _mutex = MONGO_MAKE_LATCH("FTDCController::_mutex");
    stdx::condition_variable _condvar;

    // Config settings that are used by controller, file manager, and all other classes.
    // Copied from _configTemp periodically to get a consistent snapshot.
    FTDCConfig _config;

    // Config settings that are manipulated by setters via setParameter.
    FTDCConfig _configTemp;

    // Set of periodic collectors
    FTDCCollectorCollection _periodicCollectors;

    // Last seen sample document from periodic collectors
    // Owned
    BSONObj _mostRecentPeriodicDocument;

    // Set of file rotation collectors
    FTDCCollectorCollection _rotateCollectors;

    // File manager that manages file rotation, and logging
    std::unique_ptr<FTDCFileManager> _mgr;

    // Background collection and writing thread
    stdx::thread _thread;
};

}  // namespace mongo
