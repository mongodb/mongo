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
#include <mutex>
#include <set>
#include <string>
#include <utility>

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

#include "mongo/base/error_codes.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/db/api_parameters.h"
#include "mongo/db/auth/validated_tenancy_scope.h"
#include "mongo/db/client.h"
#include "mongo/db/commands.h"
#include "mongo/db/commands/query_cmd/explain_gen.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/query/explain_options.h"
#include "mongo/db/service_context.h"
#include "mongo/idl/command_generic_argument.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/rpc/op_msg.h"
#include "mongo/rpc/reply_builder_interface.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/database_name_util.h"
#include "mongo/util/decorable.h"
#include "mongo/util/str.h"

namespace mongo {
namespace {

/**
 * Implements the explain command on mongos.
 */

class ClusterExplainCmd final : public Command {
public:
    ClusterExplainCmd() : Command("explain") {}

    const std::set<std::string>& apiVersions() const override {
        return kApiVersions1;
    }

    std::unique_ptr<CommandInvocation> parse(OperationContext* opCtx,
                                             const OpMsgRequest& request) override;

    /**
     * Running an explain on a secondary requires explicitly setting slaveOk.
     */
    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kOptIn;
    }

    bool maintenanceOk() const override {
        return false;
    }

    bool adminOnly() const override {
        return false;
    }

    std::string help() const override {
        return "explain database reads and writes";
    }

    bool enableDiagnosticPrintingOnFailure() const final {
        return true;
    }

private:
    class Invocation;
};

class ClusterExplainCmd::Invocation final : public CommandInvocation {
public:
    Invocation(const ClusterExplainCmd* explainCommand,
               const OpMsgRequest& request,
               ExplainOptions::Verbosity verbosity,
               std::unique_ptr<OpMsgRequest> innerRequest,
               std::unique_ptr<CommandInvocation> innerInvocation)
        : CommandInvocation(explainCommand),
          _outerRequest{&request},
          _ns{CommandHelpers::parseNsFromCommand(_outerRequest->parseDbName(),
                                                 _outerRequest->body)},
          _verbosity{std::move(verbosity)},
          _genericArgs(GenericArguments::parse(IDLParserContext("explain",
                                                                request.validatedTenancyScope,
                                                                request.getValidatedTenantId(),
                                                                request.getSerializationContext()),
                                               _outerRequest->body)),
          _innerRequest{std::move(innerRequest)},
          _innerInvocation{std::move(innerInvocation)} {}

    ReadConcernSupportResult supportsReadConcern(repl::ReadConcernLevel level,
                                                 bool isImplicitDefault) const override {
        static const Status kDefaultReadConcernNotPermitted{
            ErrorCodes::InvalidOptions,
            "Explain does not permit default readConcern to be applied."};
        return {Status::OK(), {kDefaultReadConcernNotPermitted}};
    }

    const GenericArguments& getGenericArguments() const override {
        return _genericArgs;
    }

private:
    void run(OperationContext* opCtx, rpc::ReplyBuilderInterface* result) override {
        _innerInvocation->explain(opCtx, _verbosity, result);
    }

    void explain(OperationContext* opCtx,
                 ExplainOptions::Verbosity verbosity,
                 rpc::ReplyBuilderInterface* result) override {
        uasserted(ErrorCodes::IllegalOperation, "Explain cannot explain itself.");
    }

    NamespaceString ns() const override {
        return _ns;
    }

    const DatabaseName& db() const override {
        return _ns.dbName();
    }

    bool supportsWriteConcern() const override {
        return false;
    }

    /**
     * You are authorized to run an explain if you are authorized to run
     * the command that you are explaining. The auth check is performed recursively
     * on the nested command.
     */
    void doCheckAuthorization(OperationContext* opCtx) const override {
        _innerInvocation->checkAuthorization(opCtx, *_innerRequest);
    }

    const ClusterExplainCmd* command() const {
        return static_cast<const ClusterExplainCmd*>(definition());
    }

    const OpMsgRequest* _outerRequest;
    NamespaceString _ns;
    ExplainOptions::Verbosity _verbosity;
    const GenericArguments _genericArgs;
    std::unique_ptr<OpMsgRequest> _innerRequest;  // Lifespan must enclose that of _innerInvocation.
    std::unique_ptr<CommandInvocation> _innerInvocation;
};

/**
 * Synthesize a BSONObj for the command to be explained.
 * To do this we must copy generic arguments from the enclosing explain command.
 */
BSONObj makeExplainedObj(const BSONObj& outerObj,
                         const DatabaseName& dbName,
                         const SerializationContext& serializationContext) {
    const auto& first = outerObj.firstElement();
    uassert(
        ErrorCodes::BadValue, "explain command requires a nested object", first.type() == Object);
    const BSONObj& innerObj = first.Obj();

    if (auto innerDb = innerObj["$db"]) {
        uassert(ErrorCodes::InvalidNamespace,
                str::stream() << "Mismatched $db in explain command. Expected "
                              << dbName.toStringForErrorMsg() << " but got "
                              << innerDb.checkAndGetStringData(),
                DatabaseNameUtil::deserialize(dbName.tenantId(),
                                              innerDb.checkAndGetStringData(),
                                              serializationContext) == dbName);
    }

    BSONObjBuilder bob;
    bob.appendElements(innerObj);
    for (auto outerElem : outerObj) {
        // If the argument is in both the inner and outer command, we currently let the
        // inner version take precedence.
        const auto name = outerElem.fieldNameStringData();
        if (isGenericArgument(name) && !innerObj.hasField(name)) {
            bob.append(outerElem);
        }
    }
    return bob.obj();
}

std::unique_ptr<CommandInvocation> ClusterExplainCmd::parse(OperationContext* opCtx,
                                                            const OpMsgRequest& request) {
    CommandHelpers::uassertNoDocumentSequences(getName(), request);

    // To enforce API versioning
    auto cmdObj = idl::parseCommandRequest<ExplainCommandRequest>(
        IDLParserContext(ExplainCommandRequest::kCommandName,
                         request.validatedTenancyScope,
                         request.getValidatedTenantId(),
                         request.getSerializationContext()),
        request);
    ExplainOptions::Verbosity verbosity = cmdObj.getVerbosity();
    // This is the nested command which we are explaining. We need to propagate generic
    // arguments into the inner command since it is what is passed to the virtual
    // CommandInvocation::explain() method.
    const BSONObj explainedObj =
        makeExplainedObj(request.body, cmdObj.getDbName(), cmdObj.getSerializationContext());

    // Extract 'comment' field from the 'explainedObj' only if there is no top-level comment.
    auto commentField = explainedObj["comment"];
    if (!opCtx->getComment() && commentField) {
        stdx::lock_guard<Client> lk(*opCtx->getClient());
        opCtx->setComment(commentField.wrap());
    }

    const std::string cmdName = explainedObj.firstElementFieldName();
    auto explainedCommand = CommandHelpers::findCommand(opCtx, cmdName);
    uassert(ErrorCodes::CommandNotFound,
            str::stream() << "Explain failed due to unknown command: " << cmdName,
            explainedCommand);
    auto innerRequest = std::make_unique<OpMsgRequest>(OpMsg{explainedObj});
    innerRequest->validatedTenancyScope = request.validatedTenancyScope;
    auto innerInvocation = explainedCommand->parseForExplain(opCtx, *innerRequest, verbosity);
    return std::make_unique<Invocation>(
        this, request, std::move(verbosity), std::move(innerRequest), std::move(innerInvocation));
}

MONGO_REGISTER_COMMAND(ClusterExplainCmd).forRouter();

}  // namespace
}  // namespace mongo
