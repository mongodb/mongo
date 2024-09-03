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


#include <boost/smart_ptr.hpp>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <type_traits>
#include <utility>

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
#include <fmt/format.h>

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/catalog/collection_catalog.h"
#include "mongo/db/curop.h"
#include "mongo/db/curop_metrics.h"
#include "mongo/db/cursor_id.h"
#include "mongo/db/dbmessage.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/not_primary_error_tracker.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/request_execution_context.h"
#include "mongo/db/session/logical_session_id.h"
#include "mongo/db/session/logical_session_id_gen.h"
#include "mongo/db/session/session.h"
#include "mongo/db/session/session_catalog.h"
#include "mongo/db/stats/top.h"
#include "mongo/logv2/log.h"
#include "mongo/logv2/log_attr.h"
#include "mongo/logv2/log_component.h"
#include "mongo/logv2/redaction.h"
#include "mongo/rpc/message.h"
#include "mongo/s/commands/strategy.h"
#include "mongo/s/grid.h"
#include "mongo/s/load_balancer_support.h"
#include "mongo/s/query/cluster_cursor_manager.h"
#include "mongo/s/service_entry_point_mongos.h"
#include "mongo/s/transaction_router.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/decorable.h"
#include "mongo/util/duration.h"
#include "mongo/util/future_impl.h"
#include "mongo/util/uuid.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kNetwork


namespace mongo {

// Allows for decomposing `handleRequest` into parts and simplifies its execution.
struct HandleRequest {
    HandleRequest(OperationContext* opCtx, const Message& message)
        : rec(opCtx, message),
          op(message.operation()),
          msgId(message.header().getId()),
          nsString(getNamespaceString(rec.getDbMessage())) {}

    // Prepares the environment for handling the request.
    void setupEnvironment();

    // Performs the heavy lifting of running client commands.
    DbResponse handleRequest();

    // Runs on successful execution of `handleRequest`.
    void onSuccess(const DbResponse&);

    // Handles the request and fully prepares the response.
    DbResponse run();

    static NamespaceString getNamespaceString(const DbMessage& dbmsg) {
        if (!dbmsg.messageShouldHaveNs())
            return {};
        return NamespaceStringUtil::deserialize(
            boost::none, dbmsg.getns(), SerializationContext::stateDefault());
    }

    RequestExecutionContext rec;
    const NetworkOp op;
    const int32_t msgId;
    const NamespaceString nsString;

    boost::optional<long long> slowMsOverride;
};

void HandleRequest::setupEnvironment() {
    using namespace fmt::literals;
    auto opCtx = rec.getOpCtx();

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

DbResponse HandleRequest::handleRequest() {
    switch (op) {
        case dbQuery:
            if (!nsString.isCommand()) {
                return makeErrorResponseToUnsupportedOpQuery("OP_QUERY is no longer supported");
            }
            [[fallthrough]];  // It's a query containing a command
        case dbMsg:
            return Strategy::clientCommand(&rec);
        case dbGetMore: {
            return makeErrorResponseToUnsupportedOpQuery("OP_GET_MORE is no longer supported");
        }
        case dbKillCursors:
            uasserted(5745707, "OP_KILL_CURSORS is no longer supported");
        case dbInsert: {
            uasserted(5745706, "OP_INSERT is no longer supported");
        }
        case dbUpdate:
            uasserted(5745705, "OP_UPDATE is no longer supported");
        case dbDelete:
            uasserted(5745704, "OP_DELETE is no longer supported");
        default:
            MONGO_UNREACHABLE;
    }
}

void HandleRequest::onSuccess(const DbResponse& dbResponse) {
    auto opCtx = rec.getOpCtx();
    const auto currentOp = CurOp::get(opCtx);

    // Mark the op as complete, populate the response length, and log it if appropriate.
    currentOp->completeAndLogOperation(
        {logv2::LogComponent::kCommand},
        CollectionCatalog::get(opCtx)
            ->getDatabaseProfileSettings(currentOp->getNSS().dbName())
            .filter,
        dbResponse.response.size(),
        slowMsOverride);

    recordCurOpMetrics(opCtx);

    // Update the source of stats shown in the db.serverStatus().opLatencies section.
    Top::get(opCtx->getServiceContext())
        .incrementGlobalLatencyStats(
            opCtx,
            durationCount<Microseconds>(currentOp->elapsedTimeExcludingPauses()),
            durationCount<Microseconds>(
                duration_cast<Microseconds>(currentOp->debug().workingTimeMillis)),
            currentOp->getReadWriteType());
}

DbResponse HandleRequest::run() {
    setupEnvironment();
    auto dbResponse = handleRequest();
    onSuccess(dbResponse);
    return dbResponse;
}

Future<DbResponse> ServiceEntryPointMongos::handleRequestImpl(OperationContext* opCtx,
                                                              const Message& message) try {
    auto hr = HandleRequest(opCtx, message);
    return hr.run();
} catch (const DBException& ex) {
    auto status = ex.toStatus();
    LOGV2(4879803, "Failed to handle request", "error"_attr = redact(status));
    return status;
} catch (...) {
    auto error = exceptionToStatus();
    LOGV2_FATAL(
        9431601, "Request handling produced unhandled exception", "error"_attr = redact(error));
}

Future<DbResponse> ServiceEntryPointMongos::handleRequest(OperationContext* opCtx,
                                                          const Message& message) noexcept {
    return handleRequestImpl(opCtx, message);
}

}  // namespace mongo
