/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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

#pragma once

#include <memory>
#include <string>
#include <vector>

#include "mongo/base/error_codes.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/client/async_client.h"
#include "mongo/client/connection_string.h"
#include "mongo/db/database_name.h"
#include "mongo/db/service_context.h"
#include "mongo/executor/remote_command_response.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/stdx/thread.h"
#include "mongo/transport/transport_layer.h"
#include "mongo/unittest/assert.h"
#include "mongo/unittest/framework.h"
#include "mongo/unittest/integration_test.h"
#include "mongo/util/duration.h"
#include "mongo/util/future.h"
#include "mongo/util/interruptible.h"
#include "mongo/util/net/hostandport.h"
#include "mongo/util/uuid.h"

#include <boost/none.hpp>

namespace mongo::transport {

class AsyncClientIntegrationTestFixture : public unittest::Test {
public:
    static BSONObj assertOK(executor::RemoteCommandResponse resp) {
        ASSERT_OK(resp.status);
        ASSERT_OK(getStatusFromCommandResult(resp.data));
        return resp.data;
    };

    static HostAndPort getServer() {
        return unittest::getFixtureConnectionString().getServers().front();
    }

    ServiceContext* getServiceContext() {
        return getGlobalServiceContext();
    }

    std::shared_ptr<Reactor> getReactor() {
        return _reactor;
    }

    std::shared_ptr<AsyncDBClient> makeClient();

    static executor::RemoteCommandRequest makeTestRequest(
        DatabaseName dbName,
        BSONObj cmdObj,
        boost::optional<UUID> clientOperationKey = boost::none,
        Milliseconds timeout = executor::RemoteCommandRequest::kNoTimeout);

    executor::RemoteCommandRequest makeExhaustHello(
        Milliseconds maxAwaitTime, boost::optional<UUID> clientOperationKey = boost::none);

    class FailPointGuard {
    public:
        FailPointGuard(StringData fpName,
                       std::shared_ptr<AsyncDBClient> client,
                       int initalTimesEntered)
            : _fpName(fpName),
              _client(std::move(client)),
              _initialTimesEntered(initalTimesEntered) {}

        FailPointGuard(const FailPointGuard&) = delete;
        FailPointGuard& operator=(const FailPointGuard&) = delete;

        ~FailPointGuard() {
            auto cmdObj = BSON("configureFailPoint" << _fpName << "mode"
                                                    << "off");
            assertOK(
                _client->runCommandRequest(makeTestRequest(DatabaseName::kAdmin, cmdObj)).get());
        }

        void waitForTimesEntered(Interruptible* interruptible, int count) {
            auto cmdObj =
                BSON("waitForFailPoint" << _fpName << "timesEntered" << _initialTimesEntered + count
                                        << "maxTimeMS" << 30000);
            assertOK(_client->runCommandRequest(makeTestRequest(DatabaseName::kAdmin, cmdObj))
                         .get(interruptible));
        }

        void waitForTimesEntered(int count) {
            waitForTimesEntered(Interruptible::notInterruptible(), count);
        }

    private:
        std::string _fpName;
        std::shared_ptr<AsyncDBClient> _client;
        int _initialTimesEntered;
    };

    FailPointGuard configureFailPoint(const std::shared_ptr<AsyncDBClient>& client,
                                      StringData fp,
                                      BSONObj data);

    FailPointGuard configureFailCommand(const std::shared_ptr<AsyncDBClient>& client,
                                        StringData failCommand,
                                        boost::optional<ErrorCodes::Error> errorCode = boost::none,
                                        boost::optional<Milliseconds> blockTime = boost::none);

    void killOp(AsyncDBClient& client, UUID opKey);

    /**
     * Returns a Baton that can be used to run commands on, or nullptr for reactor-only operation.
     */
    virtual BatonHandle baton() {
        return nullptr;
    }

    /** Returns an Interruptible appropriate for the Baton returned from baton(). */
    virtual Interruptible* interruptible() {
        return Interruptible::notInterruptible();
    }

protected:
    ReactorHandle _reactor;
    stdx::thread _reactorThread;

public:
    void testShortReadsAndWritesWork();
    void testAsyncConnectTimeoutCleansUpSocket();
    void testExhaustHelloShouldReceiveMultipleReplies();
    void testExhaustHelloShouldStopOnFailure();
    void testRunCommandRequest();
    void testRunCommandRequestCancel();
    void testRunCommandRequestCancelEarly();
    void testRunCommandRequestCancelSourceDismissed();
    void testRunCommandCancelBetweenSendAndRead();
    void testExhaustCommand();
    void testBeginExhaustCommandRequestCancel();
    void testBeginExhaustCommandCancelEarly();
    void testAwaitExhaustCommandCancel();
    void testAwaitExhaustCommandCancelEarly();
    void testEgressNetworkMetrics();
};

}  // namespace mongo::transport
