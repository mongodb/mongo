/**
 *    Copyright (C) 2018 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kCommand

#include "mongo/platform/basic.h"

#include "mongo/db/service_entry_point_common.h"

#include "mongo/base/checked_cast.h"
#include "mongo/bson/mutable/document.h"
#include "mongo/bson/util/bson_extract.h"
#include "mongo/db/audit.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/impersonation_session.h"
#include "mongo/db/client.h"
#include "mongo/db/command_can_run_here.h"
#include "mongo/db/commands.h"
#include "mongo/db/commands/test_commands_enabled.h"
#include "mongo/db/concurrency/global_lock_acquisition_tracker.h"
#include "mongo/db/curop.h"
#include "mongo/db/curop_metrics.h"
#include "mongo/db/cursor_manager.h"
#include "mongo/db/dbdirectclient.h"
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
#include "mongo/db/repl/optime.h"
#include "mongo/db/repl/read_concern_args.h"
#include "mongo/db/repl/repl_client_info.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/s/implicit_create_collection.h"
#include "mongo/db/s/operation_sharding_state.h"
#include "mongo/db/s/shard_filtering_metadata_refresh.h"
#include "mongo/db/s/sharded_connection_info.h"
#include "mongo/db/s/sharding_state.h"
#include "mongo/db/service_entry_point_common.h"
#include "mongo/db/session_catalog.h"
#include "mongo/db/snapshot_window_util.h"
#include "mongo/db/stats/counters.h"
#include "mongo/db/stats/top.h"
#include "mongo/rpc/factory.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/rpc/message.h"
#include "mongo/rpc/metadata.h"
#include "mongo/rpc/metadata/config_server_metadata.h"
#include "mongo/rpc/metadata/logical_time_metadata.h"
#include "mongo/rpc/metadata/oplog_query_metadata.h"
#include "mongo/rpc/metadata/repl_set_metadata.h"
#include "mongo/rpc/metadata/sharding_metadata.h"
#include "mongo/rpc/metadata/tracking_metadata.h"
#include "mongo/rpc/op_msg.h"
#include "mongo/rpc/reply_builder_interface.h"
#include "mongo/s/cannot_implicitly_create_collection_info.h"
#include "mongo/s/grid.h"
#include "mongo/s/stale_exception.h"
#include "mongo/util/fail_point_service.h"
#include "mongo/util/log.h"
#include "mongo/util/scopeguard.h"

namespace mongo {

MONGO_FP_DECLARE(failCommand);
MONGO_FP_DECLARE(rsStopGetMore);
MONGO_FP_DECLARE(respondWithNotPrimaryInCommandDispatch);
MONGO_FP_DECLARE(skipCheckingForNotMasterInCommandDispatch);

namespace {
using logger::LogComponent;

// The command names for which to check out a session. These are commands that support retryable
// writes, readConcern snapshot, or multi-statement transactions. We additionally check out the
// session for commands that can take a lock and then run another whitelisted command in
// DBDirectClient. Otherwise, the nested command would try to check out a session under a lock,
// which is not allowed.
const StringMap<int> sessionCheckoutWhitelist = {{"abortTransaction", 1},
                                                 {"aggregate", 1},
                                                 {"applyOps", 1},
                                                 {"commitTransaction", 1},
                                                 {"count", 1},
                                                 {"dbHash", 1},
                                                 {"delete", 1},
                                                 {"distinct", 1},
                                                 {"doTxn", 1},
                                                 {"eval", 1},
                                                 {"$eval", 1},
                                                 {"explain", 1},
                                                 {"filemd5", 1},
                                                 {"find", 1},
                                                 {"findandmodify", 1},
                                                 {"findAndModify", 1},
                                                 {"geoNear", 1},
                                                 {"geoSearch", 1},
                                                 {"getMore", 1},
                                                 {"group", 1},
                                                 {"insert", 1},
                                                 {"killCursors", 1},
                                                 {"mapReduce", 1},
                                                 {"parallelCollectionScan", 1},
                                                 {"prepareTransaction", 1},
                                                 {"refreshLogicalSessionCacheNow", 1},
                                                 {"update", 1}};

void generateLegacyQueryErrorResponse(const AssertionException* exception,
                                      const QueryMessage& queryMessage,
                                      CurOp* curop,
                                      Message* response) {
    curop->debug().errInfo = exception->toStatus();

    log(LogComponent::kQuery) << "assertion " << exception->toString() << " ns:" << queryMessage.ns
                              << " query:" << (queryMessage.query.valid(BSONVersion::kLatest)
                                                   ? redact(queryMessage.query)
                                                   : "query object is corrupt");
    if (queryMessage.ntoskip || queryMessage.ntoreturn) {
        log(LogComponent::kQuery) << " ntoskip:" << queryMessage.ntoskip
                                  << " ntoreturn:" << queryMessage.ntoreturn;
    }

    auto scex = exception->extraInfo<StaleConfigInfo>();

    BSONObjBuilder err;
    err.append("$err", exception->reason());
    err.append("code", exception->code());
    if (scex) {
        err.append("ok", 0.0);
        err.append("ns", scex->getns());
        scex->getVersionReceived().addToBSON(err, "vReceived");
        scex->getVersionWanted().addToBSON(err, "vWanted");
    }
    BSONObj errObj = err.done();

    if (scex) {
        log(LogComponent::kQuery) << "stale version detected during query over " << queryMessage.ns
                                  << " : " << errObj;
    }

    BufBuilder bb;
    bb.skip(sizeof(QueryResult::Value));
    bb.appendBuf((void*)errObj.objdata(), errObj.objsize());

    // TODO: call replyToQuery() from here instead of this!!! see dbmessage.h
    QueryResult::View msgdata = bb.buf();
    QueryResult::View qr = msgdata;
    qr.setResultFlags(ResultFlag_ErrSet);
    if (scex)
        qr.setResultFlags(qr.getResultFlags() | ResultFlag_ShardConfigStale);
    qr.msgdata().setLen(bb.len());
    qr.msgdata().setOperation(opReply);
    qr.setCursorId(0);
    qr.setStartingFrom(0);
    qr.setNReturned(1);
    response->setData(bb.release());
}

void registerError(OperationContext* opCtx, const DBException& exception) {
    LastError::get(opCtx->getClient()).setLastError(exception.code(), exception.reason());
    CurOp::get(opCtx)->debug().errInfo = exception.toStatus();
}

void generateErrorResponse(OperationContext* opCtx,
                           rpc::ReplyBuilderInterface* replyBuilder,
                           const DBException& exception,
                           const BSONObj& replyMetadata,
                           BSONObj extraFields = {}) {
    registerError(opCtx, exception);

    // We could have thrown an exception after setting fields in the builder,
    // so we need to reset it to a clean state just to be sure.
    replyBuilder->reset();
    replyBuilder->setCommandReply(exception.toStatus(), extraFields);
    replyBuilder->setMetadata(replyMetadata);
}

BSONObj getErrorLabels(const boost::optional<OperationSessionInfoFromClient>& sessionOptions,
                       const std::string& commandName,
                       ErrorCodes::Error code) {
    // By specifying "autocommit", the user indicates they want to run a transaction.
    if (!sessionOptions || !sessionOptions->getAutocommit()) {
        return {};
    }

    bool isRetryable = ErrorCodes::isNotMasterError(code) || ErrorCodes::isShutdownError(code);
    bool isTransientTransactionError = code == ErrorCodes::WriteConflict  //
        || code == ErrorCodes::SnapshotUnavailable                        //
        || code == ErrorCodes::NoSuchTransaction                          //
        || code == ErrorCodes::LockTimeout                                //
        // Clients can retry a single commitTransaction command, but cannot retry the whole
        // transaction if commitTransaction fails due to NotMaster.
        || (isRetryable && (commandName != "commitTransaction"));

    if (isTransientTransactionError) {
        return BSON("errorLabels" << BSON_ARRAY("TransientTransactionError"));
    }
    return {};
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
    MONGO_DISALLOW_COPYING(MaintenanceModeSetter);

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

constexpr auto kLastCommittedOpTimeFieldName = "lastCommittedOpTime"_sd;

// Called from the error contexts where request may not be available.
void appendReplyMetadataOnError(OperationContext* opCtx, BSONObjBuilder* metadataBob) {
    const bool isConfig = serverGlobalParams.clusterRole == ClusterRole::ConfigServer;
    if (ShardingState::get(opCtx)->enabled() || isConfig) {
        auto lastCommittedOpTime =
            repl::ReplicationCoordinator::get(opCtx)->getLastCommittedOpTime();
        metadataBob->append(kLastCommittedOpTimeFieldName, lastCommittedOpTime.getTimestamp());
    }
}

void appendReplyMetadata(OperationContext* opCtx,
                         const OpMsgRequest& request,
                         BSONObjBuilder* metadataBob) {
    const bool isShardingAware = ShardingState::get(opCtx)->enabled();
    const bool isConfig = serverGlobalParams.clusterRole == ClusterRole::ConfigServer;
    auto const replCoord = repl::ReplicationCoordinator::get(opCtx);
    const bool isReplSet =
        replCoord->getReplicationMode() == repl::ReplicationCoordinator::modeReplSet;

    if (isReplSet) {
        // Attach our own last opTime.
        repl::OpTime lastOpTimeFromClient =
            repl::ReplClientInfo::forClient(opCtx->getClient()).getLastOp();
        replCoord->prepareReplMetadata(request.body, lastOpTimeFromClient, metadataBob);
        // For commands from mongos, append some info to help getLastError(w) work.
        // TODO: refactor out of here as part of SERVER-18236
        if (isShardingAware || isConfig) {
            rpc::ShardingMetadata(lastOpTimeFromClient, replCoord->getElectionId())
                .writeToMetadata(metadataBob)
                .transitional_ignore();
        }

        if (isShardingAware || isConfig) {
            auto lastCommittedOpTime = replCoord->getLastCommittedOpTime();
            metadataBob->append(kLastCommittedOpTimeFieldName, lastCommittedOpTime.getTimestamp());
        }
    }

    // If we're a shard other than the config shard, attach the last configOpTime we know about.
    if (isShardingAware && !isConfig) {
        auto opTime = Grid::get(opCtx)->configOpTime();
        rpc::ConfigServerMetadata(opTime).writeToMetadata(metadataBob);
    }
}

/**
 * Given the specified command, returns an effective read concern which should be used or an error
 * if the read concern is not valid for the command.
 */
StatusWith<repl::ReadConcernArgs> _extractReadConcern(const CommandInvocation* invocation,
                                                      const BSONObj& cmdObj,
                                                      bool upconvertToSnapshot) {
    repl::ReadConcernArgs readConcernArgs;

    auto readConcernParseStatus = readConcernArgs.initialize(cmdObj);
    if (!readConcernParseStatus.isOK()) {
        return readConcernParseStatus;
    }

    if (upconvertToSnapshot) {
        auto upconvertToSnapshotStatus = readConcernArgs.upconvertReadConcernLevelToSnapshot();
        if (!upconvertToSnapshotStatus.isOK()) {
            return upconvertToSnapshotStatus;
        }
    }

    if (!invocation->supportsReadConcern(readConcernArgs.getLevel())) {
        return {ErrorCodes::InvalidOptions,
                str::stream() << "Command does not support read concern "
                              << readConcernArgs.toString()};
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
        repl::ReplClientInfo::forClient(opCtx->getClient()).getLastOp().getTimestamp());
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
    if (startOperationTime == LogicalTime::kUninitialized) {
        return LogicalClock::get(opCtx)->getClusterTime();
    }

    auto operationTime = getClientOperationTime(opCtx);
    invariant(operationTime >= startOperationTime);

    // If the last operationTime has not changed, consider this command a read, and, for replica set
    // members, construct the operationTime with the proper optime for its read concern level.
    if (operationTime == startOperationTime) {
        auto const replCoord = repl::ReplicationCoordinator::get(opCtx);
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
        !LogicalClock::get(opCtx)->isEnabled()) {
        return;
    }

    // Authorized clients always receive operationTime and dummy signed $clusterTime.
    if (LogicalTimeValidator::isAuthorizedToAdvanceClock(opCtx)) {
        auto operationTime = computeOperationTime(opCtx, startTime);
        auto signedTime = SignedLogicalTime(
            LogicalClock::get(opCtx)->getClusterTime(), TimeProofService::TimeProof(), 0);

        invariant(signedTime.getTime() >= operationTime);
        rpc::LogicalTimeMetadata(signedTime).writeToMetadata(metadataBob);
        operationTime.appendAsOperationTime(commandBodyFieldsBob);

        return;
    }

    // Servers without validators (e.g. a shard server not yet added to a cluster) do not return
    // logical times to unauthorized clients.
    auto validator = LogicalTimeValidator::get(opCtx);
    if (!validator) {
        return;
    }

    auto operationTime = computeOperationTime(opCtx, startTime);
    auto signedTime = validator->trySignLogicalTime(LogicalClock::get(opCtx)->getClusterTime());

    // If there were no keys, do not return $clusterTime or operationTime to unauthorized clients.
    if (signedTime.getKeyId() == 0) {
        return;
    }

    invariant(signedTime.getTime() >= operationTime);
    rpc::LogicalTimeMetadata(signedTime).writeToMetadata(metadataBob);
    operationTime.appendAsOperationTime(commandBodyFieldsBob);
}

void invokeInTransaction(OperationContext* opCtx,
                         CommandInvocation* invocation,
                         CommandReplyBuilder* replyBuilder) {
    auto session = OperationContextSession::get(opCtx);
    if (!session) {
        // Run the command directly if we're not in a transaction.
        invocation->run(opCtx, replyBuilder);
        return;
    }

    session->unstashTransactionResources(opCtx, invocation->definition()->getName());
    ScopeGuard guard = MakeGuard([session, opCtx]() { session->abortActiveTransaction(opCtx); });

    invocation->run(opCtx, replyBuilder);

    if (auto okField = replyBuilder->getBodyBuilder().asTempObj()["ok"]) {
        // If ok is present, use its truthiness.
        if (!okField.trueValue()) {
            return;
        }
    }

    // Stash or commit the transaction when the command succeeds.
    session->stashTransactionResources(opCtx);
    guard.Dismiss();
}

bool runCommandImpl(OperationContext* opCtx,
                    CommandInvocation* invocation,
                    const OpMsgRequest& request,
                    rpc::ReplyBuilderInterface* replyBuilder,
                    LogicalTime startOperationTime,
                    const ServiceEntryPointCommon::Hooks& behaviors,
                    BSONObjBuilder* extraFieldsBuilder,
                    const boost::optional<OperationSessionInfoFromClient>& sessionOptions) {
    const Command* command = invocation->definition();
    auto bytesToReserve = command->reserveBytesForReply();

// SERVER-22100: In Windows DEBUG builds, the CRT heap debugging overhead, in conjunction with the
// additional memory pressure introduced by reply buffer pre-allocation, causes the concurrency
// suite to run extremely slowly. As a workaround we do not pre-allocate in Windows DEBUG builds.
#ifdef _WIN32
    if (kDebugBuild)
        bytesToReserve = 0;
#endif

    CommandReplyBuilder crb(replyBuilder->getInPlaceReplyBuilder(bytesToReserve));

    if (!invocation->supportsWriteConcern()) {
        behaviors.uassertCommandDoesNotSpecifyWriteConcern(request.body);
        invokeInTransaction(opCtx, invocation, &crb);
    } else {
        auto wcResult = uassertStatusOK(extractWriteConcern(opCtx, request.body));
        auto session = OperationContextSession::get(opCtx);
        uassert(ErrorCodes::InvalidOptions,
                "writeConcern is not allowed within a multi-statement transaction",
                wcResult.usedDefault || !session || !session->inMultiDocumentTransaction() ||
                    invocation->definition()->getName() == "commitTransaction" ||
                    invocation->definition()->getName() == "abortTransaction" ||
                    invocation->definition()->getName() == "doTxn");

        auto lastOpBeforeRun = repl::ReplClientInfo::forClient(opCtx->getClient()).getLastOp();

        // Change the write concern while running the command.
        const auto oldWC = opCtx->getWriteConcern();
        ON_BLOCK_EXIT([&] { opCtx->setWriteConcern(oldWC); });
        opCtx->setWriteConcern(wcResult);

        try {
            invokeInTransaction(opCtx, invocation, &crb);
        } catch (const DBException&) {
            behaviors.waitForWriteConcern(opCtx, invocation, lastOpBeforeRun, *extraFieldsBuilder);
            throw;
        }

        auto bb = crb.getBodyBuilder();
        behaviors.waitForWriteConcern(opCtx, invocation, lastOpBeforeRun, bb);

        // Nothing in run() should change the writeConcern.
        dassert(SimpleBSONObjComparator::kInstance.evaluate(opCtx->getWriteConcern().toBSON() ==
                                                            wcResult.toBSON()));
    }

    behaviors.waitForLinearizableReadConcern(opCtx);

    const bool ok = [&] {
        auto body = crb.getBodyBuilder();
        return CommandHelpers::extractOrAppendOk(body);
    }();
    behaviors.attachCurOpErrInfo(opCtx, crb.getBodyBuilder().asTempObj());

    if (!ok) {
        auto response = crb.getBodyBuilder().asTempObj();
        auto codeField = response["code"];

        if (codeField.isNumber()) {
            auto code = ErrorCodes::Error(codeField.numberInt());
            // Append the error labels for transient transaction errors.
            auto errorLabels = getErrorLabels(sessionOptions, command->getName(), code);
            crb.getBodyBuilder().appendElements(errorLabels);
        }
    }

    BSONObjBuilder metadataBob;
    appendReplyMetadata(opCtx, request, &metadataBob);

    {
        auto commandBodyBob = crb.getBodyBuilder();
        appendClusterAndOperationTime(opCtx, &commandBodyBob, &metadataBob, startOperationTime);
    }

    replyBuilder->setMetadata(metadataBob.obj());
    return ok;
}

/**
 * Maybe uassert according to the 'failCommand' fail point.
 */
void evaluateFailCommandFailPoint(OperationContext* opCtx, StringData commandName) {
    MONGO_FAIL_POINT_BLOCK_IF(failCommand, data, [&](const BSONObj& data) {
        BSONElement failCommandsList;
        auto status = bsonExtractTypedField(data, "failCommands", Array, &failCommandsList);
        if (!status.isOK()) {
            return false;
        }

        if (commandName == "configureFailPoint"_sd) {
            return false;
        }

        for (auto&& failCommand : failCommandsList.Obj()) {
            if (failCommand.type() == BSONType::String) {
                if (failCommand.valueStringData() == commandName) {
                    return true;
                }
            }
        }

        return false;
    }) {
        bool closeConnection;
        if (bsonExtractBooleanField(data.getData(), "closeConnection", &closeConnection).isOK() &&
            closeConnection) {
            opCtx->getClient()->session()->end();
            uasserted(50838, "Failing command due to 'failCommand' failpoint");
        }

        long long errorCode;
        if (bsonExtractIntegerField(data.getData(), "errorCode", &errorCode).isOK()) {
            uasserted(ErrorCodes::Error(errorCode),
                      "Failing command due to 'failCommand' failpoint");
        }
    }
}

/**
 * Executes a command after stripping metadata, performing authorization checks,
 * handling audit impersonation, and (potentially) setting maintenance mode. This method
 * also checks that the command is permissible to run on the node given its current
 * replication state. All the logic here is independent of any particular command; any
 * functionality relevant to a specific command should be confined to its run() method.
 */
void execCommandDatabase(OperationContext* opCtx,
                         Command* command,
                         const OpMsgRequest& request,
                         rpc::ReplyBuilderInterface* replyBuilder,
                         const ServiceEntryPointCommon::Hooks& behaviors) {
    BSONObjBuilder extraFieldsBuilder;
    auto startOperationTime = getClientOperationTime(opCtx);
    auto invocation = command->parse(opCtx, request);
    boost::optional<OperationSessionInfoFromClient> sessionOptions = boost::none;
    try {
        {
            stdx::lock_guard<Client> lk(*opCtx->getClient());
            CurOp::get(opCtx)->setCommand_inlock(command);
        }

        // TODO: move this back to runCommands when mongos supports OperationContext
        // see SERVER-18515 for details.
        rpc::readRequestMetadata(opCtx, request.body);
        rpc::TrackingMetadata::get(opCtx).initWithOperName(command->getName());

        auto const replCoord = repl::ReplicationCoordinator::get(opCtx);
        sessionOptions = initializeOperationSessionInfo(
            opCtx,
            request.body,
            command->requiresAuth(),
            replCoord->getReplicationMode() == repl::ReplicationCoordinator::modeReplSet,
            opCtx->getServiceContext()->getStorageEngine()->supportsDocLocking());

        evaluateFailCommandFailPoint(opCtx, command->getName());

        const auto dbname = request.getDatabase().toString();
        uassert(
            ErrorCodes::InvalidNamespace,
            str::stream() << "Invalid database name: '" << dbname << "'",
            NamespaceString::validDBName(dbname, NamespaceString::DollarInDbNameBehavior::Allow));

        // Session ids are forwarded in requests, so commands that require roundtrips between
        // servers may result in a deadlock when a server tries to check out a session it is already
        // using to service an earlier operation in the command's chain. To avoid this, only check
        // out sessions for commands that require them.
        const bool shouldCheckoutSession = static_cast<bool>(opCtx->getTxnNumber()) &&
            sessionCheckoutWhitelist.find(command->getName()) != sessionCheckoutWhitelist.cend();

        // Parse the arguments specific to multi-statement transactions.
        boost::optional<bool> startMultiDocTxn = boost::none;
        boost::optional<bool> autocommitVal = boost::none;
        if (sessionOptions) {
            startMultiDocTxn = sessionOptions->getStartTransaction();
            autocommitVal = sessionOptions->getAutocommit();
            if (command->getName() == "doTxn") {
                // Autocommit and 'startMultiDocTxn' are overridden for 'doTxn' to get the oplog
                // entry generation behavior used for multi-document transactions. The 'doTxn'
                // command still logically behaves as a commit.
                autocommitVal = false;
                startMultiDocTxn = true;
            }
        }

        // Reject commands with 'txnNumber' that do not check out the Session, since no retryable
        // writes or transaction machinery will be used to execute commands that do not check out
        // the Session. Do not check this if we are in DBDirectClient because the outer command is
        // responsible for checking out the Session.
        if (!opCtx->getClient()->isInDirectClient()) {
            uassert(50768,
                    str::stream() << "It is illegal to provide a txnNumber for command "
                                  << command->getName(),
                    shouldCheckoutSession || !opCtx->getTxnNumber());
        }

        // This constructor will check out the session and start a transaction, if necessary. It
        // handles the appropriate state management for both multi-statement transactions and
        // retryable writes.
        OperationContextSession sessionTxnState(opCtx,
                                                shouldCheckoutSession,
                                                autocommitVal,
                                                startMultiDocTxn,
                                                dbname,
                                                command->getName());

        std::unique_ptr<MaintenanceModeSetter> mmSetter;

        BSONElement cmdOptionMaxTimeMSField;
        BSONElement allowImplicitCollectionCreationField;
        BSONElement helpField;

        StringMap<int> topLevelFields;
        for (auto&& element : request.body) {
            StringData fieldName = element.fieldNameStringData();
            if (fieldName == QueryRequest::cmdOptionMaxTimeMS) {
                cmdOptionMaxTimeMSField = element;
            } else if (fieldName == "allowImplicitCollectionCreation") {
                allowImplicitCollectionCreationField = element;
            } else if (fieldName == CommandHelpers::kHelpFieldName) {
                helpField = element;
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
            // We disable last-error for help requests due to SERVER-11492, because config servers
            // use help requests to determine which commands are database writes, and so must be
            // forwarded to all config servers.
            LastError::get(opCtx->getClient()).disable();
            Command::generateHelpResponse(opCtx, replyBuilder, *command);
            return;
        }

        ImpersonationSessionGuard guard(opCtx);
        invocation->checkAuthorization(opCtx, request);

        const bool iAmPrimary = replCoord->canAcceptWritesForDatabase_UNSAFE(opCtx, dbname);

        if (!opCtx->getClient()->isInDirectClient() &&
            !MONGO_FAIL_POINT(skipCheckingForNotMasterInCommandDispatch)) {
            auto allowed = command->secondaryAllowed(opCtx->getServiceContext());
            bool alwaysAllowed = allowed == Command::AllowedOnSecondary::kAlways;
            bool couldHaveOptedIn = allowed == Command::AllowedOnSecondary::kOptIn;
            bool optedIn =
                couldHaveOptedIn && ReadPreferenceSetting::get(opCtx).canRunOnSecondary();
            bool canRunHere = commandCanRunHere(opCtx, dbname, command);
            if (!canRunHere && couldHaveOptedIn) {
                uasserted(ErrorCodes::NotMasterNoSlaveOk, "not master and slaveOk=false");
            }

            if (MONGO_FAIL_POINT(respondWithNotPrimaryInCommandDispatch)) {
                uassert(ErrorCodes::NotMaster, "not primary", canRunHere);
            } else {
                uassert(ErrorCodes::NotMaster, "not master", canRunHere);
            }

            if (!command->maintenanceOk() &&
                replCoord->getReplicationMode() == repl::ReplicationCoordinator::modeReplSet &&
                !replCoord->canAcceptWritesForDatabase_UNSAFE(opCtx, dbname) &&
                !replCoord->getMemberState().secondary()) {

                uassert(ErrorCodes::NotMasterOrSecondary,
                        "node is recovering",
                        !replCoord->getMemberState().recovering());
                uassert(ErrorCodes::NotMasterOrSecondary,
                        "node is not in primary or recovering state",
                        replCoord->getMemberState().primary());
                // Check ticket SERVER-21432, slaveOk commands are allowed in drain mode
                uassert(ErrorCodes::NotMasterOrSecondary,
                        "node is in drain mode",
                        optedIn || alwaysAllowed);
            }
        }

        if (command->adminOnly()) {
            LOG(2) << "command: " << request.getCommandName();
        }

        if (command->maintenanceMode()) {
            mmSetter.reset(new MaintenanceModeSetter(opCtx));
        }

        if (command->shouldAffectCommandCounter()) {
            OpCounters* opCounters = &globalOpCounters;
            opCounters->gotCommand();
        }

        // Parse the 'maxTimeMS' command option, and use it to set a deadline for the operation on
        // the OperationContext. The 'maxTimeMS' option unfortunately has a different meaning for a
        // getMore command, where it is used to communicate the maximum time to wait for new inserts
        // on tailable cursors, not as a deadline for the operation.
        // TODO SERVER-34277 Remove the special handling for maxTimeMS for getMores. This will
        // require introducing a new 'max await time' parameter for getMore, and eventually banning
        // maxTimeMS altogether on a getMore command.
        const int maxTimeMS =
            uassertStatusOK(QueryRequest::parseMaxTimeMS(cmdOptionMaxTimeMSField));
        if (maxTimeMS > 0 && command->getLogicalOp() != LogicalOp::opGetMore) {
            uassert(40119,
                    "Illegal attempt to set operation deadline within DBDirectClient",
                    !opCtx->getClient()->isInDirectClient());
            opCtx->setDeadlineAfterNowBy(Milliseconds{maxTimeMS});
        }

        auto& readConcernArgs = repl::ReadConcernArgs::get(opCtx);
        // TODO(SERVER-34113) replace below txnNumber/logicalSessionId checks with
        // Session::inMultiDocumentTransaction().
        if (!opCtx->getClient()->isInDirectClient() || !opCtx->getTxnNumber() ||
            !opCtx->getLogicalSessionId()) {
            auto session = OperationContextSession::get(opCtx);
            const bool upconvertToSnapshot = session && session->inMultiDocumentTransaction() &&
                sessionOptions &&
                (sessionOptions->getStartTransaction() == boost::optional<bool>(true));
            readConcernArgs = uassertStatusOK(
                _extractReadConcern(invocation.get(), request.body, upconvertToSnapshot));
        }

        if (readConcernArgs.getArgsAtClusterTime()) {
            uassert(ErrorCodes::InvalidOptions,
                    "atClusterTime is only used for testing",
                    getTestCommandsEnabled());
        }

        if (readConcernArgs.getLevel() == repl::ReadConcernLevel::kSnapshotReadConcern) {
            uassert(ErrorCodes::InvalidOptions,
                    "readConcern level snapshot is only valid in multi-statement transactions",
                    // With test commands enabled, a read command with readConcern snapshot is
                    // a valid snapshot read.
                    (getTestCommandsEnabled() &&
                     invocation->definition()->getReadWriteType() ==
                         BasicCommand::ReadWriteType::kRead) ||
                        (autocommitVal != boost::none && *autocommitVal == false));
            uassert(ErrorCodes::InvalidOptions,
                    "readConcern level snapshot requires a session ID",
                    opCtx->getLogicalSessionId());
            uassert(ErrorCodes::InvalidOptions,
                    "readConcern level snapshot requires a txnNumber",
                    opCtx->getTxnNumber());

            opCtx->lockState()->setSharedLocksShouldTwoPhaseLock(true);
        }

        auto& oss = OperationShardingState::get(opCtx);

        if (!opCtx->getClient()->isInDirectClient() &&
            readConcernArgs.getLevel() != repl::ReadConcernLevel::kAvailableReadConcern &&
            (iAmPrimary ||
             (readConcernArgs.hasLevel() || readConcernArgs.getArgsAfterClusterTime()))) {
            oss.initializeClientRoutingVersions(invocation->ns(), request.body);

            auto const shardingState = ShardingState::get(opCtx);
            if (oss.hasShardVersion() || oss.hasDbVersion()) {
                uassertStatusOK(shardingState->canAcceptShardedCommands());
            }

            // Handle config optime information that may have been sent along with the command.
            uassertStatusOK(shardingState->updateConfigServerOpTimeFromMetadata(opCtx));
        }

        oss.setAllowImplicitCollectionCreation(allowImplicitCollectionCreationField);

        // Can throw
        opCtx->checkForInterrupt();  // May trigger maxTimeAlwaysTimeOut fail point.

        CurOp::get(opCtx)->ensureStarted();

        command->incrementCommandsExecuted();

        if (logger::globalLogDomain()->shouldLog(logger::LogComponent::kTracking,
                                                 logger::LogSeverity::Debug(1)) &&
            rpc::TrackingMetadata::get(opCtx).getParentOperId()) {
            MONGO_LOG_COMPONENT(1, logger::LogComponent::kTracking)
                << rpc::TrackingMetadata::get(opCtx).toString();
            rpc::TrackingMetadata::get(opCtx).setIsLogged(true);
        }

        behaviors.waitForReadConcern(opCtx, invocation.get(), request);

        try {
            if (!runCommandImpl(opCtx,
                                invocation.get(),
                                request,
                                replyBuilder,
                                startOperationTime,
                                behaviors,
                                &extraFieldsBuilder,
                                sessionOptions)) {
                command->incrementCommandsFailed();
            }
        } catch (DBException&) {
            command->incrementCommandsFailed();
            throw;
        }
    } catch (const DBException& e) {
        // If we got a stale config, wait in case the operation is stuck in a critical section
        if (auto sce = e.extraInfo<StaleConfigInfo>()) {
            if (!opCtx->getClient()->isInDirectClient()) {
                // We already have the StaleConfig exception, so just swallow any errors due to
                // refresh
                onShardVersionMismatch(
                    opCtx, NamespaceString(sce->getns()), sce->getVersionReceived())
                    .ignore();
            }
        } else if (auto sce = e.extraInfo<StaleDbRoutingVersion>()) {
            if (!opCtx->getClient()->isInDirectClient()) {
                onDbVersionMismatch(
                    opCtx, sce->getDb(), sce->getVersionReceived(), sce->getVersionWanted());
            }
        } else if (auto cannotImplicitCreateCollInfo =
                       e.extraInfo<CannotImplicitlyCreateCollectionInfo>()) {
            if (ShardingState::get(opCtx)->enabled()) {
                onCannotImplicitlyCreateCollection(opCtx, cannotImplicitCreateCollInfo->getNss())
                    .ignore();
            }
        } else if (e.code() == ErrorCodes::SnapshotTooOld) {
            // SnapshotTooOld errors indicate that PIT ops are failing to find an available snapshot
            // at their specified atClusterTime. Therefore, we'll try to increase the snapshot
            // history window that the storage engine maintains in order to increase the likelihood
            // of successful future PIT atClusterTime requests.
            auto engine = opCtx->getServiceContext()->getStorageEngine();
            if (engine && engine->supportsReadConcernSnapshot()) {
                SnapshotWindowUtil::increaseTargetSnapshotWindowSize(opCtx);
            }
        }

        // Append the error labels for transient transaction errors.
        auto errorLabels = getErrorLabels(sessionOptions, command->getName(), e.code());
        extraFieldsBuilder.appendElements(errorLabels);

        BSONObjBuilder metadataBob;
        appendReplyMetadata(opCtx, request, &metadataBob);

        // The read concern may not have yet been placed on the operation context, so attempt to
        // parse it here, so if it is valid it can be used to compute the proper operationTime.
        auto& readConcernArgs = repl::ReadConcernArgs::get(opCtx);
        if (readConcernArgs.isEmpty()) {
            auto readConcernArgsStatus = _extractReadConcern(invocation.get(), request.body, false);
            if (readConcernArgsStatus.isOK()) {
                readConcernArgs = readConcernArgsStatus.getValue();
            }
        }
        appendClusterAndOperationTime(opCtx, &extraFieldsBuilder, &metadataBob, startOperationTime);

        LOG(1) << "assertion while executing command '" << request.getCommandName() << "' "
               << "on database '" << request.getDatabase() << "' "
               << "with arguments '"
               << redact(ServiceEntryPointCommon::getRedactedCopyForLogging(command, request.body))
               << "': " << redact(e.toString());

        generateErrorResponse(opCtx, replyBuilder, e, metadataBob.obj(), extraFieldsBuilder.obj());
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

DbResponse runCommands(OperationContext* opCtx,
                       const Message& message,
                       const ServiceEntryPointCommon::Hooks& behaviors) {
    auto replyBuilder = rpc::makeReplyBuilder(rpc::protocolForMessage(message));
    [&] {
        OpMsgRequest request;
        try {  // Parse.
            request = rpc::opMsgRequestFromAnyProtocol(message);
        } catch (const DBException& ex) {
            // If this error needs to fail the connection, propagate it out.
            if (ErrorCodes::isConnectionFatalMessageParseError(ex.code()))
                throw;

            BSONObjBuilder metadataBob;
            appendReplyMetadataOnError(opCtx, &metadataBob);

            BSONObjBuilder extraFieldsBuilder;
            appendClusterAndOperationTime(
                opCtx, &extraFieldsBuilder, &metadataBob, LogicalTime::kUninitialized);

            // Otherwise, reply with the parse error. This is useful for cases where parsing fails
            // due to user-supplied input, such as the document too deep error. Since we failed
            // during parsing, we can't log anything about the command.
            LOG(1) << "assertion while parsing command: " << ex.toString();

            generateErrorResponse(
                opCtx, replyBuilder.get(), ex, metadataBob.obj(), extraFieldsBuilder.obj());

            return;  // From lambda. Don't try executing if parsing failed.
        }

        try {  // Execute.
            curOpCommandSetup(opCtx, request);

            Command* c = nullptr;
            // In the absence of a Command object, no redaction is possible. Therefore
            // to avoid displaying potentially sensitive information in the logs,
            // we restrict the log message to the name of the unrecognized command.
            // However, the complete command object will still be echoed to the client.
            if (!(c = CommandHelpers::findCommand(request.getCommandName()))) {
                globalCommandRegistry()->incrementUnknownCommands();
                std::string msg = str::stream() << "no such command: '" << request.getCommandName()
                                                << "'";
                LOG(2) << msg;
                uasserted(ErrorCodes::CommandNotFound, str::stream() << msg);
            }

            LOG(2) << "run command " << request.getDatabase() << ".$cmd" << ' '
                   << redact(ServiceEntryPointCommon::getRedactedCopyForLogging(c, request.body));

            {
                // Try to set this as early as possible, as soon as we have figured out the command.
                stdx::lock_guard<Client> lk(*opCtx->getClient());
                CurOp::get(opCtx)->setLogicalOp_inlock(c->getLogicalOp());
            }

            execCommandDatabase(opCtx, c, request, replyBuilder.get(), behaviors);
        } catch (const DBException& ex) {
            BSONObjBuilder metadataBob;
            appendReplyMetadataOnError(opCtx, &metadataBob);

            BSONObjBuilder extraFieldsBuilder;
            appendClusterAndOperationTime(
                opCtx, &extraFieldsBuilder, &metadataBob, LogicalTime::kUninitialized);

            LOG(1) << "assertion while executing command '" << request.getCommandName() << "' "
                   << "on database '" << request.getDatabase() << "': " << ex.toString();

            generateErrorResponse(
                opCtx, replyBuilder.get(), ex, metadataBob.obj(), extraFieldsBuilder.obj());
        }
    }();

    if (OpMsg::isFlagSet(message, OpMsg::kMoreToCome)) {
        // Close the connection to get client to go through server selection again.
        uassert(ErrorCodes::NotMaster,
                "Not-master error during fire-and-forget command processing",
                !LastError::get(opCtx->getClient()).hadNotMasterError());

        return {};  // Don't reply.
    }

    auto response = replyBuilder->done();
    CurOp::get(opCtx)->debug().responseLength = response.header().dataLen();

    // TODO exhaust
    return DbResponse{std::move(response)};
}

DbResponse receivedQuery(OperationContext* opCtx,
                         const NamespaceString& nss,
                         Client& c,
                         const Message& m) {
    invariant(!nss.isCommand());
    globalOpCounters.gotQuery();

    DbMessage d(m);
    QueryMessage q(d);

    CurOp& op = *CurOp::get(opCtx);
    DbResponse dbResponse;

    try {
        Client* client = opCtx->getClient();
        Status status = AuthorizationSession::get(client)->checkAuthForFind(nss, false);
        audit::logQueryAuthzCheck(client, nss, q.query, status.code());
        uassertStatusOK(status);

        dbResponse.exhaustNS = runQuery(opCtx, q, nss, dbResponse.response);
    } catch (const AssertionException& e) {
        // If we got a stale config, wait in case the operation is stuck in a critical section
        if (auto sce = e.extraInfo<StaleConfigInfo>()) {
            if (!opCtx->getClient()->isInDirectClient()) {
                // We already have the StaleConfig exception, so just swallow any errors due to
                // refresh
                onShardVersionMismatch(
                    opCtx, NamespaceString(sce->getns()), sce->getVersionReceived())
                    .ignore();
            }
        }

        dbResponse.response.reset();
        generateLegacyQueryErrorResponse(&e, q, &op, &dbResponse.response);
    }

    op.debug().responseLength = dbResponse.response.header().dataLen();
    return dbResponse;
}

void receivedKillCursors(OperationContext* opCtx, const Message& m) {
    LastError::get(opCtx->getClient()).disable();
    DbMessage dbmessage(m);
    int n = dbmessage.pullInt();

    uassert(13659, "sent 0 cursors to kill", n != 0);
    massert(13658,
            str::stream() << "bad kill cursors size: " << m.dataSize(),
            m.dataSize() == 8 + (8 * n));
    uassert(13004, str::stream() << "sent negative cursors to kill: " << n, n >= 1);

    if (n > 2000) {
        (n < 30000 ? warning() : error()) << "receivedKillCursors, n=" << n;
        verify(n < 30000);
    }

    const char* cursorArray = dbmessage.getArray(n);

    int found = CursorManager::killCursorGlobalIfAuthorized(opCtx, n, cursorArray);

    if (shouldLog(logger::LogSeverity::Debug(1)) || found != n) {
        LOG(found == n ? 1 : 0) << "killcursors: found " << found << " of " << n;
    }
}

void receivedInsert(OperationContext* opCtx, const NamespaceString& nsString, const Message& m) {
    auto insertOp = InsertOp::parseLegacy(m);
    invariant(insertOp.getNamespace() == nsString);

    for (const auto& obj : insertOp.getDocuments()) {
        Status status =
            AuthorizationSession::get(opCtx->getClient())->checkAuthForInsert(opCtx, nsString, obj);
        audit::logInsertAuthzCheck(opCtx->getClient(), nsString, obj, status.code());
        uassertStatusOK(status);
    }
    performInserts(opCtx, insertOp);
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

    performUpdates(opCtx, updateOp);
}

void receivedDelete(OperationContext* opCtx, const NamespaceString& nsString, const Message& m) {
    auto deleteOp = DeleteOp::parseLegacy(m);
    auto& singleDelete = deleteOp.getDeletes()[0];
    invariant(deleteOp.getNamespace() == nsString);

    Status status = AuthorizationSession::get(opCtx->getClient())
                        ->checkAuthForDelete(opCtx, nsString, singleDelete.getQ());
    audit::logDeleteAuthzCheck(opCtx->getClient(), nsString, singleDelete.getQ(), status.code());
    uassertStatusOK(status);

    performDeletes(opCtx, deleteOp);
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

        while (MONGO_FAIL_POINT(rsStopGetMore)) {
            sleepmillis(0);
        }

        dbresponse.response =
            getMore(opCtx, ns, ntoreturn, cursorid, &exhaust, &isCursorAuthorized);
    } catch (AssertionException& e) {
        if (isCursorAuthorized) {
            // Make sure that killCursorGlobal does not throw an exception if it is interrupted.
            UninterruptibleLockGuard noInterrupt(opCtx->lockState());

            // If a cursor with id 'cursorid' was authorized, it may have been advanced
            // before an exception terminated processGetMore.  Erase the ClientCursor
            // because it may now be out of sync with the client's iteration state.
            // SERVER-7952
            // TODO Temporary code, see SERVER-4563 for a cleanup overview.
            CursorManager::killCursorGlobal(opCtx, cursorid);
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
        dbresponse.exhaustNS = ns;
    }

    return dbresponse;
}

}  // namespace

BSONObj ServiceEntryPointCommon::getRedactedCopyForLogging(const Command* command,
                                                           const BSONObj& cmdObj) {
    mutablebson::Document cmdToLog(cmdObj, mutablebson::Document::kInPlaceDisabled);
    command->redactForLogging(&cmdToLog);
    BSONObjBuilder bob;
    cmdToLog.writeTo(&bob);
    return bob.obj();
}

DbResponse ServiceEntryPointCommon::handleRequest(OperationContext* opCtx,
                                                  const Message& m,
                                                  const Hooks& behaviors) {
    // before we lock...
    NetworkOp op = m.operation();
    bool isCommand = false;

    DbMessage dbmsg(m);

    Client& c = *opCtx->getClient();

    if (c.isInDirectClient()) {
        if (!opCtx->getLogicalSessionId() || !opCtx->getTxnNumber() ||
            repl::ReadConcernArgs::get(opCtx).getLevel() !=
                repl::ReadConcernLevel::kSnapshotReadConcern) {
            invariant(!opCtx->lockState()->inAWriteUnitOfWork());
        }
    } else {
        LastError::get(c).startRequest();
        AuthorizationSession::get(c)->startRequest(opCtx);

        // We should not be holding any locks at this point
        invariant(!opCtx->lockState()->isLocked());
    }

    const char* ns = dbmsg.messageShouldHaveNs() ? dbmsg.getns() : NULL;
    const NamespaceString nsString = ns ? NamespaceString(ns) : NamespaceString();

    if (op == dbQuery) {
        if (nsString.isCommand()) {
            isCommand = true;
        }
    } else if (op == dbCommand || op == dbMsg) {
        isCommand = true;
    }

    CurOp& currentOp = *CurOp::get(opCtx);
    {
        stdx::lock_guard<Client> lk(*opCtx->getClient());
        // Commands handling code will reset this if the operation is a command
        // which is logically a basic CRUD operation like query, insert, etc.
        currentOp.setNetworkOp_inlock(op);
        currentOp.setLogicalOp_inlock(networkOpToLogicalOp(op));
    }

    OpDebug& debug = currentOp.debug();

    boost::optional<long long> slowMsOverride;
    bool forceLog = false;

    DbResponse dbresponse;
    if (op == dbMsg || op == dbCommand || (op == dbQuery && isCommand)) {
        dbresponse = runCommands(opCtx, m, behaviors);
    } else if (op == dbQuery) {
        invariant(!isCommand);
        dbresponse = receivedQuery(opCtx, nsString, c, m);
    } else if (op == dbGetMore) {
        dbresponse = receivedGetMore(opCtx, m, currentOp, &forceLog);
    } else {
        // The remaining operations do not return any response. They are fire-and-forget.
        try {
            if (op == dbKillCursors) {
                currentOp.ensureStarted();
                slowMsOverride = 10;
                receivedKillCursors(opCtx, m);
            } else if (op != dbInsert && op != dbUpdate && op != dbDelete) {
                log() << "    operation isn't supported: " << static_cast<int>(op);
                currentOp.done();
                forceLog = true;
            } else {
                if (!opCtx->getClient()->isInDirectClient()) {
                    uassert(18663,
                            str::stream() << "legacy writeOps not longer supported for "
                                          << "versioned connections, ns: "
                                          << nsString.ns()
                                          << ", op: "
                                          << networkOpToString(op),
                            !ShardedConnectionInfo::get(&c, false));
                }

                if (!nsString.isValid()) {
                    uassert(16257, str::stream() << "Invalid ns [" << ns << "]", false);
                } else if (op == dbInsert) {
                    receivedInsert(opCtx, nsString, m);
                } else if (op == dbUpdate) {
                    receivedUpdate(opCtx, nsString, m);
                } else if (op == dbDelete) {
                    receivedDelete(opCtx, nsString, m);
                } else {
                    MONGO_UNREACHABLE;
                }
            }
        } catch (const AssertionException& ue) {
            LastError::get(c).setLastError(ue.code(), ue.reason());
            LOG(3) << " Caught Assertion in " << networkOpToString(op) << ", continuing "
                   << redact(ue);
            debug.errInfo = ue.toStatus();
        }
    }

    // Mark the op as complete, and log it if appropriate. Returns a boolean indicating whether
    // this op should be sampled for profiling.
    const bool shouldSample = currentOp.completeAndLogOperation(
        opCtx, MONGO_LOG_DEFAULT_COMPONENT, dbresponse.response.size(), slowMsOverride, forceLog);

    Top::get(opCtx->getServiceContext())
        .incrementGlobalLatencyStats(
            opCtx,
            durationCount<Microseconds>(currentOp.elapsedTimeExcludingPauses()),
            currentOp.getReadWriteType());

    if (currentOp.shouldDBProfile(shouldSample)) {
        // Performance profiling is on
        if (opCtx->lockState()->isReadLocked()) {
            LOG(1) << "note: not profiling because recursive read lock";
        } else if (c.isInDirectClient()) {
            LOG(1) << "note: not profiling because we are in DBDirectClient";
        } else if (behaviors.lockedForWriting()) {
            // TODO SERVER-26825: Fix race condition where fsyncLock is acquired post
            // lockedForWriting() call but prior to profile collection lock acquisition.
            LOG(1) << "note: not profiling because doing fsync+lock";
        } else if (storageGlobalParams.readOnly) {
            LOG(1) << "note: not profiling because server is read-only";
        } else {
            invariant(!opCtx->lockState()->inAWriteUnitOfWork());
            profile(opCtx, op);
        }
    }

    recordCurOpMetrics(opCtx);
    return dbresponse;
}

ServiceEntryPointCommon::Hooks::~Hooks() = default;

}  // namespace mongo
