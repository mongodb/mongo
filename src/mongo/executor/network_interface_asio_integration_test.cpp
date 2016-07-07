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

class NetworkInterfaceASIOIntegrationTest : public mongo::unittest::Test {
public:
    void startNet(NetworkInterfaceASIO::Options options = NetworkInterfaceASIO::Options()) {
        options.streamFactory = stdx::make_unique<AsyncStreamFactory>();
        options.timerFactory = stdx::make_unique<AsyncTimerFactoryASIO>();
        options.connectionPoolOptions.maxConnections = 256u;
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

    Deferred<StatusWith<RemoteCommandResponse>> runCommand(
        const TaskExecutor::CallbackHandle& cbHandle, const RemoteCommandRequest& request) {
        Deferred<StatusWith<RemoteCommandResponse>> deferred;
        net().startCommand(
            cbHandle, request, [deferred](StatusWith<RemoteCommandResponse> resp) mutable {
                deferred.emplace(std::move(resp));
            });
        return deferred;
    }

    StatusWith<RemoteCommandResponse> runCommandSync(const RemoteCommandRequest& request) {
        auto deferred = runCommand(makeCallbackHandle(), request);
        auto& res = deferred.get();
        if (res.isOK()) {
            log() << "got command result: " << res.getValue().toString();
        } else {
            log() << "command failed: " << res.getStatus();
        }
        return res;
    }

    void assertCommandOK(StringData db,
                         const BSONObj& cmd,
                         Milliseconds timeoutMillis = Milliseconds(-1)) {
        auto res = unittest::assertGet(runCommandSync(
            {fixture().getServers()[0], db.toString(), cmd, BSONObj(), timeoutMillis}));
        ASSERT_OK(getStatusFromCommandResult(res.data));
    }

    void assertCommandFailsOnClient(StringData db,
                                    const BSONObj& cmd,
                                    Milliseconds timeoutMillis,
                                    ErrorCodes::Error reason) {
        auto clientStatus = runCommandSync(
            {fixture().getServers()[0], db.toString(), cmd, BSONObj(), timeoutMillis});
        ASSERT_TRUE(clientStatus == reason);
    }

    void assertCommandFailsOnServer(StringData db,
                                    const BSONObj& cmd,
                                    Milliseconds timeoutMillis,
                                    ErrorCodes::Error reason) {
        auto res = unittest::assertGet(runCommandSync(
            {fixture().getServers()[0], db.toString(), cmd, BSONObj(), timeoutMillis}));
        auto serverStatus = getStatusFromCommandResult(res.data);
        ASSERT_TRUE(serverStatus == reason);
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

    Deferred<Status> run(Fixture* fixture,
                         Pool* pool,
                         Milliseconds timeout = RemoteCommandRequest::kNoTimeout) {
        auto cb = makeCallbackHandle();
        auto self = *this;
        auto out = fixture
                       ->runCommand(cb,
                                    {unittest::getFixtureConnectionString().getServers()[0],
                                     "admin",
                                     _command,
                                     timeout})
                       .then(pool, [self](StatusWith<RemoteCommandResponse> resp) -> Status {
                           auto status = resp.isOK()
                               ? getStatusFromCommandResult(resp.getValue().data)
                               : resp.getStatus();

                           return status == self._expected
                               ? Status::OK()
                               : Status{ErrorCodes::BadValue,
                                        str::stream() << "Expected "
                                                      << ErrorCodes::errorString(self._expected)
                                                      << " but got "
                                                      << status.toString()};
                       });
        if (_cancel) {
            invariant(fixture->randomNumberGenerator());
            sleepmillis(fixture->randomNumberGenerator()->nextInt32(10));
            fixture->net().cancelCommand(cb);
        }
        return out;
    }

    static Deferred<Status> runTimeoutOp(Fixture* fixture, Pool* pool) {
        return StressTestOp(BSON("sleep" << 1 << "lock"
                                         << "none"
                                         << "secs"
                                         << 1),
                            ErrorCodes::ExceededTimeLimit,
                            false)
            .run(fixture, pool, Milliseconds(100));
    }

    static Deferred<Status> runCompleteOp(Fixture* fixture, Pool* pool) {
        return StressTestOp(BSON("sleep" << 1 << "lock"
                                         << "none"
                                         << "millis"
                                         << 100),
                            ErrorCodes::OK,
                            false)
            .run(fixture, pool);
    }

    static Deferred<Status> runCancelOp(Fixture* fixture, Pool* pool) {
        return StressTestOp(BSON("sleep" << 1 << "lock"
                                         << "none"
                                         << "secs"
                                         << 10),
                            ErrorCodes::CallbackCanceled,
                            true)
            .run(fixture, pool);
    }

    static Deferred<Status> runLongOp(Fixture* fixture, Pool* pool) {
        return StressTestOp(BSON("sleep" << 1 << "lock"
                                         << "none"
                                         << "secs"
                                         << 30),
                            ErrorCodes::OK,
                            false)
            .run(fixture, pool, RemoteCommandRequest::kNoTimeout);
    }

private:
    StressTestOp(const BSONObj& command, ErrorCodes::Error expected, bool cancel)
        : _command(command), _expected(expected), _cancel(cancel) {}

    BSONObj _command;
    ErrorCodes::Error _expected;
    bool _cancel;
};

TEST_F(NetworkInterfaceASIOIntegrationTest, StressTest) {
    startNet();
    const std::size_t numOps = 10000;
    std::vector<Deferred<Status>> ops;

    std::unique_ptr<SecureRandom> seedSource{SecureRandom::create()};
    auto seed = seedSource->nextInt64();

    log() << "Random seed is " << seed;
    auto rng = PseudoRandom(seed);  // TODO: read from command line
    randomNumberGenerator(&rng);
    log() << "Starting stress test...";

    ThreadPool::Options threadPoolOpts;
    threadPoolOpts.poolName = "StressTestPool";
    threadPoolOpts.maxThreads = 8;
    ThreadPool pool(threadPoolOpts);
    pool.startup();

    auto poolGuard = MakeGuard([&pool] {
        pool.schedule([&pool] { pool.shutdown(); });
        pool.join();
    });

    std::generate_n(std::back_inserter(ops), numOps, [&rng, &pool, this] {

        // stagger operations slightly to mitigate connection pool contention
        sleepmillis(rng.nextInt32(10));

        auto i = rng.nextCanonicalDouble();

        if (i < .3) {
            return StressTestOp::runCancelOp(this, &pool);
        } else if (i < .7) {
            return StressTestOp::runCompleteOp(this, &pool);
        } else if (i < .99) {
            return StressTestOp::runTimeoutOp(this, &pool);
        } else {
            // Just a sprinkling of long ops, to mitigate connection pool contention
            return StressTestOp::runLongOp(this, &pool);
        }
    });

    log() << "running ops";
    auto res = helpers::collect(ops, &pool)
                   .then(&pool,
                         [](std::vector<Status> opResults) -> Status {
                             for (const auto& opResult : opResults) {
                                 if (!opResult.isOK()) {
                                     return opResult;
                                 }
                             }
                             return Status::OK();
                         })
                   .get();
    ASSERT_OK(res);
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
                                                          BSONObj()))};
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
