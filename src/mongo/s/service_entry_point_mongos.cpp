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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kNetwork

#include <memory>

#include "mongo/platform/basic.h"

#include "mongo/s/service_entry_point_mongos.h"

#include "mongo/client/server_is_master_monitor.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/commands.h"
#include "mongo/db/curop.h"
#include "mongo/db/dbmessage.h"
#include "mongo/db/lasterror.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/request_execution_context.h"
#include "mongo/db/service_context.h"
#include "mongo/logv2/log.h"
#include "mongo/rpc/message.h"
#include "mongo/s/cluster_last_error_info.h"
#include "mongo/s/commands/strategy.h"

namespace mongo {

namespace {

BSONObj buildErrReply(const DBException& ex) {
    BSONObjBuilder errB;
    errB.append("$err", ex.what());
    errB.append("code", ex.code());
    return errB.obj();
}

}  // namespace

// Allows for decomposing `handleRequest` into parts and simplifies composing the future-chain.
struct HandleRequest : public std::enable_shared_from_this<HandleRequest> {
    struct OpRunnerBase;

    HandleRequest(OperationContext* opCtx, const Message& message)
        : rec(std::make_shared<RequestExecutionContext>(opCtx, message)),
          op(message.operation()),
          msgId(message.header().getId()),
          nsString(getNamespaceString(rec->getDbMessage())) {}

    // Prepares the environment for handling the request (e.g., setting up `ClusterLastErrorInfo`).
    void setupEnvironment();

    // Returns a future that does the heavy lifting of running client commands.
    Future<DbResponse> handleRequest();

    // Runs on successful execution of the future returned by `handleRequest`.
    void onSuccess(const DbResponse&);

    // Returns a future-chain to handle the request and prepare the response.
    Future<DbResponse> run();

    static NamespaceString getNamespaceString(const DbMessage& dbmsg) {
        if (!dbmsg.messageShouldHaveNs())
            return {};
        return NamespaceString(dbmsg.getns());
    }

    const std::shared_ptr<RequestExecutionContext> rec;
    const NetworkOp op;
    const int32_t msgId;
    const NamespaceString nsString;

    boost::optional<long long> slowMsOverride;
};

void HandleRequest::setupEnvironment() {
    using namespace fmt::literals;
    auto opCtx = rec->getOpCtx();

    // This exception will not be returned to the caller, but will be logged and will close the
    // connection
    uassert(ErrorCodes::IllegalOperation,
            "Message type {} is not supported."_format(op),
            isSupportedRequestNetworkOp(op) &&
                op != dbCompressed);  // Decompression should be handled above us.

    // Start a new LastError session. Any exceptions thrown from here onwards will be returned
    // to the caller (if the type of the message permits it).
    auto client = opCtx->getClient();
    if (!ClusterLastErrorInfo::get(client)) {
        ClusterLastErrorInfo::get(client) = std::make_shared<ClusterLastErrorInfo>();
    }
    ClusterLastErrorInfo::get(client)->newRequest();
    LastError::get(client).startRequest();
    AuthorizationSession::get(opCtx->getClient())->startRequest(opCtx);

    CurOp::get(opCtx)->ensureStarted();
}

// The base for various operation runners that handle the request, and often generate a DbResponse.
struct HandleRequest::OpRunnerBase {
    explicit OpRunnerBase(std::shared_ptr<HandleRequest> hr) : hr(std::move(hr)) {}
    virtual ~OpRunnerBase() = default;
    virtual Future<DbResponse> run() = 0;
    const std::shared_ptr<HandleRequest> hr;
};

struct CommandOpRunner final : public HandleRequest::OpRunnerBase {
    using HandleRequest::OpRunnerBase::OpRunnerBase;
    Future<DbResponse> run() override {
        return Strategy::clientCommand(hr->rec).tap([hr = hr](const DbResponse&) {
            // Hello should take kMaxAwaitTimeMs at most, log if it takes twice that.
            if (auto command = CurOp::get(hr->rec->getOpCtx())->getCommand();
                command && (command->getName() == "hello")) {
                hr->slowMsOverride =
                    2 * durationCount<Milliseconds>(SingleServerIsMasterMonitor::kMaxAwaitTime);
            }
        });
    }
};

// The base for operations that may throw exceptions, but should not cause the connection to close.
struct OpRunner : public HandleRequest::OpRunnerBase {
    using HandleRequest::OpRunnerBase::OpRunnerBase;
    virtual DbResponse runOperation() = 0;
    Future<DbResponse> run() override;
};

Future<DbResponse> OpRunner::run() try {
    using namespace fmt::literals;
    const NamespaceString& nss = hr->nsString;
    const DbMessage& dbm = hr->rec->getDbMessage();

    if (dbm.messageShouldHaveNs()) {
        uassert(ErrorCodes::InvalidNamespace, "Invalid ns [{}]"_format(nss.ns()), nss.isValid());

        uassert(ErrorCodes::IllegalOperation,
                "Can't use 'local' database through mongos",
                nss.db() != NamespaceString::kLocalDb);
    }

    LOGV2_DEBUG(22867,
                3,
                "Request::process begin ns: {namespace} msg id: {msgId} op: {operation}",
                "Starting operation",
                "namespace"_attr = nss,
                "msgId"_attr = hr->msgId,
                "operation"_attr = networkOpToString(hr->op));

    auto dbResponse = runOperation();

    LOGV2_DEBUG(22868,
                3,
                "Request::process end ns: {namespace} msg id: {msgId} op: {operation}",
                "Done processing operation",
                "namespace"_attr = nss,
                "msgId"_attr = hr->msgId,
                "operation"_attr = networkOpToString(hr->op));

    return Future<DbResponse>::makeReady(std::move(dbResponse));
} catch (const DBException& ex) {
    LOGV2_DEBUG(22869,
                1,
                "Exception thrown while processing {operation} op for {namespace}: {error}",
                "Got an error while processing operation",
                "operation"_attr = networkOpToString(hr->op),
                "namespace"_attr = hr->nsString.ns(),
                "error"_attr = ex);

    DbResponse dbResponse;
    if (hr->op == dbQuery || hr->op == dbGetMore) {
        dbResponse = replyToQuery(buildErrReply(ex), ResultFlag_ErrSet);
    } else {
        // No Response.
    }

    // We *always* populate the last error for now
    auto opCtx = hr->rec->getOpCtx();
    LastError::get(opCtx->getClient()).setLastError(ex.code(), ex.what());

    CurOp::get(opCtx)->debug().errInfo = ex.toStatus();

    return Future<DbResponse>::makeReady(std::move(dbResponse));
}

struct QueryOpRunner final : public OpRunner {
    using OpRunner::OpRunner;
    DbResponse runOperation() override {
        // Commands are handled through CommandOpRunner and Strategy::clientCommand().
        invariant(!hr->nsString.isCommand());
        hr->rec->getOpCtx()->markKillOnClientDisconnect();
        return Strategy::queryOp(hr->rec->getOpCtx(), hr->nsString, &hr->rec->getDbMessage());
    }
};

struct GetMoreOpRunner final : public OpRunner {
    using OpRunner::OpRunner;
    DbResponse runOperation() override {
        return Strategy::getMore(hr->rec->getOpCtx(), hr->nsString, &hr->rec->getDbMessage());
    }
};

struct KillCursorsOpRunner final : public OpRunner {
    using OpRunner::OpRunner;
    DbResponse runOperation() override {
        Strategy::killCursors(hr->rec->getOpCtx(), &hr->rec->getDbMessage());  // No Response.
        return {};
    }
};

struct WriteOpRunner final : public OpRunner {
    using OpRunner::OpRunner;
    DbResponse runOperation() override {
        Strategy::writeOp(hr->rec->getOpCtx(), &hr->rec->getDbMessage());  // No Response.
        return {};
    }
};

Future<DbResponse> HandleRequest::handleRequest() {
    switch (op) {
        case dbQuery:
            if (!nsString.isCommand())
                return std::make_unique<QueryOpRunner>(shared_from_this())->run();
        // FALLTHROUGH: it's a query containing a command
        case dbMsg:
            return std::make_unique<CommandOpRunner>(shared_from_this())->run();
        case dbGetMore:
            return std::make_unique<GetMoreOpRunner>(shared_from_this())->run();
        case dbKillCursors:
            return std::make_unique<KillCursorsOpRunner>(shared_from_this())->run();
        case dbInsert:
        case dbUpdate:
        case dbDelete:
            return std::make_unique<WriteOpRunner>(shared_from_this())->run();
        default:
            MONGO_UNREACHABLE;
    }
}

void HandleRequest::onSuccess(const DbResponse& dbResponse) {
    auto opCtx = rec->getOpCtx();
    // Mark the op as complete, populate the response length, and log it if appropriate.
    CurOp::get(opCtx)->completeAndLogOperation(
        opCtx, logv2::LogComponent::kCommand, dbResponse.response.size(), slowMsOverride);
}

Future<DbResponse> HandleRequest::run() {
    auto fp = makePromiseFuture<void>();
    auto future = std::move(fp.future)
                      .then([this, anchor = shared_from_this()] { setupEnvironment(); })
                      .then([this, anchor = shared_from_this()] { return handleRequest(); })
                      .tap([this, anchor = shared_from_this()](const DbResponse& dbResponse) {
                          onSuccess(dbResponse);
                      })
                      .tapError([](Status status) {
                          LOGV2(4879803, "Failed to handle request", "error"_attr = redact(status));
                      });
    fp.promise.emplaceValue();
    return future;
}

Future<DbResponse> ServiceEntryPointMongos::handleRequest(OperationContext* opCtx,
                                                          const Message& message) noexcept {
    auto hr = std::make_shared<HandleRequest>(opCtx, message);
    return hr->run();
}

}  // namespace mongo
