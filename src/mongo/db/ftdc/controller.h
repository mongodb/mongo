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

#include "mongo/base/status.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/ftdc/collector.h"
#include "mongo/db/ftdc/config.h"
#include "mongo/db/ftdc/file_manager.h"
#include "mongo/db/ftdc/ftdc_feature_flag_gen.h"
#include "mongo/platform/atomic.h"
#include "mongo/stdx/condition_variable.h"
#include "mongo/stdx/mutex.h"
#include "mongo/stdx/thread.h"
#include "mongo/util/duration.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <utility>

#include <boost/filesystem/path.hpp>

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
    class Env {
    public:
        virtual ~Env() = default;
        virtual void onStartLoop() {}
    };

    FTDCController(boost::filesystem::path path,
                   FTDCConfig config,
                   std::unique_ptr<Env> env = _makeDefaultEnv())
        : _env(std::move(env)),
          _path(std::move(path)),
          _config(std::move(config)),
          _configTemp(_config) {
        if (feature_flags::gFeatureFlagGaplessFTDC.isEnabled()) {
            _asyncPeriodicCollectors.emplace(
                _config.sampleTimeout, _config.minThreads, _config.maxThreads);
        }
    }

    ~FTDCController() = default;

    /**
     * Get the number of async periodic collectors that are registered on this controller.
     */
    long long getNumAsyncPeriodicCollectors();

    /*
     * Set whether the controller is enabled, and collects data.
     *
     * Returns ErrorCodes::FTDCPathNotSet if no log path has been specified for FTDC. This occurs
     * in MongoS in some situations since MongoS is not required to have a storage directory like
     * MongoD does.
     */
    Status setEnabled(bool enabled);

    /**
     * Set the frequency of metadata capture. Metadata will be captured once every `freq` times FTDC
     * data is collected.
     */
    void setMetadataCaptureFrequency(std::uint64_t freq);

    /**
     * Get the period for data collection.
     */
    Milliseconds getPeriod();

    /**
     * Set the period for data collection.
     */
    void setPeriod(Milliseconds millis);

    /**
     * Set the sample timeout for async collectors.
     */
    Status setSampleTimeout(Milliseconds newValue);

    /**
     * Set the minimum number of threads used to run async collectors.
     */
    Status setMinThreads(size_t newValue);

    /**
     * Set the maximum number of threads used to run async collectors.
     */
    Status setMaxThreads(size_t newValue);

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
     * Add a metadata collector to collect periodically (e.g., Configuration Settings). Does not
     * lose info on compression. Collects as or less frequently than PeriodicCollector.
     *
     * It is not safe to call this after FTDC startup.
     */
    void addPeriodicMetadataCollector(std::unique_ptr<FTDCCollectorInterface> collector);

    /**
     * Add a metric collector to collect periodically (e.g., serverStatus).
     *
     * It is not safe to call this after FTDC startup.
     */
    void addPeriodicCollector(std::unique_ptr<FTDCCollectorInterface> collector);

    /**
     * Add a collector that gathers machine or process configuration settings (not metrics).
     * These are emitted at the top of every log file the server produces, which is
     * why the "on rotate" terminology is used.
     *
     * It is not safe to call this after FTDC startup.
     */
    void addOnRotateCollector(std::unique_ptr<FTDCCollectorInterface> collector);

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
     * Get the FTDCController from ServiceContext.
     */
    static FTDCController* get(ServiceContext* serviceContext);

    /**
     * Get a reference to most recent document from the periodic collectors.
     */
    BSONObj getMostRecentPeriodicDocument();

    /*
     * Forces a rotate before the next FTDC log is written.
     */
    void triggerRotate();

private:
    static std::unique_ptr<Env> _makeDefaultEnv() {
        return std::make_unique<Env>();
    }

    /**
     * Do periodic statistics collection, and all other work on the background thread.
     */
    void doLoop(Service* service);

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

    std::unique_ptr<Env> _env;

    // state
    State _state{State::kNotStarted};

    // Directory to store files
    boost::filesystem::path _path;

    // Mutex to protect the condvar, configuration changes, and most recent periodic document.
    stdx::mutex _mutex;
    stdx::condition_variable _condvar;

    // Indicates that a rotate should be triggered before the next FTDC log is collected and
    // written.
    Atomic<bool> _shouldRotateBeforeNextSample = false;

    // Config settings that are used by controller, file manager, and all other classes.
    // Copied from _configTemp periodically to get a consistent snapshot.
    FTDCConfig _config;

    // Config settings that are manipulated by setters via setParameter.
    FTDCConfig _configTemp;

    // Set of periodic metadata collectors
    SyncFTDCCollectorCollection _periodicMetadataCollectors;

    // Set of periodic collectors
    SyncFTDCCollectorCollection _periodicCollectors;

    // Set of async periodic collectors
    boost::optional<AsyncFTDCCollectorCollection> _asyncPeriodicCollectors;

    // Tracks the number of async periodic collectors that are registered with this controller.
    Counter64 _numAsyncPeriodicCollectors;

    // Last seen sample document from periodic collectors
    // Owned
    BSONObj _mostRecentPeriodicDocument;

    // Set of file rotation collectors
    SyncFTDCCollectorCollection _rotateCollectors;

    // File manager that manages file rotation, and logging
    std::unique_ptr<FTDCFileManager> _mgr;

    // Background collection and writing thread
    stdx::thread _thread;
};

}  // namespace mongo
