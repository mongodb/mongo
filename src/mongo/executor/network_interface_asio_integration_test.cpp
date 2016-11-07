/**
 *    Copyright (C) 2015 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kASIO

#include "mongo/platform/basic.h"

#include <algorithm>
#include <exception>

#include "mongo/client/connection_string.h"
#include "mongo/executor/async_stream_factory.h"
#include "mongo/executor/async_stream_interface.h"
#include "mongo/executor/async_timer_asio.h"
#include "mongo/executor/network_interface_asio.h"
#include "mongo/executor/network_interface_asio_test_utils.h"
#include "mongo/executor/task_executor.h"
#include "mongo/platform/random.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/stdx/future.h"
#include "mongo/stdx/memory.h"
#include "mongo/unittest/integration_test.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/concurrency/thread_pool.h"
#include "mongo/util/log.h"
#include "mongo/util/scopeguard.h"

namespace mongo {
namespace executor {
namespace {

using StartCommandCB = stdx::function<void(const RemoteCommandResponse&)>;

class NetworkInterfaceASIOIntegrationTest : public mongo::unittest::Test {
public:
    void startNet(NetworkInterfaceASIO::Options options = NetworkInterfaceASIO::Options()) {
        options.streamFactory = stdx::make_unique<AsyncStreamFactory>();
        options.timerFactory = stdx::make_unique<AsyncTimerFactoryASIO>();
#ifdef _WIN32
        // Connections won't queue on widnows, so attempting to open too many connections
        // concurrently will result in refused connections and test failure.
        options.connectionPoolOptions.maxConnections = 16u;
#else
        options.connectionPoolOptions.maxConnections = 256u;
#endif
        _net = stdx::make_unique<NetworkInterfaceASIO>(std::move(options));
        _net->startup();
    }

    void tearDown() override {
        if (!_net->inShutdown()) {
            _net->shutdown();
        }
    }

    NetworkInterfaceASIO& net() {
        return *_net;
    }

    ConnectionString fixture() {
        return unittest::getFixtureConnectionString();
    }

    void randomNumberGenerator(PseudoRandom* generator) {
        _rng = generator;
    }

    PseudoRandom* randomNumberGenerator() {
        return _rng;
    }

    void startCommand(const TaskExecutor::CallbackHandle& cbHandle,
                      RemoteCommandRequest& request,
                      StartCommandCB onFinish) {
        net().startCommand(cbHandle, request, onFinish);
    }

    Deferred<RemoteCommandResponse> runCommand(const TaskExecutor::CallbackHandle& cbHandle,
                                               RemoteCommandRequest& request) {
        Deferred<RemoteCommandResponse> deferred;
        net().startCommand(cbHandle, request, [deferred](RemoteCommandResponse resp) mutable {
            deferred.emplace(std::move(resp));
        });
        return deferred;
    }

    RemoteCommandResponse runCommandSync(RemoteCommandRequest& request) {
        auto deferred = runCommand(makeCallbackHandle(), request);
        auto& res = deferred.get();
        if (res.isOK()) {
            log() << "got command result: " << res.toString();
        } else {
            log() << "command failed: " << res.status;
        }
        return res;
    }

    void assertCommandOK(StringData db,
                         const BSONObj& cmd,
                         Milliseconds timeoutMillis = Milliseconds(-1)) {
        RemoteCommandRequest request{
            fixture().getServers()[0], db.toString(), cmd, BSONObj(), nullptr, timeoutMillis};
        auto res = runCommandSync(request);
        ASSERT_OK(res.status);
        ASSERT_OK(getStatusFromCommandResult(res.data));
    }

    void assertCommandFailsOnClient(StringData db,
                                    const BSONObj& cmd,
                                    Milliseconds timeoutMillis,
                                    ErrorCodes::Error reason) {
        RemoteCommandRequest request{
            fixture().getServers()[0], db.toString(), cmd, BSONObj(), nullptr, timeoutMillis};
        auto res = runCommandSync(request);
        ASSERT_EQ(reason, res.status.code());
    }

    void assertCommandFailsOnServer(StringData db,
                                    const BSONObj& cmd,
                                    Milliseconds timeoutMillis,
                                    ErrorCodes::Error reason) {
        RemoteCommandRequest request{
            fixture().getServers()[0], db.toString(), cmd, BSONObj(), nullptr, timeoutMillis};
        auto res = runCommandSync(request);
        ASSERT_OK(res.status);
        auto serverStatus = getStatusFromCommandResult(res.data);
        ASSERT_EQ(reason, serverStatus);
    }

private:
    std::unique_ptr<NetworkInterfaceASIO> _net;
    PseudoRandom* _rng = nullptr;
};

TEST_F(NetworkInterfaceASIOIntegrationTest, Ping) {
    startNet();
    assertCommandOK("admin", BSON("ping" << 1));
}

TEST_F(NetworkInterfaceASIOIntegrationTest, Timeouts) {
    startNet();
    // This sleep command will take 10 seconds, so we should time out client side first given
    // our timeout of 100 milliseconds.
    assertCommandFailsOnClient("admin",
                               BSON("sleep" << 1 << "lock"
                                            << "none"
                                            << "secs"
                                            << 10),
                               Milliseconds(100),
                               ErrorCodes::ExceededTimeLimit);

    // Run a sleep command that should return before we hit the ASIO timeout.
    assertCommandOK("admin",
                    BSON("sleep" << 1 << "lock"
                                 << "none"
                                 << "secs"
                                 << 1),
                    Milliseconds(10000000));
}

class StressTestOp {
public:
    using Fixture = NetworkInterfaceASIOIntegrationTest;
    using Pool = ThreadPoolInterface;

    void run(Fixture* fixture,
             StartCommandCB onFinish,
             Milliseconds timeout = RemoteCommandRequest::kNoTimeout) {
        auto cb = makeCallbackHandle();

        RemoteCommandRequest request{unittest::getFixtureConnectionString().getServers()[0],
                                     "admin",
                                     _command,
                                     nullptr,
                                     timeout};

        fixture->startCommand(cb, request, onFinish);

        if (_cancel) {
            invariant(fixture->randomNumberGenerator());
            sleepmillis(fixture->randomNumberGenerator()->nextInt32(10));
            fixture->net().cancelCommand(cb);
        }
    }

    static void runTimeoutOp(Fixture* fixture, StartCommandCB onFinish) {
        return StressTestOp(BSON("sleep" << 1 << "lock"
                                         << "none"
                                         << "secs"
                                         << 1),
                            false)
            .run(fixture, onFinish, Milliseconds(100));
    }

    static void runCompleteOp(Fixture* fixture, StartCommandCB onFinish) {
        return StressTestOp(BSON("sleep" << 1 << "lock"
                                         << "none"
                                         << "millis"
                                         << 100),
                            false)
            .run(fixture, onFinish);
    }

    static void runCancelOp(Fixture* fixture, StartCommandCB onFinish) {
        return StressTestOp(BSON("sleep" << 1 << "lock"
                                         << "none"
                                         << "secs"
                                         << 10),
                            true)
            .run(fixture, onFinish);
    }

    static void runLongOp(Fixture* fixture, StartCommandCB onFinish) {
        return StressTestOp(BSON("sleep" << 1 << "lock"
                                         << "none"
                                         << "secs"
                                         << 30),
                            false)
            .run(fixture, onFinish);
    }

private:
    StressTestOp(const BSONObj& command, bool cancel) : _command(command), _cancel(cancel) {}

    BSONObj _command;
    bool _cancel;
};

TEST_F(NetworkInterfaceASIOIntegrationTest, StressTest) {
    constexpr std::size_t numOps = 1000;
    RemoteCommandResponse testResults[numOps];
    ErrorCodes::Error expectedResults[numOps];
    CountdownLatch cl(numOps);

    startNet();

    std::unique_ptr<SecureRandom> seedSource{SecureRandom::create()};
    auto seed = seedSource->nextInt64();

    log() << "Random seed is " << seed;
    auto rng = PseudoRandom(seed);  // TODO: read from command line
    randomNumberGenerator(&rng);
    log() << "Starting stress test...";

    for (std::size_t i = 0; i < numOps; ++i) {
        // stagger operations slightly to mitigate connection pool contention
        sleepmillis(rng.nextInt32(16));

        auto r = rng.nextCanonicalDouble();

        auto cb = [&testResults, &cl, i](const RemoteCommandResponse& resp) {
            testResults[i] = resp;
            cl.countDown();
        };

        if (r < .3) {
            expectedResults[i] = ErrorCodes::CallbackCanceled;
            StressTestOp::runCancelOp(this, cb);
        } else if (r < .7) {
            expectedResults[i] = ErrorCodes::OK;
            StressTestOp::runCompleteOp(this, cb);
        } else if (r < .99) {
            expectedResults[i] = ErrorCodes::ExceededTimeLimit;
            StressTestOp::runTimeoutOp(this, cb);
        } else {
            // Just a sprinkling of long ops, to mitigate connection pool contention
            expectedResults[i] = ErrorCodes::OK;
            StressTestOp::runLongOp(this, cb);
        }
    };

    cl.await();

    for (std::size_t i = 0; i < numOps; ++i) {
        const auto& resp = testResults[i];
        auto ec = resp.isOK() ? getStatusFromCommandResult(resp.data) : resp.status;
        ASSERT_EQ(ec, expectedResults[i]);
    }
}

// Hook that intentionally never finishes
class HangingHook : public executor::NetworkConnectionHook {
    Status validateHost(const HostAndPort&, const RemoteCommandResponse&) final {
        return Status::OK();
    }

    StatusWith<boost::optional<RemoteCommandRequest>> makeRequest(
        const HostAndPort& remoteHost) final {
        return {boost::make_optional(RemoteCommandRequest(remoteHost,
                                                          "admin",
                                                          BSON("sleep" << 1 << "lock"
                                                                       << "none"
                                                                       << "secs"
                                                                       << 100000000),
                                                          BSONObj(),
                                                          nullptr))};
    }

    Status handleReply(const HostAndPort& remoteHost, RemoteCommandResponse&& response) final {
        MONGO_UNREACHABLE;
    }
};


// Test that we time out a command if the connection hook hangs.
TEST_F(NetworkInterfaceASIOIntegrationTest, HookHangs) {
    NetworkInterfaceASIO::Options options;
    options.networkConnectionHook = stdx::make_unique<HangingHook>();
    startNet(std::move(options));

    assertCommandFailsOnClient(
        "admin", BSON("ping" << 1), Seconds(1), ErrorCodes::ExceededTimeLimit);
}

}  // namespace
}  // namespace executor
}  // namespace mongo
