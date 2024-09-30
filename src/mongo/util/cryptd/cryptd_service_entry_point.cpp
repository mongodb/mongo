/**
 *    Copyright (C) 2023-present MongoDB, Inc.
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
#include "mongo/util/cryptd/cryptd_service_entry_point.h"

#include "mongo/db/commands.h"
#include "mongo/db/curop.h"
#include "mongo/db/dbmessage.h"
#include "mongo/db/validate_api_parameters.h"
#include "mongo/logv2/log.h"
#include "mongo/rpc/factory.h"
#include "mongo/rpc/message.h"
#include "mongo/rpc/reply_builder_interface.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/cryptd/cryptd_watchdog.h"
#include "mongo/util/str.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kControl


namespace mongo {
namespace {
void generateErrorResponse(OperationContext* opCtx,
                           rpc::ReplyBuilderInterface* replyBuilder,
                           const DBException& exception,
                           const BSONObj& replyMetadata,
                           BSONObj extraFields = {}) {

    // We could have thrown an exception after setting fields in the builder,
    // so we need to reset it to a clean state just to be sure.
    replyBuilder->reset();
    replyBuilder->setCommandReply(exception.toStatus(), extraFields);
    replyBuilder->getBodyBuilder().appendElements(replyMetadata);
}

void runCommand(OperationContext* opCtx,
                Command* command,
                const OpMsgRequest& request,
                rpc::ReplyBuilderInterface* replyBuilder) {

    auto invocation = command->parse(opCtx, request);

    const auto& dbname = invocation->db();
    uassert(ErrorCodes::InvalidNamespace,
            str::stream() << "Invalid database name: '" << dbname.toStringForErrorMsg() << "'",
            DatabaseName::isValid(dbname, DatabaseName::DollarInDbNameBehavior::Allow));

    const auto apiParamsFromClient = parseAndValidateAPIParameters(*invocation);
    {
        stdx::lock_guard<Client> lk(*opCtx->getClient());
        CurOp::get(opCtx)->setCommand(lk, command);
        APIParameters::get(opCtx) = APIParameters::fromClient(apiParamsFromClient);
    }

    invocation->run(opCtx, replyBuilder);

    auto body = replyBuilder->getBodyBuilder();
    CommandHelpers::extractOrAppendOk(body);
}
}  // namespace

void ClientObserverCryptD::onClientConnect(Client*) {
    signalIdleWatchdog();
}

Future<DbResponse> ServiceEntryPointCryptD::handleRequest(OperationContext* opCtx,
                                                          const Message& message) noexcept try {
    auto replyBuilder = rpc::makeReplyBuilder(rpc::protocolForMessage(message));

    OpMsgRequest request;
    try {  // Parse.
        request = rpc::opMsgRequestFromAnyProtocol(message, opCtx->getClient());
    } catch (const DBException& ex) {
        // If this error needs to fail the connection, propagate it out.
        if (ErrorCodes::isConnectionFatalMessageParseError(ex.code()))
            return ex.toStatus();

        // Otherwise, reply with the parse error. This is useful for cases where parsing fails
        // due to user-supplied input, such as the document too deep error. Since we failed
        // during parsing, we can't log anything about the command.
        LOGV2(24066, "Assertion while parsing command", "error"_attr = ex);

        BSONObjBuilder metadataBob;

        generateErrorResponse(opCtx, replyBuilder.get(), ex, metadataBob.obj(), BSONObj());

        auto response = replyBuilder->done();
        return Future<DbResponse>::makeReady({std::move(response)});
    }

    try {
        Command* c = nullptr;
        // In the absence of a Command object, no redaction is possible. Therefore
        // to avoid displaying potentially sensitive information in the logs,
        // we restrict the log message to the name of the unrecognized command.
        // However, the complete command object will still be echoed to the client.
        if (!(c = CommandHelpers::findCommand(opCtx, request.getCommandName()))) {
            LOGV2_DEBUG(24067, 2, "No such command", "command"_attr = request.getCommandName());
            uasserted(ErrorCodes::CommandNotFound,
                      str::stream() << "no such command: '" << request.getCommandName() << "'");
        }

        LOGV2_DEBUG(24068,
                    2,
                    "Run command",
                    "db"_attr = request.readDatabaseForLogging(),
                    "body"_attr = redact(request.body));

        runCommand(opCtx, c, request, replyBuilder.get());

    } catch (const DBException& ex) {
        BSONObjBuilder metadataBob;

        LOGV2_DEBUG(24069,
                    1,
                    "Assertion while executing command",
                    "command"_attr = request.getCommandName(),
                    "db"_attr = request.readDatabaseForLogging(),
                    "body"_attr = redact(request.body),
                    "error"_attr = redact(ex.toString()));

        if (ErrorCodes::isA<ErrorCategory::CloseConnectionError>(ex.code())) {
            // Rethrow the exception to the top to signal that the client connection should be
            // closed.
            throw;
        }

        generateErrorResponse(opCtx, replyBuilder.get(), ex, metadataBob.obj(), BSONObj());

        auto response = replyBuilder->done();
        return DbResponse{std::move(response)};
    }

    auto response = replyBuilder->done();
    return Future<DbResponse>::makeReady({std::move(response)});
} catch (const DBException& ex) {
    LOGV2(4879801, "Assertion while handling request", "error"_attr = redact(ex));
    return ex.toStatus();
}

}  // namespace mongo
