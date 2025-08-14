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


#include "mongo/db/service_entry_point_shard_role.h"

#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/client/read_preference.h"
#include "mongo/db/admission/ingress_admission_context.h"
#include "mongo/db/admission/ingress_admission_control_gen.h"
#include "mongo/db/admission/ingress_admission_controller.h"
#include "mongo/db/api_parameters.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_manager.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/ldap_cumulative_operation_stats.h"
#include "mongo/db/auth/ldap_operation_stats.h"
#include "mongo/db/auth/resource_pattern.h"
#include "mongo/db/auth/security_token_authentication_guard.h"
#include "mongo/db/auth/user_acquisition_stats.h"
#include "mongo/db/auth/validated_tenancy_scope.h"
#include "mongo/db/client.h"
#include "mongo/db/cluster_parameters/sharding_cluster_parameters_gen.h"
#include "mongo/db/command_can_run_here.h"
#include "mongo/db/commands.h"
#include "mongo/db/commands/server_status_metric.h"
#include "mongo/db/commands/txn_cmds_gen.h"
#include "mongo/db/curop.h"
#include "mongo/db/curop_failpoint_helpers.h"
#include "mongo/db/curop_metrics.h"
#include "mongo/db/database_name.h"
#include "mongo/db/default_max_time_ms_cluster_parameter.h"
#include "mongo/db/error_labels.h"
#include "mongo/db/exec/mutable_bson/document.h"
#include "mongo/db/feature_flag.h"
#include "mongo/db/generic_argument_util.h"
#include "mongo/db/global_catalog/catalog_cache/shard_cannot_refresh_due_to_locks_held_exception.h"
#include "mongo/db/initialize_operation_session_info.h"
#include "mongo/db/local_catalog/collection.h"
#include "mongo/db/local_catalog/collection_catalog.h"
#include "mongo/db/local_catalog/lock_manager/lock_manager_defs.h"
#include "mongo/db/local_catalog/shard_role_api/transaction_resources.h"
#include "mongo/db/local_catalog/shard_role_catalog/operation_sharding_state.h"
#include "mongo/db/logical_time.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/not_primary_error_tracker.h"
#include "mongo/db/op_observer/op_observer.h"
#include "mongo/db/profile_collection.h"
#include "mongo/db/profile_settings.h"
#include "mongo/db/query/query_request_helper.h"
#include "mongo/db/read_concern_support_result.h"
#include "mongo/db/read_write_concern_defaults.h"
#include "mongo/db/read_write_concern_defaults_gen.h"
#include "mongo/db/read_write_concern_provenance.h"
#include "mongo/db/repl/member_state.h"
#include "mongo/db/repl/optime.h"
#include "mongo/db/repl/read_concern_args.h"
#include "mongo/db/repl/read_concern_level.h"
#include "mongo/db/repl/repl_client_info.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/repl/storage_interface.h"
#include "mongo/db/replication_state_transition_lock_guard.h"
#include "mongo/db/request_execution_context.h"
#include "mongo/db/server_feature_flags_gen.h"
#include "mongo/db/server_options.h"
#include "mongo/db/server_parameter.h"
#include "mongo/db/server_parameter_with_storage.h"
#include "mongo/db/service_context.h"
#include "mongo/db/service_entry_point_shard_role_helpers.h"
#include "mongo/db/session/logical_session_id_gen.h"
#include "mongo/db/session/session_catalog.h"
#include "mongo/db/session/session_catalog_mongod.h"
#include "mongo/db/sharding_environment/shard_id.h"
#include "mongo/db/sharding_environment/sharding_feature_flags_gen.h"
#include "mongo/db/sharding_environment/sharding_initialization_waiter.h"
#include "mongo/db/sharding_environment/sharding_statistics.h"
#include "mongo/db/stats/api_version_metrics.h"
#include "mongo/db/stats/counters.h"
#include "mongo/db/stats/read_preference_metrics.h"
#include "mongo/db/stats/server_read_concern_metrics.h"
#include "mongo/db/stats/top.h"
#include "mongo/db/tenant_id.h"
#include "mongo/db/topology/cluster_role.h"
#include "mongo/db/topology/sharding_state.h"
#include "mongo/db/transaction/transaction_participant.h"
#include "mongo/db/transaction_validation.h"
#include "mongo/db/validate_api_parameters.h"
#include "mongo/db/vector_clock/vector_clock.h"
#include "mongo/db/versioning_protocol/chunk_version.h"
#include "mongo/db/versioning_protocol/database_version.h"
#include "mongo/db/versioning_protocol/shard_version.h"
#include "mongo/db/versioning_protocol/stale_exception.h"
#include "mongo/db/write_concern.h"
#include "mongo/db/write_concern_options.h"
#include "mongo/logv2/log.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/rpc/check_allowed_op_query_cmd.h"
#include "mongo/rpc/factory.h"
#include "mongo/rpc/message.h"
#include "mongo/rpc/metadata.h"
#include "mongo/rpc/metadata/audit_user_attrs.h"
#include "mongo/rpc/metadata/client_metadata.h"
#include "mongo/rpc/metadata/impersonated_client_session.h"
#include "mongo/rpc/op_msg.h"
#include "mongo/rpc/protocol.h"
#include "mongo/rpc/reply_builder_interface.h"
#include "mongo/rpc/topology_version_gen.h"
#include "mongo/s/analyze_shard_key_role.h"
#include "mongo/s/query/exec/document_source_merge_cursors.h"
#include "mongo/s/query_analysis_sampler.h"
#include "mongo/s/transaction_router.h"
#include "mongo/s/would_change_owning_shard_exception.h"
#include "mongo/stdx/mutex.h"
#include "mongo/transport/hello_metrics.h"
#include "mongo/transport/service_executor.h"
#include "mongo/transport/session.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/clock_source.h"
#include "mongo/util/concurrency/admission_context.h"
#include "mongo/util/concurrency/ticketholder.h"
#include "mongo/util/database_name_util.h"
#include "mongo/util/decorable.h"
#include "mongo/util/duration.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/future_impl.h"
#include "mongo/util/future_util.h"
#include "mongo/util/namespace_string_util.h"
#include "mongo/util/net/hostandport.h"
#include "mongo/util/scopeguard.h"
#include "mongo/util/serialization_context.h"
#include "mongo/util/str.h"
#include "mongo/util/string_map.h"
#include "mongo/util/testing_proctor.h"
#include "mongo/util/time_support.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include <absl/container/node_hash_map.h>
#include <boost/cstdint.hpp>
#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>
#include <fmt/format.h>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kCommand


namespace mongo {

MONGO_FAIL_POINT_DEFINE(respondWithNotPrimaryInCommandDispatch);
MONGO_FAIL_POINT_DEFINE(skipCheckingForNotPrimaryInCommandDispatch);
MONGO_FAIL_POINT_DEFINE(waitAfterNewStatementBlocksBehindPrepare);
MONGO_FAIL_POINT_DEFINE(waitAfterNewStatementBlocksBehindOpenInternalTransactionForRetryableWrite);
MONGO_FAIL_POINT_DEFINE(waitAfterCommandFinishesExecution);
MONGO_FAIL_POINT_DEFINE(failWithErrorCodeInRunCommand);
MONGO_FAIL_POINT_DEFINE(hangBeforeSessionCheckOut);
MONGO_FAIL_POINT_DEFINE(hangAfterSessionCheckOut);
MONGO_FAIL_POINT_DEFINE(hangBeforeSettingTxnInterruptFlag);
MONGO_FAIL_POINT_DEFINE(hangAfterCheckingWritabilityForMultiDocumentTransactions);
MONGO_FAIL_POINT_DEFINE(failWithErrorCodeAfterSessionCheckOut);

// Tracks the number of times a legacy unacknowledged write failed due to
// not primary error resulted in network disconnection.
auto& notPrimaryLegacyUnackWrites =
    *MetricBuilder<Counter64>{"repl.network.notPrimaryLegacyUnacknowledgedWrites"};

// Tracks the number of times an unacknowledged write failed due to not primary error
// resulted in network disconnection.
auto& notPrimaryUnackWrites =
    *MetricBuilder<Counter64>{"repl.network.notPrimaryUnacknowledgedWrites"};

namespace {


void runCommandInvocation(const RequestExecutionContext& rec, CommandInvocation* invocation) {
    CommandHelpers::runCommandInvocation(rec.getOpCtx(), invocation, rec.getReplyBuilder());
}

/*
 * Allows for the very complex handleRequest function to be decomposed into parts.
 * It also provides the infrastructure to futurize the process of executing commands.
 */
struct HandleRequest {
    // Maintains the context (e.g., opCtx and replyBuilder) required for command execution.
    struct ExecutionContext final : public RequestExecutionContext {
        ExecutionContext(OperationContext* opCtx, Message msg, Date_t started)
            : RequestExecutionContext(opCtx, std::move(msg), started) {}
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
            return NamespaceStringUtil::deserialize(
                boost::none, dbmsg.getns(), SerializationContext::stateDefault());
        }

        void assertValidNsString() {
            if (!nsString().isValid()) {
                uassert(
                    16257, fmt::format("Invalid ns [{}]", nsString().toStringForErrorMsg()), false);
            }
        }

        /**
         * Note that DBDirectClient is treated as an internal client in relation to letting
         * internal errors escape.
         */
        bool isInternalClient() const {
            return client().isInDirectClient() ||
                (client().session() && client().isInternalClient());
        }

        boost::optional<long long> slowMsOverride;
        bool forceLog = false;
    };

    HandleRequest(OperationContext* opCtx, const Message& msg, Date_t started)
        : executionContext(opCtx, const_cast<Message&>(msg), started) {}

    void startOperation();
    DbResponse runOperation();
    void completeOperation(DbResponse&);

    ExecutionContext executionContext;
};

void registerError(OperationContext* opCtx, const Status& status) {
    NotPrimaryErrorTracker::get(opCtx->getClient()).recordError(status.code());
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
 * For replica set members it returns the last known op time from opCtx. Otherwise will return
 * uninitialized cluster time.
 */
LogicalTime getClientOperationTime(OperationContext* opCtx) {
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
    dassert(replCoord->getSettings().isReplSet());

    auto operationTime = getClientOperationTime(opCtx);
    invariant(operationTime >= startOperationTime);

    // If the last operationTime has not changed, consider this command a read, and, for replica set
    // members, construct the operationTime with the proper optime for its read concern level.
    if (operationTime == startOperationTime) {
        auto& readConcernArgs = repl::ReadConcernArgs::get(opCtx);

        // Note: ReadConcernArgs::getLevel returns kLocal if none was set.
        if (readConcernArgs.getLevel() == repl::ReadConcernLevel::kMajorityReadConcern) {
            operationTime =
                LogicalTime(replCoord->getCurrentCommittedSnapshotOpTime().getTimestamp());
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
    if (!repl::ReplicationCoordinator::get(opCtx)->getSettings().isReplSet() ||
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
                                         bool isInternalClient,
                                         const repl::OpTime& lastOpBeforeRun,
                                         const repl::OpTime& lastOpAfterRun) {
    if (!code && !wcCode) {
        return;
    }

    auto errorLabels =
        getErrorLabels(opCtx,
                       sessionOptions,
                       commandName,
                       code,
                       wcCode,
                       isInternalClient,
                       false /* isMongos */,
                       OperationShardingState::isComingFromRouter(opCtx) /* isComingFromRouter */,
                       lastOpBeforeRun,
                       lastOpAfterRun);
    commandBodyFieldsBob->appendElements(errorLabels);

    const auto isNotPrimaryError =
        (code && ErrorCodes::isA<ErrorCategory::NotPrimaryError>(*code)) ||
        (wcCode && ErrorCodes::isA<ErrorCategory::NotPrimaryError>(*wcCode));

    const auto isShutdownError = (code && ErrorCodes::isA<ErrorCategory::ShutdownError>(*code)) ||
        (wcCode && ErrorCodes::isA<ErrorCategory::ShutdownError>(*wcCode));

    const auto replCoord = repl::ReplicationCoordinator::get(opCtx);
    // NotPrimary errors always include a topologyVersion, since we increment topologyVersion on
    // stepdown. ShutdownErrors only include a topologyVersion if the server is in quiesce mode,
    // since we only increment the topologyVersion at shutdown and alert waiting isMaster/hello
    // commands if the server enters quiesce mode.
    const auto shouldAppendTopologyVersion =
        (replCoord->getSettings().isReplSet() && isNotPrimaryError) ||
        (isShutdownError && replCoord->inQuiesceMode());

    if (!shouldAppendTopologyVersion) {
        return;
    }

    const auto topologyVersion = replCoord->getTopologyVersion();
    commandBodyFieldsBob->appendElementsUnique(BSON("topologyVersion" << topologyVersion.toBSON()));
}

void appendAdditionalParticipants(OperationContext* opCtx, BSONObjBuilder* commandBodyFieldsBob) {
    auto txnRouter = TransactionRouter::get(opCtx);
    if (!txnRouter)
        return;

    auto additionalParticipants = txnRouter.getAdditionalParticipantsForResponse(opCtx);
    if (!additionalParticipants)
        return;

    std::vector<BSONObj> participantArray;
    for (const auto& p : *additionalParticipants) {
        auto shardId = ShardId(p.first);

        // The "readOnly" value is set for participants upon a successful response. If an error
        // occurred before getting a response from a participant, it will not have a readOnly value
        // set.
        auto readOnly = p.second;
        if (readOnly) {
            participantArray.emplace_back(BSON("shardId" << shardId << "readOnly" << *readOnly));
        } else {
            participantArray.emplace_back(BSON("shardId" << shardId));
        }
    }

    commandBodyFieldsBob->appendElements(
        BSON(TxnResponseMetadata::kAdditionalParticipantsFieldName << participantArray));
}

class RunCommandOpTimes {
public:
    RunCommandOpTimes(OperationContext* opCtx) : _lastOpBeforeRun(getLastOp(opCtx)) {}

    void onCommandFinished(OperationContext* opCtx) {
        if (!_lastOpAfterRun.isNull()) {
            return;
        }
        _lastOpAfterRun = getLastOp(opCtx);
    }

    const repl::OpTime& getLastOpBeforeRun() const {
        return _lastOpBeforeRun;
    }

    const repl::OpTime& getLastOpAfterRun() const {
        return _lastOpAfterRun;
    }

private:
    repl::OpTime getLastOp(OperationContext* opCtx) {
        return repl::ReplClientInfo::forClient(opCtx->getClient()).getLastOp();
    }

    repl::OpTime _lastOpBeforeRun;
    repl::OpTime _lastOpAfterRun;
};

namespace {
const CommandNameAtom helloAtom("hello");
const CommandNameAtom isMasterAtom("isMaster");
}  // namespace

class ExecCommandDatabase {
public:
    explicit ExecCommandDatabase(HandleRequest::ExecutionContext& execContext)
        : _execContext(execContext) {
        _parseCommand();
    }

    // Executes a command after stripping metadata, performing authorization checks, handling audit
    // impersonation, and (potentially) setting maintenance mode. Also checks that the command is
    // permissible to run on the node given its current replication state. All the logic here is
    // independent of any particular command; any functionality relevant to a specific command
    // should be confined to its run() method.
    void run() {
        auto status = [&] {
            try {
                _initiateCommand();
                _commandExec();
            } catch (const DBException& ex) {
                return ex.toStatus();
            }

            return Status::OK();
        }();

        // Release the ingress admission ticket
        _admissionTicket = boost::none;

        if (!_execContext.client().isInDirectClient()) {
            auto authzSession = AuthorizationSession::get(_execContext.client());
            authzSession->verifyContract(_execContext.getCommand()->getAuthorizationContract());
        }

        if (MONGO_unlikely(!status.isOK()))
            _handleFailure(std::move(status));
    }

    const HandleRequest::ExecutionContext& getExecutionContext() {
        return _execContext;
    }
    CommandInvocation* getInvocation() {
        return _invocation.get();
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

    void onCommandFinished() {
        if (!_runCommandOpTimes) {
            return;
        }
        _runCommandOpTimes->onCommandFinished(_execContext.getOpCtx());
    }

    const boost::optional<RunCommandOpTimes>& getRunCommandOpTimes() {
        return _runCommandOpTimes;
    }

    repl::OpTime getLastOpBeforeRun() {
        if (!_runCommandOpTimes) {
            return repl::OpTime{};
        }
        return _runCommandOpTimes->getLastOpBeforeRun();
    }

    repl::OpTime getLastOpAfterRun() {
        if (!_runCommandOpTimes) {
            return repl::OpTime{};
        }
        return _runCommandOpTimes->getLastOpAfterRun();
    }

    bool isHello() const {
        auto atom = _execContext.getCommand()->getNameAtom();
        return atom == helloAtom || atom == isMasterAtom;
    }

    // Decides if the command can be retried based on the execution error.
    bool canRetryCommand(const Status& execError);

    // Sets a flag indicating that the command cannot be retried, regardless of the execution error.
    void setCannotRetry() {
        _cannotRetry = true;
    }

private:
    void _parseCommand() {
        auto opCtx = _execContext.getOpCtx();
        auto command = _execContext.getCommand();
        auto& request = _execContext.getRequest();

        CommandHelpers::uassertShouldAttemptParse(opCtx, command, request);

        invariant(!auth::ValidatedTenancyScope::get(opCtx).has_value() ||
                  (request.validatedTenancyScope &&
                   *auth::ValidatedTenancyScope::get(opCtx) == *(request.validatedTenancyScope)));

        auth::ValidatedTenancyScope::set(opCtx, request.validatedTenancyScope);

        _invocation = command->parse(opCtx, request);
        CommandInvocation::set(opCtx, _invocation);

        auto apiParameters = parseAndValidateAPIParameters(*_invocation);

        _startOperationTime = getClientOperationTime(opCtx);
        Client* client = opCtx->getClient();
        {
            stdx::lock_guard<Client> lk(*client);
            // We construct a legacy $cmd namespace so we can fill in curOp using
            // the existing logic that existed for OP_QUERY commands
            CurOp::get(opCtx)->setGenericOpRequestDetails(
                lk,
                NamespaceString::makeCommandNamespace(_invocation->db()),
                command,
                request.body,
                _execContext.op());
            // We must obtain the client lock to set APIParameters on the operation context, as it
            // may be concurrently read by CurrentOp.
            APIParameters::get(opCtx) = APIParameters::fromClient(apiParameters);
        }

        rpc::readRequestMetadata(opCtx,
                                 _invocation->getGenericArguments(),
                                 command->requiresAuth(),
                                 _clientSessionGuard);

        const auto session = _execContext.getOpCtx()->getClient()->session();
        if (session) {
            if (!opCtx->isExhaust() || !isHello()) {
                InExhaustHello::get(session.get())->setInExhaust(false, command->getName());
            }
        }
    }

    // Do any initialization of the lock state required for a transaction.
    void _setLockStateForTransaction(OperationContext* opCtx) {
        shard_role_details::getLocker(opCtx)->setSharedLocksShouldTwoPhaseLock(true);
    }

    // Clear any lock state which may have changed after the locker update.
    void _resetLockerStateAfterShardingUpdate(OperationContext* opCtx) {
        dassert(!opCtx->isContinuingMultiDocumentTransaction());
        service_entry_point_shard_role_helpers::resetLockerState(opCtx);
        if (opCtx->isStartingMultiDocumentTransaction())
            _setLockStateForTransaction(opCtx);
    }

    // Any logic, such as authorization and auditing, that must precede execution of the command.
    void _initiateCommand();

    // Executes the parsed command against the database.
    void _commandExec();

    // Takes a command execution error (or write error), attempts to perform metadata refresh and
    // return true in case the refresh was executed, false in case no refresh was executed and an
    // error status if the refresh failed.
    StatusWith<bool> _refreshIfNeeded(const Status& execError);

    // Takes a command execution error (or write error), checks if the problem was that sharding is
    // not yet initialized, and waits for initialization if so. Returns true in the case that we
    // waited for sharding initialization and false otherwise.
    bool _awaitShardingInitializedIfNeeded(const Status& status);

    // Any error-handling logic that must be performed if the command initiation/execution
    // fails.
    void _handleFailure(Status status);

    bool _isInternalClient() const {
        return _execContext.isInternalClient();
    }

    StatusWith<repl::ReadConcernArgs> _extractReadConcern(OperationContext* opCtx,
                                                          bool startTransaction,
                                                          bool startOrContinueTransaction);

    const HandleRequest::ExecutionContext& _execContext;

    // The following allows `_initiateCommand`, `_commandExec`, and `_handleFailure` to share
    // execution state without concerning the lifetime of these variables.
    BSONObjBuilder _extraFieldsBuilder;
    std::shared_ptr<CommandInvocation> _invocation;
    LogicalTime _startOperationTime;
    OperationSessionInfoFromClient _sessionOptions;

    boost::optional<RunCommandOpTimes> _runCommandOpTimes;
    boost::optional<rpc::ImpersonatedClientSessionGuard> _clientSessionGuard;
    boost::optional<auth::SecurityTokenAuthenticationGuard> _tokenAuthorizationSessionGuard;
    bool _refreshedDatabase = false;
    bool _refreshedCollection = false;
    bool _refreshedCatalogCache = false;
    bool _awaitedShardingInitialization = false;
    bool _cannotRetry = false;

    boost::optional<Ticket> _admissionTicket;
};

class RunCommandImpl {
public:
    explicit RunCommandImpl(ExecCommandDatabase* ecd) : _ecd(ecd) {}
    virtual ~RunCommandImpl() = default;

    void run() {
        auto status = [&] {
            try {
                _prologue();
                _runImpl();
                _epilogue();
                return Status::OK();
            } catch (const DBException& ex) {
                return ex.toStatus();
            }
        }();
        // Failure to run a command is either indicated by throwing an exception or
        // adding a non-okay field to the replyBuilder.
        if (MONGO_unlikely(!status.isOK() || !_ok)) {
            auto& execContext = _ecd->getExecutionContext();
            if (status.code() == ErrorCodes::QueryRejectedBySettings) {
                execContext.getCommand()->incrementCommandsRejected();
            } else {
                execContext.getCommand()->incrementCommandsFailed();
            }
            if (status.code() == ErrorCodes::Unauthorized) {
                CommandHelpers::auditLogAuthEvent(execContext.getOpCtx(),
                                                  _ecd->getInvocation(),
                                                  execContext.getRequest(),
                                                  status.code());
            }
            iassert(status);
        }
    }

protected:
    // Reference to attributes defined in `ExecCommandDatabase` (e.g., sessionOptions).
    ExecCommandDatabase* const _ecd;

    // Any code that must run before command execution (e.g., reserving bytes for reply builder).
    void _prologue();

    // Runs the command possibly waiting for write concern.
    virtual void _runImpl();

    // Runs the command without waiting for write concern.
    void _runCommand();

    // Any code that must run after command execution.
    void _epilogue();

    bool _isInternalClient() const {
        return _ecd->getExecutionContext().isInternalClient();
    }

    // If the command resolved successfully.
    bool _ok = false;
};

class RunCommandAndWaitForWriteConcern final : public RunCommandImpl {
public:
    explicit RunCommandAndWaitForWriteConcern(ExecCommandDatabase* ecd)
        : RunCommandImpl(ecd),
          _execContext(_ecd->getExecutionContext()),
          _oldWriteConcern(_execContext.getOpCtx()->getWriteConcern()) {}

    ~RunCommandAndWaitForWriteConcern() override {
        _execContext.getOpCtx()->setWriteConcern(_oldWriteConcern);
    }

    void _runImpl() override;

private:
    void _setup();
    void _runCommandWithFailPoint();
    void _waitForWriteConcern(BSONObjBuilder& bb);
    void _handleError(Status status);
    void _checkWriteConcern();

    const HandleRequest::ExecutionContext& _execContext;

    // Allows changing the write concern while running the command and resetting on destruction.
    const WriteConcernOptions _oldWriteConcern;
    boost::optional<WriteConcernOptions> _extractedWriteConcern;
};

// Simplifies the interface for invoking commands and allows asynchronous execution of command
// invocations.
class InvokeCommand {
public:
    explicit InvokeCommand(ExecCommandDatabase* ecd) : _ecd(ecd) {}

    void run();

private:
    ExecCommandDatabase* const _ecd;
};

class CheckoutSessionAndInvokeCommand {
public:
    CheckoutSessionAndInvokeCommand(ExecCommandDatabase* ecd) : _ecd{ecd} {}

    ~CheckoutSessionAndInvokeCommand() {
        auto opCtx = _ecd->getExecutionContext().getOpCtx();
        if (auto txnParticipant = TransactionParticipant::get(opCtx)) {
            // Only cleanup if we didn't yield the session.
            _cleanupTransaction(txnParticipant);
        }
    }

    void run();

private:
    void _stashTransaction(TransactionParticipant::Participant& txnParticipant);
    void _cleanupTransaction(TransactionParticipant::Participant& txnParticipant);

    void _checkOutSession();
    void _tapError(Status);
    void _commitInvocation();

    ExecCommandDatabase* const _ecd;

    std::unique_ptr<MongoDSessionCatalog::Session> _sessionTxnState;
    bool _shouldCleanUp = false;
};

void InvokeCommand::run() {
    runCommandInvocation(_ecd->getExecutionContext(), _ecd->getInvocation());
}

void CheckoutSessionAndInvokeCommand::run() {
    auto status = [&] {
        try {
            _checkOutSession();

            if (auto scoped = failWithErrorCodeAfterSessionCheckOut.scoped();
                MONGO_unlikely(scoped.isActive())) {
                const auto errorCode =
                    static_cast<ErrorCodes::Error>(scoped.getData()["errorCode"].numberInt());
                LOGV2_DEBUG(8535500,
                            1,
                            "failWithErrorCodeAfterSessionCheckOut enabled, failing command",
                            "errorCode"_attr = errorCode);
                BSONObjBuilder errorBuilder;
                return Status(errorCode, "failWithErrorCodeAfterSessionCheckOut enabled.");
            }

            runCommandInvocation(_ecd->getExecutionContext(), _ecd->getInvocation());
            return Status::OK();
        } catch (const ExceptionFor<ErrorCodes::WouldChangeOwningShard>& ex) {
            auto opCtx = _ecd->getExecutionContext().getOpCtx();
            auto txnParticipant = TransactionParticipant::get(opCtx);
            if (!txnParticipant) {
                // No code paths that can throw this error should yield their session but uassert
                // instead of invariant in case that assumption is ever broken since this only needs
                // to be operation fatal.
                auto statusWithContext = ex.toStatus().withContext(
                    "Cannot handle WouldChangeOwningShard error because the operation yielded its "
                    "session");
                uasserted(6609000, statusWithContext.reason());
            }

            auto wouldChangeOwningShardInfo = ex.toStatus().extraInfo<WouldChangeOwningShardInfo>();
            invariant(wouldChangeOwningShardInfo);
            txnParticipant.handleWouldChangeOwningShardError(opCtx, wouldChangeOwningShardInfo);
            _stashTransaction(txnParticipant);

            auto txnResponseMetadata = txnParticipant.getResponseMetadata();
            (_ecd->getExtraFieldsBuilder())->appendElements(txnResponseMetadata);
            return ex.toStatus();
        } catch (const DBException& ex) {
            return ex.toStatus();
        }
    }();

    if (MONGO_unlikely(!status.isOK())) {
        _tapError(status);
        iasserted(status);
    }
    _commitInvocation();
}

void CheckoutSessionAndInvokeCommand::_stashTransaction(
    TransactionParticipant::Participant& txnParticipant) {
    invariant(txnParticipant);
    if (!_shouldCleanUp) {
        return;
    }
    _shouldCleanUp = false;

    auto opCtx = _ecd->getExecutionContext().getOpCtx();
    txnParticipant.stashTransactionResources(opCtx);
}

void CheckoutSessionAndInvokeCommand::_cleanupTransaction(
    TransactionParticipant::Participant& txnParticipant) {
    invariant(txnParticipant);
    if (!_shouldCleanUp) {
        return;
    }
    _shouldCleanUp = false;

    auto opCtx = _ecd->getExecutionContext().getOpCtx();
    const bool isPrepared = txnParticipant.transactionIsPrepared();
    try {
        if (isPrepared)
            txnParticipant.stashTransactionResources(opCtx);
        else if (txnParticipant.transactionIsOpen())
            txnParticipant.abortTransaction(opCtx);
    } catch (...) {
        // It is illegal for this to throw so we catch and log this here for diagnosability.
        LOGV2_FATAL(21974,
                    "Unable to stash/abort transaction",
                    "operation"_attr = (isPrepared ? "stash" : "abort"),
                    "txnNumber"_attr = opCtx->getTxnNumber(),
                    "logicalSessionId"_attr = opCtx->getLogicalSessionId()->toBSON(),
                    "error"_attr = exceptionToStatus());
    }
}

namespace {
const CommandNameAtom createAtom("create");
const CommandNameAtom shardSvrAtom("_shardsvrCreateCollection");
const CommandNameAtom createIndexesAtom("createIndexes");

bool isCreate(const Command* cmd) {
    auto atom = cmd->getNameAtom();
    return atom == createAtom || atom == shardSvrAtom || atom == createIndexesAtom;
}
}  // namespace


void CheckoutSessionAndInvokeCommand::_checkOutSession() {
    auto& execContext = _ecd->getExecutionContext();
    auto opCtx = execContext.getOpCtx();
    CommandInvocation* invocation = _ecd->getInvocation();
    const OperationSessionInfoFromClient& sessionOptions = _ecd->getSessionOptions();

    // This constructor will check out the session. It handles the appropriate state management
    // for both multi-statement transactions and retryable writes. Currently, only requests with
    // a transaction number will check out the session.
    hangBeforeSessionCheckOut.pauseWhileSet();
    auto mongoDSessionCatalog = MongoDSessionCatalog::get(opCtx);
    _sessionTxnState = mongoDSessionCatalog->checkOutSession(opCtx);
    auto txnParticipant = TransactionParticipant::get(opCtx);
    hangAfterSessionCheckOut.pauseWhileSet();

    // Used for waiting for an in-progress transaction to transition out of the conflicting state.
    auto waitForInProgressTxn = [this](OperationContext* opCtx, auto& stateTransitionFuture) {
        // Check the session back in and wait for the conflict to resolve.
        _sessionTxnState->checkIn(opCtx, OperationContextSession::CheckInReason::kYield);
        stateTransitionFuture.wait(opCtx);
        // Wait for any commit or abort oplog entry to be visible in the oplog. This will prevent a
        // new transaction from missing the transaction table update for the previous commit or
        // abort due to an oplog hole.
        auto storageInterface = repl::StorageInterface::get(opCtx);
        storageInterface->waitForAllEarlierOplogWritesToBeVisible(opCtx);
        // Check out the session again.
        _sessionTxnState->checkOut(opCtx);
    };

    auto apiParamsFromClient = APIParameters::get(opCtx);
    auto apiParamsFromTxn = txnParticipant.getAPIParameters(opCtx);
    uassert(
        ErrorCodes::APIMismatchError,
        fmt::format(
            "API parameter mismatch: {} used params {}, the transaction's first command used {}",
            invocation->definition()->getName(),
            apiParamsFromClient.toBSON().toString(),
            apiParamsFromTxn.toBSON().toString()),
        apiParamsFromTxn == apiParamsFromClient);

    if (!opCtx->getClient()->isInDirectClient()) {
        bool beganOrContinuedTxn{false};
        // This loop allows new transactions on a session to block behind a previous prepared
        // transaction on that session.
        while (!beganOrContinuedTxn) {
            try {
                auto opObserver = opCtx->getServiceContext()->getOpObserver();
                opObserver->onTransactionStart(opCtx);
                auto transactionAction = TransactionParticipant::TransactionActions::kNone;
                if (sessionOptions.getStartTransaction()) {
                    transactionAction = TransactionParticipant::TransactionActions::kStart;
                } else if (sessionOptions.getStartOrContinueTransaction()) {
                    transactionAction =
                        TransactionParticipant::TransactionActions::kStartOrContinue;
                } else if (sessionOptions.getAutocommit()) {
                    transactionAction = TransactionParticipant::TransactionActions::kContinue;
                }
                txnParticipant.beginOrContinue(
                    opCtx,
                    {*sessionOptions.getTxnNumber(), sessionOptions.getTxnRetryCounter()},
                    sessionOptions.getAutocommit(),
                    transactionAction);
                beganOrContinuedTxn = true;
            } catch (const ExceptionFor<ErrorCodes::PreparedTransactionInProgress>&) {
                auto prevTxnExitedPrepare = txnParticipant.onExitPrepare();

                CurOpFailpointHelpers::waitWhileFailPointEnabled(
                    &waitAfterNewStatementBlocksBehindPrepare,
                    opCtx,
                    "waitAfterNewStatementBlocksBehindPrepare");

                waitForInProgressTxn(opCtx, prevTxnExitedPrepare);
            } catch (const ExceptionFor<ErrorCodes::RetryableTransactionInProgress>&) {
                auto conflictingTxnCommittedOrAborted =
                    txnParticipant.onConflictingInternalTransactionCompletion(opCtx);

                waitAfterNewStatementBlocksBehindOpenInternalTransactionForRetryableWrite
                    .pauseWhileSet();

                waitForInProgressTxn(opCtx, conflictingTxnCommittedOrAborted);
            }
        }

        // Create coordinator if needed. If "startTransaction" is present, it must be true.
        if (sessionOptions.getStartTransaction()) {
            // If this shard has been selected as the coordinator, set up the coordinator state
            // to be ready to receive votes.
            if (sessionOptions.getCoordinator() == boost::optional<bool>(true)) {
                service_entry_point_shard_role_helpers::createTransactionCoordinator(
                    opCtx, *sessionOptions.getTxnNumber(), sessionOptions.getTxnRetryCounter());
            }
        }

        // Release the transaction lock resources and abort storage transaction for unprepared
        // transactions on failure to unstash the transaction resources to opCtx. We don't want
        // to have this error guard for beginOrContinue as it can abort the transaction for any
        // accidental invalid statements in the transaction.
        //
        // Unstashing resources can't yield the session so it's safe to capture a reference to
        // the TransactionParticipant in this scope guard.
        ScopeGuard abortOnError([&] {
            if (txnParticipant.transactionIsInProgress()) {
                txnParticipant.abortTransaction(opCtx);
            }
        });

        if (opCtx->isStartingMultiDocumentTransaction()) {
            service_entry_point_shard_role_helpers::waitForReadConcern(
                opCtx, _ecd->getInvocation(), execContext.getRequest());
        }

        txnParticipant.unstashTransactionResources(opCtx, invocation->definition()->getName());

        // Unstash success.
        abortOnError.dismiss();
    }

    _shouldCleanUp = true;

    if (!opCtx->getClient()->isInDirectClient()) {
        const auto& readConcernArgs = repl::ReadConcernArgs::get(opCtx);

        auto command = invocation->definition();
        // Record readConcern usages for commands run inside transactions after unstashing the
        // transaction resources.
        if (command->shouldAffectReadOptionCounters() && opCtx->inMultiDocumentTransaction()) {
            ServerReadConcernMetrics::get(opCtx)->recordReadConcern(readConcernArgs,
                                                                    true /* isTransaction */);
        }

        // For replica sets, we do not receive the readConcernArgs of our parent transaction
        // statements until we unstash the transaction resources. The below check is necessary to
        // ensure commands, including those occurring after the first statement in their respective
        // transactions, are checked for readConcern support. Presently, only `create` and
        // `createIndexes` do not support readConcern inside transactions.
        // Note: _shardsvrCreateCollection is used to run the 'create' command on the primary in
        // case of sharded cluster
        // TODO(SERVER-46971): Consider how to extend this check to other commands.
        auto readConcernSupport = invocation->supportsReadConcern(
            readConcernArgs.getLevel(), readConcernArgs.isImplicitDefault());
        if (readConcernArgs.hasLevel() && isCreate(command)) {
            if (!readConcernSupport.readConcernSupport.isOK()) {
                uassertStatusOK(readConcernSupport.readConcernSupport.withContext(
                    fmt::format("Command {} does not support this transaction's {}",
                                command->getName(),
                                readConcernArgs.toString())));
            }
        }
    }
}

void CheckoutSessionAndInvokeCommand::_tapError(Status status) {
    auto& execContext = _ecd->getExecutionContext();
    auto opCtx = execContext.getOpCtx();

    // We still append any participants that this shard might have added to the transaction on an
    // error response because:
    // 1. There are some errors that mongos will retry on when there is only one participant. It's
    // important that mongos does not retry on these errors if the participant that it contacted
    // added additional participants.
    // 2. If the error is not retryable, mongos can then abort the transaction on the added
    // participants rather than waiting for the added shards to abort either due to a transaction
    // timeout or a new transaction being started, releasing their resources sooner.
    appendAdditionalParticipants(opCtx, _ecd->getExtraFieldsBuilder());

    if (status.code() == ErrorCodes::CommandOnShardedViewNotSupportedOnMongod) {
        // Exceptions are used to resolve views in a sharded cluster, so they should be handled
        // specially to avoid unnecessary aborts.

        if (opCtx->isStartingMultiDocumentTransaction()) {
            // If the first command a shard receives in a transactions fails with this code, the
            // shard may not be included in the final participant list if the router's retry after
            // resolving the view does not re-target it, which is possible if the underlying
            // collection is sharded. The shard's transaction should be preemptively aborted to
            // avoid leaving it orphaned in this case, which is fine even if it is re-targeted
            // because the retry will include "startTransaction" or "startOrContinueTransaction"
            // again and "restart" a transaction at the active txnNumber.
            return;
        }

        auto txnParticipant = TransactionParticipant::get(opCtx);
        if (!txnParticipant) {
            // No code paths that can throw this error should yield their session but uassert
            // instead of invariant in case that assumption is ever broken since this only needs to
            // be operation fatal.
            auto statusWithContext = status.withContext(
                "Cannot handle CommandOnShardedViewNotSupportedOnMongod error because the "
                "operation yielded its session");
            uasserted(6609001, statusWithContext.reason());
        }

        // If this shard has completed an earlier statement for this transaction, it must already be
        // in the transaction's participant list, so it is guaranteed to learn its outcome.
        _stashTransaction(txnParticipant);
    } else if (_ecd->canRetryCommand(status)) {
        // If the session is yielded, set a flag on ExecCommandDatabase to disallow retrying even on
        // a retryable error - we can't know whether this participant acted as a sub-router, so we
        // can't know whether it is safe to retry.
        auto txnParticipant = TransactionParticipant::get(opCtx);
        if (!txnParticipant) {
            _ecd->setCannotRetry();
        }

        // If this shard acted as a sub-router, set the flag to disallow retrying.
        auto txnRouter = TransactionRouter::get(opCtx);
        if (!txnRouter)
            return;

        if (!txnRouter.isSafeToRetryStaleErrors(opCtx)) {
            _ecd->setCannotRetry();
        }
    }
}

void CheckoutSessionAndInvokeCommand::_commitInvocation() {
    auto& execContext = _ecd->getExecutionContext();
    auto replyBuilder = execContext.getReplyBuilder();
    if (auto okField = replyBuilder->getBodyBuilder().asTempObj()["ok"]) {
        // If ok is present, use its truthiness.
        if (!okField.trueValue()) {
            return;
        }
    }

    if (auto txnParticipant = TransactionParticipant::get(execContext.getOpCtx())) {
        // If the command didn't yield its session, stash or commit the transaction when the command
        // succeeds.
        _stashTransaction(txnParticipant);

        if (serverGlobalParams.clusterRole.has(ClusterRole::ShardServer) ||
            serverGlobalParams.clusterRole.has(ClusterRole::ConfigServer)) {
            auto txnResponseMetadata = txnParticipant.getResponseMetadata();
            auto bodyBuilder = replyBuilder->getBodyBuilder();
            bodyBuilder.appendElements(txnResponseMetadata);
            appendAdditionalParticipants(execContext.getOpCtx(), &bodyBuilder);
        }
    }
}

void RunCommandImpl::_prologue() {
    auto& execContext = _ecd->getExecutionContext();
    auto opCtx = execContext.getOpCtx();
    const Command* command = _ecd->getInvocation()->definition();
    auto bytesToReserve = command->reserveBytesForReply();
// SERVER-22100: In Windows DEBUG builds, the CRT heap debugging overhead, in conjunction with the
// additional memory pressure introduced by reply buffer pre-allocation, causes the concurrency
// suite to run extremely slowly. As a workaround we do not pre-allocate in Windows DEBUG builds.
#ifdef _WIN32
    if (kDebugBuild)
        bytesToReserve = 0;
#endif
    execContext.getReplyBuilder()->reserveBytes(bytesToReserve);

    // Record readConcern usages for commands run outside of transactions, excluding DBDirectClient.
    // For commands inside a transaction, they inherit the readConcern from the transaction. So we
    // will record their readConcern usages after we have unstashed the transaction resources.
    if (!opCtx->getClient()->isInDirectClient() && command->shouldAffectReadOptionCounters() &&
        !opCtx->inMultiDocumentTransaction()) {
        ServerReadConcernMetrics::get(opCtx)->recordReadConcern(repl::ReadConcernArgs::get(opCtx),
                                                                false /* isTransaction */);
    }

    auto const replCoord = repl::ReplicationCoordinator::get(opCtx);
    // If the state is not primary or secondary, we skip collecting metrics. We also use the UNSAFE
    // method in the replication coordinator, as collecting metrics around read preference usage is
    // best-effort and should not contend for the replication coordinator mutex.
    if (replCoord->getSettings().isReplSet() && replCoord->isInPrimaryOrSecondaryState_UNSAFE()) {
        auto isPrimary = replCoord->canAcceptWritesForDatabase_UNSAFE(opCtx, DatabaseName::kAdmin);
        // Skip incrementing metrics when the command is not a read operation, as we expect to all
        // commands sent via the driver to inherit the read preference, even if we don't use it.
        if (command->shouldAffectReadOptionCounters()) {
            ReadPreferenceMetrics::get(opCtx)->recordReadPreference(
                ReadPreferenceSetting::get(opCtx), _isInternalClient(), isPrimary);
        }
    }
}

void RunCommandImpl::_epilogue() {
    auto& execContext = _ecd->getExecutionContext();
    auto opCtx = execContext.getOpCtx();
    auto command = execContext.getCommand();
    auto replyBuilder = execContext.getReplyBuilder();
    _ecd->onCommandFinished();

    // This fail point blocks all commands which are running on the specified namespace, or which
    // are present in the given list of commands, or which match a given comment. If no namespace,
    // command list, or comment are provided, then the failpoint will block all commands.
    waitAfterCommandFinishesExecution.executeIf(
        [&](const BSONObj& data) {
            CurOpFailpointHelpers::waitWhileFailPointEnabled(
                &waitAfterCommandFinishesExecution, opCtx, "waitAfterCommandFinishesExecution");
        },
        [&](const BSONObj& data) {
            auto& request = execContext.getRequest();
            auto commands =
                data.hasField("commands") ? data["commands"].Array() : std::vector<BSONElement>();
            bool requestMatchesComment = data.hasField("comment")
                ? data.getField("comment").woCompare(request.body.getField("comment")) == 0
                : true;

            // If 'ns', 'commands', or 'comment' is not set, block for all the namespaces, commands,
            // or comments respectively.
            const auto fpNss = NamespaceStringUtil::parseFailPointData(data, "ns");
            return (fpNss.isEmpty() || _ecd->getInvocation()->ns() == fpNss) &&
                (commands.empty() ||
                 std::any_of(commands.begin(),
                             commands.end(),
                             [&request](auto& element) {
                                 return element.valueStringDataSafe() == request.getCommandName();
                             })) &&
                requestMatchesComment;
        });

    service_entry_point_shard_role_helpers::waitForLinearizableReadConcern(opCtx);

    // Wait for data to satisfy the read concern level, if necessary.
    service_entry_point_shard_role_helpers::waitForSpeculativeMajorityReadConcern(opCtx);

    {
        auto body = replyBuilder->getBodyBuilder();
        auto status = CommandHelpers::extractOrAppendOkAndGetStatus(body);
        _ok = status.isOK();
        service_entry_point_shard_role_helpers::attachCurOpErrInfo(opCtx, status);

        boost::optional<ErrorCodes::Error> code =
            _ok ? boost::none : boost::optional<ErrorCodes::Error>(status.code());
        boost::optional<ErrorCodes::Error> wcCode;
        auto response = body.asTempObj();
        if (auto wcErrElement = response["writeConcernError"]; !wcErrElement.eoo()) {
            wcCode = ErrorCodes::Error(wcErrElement["code"].numberInt());
        }
        appendErrorLabelsAndTopologyVersion(opCtx,
                                            &body,
                                            _ecd->getSessionOptions(),
                                            command->getName(),
                                            code,
                                            wcCode,
                                            _isInternalClient(),
                                            _ecd->getLastOpBeforeRun(),
                                            _ecd->getLastOpAfterRun());
    }

    auto commandBodyBob = replyBuilder->getBodyBuilder();
    service_entry_point_shard_role_helpers::appendReplyMetadata(
        opCtx, _ecd->getInvocation()->getGenericArguments(), &commandBodyBob);
    appendClusterAndOperationTime(
        opCtx, &commandBodyBob, &commandBodyBob, _ecd->getStartOperationTime());
}

void RunCommandImpl::_runImpl() {
    service_entry_point_shard_role_helpers::uassertCommandDoesNotSpecifyWriteConcern(
        _ecd->getInvocation()->getGenericArguments());
    _runCommand();
}

void RunCommandImpl::_runCommand() {
    auto shouldCheckoutSession = _ecd->getSessionOptions().getTxnNumber() &&
        _ecd->getInvocation()->definition()->shouldCheckoutSession();
    if (shouldCheckoutSession) {
        CheckoutSessionAndInvokeCommand path(_ecd);
        path.run();
    } else {
        InvokeCommand path(_ecd);
        path.run();
    }
}

void RunCommandAndWaitForWriteConcern::_waitForWriteConcern(BSONObjBuilder& bb) {
    auto invocation = _ecd->getInvocation();
    auto opCtx = _execContext.getOpCtx();
    if (auto scoped = failCommand.scopedIf([&](const BSONObj& obj) {
            return CommandHelpers::shouldActivateFailCommandFailPoint(
                       obj, invocation, opCtx->getClient()) &&
                obj.hasField("writeConcernError"_sd) && !bb.hasField("writeConcernError"_sd);
        });
        MONGO_unlikely(scoped.isActive())) {
        const BSONObj& data = scoped.getData();
        bb.append(data["writeConcernError"_sd]);
        if (data.hasField(kErrorLabelsFieldName) &&
            data[kErrorLabelsFieldName].type() == BSONType::array) {
            // Propagate error labels specified in the failCommand failpoint to the
            // OperationContext decoration to override getErrorLabels() behaviors.
            invariant(!errorLabelsOverride(opCtx));
            errorLabelsOverride(opCtx).emplace(
                data.getObjectField(kErrorLabelsFieldName).getOwned());
        }
        return;
    }

    CurOp::get(opCtx)->debug().writeConcern.emplace(opCtx->getWriteConcern());
    service_entry_point_shard_role_helpers::waitForWriteConcern(
        opCtx, invocation, _ecd->getLastOpBeforeRun(), bb);
}

void RunCommandAndWaitForWriteConcern::_runImpl() {
    _setup();

    auto status = [&] {
        try {
            _runCommandWithFailPoint();
            return Status::OK();
        } catch (const DBException& ex) {
            return ex.toStatus();
        }
    }();

    _ecd->onCommandFinished();
    if (status.isOK()) {
        _checkWriteConcern();
    } else {
        _handleError(std::move(status));
    }
}

void RunCommandAndWaitForWriteConcern::_setup() {
    const auto& invocation = *_ecd->getInvocation();
    OperationContext* opCtx = _execContext.getOpCtx();
    const Command* command = invocation.definition();
    const GenericArguments& genericArgs = invocation.getGenericArguments();

    if (command->getLogicalOp() == LogicalOp::opGetMore) {
        // WriteConcern will be set up during command processing, it must not be specified on
        // the command body.
        service_entry_point_shard_role_helpers::uassertCommandDoesNotSpecifyWriteConcern(
            genericArgs);
    } else {
        // WriteConcern should always be explicitly specified by operations received on shard
        // and config servers, even if it is empty (ie. writeConcern: {}).  In this context
        // (shard/config servers) an empty WC indicates the operation should use the implicit
        // server defaults.  So, warn if the operation has not specified writeConcern and is on
        // a shard/config server.
        if (!opCtx->getClient()->isInDirectClient() &&
            (!opCtx->inMultiDocumentTransaction() || command->isTransactionCommand())) {
            if (_isInternalClient()) {
                // WriteConcern should always be explicitly specified by operations received
                // from internal clients (ie. from a mongos or mongod), even if it is empty
                // (ie. writeConcern: {}, which is equivalent to { w: 1, wtimeout: 0 }).
                uassert(4569201,
                        fmt::format("received command without explicit writeConcern on an "
                                    "internalClient connection {}",
                                    redact(_execContext.getRequest().body.toString())),
                        genericArgs.getWriteConcern());
            } else if (serverGlobalParams.clusterRole.has(ClusterRole::ShardServer) ||
                       serverGlobalParams.clusterRole.has(ClusterRole::ConfigServer)) {
                if (!genericArgs.getWriteConcern()) {
                    // TODO: Disabled until after SERVER-44539, to avoid log spam.
                    // LOGV2(21959, "Missing writeConcern on {command}", "Missing "
                    // "writeConcern on command", "command"_attr = command->getName());
                }
            }
        }

        _extractedWriteConcern.emplace(uassertStatusOK(extractWriteConcern(
            opCtx, invocation.getGenericArguments(), command->getName(), _isInternalClient())));
        if (_ecd->getSessionOptions().getAutocommit()) {
            validateWriteConcernForTransaction(*_extractedWriteConcern, invocation.definition());
        }

        // Ensure that the WC being set on the opCtx has provenance.
        invariant(_extractedWriteConcern->getProvenance().hasSource(),
                  fmt::format("unexpected unset provenance on writeConcern: {}",
                              _extractedWriteConcern->toBSON().jsonString()));

        opCtx->setWriteConcern(*_extractedWriteConcern);
    }
}

void RunCommandAndWaitForWriteConcern::_runCommandWithFailPoint() {
    // TODO SERVER-69061 rename `failWithErrorCodeInRunCommand` and remove the following comment.
    // Despite the name, this failpoint only affects commands with write concerns.
    if (auto scoped = failWithErrorCodeInRunCommand.scoped(); MONGO_unlikely(scoped.isActive())) {
        const auto errorCode = scoped.getData()["errorCode"].numberInt();
        LOGV2(21960,
              "failWithErrorCodeInRunCommand enabled, failing command",
              "errorCode"_attr = errorCode);
        BSONObjBuilder errorBuilder;
        errorBuilder.append("ok", 0.0);
        errorBuilder.append("code", errorCode);
        errorBuilder.append("errmsg", "failWithErrorCodeInRunCommand enabled.");
        _ecd->getExecutionContext().getReplyBuilder()->setCommandReply(errorBuilder.obj());
        return;
    }

    RunCommandImpl::_runCommand();
}

void RunCommandAndWaitForWriteConcern::_handleError(Status status) {
    auto opCtx = _execContext.getOpCtx();
    // Do no-op write before returning NoSuchTransaction if command has writeConcern.
    if (status.code() == ErrorCodes::NoSuchTransaction &&
        !opCtx->getWriteConcern().usedDefaultConstructedWC) {
        TransactionParticipant::performNoopWrite(opCtx, "NoSuchTransaction error");
    }
    _waitForWriteConcern(*_ecd->getExtraFieldsBuilder());
    iasserted(status);
}

void RunCommandAndWaitForWriteConcern::_checkWriteConcern() {
    auto opCtx = _execContext.getOpCtx();
    auto bb = _execContext.getReplyBuilder()->getBodyBuilder();
    _waitForWriteConcern(bb);

    // With the exception of getMores inheriting the WriteConcern from the originating command,
    // nothing in run() should change the writeConcern.
    if (_execContext.getCommand()->getLogicalOp() == LogicalOp::opGetMore) {
        dassert(!_extractedWriteConcern, "opGetMore contained unexpected extracted write concern");
    } else {
        dassert(_extractedWriteConcern, "no extracted write concern");
        dassert(opCtx->getWriteConcern() == _extractedWriteConcern,
                fmt::format("opCtx wc: {} extracted wc: {}",
                            opCtx->getWriteConcern().toBSON().jsonString(),
                            _extractedWriteConcern->toBSON().jsonString()));
    }
}

/**
 * Given the specified command, returns an effective read concern which should be used or an error
 * if the read concern is not valid for the command.
 * Note that the validation performed is not necessarily exhaustive.
 */
StatusWith<repl::ReadConcernArgs> ExecCommandDatabase::_extractReadConcern(
    OperationContext* opCtx, bool startTransaction, bool startOrContinueTransaction) {
    repl::ReadConcernArgs readConcernArgs;

    if (auto& rc = _invocation->getGenericArguments().getReadConcern()) {
        readConcernArgs = *rc;
    }
    bool clientSuppliedReadConcern = !readConcernArgs.isEmpty();
    bool customDefaultWasApplied = false;
    auto readConcernSupport = _invocation->supportsReadConcern(readConcernArgs.getLevel(),
                                                               readConcernArgs.isImplicitDefault());

    auto applyDefaultReadConcern = [&](const repl::ReadConcernArgs rcDefault) -> void {
        LOGV2_DEBUG(21955,
                    2,
                    "Applying default readConcern on command",
                    "readConcernDefault"_attr = rcDefault,
                    "command"_attr = _invocation->definition()->getName());
        readConcernArgs = std::move(rcDefault);
        // Update the readConcernSupport, since the default RC was applied.
        readConcernSupport =
            _invocation->supportsReadConcern(readConcernArgs.getLevel(), !customDefaultWasApplied);
    };

    auto shouldApplyDefaults = (startTransaction || !opCtx->inMultiDocumentTransaction()) &&
        repl::ReplicationCoordinator::get(opCtx)->getSettings().isReplSet() &&
        !opCtx->getClient()->isInDirectClient();

    if (readConcernSupport.defaultReadConcernPermit.isOK() && shouldApplyDefaults) {
        if (_isInternalClient()) {
            // ReadConcern should always be explicitly specified by operations received from
            // internal clients (ie. from a mongos or mongod), even if it is empty (ie.
            // readConcern: {}, meaning to use the implicit server defaults).
            uassert(4569200,
                    fmt::format("received command without explicit readConcern on an "
                                "internalClient connection {}",
                                redact(_execContext.getRequest().body.toString())),
                    readConcernArgs.isSpecified());
        } else if (serverGlobalParams.clusterRole.has(ClusterRole::ShardServer) ||
                   serverGlobalParams.clusterRole.has(ClusterRole::ConfigServer)) {
            if (!readConcernArgs.isSpecified()) {
                // TODO: Disabled until after SERVER-44539, to avoid log spam.
                // LOGV2(21954, "Missing readConcern on {command}", "Missing readConcern "
                // "for command", "command"_attr = _invocation->definition()->getName());
            }
        } else {
            // A member in a regular replica set.  Since these servers receive client queries, in
            // this context empty RC (ie. readConcern: {}) means the same as if absent/unspecified,
            // which is to apply the CWRWC defaults if present.  This means we just test isEmpty(),
            // since this covers both isSpecified() && !isSpecified()
            if (readConcernArgs.isEmpty()) {
                const auto rwcDefaults = ReadWriteConcernDefaults::get(opCtx).getDefault(opCtx);
                const auto rcDefault = rwcDefaults.getDefaultReadConcern();
                if (rcDefault) {
                    const auto readConcernSource = rwcDefaults.getDefaultReadConcernSource();
                    customDefaultWasApplied =
                        (readConcernSource &&
                         readConcernSource.value() == DefaultReadConcernSourceEnum::kGlobal);

                    applyDefaultReadConcern(*rcDefault);
                }
            }
        }
    }

    // Apply the implicit default read concern even if the command does not support a cluster wide
    // read concern.
    if (!readConcernSupport.defaultReadConcernPermit.isOK() &&
        readConcernSupport.implicitDefaultReadConcernPermit.isOK() && shouldApplyDefaults &&
        !_isInternalClient() && readConcernArgs.isEmpty()) {
        auto rcDefault = ReadWriteConcernDefaults::get(opCtx).getImplicitDefaultReadConcern();
        applyDefaultReadConcern(rcDefault);
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
    if (startTransaction || startOrContinueTransaction) {
        if (!isReadConcernLevelAllowedInTransaction(readConcernArgs.getLevel())) {
            return {ErrorCodes::InvalidOptions,
                    "The readConcern level must be either 'local' (default), 'majority' or "
                    "'snapshot' in "
                    "order to run in a transaction"};
        }
        if (readConcernArgs.getArgsOpTime()) {
            return {ErrorCodes::InvalidOptions,
                    fmt::format("The readConcern cannot specify '{}' in a transaction",
                                repl::ReadConcernArgs::kAfterOpTimeFieldName)};
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
                fmt::format("Command {} does not support {}",
                            _invocation->definition()->getName(),
                            readConcernArgs.toString()));
        }
    }

    return readConcernArgs;
}

void ExecCommandDatabase::_initiateCommand() {
    auto opCtx = _execContext.getOpCtx();
    auto validatedTenancyScope = _execContext.getRequest().validatedTenancyScope;
    auto command = _execContext.getCommand();
    auto replyBuilder = _execContext.getReplyBuilder();
    auto& genericArgs = _invocation->getGenericArguments();

    Client* client = opCtx->getClient();

    // Start authz contract tracking before we evaluate failpoints
    auto authzSession = AuthorizationSession::get(client);
    authzSession->startContractTracking();

    if (validatedTenancyScope && validatedTenancyScope->hasAuthenticatedUser()) {
        uassert(ErrorCodes::Unauthorized,
                fmt::format("Command {} is not supported in multitenancy mode", command->getName()),
                command->allowedWithSecurityToken());
        _tokenAuthorizationSessionGuard.emplace(opCtx, validatedTenancyScope.value());
    }

    if (MONGO_unlikely(isHello())) {
        auto& request = _execContext.getRequest();
        // Preload generic ClientMetadata ahead of our first hello request. After the first
        // request, metaElement should always be empty.
        auto metaElem = request.body[kMetadataDocumentName];
        auto isInternalClient = request.body["internalClient"_sd].ok();
        ClientMetadata::setFromMetadata(opCtx->getClient(), metaElem, isInternalClient);
    }

    if (auto clientMetadata = ClientMetadata::get(client)) {
        auto& apiParams = APIParameters::get(opCtx);
        auto& apiVersionMetrics = APIVersionMetrics::get(opCtx->getServiceContext());
        auto appName = clientMetadata->getApplicationName();
        apiVersionMetrics.update(appName, apiParams);
    }

    auto const replCoord = repl::ReplicationCoordinator::get(opCtx);

    _sessionOptions = initializeOperationSessionInfo(
        opCtx,
        validatedTenancyScope.map([](auto& scope) { return scope.tenantId(); }),
        generic_argument_util::getOperationSessionInfoFromClient(genericArgs),
        command->requiresAuth(),
        command->attachLogicalSessionsToOpCtx(),
        replCoord->getSettings().isReplSet());

    CommandHelpers::evaluateFailCommandFailPoint(opCtx, getInvocation());

    const auto dbName = CurOp::get(opCtx)->getNSS().dbName();
    uassert(ErrorCodes::InvalidNamespace,
            fmt::format("Invalid database name: '{}'", dbName.toStringForErrorMsg()),
            DatabaseName::isValid(dbName, DatabaseName::DollarInDbNameBehavior::Allow));

    const auto allowTransactionsOnConfigDatabase =
        (serverGlobalParams.clusterRole.has(ClusterRole::ConfigServer) ||
         serverGlobalParams.clusterRole.has(ClusterRole::ShardServer)) ||
        client->isFromSystemConnection();

    validateSessionOptions(
        _sessionOptions, command, _invocation->allNamespaces(), allowTransactionsOnConfigDatabase);

    if (auto& commentField = genericArgs.getComment()) {
        stdx::lock_guard<Client> lk(*client);
        opCtx->setComment(commentField->getElement().wrap());
    }

    if (genericArgs.getMaxTimeMSOpOnly()) {
        uassert(ErrorCodes::InvalidOptions,
                "Can not specify maxTimeMSOpOnly for non internal clients",
                _isInternalClient());
    }

    if (MONGO_unlikely(genericArgs.getHelp().value_or(false))) {
        // We disable not-primary-error tracker for help requests due to SERVER-11492, because
        // config servers use help requests to determine which commands are database writes, and so
        // must be forwarded to all config servers.
        NotPrimaryErrorTracker::get(opCtx->getClient()).disable();
        Command::generateHelpResponse(opCtx, replyBuilder, *command);
        iasserted(Status(ErrorCodes::SkipCommandExecution,
                         "Skipping command execution for help request"));
    }

    _invocation->checkAuthorization(opCtx, _execContext.getRequest());

    boost::optional<rss::consensus::WriteIntentGuard> writeGuard;

    if (!opCtx->getClient()->isInDirectClient() &&
        !MONGO_unlikely(skipCheckingForNotPrimaryInCommandDispatch.shouldFail())) {
        const bool inMultiDocumentTransaction = (_sessionOptions.getAutocommit() == false);

        // Kill this operation on step down even if it hasn't taken write locks yet, because it
        // could conflict with transactions from a new primary.
        if (inMultiDocumentTransaction) {
            hangBeforeSettingTxnInterruptFlag.pauseWhileSet();
            opCtx->setAlwaysInterruptAtStepDownOrUp_UNSAFE();
        }

        auto allowed = command->secondaryAllowed(opCtx->getServiceContext());
        bool alwaysAllowed = allowed == Command::AllowedOnSecondary::kAlways;
        bool couldHaveOptedIn =
            allowed == Command::AllowedOnSecondary::kOptIn && !inMultiDocumentTransaction;
        bool optedIn = couldHaveOptedIn && ReadPreferenceSetting::get(opCtx).canRunOnSecondary();
        bool canRunHere = commandCanRunHere(opCtx, dbName, command, inMultiDocumentTransaction);
        if (!canRunHere && couldHaveOptedIn) {
            const auto msg = client->supportsHello() ? "not primary and secondaryOk=false"_sd
                                                     : "not master and slaveOk=false"_sd;
            uasserted(ErrorCodes::NotPrimaryNoSecondaryOk, msg);
        }

        if (MONGO_unlikely(respondWithNotPrimaryInCommandDispatch.shouldFail())) {
            uassert(ErrorCodes::NotWritablePrimary, "not primary", canRunHere);
        } else {
            const auto msg = client->supportsHello() ? "not primary"_sd : "not master"_sd;
            uassert(ErrorCodes::NotWritablePrimary, msg, canRunHere);
        }

        if (!command->maintenanceOk() && replCoord->getSettings().isReplSet() &&
            !replCoord->canAcceptWritesForDatabase_UNSAFE(opCtx, dbName) &&
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

        // We acquire the RSTL which helps us here in two ways:
        // 1) It forces us to wait out any outstanding stepdown attempts.
        // 2) It guarantees that future RSTL holders will see the
        // 'setAlwaysInterruptAtStepDownOrUp_UNSAFE' flag we set above.
        if (inMultiDocumentTransaction) {
            hangAfterCheckingWritabilityForMultiDocumentTransactions.pauseWhileSet();
            // Declaring Write intent ensures we are the primary node and this operation will be
            // interrupted by StepDown.
            if (gFeatureFlagIntentRegistration.isEnabled()) {
                writeGuard.emplace(opCtx);
            }
            repl::ReplicationStateTransitionLockGuard rstl(opCtx, MODE_IX);
            uassert(ErrorCodes::NotWritablePrimary,
                    "Cannot start a transaction in a non-primary state",
                    replCoord->canAcceptWritesForDatabase(opCtx, dbName));
        }
    }

    if (command->adminOnly()) {
        LOGV2_DEBUG(21961, 2, "Admin only command", "command"_attr = command->getName());
    }

    if (command->shouldAffectCommandCounter()) {
        serviceOpCounters(opCtx).gotCommand();
        if (analyze_shard_key::supportsSamplingQueries(opCtx)) {
            analyze_shard_key::QueryAnalysisSampler::get(opCtx).gotCommand(command->getName());
        }
    }

    if (command->shouldAffectQueryCounter()) {
        serviceOpCounters(opCtx).gotQuery();
    }

    auto [requestOrDefaultMaxTimeMS, usesDefaultMaxTimeMS] = getRequestOrDefaultMaxTimeMS(
        opCtx, genericArgs.getMaxTimeMS(), getInvocation()->isReadOperation());
    if (requestOrDefaultMaxTimeMS || genericArgs.getMaxTimeMSOpOnly()) {
        // Parse the 'maxTimeMS' command option, and use it to set a deadline for the operation on
        // the OperationContext. The 'maxTimeMS' option unfortunately has a different meaning for a
        // getMore command, where it is used to communicate the maximum time to wait for new inserts
        // on tailable cursors, not as a deadline for the operation.
        //
        // TODO SERVER-34277 Remove the special handling for maxTimeMS for getMores. This will
        // require introducing a new 'max await time' parameter for getMore, and eventually banning
        // maxTimeMS altogether on a getMore command.
        const auto maxTimeMS = requestOrDefaultMaxTimeMS.value_or(Milliseconds{0});
        const auto maxTimeMSOpOnly = Milliseconds(genericArgs.getMaxTimeMSOpOnly().value_or(0));

        if ((maxTimeMS > Milliseconds::zero() || maxTimeMSOpOnly > Milliseconds::zero()) &&
            command->getLogicalOp() != LogicalOp::opGetMore) {
            uassert(40119,
                    "Illegal attempt to set operation deadline within DBDirectClient",
                    !opCtx->getClient()->isInDirectClient());

            // The "hello" command should not inherit the deadline from the user op it is operating
            // as a part of as that can interfere with replica set monitoring and host selection.
            const bool ignoreMaxTimeMSOpOnly = isHello();

            if (!ignoreMaxTimeMSOpOnly && maxTimeMSOpOnly > Milliseconds::zero() &&
                (maxTimeMS == Milliseconds::zero() || maxTimeMSOpOnly < maxTimeMS)) {
                opCtx->storeMaxTimeMS(maxTimeMS);
                opCtx->setDeadlineByDate(_execContext.getStarted() + maxTimeMSOpOnly,
                                         ErrorCodes::MaxTimeMSExpired);
            } else if (maxTimeMS > Milliseconds::zero()) {
                opCtx->setDeadlineByDate(_execContext.getStarted() + maxTimeMS,
                                         ErrorCodes::MaxTimeMSExpired);
            }
            opCtx->setUsesDefaultMaxTimeMS(genericArgs.getUsesDefaultMaxTimeMS().value_or(false) ||
                                           usesDefaultMaxTimeMS);
        }
    }

    const auto isProcessInternalCommand = isProcessInternalClient(*opCtx->getClient());

    if (gIngressAdmissionControlEnabled.load()) {
        // The way ingress admission works, one ticket should cover all the work for the operation.
        // Therefore, if the operation has already been admitted by IngressAdmissionController, all
        // of the subsequent admissions of the same operation (e.g. via DBDirectClient) should be
        // exempt from ingress admission control.
        boost::optional<ScopedAdmissionPriority<IngressAdmissionContext>> admissionPriority;
        if (isProcessInternalCommand || !_invocation->isSubjectToIngressAdmissionControl() ||
            IngressAdmissionContext::get(opCtx).isHoldingTicket()) {
            admissionPriority.emplace(opCtx, AdmissionContext::Priority::kExempt);
        }
        auto& admissionController = IngressAdmissionController::get(opCtx);
        _admissionTicket = admissionController.admitOperation(opCtx);
    }

    auto& readConcernArgs = repl::ReadConcernArgs::get(opCtx);

    // If the operation is being executed as part of DBDirectClient this means we must use the
    // original read concern.
    auto skipReadConcern = opCtx->getClient()->isInDirectClient();
    bool startTransaction = static_cast<bool>(_sessionOptions.getStartTransaction());
    bool startOrContinueTransaction =
        static_cast<bool>(_sessionOptions.getStartOrContinueTransaction());
    if (!skipReadConcern) {
        auto newReadConcernArgs = uassertStatusOK(
            _extractReadConcern(opCtx, startTransaction, startOrContinueTransaction));

        // Ensure that the RC being set on the opCtx has provenance.
        invariant(newReadConcernArgs.getProvenance().hasSource(),
                  fmt::format("unexpected unset provenance on readConcern: {}",
                              newReadConcernArgs.toBSONInner().toString()));

        // If startOrContinueTransaction is set, that means that the shard is trying to
        // add another shard to the transaction, so the command should allow specifying a
        // readConcern even if it is not the first command in the transaction.
        uassert(ErrorCodes::InvalidOptions,
                "Only the first command in a transaction may specify a readConcern",
                startTransaction || startOrContinueTransaction ||
                    !opCtx->inMultiDocumentTransaction() || newReadConcernArgs.isEmpty());

        {
            // We must obtain the client lock to set the ReadConcernArgs on the operation context as
            // it may be concurrently read by CurrentOp.
            stdx::lock_guard<Client> lk(*opCtx->getClient());
            readConcernArgs = std::move(newReadConcernArgs);
        }
    }

    uassert(ErrorCodes::InvalidOptions,
            "Command does not support the rawData option",
            !genericArgs.getRawData() || _invocation->supportsRawData() ||
                client->isInternalClient());
    uassert(ErrorCodes::InvalidOptions,
            "rawData is not enabled",
            !genericArgs.getRawData() || gFeatureFlagRawDataCrudOperations.isEnabled());

    if (opCtx->isStartingMultiDocumentTransaction()) {
        _setLockStateForTransaction(opCtx);
    }

    // Once API params and txn state are set on opCtx, enforce the "requireApiVersion" setting.
    enforceRequireAPIVersion(opCtx, command, _execContext.getRequest());

    if (!opCtx->getClient()->isInDirectClient()) {
        const boost::optional<ShardVersion>& shardVersion = genericArgs.getShardVersion();
        const boost::optional<DatabaseVersion>& databaseVersion = genericArgs.getDatabaseVersion();

        if (shardVersion || databaseVersion) {
            if (readConcernArgs.getLevel() == repl::ReadConcernLevel::kAvailableReadConcern) {
                OperationShardingState::get(opCtx).setTreatAsFromRouter();
            } else {
                const auto invocationNss = _invocation->ns();
                OperationShardingState::setShardRole(
                    opCtx, invocationNss, shardVersion, databaseVersion);

                // If a timeseries collection is sharded, only the buckets collection would be
                // sharded. We expect all versioned commands to be sent over 'system.buckets'
                // namespace. But it is possible that a stale mongos may send the request over a
                // view namespace. In this case, we initialize the 'OperationShardingState' with
                // both the invocation and buckets namespaces.
                // TODO: SERVER-80719 revisit this.
                auto bucketNss = invocationNss.makeTimeseriesBucketsNamespace();
                auto catalog = CollectionCatalog::get(opCtx);
                auto coll = catalog->lookupCollectionByNamespace(opCtx, bucketNss);

                if (coll && coll->getTimeseriesOptions()) {
                    OperationShardingState::setShardRole(opCtx, bucketNss, shardVersion, {});
                }
            }
        }
    }

    // This may trigger the maxTimeAlwaysTimeOut failpoint.
    auto status = opCtx->checkForInterruptNoAssert();

    // We still proceed if the primary stepped down, but accept other kinds of interruptions. We
    // defer to individual commands to allow themselves to be interruptible by stepdowns, since
    // commands like 'voteRequest' should conversely continue executing.
    if (status != ErrorCodes::PrimarySteppedDown &&
        status != ErrorCodes::InterruptedDueToReplStateChange) {
        uassertStatusOK(status);
    }

    command->incrementCommandsExecuted();
}

void ExecCommandDatabase::_commandExec() {
    auto opCtx = _execContext.getOpCtx();

    // If this command should start a new transaction, waitForReadConcern will be invoked
    // after invoking the TransactionParticipant, which will determine whether a transaction
    // is being started or continued.
    if (!opCtx->inMultiDocumentTransaction()) {
        service_entry_point_shard_role_helpers::waitForReadConcern(
            opCtx, getInvocation(), _execContext.getRequest());
    }
    service_entry_point_shard_role_helpers::setPrepareConflictBehaviorForReadConcern(
        opCtx, getInvocation());

    _extraFieldsBuilder.resetToEmpty();
    _execContext.getReplyBuilder()->reset();

    try {
        // TODO (SERVER-90204) Replace with a more accurate check of whether the command is coming
        // from a router.
        if (OperationShardingState::isComingFromRouter(opCtx)) {
            ShardingState::get(opCtx)->assertCanAcceptShardedCommands();
        }
        _runCommandOpTimes.emplace(opCtx);
        if (getInvocation()->supportsWriteConcern() ||
            getInvocation()->definition()->getLogicalOp() == LogicalOp::opGetMore) {
            // getMore operations inherit a WriteConcern from their originating cursor. For example,
            // if the originating command was an aggregate with a $out and batchSize: 0. Note that
            // if the command only performed reads then we will not need to wait at all.
            RunCommandAndWaitForWriteConcern runner(this);
            runner.run();
        } else {
            RunCommandImpl runner(this);
            runner.run();
        }
    } catch (const DBException& ex) {
        // If the command has failed, there is no need to look for write errors at the oss.
        OperationShardingState::get(opCtx).resetShardingOperationFailedStatus();

        const auto metadataRefreshStatus = _refreshIfNeeded(ex.toStatus());
        const auto refreshed = uassertStatusOK(metadataRefreshStatus);

        const auto waitedForInitialized = _awaitShardingInitializedIfNeeded(ex.toStatus());

        if ((refreshed || waitedForInitialized) && canRetryCommand(ex.toStatus())) {
            _resetLockerStateAfterShardingUpdate(opCtx);
            _commandExec();
            return;
        }

        throw;
    }

    // Regardless if the command has succeeded, it needs to check if the operation sharding state
    // has some stale config errors to be handled before returning to the router.
    if (auto writeError = OperationShardingState::get(opCtx).resetShardingOperationFailedStatus()) {
        const auto metadataRefreshStatus = _refreshIfNeeded(*writeError);
        if (!metadataRefreshStatus.isOK() &&
            ErrorCodes::isInterruption(metadataRefreshStatus.getStatus())) {
            uassertStatusOK(metadataRefreshStatus);
        }
    }
}

StatusWith<bool> ExecCommandDatabase::_refreshIfNeeded(const Status& execError) {
    auto opCtx = _execContext.getOpCtx();

    tassert(8462308, "Expected to find an error in the status of the command", !execError.isOK());

    if (execError == ErrorCodes::StaleConfig) {
        ShardingStatistics::get(opCtx).countStaleConfigErrors.addAndFetch(1);
    }

    if (opCtx->getClient()->isInDirectClient()) {
        return false;
    }

    if (execError == ErrorCodes::StaleDbVersion && !_refreshedDatabase) {
        const auto staleInfo = execError.extraInfo<StaleDbRoutingVersion>();
        tassert(8462303, "StaleDbVersion must have extraInfo", staleInfo);
        const auto stableLocalVersion =
            !staleInfo->getCriticalSectionSignal() && staleInfo->getVersionWanted();

        if (stableLocalVersion && staleInfo->getVersionReceived() < staleInfo->getVersionWanted()) {
            // The shard is recovered and the router is staler than the shard, so we cannot retry
            // locally.
            return false;
        }

        const auto refreshStatus =
            service_entry_point_shard_role_helpers::refreshDatabase(opCtx, *staleInfo);
        if (refreshStatus.isOK()) {
            _refreshedDatabase = true;
            return true;
        } else {
            LOGV2_WARNING(
                8462300,
                "Failed to refresh database metadata cache while handling StaleDbVersion exception",
                "error"_attr = redact(refreshStatus));
            return refreshStatus;
        }
    } else if (execError == ErrorCodes::StaleConfig && !_refreshedCollection) {
        const auto staleInfo = execError.extraInfo<StaleConfigInfo>();
        tassert(8462304, "StaleConfig must have extraInfo", staleInfo);
        const auto inCriticalSection = staleInfo->getCriticalSectionSignal().has_value();
        const auto stableLocalVersion = !inCriticalSection && staleInfo->getVersionWanted();

        if (stableLocalVersion &&
            ShardVersion::isPlacementVersionIgnored(staleInfo->getVersionReceived())) {
            // Shard is recovered, but the router didn't sent a shard version, therefore we just
            // need to tell the router how much it needs to advance to (getVersionWanted).
            return false;
        }

        if (stableLocalVersion &&
            (staleInfo->getVersionReceived().placementVersion() <=>
             staleInfo->getVersionWanted()->placementVersion()) == std::partial_ordering::less) {
            // Shard is recovered and the router is staler than the shard.
            return false;
        }

        if (inCriticalSection) {
            service_entry_point_shard_role_helpers::handleReshardingCriticalSectionMetrics(
                opCtx, *staleInfo);
        }

        const auto refreshStatus =
            service_entry_point_shard_role_helpers::refreshCollection(opCtx, *staleInfo);

        // Fail the direct shard operation so that a RetryableWriteError label can be returned and
        // the write can be retried by the driver.
        const auto fromRouter = OperationShardingState::isComingFromRouter(opCtx);
        if (opCtx->isRetryableWrite() && !fromRouter) {
            return false;
        }

        if (refreshStatus.isOK()) {
            _refreshedCollection = true;
            return true;
        } else {
            LOGV2_WARNING(
                8462301,
                "Failed to refresh collection metadata cache while handling StaleConfig exception",
                "error"_attr = redact(refreshStatus));
            return refreshStatus;
        }
    } else if (execError == ErrorCodes::ShardCannotRefreshDueToLocksHeld &&
               !_refreshedCatalogCache) {
        const auto refreshInfo = execError.extraInfo<ShardCannotRefreshDueToLocksHeldInfo>();
        tassert(8462305, "ShardCannotRefreshDueToLocksHeld must have extraInfo", refreshInfo);
        invariant(!shard_role_details::getLocker(opCtx)->isLocked());

        const auto refreshStatus =
            service_entry_point_shard_role_helpers::refreshCatalogCache(opCtx, *refreshInfo);
        if (refreshStatus.isOK()) {
            _refreshedCatalogCache = true;
            return true;
        } else {
            LOGV2_WARNING(8462302,
                          "Failed to refresh catalog cache while handling "
                          "ShardCannotRefreshDueToLocksHeld exception",
                          "error"_attr = redact(refreshStatus));
            return refreshStatus;
        }
    }

    return false;
}

bool ExecCommandDatabase::_awaitShardingInitializedIfNeeded(const Status& status) {
    auto opCtx = _execContext.getOpCtx();

    // If this node hasn't even been started with --shardsvr then there is no chance sharding can
    // be initialized so there is no point waiting.
    // TODO (SERVER-103081): non-shardsvr nodes should not receive the ShardingStateNotInitialized
    // error.
    if (opCtx->getClient()->isInDirectClient() ||
        !serverGlobalParams.clusterRole.has(ClusterRole::ShardServer)) {
        return false;
    }

    if (status == ErrorCodes::ShardingStateNotInitialized && !_awaitedShardingInitialization) {
        sharding::awaitShardRoleReady(opCtx);
        _awaitedShardingInitialization = true;
        return true;
    }
    return false;
}

bool ExecCommandDatabase::canRetryCommand(const Status& execError) {
    auto opCtx = _execContext.getOpCtx();

    tassert(8462309, "Expected to find an error in the status of the command", !execError.isOK());
    tassert(8462310,
            "Expected to not be in a direct client connection",
            !opCtx->getClient()->isInDirectClient());

    if (opCtx->isContinuingMultiDocumentTransaction() || _cannotRetry) {
        return false;
    }

    if (execError == ErrorCodes::StaleDbVersion) {
        const auto staleInfo = execError.extraInfo<StaleDbRoutingVersion>();
        tassert(8462306, "StaleDbVersion must have extraInfo", staleInfo);
        const auto inCriticalSection = staleInfo->getCriticalSectionSignal().has_value();

        return !inCriticalSection;
    }

    if (execError == ErrorCodes::ShardingStateNotInitialized) {
        // We can retry commands with attached shard versions because we know that the check for
        // sharding initialization happened before anything else. For other commands (ie. those
        // which access the grid before it is ready), we do not know what the commands did before
        // accessing the grid and so retrying is not necessarily safe.
        // TODO (SERVER-90204) Replace with a more accurate check of whether the command is coming
        // from a router.
        return OperationShardingState::isComingFromRouter(opCtx);
    }

    const auto canRetryCmd = _invocation->canRetryOnStaleConfigOrShardCannotRefreshDueToLocksHeld(
        _execContext.getRequest());

    if (execError == ErrorCodes::StaleConfig) {
        const auto staleInfo = execError.extraInfo<StaleConfigInfo>();
        tassert(8462307, "StaleConfig must have extraInfo", staleInfo);
        const auto inCriticalSection = staleInfo->getCriticalSectionSignal().has_value();

        return !inCriticalSection && canRetryCmd;
    } else if (execError == ErrorCodes::ShardCannotRefreshDueToLocksHeld) {
        return canRetryCmd;
    }

    return false;
}

void ExecCommandDatabase::_handleFailure(Status status) {
    // Absorb the exception as the command execution has already been skipped.
    if (status.code() == ErrorCodes::SkipCommandExecution)
        return;

    auto opCtx = _execContext.getOpCtx();
    auto command = _execContext.getCommand();
    auto replyBuilder = _execContext.getReplyBuilder();
    onCommandFinished();

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
                                        _isInternalClient(),
                                        getLastOpBeforeRun(),
                                        getLastOpAfterRun());

    BSONObjBuilder metadataBob;
    service_entry_point_shard_role_helpers::appendReplyMetadata(
        opCtx, _invocation->getGenericArguments(), &metadataBob);

    // The read concern may not have yet been placed on the operation context, so attempt to parse
    // it here, so if it is valid it can be used to compute the proper operationTime.
    auto& readConcernArgs = repl::ReadConcernArgs::get(opCtx);
    if (readConcernArgs.isEmpty()) {
        auto readConcernArgsStatus = _extractReadConcern(
            opCtx, false /*startTransaction*/, false /* startOrContinueTransaction */);
        if (readConcernArgsStatus.isOK()) {
            // We must obtain the client lock to set the ReadConcernArgs on the operation context as
            // it may be concurrently read by CurrentOp.
            stdx::lock_guard<Client> lk(*opCtx->getClient());
            readConcernArgs = readConcernArgsStatus.getValue();
        }
    }
    appendClusterAndOperationTime(opCtx, &_extraFieldsBuilder, &metadataBob, _startOperationTime);

    const auto logLevel = MONGO_unlikely(TestingProctor::instance().isEnabled()) ? 0 : 1;
    LOGV2_DEBUG(21962,
                logLevel,
                "Assertion while executing command",
                "command"_attr = _execContext.getRequest().getCommandName(),
                "db"_attr = _execContext.getRequest().readDatabaseForLogging(),
                "commandArgs"_attr =
                    redact(service_entry_point_shard_role_helpers::getRedactedCopyForLogging(
                        command, _execContext.getRequest().body)),
                "error"_attr = redact(status.toString()));

    generateErrorResponse(
        opCtx, replyBuilder, status, metadataBob.obj(), _extraFieldsBuilder.obj());

    if (ErrorCodes::isA<ErrorCategory::CloseConnectionError>(status.code())) {
        // Rethrow the exception to the top to signal that the client connection should be closed.
        iasserted(status);
    }
}

void parseCommand(HandleRequest::ExecutionContext& execContext) try {
    const auto& msg = execContext.getMessage();
    auto client = execContext.getOpCtx()->getClient();
    auto opMsgReq = rpc::opMsgRequestFromAnyProtocol(msg, client);

    if (msg.operation() == dbQuery) {
        checkAllowedOpQueryCommand(*client, opMsgReq.getCommandName());
    }
    execContext.setRequest(opMsgReq);
} catch (const DBException& ex) {
    // Need to set request as `makeCommandResponse` expects an empty request on failure.
    execContext.setRequest({});

    // Otherwise, reply with the parse error. This is useful for cases where parsing fails due to
    // user-supplied input, such as the document too deep error. Since we failed during parsing, we
    // can't log anything about the command.
    LOGV2_DEBUG(21963, 1, "Assertion while parsing command", "error"_attr = ex.toString());
    throw;
}

void executeCommand(HandleRequest::ExecutionContext& execContext) {
    // Prepare environment for command execution (e.g., find command object in registry)
    auto opCtx = execContext.getOpCtx();
    auto& request = execContext.getRequest();

    // In the absence of a Command object, no redaction is possible. Therefore to avoid
    // displaying potentially sensitive information in the logs, we restrict the log
    // message to the name of the unrecognized command. However, the complete command
    // object will still be echoed to the client.
    if (execContext.setCommand(CommandHelpers::findCommand(opCtx, request.getCommandName()));
        !execContext.getCommand()) {
        getCommandRegistry(opCtx)->incrementUnknownCommands();
        LOGV2_DEBUG(
            21964, 2, "Command not found in registry", "command"_attr = request.getCommandName());
        iasserted(Status(ErrorCodes::CommandNotFound,
                         fmt::format("no such command: '{}'", request.getCommandName())));
    }

    Command* c = execContext.getCommand();
    LOGV2_DEBUG(
        21965,
        2,
        "About to run the command",
        "db"_attr = request.readDatabaseForLogging(),
        "client"_attr = (opCtx->getClient() && opCtx->getClient()->hasRemote()
                             ? opCtx->getClient()->getRemote().toString()
                             : ""),
        "commandArgs"_attr = redact(
            service_entry_point_shard_role_helpers::getRedactedCopyForLogging(c, request.body)));

    opCtx->setExhaust(OpMsg::isFlagSet(execContext.getMessage(), OpMsg::kExhaustSupported));

    try {
        ExecCommandDatabase runner(execContext);
        runner.run();
    } catch (const DBException& ex) {
        LOGV2_DEBUG(21966,
                    1,
                    "Assertion while executing command",
                    "command"_attr = execContext.getRequest().getCommandName(),
                    "db"_attr = execContext.getRequest().readDatabaseForLogging(),
                    "error"_attr = ex.toStatus().toString());
        throw;
    }
}

DbResponse makeCommandResponse(const HandleRequest::ExecutionContext& execContext) {
    auto opCtx = execContext.getOpCtx();
    const Message& message = execContext.getMessage();
    const Command* c = execContext.getCommand();
    auto replyBuilder = execContext.getReplyBuilder();

    if (OpMsg::isFlagSet(message, OpMsg::kMoreToCome)) {
        // Close the connection to get client to go through server selection again.
        if (NotPrimaryErrorTracker::get(opCtx->getClient()).hadError()) {
            if (c && c->getReadWriteType() == Command::ReadWriteType::kWrite)
                notPrimaryUnackWrites.increment();

            uasserted(ErrorCodes::NotWritablePrimary,
                      fmt::format("Not-primary error while processing '{}' operation  on '{}' "
                                  "database via fire-and-forget command execution.",
                                  execContext.getRequest().getCommandName(),
                                  execContext.getRequest().readDatabaseForLogging()));
        }
        return {};  // Don't reply.
    }

    DbResponse dbResponse;
    if constexpr (kDebugBuild) {
        CommandHelpers::checkForInternalError(replyBuilder, execContext.isInternalClient());
    }

    if (OpMsg::isFlagSet(message, OpMsg::kExhaustSupported)) {
        auto responseObj = replyBuilder->getBodyBuilder().asTempObj();

        if (responseObj.getField("ok").trueValue()) {
            dbResponse.shouldRunAgainForExhaust = replyBuilder->shouldRunAgainForExhaust();
            dbResponse.nextInvocation = replyBuilder->getNextInvocation();
        }
    }

    try {
        dbResponse.response = replyBuilder->done();
    } catch (const ExceptionFor<ErrorCodes::BSONObjectTooLarge>& ex) {
        // Create a new reply builder as subsequently calling any methods on a builder after
        // 'done()' results in undefined behavior.
        auto errorReplyBuilder = execContext.getReplyBuilder();
        BSONObjBuilder metadataBob;
        BSONObjBuilder extraFieldsBuilder;
        appendClusterAndOperationTime(
            opCtx, &extraFieldsBuilder, &metadataBob, LogicalTime::kUninitialized);
        generateErrorResponse(
            opCtx, errorReplyBuilder, ex.toStatus(), metadataBob.obj(), extraFieldsBuilder.obj());
        dbResponse.response = errorReplyBuilder->done();
    }
    CurOp::get(opCtx)->debug().responseLength = dbResponse.response.header().dataLen();

    return dbResponse;
}

DbResponse receivedCommands(HandleRequest::ExecutionContext& execContext) {
    try {
        execContext.setReplyBuilder(
            rpc::makeReplyBuilder(rpc::protocolForMessage(execContext.getMessage())));
        parseCommand(execContext);
        executeCommand(execContext);
    } catch (const DBException& ex) {
        auto& status = ex.toStatus();

        // If this error needs to fail the connection, propagate it out.
        if (ErrorCodes::isConnectionFatalMessageParseError(status.code())) {
            throw;
        }

        if (ErrorCodes::isA<ErrorCategory::CloseConnectionError>(status.code())) {
            // Return the exception to the top to signal that the client connection should be
            // closed.
            throw;
        }

        auto opCtx = execContext.getOpCtx();
        BSONObjBuilder metadataBob;
        BSONObjBuilder extraFieldsBuilder;
        appendClusterAndOperationTime(
            opCtx, &extraFieldsBuilder, &metadataBob, LogicalTime::kUninitialized);

        auto replyBuilder = execContext.getReplyBuilder();
        generateErrorResponse(
            opCtx, replyBuilder, status, metadataBob.obj(), extraFieldsBuilder.obj());
    };

    return makeCommandResponse(execContext);
}

void HandleRequest::startOperation() {
    auto opCtx = executionContext.getOpCtx();
    auto& client = executionContext.client();

    CurOp::get(opCtx)->ensureStarted();

    if (client.isInDirectClient()) {
        if (!opCtx->getLogicalSessionId() || !opCtx->getTxnNumber()) {
            invariant(!opCtx->inMultiDocumentTransaction() &&
                      !shard_role_details::getLocker(opCtx)->inAWriteUnitOfWork());
        }
    } else {
        NotPrimaryErrorTracker::get(client).startRequest();
        AuthorizationSession::get(client)->startRequest(opCtx);

        // We should not be holding any locks at this point
        invariant(!shard_role_details::getLocker(opCtx)->isLocked());
    }
}

DbResponse HandleRequest::runOperation() {
    switch (executionContext.op()) {
        case dbQuery:
            if (!executionContext.nsString().isCommand())
                return makeErrorResponseToUnsupportedOpQuery("OP_QUERY is no longer supported");
            // Fallthrough because it's a query containing a command. Ideally, we'd like to let
            // through only hello|isMaster commands but at this point the command hasn't been
            // parsed yet, so we don't know what it is.
            [[fallthrough]];
        case dbMsg:
            return receivedCommands(executionContext);
        case dbGetMore:
            return makeErrorResponseToUnsupportedOpQuery("OP_GET_MORE is no longer supported");
        case dbKillCursors:
            uasserted(5745703, "OP_KILL_CURSORS is no longer supported");
        case dbInsert:
            uasserted(5745702, "OP_INSERT is no longer supported");
        case dbUpdate:
            uasserted(5745701, "OP_UPDATE is no longer supported");
        case dbDelete:
            uasserted(5745700, "OP_DELETE is no longer supported");
        default:
            // For compatibility reasons, we only log incidents of receiving operations that are not
            // supported and return an empty response to the caller.
            LOGV2(21968,
                  "Operation is not supported",
                  "operation"_attr = static_cast<int>(executionContext.op()));
            executionContext.currentOp().done();
            executionContext.forceLog = true;
            return {};
    }
}

void HandleRequest::completeOperation(DbResponse& response) {
    auto opCtx = executionContext.getOpCtx();
    auto& currentOp = executionContext.currentOp();

    // Mark the op as complete, and log it if appropriate. Returns a boolean indicating whether
    // this op should be written to the profiler.
    const bool shouldProfile = currentOp.completeAndLogOperation(
        {MONGO_LOGV2_DEFAULT_COMPONENT},
        DatabaseProfileSettings::get(opCtx->getServiceContext())
            .getDatabaseProfileSettings(currentOp.getNSS().dbName())
            .filter,
        response.response.size(),
        executionContext.slowMsOverride,
        executionContext.forceLog);

    ServiceLatencyTracker::getDecoration(opCtx->getService())
        .increment(opCtx,
                   currentOp.elapsedTimeExcludingPauses(),
                   currentOp.debug().workingTimeMillis,
                   currentOp.getReadWriteType());

    if (shouldProfile) {
        // Performance profiling is on
        if (shard_role_details::getLocker(opCtx)->isReadLocked()) {
            LOGV2_DEBUG(21970, 1, "Note: not profiling because of recursive read lock");
        } else if (executionContext.client().isInDirectClient()) {
            LOGV2_DEBUG(21971, 1, "Note: not profiling because we are in DBDirectClient");
        } else if (service_entry_point_shard_role_helpers::lockedForWriting()) {
            LOGV2_DEBUG(21972, 1, "Note: not profiling because doing fsync+lock");
        } else if (opCtx->readOnly()) {
            LOGV2_DEBUG(21973, 1, "Note: not profiling because server is read-only");
        } else {
            invariant(!shard_role_details::getLocker(opCtx)->inAWriteUnitOfWork());
            profile_collection::profile(opCtx, executionContext.op());
        }
    }

    recordCurOpMetrics(opCtx);

    const auto& ldapOperationStatsSnapshot =
        CurOp::get(opCtx)->getUserAcquisitionStats()->getLdapOperationStatsSnapshot();
    if (ldapOperationStatsSnapshot.shouldReport()) {
        auto ldapCumulativeOperationsStats = LDAPCumulativeOperationStats::get();
        if (ldapCumulativeOperationsStats) {
            ldapCumulativeOperationsStats->recordOpStats(ldapOperationStatsSnapshot);
        }
    }
}

}  // namespace

void logHandleRequestFailure(const Status& status) {
    LOGV2_INFO(4879802, "Failed to handle request", "error"_attr = redact(status));
}

void onHandleRequestException(const HandleRequest::ExecutionContext& context,
                              const Status& status) {
    auto isMirrorOp = [&] {
        const auto& obj = context.getRequest().body;
        if (auto e = obj.getField("mirrored"); MONGO_unlikely(e.ok() && e.boolean()))
            return true;
        return false;
    };

    // TODO SERVER-70510 revert changes introduced by SERVER-60553 that suppresses errors occurred
    // during handling of mirroring operations on recovering secondaries.
    if (MONGO_unlikely(status == ErrorCodes::NotWritablePrimary && isMirrorOp()))
        return;

    logHandleRequestFailure(status);
}

Future<DbResponse> ServiceEntryPointShardRole::handleRequest(OperationContext* opCtx,
                                                             const Message& m,
                                                             Date_t started) try {
    tassert(9391501,
            "Invalid ClusterRole in ServiceEntryPointShardRole",
            opCtx->getService()->role().hasExclusively(ClusterRole::ShardServer));
    HandleRequest hr(opCtx, m, started);
    hr.startOperation();

    try {
        auto response = hr.runOperation();
        hr.completeOperation(response);
        return response;
    } catch (const DBException& ex) {
        onHandleRequestException(hr.executionContext, ex.toStatus());
        return ex.toStatus();
    }
} catch (const DBException& ex) {
    auto status = ex.toStatus();
    logHandleRequestFailure(status);
    return status;
}

}  // namespace mongo
