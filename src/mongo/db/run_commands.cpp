/*    Copyright 2016 10gen Inc.
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
 *    must comply with the GNU Affero General Public License in all respects
 *    for all of the code used other than as permitted herein. If you modify
 *    file(s) with this exception, you may extend this exception to your
 *    version of the file(s), but you are not obligated to do so. If you do not
 *    wish to do so, delete this exception statement from your version. If you
 *    delete this exception statement from all source files in the program,
 *    then also delete it in the license file.
 */

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kCommand

#include "mongo/platform/basic.h"

#include "mongo/db/run_commands.h"

#include "mongo/db/auth/impersonation_session.h"
#include "mongo/db/client.h"
#include "mongo/db/commands.h"
#include "mongo/db/curop.h"
#include "mongo/db/lasterror.h"
#include "mongo/db/logical_clock.h"
#include "mongo/db/logical_time_validator.h"
#include "mongo/db/read_concern.h"
#include "mongo/db/repl/repl_client_info.h"
#include "mongo/db/repl/replication_coordinator_global.h"
#include "mongo/db/s/operation_sharding_state.h"
#include "mongo/db/s/sharding_state.h"
#include "mongo/db/stats/counters.h"
#include "mongo/rpc/metadata.h"
#include "mongo/rpc/metadata/config_server_metadata.h"
#include "mongo/rpc/metadata/logical_time_metadata.h"
#include "mongo/rpc/metadata/oplog_query_metadata.h"
#include "mongo/rpc/metadata/repl_set_metadata.h"
#include "mongo/rpc/metadata/sharding_metadata.h"
#include "mongo/rpc/metadata/tracking_metadata.h"
#include "mongo/rpc/reply_builder_interface.h"
#include "mongo/rpc/request_interface.h"
#include "mongo/s/grid.h"
#include "mongo/s/stale_exception.h"
#include "mongo/util/log.h"
#include "mongo/util/scopeguard.h"

namespace mongo {

namespace {

void registerError(OperationContext* opCtx, const DBException& exception) {
    CurOp::get(opCtx)->debug().exceptionInfo = exception.getInfo();
}

void _generateErrorResponse(OperationContext* opCtx,
                            rpc::ReplyBuilderInterface* replyBuilder,
                            const DBException& exception,
                            const BSONObj& metadata) {
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

    replyBuilder->setMetadata(metadata);
}

void _generateErrorResponse(OperationContext* opCtx,
                            rpc::ReplyBuilderInterface* replyBuilder,
                            const DBException& exception,
                            const BSONObj& metadata,
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

    replyBuilder->setMetadata(metadata);
}


/**
 * Generates a command error response. This overload of generateErrorResponse is intended
 * to also add an operationTime.
 */
void generateErrorResponse(OperationContext* opCtx,
                           rpc::ReplyBuilderInterface* replyBuilder,
                           const DBException& exception,
                           const rpc::RequestInterface& request,
                           Command* command,
                           const BSONObj& metadata,
                           LogicalTime operationTime) {
    LOG(1) << "assertion while executing command '" << request.getCommandName() << "' "
           << "on database '" << request.getDatabase() << "' "
           << "with arguments '" << command->getRedactedCopyForLogging(request.getCommandArgs())
           << "' metadata '" << request.getMetadata() << "' and operationTime '"
           << operationTime.toString() << "': " << exception.toString();

    _generateErrorResponse(opCtx, replyBuilder, exception, metadata, operationTime);
}

/**
 * When an assertion is hit during command execution, this method is used to fill the fields
 * of the command reply with the information from the error. In addition, information about
 * the command is logged. This function does not return anything, because there is typically
 * already an active exception when this function is called, so there
 * is little that can be done if it fails.
 */
void generateErrorResponse(OperationContext* opCtx,
                           rpc::ReplyBuilderInterface* replyBuilder,
                           const DBException& exception,
                           const rpc::RequestInterface& request,
                           Command* command,
                           const BSONObj& metadata) {
    LOG(1) << "assertion while executing command '" << request.getCommandName() << "' "
           << "on database '" << request.getDatabase() << "' "
           << "with arguments '" << command->getRedactedCopyForLogging(request.getCommandArgs())
           << "' "
           << "and metadata '" << request.getMetadata() << "': " << exception.toString();

    _generateErrorResponse(opCtx, replyBuilder, exception, metadata);
}

/**
 * Generates a command error response. This overload of generateErrorResponse is intended
 * to be called if the command is successfully parsed, but there is an error before we have
 * a handle to the actual Command object. This can happen, for example, when the command
 * is not found.
 */
void generateErrorResponse(OperationContext* opCtx,
                           rpc::ReplyBuilderInterface* replyBuilder,
                           const DBException& exception,
                           const rpc::RequestInterface& request) {
    LOG(1) << "assertion while executing command '" << request.getCommandName() << "' "
           << "on database '" << request.getDatabase() << "': " << exception.toString();

    _generateErrorResponse(opCtx, replyBuilder, exception, rpc::makeEmptyMetadata());
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
            repl::getGlobalReplicationCoordinator()->setMaintenanceMode(false);
    }

private:
    bool maintenanceModeSet;
};

void appendReplyMetadata(OperationContext* opCtx,
                         const rpc::RequestInterface& request,
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
        replCoord->prepareReplMetadata(
            opCtx, request.getMetadata(), lastOpTimeFromClient, metadataBob);
        // For commands from mongos, append some info to help getLastError(w) work.
        // TODO: refactor out of here as part of SERVER-18236
        if (isShardingAware || isConfig) {
            rpc::ShardingMetadata(lastOpTimeFromClient, replCoord->getElectionId())
                .writeToMetadata(metadataBob);
            if (LogicalTimeValidator::isAuthorizedToAdvanceClock(opCtx)) {
                // No need to sign logical times for internal clients.
                SignedLogicalTime currentTime(
                    LogicalClock::get(opCtx)->getClusterTime(), TimeProofService::TimeProof(), 0);
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
                                                 BSONObjBuilder* commandResponseBuilder) {
    WriteConcernResult res;
    auto waitForWCStatus =
        waitForWriteConcern(opCtx,
                            repl::ReplClientInfo::forClient(opCtx->getClient()).getLastOp(),
                            opCtx->getWriteConcern(),
                            &res);
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
 * uninitialized logical time.
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
 * reads, and the last applied optime for every other read. An uninitialized logical time is
 * returned for non replica set members.
 *
 * TODO: SERVER-28419 Do not compute operationTime if replica set does not propagate clusterTime.
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
                    const rpc::RequestInterface& request,
                    rpc::ReplyBuilderInterface* replyBuilder) {
    auto bytesToReserve = command->reserveBytesForReply();

// SERVER-22100: In Windows DEBUG builds, the CRT heap debugging overhead, in conjunction with the
// additional memory pressure introduced by reply buffer pre-allocation, causes the concurrency
// suite to run extremely slowly. As a workaround we do not pre-allocate in Windows DEBUG builds.
#ifdef _WIN32
    if (kDebugBuild)
        bytesToReserve = 0;
#endif

    // run expects non-const bsonobj
    BSONObj cmd = request.getCommandArgs();

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
                            << redact(command->getRedactedCopyForLogging(request.getCommandArgs()));
        }

        auto result = Command::appendCommandStatus(inPlaceReplyBob, rcStatus);
        inPlaceReplyBob.doneFast();
        replyBuilder->setMetadata(rpc::makeEmptyMetadata());
        return result;
    }

    std::string errmsg;
    bool result;
    auto startOperationTime = getClientOperationTime(opCtx);
    if (!command->supportsWriteConcern(cmd)) {
        if (commandSpecifiesWriteConcern(cmd)) {
            auto result = Command::appendCommandStatus(
                inPlaceReplyBob,
                {ErrorCodes::InvalidOptions, "Command does not support writeConcern"});
            inPlaceReplyBob.doneFast();
            replyBuilder->setMetadata(rpc::makeEmptyMetadata());
            return result;
        }

        // TODO: remove queryOptions parameter from command's run method.
        result = command->run(opCtx, db, cmd, errmsg, inPlaceReplyBob);
    } else {
        auto wcResult = extractWriteConcern(opCtx, cmd, db);
        if (!wcResult.isOK()) {
            auto result = Command::appendCommandStatus(inPlaceReplyBob, wcResult.getStatus());
            inPlaceReplyBob.doneFast();
            replyBuilder->setMetadata(rpc::makeEmptyMetadata());
            return result;
        }

        // Change the write concern while running the command.
        const auto oldWC = opCtx->getWriteConcern();
        ON_BLOCK_EXIT([&] { opCtx->setWriteConcern(oldWC); });
        opCtx->setWriteConcern(wcResult.getValue());
        ON_BLOCK_EXIT([&] {
            _waitForWriteConcernAndAddToCommandResponse(
                opCtx, command->getName(), &inPlaceReplyBob);
        });

        result = command->run(opCtx, db, cmd, errmsg, inPlaceReplyBob);

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

    Command::appendCommandStatus(inPlaceReplyBob, result, errmsg);

    auto operationTime = computeOperationTime(
        opCtx, startOperationTime, readConcernArgsStatus.getValue().getLevel());

    // An uninitialized operation time means the cluster time is not propagated, so the operation
    // time should not be attached to the response.
    if (operationTime != LogicalTime::kUninitialized) {
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
                         const rpc::RequestInterface& request,
                         rpc::ReplyBuilderInterface* replyBuilder) {
    try {
        {
            stdx::lock_guard<Client> lk(*opCtx->getClient());
            CurOp::get(opCtx)->setCommand_inlock(command);
        }

        // TODO: move this back to runCommands when mongos supports OperationContext
        // see SERVER-18515 for details.
        rpc::readRequestMetadata(opCtx, request.getMetadata());
        rpc::TrackingMetadata::get(opCtx).initWithOperName(command->getName());

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
        for (auto&& element : request.getCommandArgs()) {
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
            Command::generateHelpResponse(opCtx, request, replyBuilder, *command);
            return;
        }

        ImpersonationSessionGuard guard(opCtx);
        uassertStatusOK(
            Command::checkAuthorization(command, opCtx, dbname, request.getCommandArgs()));

        repl::ReplicationCoordinator* replCoord =
            repl::ReplicationCoordinator::get(opCtx->getClient()->getServiceContext());
        const bool iAmPrimary = replCoord->canAcceptWritesForDatabase_UNSAFE(opCtx, dbname);

        {
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

        // Operations are only versioned against the primary. We also make sure not to redo shard
        // version handling if this command was issued via the direct client.
        if (iAmPrimary && !opCtx->getClient()->isInDirectClient()) {
            // Handle a shard version that may have been sent along with the command.
            auto commandNS = NamespaceString(command->parseNs(dbname, request.getCommandArgs()));
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
        retval = runCommandImpl(opCtx, command, request, replyBuilder);

        if (!retval) {
            command->incrementCommandsFailed();
        }
    } catch (const DBException& e) {
        // If we got a stale config, wait in case the operation is stuck in a critical section
        if (e.getCode() == ErrorCodes::SendStaleConfig) {
            auto sce = dynamic_cast<const StaleConfigException*>(&e);
            invariant(sce);  // do not upcasts from DBException created by uassert variants.

            if (!opCtx->getClient()->isInDirectClient()) {
                ShardingState::get(opCtx)->onStaleShardVersion(
                    opCtx, NamespaceString(sce->getns()), sce->getVersionReceived());
            }
        }

        BSONObjBuilder metadataBob;
        appendReplyMetadata(opCtx, request, &metadataBob);

        // Ideally this should be using computeOperationTime, but with the code
        // structured as it currently is we don't know the startOperationTime or
        // readConcern at this point. Using the cluster time instead of the actual
        // operation time is correct, but can result in extra waiting on subsequent
        // afterClusterTime reads.
        //
        // TODO: SERVER-28445 change this to use computeOperationTime once the exception handling
        // path is moved into Command::run()
        auto operationTime = LogicalClock::get(opCtx)->getClusterTime();

        // An uninitialized operation time means the cluster time is not propagated, so the
        // operation time should not be attached to the error response.
        if (operationTime != LogicalTime::kUninitialized) {
            generateErrorResponse(
                opCtx, replyBuilder, e, request, command, metadataBob.done(), operationTime);
        } else {
            generateErrorResponse(opCtx, replyBuilder, e, request, command, metadataBob.done());
        }
    }
}
}  // namespace

void generateErrorResponse(OperationContext* opCtx,
                           rpc::ReplyBuilderInterface* replyBuilder,
                           const DBException& exception) {
    LOG(1) << "assertion while executing command: " << exception.toString();
    _generateErrorResponse(opCtx, replyBuilder, exception, rpc::makeEmptyMetadata());
}

void runCommands(OperationContext* opCtx,
                 const rpc::RequestInterface& request,
                 rpc::ReplyBuilderInterface* replyBuilder) {
    try {
        Command* c = nullptr;
        // In the absence of a Command object, no redaction is possible. Therefore
        // to avoid displaying potentially sensitive information in the logs,
        // we restrict the log message to the name of the unrecognized command.
        // However, the complete command object will still be echoed to the client.
        if (!(c = Command::findCommand(request.getCommandName()))) {
            Command::unknownCommands.increment();
            std::string msg = str::stream() << "no such command: '" << request.getCommandName()
                                            << "'";
            LOG(2) << msg;
            uasserted(ErrorCodes::CommandNotFound,
                      str::stream() << msg << ", bad cmd: '" << redact(request.getCommandArgs())
                                    << "'");
        }

        LOG(2) << "run command " << request.getDatabase() << ".$cmd" << ' '
               << c->getRedactedCopyForLogging(request.getCommandArgs());

        {
            // Try to set this as early as possible, as soon as we have figured out the command.
            stdx::lock_guard<Client> lk(*opCtx->getClient());
            CurOp::get(opCtx)->setLogicalOp_inlock(c->getLogicalOp());
        }

        execCommandDatabase(opCtx, c, request, replyBuilder);
    }

    catch (const DBException& ex) {
        generateErrorResponse(opCtx, replyBuilder, ex, request);
    }
}

}  // namespace mongo
