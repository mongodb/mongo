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

#include "mongo/logv2/log_severity.h"
#include <boost/optional.hpp>
#include <boost/smart_ptr.hpp>
#include <fmt/format.h>
#include <string>
#include <utility>

#include <absl/container/node_hash_map.h>
#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

#include "mongo/base/error_codes.h"
#include "mongo/base/init.h"  // IWYU pragma: keep
#include "mongo/base/status.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/bson/timestamp.h"
#include "mongo/client/read_preference.h"
#include "mongo/db/api_parameters.h"
#include "mongo/db/client.h"
#include "mongo/db/commands.h"
#include "mongo/db/commands/txn_cmds_gen.h"
#include "mongo/db/curop.h"
#include "mongo/db/database_name.h"
#include "mongo/db/default_max_time_ms_cluster_parameter.h"
#include "mongo/db/error_labels.h"
#include "mongo/db/generic_argument_util.h"
#include "mongo/db/initialize_operation_session_info.h"
#include "mongo/db/logical_time.h"
#include "mongo/db/logical_time_validator.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/not_primary_error_tracker.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/operation_time_tracker.h"
#include "mongo/db/query/query_request_helper.h"
#include "mongo/db/read_concern_support_result.h"
#include "mongo/db/read_write_concern_defaults.h"
#include "mongo/db/read_write_concern_defaults_gen.h"
#include "mongo/db/read_write_concern_provenance.h"
#include "mongo/db/repl/optime.h"
#include "mongo/db/repl/read_concern_args.h"
#include "mongo/db/repl/read_concern_level.h"
#include "mongo/db/replica_set_endpoint_util.h"
#include "mongo/db/session/logical_session_id.h"
#include "mongo/db/session/logical_session_id_gen.h"
#include "mongo/db/stats/api_version_metrics.h"
#include "mongo/db/stats/counters.h"
#include "mongo/db/transaction_validation.h"
#include "mongo/db/validate_api_parameters.h"
#include "mongo/db/vector_clock.h"
#include "mongo/db/write_concern_options.h"
#include "mongo/logv2/log.h"
#include "mongo/logv2/log_attr.h"
#include "mongo/logv2/log_component.h"
#include "mongo/logv2/log_severity.h"
#include "mongo/logv2/log_severity_suppressor.h"
#include "mongo/logv2/redaction.h"
#include "mongo/platform/compiler.h"
#include "mongo/rpc/check_allowed_op_query_cmd.h"
#include "mongo/rpc/factory.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/rpc/message.h"
#include "mongo/rpc/metadata.h"
#include "mongo/rpc/metadata/client_metadata.h"
#include "mongo/rpc/op_msg.h"
#include "mongo/rpc/protocol.h"
#include "mongo/rpc/reply_builder_interface.h"
#include "mongo/rpc/rewrite_state_change_errors.h"
#include "mongo/rpc/topology_version_gen.h"
#include "mongo/s/analyze_shard_key_role.h"
#include "mongo/s/cannot_implicitly_create_collection_info.h"
#include "mongo/s/catalog_cache.h"
#include "mongo/s/cluster_ddl.h"
#include "mongo/s/commands/strategy.h"
#include "mongo/s/grid.h"
#include "mongo/s/load_balancer_support.h"
#include "mongo/s/mongod_and_mongos_server_parameters_gen.h"
#include "mongo/s/mongos_topology_coordinator.h"
#include "mongo/s/query_analysis_sampler.h"
#include "mongo/s/session_catalog_router.h"
#include "mongo/s/stale_exception.h"
#include "mongo/s/transaction_participant_failed_unyield_exception.h"
#include "mongo/s/transaction_router.h"
#include "mongo/transport/hello_metrics.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/decorable.h"
#include "mongo/util/duration.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/namespace_string_util.h"
#include "mongo/util/scopeguard.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding

namespace mongo {
namespace {
using namespace fmt::literals;

MONGO_FAIL_POINT_DEFINE(hangBeforeCheckingMongosShutdownInterrupt);
const auto kOperationTime = "operationTime"_sd;

void runCommandInvocation(const RequestExecutionContext* rec, CommandInvocation* invocation) {
    CommandHelpers::runCommandInvocation(rec->getOpCtx(), invocation, rec->getReplyBuilder());
}

/**
 * Append required fields to command response.
 */
void appendRequiredFieldsToResponse(OperationContext* opCtx, BSONObjBuilder* responseBuilder) {
    // The appended operationTime must always be <= the appended $clusterTime, so in case we need to
    // use $clusterTime as the operationTime below, take a $clusterTime value which is guaranteed to
    // be <= the value output by gossipOut().
    const auto currentTime = VectorClock::get(opCtx)->getTime();
    const auto clusterTime = currentTime.clusterTime();

    bool clusterTimeWasOutput = VectorClock::get(opCtx)->gossipOut(opCtx, responseBuilder);

    // Ensure that either both operationTime and $clusterTime are output, or neither.
    if (MONGO_likely(clusterTimeWasOutput)) {
        auto operationTime = OperationTimeTracker::get(opCtx)->getMaxOperationTime();
        if (VectorClock::isValidComponentTime(operationTime)) {
            LOGV2_DEBUG(22764,
                        5,
                        "Appending operationTime",
                        "operationTime"_attr = operationTime.asTimestamp());
            operationTime.appendAsOperationTime(responseBuilder);
        } else if (VectorClock::isValidComponentTime(clusterTime)) {
            // If we don't know the actual operation time, use the cluster time instead. This is
            // safe but not optimal because we can always return a later operation time than
            // actual.
            LOGV2_DEBUG(22765,
                        5,
                        "Appending clusterTime as operationTime",
                        "clusterTime"_attr = clusterTime.asTimestamp());
            clusterTime.appendAsOperationTime(responseBuilder);
        }
    }
}

/**
 * Invokes the given command and aborts the transaction on any non-retryable errors.
 */
void invokeInTransactionRouter(TransactionRouter::Router& txnRouter,
                               const RequestExecutionContext* rec,
                               CommandInvocation* invocation) {
    auto opCtx = rec->getOpCtx();
    txnRouter.setDefaultAtClusterTime(opCtx);

    try {
        runCommandInvocation(rec, invocation);
    } catch (const DBException& ex) {
        auto status = ex.toStatus();

        if (auto code = status.code(); ErrorCodes::isSnapshotError(code) ||
            ErrorCodes::isNeedRetargettingError(code) || code == ErrorCodes::StaleDbVersion ||
            code == ErrorCodes::ShardCannotRefreshDueToLocksHeld ||
            code == ErrorCodes::WouldChangeOwningShard) {
            // Don't abort on possibly retryable errors.
            throw;
        }

        auto opCtx = rec->getOpCtx();

        // Abort if the router wasn't yielded, which may happen at global shutdown.
        if (auto txnRouter = TransactionRouter::get(opCtx)) {
            txnRouter.implicitlyAbortTransaction(opCtx, status);
        }

        throw;
    }
}

/**
 * Adds info from the active transaction and the given reason as context to the active exception.
 */
void addContextForTransactionAbortingError(StringData txnIdAsString,
                                           StmtId latestStmtId,
                                           Status& status,
                                           StringData reason) {
    status.addContext("Transaction {} was aborted on statement {} due to: {}"_format(
        txnIdAsString, latestStmtId, reason));
}

// Type that executes the invocation against the database.
class ExecCommandClient final {
public:
    ExecCommandClient(ExecCommandClient&&) = delete;
    ExecCommandClient(const ExecCommandClient&) = delete;

    ExecCommandClient(RequestExecutionContext* rec, CommandInvocation* invocation)
        : _rec(rec), _invocation(invocation) {}

    void run();

private:
    // Prepare the environment for running the invocation (e.g., checking authorization).
    void _prologue();

    // Runs the command invocation.
    void _run();

    // Any logic that must be done post command execution, unless an exception is thrown.
    void _epilogue();

    // Runs at the end of `run()` unless an exception, other than
    // `ErrorCodes::SkipCommandExecution`, is thrown earlier.
    void _onCompletion();

    const RequestExecutionContext* _rec;
    CommandInvocation* _invocation;
};

void ExecCommandClient::_prologue() {
    auto opCtx = _rec->getOpCtx();
    auto result = _rec->getReplyBuilder();
    const auto& request = _rec->getRequest();

    const auto& dbname = _invocation->db();
    uassert(ErrorCodes::IllegalOperation,
            "Can't use 'local' database through mongos",
            !dbname.isLocalDB());
    uassert(ErrorCodes::InvalidNamespace,
            "Invalid database name: '{}'"_format(dbname.toStringForErrorMsg()),
            DatabaseName::isValid(dbname, DatabaseName::DollarInDbNameBehavior::Allow));

    try {
        _invocation->checkAuthorization(opCtx, request);
    } catch (const DBException& e) {
        auto body = result->getBodyBuilder();
        CommandHelpers::appendCommandStatusNoThrow(body, e.toStatus());
        iassert(Status(ErrorCodes::SkipCommandExecution, "Failed to check authorization"));
    }
}

void ExecCommandClient::_run() {
    OperationContext* opCtx = _rec->getOpCtx();
    if (auto txnRouter = TransactionRouter::get(opCtx); txnRouter) {
        invokeInTransactionRouter(txnRouter, _rec, _invocation);
    } else {
        runCommandInvocation(_rec, _invocation);
    }
}

void ExecCommandClient::_epilogue() {
    auto opCtx = _rec->getOpCtx();
    auto result = _rec->getReplyBuilder();
    if (_invocation->supportsWriteConcern()) {
        failCommand.executeIf(
            [&](const BSONObj& data) {
                rpc::RewriteStateChangeErrors::onActiveFailCommand(opCtx, data);
                result->getBodyBuilder().append(data["writeConcernError"]);
                if (data.hasField(kErrorLabelsFieldName) &&
                    data[kErrorLabelsFieldName].type() == Array) {
                    auto labels = data.getObjectField(kErrorLabelsFieldName).getOwned();
                    if (!labels.isEmpty()) {
                        result->getBodyBuilder().append(kErrorLabelsFieldName, BSONArray(labels));
                    }
                }
            },
            [&](const BSONObj& data) {
                return CommandHelpers::shouldActivateFailCommandFailPoint(
                           data, _invocation, opCtx->getClient()) &&
                    data.hasField("writeConcernError");
            });
    }

    auto body = result->getBodyBuilder();

    if (bool ok = CommandHelpers::extractOrAppendOk(body); !ok) {
        const Command* c = _invocation->definition();
        c->incrementCommandsFailed();

        auto status = getStatusFromCommandResult(body.asTempObj());
        if (auto txnRouter = TransactionRouter::get(opCtx)) {
            txnRouter.implicitlyAbortTransaction(opCtx, status);
        }

        // Throw this error so that it will be upconverted into the orignal error before being
        // returned back to the client
        if (status.code() == ErrorCodes::TransactionParticipantFailedUnyield) {
            iassert(status);
        }
    }
}

void ExecCommandClient::_onCompletion() {
    auto opCtx = _rec->getOpCtx();
    auto body = _rec->getReplyBuilder()->getBodyBuilder();
    appendRequiredFieldsToResponse(opCtx, &body);
}

void ExecCommandClient::run() {
    auto status = [&] {
        try {
            _prologue();
            _run();
            _epilogue();
            return Status::OK();
        } catch (const DBException& ex) {
            return ex.toStatus();
        }
    }();

    if (MONGO_unlikely(!status.isOK() && status.code() != ErrorCodes::SkipCommandExecution))
        iasserted(status);  // Execution was interrupted due to an error.

    _onCompletion();
}

MONGO_FAIL_POINT_DEFINE(doNotRefreshShardsOnRetargettingError);

/**
 * Produces a type that parses the command, runs the parsed command, and captures the result in
 * replyBuilder.
 */
class ParseAndRunCommand final {
public:
    ParseAndRunCommand(const ParseAndRunCommand&) = delete;
    ParseAndRunCommand(ParseAndRunCommand&&) = delete;

    ParseAndRunCommand(RequestExecutionContext* rec, BSONObjBuilder* errorBuilder)
        : _rec(rec),
          _errorBuilder(errorBuilder),
          _opType(_rec->getMessage().operation()),
          _commandName(_rec->getRequest().getCommandName()) {}

    void run();

private:
    class RunInvocation;
    class RunAndRetry;

    // Prepares the environment for running the command (e.g., parsing the command to produce the
    // invocation and extracting read/write concerns).
    void _parseCommand();

    // updates statistics and applies labels if an error occurs.
    void _updateStatsAndApplyErrorLabels(const Status& status);

    RequestExecutionContext* _rec;
    BSONObjBuilder* _errorBuilder;
    const NetworkOp _opType;
    const StringData _commandName;

    std::shared_ptr<CommandInvocation> _invocation;
    boost::optional<NamespaceString> _ns;
    OperationSessionInfoFromClient _osi;
    boost::optional<WriteConcernOptions> _wc;
    boost::optional<bool> _isHello;
};

/*
 * Produces a type that runs the invocation and capture the result in replyBuilder.
 */
class ParseAndRunCommand::RunInvocation final {
public:
    RunInvocation(RunInvocation&&) = delete;
    RunInvocation(const RunInvocation&) = delete;

    explicit RunInvocation(ParseAndRunCommand* parc) : _parc(parc) {}

    void run();

private:
    Status _setup();

    ParseAndRunCommand* const _parc;

    boost::optional<RouterOperationContextSession> _routerSession;
};

/*
 * Produces a type that runs the invocation and retries if necessary.
 */
class ParseAndRunCommand::RunAndRetry final {
public:
    RunAndRetry(RunAndRetry&&) = delete;
    RunAndRetry(const RunAndRetry&) = delete;

    explicit RunAndRetry(ParseAndRunCommand* parc) : _parc(parc) {}

    void run();

private:
    bool _canRetry() const {
        return _tries < gMaxNumStaleVersionRetries.load();
    }

    // Sets up the environment for running the invocation, and clears the state from the last try.
    void _setup();

    void _run();

    // Exception handler for error codes that may trigger a retry. All methods will throw `status`
    // unless an attempt to retry is possible.
    void _checkRetryForTransaction(Status& status);
    void _onNeedRetargetting(Status& status);
    void _onStaleDbVersion(Status& status);
    void _onSnapshotError(Status& status);
    void _onShardCannotRefreshDueToLocksHeldError(Status& status);
    void _onCannotImplicitlyCreateCollection(Status& status);

    ParseAndRunCommand* const _parc;

    int _tries = 0;
};
void ParseAndRunCommand::_updateStatsAndApplyErrorLabels(const Status& status) {
    auto opCtx = _rec->getOpCtx();
    const auto command = _rec->getCommand();

    NotPrimaryErrorTracker::get(opCtx->getClient()).recordError(status.code());

    if (!command)
        return;


    if (status.code() == ErrorCodes::QueryRejectedBySettings) {
        command->incrementCommandsRejected();
    } else {
        command->incrementCommandsFailed();
    }

    // WriteConcern error (wcCode) is set to boost::none because:
    // 1. TransientTransaction error label handling for commitTransaction command in mongos is
    //    delegated to the shards. Mongos simply propagates the shard's response up to the client.
    // 2. For other commands in a transaction, they shouldn't get a writeConcern error so this
    //    setting doesn't apply.
    auto errorLabels = getErrorLabels(opCtx,
                                      _osi,
                                      command->getName(),
                                      status.code(),
                                      boost::none,
                                      false /* isInternalClient */,
                                      true /* isMongos */,
                                      false /* isComingFromRouter */,
                                      repl::OpTime{},
                                      repl::OpTime{});
    _errorBuilder->appendElements(errorLabels);
}
void ParseAndRunCommand::_parseCommand() {
    auto opCtx = _rec->getOpCtx();
    const auto& m = _rec->getMessage();
    const auto& request = _rec->getRequest();
    auto replyBuilder = _rec->getReplyBuilder();

    auto const command = CommandHelpers::findCommand(opCtx, _commandName);
    if (!command) {
        const std::string errorMsg = "no such cmd: {}"_format(_commandName);
        auto builder = replyBuilder->getBodyBuilder();
        CommandHelpers::appendCommandStatusNoThrow(builder,
                                                   {ErrorCodes::CommandNotFound, errorMsg});
        getCommandRegistry(opCtx)->incrementUnknownCommands();
        appendRequiredFieldsToResponse(opCtx, &builder);
        iassert(Status(ErrorCodes::SkipCommandExecution, errorMsg));
    }

    _rec->setCommand(command);

    _isHello.emplace(command->getName() == "hello"_sd || command->getName() == "isMaster"_sd);

    opCtx->setExhaust(OpMsg::isFlagSet(m, OpMsg::kExhaustSupported));
    Client* client = opCtx->getClient();
    const auto session = client->session();
    if (session) {
        if (!opCtx->isExhaust() || !_isHello.value()) {
            InExhaustHello::get(session.get())->setInExhaust(false, _commandName);
        }
    }

    CommandHelpers::uassertShouldAttemptParse(opCtx, command, request);

    invariant(!auth::ValidatedTenancyScope::get(opCtx).has_value() ||
              (request.validatedTenancyScope &&
               *auth::ValidatedTenancyScope::get(opCtx) == *(request.validatedTenancyScope)));

    auth::ValidatedTenancyScope::set(opCtx, request.validatedTenancyScope);

    _invocation = command->parse(opCtx, request);

    // If the command includes a 'comment' field, set it on the current OpCtx.
    if (auto& commentField = _invocation->getGenericArguments().getComment()) {
        stdx::lock_guard<Client> lk(*client);
        opCtx->setComment(commentField->getElement().wrap());
    }

    auto apiParams = parseAndValidateAPIParameters(*_invocation);
    {
        // We must obtain the client lock to set APIParameters on the operation context, as it may
        // be concurrently read by CurrentOp.
        stdx::lock_guard<Client> lk(*client);
        APIParameters::get(opCtx) = APIParameters::fromClient(std::move(apiParams));
    }

    rpc::readRequestMetadata(opCtx, _invocation->getGenericArguments(), command->requiresAuth());

    CommandInvocation::set(opCtx, _invocation);

    // Set the logical optype, command object and namespace as soon as we identify the command. If
    // the command does not define a fully-qualified namespace, set CurOp to the generic command
    // namespace db.$cmd.
    _ns.emplace(_invocation->ns());
    const auto nss = (NamespaceString(_invocation->db()) == *_ns
                          ? NamespaceString::makeCommandNamespace(_invocation->ns().dbName())
                          : _invocation->ns());

    // Fill out all currentOp details.
    {
        stdx::lock_guard<Client> lk(*client);
        CurOp::get(opCtx)->setGenericOpRequestDetails(lk, nss, command, request.body, _opType);
    }

    _osi = initializeOperationSessionInfo(opCtx,
                                          request.getValidatedTenantId(),
                                          generic_argument_util::getOperationSessionInfoFromClient(
                                              _invocation->getGenericArguments()),
                                          command->requiresAuth(),
                                          command->attachLogicalSessionsToOpCtx(),
                                          true);

    auto allowTransactionsOnConfigDatabase =
        !serverGlobalParams.clusterRole.hasExclusively(ClusterRole::RouterServer) ||
        client->isFromSystemConnection();

    // If there are multiple namespaces this command operates on we need to validate them all
    // explicitly. Otherwise we can use the nss defined above which may be the generic command
    // namespace.
    std::vector<NamespaceString> namespaces = {nss};
    if (_invocation->allNamespaces().size() > 1) {
        namespaces = _invocation->allNamespaces();
    }
    validateSessionOptions(_osi, command, namespaces, allowTransactionsOnConfigDatabase);

    if (auto readPreference = _invocation->getGenericArguments().getReadPreference();
        readPreference && readPreference->hedgingMode) {
        static logv2::SeveritySuppressor logSeverity{
            Minutes{1}, logv2::LogSeverity::Warning(), logv2::LogSeverity::Debug(5)};
        LOGV2_DEBUG(9206200,
                    logSeverity().toInt(),
                    "Hedged reads have been deprecated. For more information please see "
                    "https://dochub.mongodb.org/core/hedged-reads-deprecated");
    }

    _wc.emplace(
        _invocation->getGenericArguments().getWriteConcern().value_or(WriteConcernOptions()));

    auto& readConcernArgs = repl::ReadConcernArgs::get(opCtx);
    {
        // We must obtain the client lock to set ReadConcernArgs on the operation context, as it may
        // be concurrently read by CurrentOp.
        stdx::lock_guard<Client> lk(*client);
        if (auto& rc = _invocation->getGenericArguments().getReadConcern()) {
            readConcernArgs = *rc;
        }
    }

    if (MONGO_unlikely(_invocation->getGenericArguments().getHelp().value_or(false))) {
        const Command* c = _invocation->definition();
        auto result = _rec->getReplyBuilder();
        auto body = result->getBodyBuilder();
        body.append(CommandHelpers::kHelpFieldName,
                    "help for: {} {}"_format(c->getName(), c->help()));
        CommandHelpers::appendSimpleCommandStatus(body, true, "");
        iassert(Status(ErrorCodes::SkipCommandExecution, "Already served help command"));
    }
}

bool isInternalClient(OperationContext* opCtx) {
    return opCtx->getClient()->session() && opCtx->getClient()->isInternalClient();
}

Status ParseAndRunCommand::RunInvocation::_setup() {
    const auto& invocation = _parc->_invocation;
    auto opCtx = _parc->_rec->getOpCtx();
    auto command = _parc->_rec->getCommand();
    const auto& request = _parc->_rec->getRequest();
    auto replyBuilder = _parc->_rec->getReplyBuilder();
    auto& genericArgs = invocation->getGenericArguments();

    if (command->getLogicalOp() != LogicalOp::opGetMore) {
        auto [requestOrDefaultMaxTimeMS, usesDefaultMaxTimeMS] = getRequestOrDefaultMaxTimeMS(
            opCtx, genericArgs.getMaxTimeMS(), invocation->isReadOperation());
        if (auto maxTimeMS = requestOrDefaultMaxTimeMS.value_or(Milliseconds{0});
            requestOrDefaultMaxTimeMS > Milliseconds::zero()) {
            opCtx->setDeadlineAfterNowBy(maxTimeMS, ErrorCodes::MaxTimeMSExpired);
        }
        opCtx->setUsesDefaultMaxTimeMS(usesDefaultMaxTimeMS);
    }

    if (MONGO_unlikely(
            hangBeforeCheckingMongosShutdownInterrupt.shouldFail([&](const BSONObj& data) {
                if (data.hasField("cmdName") && data.hasField("ns")) {
                    const auto cmdNss = _parc->_ns.value();
                    const auto fpNss = NamespaceStringUtil::parseFailPointData(data, "ns"_sd);
                    return (data.getStringField("cmdName") == _parc->_commandName &&
                            fpNss == cmdNss);
                }
                return false;
            }))) {

        LOGV2(6217501, "Hanging before hangBeforeCheckingMongosShutdownInterrupt is cancelled");
        hangBeforeCheckingMongosShutdownInterrupt.pauseWhileSet();
    }
    opCtx->checkForInterrupt();  // May trigger maxTimeAlwaysTimeOut fail point.

    auto appendStatusToReplyAndSkipCommandExecution = [replyBuilder](Status status) -> Status {
        auto responseBuilder = replyBuilder->getBodyBuilder();
        CommandHelpers::appendCommandStatusNoThrow(responseBuilder, status);
        return Status(ErrorCodes::SkipCommandExecution, status.reason());
    };

    if (MONGO_unlikely(_parc->_isHello.value())) {
        // Preload generic ClientMetadata ahead of our first hello request. After the first
        // request, metaElement should always be empty.
        auto metaElem = request.body[kMetadataDocumentName];
        ClientMetadata::setFromMetadata(opCtx->getClient(), metaElem, false);
    }

    enforceRequireAPIVersion(opCtx, command);

    if (auto clientMetadata = ClientMetadata::get(opCtx->getClient())) {
        auto& apiParams = APIParameters::get(opCtx);
        auto& apiVersionMetrics = APIVersionMetrics::get(opCtx->getServiceContext());
        auto appName = clientMetadata->getApplicationName();
        apiVersionMetrics.update(appName, apiParams);
    }

    CommandHelpers::evaluateFailCommandFailPoint(opCtx, invocation.get());
    bool startTransaction = false;
    if (_parc->_osi.getAutocommit()) {
        _routerSession.emplace(opCtx);

        load_balancer_support::setMruSession(opCtx->getClient(), *opCtx->getLogicalSessionId());

        auto txnRouter = TransactionRouter::get(opCtx);
        invariant(txnRouter);

        auto txnNumber = opCtx->getTxnNumber();
        invariant(txnNumber);

        auto transactionAction = ([&] {
            auto startTxnSetting = _parc->_osi.getStartTransaction();
            if (startTxnSetting && *startTxnSetting) {
                return TransactionRouter::TransactionActions::kStart;
            }

            if (command->getName() == CommitTransaction::kCommandName) {
                return TransactionRouter::TransactionActions::kCommit;
            }

            return TransactionRouter::TransactionActions::kContinue;
        })();

        startTransaction = (transactionAction == TransactionRouter::TransactionActions::kStart);
        txnRouter.beginOrContinueTxn(opCtx, *txnNumber, transactionAction);
    }

    bool supportsWriteConcern = invocation->supportsWriteConcern();
    if (MONGO_unlikely(!supportsWriteConcern && genericArgs.getWriteConcern())) {
        // This command doesn't do writes so it should not be passed a writeConcern.
        const auto errorMsg = "Command does not support writeConcern";
        return appendStatusToReplyAndSkipCommandExecution({ErrorCodes::InvalidOptions, errorMsg});
    }

    // This is the WC extracted from the command object, so the CWWC or implicit default hasn't been
    // applied yet, which is why "usedDefaultConstructedWC" flag can be used an indicator of whether
    // the client supplied a WC or not.
    bool clientSuppliedWriteConcern = !_parc->_wc->usedDefaultConstructedWC;
    bool customDefaultWriteConcernWasApplied = false;
    bool isInternalClientValue = isInternalClient(opCtx);

    bool canApplyDefaultWC = supportsWriteConcern &&
        (!TransactionRouter::get(opCtx) || command->isTransactionCommand()) &&
        !opCtx->getClient()->isInDirectClient();

    if (canApplyDefaultWC) {
        auto getDefaultWC = ([&]() {
            auto rwcDefaults = ReadWriteConcernDefaults::get(opCtx).getDefault(opCtx);
            auto wcDefault = rwcDefaults.getDefaultWriteConcern();
            const auto defaultWriteConcernSource = rwcDefaults.getDefaultWriteConcernSource();
            customDefaultWriteConcernWasApplied = defaultWriteConcernSource &&
                defaultWriteConcernSource == DefaultWriteConcernSourceEnum::kGlobal;
            return wcDefault;
        });

        if (!clientSuppliedWriteConcern) {
            if (isInternalClientValue) {
                uassert(
                    5569900,
                    "received command without explicit writeConcern on an internalClient connection {}"_format(
                        redact(request.body.toString())),
                    genericArgs.getWriteConcern());
            } else {
                // This command is not from a DBDirectClient or internal client, and supports WC,
                // but wasn't given one - so apply the default, if there is one.
                const auto wcDefault = getDefaultWC();
                // Default WC can be 'boost::none' if the implicit default is used and set to 'w:1'.
                if (wcDefault) {
                    _parc->_wc = *wcDefault;
                    LOGV2_DEBUG(22766,
                                2,
                                "Applying default writeConcern on command",
                                "command"_attr = request.getCommandName(),
                                "writeConcern"_attr = *wcDefault);
                }
            }
        }
        // Client supplied a write concern object without 'w' field.
        else if (_parc->_wc->isExplicitWithoutWField()) {
            const auto wcDefault = getDefaultWC();
            // Default WC can be 'boost::none' if the implicit default is used and set to 'w:1'.
            if (wcDefault) {
                clientSuppliedWriteConcern = false;
                _parc->_wc->w = wcDefault->w;
                if (_parc->_wc->syncMode == WriteConcernOptions::SyncMode::UNSET) {
                    _parc->_wc->syncMode = wcDefault->syncMode;
                }
            }
        }
    }

    if (TransactionRouter::get(opCtx)) {
        validateWriteConcernForTransaction(*_parc->_wc, command);
    }

    if (supportsWriteConcern) {
        auto& provenance = _parc->_wc->getProvenance();

        // ClientSupplied is the only provenance that clients are allowed to pass to mongos.
        if (provenance.hasSource() && !provenance.isClientSupplied()) {
            const auto errorMsg = "writeConcern provenance must be unset or \"{}\""_format(
                ReadWriteConcernProvenance::kClientSupplied);
            return appendStatusToReplyAndSkipCommandExecution(
                {ErrorCodes::InvalidOptions, errorMsg});
        }

        // If the client didn't provide a provenance, then an appropriate value needs to be
        // determined.
        if (!provenance.hasSource()) {
            if (clientSuppliedWriteConcern) {
                provenance.setSource(ReadWriteConcernProvenance::Source::clientSupplied);
            } else if (customDefaultWriteConcernWasApplied) {
                provenance.setSource(ReadWriteConcernProvenance::Source::customDefault);
            } else if (opCtx->getClient()->isInDirectClient() || isInternalClientValue) {
                provenance.setSource(ReadWriteConcernProvenance::Source::internalWriteDefault);
            } else {
                provenance.setSource(ReadWriteConcernProvenance::Source::implicitDefault);
            }
        }

        // Ensure that the WC being set on the opCtx has provenance.
        invariant(_parc->_wc->getProvenance().hasSource(),
                  "unexpected unset provenance on writeConcern: {}"_format(
                      _parc->_wc->toBSON().toString()));

        opCtx->setWriteConcern(*_parc->_wc);
    }

    auto& readConcernArgs = repl::ReadConcernArgs::get(opCtx);
    bool clientSuppliedReadConcern = readConcernArgs.isSpecified();
    bool customDefaultReadConcernWasApplied = false;

    auto readConcernSupport = invocation->supportsReadConcern(readConcernArgs.getLevel(),
                                                              readConcernArgs.isImplicitDefault());

    auto applyDefaultReadConcern = [&](const repl::ReadConcernArgs rcDefault) -> void {
        // We must obtain the client lock to set ReadConcernArgs, because it's an
        // in-place reference to the object on the operation context, which may be
        // concurrently used elsewhere (eg. read by currentOp).
        stdx::lock_guard<Client> lk(*opCtx->getClient());
        LOGV2_DEBUG(22767,
                    2,
                    "Applying default readConcern on command",
                    "command"_attr = invocation->definition()->getName(),
                    "readConcern"_attr = rcDefault);
        readConcernArgs = std::move(rcDefault);
        // Update the readConcernSupport, since the default RC was applied.
        readConcernSupport = invocation->supportsReadConcern(readConcernArgs.getLevel(),
                                                             !customDefaultReadConcernWasApplied);
    };

    auto shouldApplyDefaults = startTransaction || !TransactionRouter::get(opCtx);
    if (readConcernSupport.defaultReadConcernPermit.isOK() && shouldApplyDefaults) {
        if (readConcernArgs.isEmpty()) {
            const auto rwcDefaults = ReadWriteConcernDefaults::get(opCtx).getDefault(opCtx);
            const auto rcDefault = rwcDefaults.getDefaultReadConcern();
            if (rcDefault) {
                const auto readConcernSource = rwcDefaults.getDefaultReadConcernSource();
                customDefaultReadConcernWasApplied =
                    (readConcernSource &&
                     readConcernSource.value() == DefaultReadConcernSourceEnum::kGlobal);

                applyDefaultReadConcern(*rcDefault);
            }
        }
    }

    // Apply the implicit default read concern even if the command does not support a cluster wide
    // read concern.
    if (!readConcernSupport.defaultReadConcernPermit.isOK() &&
        readConcernSupport.implicitDefaultReadConcernPermit.isOK() && shouldApplyDefaults &&
        readConcernArgs.isEmpty()) {
        const auto rcDefault = ReadWriteConcernDefaults::get(opCtx).getImplicitDefaultReadConcern();
        applyDefaultReadConcern(rcDefault);
    }

    auto& provenance = readConcernArgs.getProvenance();

    // ClientSupplied is the only provenance that clients are allowed to pass to mongos.
    if (provenance.hasSource() && !provenance.isClientSupplied()) {
        const auto errorMsg = "readConcern provenance must be unset or \"{}\""_format(
            ReadWriteConcernProvenance::kClientSupplied);
        return appendStatusToReplyAndSkipCommandExecution({ErrorCodes::InvalidOptions, errorMsg});
    }

    // If the client didn't provide a provenance, then an appropriate value needs to be determined.
    if (!provenance.hasSource()) {
        // We must obtain the client lock to set the provenance of the opCtx's ReadConcernArgs as it
        // may be concurrently read by CurrentOp.
        stdx::lock_guard<Client> lk(*opCtx->getClient());
        if (clientSuppliedReadConcern) {
            provenance.setSource(ReadWriteConcernProvenance::Source::clientSupplied);
        } else if (customDefaultReadConcernWasApplied) {
            provenance.setSource(ReadWriteConcernProvenance::Source::customDefault);
        } else {
            provenance.setSource(ReadWriteConcernProvenance::Source::implicitDefault);
        }
    }

    // Ensure that the RC on the opCtx has provenance.
    invariant(readConcernArgs.getProvenance().hasSource(),
              "unexpected unset provenance on readConcern: {}"_format(
                  readConcernArgs.toBSONInner().toString()));

    // If we are starting a transaction, we only need to check whether the read concern is
    // appropriate for running a transaction. There is no need to check whether the specific command
    // supports the read concern, because all commands that are allowed to run in a transaction must
    // support all applicable read concerns.
    if (startTransaction) {
        if (!isReadConcernLevelAllowedInTransaction(readConcernArgs.getLevel())) {
            const auto errorMsg =
                "The readConcern level must be either 'local' (default), 'majority' or "
                "'snapshot' in order to run in a transaction";
            return appendStatusToReplyAndSkipCommandExecution(
                {ErrorCodes::InvalidOptions, errorMsg});
        }
        if (readConcernArgs.getArgsOpTime()) {
            const std::string errorMsg =
                "The readConcern cannot specify '{}' in a transaction"_format(
                    repl::ReadConcernArgs::kAfterOpTimeFieldName);
            return appendStatusToReplyAndSkipCommandExecution(
                {ErrorCodes::InvalidOptions, errorMsg});
        }
    }

    // Otherwise, if there is a read concern present - either user-specified or the default - then
    // check whether the command supports it. If there is no explicit read concern level, then it is
    // implicitly "local". There is no need to check whether this is supported, because all commands
    // either support "local" or upconvert the absent readConcern to a stronger level that they do
    // support; e.g. $changeStream upconverts to RC "majority".
    //
    // Individual transaction statements are checked later on, after we've unstashed the transaction
    // resources.
    if (MONGO_unlikely(!TransactionRouter::get(opCtx) && readConcernArgs.hasLevel() &&
                       !readConcernSupport.readConcernSupport.isOK())) {
        const std::string errorMsg = "Command {} does not support {}"_format(
            invocation->definition()->getName(), readConcernArgs.toString());
        return appendStatusToReplyAndSkipCommandExecution(
            readConcernSupport.readConcernSupport.withContext(errorMsg));
    }

    // Remember whether or not this operation is starting a transaction, in case something later in
    // the execution needs to adjust its behavior based on this.
    opCtx->setIsStartingMultiDocumentTransaction(startTransaction);

    command->incrementCommandsExecuted();

    if (command->shouldAffectCommandCounter()) {
        serviceOpCounters(opCtx).gotCommand();
        if (analyze_shard_key::supportsSamplingQueries(opCtx)) {
            analyze_shard_key::QueryAnalysisSampler::get(opCtx).gotCommand(command->getName());
        }
    }

    if (command->shouldAffectQueryCounter()) {
        serviceOpCounters(opCtx).gotQuery();
    }

    if (opCtx->routedByReplicaSetEndpoint()) {
        replica_set_endpoint::checkIfCanRunCommand(opCtx, request);
    }

    return Status::OK();
}

void ParseAndRunCommand::RunAndRetry::_setup() {
    auto opCtx = _parc->_rec->getOpCtx();
    const auto command = _parc->_rec->getCommand();
    const auto& request = _parc->_rec->getRequest();
    auto& readConcernArgs = repl::ReadConcernArgs::get(opCtx);

    if (_tries > 1) {
        // Re-parse before retrying in case the process of run()-ning the invocation could
        // affect the parsed result.
        _parc->_invocation = command->parse(opCtx, request);
        invariant(_parc->_invocation->ns() == _parc->_ns,
                  "unexpected change of namespace when retrying");
    }

    // On each try, select the latest known clusterTime as the atClusterTime for snapshot reads
    // outside of transactions.
    if (readConcernArgs.getLevel() == repl::ReadConcernLevel::kSnapshotReadConcern &&
        !TransactionRouter::get(opCtx) &&
        (!readConcernArgs.getArgsAtClusterTime() || readConcernArgs.wasAtClusterTimeSelected())) {
        auto atClusterTime = [](OperationContext* opCtx, repl::ReadConcernArgs& readConcernArgs) {
            const auto latestKnownTime = VectorClock::get(opCtx)->getTime();
            // Choose a time after the user-supplied afterClusterTime.
            auto afterClusterTime = readConcernArgs.getArgsAfterClusterTime();
            if (afterClusterTime && *afterClusterTime > latestKnownTime.clusterTime()) {
                return afterClusterTime->asTimestamp();
            }
            return latestKnownTime.clusterTime().asTimestamp();
        }(opCtx, readConcernArgs);
        readConcernArgs.setArgsAtClusterTimeForSnapshot(atClusterTime);
    }

    _parc->_rec->getReplyBuilder()->reset();
}

void ParseAndRunCommand::RunAndRetry::_run() {
    ExecCommandClient runner(_parc->_rec, _parc->_invocation.get());
    runner.run();

    auto opCtx = _parc->_rec->getOpCtx();
    auto responseBuilder = _parc->_rec->getReplyBuilder()->getBodyBuilder();
    if (auto txnRouter = TransactionRouter::get(opCtx)) {
        txnRouter.appendRecoveryToken(&responseBuilder);
    }
}

void ParseAndRunCommand::RunAndRetry::_checkRetryForTransaction(Status& status) {
    // Retry logic specific to transactions. Throws and aborts the transaction if the error
    // cannot be retried on.
    auto opCtx = _parc->_rec->getOpCtx();
    auto txnRouter = TransactionRouter::get(opCtx);
    if (!txnRouter) {
        if (opCtx->inMultiDocumentTransaction()) {
            // This command must have failed while its session was yielded. We cannot retry in
            // this case, whatever the session was yielded to is responsible for that, so
            // rethrow the error.
            iassert(status);
        }

        return;
    }

    ScopeGuard abortGuard([&] { txnRouter.implicitlyAbortTransaction(opCtx, status); });

    if (!_canRetry()) {
        addContextForTransactionAbortingError(
            txnRouter.txnIdToString(), txnRouter.getLatestStmtId(), status, "exhausted retries");
        iassert(status);
    }

    // TODO SERVER-39704 Allow mongos to retry on stale shard, stale db, snapshot, or shard
    // invalidated for targeting errors.
    if (ErrorCodes::isA<ErrorCategory::SnapshotError>(status)) {
        if (!txnRouter.canContinueOnSnapshotError()) {
            addContextForTransactionAbortingError(txnRouter.txnIdToString(),
                                                  txnRouter.getLatestStmtId(),
                                                  status,
                                                  "a non-retryable snapshot error");
            iassert(status);
        }

        // The error is retryable, so update transaction state before retrying.
        txnRouter.onSnapshotError(opCtx, status);
    } else {
        invariant(ErrorCodes::isA<ErrorCategory::NeedRetargettingError>(status) ||
                  status.code() == ErrorCodes::StaleDbVersion ||
                  status.code() == ErrorCodes::ShardCannotRefreshDueToLocksHeld);

        if (!txnRouter.canContinueOnStaleShardOrDbError(_parc->_commandName, status)) {
            addContextForTransactionAbortingError(txnRouter.txnIdToString(),
                                                  txnRouter.getLatestStmtId(),
                                                  status,
                                                  "an error from cluster data placement change");
            iassert(status);
        }

        // The error is retryable, so update transaction state before retrying.
        txnRouter.onStaleShardOrDbError(opCtx, _parc->_commandName, status);
    }

    abortGuard.dismiss();
}

void ParseAndRunCommand::RunAndRetry::_onNeedRetargetting(Status& status) {
    invariant(ErrorCodes::isA<ErrorCategory::NeedRetargettingError>(status));

    auto staleInfo = status.extraInfo<StaleConfigInfo>();
    if (!staleInfo)
        iassert(status);

    auto opCtx = _parc->_rec->getOpCtx();
    const auto staleNs = staleInfo->getNss();
    const auto& originalNs = _parc->_invocation->ns();
    auto catalogCache = Grid::get(opCtx)->catalogCache();
    catalogCache->onStaleCollectionVersion(staleNs, staleInfo->getVersionWanted());

    if ((staleNs.isTimeseriesBucketsCollection() || originalNs.isTimeseriesBucketsCollection()) &&
        staleNs != originalNs) {
        // A timeseries might've been created, so we need to invalidate the original namespace
        // version.
        Grid::get(opCtx)->catalogCache()->onStaleCollectionVersion(originalNs, boost::none);
    }

    _checkRetryForTransaction(status);
}

void ParseAndRunCommand::RunAndRetry::_onStaleDbVersion(Status& status) {
    invariant(status.code() == ErrorCodes::StaleDbVersion);
    auto opCtx = _parc->_rec->getOpCtx();

    // Mark database entry in cache as stale.
    auto extraInfo = status.extraInfo<StaleDbRoutingVersion>();
    invariant(extraInfo);
    Grid::get(opCtx)->catalogCache()->onStaleDatabaseVersion(extraInfo->getDb(),
                                                             extraInfo->getVersionWanted());

    _checkRetryForTransaction(status);
}

void ParseAndRunCommand::RunAndRetry::_onSnapshotError(Status& status) {
    // Simple retry on any type of snapshot error.
    invariant(ErrorCodes::isA<ErrorCategory::SnapshotError>(status));

    _checkRetryForTransaction(status);

    auto opCtx = _parc->_rec->getOpCtx();
    if (auto txnRouter = TransactionRouter::get(opCtx);
        !txnRouter && !repl::ReadConcernArgs::get(opCtx).wasAtClusterTimeSelected()) {
        // Non-transaction snapshot read. The client sent readConcern: {level: "snapshot",
        // atClusterTime: T}, where T is older than minSnapshotHistoryWindowInSeconds, retrying
        // won't succeed.
        iassert(status);
    }
}

void ParseAndRunCommand::RunAndRetry::_onShardCannotRefreshDueToLocksHeldError(Status& status) {
    invariant(status.code() == ErrorCodes::ShardCannotRefreshDueToLocksHeld);

    _checkRetryForTransaction(status);
}

void ParseAndRunCommand::RunAndRetry::_onCannotImplicitlyCreateCollection(Status& status) {
    invariant(status.code() == ErrorCodes::CannotImplicitlyCreateCollection);

    auto opCtx = _parc->_rec->getOpCtx();

    auto extraInfo = status.extraInfo<CannotImplicitlyCreateCollectionInfo>();
    invariant(extraInfo);

    cluster::createCollectionWithRouterLoop(opCtx, extraInfo->getNss());
}

void ParseAndRunCommand::RunAndRetry::run() {
    do {
        try {
            // Try gMaxNumStaleVersionRetries times. On the last try, exceptions are
            // rethrown.
            _tries++;

            _setup();
            _run();
            return;
        } catch (const DBException& ex) {
            auto status = ex.toStatus();

            if (status.isA<ErrorCategory::NeedRetargettingError>()) {
                _onNeedRetargetting(status);
            } else if (status == ErrorCodes::StaleDbVersion) {
                _onStaleDbVersion(status);
            } else if (status.isA<ErrorCategory::SnapshotError>()) {
                _onSnapshotError(status);
            } else if (status == ErrorCodes::ShardCannotRefreshDueToLocksHeld) {
                _onShardCannotRefreshDueToLocksHeldError(status);
            } else if (status == ErrorCodes::CannotImplicitlyCreateCollection) {
                _onCannotImplicitlyCreateCollection(status);
            } else if (status == ErrorCodes::TransactionParticipantFailedUnyield) {
                auto originalErrorInfo =
                    status.extraInfo<TransactionParticipantFailedUnyieldInfo>();
                if (!originalErrorInfo)
                    iassert(status);

                // Throw original error
                auto originalStatus = originalErrorInfo->getOriginalError();
                iassert(originalStatus);
            } else {
                throw;
            }

            if (!_canRetry()) {
                throw;
            }
        }
    } while (true);
}

void ParseAndRunCommand::RunInvocation::run() {
    iassert(_setup());
    RunAndRetry runner(_parc);
    runner.run();
}

void ParseAndRunCommand::run() {
    try {
        _parseCommand();
        RunInvocation runner(this);
        runner.run();
    } catch (const DBException& ex) {
        auto status = ex.toStatus();

        _updateStatsAndApplyErrorLabels(status);

        if (status == ErrorCodes::SkipCommandExecution) {
            // We've already skipped execution, so no other action is required.
            return;
        }

        throw;
    }
}

// Maintains the state required to execute client commands, and provides the interface to construct
// a type that runs the command against the database.
class ClientCommand final {
public:
    ClientCommand(ClientCommand&&) = delete;
    ClientCommand(const ClientCommand&) = delete;

    explicit ClientCommand(RequestExecutionContext* rec) : _rec(rec) {}

    DbResponse run();

private:
    StringData _getDatabaseStringForLogging() const {
        return _rec->getRequest().readDatabaseForLogging();
    }

    void _parseMessage();

    void _execute();

    // Handler for exceptions thrown during parsing and executing the command.
    void _handleException(Status);

    // Extracts the command response from the replyBuilder.
    DbResponse _produceResponse();

    RequestExecutionContext* _rec;
    BSONObjBuilder _errorBuilder;

    bool _propagateException = false;
};

void ClientCommand::_parseMessage() try {
    const auto& msg = _rec->getMessage();
    _rec->setReplyBuilder(rpc::makeReplyBuilder(rpc::protocolForMessage(msg)));
    auto opMsgReq = rpc::opMsgRequestFromAnyProtocol(msg, _rec->getOpCtx()->getClient());

    if (msg.operation() == dbQuery) {
        checkAllowedOpQueryCommand(*(_rec->getOpCtx()->getClient()), opMsgReq.getCommandName());
    }
    _rec->setRequest(opMsgReq);
} catch (const DBException& ex) {
    // If this error needs to fail the connection, propagate it out.
    if (ErrorCodes::isConnectionFatalMessageParseError(ex.code()))
        _propagateException = true;

    LOGV2_DEBUG(22769, 1, "Exception thrown while parsing command", "error"_attr = redact(ex));
    throw;
}

void ClientCommand::_execute() {
    LOGV2_DEBUG(22770,
                3,
                "Command begin",
                "db"_attr = _getDatabaseStringForLogging(),
                "headerId"_attr = _rec->getMessage().header().getId());

    try {
        ParseAndRunCommand runner(_rec, &_errorBuilder);
        runner.run();

        LOGV2_DEBUG(22771,
                    3,
                    "Command end",
                    "db"_attr = _getDatabaseStringForLogging(),
                    "headerId"_attr = _rec->getMessage().header().getId());
    } catch (const DBException& ex) {
        auto status = ex.toStatus();

        LOGV2_DEBUG(22772,
                    1,
                    "Exception thrown while processing command",
                    "db"_attr = _getDatabaseStringForLogging(),
                    "headerId"_attr = _rec->getMessage().header().getId(),
                    "error"_attr = redact(status));

        // Record the exception in CurOp.
        CurOp::get(_rec->getOpCtx())->debug().errInfo = std::move(status);

        throw;
    };
}

void ClientCommand::_handleException(Status status) {
    if (status == ErrorCodes::CloseConnectionForShutdownCommand || _propagateException) {
        iassert(status);
    }

    auto opCtx = _rec->getOpCtx();
    auto reply = _rec->getReplyBuilder();

    // Salvage the value of the 'writeConcernError' field, if already set in the reply.
    // We will re-add this value later to the reply we will build from scratch.
    BSONObjBuilder wceBuilder;
    {
        auto bob = reply->getBodyBuilder().asTempObj();
        if (auto f = bob.getField("writeConcernError"_sd); !f.eoo()) {
            wceBuilder.append(f);
            wceBuilder.done();
        }
    }

    // Wipe whatever was already built in the reply so far and start a new reply.
    reply->reset();
    auto bob = reply->getBodyBuilder();

    CommandHelpers::appendCommandStatusNoThrow(bob, status);

    // Append original writeConcernError if it was set in the original reply.
    bob.appendElements(wceBuilder.asTempObj());

    appendRequiredFieldsToResponse(opCtx, &bob);

    // Only attach the topology version to the response if mongos is in quiesce mode. If mongos
    // is in quiesce mode, this shutdown error is due to mongos rather than a shard.
    if (ErrorCodes::isA<ErrorCategory::ShutdownError>(status)) {
        if (auto mongosTopCoord = MongosTopologyCoordinator::get(opCtx);
            mongosTopCoord && mongosTopCoord->inQuiesceMode()) {
            // Append the topology version to the response.
            const auto topologyVersion = mongosTopCoord->getTopologyVersion();
            BSONObjBuilder topologyVersionBuilder(_errorBuilder.subobjStart("topologyVersion"));
            topologyVersion.serialize(&topologyVersionBuilder);
        }
    }

    bob.appendElements(_errorBuilder.obj());
}

DbResponse ClientCommand::_produceResponse() {
    const auto& m = _rec->getMessage();
    auto reply = _rec->getReplyBuilder();

    if (OpMsg::isFlagSet(m, OpMsg::kMoreToCome)) {
        return {};  // Don't reply.
    }

    CommandHelpers::checkForInternalError(reply, isInternalClient(_rec->getOpCtx()));

    DbResponse dbResponse;
    if (OpMsg::isFlagSet(m, OpMsg::kExhaustSupported)) {
        auto responseObj = reply->getBodyBuilder().asTempObj();
        if (responseObj.getField("ok").trueValue()) {
            dbResponse.shouldRunAgainForExhaust = reply->shouldRunAgainForExhaust();
            dbResponse.nextInvocation = reply->getNextInvocation();
        }
    }
    if (auto doc = rpc::RewriteStateChangeErrors::rewrite(reply->getBodyBuilder().asTempObj(),
                                                          _rec->getOpCtx())) {
        reply->reset();
        reply->getBodyBuilder().appendElements(*doc);
    }
    dbResponse.response = reply->done();

    return dbResponse;
}

DbResponse ClientCommand::run() {
    try {
        _parseMessage();
        _execute();
    } catch (const DBException& ex) {
        _handleException(ex.toStatus());
    }
    return _produceResponse();
}

}  // namespace

DbResponse Strategy::clientCommand(RequestExecutionContext* rec) {
    ClientCommand runner(rec);
    return runner.run();
}

}  // namespace mongo
