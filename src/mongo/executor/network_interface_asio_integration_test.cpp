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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kExecutor

#include "mongo/platform/basic.h"

#include <exception>

#include "mongo/client/connection_string.h"
#include "mongo/executor/async_stream_factory.h"
#include "mongo/executor/async_stream_interface.h"
#include "mongo/executor/async_timer_asio.h"
#include "mongo/executor/network_interface_asio.h"
#include "mongo/executor/task_executor.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/stdx/future.h"
#include "mongo/stdx/memory.h"
#include "mongo/unittest/integration_test.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/log.h"
#include "mongo/util/scopeguard.h"

namespace mongo {
namespace executor {
namespace {

class NetworkInterfaceASIOIntegrationTest : public mongo::unittest::Test {
public:
    void setUp() override {
        NetworkInterfaceASIO::Options options{};
        options.streamFactory = stdx::make_unique<AsyncStreamFactory>();
        options.timerFactory = stdx::make_unique<AsyncTimerFactoryASIO>();
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

    StatusWith<RemoteCommandResponse> runCommand(const RemoteCommandRequest& request) {
        TaskExecutor::CallbackHandle cb{};
        stdx::promise<RemoteCommandResponse> result;

        log() << "running command: " << request.toString();

        net().startCommand(cb,
                           request,
                           [&result](StatusWith<RemoteCommandResponse> resp) {
                               try {
                                   result.set_value(uassertStatusOK(resp));
                               } catch (...) {
                                   result.set_exception(std::current_exception());
                               }
                           });
        try {
            // We can't construct a stdx::promise<StatusWith<RemoteCommandResponse>> because
            // StatusWith is not default constructible. So we do the status -> exception -> status
            // dance instead.
            auto res = result.get_future().get();
            log() << "got command result: " << res.toString();
            return res;
        } catch (...) {
            auto status = exceptionToStatus();
            log() << "command failed with status: " << status;
            return status;
        }
    }

    void assertCommandOK(StringData db,
                         const BSONObj& cmd,
                         Milliseconds timeoutMillis = Milliseconds(-1)) {
        auto res = unittest::assertGet(
            runCommand({fixture().getServers()[0], db.toString(), cmd, BSONObj(), timeoutMillis}));
        ASSERT_OK(getStatusFromCommandResult(res.data));
    }

    void assertCommandFailsOnClient(StringData db,
                                    const BSONObj& cmd,
                                    Milliseconds timeoutMillis,
                                    ErrorCodes::Error reason) {
        auto clientStatus =
            runCommand({fixture().getServers()[0], db.toString(), cmd, BSONObj(), timeoutMillis});
        ASSERT_TRUE(clientStatus == reason);
    }

    void assertCommandFailsOnServer(StringData db,
                                    const BSONObj& cmd,
                                    Milliseconds timeoutMillis,
                                    ErrorCodes::Error reason) {
        auto res = unittest::assertGet(
            runCommand({fixture().getServers()[0], db.toString(), cmd, BSONObj(), timeoutMillis}));
        auto serverStatus = getStatusFromCommandResult(res.data);
        ASSERT_TRUE(serverStatus == reason);
    }

private:
    std::unique_ptr<NetworkInterfaceASIO> _net;
};

TEST_F(NetworkInterfaceASIOIntegrationTest, Ping) {
    assertCommandOK("admin", BSON("ping" << 1));
}

TEST_F(NetworkInterfaceASIOIntegrationTest, Timeouts) {
    // Insert 1 document in collection foo.bar. If we don't do this our queries will return
    // immediately.
    assertCommandOK("foo",
                    BSON("insert"
                         << "bar"
                         << "documents" << BSON_ARRAY(BSON("foo" << 1))));

    // Run a find command with a $where with an infinite loop. The remote server should time this
    // out in 30 seconds, so we should time out client side first given our timeout of 100
    // milliseconds.
    assertCommandFailsOnClient("foo",
                               BSON("find"
                                    << "bar"
                                    << "filter" << BSON("$where"
                                                        << "while(true) { sleep(1); }")),
                               Milliseconds(100),
                               ErrorCodes::ExceededTimeLimit);

    // Run a find command with a $where with an infinite loop. The server should time out the
    // command.
    assertCommandFailsOnServer("foo",
                               BSON("find"
                                    << "bar"
                                    << "filter" << BSON("$where"
                                                        << "while(true) { sleep(1); };")),
                               Milliseconds(10000000000),  // big, big timeout.
                               ErrorCodes::JSInterpreterFailure);

    // Run a find command with a big timeout. It should return before we hit the ASIO timeout.
    assertCommandOK("foo",
                    BSON("find"
                         << "bar"
                         << "limit" << 1),
                    Milliseconds(10000000));
}

}  // namespace
}  // namespace executor
}  // namespace mongo
