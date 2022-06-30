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


#include <memory>

#include "mongo/platform/basic.h"

#include "mongo/s/service_entry_point_mongos.h"

#include "mongo/client/server_discovery_monitor.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/commands.h"
#include "mongo/db/curop.h"
#include "mongo/db/dbmessage.h"
#include "mongo/db/not_primary_error_tracker.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/request_execution_context.h"
#include "mongo/db/service_context.h"
#include "mongo/db/session_catalog.h"
#include "mongo/db/stats/counters.h"
#include "mongo/logv2/log.h"
#include "mongo/rpc/message.h"
#include "mongo/rpc/warn_unsupported_wire_ops.h"
#include "mongo/s/commands/strategy.h"
#include "mongo/s/grid.h"
#include "mongo/s/load_balancer_support.h"
#include "mongo/s/query/cluster_cursor_manager.h"
#include "mongo/s/transaction_router.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kNetwork


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
    struct CommandOpRunner;

    HandleRequest(OperationContext* opCtx, const Message& message)
        : rec(std::make_shared<RequestExecutionContext>(opCtx, message)),
          op(message.operation()),
          msgId(message.header().getId()),
          nsString(getNamespaceString(rec->getDbMessage())) {}

    // Prepares the environment for handling the request.
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

    // Start a new NotPrimaryErrorTracker session. Any exceptions thrown from here onwards will be
    // returned to the caller (if the type of the message permits it).
    auto client = opCtx->getClient();
    NotPrimaryErrorTracker::get(client).startRequest();
    AuthorizationSession::get(client)->startRequest(opCtx);

    CurOp::get(opCtx)->ensureStarted();
}

struct HandleRequest::CommandOpRunner {
    explicit CommandOpRunner(std::shared_ptr<HandleRequest> hr) : hr(std::move(hr)) {}
    Future<DbResponse> run() {
        return Strategy::clientCommand(hr->rec);
    }
    const std::shared_ptr<HandleRequest> hr;
};

Future<DbResponse> HandleRequest::handleRequest() {
    switch (op) {
        case dbQuery:
            if (!nsString.isCommand()) {
                globalOpCounters.gotQueryDeprecated();
                warnUnsupportedOp(*(rec->getOpCtx()->getClient()), networkOpToString(dbQuery));
                return Future<DbResponse>::makeReady(
                    makeErrorResponseToUnsupportedOpQuery("OP_QUERY is no longer supported"));
            }
            [[fallthrough]];  // It's a query containing a command
        case dbMsg:
            return std::make_unique<CommandOpRunner>(shared_from_this())->run();
        case dbGetMore: {
            globalOpCounters.gotGetMoreDeprecated();
            warnUnsupportedOp(*(rec->getOpCtx()->getClient()), networkOpToString(dbGetMore));
            return Future<DbResponse>::makeReady(
                makeErrorResponseToUnsupportedOpQuery("OP_GET_MORE is no longer supported"));
        }
        case dbKillCursors:
            globalOpCounters.gotKillCursorsDeprecated();
            warnUnsupportedOp(*(rec->getOpCtx()->getClient()), networkOpToString(op));
            uasserted(5745707, "OP_KILL_CURSORS is no longer supported");
        case dbInsert: {
            auto opInsert = InsertOp::parseLegacy(rec->getMessage());
            globalOpCounters.gotInsertsDeprecated(opInsert.getDocuments().size());
            warnUnsupportedOp(*(rec->getOpCtx()->getClient()), networkOpToString(op));
            uasserted(5745706, "OP_INSERT is no longer supported");
        }
        case dbUpdate:
            globalOpCounters.gotUpdateDeprecated();
            warnUnsupportedOp(*(rec->getOpCtx()->getClient()), networkOpToString(op));
            uasserted(5745705, "OP_UPDATE is no longer supported");
        case dbDelete:
            globalOpCounters.gotDeleteDeprecated();
            warnUnsupportedOp(*(rec->getOpCtx()->getClient()), networkOpToString(op));
            uasserted(5745704, "OP_DELETE is no longer supported");
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

Future<DbResponse> ServiceEntryPointMongos::handleRequestImpl(OperationContext* opCtx,
                                                              const Message& message) noexcept {
    auto hr = std::make_shared<HandleRequest>(opCtx, message);
    return hr->run();
}

Future<DbResponse> ServiceEntryPointMongos::handleRequest(OperationContext* opCtx,
                                                          const Message& message) noexcept {
    return handleRequestImpl(opCtx, message);
}

void ServiceEntryPointMongos::onClientConnect(Client* client) {
    if (load_balancer_support::isFromLoadBalancer(client)) {
        _loadBalancedConnections.increment();
    }
}

void ServiceEntryPointMongos::derivedOnClientDisconnect(Client* client) {
    if (load_balancer_support::isFromLoadBalancer(client)) {
        _loadBalancedConnections.decrement();

        auto killerOperationContext = client->makeOperationContext();

        // Kill any cursors opened by the given Client.
        auto ccm = Grid::get(client->getServiceContext())->getCursorManager();
        ccm->killCursorsSatisfying(killerOperationContext.get(),
                                   [&](CursorId, const ClusterCursorManager::CursorEntry& entry) {
                                       return entry.originatingClientUuid() == client->getUUID();
                                   });

        // Kill any in-progress transactions over this Client connection.
        auto lsid = load_balancer_support::getMruSession(client);

        auto killToken = [&]() -> boost::optional<SessionCatalog::KillToken> {
            try {
                return SessionCatalog::get(killerOperationContext.get())->killSession(lsid);
            } catch (const ExceptionFor<ErrorCodes::NoSuchSession>&) {
                return boost::none;
            }
        }();
        if (!killToken) {
            // There was no entry in the SessionCatalog for the session most recently used by the
            // disconnecting client, so we have no transaction state to clean up.
            return;
        }
        OperationContextSession sessionCtx(killerOperationContext.get(), std::move(*killToken));
        invariant(lsid ==
                  OperationContextSession::get(killerOperationContext.get())->getSessionId());

        auto txnRouter = TransactionRouter::get(killerOperationContext.get());
        if (txnRouter && txnRouter.isInitialized() && !txnRouter.isTrackingOver()) {
            txnRouter.implicitlyAbortTransaction(
                killerOperationContext.get(),
                {ErrorCodes::Interrupted,
                 "aborting in-progress transaction because load-balanced client disconnected"});
        }
    }
}

void ServiceEntryPointMongos::appendStats(BSONObjBuilder* bob) const {
    ServiceEntryPointImpl::appendStats(bob);
    if (load_balancer_support::isEnabled()) {
        bob->append("loadBalanced", _loadBalancedConnections);
    }
}
}  // namespace mongo
