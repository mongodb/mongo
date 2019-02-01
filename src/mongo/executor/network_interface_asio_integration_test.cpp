
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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kASIO

#include "mongo/platform/basic.h"

#include <algorithm>
#include <exception>

#include "mongo/client/connection_string.h"
#include "mongo/executor/network_interface_asio_integration_fixture.h"
#include "mongo/executor/network_interface_asio_test_utils.h"
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

TEST_F(NetworkInterfaceASIOIntegrationFixture, Ping) {
    startNet();
    assertCommandOK("admin", BSON("ping" << 1));
}

TEST_F(NetworkInterfaceASIOIntegrationFixture, Timeouts) {
    startNet();
    // This sleep command will take 10 seconds, so we should time out client side first given
    // our timeout of 100 milliseconds.
    assertCommandFailsOnClient("admin",
                               BSON("sleep" << 1 << "lock"
                                            << "none"
                                            << "secs"
                                            << 10),
                               ErrorCodes::NetworkInterfaceExceededTimeLimit,
                               Milliseconds(100));

    // Run a sleep command that should return before we hit the ASIO timeout.
    assertCommandOK("admin",
                    BSON("sleep" << 1 << "lock"
                                 << "none"
                                 << "secs"
                                 << 1),
                    Milliseconds(10000000));
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
TEST_F(NetworkInterfaceASIOIntegrationFixture, HookHangs) {
    NetworkInterfaceASIO::Options options;
    options.networkConnectionHook = stdx::make_unique<HangingHook>();
    startNet(std::move(options));

    assertCommandFailsOnClient(
        "admin", BSON("ping" << 1), ErrorCodes::NetworkInterfaceExceededTimeLimit, Seconds(1));
}

}  // namespace
}  // namespace executor
}  // namespace mongo
