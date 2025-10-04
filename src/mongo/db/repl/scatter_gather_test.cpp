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

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/repl/scatter_gather_algorithm.h"
#include "mongo/db/repl/scatter_gather_runner.h"
#include "mongo/executor/network_connection_hook.h"
#include "mongo/executor/network_interface_mock.h"
#include "mongo/executor/remote_command_request.h"
#include "mongo/executor/remote_command_response.h"
#include "mongo/executor/task_executor.h"
#include "mongo/executor/task_executor_test_fixture.h"
#include "mongo/executor/thread_pool_task_executor_test_fixture.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/stdx/thread.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/duration.h"
#include "mongo/util/net/hostandport.h"
#include "mongo/util/time_support.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace mongo {
namespace repl {
namespace {

using executor::NetworkInterfaceMock;
using executor::RemoteCommandRequest;
using executor::RemoteCommandResponse;

static const int kTotalRequests = 3;

/**
 * Algorithm for testing the ScatterGatherRunner, which will finish running when finish() is
 * called, or upon receiving responses from two nodes. Creates a three requests algorithm
 * simulating running an algorithm against three other nodes.
 */
class ScatterGatherTestAlgorithm : public ScatterGatherAlgorithm {
public:
    ScatterGatherTestAlgorithm(int64_t maxResponses = 2)
        : _done(false), _numResponses(0), _maxResponses(maxResponses) {}

    std::vector<RemoteCommandRequest> getRequests() const override {
        std::vector<RemoteCommandRequest> requests;
        for (int i = 0; i < kTotalRequests; i++) {
            requests.push_back(RemoteCommandRequest(HostAndPort("hostname", i),
                                                    DatabaseName::kAdmin,
                                                    BSONObj(),
                                                    nullptr,
                                                    Milliseconds(30 * 1000)));
        }
        return requests;
    }

    void processResponse(const RemoteCommandRequest& request,
                         const RemoteCommandResponse& response) override {
        _numResponses.fetchAndAdd(1);
    }

    void finish() {
        _done.store(true);
    }

    bool hasReceivedSufficientResponses() const override {
        if (_done.load()) {
            return _done.load();
        }

        return _numResponses.load() >= _maxResponses.load();
    }

    int getResponseCount() {
        return _numResponses.load();
    }

private:
    AtomicWord<bool> _done;
    AtomicWord<int64_t> _numResponses;
    AtomicWord<int64_t> _maxResponses;
};

/**
 * ScatterGatherTest base class which sets up the TaskExecutor and NetworkInterfaceMock.
 */
class ScatterGatherTest : public executor::ThreadPoolExecutorTest {
protected:
    int64_t countLogLinesContaining(const std::string& needle);
    void setUp() override {
        executor::ThreadPoolExecutorTest::setUp();
        launchExecutorThread();
    }
};

// Used to run a ScatterGatherRunner in a separate thread, to avoid blocking test execution.
class ScatterGatherRunnerRunner {
public:
    ScatterGatherRunnerRunner(ScatterGatherRunner* sgr, executor::TaskExecutor* executor)
        : _sgr(sgr),
          _executor(executor),
          _result(Status(ErrorCodes::BadValue, "failed to set status")) {}

    // Could block if _sgr has not finished
    Status getResult() {
        _thread->join();
        return _result;
    }

    void run() {
        _thread = std::make_unique<stdx::thread>([this] {
            setThreadName("ScatterGatherRunner");
            _run(_executor);
        });
    }

private:
    void _run(executor::TaskExecutor* executor) {
        _result = _sgr->run();
    }

    ScatterGatherRunner* _sgr;
    executor::TaskExecutor* _executor;
    Status _result;
    std::unique_ptr<stdx::thread> _thread;
};

// Simple onCompletion function which will toggle a bool, so that we can check the logs to
// ensure the onCompletion function ran when expected.
executor::TaskExecutor::CallbackFn getOnCompletionTestFunction(bool* ran) {
    auto cb = [ran](const executor::TaskExecutor::CallbackArgs& cbData) {
        if (!cbData.status.isOK()) {
            return;
        }
        *ran = true;
    };
    return cb;
}


// Confirm that running via start() will finish and run the onComplete function once sufficient
// responses have been received.
// Confirm that deleting both the ScatterGatherTestAlgorithm and ScatterGatherRunner while
// scheduled callbacks still exist will not be unsafe (ASAN builder) after the algorithm has
// completed.
TEST_F(ScatterGatherTest, DeleteAlgorithmAfterItHasCompleted) {
    auto sga = std::make_shared<ScatterGatherTestAlgorithm>();
    ScatterGatherRunner* sgr = new ScatterGatherRunner(sga, &getExecutor(), "test");
    bool ranCompletion = false;
    StatusWith<executor::TaskExecutor::EventHandle> status = sgr->start();
    ASSERT_OK(getExecutor()
                  .onEvent(status.getValue(), getOnCompletionTestFunction(&ranCompletion))
                  .getStatus());
    ASSERT_OK(status.getStatus());
    ASSERT_FALSE(ranCompletion);

    NetworkInterfaceMock* net = getNet();
    net->enterNetwork();
    NetworkInterfaceMock::NetworkOperationIterator noi = net->getNextReadyRequest();
    net->scheduleResponse(noi,
                          net->now() + Seconds(2),
                          (RemoteCommandResponse::make_forTest(BSON("ok" << 1), Milliseconds(10))));
    ASSERT_FALSE(ranCompletion);

    noi = net->getNextReadyRequest();
    net->scheduleResponse(noi,
                          net->now() + Seconds(2),
                          (RemoteCommandResponse::make_forTest(BSON("ok" << 1), Milliseconds(10))));
    ASSERT_FALSE(ranCompletion);

    noi = net->getNextReadyRequest();
    net->scheduleResponse(noi,
                          net->now() + Seconds(5),
                          (RemoteCommandResponse::make_forTest(BSON("ok" << 1), Milliseconds(10))));
    ASSERT_FALSE(ranCompletion);

    net->runUntil(net->now() + Seconds(2));
    ASSERT_TRUE(ranCompletion);

    sga.reset();
    delete sgr;

    net->runReadyNetworkOperations();

    net->exitNetwork();
}

TEST_F(ScatterGatherTest, DeleteAlgorithmBeforeItCompletes) {
    auto sga = std::make_shared<ScatterGatherTestAlgorithm>();
    ScatterGatherRunner* sgr = new ScatterGatherRunner(sga, &getExecutor(), "test");
    bool ranCompletion = false;
    StatusWith<executor::TaskExecutor::EventHandle> status = sgr->start();
    ASSERT_OK(status.getStatus());
    ASSERT_OK(getExecutor()
                  .onEvent(status.getValue(), getOnCompletionTestFunction(&ranCompletion))
                  .getStatus());
    ASSERT_FALSE(ranCompletion);

    NetworkInterfaceMock* net = getNet();
    net->enterNetwork();
    NetworkInterfaceMock::NetworkOperationIterator noi = net->getNextReadyRequest();
    net->scheduleResponse(
        noi, net->now(), (RemoteCommandResponse::make_forTest(BSON("ok" << 1), Milliseconds(10))));
    ASSERT_FALSE(ranCompletion);
    // Get and process the response from the first node immediately.
    net->runReadyNetworkOperations();

    noi = net->getNextReadyRequest();
    net->scheduleResponse(noi,
                          net->now() + Seconds(2),
                          (RemoteCommandResponse::make_forTest(BSON("ok" << 1), Milliseconds(10))));
    ASSERT_FALSE(ranCompletion);

    noi = net->getNextReadyRequest();
    net->scheduleResponse(noi,
                          net->now() + Seconds(5),
                          (RemoteCommandResponse::make_forTest(BSON("ok" << 1), Milliseconds(10))));
    ASSERT_FALSE(ranCompletion);

    sga.reset();
    delete sgr;

    net->runUntil(net->now() + Seconds(2));
    ASSERT_TRUE(ranCompletion);

    net->exitNetwork();
}

TEST_F(ScatterGatherTest, DeleteAlgorithmAfterCancel) {
    auto sga = std::make_shared<ScatterGatherTestAlgorithm>();
    ScatterGatherRunner* sgr = new ScatterGatherRunner(sga, &getExecutor(), "test");
    bool ranCompletion = false;
    StatusWith<executor::TaskExecutor::EventHandle> status = sgr->start();
    ASSERT_OK(status.getStatus());
    ASSERT_OK(getExecutor()
                  .onEvent(status.getValue(), getOnCompletionTestFunction(&ranCompletion))
                  .getStatus());
    ASSERT_FALSE(ranCompletion);

    NetworkInterfaceMock* net = getNet();
    net->enterNetwork();
    NetworkInterfaceMock::NetworkOperationIterator noi = net->getNextReadyRequest();
    net->scheduleResponse(noi,
                          net->now() + Seconds(2),
                          (RemoteCommandResponse::make_forTest(BSON("ok" << 1), Milliseconds(10))));
    ASSERT_FALSE(ranCompletion);

    // Cancel the runner so following responses won't change the result. All pending requests
    // are cancelled.
    sgr->cancel();
    ASSERT_FALSE(net->hasReadyRequests());
    // Run the event that gets signaled by cancellation.
    net->runReadyNetworkOperations();
    ASSERT_TRUE(ranCompletion);

    sga.reset();
    delete sgr;

    // It's safe to advance the clock to process the scheduled response.
    auto now = net->now();
    ASSERT_EQ(net->runUntil(net->now() + Seconds(2)), now + Seconds(2));
    net->exitNetwork();
}

// Confirm that shutting the TaskExecutor down before calling run() will cause run()
// to return ErrorCodes::ShutdownInProgress.
TEST_F(ScatterGatherTest, ShutdownExecutorBeforeRun) {
    auto sga = std::make_shared<ScatterGatherTestAlgorithm>();
    ScatterGatherRunner sgr(sga, &getExecutor(), "test");
    shutdownExecutorThread();
    sga->finish();
    Status status = sgr.run();
    ASSERT_EQUALS(ErrorCodes::ShutdownInProgress, status);
}

// Confirm that shutting the TaskExecutor down after calling run(), but before run()
// finishes will cause run() to return Status::OK().
TEST_F(ScatterGatherTest, ShutdownExecutorAfterRun) {
    auto sga = std::make_shared<ScatterGatherTestAlgorithm>();
    ScatterGatherRunner sgr(sga, &getExecutor(), "test");
    ScatterGatherRunnerRunner sgrr(&sgr, &getExecutor());
    sgrr.run();
    // need to wait for the scatter-gather to be scheduled in the executor
    NetworkInterfaceMock* net = getNet();
    net->enterNetwork();
    // Black hole all requests before shutdown, so that scheduleRemoteCommand will succeed.
    for (int i = 0; i < kTotalRequests; i++) {
        NetworkInterfaceMock::NetworkOperationIterator noi = net->getNextReadyRequest();
        net->blackHole(noi);
    }
    net->exitNetwork();
    shutdownExecutorThread();
    joinExecutorThread();
    Status status = sgrr.getResult();
    ASSERT_OK(status);
}

// Confirm that shutting the TaskExecutor down before calling start() will cause start()
// to return ErrorCodes::ShutdownInProgress and should not run onCompletion().
TEST_F(ScatterGatherTest, ShutdownExecutorBeforeStart) {
    auto sga = std::make_shared<ScatterGatherTestAlgorithm>();
    ScatterGatherRunner sgr(sga, &getExecutor(), "test");
    shutdownExecutorThread();
    bool ranCompletion = false;
    StatusWith<executor::TaskExecutor::EventHandle> status = sgr.start();
    sga->finish();
    ASSERT_FALSE(ranCompletion);
    ASSERT_EQUALS(ErrorCodes::ShutdownInProgress, status.getStatus());
}

// Confirm that shutting the TaskExecutor down after calling start() will cause start()
// to return Status::OK and should not run onCompletion().
TEST_F(ScatterGatherTest, ShutdownExecutorAfterStart) {
    auto sga = std::make_shared<ScatterGatherTestAlgorithm>();
    ScatterGatherRunner sgr(sga, &getExecutor(), "test");
    bool ranCompletion = false;
    StatusWith<executor::TaskExecutor::EventHandle> status = sgr.start();
    ASSERT_OK(getExecutor()
                  .onEvent(status.getValue(), getOnCompletionTestFunction(&ranCompletion))
                  .getStatus());
    shutdownExecutorThread();
    sga->finish();
    ASSERT_FALSE(ranCompletion);
    ASSERT_OK(status.getStatus());
}

// Confirm that responses are not processed once sufficient responses have been received.
TEST_F(ScatterGatherTest, DoNotProcessMoreThanSufficientResponses) {
    auto sga = std::make_shared<ScatterGatherTestAlgorithm>();
    ScatterGatherRunner sgr(sga, &getExecutor(), "test");
    bool ranCompletion = false;
    StatusWith<executor::TaskExecutor::EventHandle> status = sgr.start();
    ASSERT_OK(getExecutor()
                  .onEvent(status.getValue(), getOnCompletionTestFunction(&ranCompletion))
                  .getStatus());
    ASSERT_OK(status.getStatus());
    ASSERT_FALSE(ranCompletion);

    NetworkInterfaceMock* net = getNet();
    net->enterNetwork();
    NetworkInterfaceMock::NetworkOperationIterator noi = net->getNextReadyRequest();
    net->scheduleResponse(noi,
                          net->now() + Seconds(2),
                          (RemoteCommandResponse::make_forTest(BSON("ok" << 1), Milliseconds(10))));
    ASSERT_FALSE(ranCompletion);

    noi = net->getNextReadyRequest();
    net->scheduleResponse(noi,
                          net->now() + Seconds(2),
                          (RemoteCommandResponse::make_forTest(BSON("ok" << 1), Milliseconds(10))));
    ASSERT_FALSE(ranCompletion);

    noi = net->getNextReadyRequest();
    net->scheduleResponse(noi,
                          net->now() + Seconds(5),
                          (RemoteCommandResponse::make_forTest(BSON("ok" << 1), Milliseconds(10))));
    ASSERT_FALSE(ranCompletion);

    net->runUntil(net->now() + Seconds(2));
    ASSERT_TRUE(ranCompletion);

    net->runReadyNetworkOperations();
    // the third resposne should not be processed, so the count should not increment
    ASSERT_EQUALS(2, sga->getResponseCount());

    net->exitNetwork();
}

// Confirm that scatter-gather runner passes CallbackCanceled error to the algorithm
// and that the algorithm processes the response correctly.
TEST_F(ScatterGatherTest, AlgorithmProcessesCallbackCanceledResponse) {
    auto sga = std::make_shared<ScatterGatherTestAlgorithm>();
    ScatterGatherRunner sgr(sga, &getExecutor(), "test");
    bool ranCompletion = false;
    StatusWith<executor::TaskExecutor::EventHandle> status = sgr.start();
    ASSERT_OK(getExecutor()
                  .onEvent(status.getValue(), getOnCompletionTestFunction(&ranCompletion))
                  .getStatus());
    ASSERT_OK(status.getStatus());
    ASSERT_FALSE(ranCompletion);

    NetworkInterfaceMock* net = getNet();
    net->enterNetwork();
    NetworkInterfaceMock::NetworkOperationIterator noi = net->getNextReadyRequest();
    net->scheduleResponse(noi,
                          net->now() + Seconds(2),
                          (RemoteCommandResponse::make_forTest(BSON("ok" << 1), Milliseconds(10))));
    ASSERT_FALSE(ranCompletion);

    noi = net->getNextReadyRequest();
    net->scheduleResponse(noi,
                          net->now() + Seconds(2),
                          (RemoteCommandResponse::make_forTest(
                              Status(ErrorCodes::CallbackCanceled, "Testing canceled callback"))));
    ASSERT_FALSE(ranCompletion);

    // We don't schedule a response from one node to make sure the response with the
    // CallbackCanceled error is needed to get the sufficient number of responses.
    noi = net->getNextReadyRequest();
    ASSERT_FALSE(ranCompletion);

    net->runUntil(net->now() + Seconds(2));
    ASSERT_TRUE(ranCompletion);

    net->runReadyNetworkOperations();
    // The response with the CallbackCanceled error should count as a response to the algorithm.
    ASSERT_EQUALS(2, sga->getResponseCount());

    net->exitNetwork();
}

// Confirm that starting with sufficient responses received will immediate complete.
TEST_F(ScatterGatherTest, DoNotCreateCallbacksIfHasSufficientResponsesReturnsTrueImmediately) {
    auto sga = std::make_shared<ScatterGatherTestAlgorithm>();
    // set hasReceivedSufficientResponses to return true before the run starts
    sga->finish();
    ScatterGatherRunner sgr(sga, &getExecutor(), "test");
    bool ranCompletion = false;
    StatusWith<executor::TaskExecutor::EventHandle> status = sgr.start();
    ASSERT_OK(getExecutor()
                  .onEvent(status.getValue(), getOnCompletionTestFunction(&ranCompletion))
                  .getStatus());
    ASSERT_OK(status.getStatus());
    // Wait until callback finishes.
    NetworkInterfaceMock* net = getNet();
    net->enterNetwork();
    net->runReadyNetworkOperations();
    net->exitNetwork();
    ASSERT_TRUE(ranCompletion);
}

#if 0
    // TODO Enable this test once we have a way to test for invariants.

    // This test ensures we do not process more responses than we've scheduled callbacks for.
    TEST_F(ScatterGatherTest, NeverEnoughResponses) {
        auto sga = std::make_shared<ScatterGatherTestAlgorithm>(5);
        ScatterGatherRunner sgr(sga);
        bool ranCompletion = false;
        StatusWith<executor::TaskExecutor::EventHandle> status = sgr.start(&getExecutor(),
                getOnCompletionTestFunction(&ranCompletion));
        ASSERT_OK(status.getStatus());
        ASSERT_FALSE(ranCompletion);

        NetworkInterfaceMock* net = getNet();
        net->enterNetwork();
        NetworkInterfaceMock::NetworkOperationIterator noi = net->getNextReadyRequest();
        net->scheduleResponse(noi,
                              net->now(),
                              (RemoteCommandResponse::make_forTest(
                                    BSON("ok" << 1),
                                    boost::posix_time::milliseconds(10))));
        net->runReadyNetworkOperations();
        ASSERT_FALSE(ranCompletion);

        noi = net->getNextReadyRequest();
        net->scheduleResponse(noi,
                              net->now(),
                              (RemoteCommandResponse::make_forTest(
                                    BSON("ok" << 1),
                                    boost::posix_time::milliseconds(10))));
        net->runReadyNetworkOperations();
        ASSERT_FALSE(ranCompletion);

        noi = net->getNextReadyRequest();
        net->scheduleResponse(noi,
                              net->now(),
                              (RemoteCommandResponse::make_forTest(
                                    BSON("ok" << 1),
                                    boost::posix_time::milliseconds(10))));
        net->runReadyNetworkOperations();
        net->exitNetwork();
        ASSERT_FALSE(ranCompletion);
    }
#endif  // 0

// Confirm that running via run() will finish once sufficient responses have been received.
TEST_F(ScatterGatherTest, SuccessfulScatterGatherViaRun) {
    auto sga = std::make_shared<ScatterGatherTestAlgorithm>();
    ScatterGatherRunner sgr(sga, &getExecutor(), "test");
    ScatterGatherRunnerRunner sgrr(&sgr, &getExecutor());
    sgrr.run();

    NetworkInterfaceMock* net = getNet();
    net->enterNetwork();
    NetworkInterfaceMock::NetworkOperationIterator noi = net->getNextReadyRequest();
    net->scheduleResponse(
        noi, net->now(), (RemoteCommandResponse::make_forTest(BSON("ok" << 1), Milliseconds(10))));
    net->runReadyNetworkOperations();

    noi = net->getNextReadyRequest();
    net->blackHole(noi);
    net->runReadyNetworkOperations();

    noi = net->getNextReadyRequest();
    net->scheduleResponse(
        noi, net->now(), (RemoteCommandResponse::make_forTest(BSON("ok" << 1), Milliseconds(10))));
    net->runReadyNetworkOperations();
    net->exitNetwork();

    Status status = sgrr.getResult();
    ASSERT_OK(status);
}

}  // namespace
}  // namespace repl
}  // namespace mongo
