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

#include <exception>

#include "mongo/base/status_with.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/client/connection_string.h"
#include "mongo/executor/async_stream_factory.h"
#include "mongo/executor/async_stream_interface.h"
#include "mongo/executor/async_timer_asio.h"
#include "mongo/executor/network_interface_asio.h"
#include "mongo/executor/network_interface_asio_test_utils.h"
#include "mongo/executor/task_executor.h"
#include "mongo/stdx/memory.h"
#include "mongo/unittest/integration_test.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/log.h"
#include "mongo/util/net/hostandport.h"
#include "mongo/util/scopeguard.h"
#include "mongo/util/timer.h"

namespace mongo {
namespace executor {
namespace {

const std::size_t numOperations = 16384;


int timeNetworkTestMillis(std::size_t operations, NetworkInterface* net) {
    net->startup();
    auto guard = MakeGuard([&] { net->shutdown(); });

    auto fixture = unittest::getFixtureConnectionString();
    auto server = fixture.getServers()[0];

    std::atomic<int> remainingOps(operations);  // NOLINT
    stdx::mutex mtx;
    stdx::condition_variable cv;
    Timer t;

    // This lambda function is declared here since it is mutually recursive with another callback
    // function
    stdx::function<void()> func;

    const auto bsonObjPing = BSON("ping" << 1);

    const auto callback = [&](StatusWith<RemoteCommandResponse> resp) {
        uassertStatusOK(resp);
        if (--remainingOps) {
            return func();
        }
        stdx::unique_lock<stdx::mutex> lk(mtx);
        cv.notify_one();
    };

    func = [&]() {
        net->startCommand(makeCallbackHandle(),
                          {server, "admin", bsonObjPing, bsonObjPing, Milliseconds(-1)},
                          callback);
    };

    func();

    stdx::unique_lock<stdx::mutex> lk(mtx);
    cv.wait(lk, [&] { return remainingOps.load() == 0; });

    return t.millis();
}

TEST(NetworkInterfaceASIO, SerialPerf) {
    NetworkInterfaceASIO::Options options{};
    options.streamFactory = stdx::make_unique<AsyncStreamFactory>();
    options.timerFactory = stdx::make_unique<AsyncTimerFactoryASIO>();
    NetworkInterfaceASIO netAsio{std::move(options)};

    int duration = timeNetworkTestMillis(numOperations, &netAsio);
    int result = numOperations * 1000 / duration;
    log() << "THROUGHPUT asio ping ops/s: " << result << std::endl;
}

}  // namespace
}  // namespace executor
}  // namespace mongo
