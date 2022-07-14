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
#include "mongo/db/auth/authorization_checks.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/commands.h"
#include "mongo/db/curop.h"
#include "mongo/db/error_labels.h"
#include "mongo/db/initialize_api_parameters.h"
#include "mongo/db/initialize_operation_session_info.h"
#include "mongo/db/logical_session_id_helpers.h"
#include "mongo/db/logical_time_validator.h"
#include "mongo/db/matcher/extensions_callback_noop.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/not_primary_error_tracker.h"
#include "mongo/db/operation_time_tracker.h"
#include "mongo/db/ops/write_ops.h"
#include "mongo/db/query/find_common.h"
#include "mongo/db/query/getmore_command_gen.h"
#include "mongo/db/query/query_request_helper.h"
#include "mongo/db/read_write_concern_defaults.h"
#include "mongo/db/repl/repl_server_parameters_gen.h"
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
#include "mongo/rpc/rewrite_state_change_errors.h"
#include "mongo/rpc/warn_unsupported_wire_ops.h"
#include "mongo/s/catalog_cache.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/cluster_commands_helpers.h"
#include "mongo/s/commands/cluster_explain.h"
#include "mongo/s/grid.h"
#include "mongo/s/load_balancer_support.h"
#include "mongo/s/mongos_topology_coordinator.h"
#include "mongo/s/query/cluster_cursor_manager.h"
#include "mongo/s/query/cluster_find.h"
#include "mongo/s/session_catalog_router.h"
#include "mongo/s/shard_invalidated_for_targeting_exception.h"
#include "mongo/s/stale_exception.h"
#include "mongo/s/transaction_router.h"
#include "mongo/transport/hello_metrics.h"
#include "mongo/transport/service_executor.h"
#include "mongo/transport/session.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/future_util.h"
#include "mongo/util/scopeguard.h"
#include "mongo/util/str.h"
#include "mongo/util/timer.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding

namespace mongo {
namespace {
MONGO_FAIL_POINT_DEFINE(hangBeforeCheckingMongosShutdownInterrupt);
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

Future<void> runCommandInvocation(std::shared_ptr<RequestExecutionContext> rec,
                                  std::shared_ptr<CommandInvocation> invocation) {
    bool useDedicatedThread = [&] {
        auto client = rec->getOpCtx()->getClient();
        if (auto context = transport::ServiceExecutorContext::get(client); context) {
            return context->useDedicatedThread();
        }
        tassert(5453902,
                "Threading model may only be absent for internal and direct clients",
                !client->hasRemote() || client->isInDirectClient());
        return true;
    }();
    return CommandHelpers::runCommandInvocation(
        std::move(rec), std::move(invocation), useDedicatedThread);
}

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
        if (VectorClock::isValidComponentTime(operationTime)) {
            LOGV2_DEBUG(22764,
                        5,
                        "Appending operationTime: {operationTime}",
                        "Appending operationTime",
                        "operationTime"_attr = operationTime.asTimestamp());
            operationTime.appendAsOperationTime(responseBuilder);
        } else if (VectorClock::isValidComponentTime(clusterTime)) {
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

    return runCommandInvocation(rec, std::move(invocation))
        .tapError([rec = std::move(rec)](Status status) {
            if (auto code = status.code(); ErrorCodes::isSnapshotError(code) ||
                ErrorCodes::isNeedRetargettingError(code) ||
                code == ErrorCodes::ShardInvalidatedForTargeting ||
                code == ErrorCodes::StaleDbVersion ||
                code == ErrorCodes::ShardCannotRefreshDueToLocksHeld ||
                code == ErrorCodes::WouldChangeOwningShard) {
                // Don't abort on possibly retryable errors.
                return;
            }

            auto opCtx = rec->getOpCtx();

            // Abort if the router wasn't yielded, which may happen at global shutdown.
            if (auto txnRouter = TransactionRouter::get(opCtx)) {
                txnRouter.implicitlyAbortTransaction(opCtx, status);
            }
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
class ExecCommandClient final {
public:
    ExecCommandClient(ExecCommandClient&&) = delete;
    ExecCommandClient(const ExecCommandClient&) = delete;

    ExecCommandClient(std::shared_ptr<RequestExecutionContext> rec,
                      std::shared_ptr<CommandInvocation> invocation)
        : _rec(std::move(rec)), _invocation(std::move(invocation)) {}

    Future<void> run();

private:
    // Prepare the environment for running the invocation (e.g., checking authorization).
    void _prologue();

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

void ExecCommandClient::_prologue() {
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
            iassert(Status(ErrorCodes::SkipCommandExecution, "Already served help command"));
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
        iassert(Status(ErrorCodes::SkipCommandExecution, "Failed to check authorization"));
    }

    // attach tracking
    rpc::TrackingMetadata trackingMetadata;
    trackingMetadata.initWithOperName(c->getName());
    rpc::TrackingMetadata::get(opCtx) = trackingMetadata;

    // Extract and process metadata from the command request body.
    ReadPreferenceSetting::get(opCtx) =
        uassertStatusOK(ReadPreferenceSetting::fromContainingBSON(request.body));
    VectorClock::get(opCtx)->gossipIn(opCtx, request.body, !c->requiresAuth());
}

Future<void> ExecCommandClient::_run() {
    OperationContext* opCtx = _rec->getOpCtx();
    if (auto txnRouter = TransactionRouter::get(opCtx); txnRouter) {
        return invokeInTransactionRouter(_rec, _invocation);
    } else {
        return runCommandInvocation(_rec, _invocation);
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

    auto seCtx = transport::ServiceExecutorContext::get(opCtx->getClient());
    if (!seCtx) {
        // We were run by a background worker.
        return;
    }

    if (!_invocation->isSafeForBorrowedThreads()) {
        // If the last command wasn't safe for a borrowed thread, then let's move
        // off of it.
        seCtx->setUseDedicatedThread(true);
    }
}

Future<void> ExecCommandClient::run() {
    return makeReadyFutureWith([&] {
               _prologue();

               return _run();
           })
        .then([this] { _epilogue(); })
        .onCompletion([this](Status status) {
            if (!status.isOK() && status.code() != ErrorCodes::SkipCommandExecution)
                return status;  // Execution was interrupted due to an error.

            _onCompletion();
            return Status::OK();
        });
}

MONGO_FAIL_POINT_DEFINE(doNotRefreshShardsOnRetargettingError);

/**
 * Produces a future-chain that parses the command, runs the parsed command, and captures the result
 * in replyBuilder.
 */
class ParseAndRunCommand final {
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
    void _parseCommand();

    // updates statistics and applies labels if an error occurs.
    void _updateStatsAndApplyErrorLabels(const Status& status);

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
class ParseAndRunCommand::RunInvocation final {
public:
    RunInvocation(RunInvocation&&) = delete;
    RunInvocation(const RunInvocation&) = delete;

    explicit RunInvocation(ParseAndRunCommand* parc) : _parc(parc) {}

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

    ParseAndRunCommand* const _parc;

    boost::optional<RouterOperationContextSession> _routerSession;
    bool _shouldAffectCommandCounter = false;
};

/*
 * Produces a future-chain that runs the invocation and retries if necessary.
 */
class ParseAndRunCommand::RunAndRetry final {
public:
    RunAndRetry(RunAndRetry&&) = delete;
    RunAndRetry(const RunAndRetry&) = delete;

    explicit RunAndRetry(ParseAndRunCommand* parc) : _parc(parc) {}

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
    void _onShardCannotRefreshDueToLocksHeldError(Status& status);
    void _onTenantMigrationAborted(Status& status);

    ParseAndRunCommand* const _parc;

    int _tries = 0;
};
void ParseAndRunCommand::_updateStatsAndApplyErrorLabels(const Status& status) {
    auto opCtx = _rec->getOpCtx();
    const auto command = _rec->getCommand();

    if (command)
        command->incrementCommandsFailed();
    NotPrimaryErrorTracker::get(opCtx->getClient()).recordError(status.code());
    // WriteConcern error (wcCode) is set to boost::none because:
    // 1. TransientTransaction error label handling for commitTransaction command in mongos is
    //    delegated to the shards. Mongos simply propagates the shard's response up to the client.
    // 2. For other commands in a transaction, they shouldn't get a writeConcern error so this
    //    setting doesn't apply.

    if (_osi.has_value()) {

        auto errorLabels = getErrorLabels(opCtx,
                                          *_osi,
                                          command->getName(),
                                          status.code(),
                                          boost::none,
                                          false /* isInternalClient */,
                                          true /* isMongos */,
                                          repl::OpTime{},
                                          repl::OpTime{});


        _errorBuilder->appendElements(errorLabels);
    }
}
void ParseAndRunCommand::_parseCommand() {
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
        iassert(Status(ErrorCodes::SkipCommandExecution, errorMsg));
    }

    _rec->setCommand(command);

    _isHello.emplace(command->getName() == "hello"_sd || command->getName() == "isMaster"_sd);

    opCtx->setExhaust(OpMsg::isFlagSet(m, OpMsg::kExhaustSupported));
    Client* client = opCtx->getClient();
    const auto session = client->session();
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
            request.body[query_request_helper::queryOptionMaxTimeMS].eoo());

    // If the command includes a 'comment' field, set it on the current OpCtx.
    if (auto commentField = request.body["comment"]) {
        stdx::lock_guard<Client> lk(*client);
        opCtx->setComment(commentField.wrap());
    }

    auto const apiParamsFromClient = initializeAPIParameters(request.body, command);

    {
        // We must obtain the client lock to set APIParameters on the operation context, as it may
        // be concurrently read by CurrentOp.
        stdx::lock_guard<Client> lk(*client);
        APIParameters::get(opCtx) = APIParameters::fromClient(apiParamsFromClient);
    }

    rpc::readRequestMetadata(opCtx, request, command->requiresAuth());

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

    auto& readConcernArgs = repl::ReadConcernArgs::get(opCtx);
    Status readConcernParseStatus = Status::OK();
    {
        // We must obtain the client lock to set ReadConcernArgs on the operation context, as it may
        // be concurrently read by CurrentOp.
        stdx::lock_guard<Client> lk(*client);
        readConcernParseStatus = readConcernArgs.initialize(request.body);
    }

    if (!readConcernParseStatus.isOK()) {
        auto builder = replyBuilder->getBodyBuilder();
        CommandHelpers::appendCommandStatusNoThrow(builder, readConcernParseStatus);
        iassert(Status(ErrorCodes::SkipCommandExecution, "Failed to parse read concern"));
    }
}

Status ParseAndRunCommand::RunInvocation::_setup() {
    auto invocation = _parc->_invocation;
    auto opCtx = _parc->_rec->getOpCtx();
    auto command = _parc->_rec->getCommand();
    const auto& request = _parc->_rec->getRequest();
    auto replyBuilder = _parc->_rec->getReplyBuilder();

    const int maxTimeMS =
        uassertStatusOK(parseMaxTimeMS(request.body[query_request_helper::cmdOptionMaxTimeMS]));
    if (maxTimeMS > 0 && command->getLogicalOp() != LogicalOp::opGetMore) {
        opCtx->setDeadlineAfterNowBy(Milliseconds{maxTimeMS}, ErrorCodes::MaxTimeMSExpired);
    }

    if (MONGO_unlikely(
            hangBeforeCheckingMongosShutdownInterrupt.shouldFail([&](const BSONObj& data) {
                if (data.hasField("cmdName") && data.hasField("ns")) {
                    std::string cmdNS = _parc->_ns.get();
                    return ((data.getStringField("cmdName") == _parc->_commandName) &&
                            (data.getStringField("ns") == cmdNS));
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

    if (_parc->_isHello.get()) {
        // Preload generic ClientMetadata ahead of our first hello request. After the first
        // request, metaElement should always be empty.
        auto metaElem = request.body[kMetadataDocumentName];
        ClientMetadata::setFromMetadata(opCtx->getClient(), metaElem);
    }

    enforceRequireAPIVersion(opCtx, command);

    auto& apiParams = APIParameters::get(opCtx);
    auto& apiVersionMetrics = APIVersionMetrics::get(opCtx->getServiceContext());
    if (auto clientMetadata = ClientMetadata::get(opCtx->getClient())) {
        auto appName = clientMetadata->getApplicationName().toString();
        apiVersionMetrics.update(appName, apiParams);
    }

    CommandHelpers::evaluateFailCommandFailPoint(opCtx, invocation.get());
    bool startTransaction = false;
    if (_parc->_osi->getAutocommit()) {
        _routerSession.emplace(opCtx);

        load_balancer_support::setMruSession(opCtx->getClient(), *opCtx->getLogicalSessionId());

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

    // This is the WC extracted from the command object, so the CWWC or implicit default hasn't been
    // applied yet, which is why "usedDefaultConstructedWC" flag can be used an indicator of whether
    // the client supplied a WC or not.
    bool clientSuppliedWriteConcern = !_parc->_wc->usedDefaultConstructedWC;
    bool customDefaultWriteConcernWasApplied = false;
    bool isInternalClient =
        (opCtx->getClient()->session() &&
         (opCtx->getClient()->session()->getTags() & transport::Session::kInternalClient));

    if (supportsWriteConcern && !clientSuppliedWriteConcern &&
        (!TransactionRouter::get(opCtx) || command->isTransactionCommand()) &&
        !opCtx->getClient()->isInDirectClient()) {
        if (isInternalClient) {
            uassert(
                5569900,
                "received command without explicit writeConcern on an internalClient connection {}"_format(
                    redact(request.body.toString())),
                request.body.hasField(WriteConcernOptions::kWriteConcernField));
        } else {
            // This command is not from a DBDirectClient or internal client, and supports WC, but
            // wasn't given one - so apply the default, if there is one.
            const auto rwcDefaults =
                ReadWriteConcernDefaults::get(opCtx->getServiceContext()).getDefault(opCtx);
            if (const auto wcDefault = rwcDefaults.getDefaultWriteConcern()) {
                _parc->_wc = *wcDefault;
                const auto defaultWriteConcernSource = rwcDefaults.getDefaultWriteConcernSource();
                customDefaultWriteConcernWasApplied = defaultWriteConcernSource &&
                    defaultWriteConcernSource == DefaultWriteConcernSourceEnum::kGlobal;
                LOGV2_DEBUG(22766,
                            2,
                            "Applying default writeConcern on {command} of {writeConcern}",
                            "Applying default writeConcern on command",
                            "command"_attr = request.getCommandName(),
                            "writeConcern"_attr = *wcDefault);
            }
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
            } else if (opCtx->getClient()->isInDirectClient() || isInternalClient) {
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
                    "Applying default readConcern on {command} of {readConcern}",
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
            const auto rwcDefaults =
                ReadWriteConcernDefaults::get(opCtx->getServiceContext()).getDefault(opCtx);
            const auto rcDefault = rwcDefaults.getDefaultReadConcern();
            if (rcDefault) {
                const auto readConcernSource = rwcDefaults.getDefaultReadConcernSource();
                customDefaultReadConcernWasApplied =
                    (readConcernSource &&
                     readConcernSource.get() == DefaultReadConcernSourceEnum::kGlobal);

                applyDefaultReadConcern(*rcDefault);
            }
        }
    }

    // Apply the implicit default read concern even if the command does not support a cluster wide
    // read concern.
    if (!readConcernSupport.defaultReadConcernPermit.isOK() &&
        readConcernSupport.implicitDefaultReadConcernPermit.isOK() && shouldApplyDefaults &&
        readConcernArgs.isEmpty()) {
        const auto rcDefault = ReadWriteConcernDefaults::get(opCtx->getServiceContext())
                                   .getImplicitDefaultReadConcern();
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
        // Re-parse before retrying in case the process of run()-ning the invocation could
        // affect the parsed result.
        _parc->_invocation = command->parse(opCtx, request);
        invariant(_parc->_invocation->ns().toString() == _parc->_ns,
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

Future<void> ParseAndRunCommand::RunAndRetry::_run() {
    return future_util::makeState<ExecCommandClient>(_parc->_rec, _parc->_invocation)
        .thenWithState([](auto* runner) { return runner->run(); })
        .then([rec = _parc->_rec] {
            auto opCtx = rec->getOpCtx();
            auto responseBuilder = rec->getReplyBuilder()->getBodyBuilder();
            if (auto txnRouter = TransactionRouter::get(opCtx)) {
                txnRouter.appendRecoveryToken(&responseBuilder);
            }
        });
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
                  status.code() == ErrorCodes::ShardInvalidatedForTargeting ||
                  status.code() == ErrorCodes::StaleDbVersion ||
                  status.code() == ErrorCodes::ShardCannotRefreshDueToLocksHeld);

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
        !txnRouter && !repl::ReadConcernArgs::get(opCtx).wasAtClusterTimeSelected()) {
        // Non-transaction snapshot read. The client sent readConcern: {level: "snapshot",
        // atClusterTime: T}, where T is older than minSnapshotHistoryWindowInSeconds, retrying
        // won't succeed.
        iassert(status);
    }

    if (!_canRetry())
        iassert(status);
}

void ParseAndRunCommand::RunAndRetry::_onShardCannotRefreshDueToLocksHeldError(Status& status) {
    invariant(status.code() == ErrorCodes::ShardCannotRefreshDueToLocksHeld);

    _checkRetryForTransaction(status);

    if (!_canRetry())
        iassert(status);
}

void ParseAndRunCommand::RunAndRetry::_onTenantMigrationAborted(Status& status) {
    invariant(status.code() == ErrorCodes::TenantMigrationAborted);

    if (!_canRetry())
        iassert(status);
}

Future<void> ParseAndRunCommand::RunAndRetry::run() {
    return makeReadyFutureWith([&] {
               // Try kMaxNumStaleVersionRetries times. On the last try, exceptions are
               // rethrown.
               _tries++;

               _setup();
               return _run();
           })
        .onError<ErrorCodes::ShardInvalidatedForTargeting>([this](Status status) {
            _onShardInvalidatedForTargeting(status);
            return run();  // Retry
        })
        .onErrorCategory<ErrorCategory::NeedRetargettingError>([this](Status status) {
            _onNeedRetargetting(status);
            return run();  // Retry
        })
        .onError<ErrorCodes::StaleDbVersion>([this](Status status) {
            _onStaleDbVersion(status);
            return run();  // Retry
        })
        .onErrorCategory<ErrorCategory::SnapshotError>([this](Status status) {
            _onSnapshotError(status);
            return run();  // Retry
        })
        .onError<ErrorCodes::ShardCannotRefreshDueToLocksHeld>([this](Status status) {
            _onShardCannotRefreshDueToLocksHeldError(status);
            return run();  // Retry
        })
        .onError<ErrorCodes::TenantMigrationAborted>([this](Status status) {
            _onTenantMigrationAborted(status);
            return run();  // Retry
        });
}

Future<void> ParseAndRunCommand::RunInvocation::run() {
    return makeReadyFutureWith([&] {
        iassert(_setup());
        return future_util::makeState<RunAndRetry>(_parc).thenWithState(
            [](auto* runner) { return runner->run(); });
    });
}

Future<void> ParseAndRunCommand::run() {
    return makeReadyFutureWith([&] {
               _parseCommand();
               return future_util::makeState<RunInvocation>(this).thenWithState(
                   [](auto* runner) { return runner->run(); });
           })
        .tapError([this](Status status) { _updateStatsAndApplyErrorLabels(status); })
        .onError<ErrorCodes::SkipCommandExecution>([this](Status status) {
            // We've already skipped execution, so no other action is required.
            return Status::OK();
        });
}

}  // namespace

// Maintains the state required to execute client commands, and provides the interface to construct
// a future-chain that runs the command against the database.
class ClientCommand final {
public:
    ClientCommand(ClientCommand&&) = delete;
    ClientCommand(const ClientCommand&) = delete;

    explicit ClientCommand(std::shared_ptr<RequestExecutionContext> rec)
        : _rec(std::move(rec)), _errorBuilder(std::make_shared<BSONObjBuilder>()) {}

    Future<DbResponse> run();

private:
    void _parseMessage();

    Future<void> _execute();

    // Handler for exceptions thrown during parsing and executing the command.
    Future<void> _handleException(Status);

    // Extracts the command response from the replyBuilder.
    DbResponse _produceResponse();

    const std::shared_ptr<RequestExecutionContext> _rec;
    const std::shared_ptr<BSONObjBuilder> _errorBuilder;

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

    return future_util::makeState<ParseAndRunCommand>(_rec, _errorBuilder)
        .thenWithState([](auto* runner) { return runner->run(); })
        .then([this] {
            LOGV2_DEBUG(22771,
                        3,
                        "Command end db: {db} msg id: {headerId}",
                        "Command end",
                        "db"_attr = _rec->getRequest().getDatabase().toString(),
                        "headerId"_attr = _rec->getMessage().header().getId());
        })
        .tapError([this](Status status) {
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
    if (status == ErrorCodes::CloseConnectionForShutdownCommand || _propagateException) {
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
    if (auto doc = rpc::RewriteStateChangeErrors::rewrite(reply->getBodyBuilder().asTempObj(),
                                                          _rec->getOpCtx())) {
        reply->reset();
        reply->getBodyBuilder().appendElements(*doc);
    }
    dbResponse.response = reply->done();

    return dbResponse;
}

Future<DbResponse> ClientCommand::run() {
    return makeReadyFutureWith([&] {
               _parseMessage();
               return _execute();
           })
        .onError([this](Status status) { return _handleException(std::move(status)); })
        .then([this] { return _produceResponse(); });
}

Future<DbResponse> Strategy::clientCommand(std::shared_ptr<RequestExecutionContext> rec) {
    return future_util::makeState<ClientCommand>(std::move(rec)).thenWithState([](auto* runner) {
        return runner->run();
    });
}

}  // namespace mongo
