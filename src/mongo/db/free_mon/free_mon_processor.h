/**
 * Copyright (C) 2018 MongoDB Inc.
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

#include <boost/optional.hpp>
#include <cstdint>
#include <memory>
#include <ratio>
#include <string>
#include <vector>

#include "mongo/db/client.h"
#include "mongo/db/free_mon/free_mon_message.h"
#include "mongo/db/free_mon/free_mon_network.h"
#include "mongo/db/free_mon/free_mon_processor.h"
#include "mongo/db/free_mon/free_mon_protocol_gen.h"
#include "mongo/db/free_mon/free_mon_queue.h"
#include "mongo/db/free_mon/free_mon_storage_gen.h"
#include "mongo/db/ftdc/collector.h"
#include "mongo/db/service_context.h"
#include "mongo/util/clock_source.h"
#include "mongo/util/duration.h"
#include "mongo/util/future.h"
#include "mongo/util/time_support.h"

namespace mongo {
using FreeMonCollectorInterface = FTDCCollectorInterface;
using FreeMonCollectorCollection = FTDCCollectorCollection;

/**
 * Reponsible for tracking when to send the next retry after errors are encountered.
 */
class RetryCounter {
    const int64_t kMax = 60 * 60 * 24;

public:
    RetryCounter() : _min(1), _max(kMax) {}
    virtual ~RetryCounter() = default;

    /**
     * Set Minimum rety interval
     */
    void setMin(Seconds s) {
        _min = s;
        reset();
    }

    /**
     * Reset the retry interval, typically occurs after a succesfull message is sent.
     */
    virtual void reset() = 0;

    /**
     * Increment the error count and compute the next interval.
     */
    virtual bool incrementError() = 0;

    /**
     * Get the next retry duration.
     */
    Seconds getNextDuration() {
        dassert(_current != Seconds(0));
        return _current;
    }

    /**
     * Get the next retry deadline
     */
    Date_t getNextDeadline(Client* client) {
        return client->getServiceContext()->getPreciseClockSource()->now() + _current;
    }

protected:
    // Current retry interval
    Seconds _current;

    // Minimum retry interval
    Seconds _min;

    // Maximum retry interval
    Seconds _max;
};

/**
 * Manage retries for registrations
 */
class RegistrationRetryCounter : public RetryCounter {
public:
    explicit RegistrationRetryCounter(PseudoRandom& random) : _random(random) {}

    void reset() final;

    bool incrementError() final;

private:
    // Random number generator for jitter
    PseudoRandom& _random;

    // Retry count for stage 1 retry
    size_t _retryCount{0};

    // Total Seconds we have retried for
    Seconds _total;

    // Last retry interval without jitter
    Seconds _base;

    // Max Retry count
    const size_t kStage1RetryCountMax{10};

    const size_t kStage1JitterMin{2};
    const size_t kStage1JitterMax{10};

    const Hours kStage2DurationMax{48};

    const size_t kStage2JitterMin{60};
    const size_t kStage2JitterMax{120};
};

/**
 * Process in an Agent in a Agent/Message Passing model.
 *
 * Messages are given to it by enqueue, and the Processor processes messages with run().
 */
class FreeMonProcessor : public std::enable_shared_from_this<FreeMonProcessor> {
public:
    FreeMonProcessor(FreeMonCollectorCollection& registration,
                     FreeMonCollectorCollection& metrics,
                     FreeMonNetworkInterface* network)
        : _registration(registration),
          _metrics(metrics),
          _network(network),
          _random(Date_t::now().asInt64()),
          _registrationRetry(_random) {
        _registrationRetry.reset();
    }

    /**
     * Enqueue a message to process
     */
    void enqueue(std::shared_ptr<FreeMonMessage> msg);

    /**
     * Stop processing messages.
     */
    void stop();

    /**
     * Processes messages forever
     */
    void run();

    /**
     * Validate the registration response. Public for unit testing.
     */
    static Status validateRegistrationResponse(const FreeMonRegistrationResponse& resp);

private:
    /**
     * Read the state from the database.
     */
    void readState(Client* client);

    /**
     * Write the state to disk if there are any changes.
     */
    void writeState(Client* client);

    /**
     * Process a registration from a command.
     */
    void doCommandRegister(Client* client, std::shared_ptr<FreeMonMessage> sharedMsg);

    /**
     * Process a registration from configuration.
     */
    void doServerRegister(Client* client,
                          const FreeMonMessageWithPayload<FreeMonMessageType::RegisterServer>* msg);

    /**
     * Process unregistration.
     */
    void doUnregister(Client* client);

    /**
     * Process a successful HTTP request.
     */
    void doAsyncRegisterComplete(
        Client* client,
        const FreeMonMessageWithPayload<FreeMonMessageType::AsyncRegisterComplete>* msg);

    /**
     * Process an unsuccessful HTTP request.
     */
    void doAsyncRegisterFail(
        Client* client,
        const FreeMonMessageWithPayload<FreeMonMessageType::AsyncRegisterFail>* msg);

    /**
     * Notify any command registers that are waiting.
     */
    void notifyPendingRegisters(const Status s);

private:
    // Collection of collectors to send on registration
    FreeMonCollectorCollection& _registration;

    // Collection of collectors to send on each metrics call
    FreeMonCollectorCollection& _metrics;

    // HTTP Network interface
    FreeMonNetworkInterface* _network;

    // Random number generator for retries
    PseudoRandom _random;

    // Registration Retry logic
    RegistrationRetryCounter _registrationRetry;

    // List of tags from server configuration registration
    std::vector<std::string> _tags;

    // In-flight registration response
    std::unique_ptr<Future<void>> _futureRegistrationResponse;

    // List of command registers waiting to be told about registration
    std::vector<std::shared_ptr<FreeMonMessage>> _pendingRegisters;

    // Last read storage state
    boost::optional<FreeMonStorageState> _lastReadState;

    // Pending update to disk
    FreeMonStorageState _state;

    // Message queue
    FreeMonMessageQueue _queue;
};

}  // namespace mongo
