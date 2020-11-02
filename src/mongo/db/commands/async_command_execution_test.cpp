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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest

#include <fmt/format.h>

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/client.h"
#include "mongo/db/client_strand.h"
#include "mongo/db/commands.h"
#include "mongo/db/request_execution_context.h"
#include "mongo/db/service_context_test_fixture.h"
#include "mongo/logv2/log.h"
#include "mongo/rpc/factory.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/fail_point.h"

namespace mongo {
namespace {

using namespace fmt::literals;

class AsyncCommandExecutionTest : public unittest::Test, public ScopedGlobalServiceContextForTest {
public:
    void runTestForCommand(StringData command) {
        BSONObj syncResponse, asyncResponse;

        auto client = getServiceContext()->makeClient("Client");
        auto strand = ClientStrand::make(std::move(client));

        {
            auto ctx = makeExecutionContext(strand, command);
            strand->run([&] { syncResponse = getSyncResponse(ctx); });
        }

        {
            auto ctx = makeExecutionContext(strand, command);
            asyncResponse = getAsyncResponse(strand, ctx);
        }

        {
            auto ctx = makeExecutionContext(strand, command);
            killAsyncCommand(strand, ctx);
        }

        ASSERT_BSONOBJ_EQ(syncResponse, asyncResponse);
    }

private:
    struct ExecutionContext {
        ServiceContext::UniqueOperationContext opCtx;
        std::shared_ptr<RequestExecutionContext> rec;
        std::shared_ptr<CommandInvocation> invocation;
    };

    ExecutionContext makeExecutionContext(ClientStrandPtr strand, StringData commandName) const {
        auto guard = strand->bind();
        ExecutionContext ctx;
        ctx.opCtx = cc().makeOperationContext();

        auto rec =
            std::make_shared<RequestExecutionContext>(ctx.opCtx.get(), mockMessage(commandName));
        rec->setReplyBuilder(makeReplyBuilder(rpc::protocolForMessage(rec->getMessage())));
        rec->setRequest(rpc::opMsgRequestFromAnyProtocol(rec->getMessage()));
        rec->setCommand(CommandHelpers::findCommand(rec->getRequest().getCommandName()));

        auto cmd = rec->getCommand();
        invariant(cmd);
        ctx.invocation = cmd->parse(ctx.opCtx.get(), rec->getRequest());
        ctx.rec = std::move(rec);
        return ctx;
    }

    BSONObj getSyncResponse(ExecutionContext& ctx) const {
        ctx.invocation->run(ctx.rec->getOpCtx(), ctx.rec->getReplyBuilder());
        return ctx.rec->getReplyBuilder()->getBodyBuilder().done().getOwned();
    }

    BSONObj getAsyncResponse(ClientStrandPtr strand, ExecutionContext& ctx) const {
        Future<void> future;
        {
            auto guard = strand->bind();
            FailPointEnableBlock fp("hangBeforeRunningAsyncRequestExecutorTask");
            future = ctx.invocation->runAsync(ctx.rec);
            ASSERT(!future.isReady());
        }

        ASSERT(future.getNoThrow().isOK());

        return [&] {
            auto guard = strand->bind();
            return ctx.rec->getReplyBuilder()->getBodyBuilder().done().getOwned();
        }();
    }

    void killAsyncCommand(ClientStrandPtr strand, ExecutionContext& ctx) const {
        Future<void> future;
        {
            auto guard = strand->bind();
            FailPointEnableBlock fp("hangBeforeRunningAsyncRequestExecutorTask");
            future = ctx.invocation->runAsync(ctx.rec);

            auto opCtx = ctx.rec->getOpCtx();
            stdx::lock_guard<Client> lk(*opCtx->getClient());
            opCtx->getServiceContext()->killOperation(lk, opCtx, ErrorCodes::Interrupted);
        }

        ASSERT_EQ(future.getNoThrow().code(), ErrorCodes::Interrupted);
    }

    Message mockMessage(StringData commandName) const {
        OpMsgBuilder builder;
        builder.setBody(BSON(commandName << 1 << "$db"
                                         << "test"));
        return builder.finish();
    }
};

TEST_F(AsyncCommandExecutionTest, BuildInfo) {
    runTestForCommand("buildinfo");
}

}  // namespace
}  // namespace mongo
