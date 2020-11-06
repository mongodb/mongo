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

#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "mongo/base/status.h"
#include "mongo/db/client.h"
#include "mongo/db/free_mon/free_mon_message.h"
#include "mongo/db/free_mon/free_mon_network.h"
#include "mongo/db/free_mon/free_mon_processor.h"
#include "mongo/db/service_context.h"
#include "mongo/stdx/thread.h"
#include "mongo/util/duration.h"

namespace mongo {

/**
 * Manages and control Free Monitoring. This is the entry point for non-free monitoring components
 * into free-monitoring.
 */
class FreeMonController {
public:
    explicit FreeMonController(std::unique_ptr<FreeMonNetworkInterface> network,
                               bool useCrankForTest = false)
        : _network(std::move(network)), _useCrankForTest(useCrankForTest) {}

    /**
     * Initializes free monitoring.
     * Start free monitoring thread in the background.
     */
    void start(RegistrationType registrationType,
               std::vector<std::string>& tags,
               Seconds gatherMetricsInterval);

    /**
     * Stops free monitoring thread.
     */
    void stop();

    /**
     * Turn the crank of the message queue by ignoring deadlines for N messages.
     */
    void turnCrankForTest(size_t countMessagesToIgnore);

    /**
     * Deproritize the first message to force interleavings of messages.
     */
    void deprioritizeFirstMessageForTest(FreeMonMessageType type);

    /**
     * Add a metric collector to collect on registration
     */
    void addRegistrationCollector(std::unique_ptr<FreeMonCollectorInterface> collector);

    /**
     * Add a metric collector to collect periodically
     */
    void addMetricsCollector(std::unique_ptr<FreeMonCollectorInterface> collector);

    /**
     * Get the FreeMonController from ServiceContext.
     */
    static FreeMonController* get(ServiceContext* serviceContext);

    /**
     * Set the FreeMonController in the ServiceContext.
     */
    static void set(ServiceContext* serviceContext, std::unique_ptr<FreeMonController> controller);

    /**
     * Start registration of mongod with remote service.
     *
     * Only sends one remote registration at a time.
     * Returns after timeout if registrations is not complete. Registration continues though.
     */
    void registerServerStartup(RegistrationType registrationType, std::vector<std::string>& tags);

    /**
     * Start registration of mongod with remote service.
     *
     * Only sends one remote registration at a time.
     * Returns after timeout if registrations is not complete. Registration continues though.
     * Update is synchronous with 10sec timeout
     * kicks off register, and once register is done kicks off metrics upload
     */
    boost::optional<Status> registerServerCommand(Milliseconds timeout);

    /**
     * Stop registration of mongod with remote service.
     *
     * As with registerServerCommand() above, but undoes registration.
     * On complettion of this command, no further metrics will be transmitted.
     */
    boost::optional<Status> unregisterServerCommand(Milliseconds timeout);

    /**
     * Populates an info blob for use by {getFreeMonitoringStatus: 1}
     */
    void getStatus(OperationContext* opCtx, BSONObjBuilder* status);

    /**
     * Populates an info blob for use by {serverStatus: 1}
     */
    void getServerStatus(OperationContext* opCtx, BSONObjBuilder* status);

    /**
     * Notify on upsert.
     *
     * Updates and inserts are treated as the same.
     */
    void notifyOnUpsert(const BSONObj& doc);

    /**
     * Notify on document delete or drop collection.
     */
    void notifyOnDelete();

    /**
     * Notify that we local instance has become a primary.
     */
    void notifyOnTransitionToPrimary();

    /**
     * Notify that storage has rolled back
     */
    void notifyOnRollback();

private:
    void _enqueue(std::shared_ptr<FreeMonMessage> msg);

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

    // Controller state
    State _state{State::kNotStarted};

    // Mutext to protect internal state
    Mutex _mutex = MONGO_MAKE_LATCH("FreeMonController::_mutex");

    // Set of registration collectors
    FreeMonCollectorCollection _registrationCollectors;

    // Set of metric collectors
    FreeMonCollectorCollection _metricCollectors;

    // Network interface
    std::unique_ptr<FreeMonNetworkInterface> _network;

    // Background thead for agent
    stdx::thread _thread;

    // Crank for test
    bool _useCrankForTest;

    // Background agent
    std::shared_ptr<FreeMonProcessor> _processor;
};

}  // namespace mongo
