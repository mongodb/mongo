// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


#include "mongo/executor/mock_network_fixture.h"

#include "mongo/db/exec/matcher/matcher.h"
#include "mongo/db/matcher/matcher.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/expression_context_builder.h"
#include "mongo/executor/network_interface_mock.h"
#include "mongo/util/intrusive_counter.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest


namespace mongo {
namespace test {
namespace mock {

MockNetwork::Matcher::Matcher(const BSONObj& matcherQuery) {
    auto expCtx = ExpressionContextBuilder{}
                      .ns(NamespaceString::createNamespaceString_forTest("db.coll"))
                      .build();
    // Expression matcher doesn't have copy constructor, so wrap it in a shared_ptr for capture.
    auto m = std::make_shared<mongo::Matcher>(matcherQuery, std::move(expCtx));
    _matcherFunc = [=](const BSONObj& request) {
        return exec::matcher::matches(m.get(), request);
    };
}

bool MockNetwork::_allExpectationsSatisfied() const {
    return std::all_of(_expectations.begin(), _expectations.end(), [](const auto& exp) {
        return exp->isDefault() || exp->isSatisfied();
    });
}

void MockNetwork::_logUnsatisfiedExpectations() const {
    for (const auto& expectation : _expectations) {
        expectation->logIfUnsatisfied();
    }
}

void MockNetwork::runUntilIdle() {
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
            auto noi = _net->getFrontOfReadyQueue();
            // Requests may have already been scheduled due to simultaneous interruptions.
            if (_net->isNetworkOperationIteratorAtEnd(noi))
                continue;
            auto request = noi->getRequest().cmdObj;

            // We ignore the next request if it's not expected (or already satisfied).
            // Default expectations are always the oldest in the vector, so matching expectations
            // in LIFO order allows us to always see the overrides first.
            // (Iterating a vector backwards is much cheaper than pushing to its front.)
            auto const& exp =
                std::find_if(_expectations.rbegin(), _expectations.rend(), [&](const auto& exp) {
                    return !exp->isSatisfied() && exp->prerequisitesMet() && exp->match(request);
                });

            if (exp != _expectations.rend()) {
                // Consume the next request and execute the action.
                noi = _net->getNextReadyRequest();
                auto response = (*exp)->run(request);
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

void MockNetwork::runUntilExpectationsSatisfied(std::chrono::seconds timeoutSeconds) {
    Timer timer;
    // If there exist extra threads beside the executor and the mock/test thread, when the
    // network is idle, the extra threads may be running and will schedule new requests. As a
    // result, the current best practice is to busy-loop to prepare for that.
    while (!_allExpectationsSatisfied()) {
        if (timer.seconds() > timeoutSeconds.count()) {
            _logUnsatisfiedExpectations();
            LOGV2_FATAL(9467601, "Still waiting on expectations past the timeout period.");
        }
        runUntilIdle();
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
        runUntilIdle();
    }
    LOGV2_DEBUG(5015403, 1, "mock reached time", "target"_attr = target);
}

}  // namespace mock
}  // namespace test
}  // namespace mongo
