
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

#include <boost/optional.hpp>
#include <cstdint>
#include <deque>
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
#include "mongo/util/synchronized_value.h"
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
    Seconds getNextDuration() const {
        dassert(_current != Seconds(0));
        return _current;
    }

    /**
     * Get the next retry deadline
     */
    Date_t getNextDeadline(Client* client) const {
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

    size_t getCount() const {
        return _retryCount;
    }

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
 * Manage retries for metrics
 */
class MetricsRetryCounter : public RetryCounter {
public:
    explicit MetricsRetryCounter(PseudoRandom& random) : _random(random) {}

    void reset() final;

    bool incrementError() final;

    size_t getCount() const {
        return _retryCount;
    }

private:
    // Random number generator for jitter
    PseudoRandom& _random;

    // Retry count for stage 1 retry
    size_t _retryCount{0};

    // Total Seconds we have retried for
    Seconds _total;

    // Last retry interval without jitter
    Seconds _base;

    // Max Duration
    const Hours kDurationMax{7 * 24};
};

/**
 * Simple bounded buffer of metrics to upload.
 */
class MetricsBuffer {
public:
    using container_type = std::deque<BSONObj>;

    /**
     * Add a metric to the buffer. Oldest metric will be discarded if buffer is at capacity.
     */
    void push(BSONObj obj) {
        if (_queue.size() == kMaxElements) {
            _queue.pop_front();
        }

        _queue.push_back(obj);
    }

    /**
     * Flush the buffer down to kMinElements entries. The last entries are held for cloud.
     */
    void reset() {
        while (_queue.size() > kMinElements) {
            _queue.pop_front();
        }
    }

    container_type::iterator begin() {
        return _queue.begin();
    }
    container_type::iterator end() {
        return _queue.end();
    }

private:
    // Bounded queue of metrics
    container_type _queue;

    const size_t kMinElements = 1;
    const size_t kMaxElements = 10;
};

/**
 * Countdown latch for test support in FreeMonProcessor so that a crank can be turned manually.
 */
class FreeMonCountdownLatch {
public:
    explicit FreeMonCountdownLatch() : _count(0) {}

    /**
     * Reset countdown latch wait for N events.
     */
    void reset(uint32_t count) {
        stdx::lock_guard<stdx::mutex> lock(_mutex);
        dassert(_count == 0);
        dassert(count > 0);
        _count = count;
    }

    /**
     * Count down an event.
     */
    void countDown() {
        stdx::lock_guard<stdx::mutex> lock(_mutex);

        if (_count > 0) {
            --_count;
            if (_count == 0) {
                _condvar.notify_one();
            }
        }
    }

    /**
     * Wait until the N events specified in reset have occured.
     */
    void wait() {
        stdx::unique_lock<stdx::mutex> lock(_mutex);
        _condvar.wait(lock, [&] { return _count == 0; });
    }

private:
    // mutex to break count and cond var
    stdx::mutex _mutex;

    // cond var to signal and wait on
    stdx::condition_variable _condvar;

    // count of events to wait for
    size_t _count;
};

/**
 * In-memory registration status
 *
 * Ensures primaries and secondaries register separately
 */
enum class FreeMonRegistrationStatus {
    /**
     * Free monitoring is not enabled - default state.
     */
    kDisabled,

    /**
     * Registration in progress.
     */
    kPending,

    /**
     * Free Monitoring is enabled.
     */
    kEnabled,
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
                     FreeMonNetworkInterface* network,
                     bool useCrankForTest,
                     Seconds metricsGatherInterval);

    /**
     * Enqueue a message to process
     */
    void enqueue(std::shared_ptr<FreeMonMessage> msg);

    /**
     * Stop processing messages.
     */
    void stop();

    /**
     * Turn the crank of the message queue by ignoring deadlines for N messages.
     */
    void turnCrankForTest(size_t countMessagesToIgnore);

    /**
     * Processes messages forever
     */
    void run();

    /**
     * Validate the registration response. Public for unit testing.
     */
    static Status validateRegistrationResponse(const FreeMonRegistrationResponse& resp);

    /**
     * Validate the metrics response. Public for unit testing.
     */
    static Status validateMetricsResponse(const FreeMonMetricsResponse& resp);

private:
    /**
     * Read the state from the database.
     */
    void readState(OperationContext* opCtx);

    /**
     * Create a short-lived opCtx and read the state from the database.
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
     * Process unregistration from a command.
     */
    void doCommandUnregister(
        Client* client,
        FreeMonWaitableMessageWithPayload<FreeMonMessageType::UnregisterCommand>* msg);

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

    /**
     * Upload collected metrics.
     */
    void doMetricsCollect(Client* client);

    /**
     * Upload gathered metrics.
     */
    void doMetricsSend(Client* client);

    /**
     * Process a successful HTTP request.
     */
    void doAsyncMetricsComplete(
        Client* client,
        const FreeMonMessageWithPayload<FreeMonMessageType::AsyncMetricsComplete>* msg);

    /**
     * Process an unsuccessful HTTP request.
     */
    void doAsyncMetricsFail(
        Client* client, const FreeMonMessageWithPayload<FreeMonMessageType::AsyncMetricsFail>* msg);

    /**
     * Process a change to become a replica set primary
     */
    void doOnTransitionToPrimary(Client* client);

    /**
     * Process a notification that storage has received insert or update.
     */
    void doNotifyOnUpsert(Client* client,
                          const FreeMonMessageWithPayload<FreeMonMessageType::NotifyOnUpsert>* msg);

    /**
     * Process a notification that storage has received delete or drop collection.
     */
    void doNotifyOnDelete(Client* client);


    /**
     * Process a notification that storage has rolled back.
     */
    void doNotifyOnRollback(Client* client);

    /**
     * Process a in-memory state transition of state.
     */
    void processInMemoryStateChange(const FreeMonStorageState& originalState,
                                    const FreeMonStorageState& newState);

protected:
    friend class FreeMonController;

    enum FreeMonGetStatusEnum {
        kServerStatus,
        kCommandStatus,
    };

    /**
     *  Populate results for getFreeMonitoringStatus or serverStatus commands.
     */
    void getStatus(OperationContext* opCtx, BSONObjBuilder* status, FreeMonGetStatusEnum mode);

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
    synchronized_value<RegistrationRetryCounter> _registrationRetry;

    // Metrics Retry logic
    synchronized_value<MetricsRetryCounter> _metricsRetry;

    // Interval for gathering metrics
    Seconds _metricsGatherInterval;

    // Buffer of metrics to upload
    MetricsBuffer _metricsBuffer;

    // When did we last send a metrics batch?
    synchronized_value<boost::optional<Date_t>> _lastMetricsSend;

    // List of tags from server configuration registration
    std::vector<std::string> _tags;

    // In-flight registration response
    std::unique_ptr<Future<void>> _futureRegistrationResponse;

    // List of command registers waiting to be told about registration
    std::vector<std::shared_ptr<FreeMonMessage>> _pendingRegisters;

    // Last read storage state
    synchronized_value<boost::optional<FreeMonStorageState>> _lastReadState;

    // When we change to primary, do we register?
    RegistrationType _registerOnTransitionToPrimary{RegistrationType::DoNotRegister};

    // Pending update to disk
    synchronized_value<FreeMonStorageState> _state;

    // In-memory registration status
    FreeMonRegistrationStatus _registrationStatus{FreeMonRegistrationStatus::kDisabled};

    // Countdown launch to support manual cranking
    FreeMonCountdownLatch _countdown;

    // Message queue
    FreeMonMessageQueue _queue;
};

}  // namespace mongo
