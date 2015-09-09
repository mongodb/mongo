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

#include "mongo/executor/connection_pool_asio.h"

#include "mongo/client/connection_string.h"
#include "mongo/executor/async_stream_factory.h"
#include "mongo/executor/network_connection_hook.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/stdx/future.h"
#include "mongo/stdx/memory.h"
#include "mongo/unittest/integration_test.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/scopeguard.h"

namespace mongo {
namespace executor {
namespace {

class MyNetworkConnectionHook : public NetworkConnectionHook {
public:
    Status validateHost(const HostAndPort& remoteHost,
                        const RemoteCommandResponse& isMasterReply) override {
        stdx::unique_lock<stdx::mutex> lk(_mutex);

        _count++;

        return Status::OK();
    }

    StatusWith<boost::optional<RemoteCommandRequest>> makeRequest(
        const HostAndPort& remoteHost) override {
        return StatusWith<boost::optional<RemoteCommandRequest>>(boost::none);
    }

    Status handleReply(const HostAndPort& remoteHost, RemoteCommandResponse&& response) override {
        MONGO_UNREACHABLE;
    }

    static int count() {
        stdx::unique_lock<stdx::mutex> lk(_mutex);

        return _count;
    }

private:
    static stdx::mutex _mutex;
    static int _count;
};

stdx::mutex MyNetworkConnectionHook::_mutex;
int MyNetworkConnectionHook::_count = 0;

TEST(ConnectionPoolASIO, TestPing) {
    auto fixture = unittest::getFixtureConnectionString();

    NetworkInterfaceASIO::Options options;
    options.connectionPoolOptions.maxConnections = 10;

    NetworkInterfaceASIO net{stdx::make_unique<AsyncStreamFactory>(),
                             stdx::make_unique<MyNetworkConnectionHook>(),
                             options};

    net.startup();
    auto guard = MakeGuard([&] { net.shutdown(); });

    const int N = 50;

    std::array<stdx::thread, N> threads;

    for (auto& thread : threads) {
        thread = stdx::thread([&net, &fixture]() {
            auto status = Status::OK();
            stdx::promise<void> result;

            net.startCommand(TaskExecutor::CallbackHandle(),
                             RemoteCommandRequest{
                                 fixture.getServers()[0], "admin", BSON("ping" << 1), BSONObj()},
                             [&result, &status](StatusWith<RemoteCommandResponse> resp) {
                                 if (!resp.isOK()) {
                                     status = std::move(resp.getStatus());
                                 }
                                 result.set_value();
                             });

            result.get_future().get();

            ASSERT_OK(status);
        });
    }

    for (auto&& thread : threads) {
        thread.join();
    }

    ASSERT_LTE(MyNetworkConnectionHook::count(), 10);
}

}  // namespace
}  // namespace executor
}  // namespace mongo
