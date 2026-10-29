// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

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
#include "mongo/unittest/integration_test.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/duration.h"
#include "mongo/util/future.h"
#include "mongo/util/interruptible.h"
#include "mongo/util/net/hostandport.h"
#include "mongo/util/uuid.h"

#include <memory>
#include <string>
#include <string_view>
#include <vector>

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

    virtual TransportLayer* getTransportLayer(ServiceContext* svc) const = 0;

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
        FailPointGuard(std::string_view fpName,
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
                                      std::string_view fp,
                                      BSONObj data);

    FailPointGuard configureFailCommand(const std::shared_ptr<AsyncDBClient>& client,
                                        std::string_view failCommand,
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
