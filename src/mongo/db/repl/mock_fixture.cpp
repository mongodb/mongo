/**
 *    Copyright (C) 2020-present MongoDB, Inc.
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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest

#include "mongo/platform/basic.h"

#include "mongo/db/repl/mock_fixture.h"

#include "mongo/executor/network_interface_mock.h"
#include "mongo/logv2/log.h"

namespace mongo {
namespace test {
namespace mock {

bool MockNetwork::_allExpectationsSatisfied() const {
    return std::all_of(_expectations.begin(), _expectations.end(), [](const Expectation& exp) {
        return exp.isDefault() || exp.isSatisfied();
    });
}

void MockNetwork::_runUntilIdle() {
    executor::NetworkInterfaceMock::InNetworkGuard guard(_net);
    do {
        // The main responsibility of the mock network is to host incoming requests and scheduled
        // responses. Additionally, the mock network interface is a de facto lock-step scheduler.
        //
        // The executor thread and the mock/test thread run in turn. The test thread
        // (1) triggers the tested behavior, e.g. by simulating a command;
        // (2) responds to network requests;
        // (3) advances the mock clock; and
        // (4) handles some network operations implicitly (explained below).
        // The executor runs the asynchronous jobs which may schedule network requests.
        //
        // The executor thread gets the first turn, then each of them yields at the end of their
        // turns by enabling and signaling the other. The executor thread yields by calling
        // waitForWork() on the mock network; the test thread yields by calling
        // runReadyNetworkOperations().
        //
        // runReadyNetworkOperations() also first checks for expired scheduled works (e.g. request
        // timeout) and executes the expired works. This behavior is the item (4) mentioned above.
        //
        // After yielding to the executor thread, it's possible that new expired scheduled works
        // were added by the executor. That's why we need to double check if there's any ready
        // network operations before deciding the network is idle.
        //
        // External threads may make things more complex. For example, they can schedule new
        // requests right after we thought the network was idle. However, that's always the case
        // with or without the mock framework.
        _net->runReadyNetworkOperations();
        if (_net->hasReadyRequests()) {
            // Peek the next request.
            auto noi = _net->getFrontOfUnscheduledQueue();
            auto request = noi->getRequest().cmdObj;

            // We ignore the next request if it's not expected.
            auto exp = std::find_if(_expectations.begin(), _expectations.end(), [&](auto& exp) {
                return !exp.isSatisfied() && exp.match(request);
            });

            if (exp != _expectations.end()) {
                // Consume the next request and execute the action.
                noi = _net->getNextReadyRequest();
                auto response = exp->run(request);
                LOGV2_DEBUG(5015401,
                            1,
                            "mock reply ",
                            "request"_attr = request,
                            "response"_attr = response);
                _net->scheduleResponse(noi, _net->now(), response);

                // Continue handling network operations and process requests.
                continue;
            }
        }

        // The executor is idle since we just ran it. Check hasReadyNetworkOperations so that no
        // scheduled work is waiting for the network thread.
    } while (_net->hasReadyNetworkOperations());
}

void MockNetwork::runUntilExpectationsSatisfied() {
    // If there exist extra threads beside the executor and the mock/test thread, when the
    // network is idle, the extra threads may be running and will schedule new requests. As a
    // result, the current best practice is to busy-loop to prepare for that.
    while (!_allExpectationsSatisfied()) {
        _runUntilIdle();
    }
}

void MockNetwork::runUntil(Date_t target) {
    while (_net->now() < target) {
        LOGV2_DEBUG(
            5015402, 1, "mock advances time", "from"_attr = _net->now(), "to"_attr = target);
        {
            executor::NetworkInterfaceMock::InNetworkGuard guard(_net);
            // Even if we cannot reach target time, we are still making progress in the loop.
            _net->runUntil(target);
        }
        // Run until idle.
        _runUntilIdle();
    }
    LOGV2_DEBUG(5015403, 1, "mock reached time", "target"_attr = target);
}

}  // namespace mock
}  // namespace test
}  // namespace mongo
