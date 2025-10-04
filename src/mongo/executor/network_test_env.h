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

#include "mongo/base/status_with.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/executor/network_interface_mock.h"
#include "mongo/executor/remote_command_request.h"
#include "mongo/executor/remote_command_response.h"
#include "mongo/executor/task_executor.h"
#include "mongo/stdx/future.h"
#include "mongo/stdx/thread.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/duration.h"
#include "mongo/util/time_support.h"

#include <chrono>
#include <functional>
#include <future>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

#include <boost/move/utility_core.hpp>

namespace mongo {
namespace executor {

/**
 * A network infrastructure for testing that provides helpers (onCommand*, onFind*) to extract
 * pending requests from the NetworkInterface in this NetworkTestEnv and set a response for it.
 * Note that these helpers are synchronous and the code that is being tested that uses the
 * TaskExecutor will typically be also synchronously waiting for the response from the network.
 * Therefore, the test will not make progress if it is ran on a single thread. To get around this,
 * the launchAsync helper can be used to create a new thread that will be used to initiate the
 * network call.
 *
 * Example usage:
 *
 * auto future = netTestEnv.launchAsync([](){
 *     return methodWithNetCallToTest();
 * });
 *
 *
 * netTestEnv.OnCommandFunction([](const RemoteCommandRequest& request){
 *     // check contents of request
 *     return BSON("ok" << 1); // return desired response
 * });
 *
 * // Add as many onCommand/onFind calls to match the number of network calls by the
 * // methodWithNetCallToTest in the order they are going to be called.
 *
 * checkResult(future.timed_get(...));
 */
class NetworkTestEnv {
public:
    // Common timeout for tests to use for any work scheduled through launchAsync to complete.
    static constexpr Minutes kDefaultLaunchAsyncFutureTimeout{5};

    /**
     * Wraps a std::future but will cancel any pending network operations in its destructor if
     * the future wasn't successfully waited on in the main test thread.
     * Without this behavior any operations launched asynchronously might never terminate if they
     * are waiting for network operations to complete.
     */
    template <class T>
    class FutureHandle {
    public:
        FutureHandle(std::future<T> future,  // NOLINT
                     executor::TaskExecutor* executor,
                     executor::NetworkInterfaceMock* network)
            : _future(std::move(future)), _executor(executor), _network(network) {}

        FutureHandle(FutureHandle&& other) = default;

        FutureHandle& operator=(FutureHandle&& other) {
            // Assigning to initialized FutureHandle is banned because of the work required prior to
            // waiting on the future.
            invariant(!_future.valid());

            _future = std::move(other._future);
            _executor = other._executor;
            _network = other._network;

            return *this;
        }

        ~FutureHandle() {
            if (_future.valid()) {
                _network->exitNetwork();
                _executor->shutdown();
                _future.wait();
            }
        }

        template <class Rep, class Period>
        T timed_get(const stdx::chrono::duration<Rep, Period>& timeout_duration) {
            auto status = _future.wait_for(timeout_duration);
            ASSERT(status == stdx::future_status::ready);

            return _future.get();
        }

        template <class Period>
        T timed_get(const Duration<Period>& timeout_duration) {
            return timed_get(timeout_duration.toSystemDuration());
        }

        T default_timed_get() {
            return timed_get(kDefaultLaunchAsyncFutureTimeout);
        }

    private:
        std::future<T> _future;  // NOLINT
        executor::TaskExecutor* _executor;
        executor::NetworkInterfaceMock* _network;
    };

    /**
     * Helper method for launching an asynchronous task in a way that will guarantees that the
     * task will finish even if the task depends on network traffic via the mock network and there's
     * an exception that prevents the main test thread from scheduling responses to the network
     * operations.  It does this by returning a FutureHandle that wraps std::future and cancels
     * all pending network operations in its destructor.
     * Must be defined in the header because of its use of templates.
     */
    template <typename Lambda>
    FutureHandle<typename std::invoke_result<Lambda>::type> launchAsync(Lambda&& func) const {
        auto future = async(stdx::launch::async, std::forward<Lambda>(func));
        return NetworkTestEnv::FutureHandle<typename std::invoke_result<Lambda>::type>{
            std::move(future), _executor, _mockNetwork};
    }

    using OnCommandFunction = std::function<StatusWith<BSONObj>(const RemoteCommandRequest&)>;
    using OnCommandWithMetadataFunction =
        std::function<RemoteCommandResponse(const RemoteCommandRequest&)>;

    using OnFindCommandFunction =
        std::function<StatusWith<std::vector<BSONObj>>(const RemoteCommandRequest&)>;
    // Function that accepts a find request and returns a tuple of resulting documents and response
    // metadata.
    using OnFindCommandWithMetadataFunction =
        std::function<StatusWith<std::tuple<std::vector<BSONObj>, BSONObj>>(
            const RemoteCommandRequest&)>;

    /**
     * Create a new test environment based on an existing executor and network.
     */
    NetworkTestEnv(TaskExecutor* executor, NetworkInterfaceMock* network);

    /**
     * Blocking methods, which receive one message from the network and respond using the
     * responses returned from the input function. This is a syntactic sugar for simple,
     * single request + response or find tests.
     */
    void onCommand(OnCommandFunction func);
    void onCommands(std::vector<OnCommandFunction> funcs);
    void onCommandWithMetadata(OnCommandWithMetadataFunction func);
    void onFindCommand(OnFindCommandFunction func);
    void onFindWithMetadataCommand(OnFindCommandWithMetadataFunction func);

private:
    // Task executor used for running asynchronous operations.
    TaskExecutor* _executor;

    // Mocked out network under the task executor.
    NetworkInterfaceMock* _mockNetwork;
};

}  // namespace executor
}  // namespace mongo
