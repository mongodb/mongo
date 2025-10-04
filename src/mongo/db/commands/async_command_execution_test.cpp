/**
 *    Copyright (C) 2020-present MongoDB, Inc.
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
#include "mongo/base/status_with.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/client.h"
#include "mongo/db/client_strand.h"
#include "mongo/db/commands.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/request_execution_context.h"
#include "mongo/db/service_context.h"
#include "mongo/db/service_context_test_fixture.h"
#include "mongo/logv2/log.h"
#include "mongo/rpc/factory.h"
#include "mongo/rpc/op_msg.h"
#include "mongo/rpc/protocol.h"
#include "mongo/rpc/reply_builder_interface.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/future.h"

#include <memory>
#include <mutex>
#include <utility>

#include <boost/move/utility_core.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>
#include <fmt/format.h>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest


namespace mongo {
namespace {

class AsyncCommandExecutionTest : public unittest::Test, public ScopedGlobalServiceContextForTest {
public:
    struct TestState;
    void runTestForCommand(StringData command);
};

// Sets up and maintains the environment (e.g., `opCtx`) required for running a test.
struct AsyncCommandExecutionTest::TestState {
    TestState(ClientStrandPtr clientStrand, StringData cmdName) : strand(std::move(clientStrand)) {
        auto guard = strand->bind();
        opCtx = guard->makeOperationContext();

        auto mockMessage = [&] {
            OpMsgBuilder builder;
            builder.setBody(BSON(cmdName << 1 << "$db"
                                         << "test"));
            return builder.finish();
        };

        // Setup the execution context
        rec = std::make_shared<RequestExecutionContext>(
            opCtx.get(), mockMessage(), opCtx.get()->fastClockSource().now());
        rec->setReplyBuilder(makeReplyBuilder(rpc::protocolForMessage(rec->getMessage())));
        rec->setRequest(rpc::opMsgRequestFromAnyProtocol(rec->getMessage(), opCtx->getClient()));
        rec->setCommand(CommandHelpers::findCommand(&*opCtx, rec->getRequest().getCommandName()));

        // Setup the invocation
        auto cmd = rec->getCommand();
        invariant(cmd);
        invocation = cmd->parse(opCtx.get(), rec->getRequest());
    }

    ~TestState() {
        // Deleting the `opCtx` will modify the `Client`, so we must bind the strand first.
        auto guard = strand->bind();
        opCtx.reset();
    }

    ClientStrandPtr strand;
    ServiceContext::UniqueOperationContext opCtx;
    std::shared_ptr<RequestExecutionContext> rec;
    std::shared_ptr<CommandInvocation> invocation;
};

BSONObj getSyncResponse(AsyncCommandExecutionTest::TestState& state) {
    state.invocation->run(state.rec->getOpCtx(), state.rec->getReplyBuilder());
    return state.rec->getReplyBuilder()->getBodyBuilder().done().getOwned();
}

BSONObj getAsyncResponse(AsyncCommandExecutionTest::TestState& state) {
    Future<void> future;
    {
        auto guard = state.strand->bind();
        FailPointEnableBlock fp("hangBeforeRunningAsyncRequestExecutorTask");
        future = state.invocation->runAsync(state.rec);
        ASSERT(!future.isReady());
    }

    ASSERT(future.getNoThrow().isOK());

    return [&] {
        auto guard = state.strand->bind();
        return state.rec->getReplyBuilder()->getBodyBuilder().done().getOwned();
    }();
}

void killAsyncCommand(AsyncCommandExecutionTest::TestState& state) {
    Future<void> future;
    {
        auto guard = state.strand->bind();
        FailPointEnableBlock fp("hangBeforeRunningAsyncRequestExecutorTask");
        future = state.invocation->runAsync(state.rec);

        auto opCtx = state.rec->getOpCtx();
        ClientLock lk(opCtx->getClient());
        opCtx->getServiceContext()->killOperation(lk, opCtx, ErrorCodes::Interrupted);
    }

    ASSERT_EQ(future.getNoThrow().code(), ErrorCodes::Interrupted);
}

void AsyncCommandExecutionTest::runTestForCommand(StringData command) {
    BSONObj syncResponse, asyncResponse;

    auto client = getServiceContext()->getService()->makeClient("Client");
    auto strand = ClientStrand::make(std::move(client));

    {
        LOGV2(5399301, "Running the command synchronously", "command"_attr = command);
        TestState state(strand, command);
        strand->run([&] { syncResponse = getSyncResponse(state); });
    }

    {
        LOGV2(5399302, "Running the command asynchronously", "command"_attr = command);
        TestState state(strand, command);
        asyncResponse = getAsyncResponse(state);
    }

    {
        LOGV2(5399303, "Canceling the command running asynchronously", "command"_attr = command);
        TestState state(strand, command);
        killAsyncCommand(state);
    }

    ASSERT_BSONOBJ_EQ(syncResponse, asyncResponse);
}

TEST_F(AsyncCommandExecutionTest, BuildInfo) {
    runTestForCommand("buildinfo");
}

}  // namespace
}  // namespace mongo
