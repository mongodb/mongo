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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding

#include "mongo/platform/basic.h"

#include "mongo/s/commands/strategy.h"

#include <fmt/format.h>

#include "mongo/base/data_cursor.h"
#include "mongo/base/init.h"
#include "mongo/base/status.h"
#include "mongo/bson/util/bson_extract.h"
#include "mongo/bson/util/builder.h"
#include "mongo/db/audit.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/commands.h"
#include "mongo/db/curop.h"
#include "mongo/db/error_labels.h"
#include "mongo/db/initialize_api_parameters.h"
#include "mongo/db/initialize_operation_session_info.h"
#include "mongo/db/lasterror.h"
#include "mongo/db/logical_session_id_helpers.h"
#include "mongo/db/logical_time_validator.h"
#include "mongo/db/matcher/extensions_callback_noop.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_time_tracker.h"
#include "mongo/db/ops/write_ops.h"
#include "mongo/db/query/find_common.h"
#include "mongo/db/query/getmore_request.h"
#include "mongo/db/query/query_request.h"
#include "mongo/db/read_write_concern_defaults.h"
#include "mongo/db/stats/api_version_metrics.h"
#include "mongo/db/stats/counters.h"
#include "mongo/db/transaction_validation.h"
#include "mongo/db/vector_clock.h"
#include "mongo/db/views/resolved_view.h"
#include "mongo/db/write_concern_options.h"
#include "mongo/logv2/log.h"
#include "mongo/rpc/factory.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/rpc/metadata/client_metadata.h"
#include "mongo/rpc/metadata/tracking_metadata.h"
#include "mongo/rpc/op_msg.h"
#include "mongo/rpc/op_msg_rpc_impls.h"
#include "mongo/s/catalog_cache.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/cluster_commands_helpers.h"
#include "mongo/s/commands/cluster_explain.h"
#include "mongo/s/grid.h"
#include "mongo/s/mongos_topology_coordinator.h"
#include "mongo/s/query/cluster_cursor_manager.h"
#include "mongo/s/query/cluster_find.h"
#include "mongo/s/session_catalog_router.h"
#include "mongo/s/shard_invalidated_for_targeting_exception.h"
#include "mongo/s/stale_exception.h"
#include "mongo/s/transaction_router.h"
#include "mongo/transport/hello_metrics.h"
#include "mongo/transport/session.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/scopeguard.h"
#include "mongo/util/str.h"
#include "mongo/util/timer.h"

using namespace fmt::literals;

namespace mongo {
namespace {

const auto kOperationTime = "operationTime"_sd;

/**
 * Invoking `shouldGossipLogicalTime()` is expected to always return "true" during normal execution.
 * SERVER-48013 uses this property to avoid the cost of calling this function during normal
 * execution. However, it might be desired to do the validation for test purposes (e.g.,
 * unit-tests). This fail-point allows going through a code path that does the check and quick
 * returns from `appendRequiredFieldsToResponse()` if `shouldGossipLogicalTime()` returns "false".
 * TODO SERVER-48142 should remove the following fail-point.
 */
MONGO_FAIL_POINT_DEFINE(allowSkippingAppendRequiredFieldsToResponse);

/**
 * Append required fields to command response.
 */
void appendRequiredFieldsToResponse(OperationContext* opCtx, BSONObjBuilder* responseBuilder) {
    // TODO SERVER-48142 should remove the following block.
    if (MONGO_unlikely(allowSkippingAppendRequiredFieldsToResponse.shouldFail())) {
        auto validator = LogicalTimeValidator::get(opCtx);
        if (!validator->shouldGossipLogicalTime()) {
            LOGV2_DEBUG(4801301, 3, "Skipped gossiping logical time");
            return;
        }
    }

    // The appended operationTime must always be <= the appended $clusterTime, so in case we need to
    // use $clusterTime as the operationTime below, take a $clusterTime value which is guaranteed to
    // be <= the value output by gossipOut().
    const auto currentTime = VectorClock::get(opCtx)->getTime();
    const auto clusterTime = currentTime.clusterTime();

    bool clusterTimeWasOutput = VectorClock::get(opCtx)->gossipOut(opCtx, responseBuilder);

    // Ensure that either both operationTime and $clusterTime are output, or neither.
    if (clusterTimeWasOutput) {
        auto operationTime = OperationTimeTracker::get(opCtx)->getMaxOperationTime();
        if (operationTime != LogicalTime::kUninitialized) {
            LOGV2_DEBUG(22764,
                        5,
                        "Appending operationTime: {operationTime}",
                        "Appending operationTime",
                        "operationTime"_attr = operationTime.asTimestamp());
            operationTime.appendAsOperationTime(responseBuilder);
        } else if (clusterTime != LogicalTime::kUninitialized) {
            // If we don't know the actual operation time, use the cluster time instead. This is
            // safe but not optimal because we can always return a later operation time than
            // actual.
            LOGV2_DEBUG(22765,
                        5,
                        "Appending clusterTime as operationTime {clusterTime}",
                        "Appending clusterTime as operationTime",
                        "clusterTime"_attr = clusterTime.asTimestamp());
            clusterTime.appendAsOperationTime(responseBuilder);
        }
    }
}

/**
 * Invokes the given command and aborts the transaction on any non-retryable errors.
 */
Future<void> invokeInTransactionRouter(std::shared_ptr<RequestExecutionContext> rec,
                                       std::shared_ptr<CommandInvocation> invocation) {
    auto opCtx = rec->getOpCtx();
    auto txnRouter = TransactionRouter::get(opCtx);
    invariant(txnRouter);

    // No-op if the transaction is not running with snapshot read concern.
    txnRouter.setDefaultAtClusterTime(opCtx);

    return CommandHelpers::runCommandInvocationAsync(rec, std::move(invocation))
        .tapError([rec = std::move(rec)](Status status) {
            if (auto code = status.code(); ErrorCodes::isSnapshotError(code) ||
                ErrorCodes::isNeedRetargettingError(code) ||
                code == ErrorCodes::ShardInvalidatedForTargeting ||
                code == ErrorCodes::StaleDbVersion) {
                // Don't abort on possibly retryable errors.
                return;
            }

            auto opCtx = rec->getOpCtx();
            TransactionRouter::get(opCtx).implicitlyAbortTransaction(opCtx, status);
        });
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

// Factory class to construct a future-chain that executes the invocation against the database.
class ExecCommandClient final : public std::enable_shared_from_this<ExecCommandClient> {
public:
    ExecCommandClient(ExecCommandClient&&) = delete;
    ExecCommandClient(const ExecCommandClient&) = delete;

    ExecCommandClient(std::shared_ptr<RequestExecutionContext> rec,
                      std::shared_ptr<CommandInvocation> invocation)
        : _rec(std::move(rec)), _invocation(std::move(invocation)) {}

    Future<void> run();

private:
    // Prepare the environment for running the invocation (e.g., checking authorization).
    Status _prologue();

    // Returns a future that runs the command invocation.
    Future<void> _run();

    // Any logic that must be done post command execution, unless an exception is thrown.
    void _epilogue();

    // Runs at the end of the future-chain returned by `run()` unless an exception, other than
    // `ErrorCodes::SkipCommandExecution`, is thrown earlier.
    void _onCompletion();

    const std::shared_ptr<RequestExecutionContext> _rec;
    const std::shared_ptr<CommandInvocation> _invocation;
};

Status ExecCommandClient::_prologue() {
    auto opCtx = _rec->getOpCtx();
    auto result = _rec->getReplyBuilder();
    const auto& request = _rec->getRequest();
    const Command* c = _invocation->definition();

    const auto dbname = request.getDatabase();
    uassert(ErrorCodes::IllegalOperation,
            "Can't use 'local' database through mongos",
            dbname != NamespaceString::kLocalDb);
    uassert(ErrorCodes::InvalidNamespace,
            "Invalid database name: '{}'"_format(dbname),
            NamespaceString::validDBName(dbname, NamespaceString::DollarInDbNameBehavior::Allow));

    StringMap<int> topLevelFields;
    for (auto&& element : request.body) {
        StringData fieldName = element.fieldNameStringData();
        if (fieldName == "help" && element.type() == Bool && element.Bool()) {
            auto body = result->getBodyBuilder();
            body.append("help", "help for: {} {}"_format(c->getName(), c->help()));
            CommandHelpers::appendSimpleCommandStatus(body, true, "");
            return {ErrorCodes::SkipCommandExecution, "Already served help command"};
        }

        uassert(ErrorCodes::FailedToParse,
                "Parsed command object contains duplicate top level key: {}"_format(fieldName),
                topLevelFields[fieldName]++ == 0);
    }

    try {
        _invocation->checkAuthorization(opCtx, request);
    } catch (const DBException& e) {
        auto body = result->getBodyBuilder();
        CommandHelpers::appendCommandStatusNoThrow(body, e.toStatus());
        return {ErrorCodes::SkipCommandExecution, "Failed to check authorization"};
    }

    // attach tracking
    rpc::TrackingMetadata trackingMetadata;
    trackingMetadata.initWithOperName(c->getName());
    rpc::TrackingMetadata::get(opCtx) = trackingMetadata;

    // Extract and process metadata from the command request body.
    ReadPreferenceSetting::get(opCtx) =
        uassertStatusOK(ReadPreferenceSetting::fromContainingBSON(request.body));
    VectorClock::get(opCtx)->gossipIn(opCtx, request.body, !c->requiresAuth());

    return Status::OK();
}

Future<void> ExecCommandClient::_run() {
    OperationContext* opCtx = _rec->getOpCtx();
    if (auto txnRouter = TransactionRouter::get(opCtx); txnRouter) {
        return invokeInTransactionRouter(_rec, _invocation);
    } else {
        return CommandHelpers::runCommandInvocationAsync(_rec, _invocation);
    }
}

void ExecCommandClient::_epilogue() {
    auto opCtx = _rec->getOpCtx();
    auto result = _rec->getReplyBuilder();
    if (_invocation->supportsWriteConcern()) {
        failCommand.executeIf(
            [&](const BSONObj& data) {
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
                           data, _invocation.get(), opCtx->getClient()) &&
                    data.hasField("writeConcernError");
            });
    }

    auto body = result->getBodyBuilder();

    if (bool ok = CommandHelpers::extractOrAppendOk(body); !ok) {
        const Command* c = _invocation->definition();
        c->incrementCommandsFailed();

        if (auto txnRouter = TransactionRouter::get(opCtx)) {
            txnRouter.implicitlyAbortTransaction(opCtx,
                                                 getStatusFromCommandResult(body.asTempObj()));
        }
    }
}

void ExecCommandClient::_onCompletion() {
    auto opCtx = _rec->getOpCtx();
    auto body = _rec->getReplyBuilder()->getBodyBuilder();
    appendRequiredFieldsToResponse(opCtx, &body);

    // TODO SERVER-49109 enable the following code-path.
    /*
    auto seCtx = transport::ServiceExecutorContext::get(opCtx->getClient());
    if (!seCtx) {
        // We were run by a background worker.
        return;
    }

    if (!_invocation->isSafeForBorrowedThreads()) {
        // If the last command wasn't safe for a borrowed thread, then let's move off of it.
        seCtx->setThreadingModel(transport::ServiceExecutor::ThreadingModel::kDedicated);
    }
    */
}

Future<void> ExecCommandClient::run() {
    auto pf = makePromiseFuture<void>();
    auto future = std::move(pf.future)
                      .then([this, anchor = shared_from_this()] { return _prologue(); })
                      .then([this, anchor = shared_from_this()] { return _run(); })
                      .then([this, anchor = shared_from_this()] { _epilogue(); })
                      .onCompletion([this, anchor = shared_from_this()](Status status) {
                          if (!status.isOK() && status.code() != ErrorCodes::SkipCommandExecution)
                              return status;  // Execution was interrupted due to an error.

                          _onCompletion();
                          return Status::OK();
                      });
    pf.promise.emplaceValue();
    return future;
}

MONGO_FAIL_POINT_DEFINE(doNotRefreshShardsOnRetargettingError);

/**
 * Produces a future-chain that parses the command, runs the parsed command, and captures the result
 * in replyBuilder.
 */
class ParseAndRunCommand final : public std::enable_shared_from_this<ParseAndRunCommand> {
public:
    ParseAndRunCommand(const ParseAndRunCommand&) = delete;
    ParseAndRunCommand(ParseAndRunCommand&&) = delete;

    ParseAndRunCommand(std::shared_ptr<RequestExecutionContext> rec,
                       std::shared_ptr<BSONObjBuilder> errorBuilder)
        : _rec(std::move(rec)),
          _errorBuilder(std::move(errorBuilder)),
          _opType(_rec->getMessage().operation()),
          _commandName(_rec->getRequest().getCommandName()) {}

    Future<void> run();

private:
    class RunInvocation;
    class RunAndRetry;

    // Prepares the environment for running the command (e.g., parsing the command to produce the
    // invocation and extracting read/write concerns).
    Status _prologue();

    // Returns a future-chain that runs the parse invocation.
    Future<void> _runInvocation();

    const std::shared_ptr<RequestExecutionContext> _rec;
    const std::shared_ptr<BSONObjBuilder> _errorBuilder;
    const NetworkOp _opType;
    const StringData _commandName;

    std::shared_ptr<CommandInvocation> _invocation;
    boost::optional<std::string> _ns;
    boost::optional<OperationSessionInfoFromClient> _osi;
    boost::optional<WriteConcernOptions> _wc;
    boost::optional<bool> _isHello;
};

/*
 * Produces a future-chain to run the invocation and capture the result in replyBuilder.
 */
class ParseAndRunCommand::RunInvocation final
    : public std::enable_shared_from_this<ParseAndRunCommand::RunInvocation> {
public:
    RunInvocation(RunInvocation&&) = delete;
    RunInvocation(const RunInvocation&) = delete;

    explicit RunInvocation(std::shared_ptr<ParseAndRunCommand> parc) : _parc(std::move(parc)) {}

    ~RunInvocation() {
        if (!_shouldAffectCommandCounter)
            return;
        auto opCtx = _parc->_rec->getOpCtx();
        Grid::get(opCtx)->catalogCache()->checkAndRecordOperationBlockedByRefresh(
            opCtx, mongo::LogicalOp::opCommand);
    }

    Future<void> run();

private:
    Status _setup();

    // Returns a future-chain that runs the invocation and retries if necessary.
    Future<void> _runAndRetry();

    // Logs and updates statistics if an error occurs during `_setup()` or `_runAndRetry()`.
    void _tapOnError(const Status& status);

    const std::shared_ptr<ParseAndRunCommand> _parc;

    boost::optional<RouterOperationContextSession> _routerSession;
    bool _shouldAffectCommandCounter = false;
};

/*
 * Produces a future-chain that runs the invocation and retries if necessary.
 */
class ParseAndRunCommand::RunAndRetry final
    : public std::enable_shared_from_this<ParseAndRunCommand::RunAndRetry> {
public:
    RunAndRetry(RunAndRetry&&) = delete;
    RunAndRetry(const RunAndRetry&) = delete;

    explicit RunAndRetry(std::shared_ptr<ParseAndRunCommand> parc) : _parc(std::move(parc)) {}

    Future<void> run();

private:
    bool _canRetry() const {
        return _tries < kMaxNumStaleVersionRetries;
    }

    // Sets up the environment for running the invocation, and clears the state from the last try.
    void _setup();

    Future<void> _run();

    // Exception handler for error codes that may trigger a retry. All methods will throw `status`
    // unless an attempt to retry is possible.
    void _checkRetryForTransaction(Status& status);
    void _onShardInvalidatedForTargeting(Status& status);
    void _onNeedRetargetting(Status& status);
    void _onStaleDbVersion(Status& status);
    void _onSnapshotError(Status& status);

    const std::shared_ptr<ParseAndRunCommand> _parc;

    int _tries = 0;
};

Status ParseAndRunCommand::_prologue() {
    auto opCtx = _rec->getOpCtx();
    const auto& m = _rec->getMessage();
    const auto& request = _rec->getRequest();
    auto replyBuilder = _rec->getReplyBuilder();

    auto const command = CommandHelpers::findCommand(_commandName);
    if (!command) {
        const std::string errorMsg = "no such cmd: {}"_format(_commandName);
        auto builder = replyBuilder->getBodyBuilder();
        CommandHelpers::appendCommandStatusNoThrow(builder,
                                                   {ErrorCodes::CommandNotFound, errorMsg});
        globalCommandRegistry()->incrementUnknownCommands();
        appendRequiredFieldsToResponse(opCtx, &builder);
        return {ErrorCodes::SkipCommandExecution, errorMsg};
    }

    _rec->setCommand(command);

    _isHello.emplace(command->getName() == "hello"_sd || command->getName() == "isMaster"_sd);

    opCtx->setExhaust(OpMsg::isFlagSet(m, OpMsg::kExhaustSupported));
    const auto session = opCtx->getClient()->session();
    if (session) {
        if (!opCtx->isExhaust() || !_isHello.get()) {
            InExhaustHello::get(session.get())->setInExhaust(false, _commandName);
        }
    }

    CommandHelpers::uassertShouldAttemptParse(opCtx, command, request);

    // Parse the 'maxTimeMS' command option, and use it to set a deadline for the operation on
    // the OperationContext. Be sure to do this as soon as possible so that further processing by
    // subsequent code has the deadline available. The 'maxTimeMS' option unfortunately has a
    // different meaning for a getMore command, where it is used to communicate the maximum time to
    // wait for new inserts on tailable cursors, not as a deadline for the operation.
    // TODO SERVER-34277 Remove the special handling for maxTimeMS for getMores. This will
    // require introducing a new 'max await time' parameter for getMore, and eventually banning
    // maxTimeMS altogether on a getMore command.
    uassert(ErrorCodes::InvalidOptions,
            "no such command option $maxTimeMs; use maxTimeMS instead",
            request.body[QueryRequest::queryOptionMaxTimeMS].eoo());
    const int maxTimeMS = uassertStatusOK(
        QueryRequest::parseMaxTimeMS(request.body[QueryRequest::cmdOptionMaxTimeMS]));
    if (maxTimeMS > 0 && command->getLogicalOp() != LogicalOp::opGetMore) {
        opCtx->setDeadlineAfterNowBy(Milliseconds{maxTimeMS}, ErrorCodes::MaxTimeMSExpired);
    }
    opCtx->checkForInterrupt();  // May trigger maxTimeAlwaysTimeOut fail point.

    // If the command includes a 'comment' field, set it on the current OpCtx.
    if (auto commentField = request.body["comment"]) {
        opCtx->setComment(commentField.wrap());
    }

    _invocation = command->parse(opCtx, request);
    CommandInvocation::set(opCtx, _invocation);

    // Set the logical optype, command object and namespace as soon as we identify the command. If
    // the command does not define a fully-qualified namespace, set CurOp to the generic command
    // namespace db.$cmd.
    _ns.emplace(_invocation->ns().toString());
    auto nss =
        (request.getDatabase() == *_ns ? NamespaceString(*_ns, "$cmd") : NamespaceString(*_ns));

    // Fill out all currentOp details.
    CurOp::get(opCtx)->setGenericOpRequestDetails(opCtx, nss, command, request.body, _opType);

    _osi.emplace(initializeOperationSessionInfo(opCtx,
                                                request.body,
                                                command->requiresAuth(),
                                                command->attachLogicalSessionsToOpCtx(),
                                                true));

    // TODO SERVER-28756: Change allowTransactionsOnConfigDatabase to true once we fix the bug
    // where the mongos custom write path incorrectly drops the client's txnNumber.
    auto allowTransactionsOnConfigDatabase = false;
    validateSessionOptions(*_osi, command->getName(), nss, allowTransactionsOnConfigDatabase);

    _wc.emplace(uassertStatusOK(WriteConcernOptions::extractWCFromCommand(request.body)));

    Client* client = opCtx->getClient();
    auto const apiParamsFromClient = initializeAPIParameters(request.body, command);

    auto& readConcernArgs = repl::ReadConcernArgs::get(opCtx);
    Status readConcernParseStatus = Status::OK();
    {
        // We must obtain the client lock to set APIParameters and ReadConcernArgs on the operation
        // context, as it may be concurrently read by CurrentOp.
        stdx::lock_guard<Client> lk(*client);
        APIParameters::get(opCtx) = APIParameters::fromClient(apiParamsFromClient);
        readConcernParseStatus = readConcernArgs.initialize(request.body);
    }

    if (!readConcernParseStatus.isOK()) {
        auto builder = replyBuilder->getBodyBuilder();
        CommandHelpers::appendCommandStatusNoThrow(builder, readConcernParseStatus);
        return {ErrorCodes::SkipCommandExecution, "Failed to parse read concern"};
    }

    return Status::OK();
}

Status ParseAndRunCommand::RunInvocation::_setup() {
    auto invocation = _parc->_invocation;
    auto opCtx = _parc->_rec->getOpCtx();
    auto command = _parc->_rec->getCommand();
    const auto& request = _parc->_rec->getRequest();
    auto replyBuilder = _parc->_rec->getReplyBuilder();

    auto appendStatusToReplyAndSkipCommandExecution = [replyBuilder](Status status) -> Status {
        auto responseBuilder = replyBuilder->getBodyBuilder();
        CommandHelpers::appendCommandStatusNoThrow(responseBuilder, status);
        return Status(ErrorCodes::SkipCommandExecution, status.reason());
    };

    if (_parc->_isHello.get()) {
        // Preload generic ClientMetadata ahead of our first hello request. After the first
        // request, metaElement should always be empty.
        auto metaElem = request.body[kMetadataDocumentName];
        ClientMetadata::setFromMetadata(opCtx->getClient(), metaElem);
    }

    auto& apiParams = APIParameters::get(opCtx);
    auto& apiVersionMetrics = APIVersionMetrics::get(opCtx->getServiceContext());
    if (auto clientMetadata = ClientMetadata::get(opCtx->getClient())) {
        auto appName = clientMetadata->getApplicationName().toString();
        apiVersionMetrics.update(appName, apiParams);
    }

    rpc::readRequestMetadata(opCtx, request.body, command->requiresAuth());

    CommandHelpers::evaluateFailCommandFailPoint(opCtx, invocation.get());
    bool startTransaction = false;
    if (_parc->_osi->getAutocommit()) {
        _routerSession.emplace(opCtx);

        auto txnRouter = TransactionRouter::get(opCtx);
        invariant(txnRouter);

        auto txnNumber = opCtx->getTxnNumber();
        invariant(txnNumber);

        auto transactionAction = ([&] {
            auto startTxnSetting = _parc->_osi->getStartTransaction();
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
    if (!supportsWriteConcern && request.body.hasField(WriteConcernOptions::kWriteConcernField)) {
        // This command doesn't do writes so it should not be passed a writeConcern.
        const auto errorMsg = "Command does not support writeConcern";
        return appendStatusToReplyAndSkipCommandExecution({ErrorCodes::InvalidOptions, errorMsg});
    }

    bool clientSuppliedWriteConcern = !_parc->_wc->usedDefault;
    bool customDefaultWriteConcernWasApplied = false;

    if (supportsWriteConcern && !clientSuppliedWriteConcern &&
        (!TransactionRouter::get(opCtx) || isTransactionCommand(_parc->_commandName))) {
        // This command supports WC, but wasn't given one - so apply the default, if there is one.
        if (const auto wcDefault = ReadWriteConcernDefaults::get(opCtx->getServiceContext())
                                       .getDefaultWriteConcern(opCtx)) {
            _parc->_wc = *wcDefault;
            customDefaultWriteConcernWasApplied = true;
            LOGV2_DEBUG(22766,
                        2,
                        "Applying default writeConcern on {command} of {writeConcern}",
                        "Applying default writeConcern on command",
                        "command"_attr = request.getCommandName(),
                        "writeConcern"_attr = *wcDefault);
        }
    }

    if (TransactionRouter::get(opCtx)) {
        validateWriteConcernForTransaction(*_parc->_wc, _parc->_commandName);
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

    auto readConcernSupport = invocation->supportsReadConcern(readConcernArgs.getLevel());
    if (readConcernSupport.defaultReadConcernPermit.isOK() &&
        (startTransaction || !TransactionRouter::get(opCtx))) {
        if (readConcernArgs.isEmpty()) {
            const auto rcDefault = ReadWriteConcernDefaults::get(opCtx->getServiceContext())
                                       .getDefaultReadConcern(opCtx);
            if (rcDefault) {
                {
                    // We must obtain the client lock to set ReadConcernArgs, because it's an
                    // in-place reference to the object on the operation context, which may be
                    // concurrently used elsewhere (eg. read by currentOp).
                    stdx::lock_guard<Client> lk(*opCtx->getClient());
                    readConcernArgs = std::move(*rcDefault);
                }
                customDefaultReadConcernWasApplied = true;
                LOGV2_DEBUG(22767,
                            2,
                            "Applying default readConcern on {command} of {readConcern}",
                            "Applying default readConcern on command",
                            "command"_attr = invocation->definition()->getName(),
                            "readConcern"_attr = *rcDefault);
                // Update the readConcernSupport, since the default RC was applied.
                readConcernSupport = invocation->supportsReadConcern(readConcernArgs.getLevel());
            }
        }
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
    if (!TransactionRouter::get(opCtx) && readConcernArgs.hasLevel()) {
        if (!readConcernSupport.readConcernSupport.isOK()) {
            const std::string errorMsg = "Command {} does not support {}"_format(
                invocation->definition()->getName(), readConcernArgs.toString());
            return appendStatusToReplyAndSkipCommandExecution(
                readConcernSupport.readConcernSupport.withContext(errorMsg));
        }
    }

    // Remember whether or not this operation is starting a transaction, in case something later in
    // the execution needs to adjust its behavior based on this.
    opCtx->setIsStartingMultiDocumentTransaction(startTransaction);

    // Once API params and txn state are set on opCtx, enforce the "requireApiVersion" setting.
    enforceRequireAPIVersion(opCtx, command);

    command->incrementCommandsExecuted();

    if (command->shouldAffectCommandCounter()) {
        globalOpCounters.gotCommand();
        _shouldAffectCommandCounter = true;
    }

    return Status::OK();
}

void ParseAndRunCommand::RunAndRetry::_setup() {
    auto opCtx = _parc->_rec->getOpCtx();
    const auto command = _parc->_rec->getCommand();
    const auto& request = _parc->_rec->getRequest();
    auto& readConcernArgs = repl::ReadConcernArgs::get(opCtx);

    if (_tries > 1) {
        // Re-parse before retrying in case the process of run()-ning the invocation could affect
        // the parsed result.
        _parc->_invocation = command->parse(opCtx, request);
        invariant(_parc->_invocation->ns().toString() == _parc->_ns,
                  "unexpected change of namespace when retrying");
    }

    // On each try, select the latest known clusterTime as the atClusterTime for snapshot reads
    // outside of transactions.
    if (readConcernArgs.getLevel() == repl::ReadConcernLevel::kSnapshotReadConcern &&
        !TransactionRouter::get(opCtx) &&
        (!readConcernArgs.getArgsAtClusterTime() || readConcernArgs.wasAtClusterTimeSelected())) {
        auto atClusterTime = [](OperationContext* opCtx, ReadConcernArgs& readConcernArgs) {
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

Future<void> ParseAndRunCommand::RunAndRetry::_run() {
    auto ecc = std::make_shared<ExecCommandClient>(_parc->_rec, _parc->_invocation);
    return ecc->run().then([rec = _parc->_rec] {
        auto opCtx = rec->getOpCtx();
        auto responseBuilder = rec->getReplyBuilder()->getBodyBuilder();
        if (auto txnRouter = TransactionRouter::get(opCtx)) {
            txnRouter.appendRecoveryToken(&responseBuilder);
        }
    });
}

void ParseAndRunCommand::RunAndRetry::_checkRetryForTransaction(Status& status) {
    // Retry logic specific to transactions. Throws and aborts the transaction if the error cannot
    // be retried on.
    auto opCtx = _parc->_rec->getOpCtx();
    auto txnRouter = TransactionRouter::get(opCtx);
    if (!txnRouter)
        return;

    auto abortGuard = makeGuard([&] { txnRouter.implicitlyAbortTransaction(opCtx, status); });

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
                  status.code() == ErrorCodes::ShardInvalidatedForTargeting ||
                  status.code() == ErrorCodes::StaleDbVersion);

        if (!txnRouter.canContinueOnStaleShardOrDbError(_parc->_commandName, status)) {
            if (status.code() == ErrorCodes::ShardInvalidatedForTargeting) {
                auto catalogCache = Grid::get(opCtx)->catalogCache();
                (void)catalogCache->getCollectionRoutingInfoWithRefresh(
                    opCtx, status.extraInfo<ShardInvalidatedForTargetingInfo>()->getNss());
            }

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

void ParseAndRunCommand::RunAndRetry::_onShardInvalidatedForTargeting(Status& status) {
    invariant(status.code() == ErrorCodes::ShardInvalidatedForTargeting);

    auto opCtx = _parc->_rec->getOpCtx();
    auto catalogCache = Grid::get(opCtx)->catalogCache();
    catalogCache->setOperationShouldBlockBehindCatalogCacheRefresh(opCtx, true);

    _checkRetryForTransaction(status);

    if (!_canRetry())
        iassert(status);
}

void ParseAndRunCommand::RunAndRetry::_onNeedRetargetting(Status& status) {
    invariant(ErrorCodes::isA<ErrorCategory::NeedRetargettingError>(status));

    auto staleInfo = status.extraInfo<StaleConfigInfo>();
    if (!staleInfo)
        iassert(status);

    auto opCtx = _parc->_rec->getOpCtx();
    const auto staleNs = staleInfo->getNss();
    auto catalogCache = Grid::get(opCtx)->catalogCache();
    catalogCache->invalidateShardOrEntireCollectionEntryForShardedCollection(
        staleNs, staleInfo->getVersionWanted(), staleInfo->getShardId());

    catalogCache->setOperationShouldBlockBehindCatalogCacheRefresh(opCtx, true);

    _checkRetryForTransaction(status);

    if (!_canRetry())
        iassert(status);
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

    if (!_canRetry())
        iassert(status);
}

void ParseAndRunCommand::RunAndRetry::_onSnapshotError(Status& status) {
    // Simple retry on any type of snapshot error.
    invariant(ErrorCodes::isA<ErrorCategory::SnapshotError>(status));

    _checkRetryForTransaction(status);

    auto opCtx = _parc->_rec->getOpCtx();
    if (auto txnRouter = TransactionRouter::get(opCtx);
        !txnRouter && !ReadConcernArgs::get(opCtx).wasAtClusterTimeSelected()) {
        // Non-transaction snapshot read. The client sent readConcern: {level: "snapshot",
        // atClusterTime: T}, where T is older than minSnapshotHistoryWindowInSeconds, retrying
        // won't succeed.
        iassert(status);
    }

    if (!_canRetry())
        iassert(status);
}

void ParseAndRunCommand::RunInvocation::_tapOnError(const Status& status) {
    auto opCtx = _parc->_rec->getOpCtx();
    const auto command = _parc->_rec->getCommand();

    command->incrementCommandsFailed();
    LastError::get(opCtx->getClient()).setLastError(status.code(), status.reason());
    // WriteConcern error (wcCode) is set to boost::none because:
    // 1. TransientTransaction error label handling for commitTransaction command in mongos is
    //    delegated to the shards. Mongos simply propagates the shard's response up to the client.
    // 2. For other commands in a transaction, they shouldn't get a writeConcern error so this
    //    setting doesn't apply.
    //
    // isInternalClient is set to true to suppress mongos from returning the RetryableWriteError
    // label.
    auto errorLabels = getErrorLabels(opCtx,
                                      *_parc->_osi,
                                      command->getName(),
                                      status.code(),
                                      boost::none,
                                      true /* isInternalClient */);
    _parc->_errorBuilder->appendElements(errorLabels);
}

Future<void> ParseAndRunCommand::RunInvocation::_runAndRetry() {
    auto instance = std::make_shared<RunAndRetry>(_parc);
    return instance->run();
}

Future<void> ParseAndRunCommand::_runInvocation() {
    auto ri = std::make_shared<RunInvocation>(shared_from_this());
    return ri->run();
}

Future<void> ParseAndRunCommand::RunAndRetry::run() {
    // Try kMaxNumStaleVersionRetries times. On the last try, exceptions are rethrown.
    _tries++;

    auto pf = makePromiseFuture<void>();
    auto future = std::move(pf.future)
                      .then([this, anchor = shared_from_this()] { _setup(); })
                      .then([this, anchor = shared_from_this()] {
                          return _run()
                              .onError<ErrorCodes::ShardInvalidatedForTargeting>(
                                  [this, anchor = shared_from_this()](Status status) {
                                      _onShardInvalidatedForTargeting(status);
                                      return run();  // Retry
                                  })
                              .onErrorCategory<ErrorCategory::NeedRetargettingError>(
                                  [this, anchor = shared_from_this()](Status status) {
                                      _onNeedRetargetting(status);
                                      return run();  // Retry
                                  })
                              .onError<ErrorCodes::StaleDbVersion>(
                                  [this, anchor = shared_from_this()](Status status) {
                                      _onStaleDbVersion(status);
                                      return run();  // Retry
                                  })
                              .onErrorCategory<ErrorCategory::SnapshotError>(
                                  [this, anchor = shared_from_this()](Status status) {
                                      _onSnapshotError(status);
                                      return run();  // Retry
                                  });
                      });
    pf.promise.emplaceValue();
    return future;
}

Future<void> ParseAndRunCommand::RunInvocation::run() {
    auto pf = makePromiseFuture<void>();
    auto future =
        std::move(pf.future)
            .then([this, anchor = shared_from_this()] { return _setup(); })
            .then([this, anchor = shared_from_this()] { return _runAndRetry(); })
            .tapError([this, anchor = shared_from_this()](Status status) { _tapOnError(status); });
    pf.promise.emplaceValue();
    return future;
}

Future<void> ParseAndRunCommand::run() {
    auto pf = makePromiseFuture<void>();
    auto future = std::move(pf.future)
                      .then([this, anchor = shared_from_this()] { return _prologue(); })
                      .then([this, anchor = shared_from_this()] { return _runInvocation(); })
                      .onError([this, anchor = shared_from_this()](Status status) {
                          if (status.code() == ErrorCodes::SkipCommandExecution)
                              // We've already skipped execution, so no other action is required.
                              return Status::OK();
                          return status;
                      });
    pf.promise.emplaceValue();
    return future;
}

/**
 * Executes the command for the given request, and appends the result to replyBuilder
 * and error labels, if any, to errorBuilder.
 */
Future<void> runCommand(std::shared_ptr<RequestExecutionContext> rec,
                        std::shared_ptr<BSONObjBuilder> errorBuilder) {
    auto instance = std::make_shared<ParseAndRunCommand>(std::move(rec), std::move(errorBuilder));
    return instance->run();
}

}  // namespace

DbResponse Strategy::queryOp(OperationContext* opCtx, const NamespaceString& nss, DbMessage* dbm) {
    globalOpCounters.gotQuery();

    ON_BLOCK_EXIT([opCtx] {
        Grid::get(opCtx)->catalogCache()->checkAndRecordOperationBlockedByRefresh(
            opCtx, mongo::LogicalOp::opQuery);
    });

    const QueryMessage q(*dbm);

    const auto upconvertedQuery = upconvertQueryEntry(q.query, nss, q.ntoreturn, q.ntoskip);

    // Set the upconverted query as the CurOp command object.
    CurOp::get(opCtx)->setGenericOpRequestDetails(
        opCtx, nss, nullptr, upconvertedQuery, dbm->msg().operation());

    Client* const client = opCtx->getClient();
    AuthorizationSession* const authSession = AuthorizationSession::get(client);

    // The legacy '$comment' operator gets converted to 'comment' by upconvertQueryEntry(). We
    // set the comment in 'opCtx' so that it can be passed on to the respective shards.
    if (auto commentField = upconvertedQuery["comment"]) {
        opCtx->setComment(commentField.wrap());
    }

    Status status = authSession->checkAuthForFind(nss, false);
    audit::logQueryAuthzCheck(client, nss, q.query, status.code());
    uassertStatusOK(status);

    LOGV2_DEBUG(22768,
                3,
                "Query: {namespace} {query} ntoreturn: {ntoreturn} options: {queryOptions}",
                "Query",
                "namespace"_attr = q.ns,
                "query"_attr = redact(q.query),
                "ntoreturn"_attr = q.ntoreturn,
                "queryOptions"_attr = q.queryOptions);

    if (q.queryOptions & QueryOption_Exhaust) {
        uasserted(18526,
                  str::stream() << "The 'exhaust' query option is invalid for mongos queries: "
                                << nss.ns() << " " << q.query.toString());
    }

    // Determine the default read preference mode based on the value of the slaveOk flag.
    const auto defaultReadPref = q.queryOptions & QueryOption_SecondaryOk
        ? ReadPreference::SecondaryPreferred
        : ReadPreference::PrimaryOnly;
    ReadPreferenceSetting::get(opCtx) =
        uassertStatusOK(ReadPreferenceSetting::fromContainingBSON(q.query, defaultReadPref));

    const boost::intrusive_ptr<ExpressionContext> expCtx;
    auto canonicalQuery = uassertStatusOK(
        CanonicalQuery::canonicalize(opCtx,
                                     q,
                                     expCtx,
                                     ExtensionsCallbackNoop(),
                                     MatchExpressionParser::kAllowAllSpecialFeatures));

    const QueryRequest& queryRequest = canonicalQuery->getQueryRequest();
    // Handle query option $maxTimeMS (not used with commands).
    if (queryRequest.getMaxTimeMS() > 0) {
        uassert(50749,
                "Illegal attempt to set operation deadline within DBDirectClient",
                !opCtx->getClient()->isInDirectClient());
        opCtx->setDeadlineAfterNowBy(Milliseconds{queryRequest.getMaxTimeMS()},
                                     ErrorCodes::MaxTimeMSExpired);
    }
    opCtx->checkForInterrupt();  // May trigger maxTimeAlwaysTimeOut fail point.

    // If the $explain flag was set, we must run the operation on the shards as an explain command
    // rather than a find command.
    if (queryRequest.isExplain()) {
        const BSONObj findCommand = queryRequest.asFindCommand();

        // We default to allPlansExecution verbosity.
        const auto verbosity = ExplainOptions::Verbosity::kExecAllPlans;

        BSONObjBuilder explainBuilder;
        Strategy::explainFind(opCtx,
                              findCommand,
                              queryRequest,
                              verbosity,
                              ReadPreferenceSetting::get(opCtx),
                              &explainBuilder);

        BSONObj explainObj = explainBuilder.done();
        return replyToQuery(explainObj);
    }

    // Do the work to generate the first batch of results. This blocks waiting to get responses from
    // the shard(s).
    std::vector<BSONObj> batch;

    // 0 means the cursor is exhausted. Otherwise we assume that a cursor with the returned id can
    // be retrieved via the ClusterCursorManager.
    CursorId cursorId;
    try {
        cursorId = ClusterFind::runQuery(
            opCtx, *canonicalQuery, ReadPreferenceSetting::get(opCtx), &batch);
    } catch (const ExceptionFor<ErrorCodes::CommandOnShardedViewNotSupportedOnMongod>&) {
        uasserted(40247, "OP_QUERY not supported on views");
    }

    // Fill out the response buffer.
    int numResults = 0;
    OpQueryReplyBuilder reply;
    for (auto&& obj : batch) {
        obj.appendSelfToBufBuilder(reply.bufBuilderForResults());
        numResults++;
    }

    return DbResponse{reply.toQueryReply(0,  // query result flags
                                         numResults,
                                         0,  // startingFrom
                                         cursorId)};
}

// Maintains the state required to execute client commands, and provides the interface to construct
// a future-chain that runs the command against the database.
class ClientCommand final : public std::enable_shared_from_this<ClientCommand> {
public:
    ClientCommand(ClientCommand&&) = delete;
    ClientCommand(const ClientCommand&) = delete;

    explicit ClientCommand(std::shared_ptr<RequestExecutionContext> rec)
        : _rec(std::move(rec)), _errorBuilder(std::make_shared<BSONObjBuilder>()) {}

    // Returns the future-chain that produces the response by parsing and executing the command.
    Future<DbResponse> run();

private:
    void _parse();

    Future<void> _execute();

    // Handler for exceptions thrown during parsing and executing the command.
    Future<void> _handleException(Status);

    // Extracts the command response from the replyBuilder.
    DbResponse _produceResponse();

    const std::shared_ptr<RequestExecutionContext> _rec;
    const std::shared_ptr<BSONObjBuilder> _errorBuilder;

    bool _propagateException = false;
};

void ClientCommand::_parse() try {
    const auto& msg = _rec->getMessage();
    _rec->setReplyBuilder(rpc::makeReplyBuilder(rpc::protocolForMessage(msg)));
    _rec->setRequest(rpc::opMsgRequestFromAnyProtocol(msg));
} catch (const DBException& ex) {
    // If this error needs to fail the connection, propagate it out.
    if (ErrorCodes::isConnectionFatalMessageParseError(ex.code()))
        _propagateException = true;

    LOGV2_DEBUG(22769,
                1,
                "Exception thrown while parsing command {error}",
                "Exception thrown while parsing command",
                "error"_attr = redact(ex));
    throw;
}

Future<void> ClientCommand::_execute() {
    LOGV2_DEBUG(22770,
                3,
                "Command begin db: {db} msg id: {headerId}",
                "Command begin",
                "db"_attr = _rec->getRequest().getDatabase().toString(),
                "headerId"_attr = _rec->getMessage().header().getId());

    return runCommand(_rec, _errorBuilder)
        .then([this, anchor = shared_from_this()] {
            LOGV2_DEBUG(22771,
                        3,
                        "Command end db: {db} msg id: {headerId}",
                        "Command end",
                        "db"_attr = _rec->getRequest().getDatabase().toString(),
                        "headerId"_attr = _rec->getMessage().header().getId());
        })
        .tapError([this, anchor = shared_from_this()](Status status) {
            LOGV2_DEBUG(
                22772,
                1,
                "Exception thrown while processing command on {db} msg id: {headerId} {error}",
                "Exception thrown while processing command",
                "db"_attr = _rec->getRequest().getDatabase().toString(),
                "headerId"_attr = _rec->getMessage().header().getId(),
                "error"_attr = redact(status));

            // Record the exception in CurOp.
            CurOp::get(_rec->getOpCtx())->debug().errInfo = std::move(status);
        });
}

Future<void> ClientCommand::_handleException(Status status) {
    if (_propagateException) {
        return status;
    }

    auto opCtx = _rec->getOpCtx();
    auto reply = _rec->getReplyBuilder();

    reply->reset();
    auto bob = reply->getBodyBuilder();
    CommandHelpers::appendCommandStatusNoThrow(bob, status);
    appendRequiredFieldsToResponse(opCtx, &bob);

    // Only attach the topology version to the response if mongos is in quiesce mode. If mongos is
    // in quiesce mode, this shutdown error is due to mongos rather than a shard.
    if (ErrorCodes::isA<ErrorCategory::ShutdownError>(status)) {
        if (auto mongosTopCoord = MongosTopologyCoordinator::get(opCtx);
            mongosTopCoord && mongosTopCoord->inQuiesceMode()) {
            // Append the topology version to the response.
            const auto topologyVersion = mongosTopCoord->getTopologyVersion();
            BSONObjBuilder topologyVersionBuilder(_errorBuilder->subobjStart("topologyVersion"));
            topologyVersion.serialize(&topologyVersionBuilder);
        }
    }

    bob.appendElements(_errorBuilder->obj());
    return Status::OK();
}

DbResponse ClientCommand::_produceResponse() {
    const auto& m = _rec->getMessage();
    auto reply = _rec->getReplyBuilder();

    if (OpMsg::isFlagSet(m, OpMsg::kMoreToCome)) {
        return {};  // Don't reply.
    }

    DbResponse dbResponse;
    if (OpMsg::isFlagSet(m, OpMsg::kExhaustSupported)) {
        auto responseObj = reply->getBodyBuilder().asTempObj();
        if (responseObj.getField("ok").trueValue()) {
            dbResponse.shouldRunAgainForExhaust = reply->shouldRunAgainForExhaust();
            dbResponse.nextInvocation = reply->getNextInvocation();
        }
    }
    dbResponse.response = reply->done();

    return dbResponse;
}

Future<DbResponse> ClientCommand::run() {
    auto pf = makePromiseFuture<void>();
    auto future = std::move(pf.future)
                      .then([this, anchor = shared_from_this()] { _parse(); })
                      .then([this, anchor = shared_from_this()] { return _execute(); })
                      .onError([this, anchor = shared_from_this()](Status status) {
                          return _handleException(std::move(status));
                      })
                      .then([this, anchor = shared_from_this()] { return _produceResponse(); });
    pf.promise.emplaceValue();
    return future;
}

Future<DbResponse> Strategy::clientCommand(std::shared_ptr<RequestExecutionContext> rec) {
    auto instance = std::make_shared<ClientCommand>(std::move(rec));
    return instance->run();
}

DbResponse Strategy::getMore(OperationContext* opCtx, const NamespaceString& nss, DbMessage* dbm) {
    const int ntoreturn = dbm->pullInt();
    uassert(
        34424, str::stream() << "Invalid ntoreturn for OP_GET_MORE: " << ntoreturn, ntoreturn >= 0);
    const long long cursorId = dbm->pullInt64();

    globalOpCounters.gotGetMore();

    // TODO: Handle stale config exceptions here from coll being dropped or sharded during op for
    // now has same semantics as legacy request.

    auto statusGetDb = Grid::get(opCtx)->catalogCache()->getDatabase(opCtx, nss.db());
    if (statusGetDb == ErrorCodes::NamespaceNotFound) {
        return replyToQuery(ResultFlag_CursorNotFound, nullptr, 0, 0);
    }
    uassertStatusOK(statusGetDb);

    boost::optional<std::int64_t> batchSize;
    if (ntoreturn) {
        batchSize = ntoreturn;
    }

    GetMoreRequest getMoreRequest(nss, cursorId, batchSize, boost::none, boost::none, boost::none);

    // Set the upconverted getMore as the CurOp command object.
    CurOp::get(opCtx)->setGenericOpRequestDetails(
        opCtx, nss, nullptr, getMoreRequest.toBSON(), dbm->msg().operation());

    auto cursorResponse = ClusterFind::runGetMore(opCtx, getMoreRequest);
    if (cursorResponse == ErrorCodes::CursorNotFound) {
        return replyToQuery(ResultFlag_CursorNotFound, nullptr, 0, 0);
    }
    uassertStatusOK(cursorResponse.getStatus());

    // Build the response document.
    BufBuilder buffer(FindCommon::kInitReplyBufferSize);

    int numResults = 0;
    for (const auto& obj : cursorResponse.getValue().getBatch()) {
        buffer.appendBuf((void*)obj.objdata(), obj.objsize());
        ++numResults;
    }

    return replyToQuery(0,
                        buffer.buf(),
                        buffer.len(),
                        numResults,
                        cursorResponse.getValue().getNumReturnedSoFar().value_or(0),
                        cursorResponse.getValue().getCursorId());
}

void Strategy::killCursors(OperationContext* opCtx, DbMessage* dbm) {
    const int numCursors = dbm->pullInt();
    massert(34425,
            str::stream() << "Invalid killCursors message. numCursors: " << numCursors
                          << ", message size: " << dbm->msg().dataSize() << ".",
            dbm->msg().dataSize() == 8 + (8 * numCursors));
    uassert(28794,
            str::stream() << "numCursors must be between 1 and 29999.  numCursors: " << numCursors
                          << ".",
            numCursors >= 1 && numCursors < 30000);

    globalOpCounters.gotOp(dbKillCursors, false);

    ConstDataCursor cursors(dbm->getArray(numCursors));

    Client* const client = opCtx->getClient();
    ClusterCursorManager* const manager = Grid::get(opCtx)->getCursorManager();

    for (int i = 0; i < numCursors; ++i) {
        const CursorId cursorId = cursors.readAndAdvance<LittleEndian<int64_t>>();

        boost::optional<NamespaceString> nss = manager->getNamespaceForCursorId(cursorId);
        if (!nss) {
            LOGV2_DEBUG(22773,
                        3,
                        "Can't find cursor to kill, no namespace found. Cursor id: {cursorId}",
                        "Can't find cursor to kill, no namespace found",
                        "cursorId"_attr = cursorId);
            continue;
        }

        auto authzSession = AuthorizationSession::get(client);
        auto authChecker = [&authzSession, &nss](UserNameIterator userNames) -> Status {
            return authzSession->checkAuthForKillCursors(*nss, userNames);
        };
        auto authzStatus = manager->checkAuthForKillCursors(opCtx, *nss, cursorId, authChecker);
        audit::logKillCursorsAuthzCheck(client, *nss, cursorId, authzStatus.code());
        if (!authzStatus.isOK()) {
            LOGV2_DEBUG(
                22774,
                3,
                "Not authorized to kill cursor. Namespace: '{namespace}', cursor id: {cursorId}",
                "Not authorized to kill cursor",
                "namespace"_attr = *nss,
                "cursorId"_attr = cursorId);
            continue;
        }

        Status killCursorStatus = manager->killCursor(opCtx, *nss, cursorId);
        if (!killCursorStatus.isOK()) {
            LOGV2_DEBUG(
                22775,
                3,
                "Can't find cursor to kill. Namespace: '{namespace}', cursor id: {cursorId}",
                "Can't find cursor to kill",
                "namespace"_attr = *nss,
                "cursorId"_attr = cursorId);
            continue;
        }

        LOGV2_DEBUG(22776,
                    3,
                    "Killed cursor. Namespace: '{namespace}', cursor id: {cursorId}",
                    "Killed cursor",
                    "namespace"_attr = *nss,
                    "cursorId"_attr = cursorId);
    }
}

void Strategy::writeOp(std::shared_ptr<RequestExecutionContext> rec) {
    rec->setRequest([msg = rec->getMessage()]() {
        switch (msg.operation()) {
            case dbInsert: {
                return InsertOp::parseLegacy(msg).serialize({});
            }
            case dbUpdate: {
                return UpdateOp::parseLegacy(msg).serialize({});
            }
            case dbDelete: {
                return DeleteOp::parseLegacy(msg).serialize({});
            }
            default:
                MONGO_UNREACHABLE;
        }
    }());

    rec->setReplyBuilder(std::make_unique<rpc::OpMsgReplyBuilder>());
    runCommand(std::move(rec),
               std::make_shared<BSONObjBuilder>())  // built objects are ignored
        .get();
}

void Strategy::explainFind(OperationContext* opCtx,
                           const BSONObj& findCommand,
                           const QueryRequest& qr,
                           ExplainOptions::Verbosity verbosity,
                           const ReadPreferenceSetting& readPref,
                           BSONObjBuilder* out) {
    const auto explainCmd = ClusterExplain::wrapAsExplain(findCommand, verbosity);

    long long millisElapsed;
    std::vector<AsyncRequestsSender::Response> shardResponses;

    for (int tries = 0;; ++tries) {
        bool canRetry = tries < 4;  // Fifth try (i.e. try #4) is the last one.

        // We will time how long it takes to run the commands on the shards.
        Timer timer;
        try {
            const auto routingInfo = uassertStatusOK(
                Grid::get(opCtx)->catalogCache()->getCollectionRoutingInfo(opCtx, qr.nss()));
            shardResponses =
                scatterGatherVersionedTargetByRoutingTable(opCtx,
                                                           qr.nss().db(),
                                                           qr.nss(),
                                                           routingInfo,
                                                           explainCmd,
                                                           readPref,
                                                           Shard::RetryPolicy::kIdempotent,
                                                           qr.getFilter(),
                                                           qr.getCollation());
            millisElapsed = timer.millis();
            break;
        } catch (ExceptionFor<ErrorCodes::ShardInvalidatedForTargeting>&) {
            Grid::get(opCtx)->catalogCache()->setOperationShouldBlockBehindCatalogCacheRefresh(
                opCtx, true);

            if (canRetry) {
                continue;
            }
            throw;
        } catch (const ExceptionForCat<ErrorCategory::NeedRetargettingError>& ex) {
            const auto staleNs = [&] {
                if (auto staleInfo = ex.extraInfo<StaleConfigInfo>()) {
                    return staleInfo->getNss();
                }
                throw;
            }();

            if (auto staleInfo = ex.extraInfo<StaleConfigInfo>()) {
                Grid::get(opCtx)
                    ->catalogCache()
                    ->invalidateShardOrEntireCollectionEntryForShardedCollection(
                        staleNs, staleInfo->getVersionWanted(), staleInfo->getShardId());
            } else {
                // If we don't have the stale config info and therefore don't know the shard's id,
                // we have to force all further targetting requests for the namespace to block on
                // a refresh.
                Grid::get(opCtx)->catalogCache()->invalidateCollectionEntry_LINEARIZABLE(staleNs);
            }

            if (canRetry) {
                continue;
            }
            throw;
        } catch (const ExceptionFor<ErrorCodes::StaleDbVersion>& ex) {
            // Mark database entry in cache as stale.
            Grid::get(opCtx)->catalogCache()->onStaleDatabaseVersion(ex->getDb(),
                                                                     ex->getVersionWanted());
            if (canRetry) {
                continue;
            }
            throw;
        } catch (const ExceptionForCat<ErrorCategory::SnapshotError>&) {
            // Simple retry on any type of snapshot error.
            if (canRetry) {
                continue;
            }
            throw;
        }
    }

    const char* mongosStageName =
        ClusterExplain::getStageNameForReadOp(shardResponses.size(), findCommand);

    uassertStatusOK(ClusterExplain::buildExplainResult(
        opCtx, shardResponses, mongosStageName, millisElapsed, findCommand, out));
}
}  // namespace mongo
