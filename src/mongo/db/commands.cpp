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

#include "mongo/db/commands.h"

#include <absl/container/node_hash_map.h>
#include <boost/none.hpp>
#include <boost/smart_ptr.hpp>
#include <fmt/format.h>
#include <memory>
#include <string>
#include <vector>

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

#include "mongo/base/error_extra_info.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/bson/mutable/algorithm.h"
#include "mongo/bson/mutable/document.h"
#include "mongo/bson/util/bson_extract.h"
#include "mongo/db/audit.h"
#include "mongo/db/auth/authorization_manager.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/validated_tenancy_scope.h"
#include "mongo/db/client.h"
#include "mongo/db/cluster_role.h"
#include "mongo/db/commands/test_commands_enabled.h"
#include "mongo/db/error_labels.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/tenant_id.h"
#include "mongo/db/transaction_resources.h"
#include "mongo/db/write_concern_options.h"
#include "mongo/idl/command_generic_argument.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/logv2/log.h"
#include "mongo/logv2/log_attr.h"
#include "mongo/logv2/log_component.h"
#include "mongo/logv2/redaction.h"
#include "mongo/platform/compiler.h"
#include "mongo/rpc/metadata/client_metadata.h"
#include "mongo/rpc/op_msg_rpc_impls.h"
#include "mongo/rpc/rewrite_state_change_errors.h"
#include "mongo/rpc/write_concern_error_detail.h"
#include "mongo/transport/session.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/database_name_util.h"
#include "mongo/util/decorable.h"
#include "mongo/util/duration.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/namespace_string_util.h"
#include "mongo/util/safe_num.h"
#include "mongo/util/static_immortal.h"
#include "mongo/util/str.h"
#include "mongo/util/string_map.h"
#include "mongo/util/uuid.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kCommand

namespace mongo {

const std::set<std::string> kNoApiVersions = {};
const std::set<std::string> kApiVersions1 = {"1"};

namespace {
using namespace fmt::literals;

const int kFailedFindCommandDebugLevel = 3;

const char kWriteConcernField[] = "writeConcern";

// Returns true if found to be authorized, false if undecided. Throws if unauthorized.
bool checkAuthorizationImplPreParse(OperationContext* opCtx,
                                    const Command* command,
                                    const OpMsgRequest& request) {
    auto client = opCtx->getClient();
    if (client->isInDirectClient())
        return true;

    uassert(ErrorCodes::Unauthorized,
            str::stream() << command->getName() << " may only be run against the admin database.",
            !command->adminOnly() || request.getDatabase() == DatabaseName::kAdmin.db(omitTenant));

    auto authzSession = AuthorizationSession::get(client);
    uassert(ErrorCodes::ReauthenticationRequired,
            fmt::format("Command {} requires reauthentication since the current authorization "
                        "session has expired. Please re-auth.",
                        command->getName()),
            !command->requiresAuth() || !authzSession->isExpired());

    if (!authzSession->getAuthorizationManager().isAuthEnabled()) {
        // Running without auth, so everything should be allowed except remotely invoked
        // commands that have the 'localHostOnlyIfNoAuth' restriction.
        uassert(ErrorCodes::Unauthorized,
                str::stream() << command->getName()
                              << " must run from localhost when running db without auth",
                !command->adminOnly() || !command->localHostOnlyIfNoAuth() ||
                    client->getIsLocalHostConnection());
        return true;  // Blanket authorization: don't need to check anything else.
    }

    if (authzSession->isUsingLocalhostBypass())
        return false;  // Still can't decide on auth because of the localhost bypass.

    uassert(ErrorCodes::Unauthorized,
            str::stream() << "Command " << command->getName() << " requires authentication",
            !command->requiresAuth() || authzSession->isAuthenticated() ||
                (request.validatedTenancyScope &&
                 request.validatedTenancyScope->hasAuthenticatedUser()));

    return false;
}

auto getCommandInvocationHooks =
    ServiceContext::declareDecoration<std::unique_ptr<CommandInvocationHooks>>();

}  // namespace

void CommandInvocationHooks::set(ServiceContext* serviceContext,
                                 std::unique_ptr<CommandInvocationHooks> hooks) {
    getCommandInvocationHooks(serviceContext) = std::move(hooks);
}

//////////////////////////////////////////////////////////////
// CommandHelpers

const WriteConcernOptions CommandHelpers::kMajorityWriteConcern(
    WriteConcernOptions::kMajority,
    // Note: Even though we're setting UNSET here, kMajority implies JOURNAL if journaling is
    // supported by the mongod.
    WriteConcernOptions::SyncMode::UNSET,
    WriteConcernOptions::kWriteConcernTimeoutUserCommand);

BSONObj CommandHelpers::runCommandDirectly(OperationContext* opCtx, const OpMsgRequest& request) {
    auto command = getCommandRegistry(opCtx)->findCommand(request.getCommandName());
    invariant(command);
    rpc::OpMsgReplyBuilder replyBuilder;
    std::unique_ptr<CommandInvocation> invocation;
    try {
        invocation = command->parse(opCtx, request);
        invocation->run(opCtx, &replyBuilder);
        auto body = replyBuilder.getBodyBuilder();
        CommandHelpers::extractOrAppendOk(body);
    } catch (const ExceptionFor<ErrorCodes::StaleConfig>&) {
        // These exceptions are intended to be handled at a higher level.
        throw;
    } catch (const DBException& ex) {
        if (ex.code() == ErrorCodes::Unauthorized) {
            CommandHelpers::auditLogAuthEvent(opCtx, invocation.get(), request, ex.code());
        }
        auto body = replyBuilder.getBodyBuilder();
        body.resetToEmpty();
        appendCommandStatusNoThrow(body, ex.toStatus());
    }
    return replyBuilder.releaseBody();
}

Future<void> CommandHelpers::runCommandInvocation(std::shared_ptr<RequestExecutionContext> rec,
                                                  std::shared_ptr<CommandInvocation> invocation,
                                                  bool useDedicatedThread) {
    if (useDedicatedThread)
        return makeReadyFutureWith([rec = std::move(rec), invocation = std::move(invocation)] {
            runCommandInvocation(
                rec->getOpCtx(), rec->getRequest(), invocation.get(), rec->getReplyBuilder());
        });
    return runCommandInvocationAsync(std::move(rec), std::move(invocation));
}

void CommandHelpers::runCommandInvocation(OperationContext* opCtx,
                                          const OpMsgRequest& request,
                                          CommandInvocation* invocation,
                                          rpc::ReplyBuilderInterface* response) {
    auto&& hooks = getCommandInvocationHooks(opCtx->getServiceContext());
    if (hooks) {
        hooks->onBeforeRun(opCtx, request, invocation);
    }

    invocation->run(opCtx, response);

    if (hooks) {
        hooks->onAfterRun(opCtx, request, invocation, response);
    }
}

Future<void> CommandHelpers::runCommandInvocationAsync(
    std::shared_ptr<RequestExecutionContext> rec,
    std::shared_ptr<CommandInvocation> invocation) try {
    auto&& hooks = getCommandInvocationHooks(rec->getOpCtx()->getServiceContext());
    if (hooks)
        hooks->onBeforeAsyncRun(rec, invocation.get());
    return invocation->runAsync(rec).then([rec, hooks = hooks.get(), invocation] {
        if (hooks)
            hooks->onAfterAsyncRun(rec, invocation.get());
    });
} catch (const DBException& e) {
    return e.toStatus();
}

void CommandHelpers::auditLogAuthEvent(OperationContext* opCtx,
                                       const CommandInvocation* invocation,
                                       const OpMsgRequest& request,
                                       ErrorCodes::Error err) {
    class Hook final : public audit::CommandInterface {
    public:
        Hook(const CommandInvocation* invocation, const OpMsgRequest& request)
            : _invocation(invocation) {
            if (_invocation) {
                _nss = _invocation->ns();
                _name = _invocation->definition()->getName();
            } else {
                _nss = NamespaceString(request.getDbName());
                _name = request.getCommandName().toString();
            }
        }

        void snipForLogging(mutablebson::Document* cmdObj) const override {
            if (_invocation) {
                _invocation->definition()->snipForLogging(cmdObj);
            }
        }

        std::set<StringData> sensitiveFieldNames() const override {
            if (_invocation) {
                return _invocation->definition()->sensitiveFieldNames();
            }
            return {};
        }

        StringData getName() const override {
            return _name;
        }

        NamespaceString ns() const override {
            return _nss;
        }

        bool redactArgs() const override {
            return !_invocation;
        }

    private:
        const CommandInvocation* _invocation;
        NamespaceString _nss;
        std::string _name;
    };

    // Always audit errors other than Unauthorized.
    //
    // When we get Unauthorized (usually),
    // then only audit if our Command definition wants it (default),
    // or if we don't know our Command definition.
    if ((err != ErrorCodes::Unauthorized) || !invocation ||
        invocation->definition()->auditAuthorizationFailure()) {
        audit::logCommandAuthzCheck(opCtx->getClient(), request, Hook(invocation, request), err);
    }
}

void CommandHelpers::uassertNoDocumentSequences(StringData commandName,
                                                const OpMsgRequest& request) {
    uassert(40472,
            str::stream() << "The " << commandName
                          << " command does not support document sequences.",
            request.sequences.empty());
}

std::string CommandHelpers::parseNsFullyQualified(const BSONObj& cmdObj) {
    BSONElement first = cmdObj.firstElement();
    uassert(ErrorCodes::BadValue,
            str::stream() << "collection name has invalid type " << typeName(first.type()),
            first.canonicalType() == canonicalizeBSONType(mongo::String));
    const auto ns = first.valueStringData();
    uassert(ErrorCodes::InvalidNamespace,
            str::stream() << "Invalid namespace specified '" << ns << "'",
            NamespaceString::isValid(ns));
    return ns.toString();
}

NamespaceString CommandHelpers::parseNsCollectionRequired(const DatabaseName& dbName,
                                                          const BSONObj& cmdObj) {
    // Accepts both BSON String and Symbol for collection name per SERVER-16260
    // TODO(kangas) remove Symbol support in MongoDB 3.0 after Ruby driver audit
    BSONElement first = cmdObj.firstElement();
    const bool isUUID = (first.canonicalType() == canonicalizeBSONType(mongo::BinData) &&
                         first.binDataType() == BinDataType::newUUID);
    uassert(ErrorCodes::InvalidNamespace,
            str::stream() << "Collection name must be provided. UUID is not valid in this "
                          << "context",
            !isUUID);
    uassert(ErrorCodes::InvalidNamespace,
            str::stream() << "collection name has invalid type " << typeName(first.type()),
            first.canonicalType() == canonicalizeBSONType(mongo::String));
    NamespaceString nss(NamespaceStringUtil::deserialize(dbName, first.valueStringData()));
    uassert(ErrorCodes::InvalidNamespace,
            str::stream() << "Invalid namespace specified '" << nss.toStringForErrorMsg() << "'",
            nss.isValid());
    return nss;
}

NamespaceStringOrUUID CommandHelpers::parseNsOrUUID(const DatabaseName& dbName,
                                                    const BSONObj& cmdObj) {
    BSONElement first = cmdObj.firstElement();
    if (first.type() == BinData && first.binDataType() == BinDataType::newUUID) {
        return {dbName, uassertStatusOK(UUID::parse(first))};
    } else {
        const NamespaceString nss(parseNsCollectionRequired(dbName, cmdObj));
        ensureValidCollectionName(nss);
        return nss;
    }
}

void CommandHelpers::ensureValidCollectionName(const NamespaceString& nss) {
    uassert(ErrorCodes::InvalidNamespace,
            str::stream() << "Invalid collection name specified '" << nss.toStringForErrorMsg()
                          << "'",
            (NamespaceString::validCollectionName(nss.coll()) ||
             nss == NamespaceString::kLocalOplogDollarMain));
}

NamespaceString CommandHelpers::parseNsFromCommand(const DatabaseName& dbName,
                                                   const BSONObj& cmdObj) {
    BSONElement first = cmdObj.firstElement();
    if (first.type() != mongo::String)
        return NamespaceString(dbName);
    return NamespaceStringUtil::deserialize(dbName, cmdObj.firstElement().valueStringData());
}

ResourcePattern CommandHelpers::resourcePatternForNamespace(const NamespaceString& ns) {
    if (!NamespaceString::validCollectionComponent(ns)) {
        return ResourcePattern::forDatabaseName(ns.dbName());
    }
    return ResourcePattern::forExactNamespace(ns);
}

bool CommandHelpers::appendCommandStatusNoThrow(BSONObjBuilder& result, const Status& status) {
    appendSimpleCommandStatus(result, status.isOK(), status.reason());
    BSONObj tmp = result.asTempObj();
    if (!status.isOK() && !tmp.hasField("code")) {
        result.append("code", status.code());
        result.append("codeName", ErrorCodes::errorString(status.code()));
    }
    if (auto extraInfo = status.extraInfo()) {
        extraInfo->serialize(&result);
    }
    // If the command has errored, assert that it satisfies the IDL-defined requirements on a
    // command error reply.
    // Only validate error reply in test mode so that we don't expose users to errors if we
    // construct an invalid error reply.
    if (!status.isOK() && getTestCommandsEnabled()) {
        try {
            ErrorReply::parse(IDLParserContext("appendCommandStatusNoThrow"), result.asTempObj());
        } catch (const DBException&) {
            invariant(false,
                      "invalid error-response to a command constructed in "
                      "CommandHelpers::appendComandStatusNoThrow. All erroring command responses "
                      "must comply with the format specified by the IDL-defined struct ErrorReply, "
                      "defined in idl/basic_types.idl");
        }
    }
    return status.isOK();
}

void CommandHelpers::appendSimpleCommandStatus(BSONObjBuilder& result,
                                               bool ok,
                                               const std::string& errmsg) {
    BSONObj tmp = result.asTempObj();
    bool have_ok = tmp.hasField("ok");
    bool need_errmsg = !ok && !tmp.hasField("errmsg");

    if (!have_ok)
        result.append("ok", ok ? 1.0 : 0.0);

    if (need_errmsg) {
        result.append("errmsg", errmsg);
    }
}

bool CommandHelpers::extractOrAppendOk(BSONObjBuilder& reply) {
    if (auto okField = reply.asTempObj()["ok"]) {
        // If ok is present, use its truthiness.
        return okField.trueValue();
    }

    // Missing "ok" field is an implied success.
    reply.append("ok", 1.0);
    return true;
}

void CommandHelpers::appendCommandWCStatus(BSONObjBuilder& result,
                                           const Status& awaitReplicationStatus,
                                           const WriteConcernResult& wcResult) {
    if (!awaitReplicationStatus.isOK() && !result.hasField("writeConcernError")) {
        WriteConcernErrorDetail wcError;
        wcError.setStatus(awaitReplicationStatus);
        BSONObjBuilder errInfoBuilder;
        if (wcResult.wTimedOut) {
            errInfoBuilder.append("wtimeout", true);
        }
        errInfoBuilder.append(kWriteConcernField, wcResult.wcUsed.toBSON());
        wcError.setErrInfo(errInfoBuilder.obj());
        result.append("writeConcernError", wcError.toBSON());
    }
}

BSONObj CommandHelpers::appendGenericCommandArgs(const BSONObj& cmdObjWithGenericArgs,
                                                 const BSONObj& request) {
    BSONObjBuilder b;
    b.appendElements(request);
    for (const auto& elem : filterCommandRequestForPassthrough(cmdObjWithGenericArgs)) {
        const auto name = elem.fieldNameStringData();
        if (isGenericArgument(name) && !request.hasField(name)) {
            b.append(elem);
        }
    }
    return b.obj();
}

void CommandHelpers::appendGenericReplyFields(const BSONObj& replyObjWithGenericReplyFields,
                                              const BSONObj& reply,
                                              BSONObjBuilder* replyBuilder) {
    replyBuilder->appendElements(reply);
    for (const auto& elem : filterCommandReplyForPassthrough(replyObjWithGenericReplyFields)) {
        const auto name = elem.fieldNameStringData();
        if (isGenericArgument(name) && !reply.hasField(name)) {
            replyBuilder->append(elem);
        }
    }
}

BSONObj CommandHelpers::appendGenericReplyFields(const BSONObj& replyObjWithGenericReplyFields,
                                                 const BSONObj& reply) {
    BSONObjBuilder b;
    appendGenericReplyFields(replyObjWithGenericReplyFields, reply, &b);
    return b.obj();
}

BSONObj CommandHelpers::appendWCToObj(const BSONObj& cmdObj, WriteConcernOptions newWC) {
    // Append all original fields except the writeConcern field to the new command.
    BSONObjBuilder cmdObjWithWriteConcern;
    for (const auto& elem : cmdObj) {
        const auto name = elem.fieldNameStringData();
        if (name != "writeConcern" && !cmdObjWithWriteConcern.hasField(name)) {
            cmdObjWithWriteConcern.append(elem);
        }
    }

    // Finally, add the new write concern.
    cmdObjWithWriteConcern.append(kWriteConcernField, newWC.toBSON());
    return cmdObjWithWriteConcern.obj();
}

BSONObj CommandHelpers::appendMajorityWriteConcern(const BSONObj& cmdObj,
                                                   WriteConcernOptions defaultWC) {
    if (cmdObj.hasField(kWriteConcernField)) {
        auto parsedWC = uassertStatusOK(WriteConcernOptions::extractWCFromCommand(cmdObj));

        // The command has a writeConcern field and it's majority, so we can return it as-is.
        if (parsedWC.isMajority()) {
            return cmdObj;
        }

        parsedWC.w = WriteConcernOptions::kMajority;
        return appendWCToObj(cmdObj, parsedWC);
    } else if (!defaultWC.usedDefaultConstructedWC) {
        defaultWC.w = WriteConcernOptions::kMajority;
        if (defaultWC.wTimeout < kMajorityWriteConcern.wTimeout) {
            defaultWC.wTimeout = kMajorityWriteConcern.wTimeout;
        }
        return appendWCToObj(cmdObj, defaultWC);
    } else {
        return appendWCToObj(cmdObj, kMajorityWriteConcern);
    }
}

BSONObj CommandHelpers::filterCommandRequestForPassthrough(const BSONObj& cmdObj) {
    BSONObjIterator cmdIter(cmdObj);
    BSONObjBuilder bob;
    filterCommandRequestForPassthrough(&cmdIter, &bob);
    return bob.obj();
}

void CommandHelpers::filterCommandRequestForPassthrough(BSONObjIterator* cmdIter,
                                                        BSONObjBuilder* requestBuilder) {
    while (cmdIter->more()) {
        auto elem = cmdIter->next();
        const auto name = elem.fieldNameStringData();
        if (name == "$readPreference") {
            BSONObjBuilder(requestBuilder->subobjStart("$queryOptions")).append(elem);
            continue;
        }
        if (!shouldForwardToShards(name))
            continue;
        requestBuilder->append(elem);
    }
}

void CommandHelpers::filterCommandReplyForPassthrough(const BSONObj& cmdObj,
                                                      BSONObjBuilder* output) {
    for (auto elem : cmdObj) {
        const auto name = elem.fieldNameStringData();
        if (!shouldForwardFromShards(name))
            continue;
        output->append(elem);
    }
}

BSONObj CommandHelpers::filterCommandReplyForPassthrough(const BSONObj& cmdObj) {
    BSONObjBuilder bob;
    filterCommandReplyForPassthrough(cmdObj, &bob);
    return bob.obj();
}

bool CommandHelpers::isHelpRequest(const BSONElement& helpElem) {
    return !helpElem.eoo() && helpElem.trueValue();
}

bool CommandHelpers::uassertShouldAttemptParse(OperationContext* opCtx,
                                               const Command* command,
                                               const OpMsgRequest& request) {
    try {
        return checkAuthorizationImplPreParse(opCtx, command, request);
    } catch (const ExceptionFor<ErrorCodes::Unauthorized>& e) {
        if (command->auditAuthorizationFailure()) {
            CommandHelpers::auditLogAuthEvent(opCtx, nullptr, request, e.code());
        }
        throw;
    }
}

void CommandHelpers::uassertCommandRunWithMajority(StringData commandName,
                                                   const WriteConcernOptions& writeConcern) {
    uassert(ErrorCodes::InvalidOptions,
            "\"{}\" must be called with majority writeConcern, got: {} "_format(
                commandName, writeConcern.toBSON().toString()),
            writeConcern.isMajority());
}

void CommandHelpers::canUseTransactions(Service* service,
                                        const std::vector<NamespaceString>& namespaces,
                                        StringData cmdName,
                                        bool allowTransactionsOnConfigDatabase) {
    uassert(ErrorCodes::OperationNotSupportedInTransaction,
            "Cannot run 'count' in a multi-document transaction. Please see "
            "http://dochub.mongodb.org/core/transaction-count for a recommended alternative.",
            cmdName != "count"_sd);

    auto command = findCommand(service, cmdName);
    uassert(ErrorCodes::CommandNotFound,
            str::stream() << "Encountered unknown command during check if can run in transactions: "
                          << cmdName,
            command);

    uassert(ErrorCodes::OperationNotSupportedInTransaction,
            str::stream() << "Cannot run '" << cmdName << "' in a multi-document transaction.",
            command->allowedInTransactions());

    for (auto& nss : namespaces) {
        const auto dbName = nss.dbName();

        uassert(ErrorCodes::OperationNotSupportedInTransaction,
                str::stream() << "Cannot run command against the '" << dbName.toStringForErrorMsg()
                              << "' database in a transaction.",
                !dbName.isLocalDB());

        uassert(ErrorCodes::OperationNotSupportedInTransaction,
                str::stream() << "Cannot run command against the '" << nss.toStringForErrorMsg()
                              << "' collection in a transaction.",
                !(nss.isSystemDotProfile() || nss.isSystemDotViews()));

        if (allowTransactionsOnConfigDatabase) {
            uassert(ErrorCodes::OperationNotSupportedInTransaction,
                    "Cannot run command against the config.transactions namespace in a transaction"
                    "on a sharded cluster.",
                    nss != NamespaceString::kSessionTransactionsTableNamespace);
        } else {
            uassert(ErrorCodes::OperationNotSupportedInTransaction,
                    "Cannot run command against the config database in a transaction.",
                    !dbName.isConfigDB());
        }
    }
}

constexpr StringData CommandHelpers::kHelpFieldName;

MONGO_FAIL_POINT_DEFINE(failCommand);
MONGO_FAIL_POINT_DEFINE(waitInCommandMarkKillOnClientDisconnect);

// A decoration representing error labels specified in a failCommand failpoint that has affected a
// command in this OperationContext.
const auto errorLabelsOverrideDecoration =
    OperationContext::declareDecoration<boost::optional<BSONArray>>();

boost::optional<BSONArray>& errorLabelsOverride(OperationContext* opCtx) {
    return (*opCtx)[errorLabelsOverrideDecoration];
}

bool CommandHelpers::shouldActivateFailCommandFailPoint(const BSONObj& data,
                                                        const CommandInvocation* invocation,
                                                        Client* client) {
    const Command* cmd = invocation->definition();
    NamespaceString nss;
    try {
        nss = invocation->ns();
    } catch (const ExceptionFor<ErrorCodes::InvalidNamespace>&) {
        return false;
    }
    return shouldActivateFailCommandFailPoint(data, nss, cmd, client);
}

bool CommandHelpers::shouldActivateFailCommandFailPoint(const BSONObj& data,
                                                        const NamespaceString& nss,
                                                        const Command* cmd,
                                                        Client* client) {
    if (cmd->getName() == "configureFailPoint"_sd)  // Banned even if in failCommands.
        return false;

    if (!(data.hasField("failLocalClients") && data.getBoolField("failLocalClients")) &&
        !client->session()) {
        return false;
    }

    auto threadName = client->desc();
    auto appName = StringData();
    if (auto clientMetadata = ClientMetadata::get(client)) {
        appName = clientMetadata->getApplicationName();
    }

    auto isInternalThreadOrClient = !client->session() || client->isInternalClient();

    if (data.hasField("threadName") && (threadName != data.getStringField("threadName"))) {
        return false;  // only activate failpoint on thread from certain client
    }

    if (data.hasField("appName") && (appName != data.getStringField("appName"))) {
        return false;  // only activate failpoint on connection with a certain appName
    }

    if (data.hasField("namespace")) {
        const auto fpNss = NamespaceStringUtil::parseFailPointData(data, "namespace"_sd);
        if (nss != fpNss) {
            return false;
        }
    }

    if (!(data.hasField("failInternalCommands") && data.getBoolField("failInternalCommands")) &&
        isInternalThreadOrClient) {
        return false;
    }

    if (data.hasField("failAllCommands")) {
        LOGV2(6348500,
              "Activating 'failCommand' failpoint for all commands",
              "data"_attr = data,
              "threadName"_attr = threadName,
              "appName"_attr = appName,
              logAttrs(nss),
              "isInternalClient"_attr = isInternalThreadOrClient,
              "command"_attr = cmd->getName());
        return true;
    }

    for (auto&& failCommand : data.getObjectField("failCommands")) {
        if (failCommand.type() == String && cmd->hasAlias(failCommand.valueStringData())) {
            LOGV2(4898500,
                  "Activating 'failCommand' failpoint",
                  "data"_attr = data,
                  "threadName"_attr = threadName,
                  "appName"_attr = appName,
                  logAttrs(nss),
                  "isInternalClient"_attr = isInternalThreadOrClient,
                  "command"_attr = cmd->getName());

            return true;
        }
    }

    return false;
}

void CommandHelpers::evaluateFailCommandFailPoint(OperationContext* opCtx,
                                                  const CommandInvocation* invocation) {
    bool closeConnection;
    bool blockConnection;
    bool hasErrorCode;
    /**
     * Default value is used to suppress the uassert for `errorExtraInfo` if `errorCode` is not set.
     */
    long long errorCode = ErrorCodes::OK;
    /**
     * errorExtraInfo only holds a reference to the BSONElement of the parent object (data).
     * The copy constructor of BSONObj handles cloning to keep references valid outside the scope.
     */
    BSONElement errorExtraInfo;
    const Command* cmd = invocation->definition();
    failCommand.executeIf(
        [&](const BSONObj& data) {
            rpc::RewriteStateChangeErrors::onActiveFailCommand(opCtx, data);

            if (data.hasField(kErrorLabelsFieldName) &&
                data[kErrorLabelsFieldName].type() == Array) {
                // Propagate error labels specified in the failCommand failpoint to the
                // OperationContext decoration to override getErrorLabels() behaviors.
                invariant(!errorLabelsOverride(opCtx));
                errorLabelsOverride(opCtx).emplace(
                    data.getObjectField(kErrorLabelsFieldName).getOwned());
            }

            if (blockConnection) {
                long long blockTimeMS = 0;
                uassert(ErrorCodes::InvalidOptions,
                        "must specify 'blockTimeMS' when 'blockConnection' is true",
                        data.hasField("blockTimeMS") &&
                            bsonExtractIntegerField(data, "blockTimeMS", &blockTimeMS).isOK());
                uassert(ErrorCodes::InvalidOptions,
                        "'blockTimeMS' must be non-negative",
                        blockTimeMS >= 0);

                LOGV2(20432,
                      "Blocking command via 'failCommand' failpoint",
                      "command"_attr = cmd->getName(),
                      "blockTime"_attr = Milliseconds{blockTimeMS});
                opCtx->sleepFor(Milliseconds{blockTimeMS});
                LOGV2(20433,
                      "Unblocking command via 'failCommand' failpoint",
                      "command"_attr = cmd->getName());
            }

            auto tassert = [&] {
                bool b;
                Status st = bsonExtractBooleanField(data, "tassert", &b);
                if (st == ErrorCodes::NoSuchKey) {
                    return false;
                }
                uassertStatusOK(st);
                return b;
            }();

            auto raise = [&](const Status& status) {
                if (tassert) {
                    tassert(status);
                } else {
                    uassertStatusOK(status);
                }
            };

            static constexpr auto failpointMsg = "Failing command via 'failCommand' failpoint"_sd;

            if (closeConnection) {
                opCtx->getClient()->session()->end();
                LOGV2(20431,
                      "Failing {command} via 'failCommand' failpoint: closing connection",
                      "command"_attr = cmd->getName());
                raise(Status(tassert ? ErrorCodes::Error(5704000) : ErrorCodes::Error(50985),
                             failpointMsg));
            }

            auto errorExtraInfo = [&]() -> boost::optional<BSONObj> {
                BSONElement e;
                Status st = bsonExtractTypedField(data, "errorExtraInfo", BSONType::Object, &e);
                if (st == ErrorCodes::NoSuchKey)
                    return {};  // It's optional. Missing is allowed. Other errors aren't.
                uassertStatusOK(st);
                return {e.Obj()};
            }();

            if (errorExtraInfo) {
                LOGV2(20434,
                      "Failing {command} via 'failCommand' failpoint: returning {errorCode} and "
                      "{errorExtraInfo}",
                      "command"_attr = cmd->getName(),
                      "errorCode"_attr = errorCode,
                      "errorExtraInfo"_attr = errorExtraInfo);
                raise(Status(ErrorCodes::Error(errorCode), failpointMsg, *errorExtraInfo));

            } else if (hasErrorCode) {
                LOGV2(
                    20435,
                    "Failing command {command} via 'failCommand' failpoint: returning {errorCode}",
                    "command"_attr = cmd->getName(),
                    "errorCode"_attr = errorCode);
                raise(Status(ErrorCodes::Error(errorCode), failpointMsg));
            }
        },
        [&](const BSONObj& data) {
            closeConnection = data.hasField("closeConnection") &&
                bsonExtractBooleanField(data, "closeConnection", &closeConnection).isOK() &&
                closeConnection;
            hasErrorCode = data.hasField("errorCode") &&
                bsonExtractIntegerField(data, "errorCode", &errorCode).isOK();
            blockConnection = data.hasField("blockConnection") &&
                bsonExtractBooleanField(data, "blockConnection", &blockConnection).isOK() &&
                blockConnection;
            return shouldActivateFailCommandFailPoint(data, invocation, opCtx->getClient()) &&
                (closeConnection || blockConnection || hasErrorCode);
        });
}

void CommandHelpers::handleMarkKillOnClientDisconnect(OperationContext* opCtx,
                                                      bool shouldMarkKill) {
    if (opCtx->getClient()->isInDirectClient()) {
        return;
    }

    if (shouldMarkKill) {
        opCtx->markKillOnClientDisconnect();
    }

    waitInCommandMarkKillOnClientDisconnect.executeIf(
        [&](const BSONObj&) { waitInCommandMarkKillOnClientDisconnect.pauseWhileSet(opCtx); },
        [&](const BSONObj& obj) {
            auto md = ClientMetadata::get(opCtx->getClient());
            return md && (md->getApplicationName() == obj["appName"].str());
        });
}

void CommandHelpers::checkForInternalError(rpc::ReplyBuilderInterface* replyBuilder,
                                           bool isInternalClient) {
    if (isInternalClient) {
        return;
    }

    auto obj = replyBuilder->getBodyBuilder().asTempObj();
    if (auto e = obj.getField("code"); MONGO_unlikely(!e.eoo())) {
        const auto errorCode = static_cast<ErrorCodes::Error>(e.safeNumberInt());
        try {
            tassert(
                errorCode,
                fmt::format("Attempted to return an internal-only error to the client, errmsg: {}",
                            obj.getStringField("errmsg")),
                !ErrorCodes::isInternalOnly(errorCode));
        } catch (...) {
            // No need to throw as we only require the diagnostics provided by `tassert` and
            // do not want to close the connection.
        }
    }
}

namespace {
// We store the CommandInvocation as a shared_ptr on the OperationContext in case we need to persist
// the invocation past the lifetime of the op. If so, this shared_ptr can be copied off to another
// thread. If not, there is only one shared_ptr and the invocation goes out of scope when the op
// ends.
auto invocationForOpCtx = OperationContext::declareDecoration<std::shared_ptr<CommandInvocation>>();
}  // namespace

//////////////////////////////////////////////////////////////
// CommandInvocation

void CommandInvocation::set(OperationContext* opCtx,
                            std::shared_ptr<CommandInvocation> invocation) {
    invocationForOpCtx(opCtx) = std::move(invocation);
}

std::shared_ptr<CommandInvocation> CommandInvocation::get(OperationContext* opCtx) {
    return invocationForOpCtx(opCtx);
}

CommandInvocation::~CommandInvocation() = default;

void CommandInvocation::checkAuthorization(OperationContext* opCtx,
                                           const OpMsgRequest& request) const {
    // Always send an authorization event to audit log, even if OK.
    // Not using a scope guard because auditLogAuthEvent could conceivably throw.
    try {
        const Command* c = definition();
        if (checkAuthorizationImplPreParse(opCtx, c, request)) {
            // Blanket authorization: don't need to check anything else.
        } else {
            try {
                doCheckAuthorization(opCtx);
            } catch (const ExceptionFor<ErrorCodes::Unauthorized>&) {
                namespace mmb = mutablebson;
                mmb::Document cmdToLog(request.body, mmb::Document::kInPlaceDisabled);
                c->snipForLogging(&cmdToLog);
                auto dbName = request.getDbName();
                uasserted(ErrorCodes::Unauthorized,
                          str::stream() << "not authorized on " << dbName.toStringForErrorMsg()
                                        << " to execute command " << redact(cmdToLog.getObject()));
            }
        }
    } catch (const DBException& e) {
        LOGV2_OPTIONS(20436,
                      {logv2::LogComponent::kAccessControl},
                      "Checking authorization failed",
                      "error"_attr = e.toStatus());
        CommandHelpers::auditLogAuthEvent(opCtx, this, request, e.code());
        throw;
    }
    CommandHelpers::auditLogAuthEvent(opCtx, this, request, ErrorCodes::OK);
}

//////////////////////////////////////////////////////////////
// Command

class BasicCommandWithReplyBuilderInterface::Invocation final : public CommandInvocation {
public:
    Invocation(OperationContext*,
               const OpMsgRequest& request,
               BasicCommandWithReplyBuilderInterface* command)
        : CommandInvocation(command),
          _command(command),
          _request(request),
          _dbName(request.getDbName()) {}

private:
    void run(OperationContext* opCtx, rpc::ReplyBuilderInterface* result) override {
        shard_role_details::getLocker(opCtx)->setDebugInfo(redact(_request.body).toString());
        bool ok = _command->runWithReplyBuilder(opCtx, _dbName, _request.body, result);
        if (!ok) {
            BSONObjBuilder bob = result->getBodyBuilder();
            CommandHelpers::appendSimpleCommandStatus(bob, ok);
        }
    }

    Future<void> runAsync(std::shared_ptr<RequestExecutionContext> rec) override {
        return _command->runAsync(rec, _dbName).onError([rec](Status status) {
            if (status.code() != ErrorCodes::FailedToRunWithReplyBuilder)
                return status;
            BSONObjBuilder bob = rec->getReplyBuilder()->getBodyBuilder();
            CommandHelpers::appendSimpleCommandStatus(bob, false);
            return Status::OK();
        });
    }

    void explain(OperationContext* opCtx,
                 ExplainOptions::Verbosity verbosity,
                 rpc::ReplyBuilderInterface* result) override {
        uassertStatusOK(_command->explain(opCtx, _request, verbosity, result));
    }

    NamespaceString ns() const override {
        return _command->parseNs(_dbName, cmdObj());
    }

    bool supportsWriteConcern() const override {
        return _command->supportsWriteConcern(cmdObj());
    }

    ReadConcernSupportResult supportsReadConcern(repl::ReadConcernLevel level,
                                                 bool isImplicitDefault) const override {
        return _command->supportsReadConcern(cmdObj(), level, isImplicitDefault);
    }

    bool isSubjectToIngressAdmissionControl() const override {
        return _command->isSubjectToIngressAdmissionControl();
    }

    bool supportsReadMirroring() const override {
        return _command->supportsReadMirroring(cmdObj());
    }

    DatabaseName getDBForReadMirroring() const override {
        invariant(cmdObj().isOwned());
        return _dbName;
    }

    void appendMirrorableRequest(BSONObjBuilder* bob) const override {
        invariant(cmdObj().isOwned());
        _command->appendMirrorableRequest(bob, cmdObj());
    }

    bool allowsAfterClusterTime() const override {
        return _command->allowsAfterClusterTime(cmdObj());
    }

    bool canIgnorePrepareConflicts() const override {
        return _command->canIgnorePrepareConflicts();
    }

    void doCheckAuthorization(OperationContext* opCtx) const override {
        uassertStatusOK(_command->checkAuthForOperation(opCtx, _dbName, _request.body));
    }

    const BSONObj& cmdObj() const {
        return _request.body;
    }

    BasicCommandWithReplyBuilderInterface* const _command;
    const OpMsgRequest _request;
    const DatabaseName _dbName;
};

Command::~Command() = default;

void Command::snipForLogging(mutablebson::Document* cmdObj) const {
    auto sensitiveFields = sensitiveFieldNames();
    if (!sensitiveFields.empty()) {
        for (auto& sensitiveField : sensitiveFields) {
            for (mutablebson::Element element =
                     mutablebson::findFirstChildNamed(cmdObj->root(), sensitiveField);
                 element.ok();
                 element = mutablebson::findElementNamed(element.rightSibling(), sensitiveField)) {
                uassertStatusOK(element.setValueString("xxx"));
            }
        }
    }
}


std::unique_ptr<CommandInvocation> BasicCommandWithReplyBuilderInterface::parse(
    OperationContext* opCtx, const OpMsgRequest& request) {
    CommandHelpers::uassertNoDocumentSequences(getName(), request);
    return std::make_unique<Invocation>(opCtx, request, this);
}

Command::Command(StringData name, std::vector<StringData> aliases)
    : _name(name.toString()), _aliases(std::move(aliases)) {}

void Command::initializeClusterRole(ClusterRole role) {
    for (auto&& [ptr, stat] : {
             std::pair{&_commandsExecuted, "total"},
             std::pair{&_commandsFailed, "failed"},
             std::pair{&_commandsRejected, "rejected"},
         })
        *ptr = &*MetricBuilder<Counter64>{"commands.{}.{}"_format(_name, stat)}.setRole(role);
    doInitializeClusterRole(role);
}

const std::set<std::string>& Command::apiVersions() const {
    return kNoApiVersions;
}

const std::set<std::string>& Command::deprecatedApiVersions() const {
    return kNoApiVersions;
}

bool Command::hasAlias(const StringData& alias) const {
    return getName() == alias ||
        std::find(_aliases.begin(), _aliases.end(), alias) != _aliases.end();
}

Status BasicCommandWithReplyBuilderInterface::explain(OperationContext* opCtx,
                                                      const OpMsgRequest& request,
                                                      ExplainOptions::Verbosity verbosity,
                                                      rpc::ReplyBuilderInterface* result) const {
    return {ErrorCodes::IllegalOperation, str::stream() << "Cannot explain cmd: " << getName()};
}

void Command::generateHelpResponse(OperationContext* opCtx,
                                   rpc::ReplyBuilderInterface* replyBuilder,
                                   const Command& command) {
    BSONObjBuilder helpBuilder;
    helpBuilder.append("help",
                       str::stream() << "help for: " << command.getName() << " " << command.help());
    replyBuilder->setCommandReply(helpBuilder.obj());
}

bool ErrmsgCommandDeprecated::run(OperationContext* opCtx,
                                  const DatabaseName& dbName,
                                  const BSONObj& cmdObj,
                                  BSONObjBuilder& result) {
    std::string errmsg;
    auto ok = errmsgRun(opCtx, dbName, cmdObj, errmsg, result);
    if (!errmsg.empty()) {
        CommandHelpers::appendSimpleCommandStatus(result, ok, errmsg);
    }
    return ok;
}

//////////////////////////////////////////////////////////////
// CommandRegistry

CommandRegistry* getCommandRegistry(Service* service) {
    auto role = service->role();
    static auto makeReg = [](Service* service) {
        CommandRegistry reg;
        // `reg` will be a singleton registry, so create a per-service unknowns
        // counter for it.
        auto unknowns = &*MetricBuilder<Counter64>{"commands.<UNKNOWN>"}.setRole(service->role());
        reg.setOnUnknownCommandCallback([unknowns] { unknowns->increment(); });
        globalCommandConstructionPlan().execute(&reg, service);
        return reg;
    };
    if (role.hasExclusively(ClusterRole::ShardServer)) {
        static StaticImmortal obj = makeReg(service);
        return &*obj;
    }
    if (role.hasExclusively(ClusterRole::RouterServer)) {
        static StaticImmortal obj = makeReg(service);
        return &*obj;
    }
    MONGO_UNREACHABLE;  // Service role has to be exclusively Shard or Router.
}

void CommandRegistry::registerCommand(Command* command) {
    StringData name = command->getName();
    std::vector<StringData> aliases = command->getAliases();
    auto ep = std::make_unique<Entry>();
    ep->command = command;
    auto [cIt, cOk] = _commands.emplace(command, std::move(ep));
    invariant(cOk, "Command identity collision: {}"_format(name));

    // When a `Command*` is introduced to `_commands`, its names are introduced
    // to `_commandNames`.
    aliases.push_back(name);
    for (StringData key : aliases) {
        if (key.empty())
            continue;
        auto [nIt, nOk] = _commandNames.try_emplace(key, command);
        invariant(nOk, "Command name collision: {}"_format(key));
    }
}

namespace {
boost::optional<ClusterRole> getRegistryRole(const CommandRegistry* reg) {
    if (auto sc = getGlobalServiceContext())
        for (auto r : {ClusterRole::ShardServer, ClusterRole::RouterServer})
            if (auto srv = sc->getService(r); srv && getCommandRegistry(srv) == reg)
                return ClusterRole(r);
    return {};
}
}  // namespace
Command* CommandRegistry::findCommand(StringData name) const {
    auto it = _commandNames.find(name);
    if (it == _commandNames.end()) {
        LOGV2_DEBUG(8097101,
                    kFailedFindCommandDebugLevel,
                    "Failed findCommand",
                    "name"_attr = name,
                    "registryRole"_attr = getRegistryRole(this));
        return nullptr;
    }
    return it->second;
}

CommandConstructionPlan& globalCommandConstructionPlan() {
    static StaticImmortal<CommandConstructionPlan> obj{};
    return *obj;
}

BSONObj toBSON(const CommandConstructionPlan::Entry& e) {
    BSONObjBuilder bob;
    bob.append("expr", e.expr);
    bob.append("roles", toString(e.roles.value_or(ClusterRole::None)));
    if (e.location)
        bob.append("loc", "{}:{}"_format(e.location->file_name(), e.location->line()));
    return bob.obj();
}

void CommandConstructionPlan::execute(CommandRegistry* registry,
                                      Service* service,
                                      const std::function<bool(const Entry&)>& pred) const {
    LOGV2_DEBUG(8043400, 3, "Constructing Command objects from specs");
    for (auto&& entry : entries()) {
        if (entry->testOnly && !getTestCommandsEnabled()) {
            LOGV2_DEBUG(8043401, 3, "Skipping test-only command", "entry"_attr = *entry);
            continue;
        }
        if (entry->featureFlag &&
            !entry->featureFlag->isEnabledUseLatestFCVWhenUninitialized(
                serverGlobalParams.featureCompatibility.acquireFCVSnapshot())) {
            LOGV2_DEBUG(8043402, 3, "Skipping FeatureFlag gated command", "entry"_attr = *entry);
            continue;
        }
        if (!pred(*entry)) {
            LOGV2_DEBUG(8043403, 3, "Skipping command for failed predicate", "entry"_attr = *entry);
            continue;
        }
        auto c = entry->construct();
        c->initializeClusterRole(service ? service->role() : ClusterRole{});
        LOGV2_DEBUG(8043404, 3, "Created", "command"_attr = c->getName(), "entry"_attr = *entry);
        registry->registerCommand(&*c);

        // In the future, we should get to the point where the registry owns the
        // command object. But we aren't there yet and they have to be leaked,
        // So we at least do it as an explicit choice here.
        static StaticImmortal leakedCommands = std::vector<std::unique_ptr<Command>>{};
        leakedCommands->push_back(std::move(c));
    }
}

void CommandConstructionPlan::execute(CommandRegistry* registry, Service* service) const {
    execute(registry, service, [r = service->role()](const auto& e) {
        invariant(e.roles, "All commands must have a role.");
        return e.roles->has(r);
    });
}

}  // namespace mongo
