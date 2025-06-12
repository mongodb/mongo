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
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/baton.h"
#include "mongo/executor/network_connection_hook.h"
#include "mongo/executor/network_interface.h"
#include "mongo/executor/remote_command_request.h"
#include "mongo/executor/remote_command_response.h"
#include "mongo/executor/task_executor.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/rpc/metadata/metadata_hook.h"
#include "mongo/stdx/condition_variable.h"
#include "mongo/stdx/mutex.h"
#include "mongo/stdx/unordered_map.h"
#include "mongo/stdx/unordered_set.h"
#include "mongo/transport/transport_layer.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/cancellation.h"
#include "mongo/util/clock_source.h"
#include "mongo/util/clock_source_mock.h"
#include "mongo/util/duration.h"
#include "mongo/util/functional.h"
#include "mongo/util/future.h"
#include "mongo/util/net/hostandport.h"
#include "mongo/util/time_support.h"

#include <cstddef>
#include <functional>
#include <list>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <utility>
#include <vector>

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {

class BSONObj;

namespace executor {

class NetworkConnectionHook;

/**
 * Mock network implementation for use in unit tests.
 *
 * To use, construct a new instance on the heap, and keep a pointer to it.  Pass
 * the pointer to the instance into the TaskExecutor constructor, transferring
 * ownership.  Start the executor's run() method in a separate thread, schedule the
 * work you want to test into the executor, then while the test is still going, iterate
 * through the ready network requests, servicing them and advancing time as needed.
 *
 * The mock has a fully virtualized notion of time and the the network.  When the
 * executor under test schedules a network operation, the startCommand
 * method of this class adds an entry to the _operations queue for immediate consideration.
 * The test driver loop, when it examines the request, may schedule a response, ask the
 * interface to redeliver the request at a later virtual time, or to swallow the virtual
 * request until the end of the simulation.  The test driver loop can also instruct the
 * interface to run forward through virtual time until there are operations ready to
 * consider, via runUntil.
 *
 * The thread acting as the "network" and the executor run thread are highly synchronized
 * by this code, allowing for deterministic control of operation interleaving.
 *
 * There are three descriptors used in the NetworkInterfaceMock for an operation:
 *  1) A "ready request" refers to an operation that has been enqueued in the _operations
 * queue, but does not have a corresponding response associated with it yet.
 *
 *  2) A "ready network operation" refers to an operation that has been enqueued in the _operation
 * queue and has an associated response in the _responses queue. That response has not yet been
 * processed by the network thread, and so the request is not yet complete.
 *
 * 3) An "unfinished network operation" refers to an operation that has not been finished. It may
 * be waiting for a response to become available, or for the network thread to move forward and
 * process that response.
 */
class NetworkInterfaceMock : public NetworkInterface {
public:
    class NetworkOperation;
    using NetworkOperationList = std::list<NetworkOperation>;
    using NetworkOperationIterator = NetworkOperationList::iterator;

    /**
     * This struct encapsulates the original Request as well as response data and metadata.
     */
    struct NetworkResponse {
        NetworkOperationIterator noi;
        Date_t when;
        TaskExecutor::ResponseStatus response;
    };
    using NetworkResponseList = std::list<NetworkResponse>;

    NetworkInterfaceMock();
    ~NetworkInterfaceMock() override;

    ////////////////////////////////////////////////////////////////////////////////
    //
    // NetworkInterface methods
    //
    ////////////////////////////////////////////////////////////////////////////////

    void appendConnectionStats(ConnectionPoolStats* stats) const override {}
    void appendStats(BSONObjBuilder&) const override {}

    std::string getDiagnosticString() override;

    Counters getCounters() const override {
        return _counters;
    }

    void startup() override;
    void shutdown() override;
    bool inShutdown() const override;
    void waitForWork() override;
    void waitForWorkUntil(Date_t when) override;
    void signalWorkAvailable() override;
    Date_t now() override;
    std::string getHostName() override;
    SemiFuture<TaskExecutor::ResponseStatus> startCommand(
        const TaskExecutor::CallbackHandle& cbHandle,
        RemoteCommandRequest& request,
        const BatonHandle& baton = nullptr,
        const CancellationToken& token = CancellationToken::uncancelable()) override;

    class ExhaustResponseReaderMock : public NetworkInterface::ExhaustResponseReader {
    public:
        ExhaustResponseReaderMock(NetworkInterfaceMock* interface,
                                  TaskExecutor::CallbackHandle cbHandle,
                                  RemoteCommandRequest request,
                                  std::shared_ptr<Baton> baton,
                                  const CancellationToken& token)
            : _interface(interface),
              _cbHandle(cbHandle),
              _initialRequest(request),
              _baton(baton),
              _cancelSource(token) {}

        SemiFuture<RemoteCommandResponse> next() override;

    private:
        enum class State { kInitialRequest, kExhaust, kDone };

        NetworkInterfaceMock* _interface;
        TaskExecutor::CallbackHandle _cbHandle;
        RemoteCommandRequest _initialRequest;
        std::shared_ptr<Baton> _baton;
        CancellationSource _cancelSource;
        State _state{State::kInitialRequest};
    };

    SemiFuture<std::shared_ptr<NetworkInterface::ExhaustResponseReader>> startExhaustCommand(
        const TaskExecutor::CallbackHandle& cbHandle,
        RemoteCommandRequest& request,
        const BatonHandle& baton = nullptr,
        const CancellationToken& = CancellationToken::uncancelable()) override;

    /**
     * Cancels the token associated with the passed in callback handle.
     */
    void cancelCommand(const TaskExecutor::CallbackHandle& cbHandle,
                       const BatonHandle& baton = nullptr) override;

    SemiFuture<void> setAlarm(
        Date_t when, const CancellationToken& token = CancellationToken::uncancelable()) override;

    Status schedule(unique_function<void(Status)> action) override;

    bool onNetworkThread() override;

    void dropConnections(const HostAndPort& target, const Status& status) override {}

    void testEgress(const HostAndPort&, transport::ConnectSSLMode, Milliseconds, Status) override {}

    using LeasedStreamMaker =
        std::function<std::unique_ptr<NetworkInterface::LeasedStream>(HostAndPort)>;
    void setLeasedStreamMaker(LeasedStreamMaker lsm) {
        _leasedStreamMaker = std::move(lsm);
    }

    SemiFuture<std::unique_ptr<NetworkInterface::LeasedStream>> leaseStream(
        const HostAndPort& hp, transport::ConnectSSLMode, Milliseconds) override {
        invariant(_leasedStreamMaker,
                  "Tried to lease a stream from NetworkInterfaceMock without providing one");
        return (*_leasedStreamMaker)(hp);
    }

    ////////////////////////////////////////////////////////////////////////////////
    //
    // Methods for simulating network operations and the passage of time.
    //
    // Methods in this section are to be called by the thread currently simulating
    // the network.
    //
    ////////////////////////////////////////////////////////////////////////////////

    /**
     * RAII-style class for entering and exiting network.
     */
    class InNetworkGuard;

    void setConnectionHook(std::unique_ptr<NetworkConnectionHook> hook);

    void setEgressMetadataHook(std::unique_ptr<rpc::EgressMetadataHook> metadataHook);

    /**
     * Causes the currently running (non-executor) thread to assume the mantle of the network
     * simulation thread.
     *
     * Call this before calling any of the other methods in this section.
     */
    void enterNetwork();

    /**
     * Causes the currently running thread to drop the mantle of "network simulation thread".
     *
     * Call this before calling any methods that might block waiting for the
     * executor thread.
     *
     * It is safe to call exitNetwork() even if enterNetwork() has not been called - it will just
     * be a no-op.
     */
    void exitNetwork();

    /**
     * Returns true if there are requests that do not yet have responses in the _responses queue
     * associated with them.
     *
     * This will not notice exhaust operations that have not yet finished but have processed all of
     * their available responses.
     */
    bool hasReadyRequests();

    /**
     * Returns the current number of requests that do not yet have responses in the _responses queue
     * associated with them.
     */
    size_t getNumReadyRequests();

    /**
     * Returns true if the given iterator points to the end of the network operation list.
     */
    bool isNetworkOperationIteratorAtEnd(const NetworkInterfaceMock::NetworkOperationIterator& itr);

    /**
     * Gets the next request that does have an associated response, blocking until one is available.
     *
     * It will also process any ready network operations while it is blocking.
     *
     * Will not return until the executor thread is blocked in waitForWorkUntil or waitForWork.
     */
    NetworkOperationIterator getNextReadyRequest();

    /**
     * Gets the first request without an associated response. There must be at least one such
     * request in the queue. Equivalent to getNthReadyRequest(0).
     */
    NetworkOperationIterator getFrontOfReadyQueue();

    /**
     * Get the nth (starting at 0) request without an associated response. Assumes there are at
     * least n+1 such requests in the queue.
     */
    NetworkOperationIterator getNthReadyRequest(size_t n);

    /**
     * Schedules "response" in response to "noi" at virtual time "when".
     */
    void scheduleResponse(NetworkOperationIterator noi,
                          Date_t when,
                          const TaskExecutor::ResponseStatus& response);

    /**
     * Schedules a successful "response" to "noi" at virtual time "when".
     * "noi" defaults to next ready request.
     * "when" defaults to now().
     * Returns the "request" that the response was scheduled for.
     */
    RemoteCommandRequest scheduleSuccessfulResponse(const BSONObj& response);
    RemoteCommandRequest scheduleSuccessfulResponse(const RemoteCommandResponse& response);
    RemoteCommandRequest scheduleSuccessfulResponse(NetworkOperationIterator noi,
                                                    const RemoteCommandResponse& response);
    RemoteCommandRequest scheduleSuccessfulResponse(NetworkOperationIterator noi,
                                                    Date_t when,
                                                    const RemoteCommandResponse& response);

    /**
     * Schedules an error "response" to "noi" at virtual time "when".
     * "noi" defaults to next ready request.
     * "when" defaults to now().
     */
    RemoteCommandRequest scheduleErrorResponse(const Status& response);
    RemoteCommandRequest scheduleErrorResponse(TaskExecutor::ResponseStatus response);
    RemoteCommandRequest scheduleErrorResponse(NetworkOperationIterator noi,
                                               const Status& response);
    RemoteCommandRequest scheduleErrorResponse(NetworkOperationIterator noi,
                                               Date_t when,
                                               const Status& response);

    /**
     * Swallows "noi", causing the network interface to not respond to it until
     * shutdown() is called.
     */
    void blackHole(NetworkOperationIterator noi);

    /**
     * Runs the simulator forward until now() == until or hasReadyRequests() is true.
     * Returns now().
     *
     * Will not return until the executor thread is blocked in waitForWorkUntil or waitForWork.
     */
    Date_t runUntil(Date_t until);

    /**
     * Runs the simulator forward until now() == until.
     */
    void advanceTime(Date_t newTime);

    /**
     * Processes all ready network operations (meaning requests with associated responses in the
     * _responses queue).
     *
     * The caller must have assumed the role of the network thread through enterNetwork.
     *
     * Will not return until the executor thread is blocked in waitForWorkUntil or waitForWork.
     */
    void runReadyNetworkOperations();

    /**
     * Blocks until all requests have been marked as _isFinished, and processes responses as they
     * become available.
     *
     * The caller must have assumed the role of the network thread through enterNetwork.
     *
     * Other threads must schedule responses for or cancel outstanding requests to unblock this
     * function.
     */
    void drainUnfinishedNetworkOperations();

    /**
     * Sets the reply of the 'hello' handshake for a specific host. This reply will only
     * be given to the 'validateHost' method of the ConnectionHook set on this object - NOT
     * to the completion handlers of any 'hello' commands scheduled with 'startCommand'.
     *
     * This reply will persist until it is changed again using this method.
     *
     * If the NetworkInterfaceMock conducts a handshake with a simulated host which has not
     * had a handshake reply set, a default constructed RemoteCommandResponse will be passed
     * to validateHost if a hook is set.
     */
    void setHandshakeReplyForHost(const HostAndPort& host, RemoteCommandResponse&& reply);

    /**
     * Returns false if there is no scheduled work (i.e. alarms and scheduled responses) for the
     * network thread to process.
     */
    bool hasReadyNetworkOperations();

    size_t getNumResponses() {
        return _responses.size();
    }

    void setOnCancelAction(std::function<void()> cb) {
        _onCancelAction = std::move(cb);
    }

private:
    /**
     * Information describing a scheduled alarm.
     */
    struct AlarmInfo {
        AlarmInfo(std::uint64_t inId, Date_t inWhen, Promise<void> inPromise)
            : id(inId), when(inWhen), promise(std::move(inPromise)) {}
        bool operator>(const AlarmInfo& rhs) const {
            return when > rhs.when;
        }

        void cancel();

        std::uint64_t id;
        Date_t when;
        Promise<void> promise;
    };

    /**
     * Type used to identify which thread (network mock or executor) is currently executing.
     *
     * Values are used in a bitmask, as well.
     */
    enum ThreadType { kNoThread = 0, kExecutorThread = 1, kNetworkThread = 2 };

    /**
     * Implementation of startup behavior.
     */
    void _startup_inlock(stdx::unique_lock<stdx::mutex>& lk);

    /**
     * Returns the current virtualized time.
     */
    Date_t _now_inlock(stdx::unique_lock<stdx::mutex>& lk) const {
        return _clkSource->now();
    }

    /**
     * Implementation of waitForWork*.
     */
    void _waitForWork_inlock(stdx::unique_lock<stdx::mutex>& lk);

    /**
     * Returns the current number of ready requests.
     */
    size_t _getNumReadyRequests_inlock(stdx::unique_lock<stdx::mutex>& lk);

    /**
     * Returns true if the network thread could run right now.
     */
    bool _isNetworkThreadRunnable_inlock(stdx::unique_lock<stdx::mutex>& lk);

    /**
     * Returns true if the executor thread could run right now.
     */
    bool _isExecutorThreadRunnable_inlock(stdx::unique_lock<stdx::mutex>& lk);

    /**
     * Enqueues a network operation to run in order of 'consideration date'.
     */
    void _enqueueOperation_inlock(stdx::unique_lock<stdx::mutex>& lk, NetworkOperation&& op);

    /**
     * "Connects" to a remote host, and then enqueues the provided operation.
     */
    void _connectThenEnqueueOperation_inlock(stdx::unique_lock<stdx::mutex>& lk,
                                             const HostAndPort& target,
                                             NetworkOperation&& op);

    /**
     * Enqueues a response to be processed the next time we runReadyNetworkOperations.
     *
     * Note that interruption and timeout also invoke this function.
     */
    void _scheduleResponse_inlock(stdx::unique_lock<stdx::mutex>& lk,
                                  NetworkOperationIterator noi,
                                  Date_t when,
                                  const TaskExecutor::ResponseStatus& response);

    /**
     * Deliver the response to the callback handle if the handle is present.
     * This represents interrupting the regular flow with, for example, a NetworkTimeout or
     * CallbackCanceled error.
     */
    void _interruptWithResponse_inlock(stdx::unique_lock<stdx::mutex>& lk,
                                       const TaskExecutor::CallbackHandle& cbHandle,
                                       const TaskExecutor::ResponseStatus& response);

    /**
     * Runs all ready network operations, called while holding "lk".  May drop and
     * reaquire "lk" several times, but will not return until the executor has blocked
     * in waitFor*.
     */
    void _runReadyNetworkOperations_inlock(stdx::unique_lock<stdx::mutex>& lk);

    /**
     * Returns true if there are operations that have not yet been marked as isFinished().
     */
    bool _hasUnfinishedNetworkOperations();

    SemiFuture<TaskExecutor::ResponseStatus> _startOperation(
        const TaskExecutor::CallbackHandle& cbHandle,
        RemoteCommandRequest& request,
        bool awaitExhaust,
        const BatonHandle& baton = nullptr,
        const CancellationToken& token = CancellationToken::uncancelable());

    /**
     * Returns an iterator pointing to the first unfinished NetworkOperation associated with the
     * provided cbHandle. Returns _operations->end() if no such operation exists.
     */
    NetworkOperationIterator _getNetworkOperation_inlock(
        WithLock, const TaskExecutor::CallbackHandle& cbHandle);

    // Mutex that synchronizes access to mutable data in this class and its subclasses.
    // Fields guarded by the mutex are labled (M), below, and those that are read-only
    // in multi-threaded execution, and so unsynchronized, are labeled (R).
    stdx::mutex _mutex;

    std::function<void()> _onCancelAction;

    // A mocked clock source.
    std::unique_ptr<ClockSourceMock> _clkSource;  // (M)

    // Condition signaled to indicate that the network processing thread should wake up.
    stdx::condition_variable _shouldWakeNetworkCondition;  // (M)

    // Condition signaled to indicate that the executor run thread should wake up.
    stdx::condition_variable _shouldWakeExecutorCondition;  // (M)

    // Bitmask indicating which threads are runnable.
    int _waitingToRunMask;  // (M)

    // Indicator of which thread, if any, is currently running.
    ThreadType _currentlyRunning;  // (M)

    // Set to true by "startUp()"
    bool _hasStarted;  // (M)

    // Set to true by "shutDown()".
    AtomicWord<bool> _inShutdown;  // (M)

    // Next date that the executor expects to wake up at (due to a scheduleWorkAt() call).
    Date_t _executorNextWakeupDate;  // (M)

    // The list of operations that have been submitted via startCommand. Operations are never
    // deleted from this list, thus NetworkOperationIterators are valid for the lifetime of the
    // NetworkInterfaceMock.
    NetworkOperationList _operations;  // (M)

    // The list of responses that have been enqueued from scheduleResponse(), cancellation, or
    // timeout. This list is ordered by NetworkResponse::when and is drained front to back by
    // runReadyNetworkOperations().
    NetworkResponseList _responses;  // (M)

    // Next alarm ID
    std::uint64_t _nextAlarmId{0};  // (M)

    // Sorted map of target to alarms, with the next alarm always first.
    std::multimap<Date_t, AlarmInfo> _alarms;                              // (M)
    stdx::unordered_map<size_t, decltype(_alarms)::iterator> _alarmsById;  // (M)
    stdx::unordered_set<size_t> _canceledAlarms;                           // (M)

    // The connection hook.
    std::unique_ptr<NetworkConnectionHook> _hook;  // (R)

    // The metadata hook.
    std::unique_ptr<rpc::EgressMetadataHook> _metadataHook;  // (R)

    // The set of hosts we have seen so far. If we see a new host, we will execute the
    // ConnectionHook's validation and post-connection logic.
    //
    // TODO: provide a way to simulate disconnections.
    stdx::unordered_set<HostAndPort> _connections;  // (M)

    // The handshake replies set for each host.
    stdx::unordered_map<HostAndPort, RemoteCommandResponse> _handshakeReplies;  // (M)

    // Track statistics about processed responses. Right now, the mock tracks the total responses
    // processed with sent, the number of OK responses with succeeded, non-OK responses with failed,
    // and cancellation errors with canceled. It does not track the timedOut or failedRemotely
    // statistics.
    Counters _counters;

    boost::optional<LeasedStreamMaker> _leasedStreamMaker;
};

/**
 * Representation of an in-progress network operation.
 */
class NetworkInterfaceMock::NetworkOperation {
    using ResponseCallback = unique_function<void(const TaskExecutor::ResponseStatus&)>;

public:
    NetworkOperation();
    NetworkOperation(const TaskExecutor::CallbackHandle& cbHandle,
                     const RemoteCommandRequest& theRequest,
                     Date_t theRequestDate,
                     const CancellationToken& token,
                     Promise<TaskExecutor::ResponseStatus> promise);

    /**
     * Mark the operation as observed by the networking thread. This is equivalent to a remote node
     * processing the operation.
     */
    void markAsProcessing() {
        _isProcessing = true;
    }

    /**
     * Mark the operation as blackholed by the networking thread.
     */
    void markAsBlackholed() {
        _isProcessing = true;
        _isBlackholed = true;
    }

    /**
     * Fulfills a response to an ongoing operation.
     */
    bool fulfillResponse_inlock(stdx::unique_lock<stdx::mutex>& lk, NetworkResponse response);

    /**
     * Predicate that returns true if cbHandle equals the executor's handle for this network
     * operation.  Used for searching lists of NetworkOperations.
     */
    bool isForCallback(const TaskExecutor::CallbackHandle& cbHandle) const {
        return cbHandle == _cbHandle;
    }

    const TaskExecutor::CallbackHandle& getCallbackHandle() const {
        return _cbHandle;
    }

    /**
     * Gets the request that initiated this operation.
     */
    const RemoteCommandRequest& getRequest() const {
        return _request;
    }

    CancellationSource& getCancellationSource() {
        return _cancelSource;
    }

    /**
     * Returns true if this operation has not been observed via getNextReadyRequest(), been
     * canceled, or timed out.
     */
    bool hasReadyRequest() const {
        return !_isProcessing && !_isFinished;
    }

    bool isFinished() const {
        return _isFinished;
    }

    /**
     * Assert that this operation has not been blackholed.
     */
    void assertNotBlackholed() {
        uassert(5440603, "Response scheduled for a blackholed operation", !_isBlackholed);
    }

    /**
     * Gets the virtual time at which the operation was started.
     */
    Date_t getRequestDate() const {
        return _requestDate;
    }

    /**
     * Returns a printable diagnostic string.
     */
    std::string getDiagnosticString() const;

private:
    Date_t _requestDate;
    TaskExecutor::CallbackHandle _cbHandle;
    RemoteCommandRequest _request;
    CancellationSource _cancelSource;

    bool _isProcessing = false;
    bool _isBlackholed = false;
    bool _isFinished = false;

    Promise<TaskExecutor::ResponseStatus> _respPromise;
};

/**
 * RAII type to enter and exit network on construction/destruction.
 *
 * Calls enterNetwork on construction, and exitNetwork during destruction,
 * unless dismissed.
 *
 * Not thread-safe.
 */
class NetworkInterfaceMock::InNetworkGuard {
    InNetworkGuard(const InNetworkGuard&) = delete;
    InNetworkGuard& operator=(const InNetworkGuard&) = delete;

public:
    /**
     * Calls enterNetwork.
     */
    explicit InNetworkGuard(NetworkInterfaceMock* net);
    /**
     * Calls exitNetwork, and disables the destructor from calling.
     */
    void dismiss();
    /**
     * Calls exitNetwork, unless dismiss has been called.
     */
    ~InNetworkGuard();

    /**
     * Returns network interface mock pointer.
     */
    NetworkInterfaceMock* operator->() const;

private:
    NetworkInterfaceMock* _net;
    bool _callExitNetwork = true;
};

}  // namespace executor
}  // namespace mongo
