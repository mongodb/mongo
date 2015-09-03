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
#include "mongo/util/scopeguard.h"

namespace mongo {
namespace executor {
namespace {

TEST(NetworkInterfaceASIO, TestPing) {
    auto fixture = unittest::getFixtureConnectionString();

    NetworkInterfaceASIO::Options options{};
    options.streamFactory = stdx::make_unique<AsyncStreamFactory>();
    options.timerFactory = stdx::make_unique<AsyncTimerFactoryASIO>();
    NetworkInterfaceASIO net{std::move(options)};

    net.startup();
    auto guard = MakeGuard([&] { net.shutdown(); });

    TaskExecutor::CallbackHandle cb{};

    stdx::promise<RemoteCommandResponse> result;

    net.startCommand(
        cb,
        RemoteCommandRequest{fixture.getServers()[0], "admin", BSON("ping" << 1), BSONObj()},
        [&result](StatusWith<RemoteCommandResponse> resp) {
            try {
                result.set_value(uassertStatusOK(resp));
            } catch (...) {
                result.set_exception(std::current_exception());
            }
        });

    auto fut = result.get_future();
    auto commandReply = fut.get();

    ASSERT_OK(getStatusFromCommandResult(commandReply.data));
}

}  // namespace
}  // namespace executor
}  // namespace mongo
