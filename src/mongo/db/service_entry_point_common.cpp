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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kCommand

#include "mongo/platform/basic.h"

#include "mongo/db/service_entry_point_common.h"

#include <fmt/format.h>

#include "mongo/base/checked_cast.h"
#include "mongo/bson/mutable/document.h"
#include "mongo/bson/util/bson_extract.h"
#include "mongo/client/server_is_master_monitor.h"
#include "mongo/db/audit.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/impersonation_session.h"
#include "mongo/db/client.h"
#include "mongo/db/command_can_run_here.h"
#include "mongo/db/commands.h"
#include "mongo/db/commands/txn_cmds_gen.h"
#include "mongo/db/curop.h"
#include "mongo/db/curop_failpoint_helpers.h"
#include "mongo/db/curop_metrics.h"
#include "mongo/db/cursor_manager.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/error_labels.h"
#include "mongo/db/initialize_api_parameters.h"
#include "mongo/db/initialize_operation_session_info.h"
#include "mongo/db/introspect.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/lasterror.h"
#include "mongo/db/logical_clock.h"
#include "mongo/db/logical_session_id.h"
#include "mongo/db/logical_session_id_helpers.h"
#include "mongo/db/logical_time_validator.h"
#include "mongo/db/ops/write_ops.h"
#include "mongo/db/ops/write_ops_exec.h"
#include "mongo/db/query/find.h"
#include "mongo/db/read_concern.h"
#include "mongo/db/read_write_concern_defaults.h"
#include "mongo/db/repl/optime.h"
#include "mongo/db/repl/read_concern_args.h"
#include "mongo/db/repl/repl_client_info.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/repl/speculative_majority_read_info.h"
#include "mongo/db/repl/storage_interface.h"
#include "mongo/db/repl/tenant_migration_donor_util.h"
#include "mongo/db/request_execution_context.h"
#include "mongo/db/run_op_kill_cursors.h"
#include "mongo/db/s/operation_sharding_state.h"
#include "mongo/db/s/sharding_state.h"
#include "mongo/db/s/transaction_coordinator_factory.h"
#include "mongo/db/service_entry_point_common.h"
#include "mongo/db/session_catalog_mongod.h"
#include "mongo/db/stats/api_version_metrics.h"
#include "mongo/db/stats/counters.h"
#include "mongo/db/stats/server_read_concern_metrics.h"
#include "mongo/db/stats/top.h"
#include "mongo/db/transaction_participant.h"
#include "mongo/db/transaction_validation.h"
#include "mongo/db/vector_clock.h"
#include "mongo/logv2/log.h"
#include "mongo/rpc/factory.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/rpc/message.h"
#include "mongo/rpc/metadata.h"
#include "mongo/rpc/metadata/oplog_query_metadata.h"
#include "mongo/rpc/metadata/repl_set_metadata.h"
#include "mongo/rpc/metadata/tracking_metadata.h"
#include "mongo/rpc/op_msg.h"
#include "mongo/rpc/reply_builder_interface.h"
#include "mongo/transport/ismaster_metrics.h"
#include "mongo/transport/service_executor.h"
#include "mongo/transport/session.h"
#include "mongo/util/duration.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/scopeguard.h"

namespace mongo {

MONGO_FAIL_POINT_DEFINE(rsStopGetMore);
MONGO_FAIL_POINT_DEFINE(respondWithNotPrimaryInCommandDispatch);
MONGO_FAIL_POINT_DEFINE(skipCheckingForNotMasterInCommandDispatch);
MONGO_FAIL_POINT_DEFINE(sleepMillisAfterCommandExecutionBegins);
MONGO_FAIL_POINT_DEFINE(waitAfterNewStatementBlocksBehindPrepare);
MONGO_FAIL_POINT_DEFINE(waitAfterCommandFinishesExecution);
MONGO_FAIL_POINT_DEFINE(failWithErrorCodeInRunCommand);

// Tracks the number of times a legacy unacknowledged write failed due to
// not primary error resulted in network disconnection.
Counter64 notPrimaryLegacyUnackWrites;
ServerStatusMetricField<Counter64> displayNotPrimaryLegacyUnackWrites(
    "repl.network.notPrimaryLegacyUnacknowledgedWrites", &notPrimaryLegacyUnackWrites);

// Tracks the number of times an unacknowledged write failed due to not primary error
// resulted in network disconnection.
Counter64 notPrimaryUnackWrites;
ServerStatusMetricField<Counter64> displayNotPrimaryUnackWrites(
    "repl.network.notPrimaryUnacknowledgedWrites", &notPrimaryUnackWrites);

namespace {

using namespace fmt::literals;

/*
 * Allows for the very complex handleRequest function to be decomposed into parts.
 * It also provides the infrastructure to futurize the process of executing commands.
 */
struct HandleRequest {
    // Maintains the context (e.g., opCtx and replyBuilder) required for command execution.
    class ExecutionContext final : public RequestExecutionContext {
    public:
        ExecutionContext(OperationContext* opCtx,
                         Message msg,
                         std::unique_ptr<const ServiceEntryPointCommon::Hooks> hooks)
            : RequestExecutionContext(opCtx), behaviors(std::move(hooks)) {
            // It also initializes dbMessage, which is accessible via getDbMessage()
            setMessage(std::move(msg));
        }
        ~ExecutionContext() = default;

        Client& client() const {
            return *getOpCtx()->getClient();
        }

        auto session() const {
            return client().session();
        }

        NetworkOp op() const {
            return getMessage().operation();
        }

        CurOp& currentOp() {
            return *CurOp::get(getOpCtx());
        }

        NamespaceString nsString() const {
            auto& dbmsg = getDbMessage();
            if (!dbmsg.messageShouldHaveNs())
                return {};
            return NamespaceString(dbmsg.getns());
        }

        void assertValidNsString() {
            if (!nsString().isValid()) {
                uassert(
                    16257, str::stream() << "Invalid ns [" << nsString().toString() << "]", false);
            }
        }

        std::unique_ptr<const ServiceEntryPointCommon::Hooks> behaviors;
        boost::optional<long long> slowMsOverride;
        bool forceLog = false;
    };

    struct OpRunner {
        explicit OpRunner(HandleRequest* hr) : executionContext{hr->executionContext} {}
        virtual ~OpRunner() = default;
        virtual Future<DbResponse> run() = 0;
        std::shared_ptr<ExecutionContext> executionContext;
    };

    HandleRequest(OperationContext* opCtx,
                  const Message& msg,
                  std::unique_ptr<const ServiceEntryPointCommon::Hooks> behaviors)
        : executionContext(std::make_shared<ExecutionContext>(
              opCtx, const_cast<Message&>(msg), std::move(behaviors))) {}

    std::unique_ptr<OpRunner> makeOpRunner();

    Future<void> startOperation();
    Future<void> completeOperation();

    std::shared_ptr<ExecutionContext> executionContext;
};

void generateLegacyQueryErrorResponse(const AssertionException& exception,
                                      const QueryMessage& queryMessage,
                                      CurOp* curop,
                                      Message* response) {
    curop->debug().errInfo = exception.toStatus();

    if (queryMessage.query.valid())
        LOGV2_OPTIONS(51777,
                      {logv2::LogComponent::kQuery},
                      "Assertion {error} ns: {namespace} query: {query}",
                      "Assertion for valid query",
                      "error"_attr = exception,
                      "namespace"_attr = queryMessage.ns,
                      "query"_attr = redact(queryMessage.query));
    else
        LOGV2_OPTIONS(51778,
                      {logv2::LogComponent::kQuery},
                      "Assertion {error} ns: {namespace} query object is corrupt",
                      "Assertion for query with corrupted object",
                      "error"_attr = exception,
                      "namespace"_attr = queryMessage.ns);

    if (queryMessage.ntoskip || queryMessage.ntoreturn) {
        LOGV2_OPTIONS(21952,
                      {logv2::LogComponent::kQuery},
                      "Query's nToSkip = {nToSkip} and nToReturn = {nToReturn}",
                      "Assertion for query with nToSkip and/or nToReturn",
                      "nToSkip"_attr = queryMessage.ntoskip,
                      "nToReturn"_attr = queryMessage.ntoreturn);
    }

    BSONObjBuilder err;
    err.append("$err", exception.reason());
    err.append("code", exception.code());
    err.append("ok", 0.0);
    auto const extraInfo = exception.extraInfo();
    if (extraInfo) {
        extraInfo->serialize(&err);
    }
    BSONObj errObj = err.done();

    const bool isStaleConfig = exception.code() == ErrorCodes::StaleConfig;
    if (isStaleConfig) {
        LOGV2_OPTIONS(21953,
                      {logv2::LogComponent::kQuery},
                      "Stale version detected during query over {namespace}: {error}",
                      "Detected stale version while querying namespace",
                      "namespace"_attr = queryMessage.ns,
                      "error"_attr = errObj);
    }

    BufBuilder bb;
    bb.skip(sizeof(QueryResult::Value));
    bb.appendBuf((void*)errObj.objdata(), errObj.objsize());

    // TODO: call replyToQuery() from here instead of this!!! see dbmessage.h
    QueryResult::View msgdata = bb.buf();
    QueryResult::View qr = msgdata;
    qr.setResultFlags(ResultFlag_ErrSet);
    if (isStaleConfig) {
        qr.setResultFlags(qr.getResultFlags() | ResultFlag_ShardConfigStale);
    }
    qr.msgdata().setLen(bb.len());
    qr.msgdata().setOperation(opReply);
    qr.setCursorId(0);
    qr.setStartingFrom(0);
    qr.setNReturned(1);
    response->setData(bb.release());
}

void registerError(OperationContext* opCtx, const Status& status) {
    LastError::get(opCtx->getClient()).setLastError(status.code(), status.reason());
    CurOp::get(opCtx)->debug().errInfo = status;
}

void generateErrorResponse(OperationContext* opCtx,
                           rpc::ReplyBuilderInterface* replyBuilder,
                           const Status& status,
                           const BSONObj& replyMetadata,
                           BSONObj extraFields = {}) {
    invariant(!status.isOK());
    registerError(opCtx, status);

    // We could have thrown an exception after setting fields in the builder,
    // so we need to reset it to a clean state just to be sure.
    replyBuilder->reset();
    replyBuilder->setCommandReply(status, extraFields);
    replyBuilder->getBodyBuilder().appendElements(replyMetadata);
}

/**
 * Guard object for making a good-faith effort to enter maintenance mode and leave it when it
 * goes out of scope.
 *
 * Sometimes we cannot set maintenance mode, in which case the call to setMaintenanceMode will
 * return a non-OK status.  This class does not treat that case as an error which means that
 * anybody using it is assuming it is ok to continue execution without maintenance mode.
 *
 * TODO: This assumption needs to be audited and documented, or this behavior should be moved
 * elsewhere.
 */
class MaintenanceModeSetter {
    MaintenanceModeSetter(const MaintenanceModeSetter&) = delete;
    MaintenanceModeSetter& operator=(const MaintenanceModeSetter&) = delete;

public:
    MaintenanceModeSetter(OperationContext* opCtx)
        : _opCtx(opCtx),
          _maintenanceModeSet(
              repl::ReplicationCoordinator::get(_opCtx)->setMaintenanceMode(true).isOK()) {}

    ~MaintenanceModeSetter() {
        if (_maintenanceModeSet) {
            repl::ReplicationCoordinator::get(_opCtx)
                ->setMaintenanceMode(false)
                .transitional_ignore();
        }
    }

private:
    OperationContext* const _opCtx;
    const bool _maintenanceModeSet;
};

/**
 * Given the specified command, returns an effective read concern which should be used or an error
 * if the read concern is not valid for the command.
 * Note that the validation performed is not necessarily exhaustive.
 */
StatusWith<repl::ReadConcernArgs> _extractReadConcern(OperationContext* opCtx,
                                                      const CommandInvocation* invocation,
                                                      const BSONObj& cmdObj,
                                                      bool startTransaction,
                                                      bool isInternalClient) {
    repl::ReadConcernArgs readConcernArgs;

    auto readConcernParseStatus = readConcernArgs.initialize(cmdObj);
    if (!readConcernParseStatus.isOK()) {
        return readConcernParseStatus;
    }

    bool clientSuppliedReadConcern = readConcernArgs.isSpecified();
    bool customDefaultWasApplied = false;
    auto readConcernSupport = invocation->supportsReadConcern(readConcernArgs.getLevel());
    if (readConcernSupport.defaultReadConcernPermit.isOK() &&
        (startTransaction || !opCtx->inMultiDocumentTransaction()) &&
        repl::ReplicationCoordinator::get(opCtx)->isReplEnabled() &&
        !opCtx->getClient()->isInDirectClient()) {

        if (isInternalClient) {
            // ReadConcern should always be explicitly specified by operations received from
            // internal clients (ie. from a mongos or mongod), even if it is empty (ie.
            // readConcern: {}, meaning to use the implicit server defaults).
            uassert(
                4569200,
                "received command without explicit readConcern on an internalClient connection {}"_format(
                    redact(cmdObj.toString())),
                readConcernArgs.isSpecified());
        } else if (serverGlobalParams.clusterRole == ClusterRole::ShardServer ||
                   serverGlobalParams.clusterRole == ClusterRole::ConfigServer) {
            if (!readConcernArgs.isSpecified()) {
                // TODO: Disabled until after SERVER-44539, to avoid log spam.
                // LOGV2(21954, "Missing readConcern on {command}", "Missing readConcern "
                // "for command", "command"_attr = invocation->definition()->getName());
            }
        } else {
            // A member in a regular replica set.  Since these servers receive client queries, in
            // this context empty RC (ie. readConcern: {}) means the same as if absent/unspecified,
            // which is to apply the CWRWC defaults if present.  This means we just test isEmpty(),
            // since this covers both isSpecified() && !isSpecified()
            if (readConcernArgs.isEmpty()) {
                const auto rcDefault = ReadWriteConcernDefaults::get(opCtx->getServiceContext())
                                           .getDefaultReadConcern(opCtx);
                if (rcDefault) {
                    customDefaultWasApplied = true;
                    readConcernArgs = std::move(*rcDefault);
                    LOGV2_DEBUG(21955,
                                2,
                                "Applying default readConcern on {command} of {readConcernDefault} "
                                "on {command}",
                                "Applying default readConcern on command",
                                "readConcernDefault"_attr = *rcDefault,
                                "command"_attr = invocation->definition()->getName());
                    // Update the readConcernSupport, since the default RC was applied.
                    readConcernSupport =
                        invocation->supportsReadConcern(readConcernArgs.getLevel());
                }
            }
        }
    }

    // It's fine for clients to provide any provenance value to mongod. But if they haven't, then an
    // appropriate provenance needs to be determined.
    auto& provenance = readConcernArgs.getProvenance();
    if (!provenance.hasSource()) {
        if (clientSuppliedReadConcern) {
            provenance.setSource(ReadWriteConcernProvenance::Source::clientSupplied);
        } else if (customDefaultWasApplied) {
            provenance.setSource(ReadWriteConcernProvenance::Source::customDefault);
        } else {
            provenance.setSource(ReadWriteConcernProvenance::Source::implicitDefault);
        }
    }

    // If we are starting a transaction, we need to check whether the read concern is
    // appropriate for running a transaction.
    if (startTransaction) {
        if (!isReadConcernLevelAllowedInTransaction(readConcernArgs.getLevel())) {
            return {ErrorCodes::InvalidOptions,
                    "The readConcern level must be either 'local' (default), 'majority' or "
                    "'snapshot' in "
                    "order to run in a transaction"};
        }
        if (readConcernArgs.getArgsOpTime()) {
            return {ErrorCodes::InvalidOptions,
                    str::stream() << "The readConcern cannot specify '"
                                  << repl::ReadConcernArgs::kAfterOpTimeFieldName
                                  << "' in a transaction"};
        }
    }

    // Otherwise, if there is a read concern present - either user-specified or from the default -
    // then check whether the command supports it. If there is no explicit read concern level, then
    // it is implicitly "local". There is no need to check whether this is supported, because all
    // commands either support "local" or upconvert the absent readConcern to a stronger level that
    // they do support; for instance, $changeStream upconverts to RC level "majority".
    //
    // Individual transaction statements are checked later on, after we've unstashed the
    // transaction resources.
    if (!opCtx->inMultiDocumentTransaction() && readConcernArgs.hasLevel()) {
        if (!readConcernSupport.readConcernSupport.isOK()) {
            return readConcernSupport.readConcernSupport.withContext(
                str::stream() << "Command " << invocation->definition()->getName()
                              << " does not support " << readConcernArgs.toString());
        }
    }

    // If this command invocation asked for 'majority' read concern, supports blocking majority
    // reads, and storage engine support for majority reads is disabled, then we set the majority
    // read mechanism appropriately i.e. we utilize "speculative" read behavior.
    if (readConcernArgs.getLevel() == repl::ReadConcernLevel::kMajorityReadConcern &&
        invocation->allowsSpeculativeMajorityReads() &&
        !serverGlobalParams.enableMajorityReadConcern) {
        readConcernArgs.setMajorityReadMechanism(
            repl::ReadConcernArgs::MajorityReadMechanism::kSpeculative);
    }

    return readConcernArgs;
}

/**
 * For replica set members it returns the last known op time from opCtx. Otherwise will return
 * uninitialized cluster time.
 */
LogicalTime getClientOperationTime(OperationContext* opCtx) {
    auto const replCoord = repl::ReplicationCoordinator::get(opCtx);
    const bool isReplSet =
        replCoord->getReplicationMode() == repl::ReplicationCoordinator::modeReplSet;

    if (!isReplSet) {
        return LogicalTime();
    }

    return LogicalTime(
        repl::ReplClientInfo::forClient(opCtx->getClient()).getMaxKnownOperationTime());
}

/**
 * Returns the proper operationTime for a command. To construct the operationTime for replica set
 * members, it uses the last optime in the oplog for writes, last committed optime for majority
 * reads, and the last applied optime for every other read. An uninitialized cluster time is
 * returned for non replica set members.
 *
 * The latest in-memory clusterTime is returned if the start operationTime is uninitialized.
 */
LogicalTime computeOperationTime(OperationContext* opCtx, LogicalTime startOperationTime) {
    auto const replCoord = repl::ReplicationCoordinator::get(opCtx);
    const bool isReplSet =
        replCoord->getReplicationMode() == repl::ReplicationCoordinator::modeReplSet;
    invariant(isReplSet);

    auto operationTime = getClientOperationTime(opCtx);
    invariant(operationTime >= startOperationTime);

    // If the last operationTime has not changed, consider this command a read, and, for replica set
    // members, construct the operationTime with the proper optime for its read concern level.
    if (operationTime == startOperationTime) {
        auto& readConcernArgs = repl::ReadConcernArgs::get(opCtx);

        // Note: ReadConcernArgs::getLevel returns kLocal if none was set.
        if (readConcernArgs.getLevel() == repl::ReadConcernLevel::kMajorityReadConcern) {
            operationTime = LogicalTime(replCoord->getLastCommittedOpTime().getTimestamp());
        } else {
            operationTime = LogicalTime(replCoord->getMyLastAppliedOpTime().getTimestamp());
        }
    }

    return operationTime;
}

/**
 * Computes the proper $clusterTime and operationTime values to include in the command response and
 * appends them to it. $clusterTime is added as metadata and operationTime as a command body field.
 *
 * The command body BSONObjBuilder is either the builder for the command body itself, or a builder
 * for extra fields to be added to the reply when generating an error response.
 */
void appendClusterAndOperationTime(OperationContext* opCtx,
                                   BSONObjBuilder* commandBodyFieldsBob,
                                   BSONObjBuilder* metadataBob,
                                   LogicalTime startTime) {
    if (repl::ReplicationCoordinator::get(opCtx)->getReplicationMode() !=
            repl::ReplicationCoordinator::modeReplSet ||
        !VectorClock::get(opCtx)->isEnabled()) {
        return;
    }

    // The appended operationTime must always be <= the appended $clusterTime, so first compute the
    // operationTime.
    auto operationTime = computeOperationTime(opCtx, startTime);

    bool clusterTimeWasOutput = VectorClock::get(opCtx)->gossipOut(opCtx, metadataBob);

    // Ensure that either both operationTime and $clusterTime are output, or neither.
    if (clusterTimeWasOutput) {
        operationTime.appendAsOperationTime(metadataBob);
    }
}

void appendErrorLabelsAndTopologyVersion(OperationContext* opCtx,
                                         BSONObjBuilder* commandBodyFieldsBob,
                                         const OperationSessionInfoFromClient& sessionOptions,
                                         const std::string& commandName,
                                         boost::optional<ErrorCodes::Error> code,
                                         boost::optional<ErrorCodes::Error> wcCode,
                                         bool isInternalClient) {
    auto errorLabels =
        getErrorLabels(opCtx, sessionOptions, commandName, code, wcCode, isInternalClient);
    commandBodyFieldsBob->appendElements(errorLabels);

    const auto isNotPrimaryError =
        (code && ErrorCodes::isA<ErrorCategory::NotPrimaryError>(*code)) ||
        (wcCode && ErrorCodes::isA<ErrorCategory::NotPrimaryError>(*wcCode));

    const auto isShutdownError = (code && ErrorCodes::isA<ErrorCategory::ShutdownError>(*code)) ||
        (wcCode && ErrorCodes::isA<ErrorCategory::ShutdownError>(*wcCode));

    const auto replCoord = repl::ReplicationCoordinator::get(opCtx);
    // NotPrimary errors always include a topologyVersion, since we increment topologyVersion on
    // stepdown. ShutdownErrors only include a topologyVersion if the server is in quiesce mode,
    // since we only increment the topologyVersion at shutdown and alert waiting isMaster commands
    // if the server enters quiesce mode.
    const auto shouldAppendTopologyVersion =
        (replCoord->getReplicationMode() == repl::ReplicationCoordinator::modeReplSet &&
         isNotPrimaryError) ||
        (isShutdownError && replCoord->inQuiesceMode());

    if (!shouldAppendTopologyVersion) {
        return;
    }

    const auto topologyVersion = replCoord->getTopologyVersion();
    BSONObjBuilder topologyVersionBuilder(commandBodyFieldsBob->subobjStart("topologyVersion"));
    topologyVersion.serialize(&topologyVersionBuilder);
}

class ExecCommandDatabase : public std::enable_shared_from_this<ExecCommandDatabase> {
public:
    explicit ExecCommandDatabase(std::shared_ptr<HandleRequest::ExecutionContext> execContext)
        : _execContext(std::move(execContext)) {}

    static Future<void> run(std::shared_ptr<HandleRequest::ExecutionContext> execContext) {
        return std::make_shared<ExecCommandDatabase>(std::move(execContext))->_makeFutureChain();
    }

    std::shared_ptr<HandleRequest::ExecutionContext> getExecutionContext() {
        return _execContext;
    }
    std::shared_ptr<CommandInvocation> getInvocation() {
        return _invocation;
    }
    const OperationSessionInfoFromClient& getSessionOptions() const {
        return _sessionOptions;
    }
    BSONObjBuilder* getExtraFieldsBuilder() {
        return &_extraFieldsBuilder;
    }
    const LogicalTime& getStartOperationTime() const {
        return _startOperationTime;
    }

private:
    // Returns a future that executes a command after stripping metadata, performing authorization
    // checks, handling audit impersonation, and (potentially) setting maintenance mode. The future
    // also checks that the command is permissible to run on the node given its current replication
    // state. All the logic here is independent of any particular command; any functionality
    // relevant to a specific command should be confined to its run() method.
    Future<void> _makeFutureChain() {
        return _parseCommand().then([this, anchor = shared_from_this()] {
            return _initiateCommand()
                .then([this] { return _commandExec(); })
                .onError([this, anchor = shared_from_this()](Status status) {
                    return _handleFailure(std::move(status));
                });
        });
    }

    Future<void> _parseCommand() {
        auto pf = makePromiseFuture<void>();
        auto future = std::move(pf.future).then([this, anchor = shared_from_this()] {
            auto opCtx = _execContext->getOpCtx();
            auto command = _execContext->getCommand();
            auto& request = _execContext->getRequest();

            CommandHelpers::uassertShouldAttemptParse(opCtx, command, request);
            _startOperationTime = getClientOperationTime(opCtx);

            _invocation = command->parse(opCtx, request);
            CommandInvocation::set(opCtx, _invocation);
        });
        pf.promise.emplaceValue();
        return future;
    }

    // Any logic, such as authorization and auditing, that must precede execution of the command.
    Future<void> _initiateCommand();

    // Returns the future chain that executes the parsed command against the database.
    Future<void> _commandExec();

    // Any error-handling logic that must be performed if the command initiation/execution fails.
    void _handleFailure(Status status);

    bool _isInternalClient() const {
        return _execContext->session() &&
            _execContext->session()->getTags() & transport::Session::kInternalClient;
    }

    const std::shared_ptr<HandleRequest::ExecutionContext> _execContext;

    // The following allows `_initiateCommand`, `_commandExec`, and `_handleFailure` to share
    // execution state without concerning the lifetime of these variables.
    BSONObjBuilder _extraFieldsBuilder;
    std::shared_ptr<CommandInvocation> _invocation;
    LogicalTime _startOperationTime;
    OperationSessionInfoFromClient _sessionOptions;
    std::unique_ptr<PolymorphicScoped> _scoped;
};

class RunCommandImpl : public std::enable_shared_from_this<RunCommandImpl> {
public:
    explicit RunCommandImpl(std::shared_ptr<ExecCommandDatabase> ecd)
        : _ecd(std::move(ecd)),
          _shouldCheckOutSession(
              _ecd->getSessionOptions().getTxnNumber() &&
              !shouldCommandSkipSessionCheckout(_ecd->getInvocation()->definition()->getName())),
          _shouldWaitForWriteConcern(_ecd->getInvocation()->supportsWriteConcern() ||
                                     _ecd->getInvocation()->definition()->getLogicalOp() ==
                                         LogicalOp::opGetMore) {}

    static Future<void> run(std::shared_ptr<ExecCommandDatabase> ecd) {
        return std::make_shared<RunCommandImpl>(std::move(ecd))->_makeFutureChain();
    }

private:
    Future<void> _makeFutureChain();

    // Anchor for references to attributes defined in `ExecCommandDatabase` (e.g., sessionOptions).
    const std::shared_ptr<ExecCommandDatabase> _ecd;

    // Any code that must run before command execution (e.g., reserving bytes for reply builder).
    Future<void> _prologue();

    // Runs the command without waiting for write concern
    Future<void> _runCommand();

    class RunCommandAndWaitForWriteConcern {
    public:
        explicit RunCommandAndWaitForWriteConcern(std::shared_ptr<RunCommandImpl> rci)
            : _rci(std::move(rci)),
              _execContext(_rci->_ecd->getExecutionContext()),
              _oldWriteConcern(_execContext->getOpCtx()->getWriteConcern()) {}

        ~RunCommandAndWaitForWriteConcern() {
            _execContext->getOpCtx()->setWriteConcern(_oldWriteConcern);
        }

        static Future<void> run(std::shared_ptr<RunCommandImpl>);

    private:
        void _waitForWriteConcern(BSONObjBuilder& bb);

        void _setup();
        Future<void> _run();
        Future<void> _onRunCompletion(Status);

        const std::shared_ptr<RunCommandImpl> _rci;
        const std::shared_ptr<HandleRequest::ExecutionContext> _execContext;

        // Allows changing the write concern while running the command and resetting on destruction.
        const WriteConcernOptions _oldWriteConcern;
        boost::optional<repl::OpTime> _lastOpBeforeRun;
        boost::optional<WriteConcernOptions> _extractedWriteConcern;
    };

    // Any code that must run after command execution -- returns true on successful execution.
    Future<bool> _epilogue();

    bool _isInternalClient() const {
        auto session = _ecd->getExecutionContext()->session();
        return session && session->getTags() & transport::Session::kInternalClient;
    }

    // Whether invoking the command requires a session to be checked out.
    const bool _shouldCheckOutSession;

    // getMore operations inherit a WriteConcern from their originating cursor. For example, if the
    // originating command was an aggregate with a $out and batchSize: 0. Note that if the command
    // only performed reads then we will not need to wait at all.
    const bool _shouldWaitForWriteConcern;
};

// Simplifies the interface for invoking commands and allows asynchronous execution of command
// invocations.
class InvokeCommand : public std::enable_shared_from_this<InvokeCommand> {
public:
    explicit InvokeCommand(std::shared_ptr<ExecCommandDatabase> ecd) : _ecd(std::move(ecd)) {}

    Future<void> run(const bool checkoutSession);

private:
    class SessionCheckoutPath;

    Future<void> _runInvocation() noexcept;

    const std::shared_ptr<ExecCommandDatabase> _ecd;
};

class InvokeCommand::SessionCheckoutPath
    : public std::enable_shared_from_this<InvokeCommand::SessionCheckoutPath> {
public:
    SessionCheckoutPath(std::shared_ptr<InvokeCommand> parent) : _parent(std::move(parent)) {}

    Future<void> run();

private:
    void _cleanupIncompleteTxn();

    Future<void> _checkOutSession();
    void _tapError(Status);
    Future<void> _commitInvocation();

    const std::shared_ptr<InvokeCommand> _parent;

    std::unique_ptr<MongoDOperationContextSession> _sessionTxnState;
    boost::optional<TransactionParticipant::Participant> _txnParticipant;
    boost::optional<ScopeGuard<std::function<void()>>> _guard;
};

Future<void> InvokeCommand::run(const bool checkoutSession) {
    auto [past, present] = makePromiseFuture<void>();
    auto future = std::move(present).then([this, checkoutSession, anchor = shared_from_this()] {
        if (checkoutSession)
            return std::make_shared<SessionCheckoutPath>(std::move(anchor))->run();
        return _runInvocation();
    });
    past.emplaceValue();
    return future;
}

Future<void> InvokeCommand::SessionCheckoutPath::run() {
    auto anchor = shared_from_this();
    return makeReadyFutureWith([] {})
        .then([this, anchor] { return _checkOutSession(); })
        .then([this, anchor] {
            return _parent->_runInvocation().tapError(
                [this, anchor](Status status) { return _tapError(std::move(status)); });
        })
        .then([this, anchor] { return _commitInvocation(); });
}

void InvokeCommand::SessionCheckoutPath::_cleanupIncompleteTxn() {
    auto opCtx = _parent->_ecd->getExecutionContext()->getOpCtx();
    const bool isPrepared = _txnParticipant->transactionIsPrepared();
    try {
        if (isPrepared)
            _txnParticipant->stashTransactionResources(opCtx);
        else if (_txnParticipant->transactionIsOpen())
            _txnParticipant->abortTransaction(opCtx);
    } catch (...) {
        // It is illegal for this to throw so we catch and log this here for diagnosability.
        LOGV2_FATAL_CONTINUE(21974,
                             "Caught exception during transaction {txnNumber} {operation} "
                             "{logicalSessionId}: {error}",
                             "Unable to stash/abort transaction",
                             "operation"_attr = (isPrepared ? "stash" : "abort"),
                             "txnNumber"_attr = opCtx->getTxnNumber(),
                             "logicalSessionId"_attr = opCtx->getLogicalSessionId()->toBSON(),
                             "error"_attr = exceptionToStatus());
        std::terminate();
    }
}

Future<void> InvokeCommand::SessionCheckoutPath::_checkOutSession() {
    auto ecd = _parent->_ecd;
    auto execContext = ecd->getExecutionContext();
    auto opCtx = execContext->getOpCtx();
    CommandInvocation* invocation = ecd->getInvocation().get();
    const OperationSessionInfoFromClient& sessionOptions = ecd->getSessionOptions();

    // This constructor will check out the session. It handles the appropriate state management
    // for both multi-statement transactions and retryable writes. Currently, only requests with
    // a transaction number will check out the session.
    _sessionTxnState = std::make_unique<MongoDOperationContextSession>(opCtx);
    _txnParticipant.emplace(TransactionParticipant::get(opCtx));

    if (!opCtx->getClient()->isInDirectClient()) {
        bool beganOrContinuedTxn{false};
        // This loop allows new transactions on a session to block behind a previous prepared
        // transaction on that session.
        while (!beganOrContinuedTxn) {
            try {
                _txnParticipant->beginOrContinue(opCtx,
                                                 *sessionOptions.getTxnNumber(),
                                                 sessionOptions.getAutocommit(),
                                                 sessionOptions.getStartTransaction());
                beganOrContinuedTxn = true;
            } catch (const ExceptionFor<ErrorCodes::PreparedTransactionInProgress>&) {
                auto prepareCompleted = _txnParticipant->onExitPrepare();

                CurOpFailpointHelpers::waitWhileFailPointEnabled(
                    &waitAfterNewStatementBlocksBehindPrepare,
                    opCtx,
                    "waitAfterNewStatementBlocksBehindPrepare");

                // Check the session back in and wait for ongoing prepared transaction to complete.
                MongoDOperationContextSession::checkIn(opCtx);
                prepareCompleted.wait(opCtx);
                // Wait for the prepared commit or abort oplog entry to be visible in the oplog.
                // This will prevent a new transaction from missing the transaction table update for
                // the previous prepared commit or abort due to an oplog hole.
                auto storageInterface = repl::StorageInterface::get(opCtx);
                storageInterface->waitForAllEarlierOplogWritesToBeVisible(opCtx);
                MongoDOperationContextSession::checkOut(opCtx);
            }
        }

        // Create coordinator if needed. If "startTransaction" is present, it must be true.
        if (sessionOptions.getStartTransaction()) {
            // If this shard has been selected as the coordinator, set up the coordinator state
            // to be ready to receive votes.
            if (sessionOptions.getCoordinator() == boost::optional<bool>(true)) {
                createTransactionCoordinator(opCtx, *sessionOptions.getTxnNumber());
            }
        }

        // Release the transaction lock resources and abort storage transaction for unprepared
        // transactions on failure to unstash the transaction resources to opCtx. We don't want to
        // have this error guard for beginOrContinue as it can abort the transaction for any
        // accidental invalid statements in the transaction.
        auto abortOnError = makeGuard([&] {
            if (_txnParticipant->transactionIsInProgress()) {
                _txnParticipant->abortTransaction(opCtx);
            }
        });

        _txnParticipant->unstashTransactionResources(opCtx, invocation->definition()->getName());

        // Unstash success.
        abortOnError.dismiss();
    }

    _guard.emplace([this] { _cleanupIncompleteTxn(); });

    if (!opCtx->getClient()->isInDirectClient()) {
        const auto& readConcernArgs = repl::ReadConcernArgs::get(opCtx);

        auto command = invocation->definition();
        // Record readConcern usages for commands run inside transactions after unstashing the
        // transaction resources.
        if (command->shouldAffectReadConcernCounter() && opCtx->inMultiDocumentTransaction()) {
            ServerReadConcernMetrics::get(opCtx)->recordReadConcern(readConcernArgs,
                                                                    true /* isTransaction */);
        }

        // For replica sets, we do not receive the readConcernArgs of our parent transaction
        // statements until we unstash the transaction resources. The below check is necessary to
        // ensure commands, including those occurring after the first statement in their respective
        // transactions, are checked for readConcern support. Presently, only `create` and
        // `createIndexes` do not support readConcern inside transactions.
        // TODO(SERVER-46971): Consider how to extend this check to other commands.
        auto cmdName = command->getName();
        auto readConcernSupport = invocation->supportsReadConcern(readConcernArgs.getLevel());
        if (readConcernArgs.hasLevel() &&
            (cmdName == "create"_sd || cmdName == "createIndexes"_sd)) {
            if (!readConcernSupport.readConcernSupport.isOK()) {
                uassertStatusOK(readConcernSupport.readConcernSupport.withContext(
                    "Command {} does not support this transaction's {}"_format(
                        cmdName, readConcernArgs.toString())));
            }
        }
    }

    // Use the API parameters that were stored when the transaction was initiated.
    APIParameters::get(opCtx) = _txnParticipant->getAPIParameters(opCtx);

    return Status::OK();
}

Future<void> InvokeCommand::_runInvocation() noexcept {
    auto execContext = _ecd->getExecutionContext();
    return tenant_migration_donor::migrationConflictHandler(
        execContext, [execContext, invocation = _ecd->getInvocation()] {
            return CommandHelpers::runCommandInvocationAsync(std::move(execContext),
                                                             std::move(invocation));
        });
}

void InvokeCommand::SessionCheckoutPath::_tapError(Status status) {
    auto opCtx = _parent->_ecd->getExecutionContext()->getOpCtx();
    const OperationSessionInfoFromClient& sessionOptions = _parent->_ecd->getSessionOptions();
    if (status.code() == ErrorCodes::CommandOnShardedViewNotSupportedOnMongod) {
        // Exceptions are used to resolve views in a sharded cluster, so they should be handled
        // specially to avoid unnecessary aborts.

        // If "startTransaction" is present, it must be true.
        if (sessionOptions.getStartTransaction()) {
            // If the first command a shard receives in a transactions fails with this code, the
            // shard may not be included in the final participant list if the router's retry after
            // resolving the view does not re-target it, which is possible if the underlying
            // collection is sharded. The shard's transaction should be preemptively aborted to
            // avoid leaving it orphaned in this case, which is fine even if it is re-targeted
            // because the retry will include "startTransaction" again and "restart" a transaction
            // at the active txnNumber.
            return;
        }

        // If this shard has completed an earlier statement for this transaction, it must already be
        // in the transaction's participant list, so it is guaranteed to learn its outcome.
        _txnParticipant->stashTransactionResources(opCtx);
        _guard->dismiss();
    } else if (status.code() == ErrorCodes::WouldChangeOwningShard) {
        _txnParticipant->stashTransactionResources(opCtx);
        _txnParticipant->resetRetryableWriteState(opCtx);
        _guard->dismiss();
    }
}

Future<void> InvokeCommand::SessionCheckoutPath::_commitInvocation() {
    auto execContext = _parent->_ecd->getExecutionContext();
    auto replyBuilder = execContext->getReplyBuilder();
    if (auto okField = replyBuilder->getBodyBuilder().asTempObj()["ok"]) {
        // If ok is present, use its truthiness.
        if (!okField.trueValue()) {
            return Status::OK();
        }
    }

    // Stash or commit the transaction when the command succeeds.
    _txnParticipant->stashTransactionResources(execContext->getOpCtx());
    _guard->dismiss();

    if (serverGlobalParams.clusterRole == ClusterRole::ShardServer ||
        serverGlobalParams.clusterRole == ClusterRole::ConfigServer) {
        auto txnResponseMetadata = _txnParticipant->getResponseMetadata();
        auto bodyBuilder = replyBuilder->getBodyBuilder();
        txnResponseMetadata.serialize(&bodyBuilder);
    }
    return Status::OK();
}

Future<void> RunCommandImpl::_prologue() try {
    auto execContext = _ecd->getExecutionContext();
    auto opCtx = execContext->getOpCtx();
    const Command* command = _ecd->getInvocation()->definition();
    auto bytesToReserve = command->reserveBytesForReply();
// SERVER-22100: In Windows DEBUG builds, the CRT heap debugging overhead, in conjunction with the
// additional memory pressure introduced by reply buffer pre-allocation, causes the concurrency
// suite to run extremely slowly. As a workaround we do not pre-allocate in Windows DEBUG builds.
#ifdef _WIN32
    if (kDebugBuild)
        bytesToReserve = 0;
#endif
    execContext->getReplyBuilder()->reserveBytes(bytesToReserve);

    // Record readConcern usages for commands run outside of transactions, excluding DBDirectClient.
    // For commands inside a transaction, they inherit the readConcern from the transaction. So we
    // will record their readConcern usages after we have unstashed the transaction resources.
    if (!opCtx->getClient()->isInDirectClient() && command->shouldAffectReadConcernCounter() &&
        !opCtx->inMultiDocumentTransaction()) {
        ServerReadConcernMetrics::get(opCtx)->recordReadConcern(repl::ReadConcernArgs::get(opCtx),
                                                                false /* isTransaction */);
    }
    return Status::OK();
} catch (const DBException& ex) {
    return ex.toStatus();
}

Future<bool> RunCommandImpl::_epilogue() {
    auto execContext = _ecd->getExecutionContext();
    auto opCtx = execContext->getOpCtx();
    auto& request = execContext->getRequest();
    auto command = execContext->getCommand();
    auto replyBuilder = execContext->getReplyBuilder();
    auto& behaviors = *execContext->behaviors;

    // This fail point blocks all commands which are running on the specified namespace, or which
    // are present in the given list of commands.If no namespace or command list are provided,then
    // the failpoint will block all commands.
    waitAfterCommandFinishesExecution.executeIf(
        [&](const BSONObj& data) {
            CurOpFailpointHelpers::waitWhileFailPointEnabled(
                &waitAfterCommandFinishesExecution, opCtx, "waitAfterCommandFinishesExecution");
        },
        [&](const BSONObj& data) {
            auto ns = data["ns"].valueStringDataSafe();
            auto commands =
                data.hasField("commands") ? data["commands"].Array() : std::vector<BSONElement>();

            // If 'ns' or 'commands' is not set, block for all the namespaces or commands
            // respectively.
            return (ns.empty() || _ecd->getInvocation()->ns().ns() == ns) &&
                (commands.empty() ||
                 std::any_of(commands.begin(), commands.end(), [&request](auto& element) {
                     return element.valueStringDataSafe() == request.getCommandName();
                 }));
        });

    behaviors.waitForLinearizableReadConcern(opCtx);
    tenant_migration_donor::checkIfLinearizableReadWasAllowedOrThrow(opCtx, request.getDatabase());

    // Wait for data to satisfy the read concern level, if necessary.
    behaviors.waitForSpeculativeMajorityReadConcern(opCtx);

    const bool ok = [&] {
        auto body = replyBuilder->getBodyBuilder();
        return CommandHelpers::extractOrAppendOk(body);
    }();
    behaviors.attachCurOpErrInfo(opCtx, replyBuilder->getBodyBuilder().asTempObj());

    {
        boost::optional<ErrorCodes::Error> code;
        boost::optional<ErrorCodes::Error> wcCode;
        auto body = replyBuilder->getBodyBuilder();
        auto response = body.asTempObj();
        auto codeField = response["code"];
        if (!ok && codeField.isNumber()) {
            code = ErrorCodes::Error(codeField.numberInt());
        }
        if (response.hasField("writeConcernError")) {
            wcCode = ErrorCodes::Error(response["writeConcernError"]["code"].numberInt());
        }
        appendErrorLabelsAndTopologyVersion(opCtx,
                                            &body,
                                            _ecd->getSessionOptions(),
                                            command->getName(),
                                            code,
                                            wcCode,
                                            _isInternalClient());
    }

    auto commandBodyBob = replyBuilder->getBodyBuilder();
    behaviors.appendReplyMetadata(opCtx, request, &commandBodyBob);
    appendClusterAndOperationTime(
        opCtx, &commandBodyBob, &commandBodyBob, _ecd->getStartOperationTime());
    return ok;
}

Future<void> RunCommandImpl::_runCommand() {
    auto execContext = _ecd->getExecutionContext();
    invariant(!_shouldWaitForWriteConcern);
    execContext->behaviors->uassertCommandDoesNotSpecifyWriteConcern(
        execContext->getRequest().body);
    return std::make_shared<InvokeCommand>(_ecd)->run(_shouldCheckOutSession);
}

void RunCommandImpl::RunCommandAndWaitForWriteConcern::_waitForWriteConcern(BSONObjBuilder& bb) {
    auto invocation = _rci->_ecd->getInvocation().get();
    auto opCtx = _execContext->getOpCtx();
    if (auto scoped = failCommand.scopedIf([&](const BSONObj& obj) {
            return CommandHelpers::shouldActivateFailCommandFailPoint(
                       obj, invocation, opCtx->getClient()) &&
                obj.hasField("writeConcernError");
        });
        MONGO_unlikely(scoped.isActive())) {
        const BSONObj& data = scoped.getData();
        bb.append(data["writeConcernError"]);
        if (data.hasField(kErrorLabelsFieldName) && data[kErrorLabelsFieldName].type() == Array) {
            // Propagate error labels specified in the failCommand failpoint to the
            // OperationContext decoration to override getErrorLabels() behaviors.
            invariant(!errorLabelsOverride(opCtx));
            errorLabelsOverride(opCtx).emplace(
                data.getObjectField(kErrorLabelsFieldName).getOwned());
        }
        return;
    }

    CurOp::get(opCtx)->debug().writeConcern.emplace(opCtx->getWriteConcern());
    _execContext->behaviors->waitForWriteConcern(opCtx, invocation, _lastOpBeforeRun.get(), bb);
}

Future<void> RunCommandImpl::RunCommandAndWaitForWriteConcern::run(
    std::shared_ptr<RunCommandImpl> rci) {
    auto instance = std::make_shared<RunCommandAndWaitForWriteConcern>(std::move(rci));
    // `_setup()` runs inline as part of preparing the future-chain, which will run the command and
    // waits for write concern, and may throw.
    instance->_setup();
    auto pf = makePromiseFuture<void>();
    auto future = std::move(pf.future)
                      .then([instance] { return instance->_run(); })
                      .onCompletion([instance](Status status) {
                          return instance->_onRunCompletion(std::move(status));
                      });
    pf.promise.emplaceValue();
    return future;
}

void RunCommandImpl::RunCommandAndWaitForWriteConcern::_setup() {
    auto invocation = _rci->_ecd->getInvocation();
    OperationContext* opCtx = _execContext->getOpCtx();
    const Command* command = invocation->definition();
    const OpMsgRequest& request = _execContext->getRequest();

    _lastOpBeforeRun.emplace(repl::ReplClientInfo::forClient(opCtx->getClient()).getLastOp());

    if (command->getLogicalOp() == LogicalOp::opGetMore) {
        // WriteConcern will be set up during command processing, it must not be specified on
        // the command body.
        _execContext->behaviors->uassertCommandDoesNotSpecifyWriteConcern(request.body);
    } else {
        // WriteConcern should always be explicitly specified by operations received on shard
        // and config servers, even if it is empty (ie. writeConcern: {}).  In this context
        // (shard/config servers) an empty WC indicates the operation should use the implicit
        // server defaults.  So, warn if the operation has not specified writeConcern and is on
        // a shard/config server.
        if (!opCtx->getClient()->isInDirectClient() &&
            (!opCtx->inMultiDocumentTransaction() || isTransactionCommand(command->getName()))) {
            if (_rci->_isInternalClient()) {
                // WriteConcern should always be explicitly specified by operations received
                // from internal clients (ie. from a mongos or mongod), even if it is empty
                // (ie. writeConcern: {}, which is equivalent to { w: 1, wtimeout: 0 }).
                uassert(
                    4569201,
                    "received command without explicit writeConcern on an internalClient connection {}"_format(
                        redact(request.body.toString())),
                    request.body.hasField(WriteConcernOptions::kWriteConcernField));
            } else if (serverGlobalParams.clusterRole == ClusterRole::ShardServer ||
                       serverGlobalParams.clusterRole == ClusterRole::ConfigServer) {
                if (!request.body.hasField(WriteConcernOptions::kWriteConcernField)) {
                    // TODO: Disabled until after SERVER-44539, to avoid log spam.
                    // LOGV2(21959, "Missing writeConcern on {command}", "Missing "
                    // "writeConcern on command", "command"_attr = command->getName());
                }
            }
        }
        _extractedWriteConcern.emplace(
            uassertStatusOK(extractWriteConcern(opCtx, request.body, _rci->_isInternalClient())));
        if (_rci->_ecd->getSessionOptions().getAutocommit()) {
            validateWriteConcernForTransaction(*_extractedWriteConcern,
                                               invocation->definition()->getName());
        }

        // Ensure that the WC being set on the opCtx has provenance.
        invariant(_extractedWriteConcern->getProvenance().hasSource(),
                  fmt::format("unexpected unset provenance on writeConcern: {}",
                              _extractedWriteConcern->toBSON().jsonString()));

        opCtx->setWriteConcern(*_extractedWriteConcern);
    }
}

Future<void> RunCommandImpl::RunCommandAndWaitForWriteConcern::_run() {
    if (auto scoped = failWithErrorCodeInRunCommand.scoped(); MONGO_unlikely(scoped.isActive())) {
        const auto errorCode = scoped.getData()["errorCode"].numberInt();
        LOGV2(21960,
              "failWithErrorCodeInRunCommand enabled - failing command with error "
              "code: {errorCode}",
              "failWithErrorCodeInRunCommand enabled, failing command",
              "errorCode"_attr = errorCode);
        BSONObjBuilder errorBuilder;
        errorBuilder.append("ok", 0.0);
        errorBuilder.append("code", errorCode);
        errorBuilder.append("errmsg", "failWithErrorCodeInRunCommand enabled.");
        _execContext->getReplyBuilder()->setCommandReply(errorBuilder.obj());
        return Status::OK();
    }
    return std::make_shared<InvokeCommand>(_rci->_ecd)->run(_rci->_shouldCheckOutSession);
}

Future<void> RunCommandImpl::RunCommandAndWaitForWriteConcern::_onRunCompletion(Status status) {
    auto opCtx = _execContext->getOpCtx();
    if (!status.isOK()) {
        // Do no-op write before returning NoSuchTransaction if command has writeConcern.
        if (status.code() == ErrorCodes::NoSuchTransaction &&
            !opCtx->getWriteConcern().usedDefault) {
            TransactionParticipant::performNoopWrite(opCtx, "NoSuchTransaction error");
        }
        _waitForWriteConcern(*_rci->_ecd->getExtraFieldsBuilder());
        return status;
    }

    auto bb = _execContext->getReplyBuilder()->getBodyBuilder();
    _waitForWriteConcern(bb);

    // With the exception of getMores inheriting the WriteConcern from the originating command,
    // nothing in run() should change the writeConcern.
    if (_execContext->getCommand()->getLogicalOp() == LogicalOp::opGetMore) {
        dassert(!_extractedWriteConcern, "opGetMore contained unexpected extracted write concern");
    } else {
        dassert(_extractedWriteConcern, "no extracted write concern");
        dassert(
            opCtx->getWriteConcern() == _extractedWriteConcern,
            "opCtx wc: {} extracted wc: {}"_format(opCtx->getWriteConcern().toBSON().jsonString(),
                                                   _extractedWriteConcern->toBSON().jsonString()));
    }
    return status;
}

Future<void> RunCommandImpl::_makeFutureChain() {
    return _prologue()
        .then([this] {
            if (_shouldWaitForWriteConcern)
                return RunCommandAndWaitForWriteConcern::run(shared_from_this());
            else
                return _runCommand();
        })
        .then([this] { return _epilogue(); })
        .onCompletion(
            [this, anchor = shared_from_this()](StatusWith<bool> ranSuccessfully) -> Future<void> {
                // Failure to run a command is either indicated by throwing an exception or adding a
                // non-okay field to the replyBuilder. The input argument (i.e., `ranSuccessfully`)
                // captures both cases. On success, it holds an okay status and a `true` value.
                auto status = ranSuccessfully.getStatus();
                if (status.isOK() && ranSuccessfully.getValue())
                    return Status::OK();

                auto execContext = _ecd->getExecutionContext();
                execContext->getCommand()->incrementCommandsFailed();
                if (status.code() == ErrorCodes::Unauthorized) {
                    CommandHelpers::auditLogAuthEvent(execContext->getOpCtx(),
                                                      _ecd->getInvocation().get(),
                                                      execContext->getRequest(),
                                                      status.code());
                }
                return status;
            });
}

Future<void> ExecCommandDatabase::_initiateCommand() try {
    auto opCtx = _execContext->getOpCtx();
    auto& request = _execContext->getRequest();
    auto command = _execContext->getCommand();
    auto replyBuilder = _execContext->getReplyBuilder();

    const auto apiParamsFromClient = initializeAPIParameters(opCtx, request.body, command);
    Client* client = opCtx->getClient();

    {
        stdx::lock_guard<Client> lk(*client);
        CurOp::get(opCtx)->setCommand_inlock(command);
        APIParameters::get(opCtx) = APIParameters::fromClient(apiParamsFromClient);
    }

    auto& apiParams = APIParameters::get(opCtx);
    auto& apiVersionMetrics = APIVersionMetrics::get(opCtx->getServiceContext());
    const auto& clientMetadata = ClientMetadataIsMasterState::get(client).getClientMetadata();
    if (clientMetadata) {
        auto appName = clientMetadata.get().getApplicationName().toString();
        apiVersionMetrics.update(appName, apiParams);
    }

    sleepMillisAfterCommandExecutionBegins.execute([&](const BSONObj& data) {
        auto numMillis = data["millis"].numberInt();
        auto commands = data["commands"].Obj().getFieldNames<std::set<std::string>>();
        // Only sleep for one of the specified commands.
        if (commands.find(command->getName()) != commands.end()) {
            mongo::sleepmillis(numMillis);
        }
    });

    rpc::readRequestMetadata(opCtx, request.body, command->requiresAuth());
    rpc::TrackingMetadata::get(opCtx).initWithOperName(command->getName());

    auto const replCoord = repl::ReplicationCoordinator::get(opCtx);

    _sessionOptions = initializeOperationSessionInfo(opCtx,
                                                     request.body,
                                                     command->requiresAuth(),
                                                     command->attachLogicalSessionsToOpCtx(),
                                                     replCoord->getReplicationMode() ==
                                                         repl::ReplicationCoordinator::modeReplSet);

    CommandHelpers::evaluateFailCommandFailPoint(opCtx, _invocation.get());

    const auto dbname = request.getDatabase().toString();
    uassert(ErrorCodes::InvalidNamespace,
            fmt::format("Invalid database name: '{}'", dbname),
            NamespaceString::validDBName(dbname, NamespaceString::DollarInDbNameBehavior::Allow));


    const auto allowTransactionsOnConfigDatabase =
        (serverGlobalParams.clusterRole == ClusterRole::ConfigServer ||
         serverGlobalParams.clusterRole == ClusterRole::ShardServer);

    validateSessionOptions(
        _sessionOptions, command->getName(), _invocation->ns(), allowTransactionsOnConfigDatabase);

    std::unique_ptr<MaintenanceModeSetter> mmSetter;

    BSONElement cmdOptionMaxTimeMSField;
    BSONElement maxTimeMSOpOnlyField;
    BSONElement allowImplicitCollectionCreationField;
    BSONElement helpField;

    StringMap<int> topLevelFields;
    for (auto&& element : request.body) {
        StringData fieldName = element.fieldNameStringData();
        if (fieldName == QueryRequest::cmdOptionMaxTimeMS) {
            cmdOptionMaxTimeMSField = element;
        } else if (fieldName == QueryRequest::kMaxTimeMSOpOnlyField) {
            uassert(ErrorCodes::InvalidOptions,
                    "Can not specify maxTimeMSOpOnly for non internal clients",
                    _isInternalClient());
            maxTimeMSOpOnlyField = element;
        } else if (fieldName == "allowImplicitCollectionCreation") {
            allowImplicitCollectionCreationField = element;
        } else if (fieldName == CommandHelpers::kHelpFieldName) {
            helpField = element;
        } else if (fieldName == "comment") {
            opCtx->setComment(element.wrap());
        } else if (fieldName == QueryRequest::queryOptionMaxTimeMS) {
            uasserted(ErrorCodes::InvalidOptions,
                      "no such command option $maxTimeMs; use maxTimeMS instead");
        }

        uassert(ErrorCodes::FailedToParse,
                str::stream() << "Parsed command object contains duplicate top level key: "
                              << fieldName,
                topLevelFields[fieldName]++ == 0);
    }

    if (CommandHelpers::isHelpRequest(helpField)) {
        CurOp::get(opCtx)->ensureStarted();
        // We disable last-error for help requests due to SERVER-11492, because config servers use
        // help requests to determine which commands are database writes, and so must be forwarded
        // to all config servers.
        LastError::get(opCtx->getClient()).disable();
        Command::generateHelpResponse(opCtx, replyBuilder, *command);
        return Status(ErrorCodes::SkipCommandExecution,
                      "Skipping command execution for help request");
    }

    ImpersonationSessionGuard guard(opCtx);
    _invocation->checkAuthorization(opCtx, request);

    const bool iAmPrimary = replCoord->canAcceptWritesForDatabase_UNSAFE(opCtx, dbname);

    if (!opCtx->getClient()->isInDirectClient() &&
        !MONGO_unlikely(skipCheckingForNotMasterInCommandDispatch.shouldFail())) {
        const bool inMultiDocumentTransaction = (_sessionOptions.getAutocommit() == false);
        auto allowed = command->secondaryAllowed(opCtx->getServiceContext());
        bool alwaysAllowed = allowed == Command::AllowedOnSecondary::kAlways;
        bool couldHaveOptedIn =
            allowed == Command::AllowedOnSecondary::kOptIn && !inMultiDocumentTransaction;
        bool optedIn = couldHaveOptedIn && ReadPreferenceSetting::get(opCtx).canRunOnSecondary();
        bool canRunHere = commandCanRunHere(opCtx, dbname, command, inMultiDocumentTransaction);
        if (!canRunHere && couldHaveOptedIn) {
            uasserted(ErrorCodes::NotPrimaryNoSecondaryOk, "not master and slaveOk=false");
        }

        if (MONGO_unlikely(respondWithNotPrimaryInCommandDispatch.shouldFail())) {
            uassert(ErrorCodes::NotWritablePrimary, "not primary", canRunHere);
        } else {
            uassert(ErrorCodes::NotWritablePrimary, "not master", canRunHere);
        }

        if (!command->maintenanceOk() &&
            replCoord->getReplicationMode() == repl::ReplicationCoordinator::modeReplSet &&
            !replCoord->canAcceptWritesForDatabase_UNSAFE(opCtx, dbname) &&
            !replCoord->getMemberState().secondary()) {

            uassert(ErrorCodes::NotPrimaryOrSecondary,
                    "node is recovering",
                    !replCoord->getMemberState().recovering());
            uassert(ErrorCodes::NotPrimaryOrSecondary,
                    "node is not in primary or recovering state",
                    replCoord->getMemberState().primary());
            // Check ticket SERVER-21432, slaveOk commands are allowed in drain mode
            uassert(ErrorCodes::NotPrimaryOrSecondary,
                    "node is in drain mode",
                    optedIn || alwaysAllowed);
        }
    }

    if (command->adminOnly()) {
        LOGV2_DEBUG(21961,
                    2,
                    "Admin only command: {command}",
                    "Admin only command",
                    "command"_attr = request.getCommandName());
    }

    if (command->maintenanceMode()) {
        mmSetter.reset(new MaintenanceModeSetter(opCtx));
    }

    if (command->shouldAffectCommandCounter()) {
        OpCounters* opCounters = &globalOpCounters;
        opCounters->gotCommand();
    }

    // Parse the 'maxTimeMS' command option, and use it to set a deadline for the operation on the
    // OperationContext. The 'maxTimeMS' option unfortunately has a different meaning for a getMore
    // command, where it is used to communicate the maximum time to wait for new inserts on tailable
    // cursors, not as a deadline for the operation.
    // TODO SERVER-34277 Remove the special handling for maxTimeMS for getMores. This will require
    // introducing a new 'max await time' parameter for getMore, and eventually banning maxTimeMS
    // altogether on a getMore command.
    int maxTimeMS = uassertStatusOK(QueryRequest::parseMaxTimeMS(cmdOptionMaxTimeMSField));
    int maxTimeMSOpOnly = uassertStatusOK(QueryRequest::parseMaxTimeMS(maxTimeMSOpOnlyField));

    // The "hello" command should not inherit the deadline from the user op it is operating as a
    // part of as that can interfere with replica set monitoring and host selection.
    bool ignoreMaxTimeMSOpOnly = command->getName() == "hello"_sd;

    if ((maxTimeMS > 0 || maxTimeMSOpOnly > 0) && command->getLogicalOp() != LogicalOp::opGetMore) {
        uassert(40119,
                "Illegal attempt to set operation deadline within DBDirectClient",
                !opCtx->getClient()->isInDirectClient());
        if (!ignoreMaxTimeMSOpOnly && maxTimeMSOpOnly > 0 &&
            (maxTimeMS == 0 || maxTimeMSOpOnly < maxTimeMS)) {
            opCtx->storeMaxTimeMS(Milliseconds{maxTimeMS});
            opCtx->setDeadlineAfterNowBy(Milliseconds{maxTimeMSOpOnly},
                                         ErrorCodes::MaxTimeMSExpired);
        } else if (maxTimeMS > 0) {
            opCtx->setDeadlineAfterNowBy(Milliseconds{maxTimeMS}, ErrorCodes::MaxTimeMSExpired);
        }
    }

    auto& readConcernArgs = repl::ReadConcernArgs::get(opCtx);

    // If the parent operation runs in a transaction, we don't override the read concern.
    auto skipReadConcern =
        opCtx->getClient()->isInDirectClient() && opCtx->inMultiDocumentTransaction();
    bool startTransaction = static_cast<bool>(_sessionOptions.getStartTransaction());
    if (!skipReadConcern) {
        auto newReadConcernArgs = uassertStatusOK(_extractReadConcern(
            opCtx, _invocation.get(), request.body, startTransaction, _isInternalClient()));

        // Ensure that the RC being set on the opCtx has provenance.
        invariant(newReadConcernArgs.getProvenance().hasSource(),
                  str::stream() << "unexpected unset provenance on readConcern: "
                                << newReadConcernArgs.toBSONInner());

        uassert(ErrorCodes::InvalidOptions,
                "Only the first command in a transaction may specify a readConcern",
                startTransaction || !opCtx->inMultiDocumentTransaction() ||
                    newReadConcernArgs.isEmpty());

        {
            // We must obtain the client lock to set the ReadConcernArgs on the operation context as
            // it may be concurrently read by CurrentOp.
            stdx::lock_guard<Client> lk(*opCtx->getClient());
            readConcernArgs = std::move(newReadConcernArgs);
        }
    }

    if (startTransaction) {
        opCtx->lockState()->setSharedLocksShouldTwoPhaseLock(true);
        opCtx->lockState()->setShouldConflictWithSecondaryBatchApplication(false);
    }

    if (opCtx->inMultiDocumentTransaction() && !startTransaction) {
        uassert(4937700,
                "API parameters are only allowed in the first command of a multi-document "
                "transaction",
                !APIParameters::get(opCtx).getParamsPassed());
    }

    // Remember whether or not this operation is starting a transaction, in case something later in
    // the execution needs to adjust its behavior based on this.
    opCtx->setIsStartingMultiDocumentTransaction(startTransaction);

    auto& oss = OperationShardingState::get(opCtx);

    if (!opCtx->getClient()->isInDirectClient() &&
        readConcernArgs.getLevel() != repl::ReadConcernLevel::kAvailableReadConcern &&
        (iAmPrimary || (readConcernArgs.hasLevel() || readConcernArgs.getArgsAfterClusterTime()))) {
        oss.initializeClientRoutingVersionsFromCommand(_invocation->ns(), request.body);

        auto const shardingState = ShardingState::get(opCtx);
        if (OperationShardingState::isOperationVersioned(opCtx) || oss.hasDbVersion()) {
            uassertStatusOK(shardingState->canAcceptShardedCommands());
        }

        _execContext->behaviors->advanceConfigOpTimeFromRequestMetadata(opCtx);
    }

    oss.setAllowImplicitCollectionCreation(allowImplicitCollectionCreationField);
    _scoped = _execContext->behaviors->scopedOperationCompletionShardingActions(opCtx);

    // This may trigger the maxTimeAlwaysTimeOut failpoint.
    auto status = opCtx->checkForInterruptNoAssert();

    // We still proceed if the primary stepped down, but accept other kinds of interruptions. We
    // defer to individual commands to allow themselves to be interruptible by stepdowns, since
    // commands like 'voteRequest' should conversely continue executing.
    if (status != ErrorCodes::PrimarySteppedDown &&
        status != ErrorCodes::InterruptedDueToReplStateChange) {
        uassertStatusOK(status);
    }

    CurOp::get(opCtx)->ensureStarted();

    command->incrementCommandsExecuted();

    if (shouldLog(logv2::LogComponent::kTracking, logv2::LogSeverity::Debug(1)) &&
        rpc::TrackingMetadata::get(opCtx).getParentOperId()) {
        LOGV2_DEBUG_OPTIONS(4615605,
                            1,
                            {logv2::LogComponent::kTracking},
                            "Command metadata: {trackingMetadata}",
                            "Command metadata",
                            "trackingMetadata"_attr = rpc::TrackingMetadata::get(opCtx));
        rpc::TrackingMetadata::get(opCtx).setIsLogged(true);
    }

    _execContext->behaviors->waitForReadConcern(opCtx, _invocation.get(), request);
    _execContext->behaviors->setPrepareConflictBehaviorForReadConcern(opCtx, _invocation.get());
    return Status::OK();
} catch (const DBException& ex) {
    return ex.toStatus();
}

Future<void> ExecCommandDatabase::_commandExec() {
    return RunCommandImpl::run(shared_from_this());
}

void ExecCommandDatabase::_handleFailure(Status status) {
    // Absorb the exception as the command execution has already been skipped.
    if (status.code() == ErrorCodes::SkipCommandExecution)
        return;

    auto opCtx = _execContext->getOpCtx();
    auto& request = _execContext->getRequest();
    auto command = _execContext->getCommand();
    auto replyBuilder = _execContext->getReplyBuilder();
    const auto& behaviors = *_execContext->behaviors;

    behaviors.handleException(status, opCtx);

    // Append the error labels for transient transaction errors.
    auto response = _extraFieldsBuilder.asTempObj();
    boost::optional<ErrorCodes::Error> wcCode;
    if (response.hasField("writeConcernError")) {
        wcCode = ErrorCodes::Error(response["writeConcernError"]["code"].numberInt());
    }
    appendErrorLabelsAndTopologyVersion(opCtx,
                                        &_extraFieldsBuilder,
                                        _sessionOptions,
                                        command->getName(),
                                        status.code(),
                                        wcCode,
                                        _isInternalClient());

    BSONObjBuilder metadataBob;
    behaviors.appendReplyMetadata(opCtx, request, &metadataBob);

    // The read concern may not have yet been placed on the operation context, so attempt to parse
    // it here, so if it is valid it can be used to compute the proper operationTime.
    auto& readConcernArgs = repl::ReadConcernArgs::get(opCtx);
    if (readConcernArgs.isEmpty()) {
        auto readConcernArgsStatus = _extractReadConcern(opCtx,
                                                         _invocation.get(),
                                                         request.body,
                                                         false /*startTransaction*/,
                                                         _isInternalClient());
        if (readConcernArgsStatus.isOK()) {
            // We must obtain the client lock to set the ReadConcernArgs on the operation context as
            // it may be concurrently read by CurrentOp.
            stdx::lock_guard<Client> lk(*opCtx->getClient());
            readConcernArgs = readConcernArgsStatus.getValue();
        }
    }
    appendClusterAndOperationTime(opCtx, &_extraFieldsBuilder, &metadataBob, _startOperationTime);

    LOGV2_DEBUG(21962,
                1,
                "Assertion while executing command '{command}' on database '{db}' with "
                "arguments '{commandArgs}': {error}",
                "Assertion while executing command",
                "command"_attr = request.getCommandName(),
                "db"_attr = request.getDatabase(),
                "commandArgs"_attr = redact(
                    ServiceEntryPointCommon::getRedactedCopyForLogging(command, request.body)),
                "error"_attr = redact(status.toString()));

    generateErrorResponse(
        opCtx, replyBuilder, status, metadataBob.obj(), _extraFieldsBuilder.obj());

    if (ErrorCodes::isA<ErrorCategory::CloseConnectionError>(status.code())) {
        // Rethrow the exception to the top to signal that the client connection should be closed.
        internalAssert(status);
    }
}

/**
 * Fills out CurOp / OpDebug with basic command info.
 */
void curOpCommandSetup(OperationContext* opCtx, const OpMsgRequest& request) {
    auto curop = CurOp::get(opCtx);
    curop->debug().iscommand = true;

    // We construct a legacy $cmd namespace so we can fill in curOp using
    // the existing logic that existed for OP_QUERY commands
    NamespaceString nss(request.getDatabase(), "$cmd");

    stdx::lock_guard<Client> lk(*opCtx->getClient());
    curop->setOpDescription_inlock(request.body);
    curop->markCommand_inlock();
    curop->setNS_inlock(nss.ns());
}

Future<void> parseCommand(std::shared_ptr<HandleRequest::ExecutionContext> execContext) try {
    execContext->setRequest(rpc::opMsgRequestFromAnyProtocol(execContext->getMessage()));
    return Status::OK();
} catch (const DBException& ex) {
    // Need to set request as `makeCommandResponse` expects an empty request on failure.
    execContext->setRequest({});

    // Otherwise, reply with the parse error. This is useful for cases where parsing fails due to
    // user-supplied input, such as the document too deep error. Since we failed during parsing, we
    // can't log anything about the command.
    LOGV2_DEBUG(21963,
                1,
                "Assertion while parsing command: {error}",
                "Assertion while parsing command",
                "error"_attr = ex.toString());

    return ex.toStatus();
}

Future<void> executeCommand(std::shared_ptr<HandleRequest::ExecutionContext> execContext) {
    auto [past, present] = makePromiseFuture<void>();
    auto future =
        std::move(present)
            .then([execContext]() -> Future<void> {
                // Prepare environment for command execution (e.g., find command object in registry)
                auto opCtx = execContext->getOpCtx();
                auto& request = execContext->getRequest();
                curOpCommandSetup(opCtx, request);

                // In the absence of a Command object, no redaction is possible. Therefore to avoid
                // displaying potentially sensitive information in the logs, we restrict the log
                // message to the name of the unrecognized command. However, the complete command
                // object will still be echoed to the client.
                if (execContext->setCommand(CommandHelpers::findCommand(request.getCommandName()));
                    !execContext->getCommand()) {
                    globalCommandRegistry()->incrementUnknownCommands();
                    LOGV2_DEBUG(21964,
                                2,
                                "No such command: {command}",
                                "Command not found in registry",
                                "command"_attr = request.getCommandName());
                    return Status(ErrorCodes::CommandNotFound,
                                  fmt::format("no such command: '{}'", request.getCommandName()));
                }

                Command* c = execContext->getCommand();
                LOGV2_DEBUG(
                    21965,
                    2,
                    "Run command {db}.$cmd {commandArgs}",
                    "About to run the command",
                    "db"_attr = request.getDatabase(),
                    "commandArgs"_attr = redact(
                        ServiceEntryPointCommon::getRedactedCopyForLogging(c, request.body)));

                {
                    // Try to set this as early as possible, as soon as we have figured out the
                    // command.
                    stdx::lock_guard<Client> lk(*opCtx->getClient());
                    CurOp::get(opCtx)->setLogicalOp_inlock(c->getLogicalOp());
                }

                opCtx->setExhaust(
                    OpMsg::isFlagSet(execContext->getMessage(), OpMsg::kExhaustSupported));

                const auto session = opCtx->getClient()->session();
                if (session) {
                    if (!opCtx->isExhaust() || c->getName() != "hello"_sd) {
                        InExhaustIsMaster::get(session.get())->setInExhaustIsMaster(false);
                    }
                }

                // Hello should take kMaxAwaitTimeMs at most, log if it takes twice that.
                if (c->getName() == "hello") {
                    execContext->slowMsOverride =
                        2 * durationCount<Milliseconds>(SingleServerIsMasterMonitor::kMaxAwaitTime);
                }

                return Status::OK();
            })
            .then([execContext] { return ExecCommandDatabase::run(std::move(execContext)); })
            .tapError([execContext](Status status) {
                LOGV2_DEBUG(
                    21966,
                    1,
                    "Assertion while executing command '{command}' on database '{db}': {error}",
                    "Assertion while executing command",
                    "command"_attr = execContext->getRequest().getCommandName(),
                    "db"_attr = execContext->getRequest().getDatabase(),
                    "error"_attr = status.toString());
            });
    past.emplaceValue();
    return future;
}

DbResponse makeCommandResponse(std::shared_ptr<HandleRequest::ExecutionContext> execContext) {
    auto opCtx = execContext->getOpCtx();
    const Message& message = execContext->getMessage();
    OpMsgRequest request = execContext->getRequest();
    const Command* c = execContext->getCommand();
    auto replyBuilder = execContext->getReplyBuilder();

    if (OpMsg::isFlagSet(message, OpMsg::kMoreToCome)) {
        // Close the connection to get client to go through server selection again.
        if (LastError::get(opCtx->getClient()).hadNotPrimaryError()) {
            if (c && c->getReadWriteType() == Command::ReadWriteType::kWrite)
                notPrimaryUnackWrites.increment();
            uasserted(ErrorCodes::NotWritablePrimary,
                      str::stream()
                          << "Not-master error while processing '" << request.getCommandName()
                          << "' operation  on '" << request.getDatabase() << "' database via "
                          << "fire-and-forget command execution.");
        }
        return {};  // Don't reply.
    }

    DbResponse dbResponse;

    if (OpMsg::isFlagSet(message, OpMsg::kExhaustSupported)) {
        auto responseObj = replyBuilder->getBodyBuilder().asTempObj();

        if (responseObj.getField("ok").trueValue()) {
            dbResponse.shouldRunAgainForExhaust = replyBuilder->shouldRunAgainForExhaust();
            dbResponse.nextInvocation = replyBuilder->getNextInvocation();
        }
    }

    dbResponse.response = replyBuilder->done();
    CurOp::get(opCtx)->debug().responseLength = dbResponse.response.header().dataLen();

    return dbResponse;
}

Future<DbResponse> receivedCommands(std::shared_ptr<HandleRequest::ExecutionContext> execContext) {
    execContext->setReplyBuilder(
        rpc::makeReplyBuilder(rpc::protocolForMessage(execContext->getMessage())));
    return parseCommand(execContext)
        .then([execContext]() { return executeCommand(std::move(execContext)); })
        .onError([execContext](Status status) {
            if (ErrorCodes::isConnectionFatalMessageParseError(status.code())) {
                // If this error needs to fail the connection, propagate it out.
                internalAssert(status);
            }

            auto opCtx = execContext->getOpCtx();
            BSONObjBuilder metadataBob;
            execContext->behaviors->appendReplyMetadataOnError(opCtx, &metadataBob);

            BSONObjBuilder extraFieldsBuilder;
            appendClusterAndOperationTime(
                opCtx, &extraFieldsBuilder, &metadataBob, LogicalTime::kUninitialized);

            auto replyBuilder = execContext->getReplyBuilder();
            generateErrorResponse(
                opCtx, replyBuilder, status, metadataBob.obj(), extraFieldsBuilder.obj());

            if (ErrorCodes::isA<ErrorCategory::CloseConnectionError>(status.code())) {
                // Return the exception to the top to signal that the client connection should be
                // closed.
                internalAssert(status);
            }
        })
        .then([execContext] { return makeCommandResponse(std::move(execContext)); });
}

DbResponse receivedQuery(OperationContext* opCtx,
                         const NamespaceString& nss,
                         Client& c,
                         const Message& m,
                         const ServiceEntryPointCommon::Hooks& behaviors) {
    invariant(!nss.isCommand());
    globalOpCounters.gotQuery();

    if (!opCtx->getClient()->isInDirectClient()) {
        ServerReadConcernMetrics::get(opCtx)->recordReadConcern(repl::ReadConcernArgs::get(opCtx),
                                                                false /* isTransaction */);
    }

    DbMessage d(m);
    QueryMessage q(d);

    CurOp& op = *CurOp::get(opCtx);
    DbResponse dbResponse;

    try {
        Client* client = opCtx->getClient();
        Status status = AuthorizationSession::get(client)->checkAuthForFind(nss, false);
        audit::logQueryAuthzCheck(client, nss, q.query, status.code());
        uassertStatusOK(status);

        dbResponse.shouldRunAgainForExhaust = runQuery(opCtx, q, nss, dbResponse.response);
    } catch (const AssertionException& e) {
        behaviors.handleException(e.toStatus(), opCtx);

        dbResponse.response.reset();
        generateLegacyQueryErrorResponse(e, q, &op, &dbResponse.response);
    }

    op.debug().responseLength = dbResponse.response.header().dataLen();
    return dbResponse;
}

void receivedKillCursors(OperationContext* opCtx, const Message& m) {
    LastError::get(opCtx->getClient()).disable();
    DbMessage dbmessage(m);
    int n = dbmessage.pullInt();

    static constexpr int kSoftKillLimit = 2000;
    static constexpr int kHardKillLimit = 29999;

    if (n > kHardKillLimit) {
        LOGV2_ERROR(4615607,
                    "Received killCursors, n={numCursors}",
                    "Received killCursors, exceeded kHardKillLimit",
                    "numCursors"_attr = n,
                    "kHardKillLimit"_attr = kHardKillLimit);
        uasserted(51250, "Must kill fewer than {} cursors"_format(kHardKillLimit));
    }

    if (n > kSoftKillLimit) {
        LOGV2_WARNING(4615606,
                      "Received killCursors, n={numCursors}",
                      "Received killCursors, exceeded kSoftKillLimit",
                      "numCursors"_attr = n,
                      "kSoftKillLimit"_attr = kSoftKillLimit);
    }

    uassert(31289, str::stream() << "must kill at least 1 cursor, n=" << n, n >= 1);
    massert(13658,
            str::stream() << "bad kill cursors size: " << m.dataSize(),
            m.dataSize() == 8 + (8 * n));

    const char* cursorArray = dbmessage.getArray(n);
    int found = runOpKillCursors(opCtx, static_cast<size_t>(n), cursorArray);

    if (shouldLog(logv2::LogSeverity::Debug(1)) || found != n) {
        LOGV2_DEBUG(21967,
                    found == n ? 1 : 0,
                    "killCursors: found {found} of {numCursors}",
                    "killCursors found fewer cursors to kill than requested",
                    "found"_attr = found,
                    "numCursors"_attr = n);
    }
}

void receivedInsert(OperationContext* opCtx, const NamespaceString& nsString, const Message& m) {
    auto insertOp = InsertOp::parseLegacy(m);
    invariant(insertOp.getNamespace() == nsString);

    for (const auto& obj : insertOp.getDocuments()) {
        Status status =
            AuthorizationSession::get(opCtx->getClient())->checkAuthForInsert(opCtx, nsString);
        audit::logInsertAuthzCheck(opCtx->getClient(), nsString, obj, status.code());
        uassertStatusOK(status);
    }
    write_ops_exec::performInserts(opCtx, insertOp);
}

void receivedUpdate(OperationContext* opCtx, const NamespaceString& nsString, const Message& m) {
    auto updateOp = UpdateOp::parseLegacy(m);
    auto& singleUpdate = updateOp.getUpdates()[0];
    invariant(updateOp.getNamespace() == nsString);

    Status status = AuthorizationSession::get(opCtx->getClient())
                        ->checkAuthForUpdate(opCtx,
                                             nsString,
                                             singleUpdate.getQ(),
                                             singleUpdate.getU(),
                                             singleUpdate.getUpsert());
    audit::logUpdateAuthzCheck(opCtx->getClient(),
                               nsString,
                               singleUpdate.getQ(),
                               singleUpdate.getU(),
                               singleUpdate.getUpsert(),
                               singleUpdate.getMulti(),
                               status.code());
    uassertStatusOK(status);

    write_ops_exec::performUpdates(opCtx, updateOp);
}

void receivedDelete(OperationContext* opCtx, const NamespaceString& nsString, const Message& m) {
    auto deleteOp = DeleteOp::parseLegacy(m);
    auto& singleDelete = deleteOp.getDeletes()[0];
    invariant(deleteOp.getNamespace() == nsString);

    Status status = AuthorizationSession::get(opCtx->getClient())
                        ->checkAuthForDelete(opCtx, nsString, singleDelete.getQ());
    audit::logDeleteAuthzCheck(opCtx->getClient(), nsString, singleDelete.getQ(), status.code());
    uassertStatusOK(status);

    write_ops_exec::performDeletes(opCtx, deleteOp);
}

DbResponse receivedGetMore(OperationContext* opCtx,
                           const Message& m,
                           CurOp& curop,
                           bool* shouldLogOpDebug) {
    globalOpCounters.gotGetMore();
    DbMessage d(m);

    const char* ns = d.getns();
    int ntoreturn = d.pullInt();
    uassert(
        34419, str::stream() << "Invalid ntoreturn for OP_GET_MORE: " << ntoreturn, ntoreturn >= 0);
    long long cursorid = d.pullInt64();

    curop.debug().ntoreturn = ntoreturn;
    curop.debug().cursorid = cursorid;

    {
        stdx::lock_guard<Client> lk(*opCtx->getClient());
        CurOp::get(opCtx)->setNS_inlock(ns);
    }

    bool exhaust = false;
    bool isCursorAuthorized = false;

    DbResponse dbresponse;
    try {
        const NamespaceString nsString(ns);
        uassert(ErrorCodes::InvalidNamespace,
                str::stream() << "Invalid ns [" << ns << "]",
                nsString.isValid());

        Status status = AuthorizationSession::get(opCtx->getClient())
                            ->checkAuthForGetMore(nsString, cursorid, false);
        audit::logGetMoreAuthzCheck(opCtx->getClient(), nsString, cursorid, status.code());
        uassertStatusOK(status);

        while (MONGO_unlikely(rsStopGetMore.shouldFail())) {
            sleepmillis(0);
        }

        dbresponse.response =
            getMore(opCtx, ns, ntoreturn, cursorid, &exhaust, &isCursorAuthorized);
    } catch (AssertionException& e) {
        if (isCursorAuthorized) {
            // Make sure that killCursorGlobal does not throw an exception if it is interrupted.
            UninterruptibleLockGuard noInterrupt(opCtx->lockState());

            // If an error was thrown prior to auth checks, then the cursor should remain alive
            // in order to prevent an unauthorized user from resulting in the death of a cursor.
            // In other error cases, the cursor is dead and should be cleaned up.
            //
            // If killing the cursor fails, ignore the error and don't try again. The cursor
            // should be reaped by the client cursor timeout thread.
            CursorManager::get(opCtx)
                ->killCursor(opCtx, cursorid, false /* shouldAudit */)
                .ignore();
        }

        BSONObjBuilder err;
        err.append("$err", e.reason());
        err.append("code", e.code());
        BSONObj errObj = err.obj();

        curop.debug().errInfo = e.toStatus();

        dbresponse = replyToQuery(errObj, ResultFlag_ErrSet);
        curop.debug().responseLength = dbresponse.response.header().dataLen();
        curop.debug().nreturned = 1;
        *shouldLogOpDebug = true;
        return dbresponse;
    }

    curop.debug().responseLength = dbresponse.response.header().dataLen();
    auto queryResult = QueryResult::ConstView(dbresponse.response.buf());
    curop.debug().nreturned = queryResult.getNReturned();

    if (exhaust) {
        curop.debug().exhaust = true;
        dbresponse.shouldRunAgainForExhaust = true;
    }

    return dbresponse;
}

struct CommandOpRunner : HandleRequest::OpRunner {
    using HandleRequest::OpRunner::OpRunner;
    Future<DbResponse> run() override {
        return receivedCommands(executionContext);
    }
};

// Allows wrapping synchronous code in futures without repeating the try-catch block.
struct SynchronousOpRunner : HandleRequest::OpRunner {
    using HandleRequest::OpRunner::OpRunner;
    virtual DbResponse runSync() = 0;
    Future<DbResponse> run() final try { return runSync(); } catch (const DBException& ex) {
        return ex.toStatus();
    }
};

struct QueryOpRunner : SynchronousOpRunner {
    using SynchronousOpRunner::SynchronousOpRunner;
    DbResponse runSync() override {
        auto opCtx = executionContext->getOpCtx();
        opCtx->markKillOnClientDisconnect();
        return receivedQuery(opCtx,
                             executionContext->nsString(),
                             executionContext->client(),
                             executionContext->getMessage(),
                             *executionContext->behaviors);
    }
};

struct GetMoreOpRunner : SynchronousOpRunner {
    using SynchronousOpRunner::SynchronousOpRunner;
    DbResponse runSync() override {
        return receivedGetMore(executionContext->getOpCtx(),
                               executionContext->getMessage(),
                               executionContext->currentOp(),
                               &executionContext->forceLog);
    }
};

/**
 * Fire and forget network operations don't produce a `DbResponse`.
 * They override `runAndForget` instead of `run`, and this base
 * class provides a `run` that calls it and handles error reporting
 * via the `LastError` slot.
 */
struct FireAndForgetOpRunner : SynchronousOpRunner {
    using SynchronousOpRunner::SynchronousOpRunner;
    virtual void runAndForget() = 0;
    DbResponse runSync() final;
};

struct KillCursorsOpRunner : FireAndForgetOpRunner {
    using FireAndForgetOpRunner::FireAndForgetOpRunner;
    void runAndForget() override {
        executionContext->currentOp().ensureStarted();
        executionContext->slowMsOverride = 10;
        receivedKillCursors(executionContext->getOpCtx(), executionContext->getMessage());
    }
};

struct InsertOpRunner : FireAndForgetOpRunner {
    using FireAndForgetOpRunner::FireAndForgetOpRunner;
    void runAndForget() override {
        executionContext->assertValidNsString();
        receivedInsert(executionContext->getOpCtx(),
                       executionContext->nsString(),
                       executionContext->getMessage());
    }
};

struct UpdateOpRunner : FireAndForgetOpRunner {
    using FireAndForgetOpRunner::FireAndForgetOpRunner;
    void runAndForget() override {
        executionContext->assertValidNsString();
        receivedUpdate(executionContext->getOpCtx(),
                       executionContext->nsString(),
                       executionContext->getMessage());
    }
};

struct DeleteOpRunner : FireAndForgetOpRunner {
    using FireAndForgetOpRunner::FireAndForgetOpRunner;
    void runAndForget() override {
        executionContext->assertValidNsString();
        receivedDelete(executionContext->getOpCtx(),
                       executionContext->nsString(),
                       executionContext->getMessage());
    }
};

struct UnsupportedOpRunner : SynchronousOpRunner {
    using SynchronousOpRunner::SynchronousOpRunner;
    DbResponse runSync() override {
        // For compatibility reasons, we only log incidents of receiving operations that are not
        // supported and return an empty response to the caller.
        LOGV2(21968,
              "Operation isn't supported: {operation}",
              "Operation is not supported",
              "operation"_attr = static_cast<int>(executionContext->op()));
        executionContext->currentOp().done();
        executionContext->forceLog = true;
        return {};
    }
};

std::unique_ptr<HandleRequest::OpRunner> HandleRequest::makeOpRunner() {
    switch (executionContext->op()) {
        case dbQuery:
            if (!executionContext->nsString().isCommand())
                return std::make_unique<QueryOpRunner>(this);
            // FALLTHROUGH: it's a query containing a command
        case dbMsg:
            return std::make_unique<CommandOpRunner>(this);
        case dbGetMore:
            return std::make_unique<GetMoreOpRunner>(this);
        case dbKillCursors:
            return std::make_unique<KillCursorsOpRunner>(this);
        case dbInsert:
            return std::make_unique<InsertOpRunner>(this);
        case dbUpdate:
            return std::make_unique<UpdateOpRunner>(this);
        case dbDelete:
            return std::make_unique<DeleteOpRunner>(this);
        default:
            return std::make_unique<UnsupportedOpRunner>(this);
    }
}

DbResponse FireAndForgetOpRunner::runSync() {
    try {
        runAndForget();
    } catch (const AssertionException& ue) {
        LastError::get(executionContext->client()).setLastError(ue.code(), ue.reason());
        LOGV2_DEBUG(21969,
                    3,
                    "Caught Assertion in {networkOp}, continuing: {error}",
                    "Assertion in fire-and-forget operation",
                    "networkOp"_attr = networkOpToString(executionContext->op()),
                    "error"_attr = redact(ue));
        executionContext->currentOp().debug().errInfo = ue.toStatus();
    }
    // A NotWritablePrimary error can be set either within
    // receivedInsert/receivedUpdate/receivedDelete or within the AssertionException handler above.
    // Either way, we want to throw an exception here, which will cause the client to be
    // disconnected.
    if (LastError::get(executionContext->client()).hadNotPrimaryError()) {
        notPrimaryLegacyUnackWrites.increment();
        uasserted(ErrorCodes::NotWritablePrimary,
                  str::stream() << "Not-master error while processing '"
                                << networkOpToString(executionContext->op()) << "' operation  on '"
                                << executionContext->nsString() << "' namespace via legacy "
                                << "fire-and-forget command execution.");
    }
    return {};
}

Future<void> HandleRequest::startOperation() try {
    auto opCtx = executionContext->getOpCtx();
    auto& client = executionContext->client();
    auto& currentOp = executionContext->currentOp();

    if (client.isInDirectClient()) {
        if (!opCtx->getLogicalSessionId() || !opCtx->getTxnNumber()) {
            invariant(!opCtx->inMultiDocumentTransaction() &&
                      !opCtx->lockState()->inAWriteUnitOfWork());
        }
    } else {
        LastError::get(client).startRequest();
        AuthorizationSession::get(client)->startRequest(opCtx);

        // We should not be holding any locks at this point
        invariant(!opCtx->lockState()->isLocked());
    }
    {
        stdx::lock_guard<Client> lk(client);
        // Commands handling code will reset this if the operation is a command
        // which is logically a basic CRUD operation like query, insert, etc.
        currentOp.setNetworkOp_inlock(executionContext->op());
        currentOp.setLogicalOp_inlock(networkOpToLogicalOp(executionContext->op()));
    }
    return {};
} catch (const DBException& ex) {
    return ex.toStatus();
}

Future<void> HandleRequest::completeOperation() try {
    auto opCtx = executionContext->getOpCtx();
    auto& currentOp = executionContext->currentOp();

    // Mark the op as complete, and log it if appropriate. Returns a boolean indicating whether
    // this op should be written to the profiler.
    const bool shouldProfile =
        currentOp.completeAndLogOperation(opCtx,
                                          MONGO_LOGV2_DEFAULT_COMPONENT,
                                          executionContext->getResponse().response.size(),
                                          executionContext->slowMsOverride,
                                          executionContext->forceLog);

    Top::get(opCtx->getServiceContext())
        .incrementGlobalLatencyStats(
            opCtx,
            durationCount<Microseconds>(currentOp.elapsedTimeExcludingPauses()),
            currentOp.getReadWriteType());

    if (shouldProfile) {
        // Performance profiling is on
        if (opCtx->lockState()->isReadLocked()) {
            LOGV2_DEBUG(21970, 1, "Note: not profiling because of recursive read lock");
        } else if (executionContext->client().isInDirectClient()) {
            LOGV2_DEBUG(21971, 1, "Note: not profiling because we are in DBDirectClient");
        } else if (executionContext->behaviors->lockedForWriting()) {
            // TODO SERVER-26825: Fix race condition where fsyncLock is acquired post
            // lockedForWriting() call but prior to profile collection lock acquisition.
            LOGV2_DEBUG(21972, 1, "Note: not profiling because doing fsync+lock");
        } else if (storageGlobalParams.readOnly) {
            LOGV2_DEBUG(21973, 1, "Note: not profiling because server is read-only");
        } else {
            invariant(!opCtx->lockState()->inAWriteUnitOfWork());
            profile(opCtx, executionContext->op());
        }
    }

    recordCurOpMetrics(opCtx);
    return {};
} catch (const DBException& ex) {
    return ex.toStatus();
}

}  // namespace

BSONObj ServiceEntryPointCommon::getRedactedCopyForLogging(const Command* command,
                                                           const BSONObj& cmdObj) {
    mutablebson::Document cmdToLog(cmdObj, mutablebson::Document::kInPlaceDisabled);
    command->snipForLogging(&cmdToLog);
    BSONObjBuilder bob;
    cmdToLog.writeTo(&bob);
    return bob.obj();
}

Future<DbResponse> ServiceEntryPointCommon::handleRequest(
    OperationContext* opCtx, const Message& m, std::unique_ptr<const Hooks> behaviors) noexcept {
    auto hr = std::make_shared<HandleRequest>(opCtx, m, std::move(behaviors));
    return hr->startOperation()
        .then([hr]() -> Future<void> {
            auto opRunner = hr->makeOpRunner();
            invariant(opRunner);
            return opRunner->run().then(
                [execContext = hr->executionContext](DbResponse response) -> void {
                    // Set the response upon successful execution
                    execContext->setResponse(std::move(response));

                    auto opCtx = execContext->getOpCtx();

                    auto seCtx = transport::ServiceExecutorContext::get(opCtx->getClient());
                    if (!seCtx) {
                        // We were run by a background worker.
                        return;
                    }

                    if (auto invocation = CommandInvocation::get(opCtx);
                        invocation && !invocation->isSafeForBorrowedThreads()) {
                        // If the last command wasn't safe for a borrowed thread, then let's move
                        // off of it.
                        seCtx->setThreadingModel(
                            transport::ServiceExecutor::ThreadingModel::kDedicated);
                    }
                });
        })
        .then([hr] { return hr->completeOperation(); })
        .onCompletion([hr](Status status) -> Future<DbResponse> {
            if (!status.isOK()) {
                LOGV2_ERROR(4879802, "Failed to handle request", "error"_attr = redact(status));
                return status;
            }
            return hr->executionContext->getResponse();
        });
}

ServiceEntryPointCommon::Hooks::~Hooks() = default;

}  // namespace mongo
