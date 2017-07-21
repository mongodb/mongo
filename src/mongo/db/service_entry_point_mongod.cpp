/**
 *    Copyright (C) 2017 MongoDB Inc.
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

#include "mongo/db/service_entry_point_mongod.h"

#include "mongo/db/audit.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/impersonation_session.h"
#include "mongo/db/client.h"
#include "mongo/db/commands.h"
#include "mongo/db/commands/fsync.h"
#include "mongo/db/concurrency/global_lock_acquisition_tracker.h"
#include "mongo/db/curop.h"
#include "mongo/db/curop_metrics.h"
#include "mongo/db/cursor_manager.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/diag_log.h"
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
#include "mongo/db/repl/repl_client_info.h"
#include "mongo/db/repl/replication_coordinator_global.h"
#include "mongo/db/s/operation_sharding_state.h"
#include "mongo/db/s/sharded_connection_info.h"
#include "mongo/db/s/sharding_state.h"
#include "mongo/db/server_options.h"
#include "mongo/db/session_catalog.h"
#include "mongo/db/stats/counters.h"
#include "mongo/db/stats/top.h"
#include "mongo/rpc/factory.h"
#include "mongo/rpc/metadata.h"
#include "mongo/rpc/metadata/config_server_metadata.h"
#include "mongo/rpc/metadata/logical_time_metadata.h"
#include "mongo/rpc/metadata/oplog_query_metadata.h"
#include "mongo/rpc/metadata/repl_set_metadata.h"
#include "mongo/rpc/metadata/sharding_metadata.h"
#include "mongo/rpc/metadata/tracking_metadata.h"
#include "mongo/rpc/reply_builder_interface.h"
#include "mongo/s/grid.h"
#include "mongo/s/stale_exception.h"
#include "mongo/util/fail_point_service.h"
#include "mongo/util/log.h"
#include "mongo/util/net/message.h"
#include "mongo/util/net/op_msg.h"
#include "mongo/util/scopeguard.h"

namespace mongo {

MONGO_FP_DECLARE(rsStopGetMore);

namespace {
using logger::LogComponent;

inline void opread(const Message& m) {
    if (_diaglog.getLevel() & 2) {
        _diaglog.readop(m.singleData().view2ptr(), m.header().getLen());
    }
}

inline void opwrite(const Message& m) {
    if (_diaglog.getLevel() & 1) {
        _diaglog.writeop(m.singleData().view2ptr(), m.header().getLen());
    }
}

void generateLegacyQueryErrorResponse(const AssertionException* exception,
                                      const QueryMessage& queryMessage,
                                      CurOp* curop,
                                      Message* response) {
    curop->debug().exceptionInfo = exception->getInfo();

    log(LogComponent::kQuery) << "assertion " << exception->toString() << " ns:" << queryMessage.ns
                              << " query:" << (queryMessage.query.valid(BSONVersion::kLatest)
                                                   ? queryMessage.query.toString()
                                                   : "query object is corrupt");
    if (queryMessage.ntoskip || queryMessage.ntoreturn) {
        log(LogComponent::kQuery) << " ntoskip:" << queryMessage.ntoskip
                                  << " ntoreturn:" << queryMessage.ntoreturn;
    }

    const SendStaleConfigException* scex = (exception->getCode() == ErrorCodes::SendStaleConfig)
        ? static_cast<const SendStaleConfigException*>(exception)
        : NULL;

    BSONObjBuilder err;
    exception->getInfo().append(err);
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
    LastError::get(opCtx->getClient()).setLastError(exception.getCode(), exception.getInfo().msg);
    CurOp::get(opCtx)->debug().exceptionInfo = exception.getInfo();
}

void _generateErrorResponse(OperationContext* opCtx,
                            rpc::ReplyBuilderInterface* replyBuilder,
                            const DBException& exception,
                            const BSONObj& replyMetadata) {
    registerError(opCtx, exception);

    // We could have thrown an exception after setting fields in the builder,
    // so we need to reset it to a clean state just to be sure.
    replyBuilder->reset();

    // We need to include some extra information for SendStaleConfig.
    if (exception.getCode() == ErrorCodes::SendStaleConfig) {
        const SendStaleConfigException& scex =
            static_cast<const SendStaleConfigException&>(exception);
        replyBuilder->setCommandReply(scex.toStatus(),
                                      BSON("ns" << scex.getns() << "vReceived"
                                                << BSONArray(scex.getVersionReceived().toBSON())
                                                << "vWanted"
                                                << BSONArray(scex.getVersionWanted().toBSON())));
    } else {
        replyBuilder->setCommandReply(exception.toStatus());
    }

    replyBuilder->setMetadata(replyMetadata);
}

void _generateErrorResponse(OperationContext* opCtx,
                            rpc::ReplyBuilderInterface* replyBuilder,
                            const DBException& exception,
                            const BSONObj& replyMetadata,
                            LogicalTime operationTime) {
    registerError(opCtx, exception);

    // We could have thrown an exception after setting fields in the builder,
    // so we need to reset it to a clean state just to be sure.
    replyBuilder->reset();

    // We need to include some extra information for SendStaleConfig.
    if (exception.getCode() == ErrorCodes::SendStaleConfig) {
        const SendStaleConfigException& scex =
            static_cast<const SendStaleConfigException&>(exception);
        replyBuilder->setCommandReply(scex.toStatus(),
                                      BSON("ns" << scex.getns() << "vReceived"
                                                << BSONArray(scex.getVersionReceived().toBSON())
                                                << "vWanted"
                                                << BSONArray(scex.getVersionWanted().toBSON())
                                                << "operationTime"
                                                << operationTime.asTimestamp()));
    } else {
        replyBuilder->setCommandReply(exception.toStatus(),
                                      BSON("operationTime" << operationTime.asTimestamp()));
    }

    replyBuilder->setMetadata(replyMetadata);
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
public:
    MaintenanceModeSetter()
        : maintenanceModeSet(
              repl::getGlobalReplicationCoordinator()->setMaintenanceMode(true).isOK()) {}
    ~MaintenanceModeSetter() {
        if (maintenanceModeSet)
            repl::getGlobalReplicationCoordinator()
                ->setMaintenanceMode(false)
                .transitional_ignore();
    }

private:
    bool maintenanceModeSet;
};

void appendReplyMetadata(OperationContext* opCtx,
                         const OpMsgRequest& request,
                         BSONObjBuilder* metadataBob) {
    const bool isShardingAware = ShardingState::get(opCtx)->enabled();
    const bool isConfig = serverGlobalParams.clusterRole == ClusterRole::ConfigServer;
    repl::ReplicationCoordinator* replCoord = repl::getGlobalReplicationCoordinator();
    const bool isReplSet =
        replCoord->getReplicationMode() == repl::ReplicationCoordinator::modeReplSet;

    if (isReplSet) {
        // Attach our own last opTime.
        repl::OpTime lastOpTimeFromClient =
            repl::ReplClientInfo::forClient(opCtx->getClient()).getLastOp();
        replCoord->prepareReplMetadata(opCtx, request.body, lastOpTimeFromClient, metadataBob);
        // For commands from mongos, append some info to help getLastError(w) work.
        // TODO: refactor out of here as part of SERVER-18236
        if (isShardingAware || isConfig) {
            rpc::ShardingMetadata(lastOpTimeFromClient, replCoord->getElectionId())
                .writeToMetadata(metadataBob)
                .transitional_ignore();
            if (serverGlobalParams.featureCompatibility.version.load() ==
                ServerGlobalParams::FeatureCompatibility::Version::k36) {
                if (LogicalTimeValidator::isAuthorizedToAdvanceClock(opCtx)) {
                    // No need to sign cluster times for internal clients.
                    SignedLogicalTime currentTime(LogicalClock::get(opCtx)->getClusterTime(),
                                                  TimeProofService::TimeProof(),
                                                  0);
                    rpc::LogicalTimeMetadata logicalTimeMetadata(currentTime);
                    logicalTimeMetadata.writeToMetadata(metadataBob);
                } else if (auto validator = LogicalTimeValidator::get(opCtx)) {
                    auto currentTime =
                        validator->trySignLogicalTime(LogicalClock::get(opCtx)->getClusterTime());
                    rpc::LogicalTimeMetadata logicalTimeMetadata(currentTime);
                    logicalTimeMetadata.writeToMetadata(metadataBob);
                }
            }
        }
    }

    // If we're a shard other than the config shard, attach the last configOpTime we know about.
    if (isShardingAware && !isConfig) {
        auto opTime = grid.configOpTime();
        rpc::ConfigServerMetadata(opTime).writeToMetadata(metadataBob);
    }
}

/**
 * Given the specified command and whether it supports read concern, returns an effective read
 * concern which should be used.
 */
StatusWith<repl::ReadConcernArgs> _extractReadConcern(const BSONObj& cmdObj,
                                                      bool supportsNonLocalReadConcern) {
    repl::ReadConcernArgs readConcernArgs;

    auto readConcernParseStatus = readConcernArgs.initialize(cmdObj);
    if (!readConcernParseStatus.isOK()) {
        return readConcernParseStatus;
    }

    if (!supportsNonLocalReadConcern &&
        readConcernArgs.getLevel() != repl::ReadConcernLevel::kLocalReadConcern) {
        return {ErrorCodes::InvalidOptions,
                str::stream() << "Command does not support non local read concern"};
    }

    return readConcernArgs;
}

void _waitForWriteConcernAndAddToCommandResponse(OperationContext* opCtx,
                                                 const std::string& commandName,
                                                 const repl::OpTime& lastOpBeforeRun,
                                                 BSONObjBuilder* commandResponseBuilder) {
    auto lastOpAfterRun = repl::ReplClientInfo::forClient(opCtx->getClient()).getLastOp();

    // Ensures that if we tried to do a write, we wait for write concern, even if that write was
    // a noop.
    if ((lastOpAfterRun == lastOpBeforeRun) &&
        GlobalLockAcquisitionTracker::get(opCtx).getGlobalExclusiveLockTaken()) {
        repl::ReplClientInfo::forClient(opCtx->getClient()).setLastOpToSystemLastOpTime(opCtx);
        lastOpAfterRun = repl::ReplClientInfo::forClient(opCtx->getClient()).getLastOp();
    }

    WriteConcernResult res;
    auto waitForWCStatus =
        waitForWriteConcern(opCtx, lastOpAfterRun, opCtx->getWriteConcern(), &res);
    Command::appendCommandWCStatus(*commandResponseBuilder, waitForWCStatus, res);

    // SERVER-22421: This code is to ensure error response backwards compatibility with the
    // user management commands. This can be removed in 3.6.
    if (!waitForWCStatus.isOK() && Command::isUserManagementCommand(commandName)) {
        BSONObj temp = commandResponseBuilder->asTempObj().copy();
        commandResponseBuilder->resetToEmpty();
        Command::appendCommandStatus(*commandResponseBuilder, waitForWCStatus);
        commandResponseBuilder->appendElementsUnique(temp);
    }
}

/**
 * For replica set members it returns the last known op time from opCtx. Otherwise will return
 * uninitialized cluster time.
 */
LogicalTime getClientOperationTime(OperationContext* opCtx) {
    repl::ReplicationCoordinator* replCoord =
        repl::ReplicationCoordinator::get(opCtx->getClient()->getServiceContext());
    const bool isReplSet =
        replCoord->getReplicationMode() == repl::ReplicationCoordinator::modeReplSet;
    LogicalTime operationTime;
    if (isReplSet) {
        operationTime = LogicalTime(
            repl::ReplClientInfo::forClient(opCtx->getClient()).getLastOp().getTimestamp());
    }
    return operationTime;
}

/**
 * Returns the proper operationTime for a command. To construct the operationTime for replica set
 * members, it uses the last optime in the oplog for writes, last committed optime for majority
 * reads, and the last applied optime for every other read. An uninitialized cluster time is
 * returned for non replica set members.
 */
LogicalTime computeOperationTime(OperationContext* opCtx,
                                 LogicalTime startOperationTime,
                                 repl::ReadConcernLevel level) {
    repl::ReplicationCoordinator* replCoord =
        repl::ReplicationCoordinator::get(opCtx->getClient()->getServiceContext());
    const bool isReplSet =
        replCoord->getReplicationMode() == repl::ReplicationCoordinator::modeReplSet;

    if (!isReplSet) {
        return LogicalTime();
    }

    auto operationTime = getClientOperationTime(opCtx);
    invariant(operationTime >= startOperationTime);

    // If the last operationTime has not changed, consider this command a read, and, for replica set
    // members, construct the operationTime with the proper optime for its read concern level.
    if (operationTime == startOperationTime) {
        if (level == repl::ReadConcernLevel::kMajorityReadConcern) {
            operationTime = LogicalTime(replCoord->getLastCommittedOpTime().getTimestamp());
        } else {
            operationTime = LogicalTime(replCoord->getMyLastAppliedOpTime().getTimestamp());
        }
    }

    return operationTime;
}

bool runCommandImpl(OperationContext* opCtx,
                    Command* command,
                    const OpMsgRequest& request,
                    rpc::ReplyBuilderInterface* replyBuilder,
                    LogicalTime startOperationTime) {
    auto bytesToReserve = command->reserveBytesForReply();

// SERVER-22100: In Windows DEBUG builds, the CRT heap debugging overhead, in conjunction with the
// additional memory pressure introduced by reply buffer pre-allocation, causes the concurrency
// suite to run extremely slowly. As a workaround we do not pre-allocate in Windows DEBUG builds.
#ifdef _WIN32
    if (kDebugBuild)
        bytesToReserve = 0;
#endif

    // run expects non-const bsonobj
    BSONObj cmd = request.body;

    // run expects const db std::string (can't bind to temporary)
    const std::string db = request.getDatabase().toString();

    BSONObjBuilder inPlaceReplyBob = replyBuilder->getInPlaceReplyBuilder(bytesToReserve);
    auto readConcernArgsStatus = _extractReadConcern(cmd, command->supportsReadConcern(db, cmd));

    if (!readConcernArgsStatus.isOK()) {
        auto result =
            Command::appendCommandStatus(inPlaceReplyBob, readConcernArgsStatus.getStatus());
        inPlaceReplyBob.doneFast();
        replyBuilder->setMetadata(rpc::makeEmptyMetadata());
        return result;
    }

    Status rcStatus = waitForReadConcern(opCtx, readConcernArgsStatus.getValue());
    if (!rcStatus.isOK()) {
        if (rcStatus == ErrorCodes::ExceededTimeLimit) {
            const int debugLevel =
                serverGlobalParams.clusterRole == ClusterRole::ConfigServer ? 0 : 2;
            LOG(debugLevel) << "Command on database " << db
                            << " timed out waiting for read concern to be satisfied. Command: "
                            << redact(command->getRedactedCopyForLogging(request.body));
        }

        auto result = Command::appendCommandStatus(inPlaceReplyBob, rcStatus);
        inPlaceReplyBob.doneFast();
        replyBuilder->setMetadata(rpc::makeEmptyMetadata());
        return result;
    }

    bool result;
    if (!command->supportsWriteConcern(cmd)) {
        if (commandSpecifiesWriteConcern(cmd)) {
            auto result = Command::appendCommandStatus(
                inPlaceReplyBob,
                {ErrorCodes::InvalidOptions, "Command does not support writeConcern"});
            inPlaceReplyBob.doneFast();
            replyBuilder->setMetadata(rpc::makeEmptyMetadata());
            return result;
        }

        result = command->enhancedRun(opCtx, request, inPlaceReplyBob);
    } else {
        auto wcResult = extractWriteConcern(opCtx, cmd, db);
        if (!wcResult.isOK()) {
            auto result = Command::appendCommandStatus(inPlaceReplyBob, wcResult.getStatus());
            inPlaceReplyBob.doneFast();
            replyBuilder->setMetadata(rpc::makeEmptyMetadata());
            return result;
        }

        auto lastOpBeforeRun = repl::ReplClientInfo::forClient(opCtx->getClient()).getLastOp();

        // Change the write concern while running the command.
        const auto oldWC = opCtx->getWriteConcern();
        ON_BLOCK_EXIT([&] { opCtx->setWriteConcern(oldWC); });
        opCtx->setWriteConcern(wcResult.getValue());
        ON_BLOCK_EXIT([&] {
            _waitForWriteConcernAndAddToCommandResponse(
                opCtx, command->getName(), lastOpBeforeRun, &inPlaceReplyBob);
        });

        result = command->enhancedRun(opCtx, request, inPlaceReplyBob);

        // Nothing in run() should change the writeConcern.
        dassert(SimpleBSONObjComparator::kInstance.evaluate(opCtx->getWriteConcern().toBSON() ==
                                                            wcResult.getValue().toBSON()));
    }

    // When a linearizable read command is passed in, check to make sure we're reading
    // from the primary.
    if (command->supportsReadConcern(db, cmd) &&
        (readConcernArgsStatus.getValue().getLevel() ==
         repl::ReadConcernLevel::kLinearizableReadConcern) &&
        (request.getCommandName() != "getMore")) {

        auto linearizableReadStatus = waitForLinearizableReadConcern(opCtx);

        if (!linearizableReadStatus.isOK()) {
            inPlaceReplyBob.resetToEmpty();
            auto result = Command::appendCommandStatus(inPlaceReplyBob, linearizableReadStatus);
            inPlaceReplyBob.doneFast();
            replyBuilder->setMetadata(rpc::makeEmptyMetadata());
            return result;
        }
    }

    Command::appendCommandStatus(inPlaceReplyBob, result);

    auto operationTime = computeOperationTime(
        opCtx, startOperationTime, readConcernArgsStatus.getValue().getLevel());

    // An uninitialized operation time means the cluster time is not propagated, so the operation
    // time should not be attached to the response.
    if (operationTime != LogicalTime::kUninitialized &&
        serverGlobalParams.featureCompatibility.version.load() ==
            ServerGlobalParams::FeatureCompatibility::Version::k36) {
        Command::appendOperationTime(inPlaceReplyBob, operationTime);
    }

    inPlaceReplyBob.doneFast();

    BSONObjBuilder metadataBob;
    appendReplyMetadata(opCtx, request, &metadataBob);
    replyBuilder->setMetadata(metadataBob.done());

    return result;
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
                         rpc::ReplyBuilderInterface* replyBuilder) {

    auto startOperationTime = getClientOperationTime(opCtx);
    try {
        {
            stdx::lock_guard<Client> lk(*opCtx->getClient());
            CurOp::get(opCtx)->setCommand_inlock(command);
        }

        // TODO: move this back to runCommands when mongos supports OperationContext
        // see SERVER-18515 for details.
        rpc::readRequestMetadata(opCtx, request.body);
        rpc::TrackingMetadata::get(opCtx).initWithOperName(command->getName());

        initializeOperationSessionInfo(opCtx, request.body, command->requiresAuth());

        std::string dbname = request.getDatabase().toString();
        uassert(
            ErrorCodes::InvalidNamespace,
            str::stream() << "Invalid database name: '" << dbname << "'",
            NamespaceString::validDBName(dbname, NamespaceString::DollarInDbNameBehavior::Allow));

        std::unique_ptr<MaintenanceModeSetter> mmSetter;

        BSONElement cmdOptionMaxTimeMSField;
        BSONElement helpField;
        BSONElement shardVersionFieldIdx;
        BSONElement queryOptionMaxTimeMSField;

        StringMap<int> topLevelFields;
        for (auto&& element : request.body) {
            StringData fieldName = element.fieldNameStringData();
            if (fieldName == QueryRequest::cmdOptionMaxTimeMS) {
                cmdOptionMaxTimeMSField = element;
            } else if (fieldName == Command::kHelpFieldName) {
                helpField = element;
            } else if (fieldName == ChunkVersion::kShardVersionField) {
                shardVersionFieldIdx = element;
            } else if (fieldName == QueryRequest::queryOptionMaxTimeMS) {
                queryOptionMaxTimeMSField = element;
            }

            uassert(ErrorCodes::FailedToParse,
                    str::stream() << "Parsed command object contains duplicate top level key: "
                                  << fieldName,
                    topLevelFields[fieldName]++ == 0);
        }

        if (Command::isHelpRequest(helpField)) {
            CurOp::get(opCtx)->ensureStarted();
            // We disable last-error for help requests due to SERVER-11492, because config servers
            // use help requests to determine which commands are database writes, and so must be
            // forwarded to all config servers.
            LastError::get(opCtx->getClient()).disable();
            Command::generateHelpResponse(opCtx, replyBuilder, *command);
            return;
        }

        OperationContextSession sessionTxnState(opCtx);

        ImpersonationSessionGuard guard(opCtx);
        uassertStatusOK(Command::checkAuthorization(command, opCtx, request));

        repl::ReplicationCoordinator* replCoord =
            repl::ReplicationCoordinator::get(opCtx->getClient()->getServiceContext());
        const bool iAmPrimary = replCoord->canAcceptWritesForDatabase_UNSAFE(opCtx, dbname);

        if (!opCtx->getClient()->isInDirectClient()) {
            bool commandCanRunOnSecondary = command->slaveOk();

            bool commandIsOverriddenToRunOnSecondary =
                command->slaveOverrideOk() && ReadPreferenceSetting::get(opCtx).canRunOnSecondary();

            bool iAmStandalone = !opCtx->writesAreReplicated();
            bool canRunHere = iAmPrimary || commandCanRunOnSecondary ||
                commandIsOverriddenToRunOnSecondary || iAmStandalone;

            // This logic is clearer if we don't have to invert it.
            if (!canRunHere && command->slaveOverrideOk()) {
                uasserted(ErrorCodes::NotMasterNoSlaveOk, "not master and slaveOk=false");
            }

            uassert(ErrorCodes::NotMaster, "not master", canRunHere);

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
                        commandIsOverriddenToRunOnSecondary || commandCanRunOnSecondary);
            }
        }

        if (command->adminOnly()) {
            LOG(2) << "command: " << request.getCommandName();
        }

        if (command->maintenanceMode()) {
            mmSetter.reset(new MaintenanceModeSetter);
        }

        if (command->shouldAffectCommandCounter()) {
            OpCounters* opCounters = &globalOpCounters;
            opCounters->gotCommand();
        }

        // Handle command option maxTimeMS.
        int maxTimeMS = uassertStatusOK(QueryRequest::parseMaxTimeMS(cmdOptionMaxTimeMSField));

        uassert(ErrorCodes::InvalidOptions,
                "no such command option $maxTimeMs; use maxTimeMS instead",
                queryOptionMaxTimeMSField.eoo());

        if (maxTimeMS > 0) {
            uassert(40119,
                    "Illegal attempt to set operation deadline within DBDirectClient",
                    !opCtx->getClient()->isInDirectClient());
            opCtx->setDeadlineAfterNowBy(Milliseconds{maxTimeMS});
        }

        // We do not redo shard version handling if this command was issued via the direct client.
        if (!opCtx->getClient()->isInDirectClient()) {
            // Handle a shard version that may have been sent along with the command.
            auto commandNS = NamespaceString(command->parseNs(dbname, request.body));
            auto& oss = OperationShardingState::get(opCtx);
            oss.initializeShardVersion(commandNS, shardVersionFieldIdx);
            auto shardingState = ShardingState::get(opCtx);
            if (oss.hasShardVersion()) {
                uassertStatusOK(shardingState->canAcceptShardedCommands());
            }

            // Handle config optime information that may have been sent along with the command.
            uassertStatusOK(shardingState->updateConfigServerOpTimeFromMetadata(opCtx));
        }

        // Can throw
        opCtx->checkForInterrupt();  // May trigger maxTimeAlwaysTimeOut fail point.

        bool retval = false;

        CurOp::get(opCtx)->ensureStarted();

        command->incrementCommandsExecuted();

        if (logger::globalLogDomain()->shouldLog(logger::LogComponent::kTracking,
                                                 logger::LogSeverity::Debug(1)) &&
            rpc::TrackingMetadata::get(opCtx).getParentOperId()) {
            MONGO_LOG_COMPONENT(1, logger::LogComponent::kTracking)
                << rpc::TrackingMetadata::get(opCtx).toString();
            rpc::TrackingMetadata::get(opCtx).setIsLogged(true);
        }
        retval = runCommandImpl(opCtx, command, request, replyBuilder, startOperationTime);

        if (!retval) {
            command->incrementCommandsFailed();
        }
    } catch (const DBException& e) {
        // If we got a stale config, wait in case the operation is stuck in a critical section
        if (e.getCode() == ErrorCodes::SendStaleConfig) {
            auto sce = dynamic_cast<const StaleConfigException*>(&e);
            invariant(sce);  // do not upcasts from DBException created by uassert variants.

            if (!opCtx->getClient()->isInDirectClient()) {
                ShardingState::get(opCtx)
                    ->onStaleShardVersion(
                        opCtx, NamespaceString(sce->getns()), sce->getVersionReceived())
                    .transitional_ignore();
            }
        }

        BSONObjBuilder metadataBob;
        appendReplyMetadata(opCtx, request, &metadataBob);

        const std::string db = request.getDatabase().toString();
        auto readConcernArgsStatus =
            _extractReadConcern(request.body, command->supportsReadConcern(db, request.body));
        auto operationTime = readConcernArgsStatus.isOK()
            ? computeOperationTime(
                  opCtx, startOperationTime, readConcernArgsStatus.getValue().getLevel())
            : LogicalClock::get(opCtx)->getClusterTime();

        // An uninitialized operation time means the cluster time is not propagated, so the
        // operation time should not be attached to the error response.
        if (operationTime != LogicalTime::kUninitialized &&
            serverGlobalParams.featureCompatibility.version.load() ==
                ServerGlobalParams::FeatureCompatibility::Version::k36) {
            LOG(1) << "assertion while executing command '" << request.getCommandName() << "' "
                   << "on database '" << request.getDatabase() << "' "
                   << "with arguments '" << command->getRedactedCopyForLogging(request.body)
                   << "' and operationTime '" << operationTime.toString() << "': " << e.toString();

            _generateErrorResponse(opCtx, replyBuilder, e, metadataBob.obj(), operationTime);
        } else {
            LOG(1) << "assertion while executing command '" << request.getCommandName() << "' "
                   << "on database '" << request.getDatabase() << "' "
                   << "with arguments '" << command->getRedactedCopyForLogging(request.body)
                   << "': " << e.toString();

            _generateErrorResponse(opCtx, replyBuilder, e, metadataBob.obj());
        }
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

DbResponse runCommands(OperationContext* opCtx, const Message& message) {
    auto replyBuilder = rpc::makeReplyBuilder(rpc::protocolForMessage(message));

    // TODO SERVER-28964 If this parsing the request fails we reply to an invalid request which
    // isn't always safe. Unfortunately tests currently rely on this. Figure out what to do
    // (probably throw a special exception type like ConnectionFatalMessageParseError).
    bool canReply = true;
    auto curOp = CurOp::get(opCtx);
    boost::optional<OpMsgRequest> request;
    try {
        request.emplace(rpc::opMsgRequestFromAnyProtocol(message));  // Request is validated here.
        canReply = !request->isFlagSet(OpMsg::kMoreToCome);

        curOpCommandSetup(opCtx, *request);

        Command* c = nullptr;
        // In the absence of a Command object, no redaction is possible. Therefore
        // to avoid displaying potentially sensitive information in the logs,
        // we restrict the log message to the name of the unrecognized command.
        // However, the complete command object will still be echoed to the client.
        if (!(c = Command::findCommand(request->getCommandName()))) {
            Command::unknownCommands.increment();
            std::string msg = str::stream() << "no such command: '" << request->getCommandName()
                                            << "'";
            LOG(2) << msg;
            uasserted(ErrorCodes::CommandNotFound,
                      str::stream() << msg << ", bad cmd: '" << redact(request->body) << "'");
        }

        LOG(2) << "run command " << request->getDatabase() << ".$cmd" << ' '
               << c->getRedactedCopyForLogging(request->body);

        {
            // Try to set this as early as possible, as soon as we have figured out the command.
            stdx::lock_guard<Client> lk(*opCtx->getClient());
            curOp->setLogicalOp_inlock(c->getLogicalOp());
        }

        execCommandDatabase(opCtx, c, *request, replyBuilder.get());
    } catch (const DBException& ex) {
        if (request) {
            LOG(1) << "assertion while executing command '" << request->getCommandName() << "' "
                   << "on database '" << request->getDatabase() << "': " << ex.toString();
        } else {
            // We failed during parsing so we can't log anything about the command.
            LOG(1) << "assertion while executing command: " << ex.toString();
        }

        _generateErrorResponse(opCtx, replyBuilder.get(), ex, rpc::makeEmptyMetadata());
    }

    if (!canReply) {
        return {};
    }

    auto response = replyBuilder->done();
    curOp->debug().responseLength = response.header().dataLen();

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
        if (!opCtx->getClient()->isInDirectClient() && e.getCode() == ErrorCodes::SendStaleConfig) {
            auto& sce = static_cast<const StaleConfigException&>(e);
            ShardingState::get(opCtx)
                ->onStaleShardVersion(opCtx, NamespaceString(sce.getns()), sce.getVersionReceived())
                .transitional_ignore();
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

    int found = CursorManager::eraseCursorGlobalIfAuthorized(opCtx, n, cursorArray);

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
            // If a cursor with id 'cursorid' was authorized, it may have been advanced
            // before an exception terminated processGetMore.  Erase the ClientCursor
            // because it may now be out of sync with the client's iteration state.
            // SERVER-7952
            // TODO Temporary code, see SERVER-4563 for a cleanup overview.
            CursorManager::eraseCursorGlobal(opCtx, cursorid);
        }

        BSONObjBuilder err;
        e.getInfo().append(err);
        BSONObj errObj = err.done();

        curop.debug().exceptionInfo = e.getInfo();

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

DbResponse ServiceEntryPointMongod::handleRequest(OperationContext* opCtx, const Message& m) {
    // before we lock...
    NetworkOp op = m.operation();
    bool isCommand = false;

    DbMessage dbmsg(m);

    Client& c = *opCtx->getClient();
    if (c.isInDirectClient()) {
        invariant(!opCtx->lockState()->inAWriteUnitOfWork());
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
            opwrite(m);
        } else {
            opread(m);
        }
    } else if (op == dbGetMore) {
        opread(m);
    } else if (op == dbCommand || op == dbMsg) {
        isCommand = true;
        opwrite(m);
    } else {
        opwrite(m);
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

    long long logThresholdMs = serverGlobalParams.slowMS;
    bool shouldLogOpDebug = shouldLog(logger::LogSeverity::Debug(1));

    DbResponse dbresponse;
    if (op == dbMsg || op == dbCommand || (op == dbQuery && isCommand)) {
        dbresponse = runCommands(opCtx, m);
    } else if (op == dbQuery) {
        invariant(!isCommand);
        dbresponse = receivedQuery(opCtx, nsString, c, m);
    } else if (op == dbGetMore) {
        dbresponse = receivedGetMore(opCtx, m, currentOp, &shouldLogOpDebug);
    } else {
        // The remaining operations do not return any response. They are fire-and-forget.
        try {
            if (op == dbKillCursors) {
                currentOp.ensureStarted();
                logThresholdMs = 10;
                receivedKillCursors(opCtx, m);
            } else if (op != dbInsert && op != dbUpdate && op != dbDelete) {
                log() << "    operation isn't supported: " << static_cast<int>(op);
                currentOp.done();
                shouldLogOpDebug = true;
            } else {
                if (!opCtx->getClient()->isInDirectClient()) {
                    const ShardedConnectionInfo* connInfo = ShardedConnectionInfo::get(&c, false);
                    uassert(18663,
                            str::stream() << "legacy writeOps not longer supported for "
                                          << "versioned connections, ns: "
                                          << nsString.ns()
                                          << ", op: "
                                          << networkOpToString(op),
                            connInfo == NULL);
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
                    invariant(false);
                }
            }
        } catch (const UserException& ue) {
            LastError::get(c).setLastError(ue.getCode(), ue.getInfo().msg);
            LOG(3) << " Caught Assertion in " << networkOpToString(op) << ", continuing "
                   << redact(ue);
            debug.exceptionInfo = ue.getInfo();
        } catch (const AssertionException& e) {
            LastError::get(c).setLastError(e.getCode(), e.getInfo().msg);
            LOG(3) << " Caught Assertion in " << networkOpToString(op) << ", continuing "
                   << redact(e);
            debug.exceptionInfo = e.getInfo();
            shouldLogOpDebug = true;
        }
    }
    currentOp.ensureStarted();
    currentOp.done();
    debug.executionTimeMicros = durationCount<Microseconds>(currentOp.elapsedTimeExcludingPauses());

    Top::get(opCtx->getServiceContext())
        .incrementGlobalLatencyStats(
            opCtx,
            durationCount<Microseconds>(currentOp.elapsedTimeExcludingPauses()),
            currentOp.getReadWriteType());

    const bool shouldSample = serverGlobalParams.sampleRate == 1.0
        ? true
        : c.getPrng().nextCanonicalDouble() < serverGlobalParams.sampleRate;

    if (shouldLogOpDebug || (shouldSample && debug.executionTimeMicros > logThresholdMs * 1000LL)) {
        Locker::LockerInfo lockerInfo;
        opCtx->lockState()->getLockerInfo(&lockerInfo);
        log() << debug.report(&c, currentOp, lockerInfo.stats);
    }

    if (currentOp.shouldDBProfile(shouldSample)) {
        // Performance profiling is on
        if (opCtx->lockState()->isReadLocked()) {
            LOG(1) << "note: not profiling because recursive read lock";
        } else if (lockedForWriting()) {
            // TODO SERVER-26825: Fix race condition where fsyncLock is acquired post
            // lockedForWriting() call but prior to profile collection lock acquisition.
            LOG(1) << "note: not profiling because doing fsync+lock";
        } else if (storageGlobalParams.readOnly) {
            LOG(1) << "note: not profiling because server is read-only";
        } else {
            profile(opCtx, op);
        }
    }

    recordCurOpMetrics(opCtx);
    return dbresponse;
}

}  // namespace mongo
