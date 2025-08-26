/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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

#include <absl/container/node_hash_map.h>
#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr.hpp>
// IWYU pragma: no_include "cxxabi.h"
#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/baton.h"
#include "mongo/db/database_name.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/server_options.h"
#include "mongo/executor/async_rpc.h"
#include "mongo/executor/async_rpc_error_info.h"
#include "mongo/executor/async_rpc_targeter.h"
#include "mongo/executor/remote_command_response.h"
#include "mongo/executor/task_executor.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/stdx/condition_variable.h"
#include "mongo/stdx/mutex.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/cancellation.h"
#include "mongo/util/duration.h"
#include "mongo/util/future.h"
#include "mongo/util/future_impl.h"
#include "mongo/util/future_util.h"
#include "mongo/util/net/hostandport.h"
#include "mongo/util/producer_consumer_queue.h"
#include "mongo/util/string_map.h"
#include "mongo/util/uuid.h"

#include <algorithm>
#include <deque>
#include <functional>
#include <iosfwd>
#include <memory>
#include <mutex>
#include <string>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

namespace mongo::async_rpc {

/**
 * This header provides two mock implementations of the async_rpc::sendCommand API. In
 * unit tests, swap out the AsyncRPCRunner decoration on the ServiceContext with an instance
 * of one of these mocks to inject your own behavior and check invariants.
 *
 *  See the header comments for each mock for details.
 */

/**
 * The SyncMockAsyncRPCRunner's representation of a RPC request. Exposed to users of the mock
 * via the onCommand/getNextRequest APIs, which can be used to examine what requests have been
 * scheduled on the mock and to schedule responsess to them.
 */
class RequestInfo {
public:
    RequestInfo(BSONObj cmd, StringData dbName, HostAndPort target, Promise<BSONObj>&& promise)
        : _cmd{cmd}, _dbName{dbName}, _target{target}, _responsePromise{std::move(promise)} {}

    bool isFinished() const {
        return _finished.load();
    }

    /**
     * Use this function to respond to the request. Errors in the 'Status' portion of the
     * 'StatusWith' correspond to 'local errors' (i.e. those on the sending node), while the BSONObj
     * corresponds to the raw BSON received as a response to the request over-the-wire.
     */
    void respondWith(StatusWith<BSONObj> response) {
        bool expected = false;
        if (_finished.compareAndSwap(&expected, true))
            _responsePromise.setFrom(response);
    }

    BSONObj _cmd;
    StringData _dbName;
    HostAndPort _target;

private:
    AtomicWord<bool> _finished{false};
    Promise<BSONObj> _responsePromise;
};

/**
 * The SyncMockAsyncRPCRunner is a mock implementation that can be interacted with in a
 * synchronous/ blocking manner. It allows you to:
 *      -> Synchronously introspect onto what requests have been scheduled.
 *      -> Synchronously respond to those requests.
 *      -> Wait (blocking) for a request to be scheduled and respond to it.
 * See the member funtions below for details.
 */
class SyncMockAsyncRPCRunner : public detail::AsyncRPCRunner {
public:
    /**
     * Mock implementation of the core functionality of the RCR. Records the provided request, and
     * notifies waiters that a new request has been scheduled.
     */
    ExecutorFuture<detail::AsyncRPCInternalResponse> _sendCommand(
        std::shared_ptr<TaskExecutor> exec,
        CancellationToken token,
        OperationContext* opCtx,
        Targeter* targeter,
        const DatabaseName& dbName,
        BSONObj cmdBSON,
        BatonHandle,
        boost::optional<UUID> clientOperationKey) final {
        auto [p, f] = makePromiseFuture<BSONObj>();
        return targeter->resolve(token)
            .thenRunOn(exec)
            .onError([](Status s) -> StatusWith<std::vector<HostAndPort>> {
                return Status{AsyncRPCErrorInfo(s), "Remote command execution failed"};
            })
            .then([=, this, f = std::move(f), p = std::move(p), dbName = dbName.toString_forTest()](
                      auto&& targets) mutable {
                stdx::lock_guard lg{_m};
                _requests.emplace_back(cmdBSON, dbName, targets[0], std::move(p));
                _hasRequestsCV.notify_one();
                return std::move(f).onCompletion(
                    [targetUsed = targets[0]](StatusWith<BSONObj> resp) {
                        if (!resp.isOK()) {
                            uassertStatusOK(Status{AsyncRPCErrorInfo(resp.getStatus(), targetUsed),
                                                   "Remote command execution failed"});
                        }
                        Status maybeError(detail::makeErrorIfNeeded(executor::RemoteCommandResponse(
                            targetUsed, resp.getValue(), Microseconds(1))));
                        uassertStatusOK(maybeError);
                        return detail::AsyncRPCInternalResponse{resp.getValue(), targetUsed};
                    });
            });
    }

    /**
     * Gets the oldest request the RCR has recieved that has not yet been responded to. Blocking
     * call - if no unresponded requests are currently queued in the mock, this call blocks until
     * one arrives.
     */
    RequestInfo& getNextRequest() {
        stdx::unique_lock lk(_m);
        auto pred = [this] {
            auto nextUnprocessedRequest =
                std::find_if(_requests.begin(), _requests.end(), [](const RequestInfo& r) {
                    return !r.isFinished();
                });
            return nextUnprocessedRequest != _requests.end();
        };
        _hasRequestsCV.wait(lk, pred);
        return *std::find_if(_requests.begin(), _requests.end(), [](const RequestInfo& r) {
            return !r.isFinished();
        });
    }

    /**
     * Responds to the first unprocessed request and then responds to it with the result of calling
     * `responseFn` with the request as argument. If there are no unprocessed requests, blocks until
     * there is.
     */
    void onCommand(std::function<StatusWith<BSONObj>(const RequestInfo&)> responseFn) {
        auto& request = getNextRequest();
        request.respondWith(responseFn(request));
    }

private:
    // All requests that have been scheduled on the mock.
    std::deque<RequestInfo> _requests;
    // Protects the above _requests queue.
    stdx::mutex _m;
    stdx::condition_variable _hasRequestsCV;
};

/**
 * The AsyncMockAsyncRPCRunner allows you to asynchrously register expectations about what
 * requests will be registered; you can:
 *  -> Create an 'expectation' that a request will be scheduled some time in the future,
 *     asynchronously.
 *  -> Provide responses to those requests.
 *  -> Ensure that any expectations were met/requests meeting all expectations were
 *     received and responded to.
 *  -> Examine if any unexpected requests were received and, if so, what they were.
 * See the member funtions below for details.
 */
class AsyncMockAsyncRPCRunner : public detail::AsyncRPCRunner {
public:
    struct Request {
        BSONObj toBSON() const {
            BSONObjBuilder bob;
            BSONObjBuilder subBob(bob.subobjStart("AsyncMockAsyncRPCRunner::Request"));
            subBob.append("dbName: ", dbName);
            subBob.append("command: ", cmdBSON);
            subBob.append("target: ", target.toString());
            subBob.done();
            return bob.obj();
        }
        std::string dbName;
        BSONObj cmdBSON;
        HostAndPort target;
    };

    /**
     * Mock implementation of the core functionality of the RCR. Checks that the request is expected
     * by ensuring that we have an unmet expectation registered that matches it. Then responds to
     * the request with the response provided by the matching expectation. If more the one
     * expectation matches the request, the first unmet one registered with `expect` will be used.
     * See `expect` below for details.  If no expectation matches the request, the request is
     * responded to with a generic error, and the request is recorded. Users that want to make sure
     * that all requests are expected can use the hadUnexpectedRequests/unexpectedRequests member
     * functions to inspect that state.
     */
    ExecutorFuture<detail::AsyncRPCInternalResponse> _sendCommand(
        std::shared_ptr<TaskExecutor> exec,
        CancellationToken token,
        OperationContext* opCtx,
        Targeter* targeter,
        const DatabaseName& dbName,
        BSONObj cmdBSON,
        BatonHandle,
        boost::optional<UUID> clientOperationKey) final {
        auto [p, f] = makePromiseFuture<BSONObj>();
        auto targetsAttempted = std::make_shared<std::vector<HostAndPort>>();
        return targeter->resolve(token).thenRunOn(exec).then(
            [this, p = std::move(p), cmdBSON, dbName = dbName.toString_forTest()](auto&& targets) {
                stdx::lock_guard lg(_m);
                Request req{dbName, cmdBSON, targets[0]};
                auto expectation =
                    std::find_if(_expectations.begin(),
                                 _expectations.end(),
                                 [&](const Expectation& e) { return e.matcher(req) && !e.met; });
                if (expectation == _expectations.end()) {
                    // This request was not expected. We record it and return a generic error
                    // response.
                    _unexpectedRequests.push_back(req);
                    uassertStatusOK(
                        Status{AsyncRPCErrorInfo(Status(ErrorCodes::InternalErrorNotSupported,
                                                        "Unexpected request")),
                               "Remote command execution failed"});
                }
                auto ans = expectation->response;
                expectation->promise.emplaceValue();
                expectation->met = true;
                if (!ans.isOK()) {
                    uassertStatusOK(Status{AsyncRPCErrorInfo(ans.getStatus(), targets[0]),
                                           "Remote command execution failed"});
                }
                Status maybeError(detail::makeErrorIfNeeded(
                    executor::RemoteCommandResponse(targets[0], ans.getValue(), Microseconds(1))));
                uassertStatusOK(maybeError);
                return detail::AsyncRPCInternalResponse{ans.getValue(), targets[0]};
            });
    }

    using RequestMatcher = std::function<bool(const Request&)>;
    struct Expectation {
        Expectation(RequestMatcher m,
                    StatusWith<BSONObj> response,
                    Promise<void>&& p,
                    std::string name)
            : matcher{m}, response{response}, promise{std::move(p)}, name{name} {}
        RequestMatcher matcher;
        StatusWith<BSONObj> response;
        Promise<void> promise;
        bool met{false};
        std::string name;
    };

    /**
     * Register an expectation with the mock that a request matching `matcher` will be scheduled on
     * the mock. The provided RequestMatcher can inspect the cmdObj and target HostAndPort of a
     * request, and should return 'true' if the request is the expected one. Once the expectation is
     * 'met' (i.e. the mock has received a matching request), the request is responded to with the
     * provided `response`. Note that expectations are "one-shot" - each expectation can match
     * exactly one request; once an expectation has been matched to a scheduled request, it is
     * considered 'met' and will not be considered in the future.  Additionally, each expectation is
     * given a name, which allows it to be serialized and identified.
     *
     * Errors in the 'Status' portion of the 'StatusWith' response correspond to 'local errors'
     * (i.e. those on the sending node), while the BSONObj corresponds to the raw BSON received as a
     * response to the request over-the-wire.
     */
    SemiFuture<void> expect(RequestMatcher matcher,
                            StatusWith<BSONObj> response,
                            std::string name) {
        auto [p, f] = makePromiseFuture<void>();
        stdx::lock_guard lg(_m);
        _expectations.emplace_back(matcher, response, std::move(p), std::move(name));
        return std::move(f).semi();
    }

    bool hasUnmetExpectations() {
        stdx::lock_guard lg(_m);
        auto it = std::find_if(_expectations.begin(),
                               _expectations.end(),
                               [&](const Expectation& e) { return !e.met; });
        return it != _expectations.end();
    }

    // Return the name of the first expectation registered that isn't met. If all have been met,
    // return boost::none.
    boost::optional<std::string> getFirstUnmetExpectation() {
        stdx::lock_guard lg(_m);
        auto it = std::find_if(_expectations.begin(),
                               _expectations.end(),
                               [&](const Expectation& e) { return !e.met; });
        if (it == _expectations.end()) {
            return boost::none;
        }
        return it->name;
    }

    // Return the set of names of all unmet expectations.
    StringSet getUnmetExpectations() {
        StringSet set;
        stdx::lock_guard lg(_m);
        for (auto&& expectation : _expectations) {
            if (!expectation.met) {
                set.insert(expectation.name);
            }
        }
        return set;
    }

    bool hadUnexpectedRequests() {
        stdx::lock_guard lg(_m);
        return !_unexpectedRequests.empty();
    }

    boost::optional<Request> getFirstUnexpectedRequest() {
        stdx::lock_guard lg(_m);
        if (!_unexpectedRequests.empty()) {
            return _unexpectedRequests[0];
        }
        return {};
    }

    std::vector<Request> getUnexpectedRequests() {
        stdx::lock_guard lg(_m);
        return _unexpectedRequests;
    }

private:
    std::vector<Expectation> _expectations;
    std::vector<Request> _unexpectedRequests;
    // Protects the above _expectations queue.
    stdx::mutex _m;
};

inline std::ostream& operator<<(std::ostream& s, const AsyncMockAsyncRPCRunner::Request& o) {
    return s << o.toBSON();
}

inline std::ostream& operator<<(std::ostream& s, const AsyncMockAsyncRPCRunner::Expectation& o) {
    return s << o.name;
}

/**
 * The NoopMockAsyncRPCRunner is a mock implementation that returns silently and successfully when a
 * command is sent.
 */
class NoopMockAsyncRPCRunner : public detail::AsyncRPCRunner {
public:
    /**
     * Mock implementation of the core functionality of the RCR. Records the provided request, and
     * notifies waiters that a new request has been scheduled.
     */
    ExecutorFuture<detail::AsyncRPCInternalResponse> _sendCommand(
        std::shared_ptr<TaskExecutor> exec,
        CancellationToken token,
        OperationContext* opCtx,
        Targeter* targeter,
        const DatabaseName& dbName,
        BSONObj cmdBSON,
        BatonHandle,
        boost::optional<UUID>) final {

        return ExecutorFuture(exec).then([] {
            detail::AsyncRPCInternalResponse response;
            response.response = BSON("ok" << 1);
            response.targetUsed = HostAndPort("localhost", serverGlobalParams.port);
            return response;
        });
    }
};

}  // namespace mongo::async_rpc
