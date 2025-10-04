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

#include "mongo/s/transaction_router.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/status_with.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/client/read_preference.h"
#include "mongo/client/remote_command_retry_scheduler.h"
#include "mongo/db/commands/txn_cmds_gen.h"
#include "mongo/db/commands/txn_two_phase_commit_cmds_gen.h"
#include "mongo/db/database_name.h"
#include "mongo/db/global_catalog/catalog_cache/shard_cannot_refresh_due_to_locks_held_exception.h"
#include "mongo/db/global_catalog/router_role_api/cluster_commands_helpers.h"
#include "mongo/db/repl/read_concern_args.h"
#include "mongo/db/repl/read_concern_level.h"
#include "mongo/db/server_feature_flags_gen.h"
#include "mongo/db/server_options.h"
#include "mongo/db/session/logical_session_id.h"
#include "mongo/db/session/logical_session_id_helpers.h"
#include "mongo/db/session/session.h"
#include "mongo/db/sharding_environment/client/shard.h"
#include "mongo/db/sharding_environment/grid.h"
#include "mongo/db/transaction_validation.h"
#include "mongo/db/vector_clock/vector_clock.h"
#include "mongo/db/versioning_protocol/stale_exception.h"
#include "mongo/db/write_concern_options.h"
#include "mongo/executor/remote_command_response.h"
#include "mongo/executor/task_executor_pool.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/logv2/attribute_storage.h"
#include "mongo/logv2/log.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/platform/compiler.h"
#include "mongo/rpc/get_status_from_command_result_write_util.h"
#include "mongo/s/async_requests_sender.h"
#include "mongo/s/multi_statement_transaction_requests_sender.h"
#include "mongo/s/router_transactions_metrics.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/clock_source.h"
#include "mongo/util/decorable.h"
#include "mongo/util/exit.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/log_with_sampling.h"
#include "mongo/util/net/socket_utils.h"
#include "mongo/util/str.h"

#include <memory>
#include <utility>

#include <absl/container/flat_hash_map.h>
#include <absl/meta/type_traits.h>
#include <boost/cstdint.hpp>
#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional.hpp>
#include <boost/optional/optional.hpp>
#include <fmt/format.h>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTransaction

namespace mongo {
namespace {

// TODO SERVER-39704: Remove this fail point once the router can safely retry within a transaction
// on stale version and snapshot errors.
MONGO_FAIL_POINT_DEFINE(enableStaleVersionAndSnapshotRetriesWithinTransactions);

MONGO_FAIL_POINT_DEFINE(hangWhenSubRouterHandlesResponseFromAddedParticipant);

const char kCoordinatorField[] = "coordinator";
const char kReadConcernLevelSnapshotName[] = "snapshot";

const auto getTransactionRouter = Session::declareDecoration<TransactionRouter>();

// Commands that are idempotent in a transaction context and can be blindly retried in the middle of
// a transaction. Writing aggregates (e.g. with a $out or $merge) is disallowed in a transaction, so
// aggregates must be read operations. Note: aggregate and find do have the side-effect of creating
// cursors, but any established during an unsuccessful attempt are best-effort killed.
const StringMap<int> alwaysRetryableCmds = {
    {"aggregate", 1}, {"distinct", 1}, {"find", 1}, {"getMore", 1}, {"killCursors", 1}};

// Returns if a transaction's commit result is unknown based on the given statuses. A result is
// considered unknown if it would be given the "UnknownTransactionCommitResult" as defined by the
// driver transactions specification or fails with one of the errors for invalid write concern that
// are specifically not given the "UnknownTransactionCommitResult" label. Additionally,
// TransactionTooOld is considered unknown because a command that fails with it could not have done
// meaningful work.
//
// The "UnknownTransactionCommitResult" specification:
// https://github.com/mongodb/specifications/blob/master/source/transactions/transactions.rst#unknowntransactioncommitresult.
bool isCommitResultUnknown(const Status& commitStatus, const Status& commitWCStatus) {
    if (!commitStatus.isOK()) {
        return isMongosRetriableError(commitStatus.code()) ||
            ErrorCodes::isExceededTimeLimitError(commitStatus) ||
            commitStatus.code() == ErrorCodes::WriteConcernTimeout ||
            commitStatus.code() == ErrorCodes::TransactionTooOld;
    }

    if (!commitWCStatus.isOK()) {
        return true;
    }

    return false;
}

BSONObj sendCommitDirectlyToShards(OperationContext* opCtx, const std::vector<ShardId>& shardIds) {
    // Assemble requests.
    std::vector<AsyncRequestsSender::Request> requests;
    for (const auto& shardId : shardIds) {
        CommitTransaction commitCmd;
        commitCmd.setDbName(DatabaseName::kAdmin);
        commitCmd.setWriteConcern(opCtx->getWriteConcern());
        const auto commitCmdObj = commitCmd.toBSON();
        requests.emplace_back(shardId, commitCmdObj);
    }

    // Send the requests.
    MultiStatementTransactionRequestsSender ars(
        opCtx,
        Grid::get(opCtx)->getExecutorPool()->getFixedExecutor(),
        DatabaseName::kAdmin,
        requests,
        ReadPreferenceSetting{ReadPreference::PrimaryOnly},
        Shard::RetryPolicy::kIdempotent);

    BSONObj lastResult;

    // Receive the responses.
    while (!ars.done()) {
        auto response = ars.next();

        uassertStatusOK(response.swResponse);
        lastResult = response.swResponse.getValue().data;

        // If any shard returned an error, return the error immediately.
        const auto commandStatus = getStatusFromCommandResult(lastResult);
        if (!commandStatus.isOK()) {
            return lastResult;
        }

        // If any participant had a writeConcern error, return the participant's writeConcern
        // error immediately.
        const auto writeConcernStatus = getWriteConcernStatusFromCommandResult(lastResult);
        if (!writeConcernStatus.isOK()) {
            return lastResult;
        }
    }

    // If all the responses were ok, return the last response.
    return lastResult;
}

// Helper to convert the CommitType enum into a human readable string for diagnostics.
std::string commitTypeToString(TransactionRouter::CommitType state) {
    switch (state) {
        case TransactionRouter::CommitType::kNotInitiated:
            return "notInitiated";
        case TransactionRouter::CommitType::kNoShards:
            return "noShards";
        case TransactionRouter::CommitType::kSingleShard:
            return "singleShard";
        case TransactionRouter::CommitType::kSingleWriteShard:
            return "singleWriteShard";
        case TransactionRouter::CommitType::kReadOnly:
            return "readOnly";
        case TransactionRouter::CommitType::kTwoPhaseCommit:
            return "twoPhaseCommit";
        case TransactionRouter::CommitType::kRecoverWithToken:
            return "recoverWithToken";
    }
    MONGO_UNREACHABLE;
}

std::string actionTypeToString(TransactionRouter::TransactionActions action) {
    switch (action) {
        case TransactionRouter::TransactionActions::kStart:
            return "start";
        case TransactionRouter::TransactionActions::kStartOrContinue:
            return "startOrContinue";
        case TransactionRouter::TransactionActions::kContinue:
            return "continue";
        case TransactionRouter::TransactionActions::kCommit:
            return "commit";
    }
    MONGO_UNREACHABLE;
}

/**
 * - If the 'readConcernArgs' specifies an 'atClusterTime', sets the 'txnAtClusterTime' to
 *   that 'atClusterTime'.
 * - If the 'readConcernArgs' specifies an 'afterClusterTime', sets the 'txnAtClusterTime' to
 *   the greater of that 'afterClusterTime' and the 'candidateTime'.
 * - If neither is specified, sets the 'txnAtClusterTime' to the 'candidateTime'.
 */
void setAtClusterTime(const LogicalSessionId& lsid,
                      const TxnNumberAndRetryCounter& txnNumberAndRetryCounter,
                      StmtId latestStmtId,
                      LogicalTime* txnAtClusterTime,
                      const repl::ReadConcernArgs& readConcernArgs,
                      const LogicalTime& candidateTime) {
    auto requestedAtClusterTime = readConcernArgs.getArgsAtClusterTime();
    auto requestedAfterClusterTime = readConcernArgs.getArgsAfterClusterTime();
    tassert(7976601,
            "Cannot specify both 'atClusterTime' and 'afterClusterTime'",
            !requestedAtClusterTime || !requestedAfterClusterTime);

    auto atClusterTime = [&] {
        if (requestedAtClusterTime) {
            return *requestedAtClusterTime;
        }
        // If the user passed afterClusterTime, the chosen time must be greater than or equal to it.
        if (requestedAfterClusterTime && *requestedAfterClusterTime > candidateTime) {

            return *requestedAfterClusterTime;
        }
        return candidateTime;
    }();

    invariant(atClusterTime != LogicalTime::kUninitialized);
    LOGV2_DEBUG(22888,
                2,
                "Setting global snapshot timestamp for transaction",
                "sessionId"_attr = lsid,
                "txnNumber"_attr = txnNumberAndRetryCounter.getTxnNumber(),
                "txnRetryCounter"_attr = txnNumberAndRetryCounter.getTxnRetryCounter(),
                "globalSnapshotTimestamp"_attr = atClusterTime,
                "latestStmtId"_attr = latestStmtId);
    *txnAtClusterTime = atClusterTime;
}

struct StrippedFields {
public:
    boost::optional<repl::ReadConcernArgs> readConcern;
    boost::optional<ShardVersion> shardVersion;
    boost::optional<DatabaseVersion> databaseVersion;
    std::vector<NamespaceInfoEntry> nsInfoEntries;
    bool isBulkWriteCommand = false;
};

/**
 * Returns the readConcern, shardVersion and databaseVersion settings from the cmdObj. If a
 * BSONObjBuilder is provided, it will append the original fields from the cmdObj except for the
 * aforementioned fields.
 */
StrippedFields stripReadConcernAndShardAndDbVersions(const BSONObj& cmdObj,
                                                     BSONObjBuilder* cmdWithoutReadConcernBuilder) {
    BSONObjBuilder strippedCmdBuilder;
    StrippedFields strippedFields;
    for (auto&& elem : cmdObj) {
        if (elem.fieldNameStringData() == repl::ReadConcernArgs::kReadConcernFieldName) {
            strippedFields.readConcern =
                repl::ReadConcernArgs::fromBSONThrows(elem.embeddedObject());
            continue;
        } else if (elem.fieldNameStringData() == ShardVersion::kShardVersionField) {
            strippedFields.shardVersion = ShardVersion::parse(elem);
            continue;
        } else if (elem.fieldNameStringData() == DatabaseVersion::kDatabaseVersionField) {
            strippedFields.databaseVersion = DatabaseVersion(elem.embeddedObject());
            continue;
        }
        // For the BulkWriteCommandRequest we need to strip all the NamespaceInfoEntry so that
        // we can update the version in each of them. Ensure to extract nsInfo only in case
        // of bulkWrite.
        if (elem.fieldNameStringData() == BulkWriteCommandRequest::kCommandName) {
            strippedFields.isBulkWriteCommand = true;
        }
        if (strippedFields.isBulkWriteCommand &&
            elem.fieldNameStringData() == BulkWriteCommandRequest::kNsInfoFieldName) {
            for (auto&& nsInfoEntry : elem.embeddedObject()) {
                IDLParserContext context(BulkWriteCommandRequest::kNsInfoFieldName);
                strippedFields.nsInfoEntries.emplace_back(
                    NamespaceInfoEntry::parse(nsInfoEntry.embeddedObject(), context));
            }
            continue;
        }

        if (cmdWithoutReadConcernBuilder) {
            cmdWithoutReadConcernBuilder->append(elem);
        }
    }
    return strippedFields;
}

// Sets the placementConflictTime metadata on 'databaseVersion' if needed.
void setPlacementConflictTimeToDatabaseVersionIfNeeded(
    const boost::optional<LogicalTime>& placementConflictTimeForNonSnapshotReadConcern,
    bool hasTxnCreatedAnyDatabase,
    DatabaseVersion& databaseVersion) {
    if (hasTxnCreatedAnyDatabase) {
        // If any database has been created within this transaction, ask the shards to avoid
        // checking placement timestamp conflicts for this database
        // (placementConflictTimestamp=Timestamp(0,0) conveys that). The reason is that the
        // newly-created database will have a timestamp greater than this transaction's timestamp,
        // so it would always conflict. This would prevent the transaction from reading its own
        // writes.
        databaseVersion.setPlacementConflictTime(LogicalTime(Timestamp(0, 0)));
    } else if (placementConflictTimeForNonSnapshotReadConcern) {
        databaseVersion.setPlacementConflictTime(*placementConflictTimeForNonSnapshotReadConcern);
    }
}

void setPlacementConflictTimeToBulkWrite(std::vector<NamespaceInfoEntry>& nsInfoEntries,
                                         bool hasTxnCreatedAnyDatabase,
                                         boost::optional<LogicalTime> placementConflictTime,
                                         BSONObjBuilder* cmdBob) {

    BSONArrayBuilder arrayBuilder(cmdBob->subarrayStart(BulkWriteCommandRequest::kNsInfoFieldName));
    for (auto& nsInfoEntry : nsInfoEntries) {
        BSONObjBuilder subObjBuilder(arrayBuilder.subobjStart());
        auto shardVersion = nsInfoEntry.getShardVersion();
        auto dbVersion = nsInfoEntry.getDatabaseVersion();
        if (dbVersion) {
            setPlacementConflictTimeToDatabaseVersionIfNeeded(
                placementConflictTime, hasTxnCreatedAnyDatabase, *dbVersion);
            nsInfoEntry.setDatabaseVersion(dbVersion);
        }
        if (shardVersion) {
            if (placementConflictTime) {
                shardVersion->setPlacementConflictTime(*placementConflictTime);
            }
            nsInfoEntry.setShardVersion(shardVersion);
        }
        nsInfoEntry.serialize(&subObjBuilder);
        subObjBuilder.doneFast();
    }
    arrayBuilder.doneFast();
}

}  // namespace

TransactionRouter::TransactionRouter() = default;

TransactionRouter::~TransactionRouter() = default;

TransactionRouter::Observer::Observer(const ObservableSession& osession)
    : Observer(&getTransactionRouter(osession.get())) {}

TransactionRouter::Router::Router(OperationContext* opCtx)
    : Observer([opCtx]() -> TransactionRouter* {
          if (auto session = OperationContextSession::get(opCtx)) {
              return &getTransactionRouter(session);
          }
          return nullptr;
      }()) {}

TransactionRouter::Participant::Participant(bool inIsCoordinator,
                                            StmtId inStmtIdCreatedAt,
                                            ReadOnly inReadOnly,
                                            SharedTransactionOptions inSharedOptions,
                                            bool isSubRouter)
    : isCoordinator(inIsCoordinator),
      readOnly(inReadOnly),
      sharedOptions(std::move(inSharedOptions)),
      stmtIdCreatedAt(inStmtIdCreatedAt),
      isSubRouter(isSubRouter) {}

BSONObj TransactionRouter::Observer::reportState(OperationContext* opCtx,
                                                 bool sessionIsActive) const {
    BSONObjBuilder builder;
    reportState(opCtx, &builder, sessionIsActive);
    return builder.obj();
}

void TransactionRouter::Observer::reportState(OperationContext* opCtx,
                                              BSONObjBuilder* builder,
                                              bool sessionIsActive) const {
    _reportState(opCtx, builder, sessionIsActive);
}

void TransactionRouter::Observer::_reportState(OperationContext* opCtx,
                                               BSONObjBuilder* builder,
                                               bool sessionIsActive) const {
    if (!isInitialized()) {
        // This transaction router is not yet initialized.
        return;
    }

    // Append relevant client metadata for transactions with inactive sessions. For those with
    // active sessions, these fields will already be in the output.

    if (!sessionIsActive) {
        builder->append("type", "idleSession");
        builder->append("host", prettyHostNameAndPort(opCtx->getClient()->getLocalPort()));
        builder->append("desc", "inactive transaction");

        const auto& lastClientInfo = o().lastClientInfo;
        builder->append("client", lastClientInfo.clientHostAndPort);
        builder->append("connectionId", lastClientInfo.connectionId);
        builder->append("appName", lastClientInfo.appName);
        builder->append("clientMetadata", lastClientInfo.clientMetadata);

        {
            BSONObjBuilder lsid(builder->subobjStart("lsid"));
            _sessionId().serialize(&lsid);
        }

        builder->append("active", sessionIsActive);
    }

    // Append current transaction info.

    BSONObjBuilder transactionBuilder;
    _reportTransactionState(opCtx, &transactionBuilder);
    builder->append("transaction", transactionBuilder.obj());
}

void TransactionRouter::Observer::_reportTransactionState(OperationContext* opCtx,
                                                          BSONObjBuilder* builder) const {
    {
        BSONObjBuilder parametersBuilder(builder->subobjStart("parameters"));
        parametersBuilder.append("txnNumber", o().txnNumberAndRetryCounter.getTxnNumber());
        parametersBuilder.append("txnRetryCounter",
                                 *o().txnNumberAndRetryCounter.getTxnRetryCounter());
        parametersBuilder.append("autocommit", false);

        if (!o().readConcernArgs.isEmpty()) {
            o().readConcernArgs.appendInfo(&parametersBuilder);
        }
    }

    if (_atClusterTimeHasBeenSet()) {
        builder->append("globalReadTimestamp",
                        o().atClusterTimeForSnapshotReadConcern->asTimestamp());
    }

    const auto& timingStats = o().metricsTracker->getTimingStats();

    builder->append("startWallClockTime", dateToISOStringLocal(timingStats.startWallClockTime));

    auto tickSource = opCtx->getServiceContext()->getTickSource();
    auto curTicks = tickSource->getTicks();

    builder->append("timeOpenMicros",
                    durationCount<Microseconds>(timingStats.getDuration(tickSource, curTicks)));

    builder->append(
        "timeActiveMicros",
        durationCount<Microseconds>(timingStats.getTimeActiveMicros(tickSource, curTicks)));

    builder->append(
        "timeInactiveMicros",
        durationCount<Microseconds>(timingStats.getTimeInactiveMicros(tickSource, curTicks)));

    int numReadOnlyParticipants = 0;
    int numNonReadOnlyParticipants = 0;

    // We don't know the participants if we're recovering the commit.
    if (o().commitType != CommitType::kRecoverWithToken) {
        builder->append("numParticipants", static_cast<int>(o().participants.size()));

        BSONArrayBuilder participantsArrayBuilder;
        for (auto const& participantPair : o().participants) {
            BSONObjBuilder participantBuilder;
            participantBuilder.append("name", participantPair.first);
            participantBuilder.append("coordinator", participantPair.second.isCoordinator);

            if (participantPair.second.readOnly == Participant::ReadOnly::kReadOnly) {
                participantBuilder.append("readOnly", true);
                ++numReadOnlyParticipants;
            } else if (participantPair.second.readOnly == Participant::ReadOnly::kNotReadOnly) {
                participantBuilder.append("readOnly", false);
                ++numNonReadOnlyParticipants;
            }
            participantsArrayBuilder.append(participantBuilder.obj());
        }

        builder->appendArray("participants", participantsArrayBuilder.obj());
    }

    if (o().metricsTracker->commitHasStarted()) {
        builder->append("commitStartWallClockTime",
                        dateToISOStringLocal(timingStats.commitStartWallClockTime));
        builder->append("commitType", commitTypeToString(o().commitType));
    }

    builder->append("numReadOnlyParticipants", numReadOnlyParticipants);
    builder->append("numNonReadOnlyParticipants", numNonReadOnlyParticipants);
}

bool TransactionRouter::Observer::_atClusterTimeHasBeenSet() const {
    return o().atClusterTimeForSnapshotReadConcern &&
        *o().atClusterTimeForSnapshotReadConcern != LogicalTime::kUninitialized;
}

const LogicalSessionId& TransactionRouter::Observer::_sessionId() const {
    const auto* owningSession = getTransactionRouter.owner(_tr);
    return owningSession->getSessionId();
}

BSONObj TransactionRouter::Participant::attachTxnFieldsIfNeeded(
    OperationContext* opCtx,
    BSONObj cmd,
    bool isFirstStatementInThisParticipant,
    bool addingParticipantViaSubRouter,
    bool hasTxnCreatedAnyDatabase) const {
    bool hasStartTxn = false;
    bool hasAutoCommit = false;
    bool hasTxnNum = false;

    BSONObjIterator iter(cmd);
    while (iter.more()) {
        auto elem = iter.next();

        if (OperationSessionInfoFromClient::kStartTransactionFieldName ==
            elem.fieldNameStringData()) {
            hasStartTxn = true;
        } else if (OperationSessionInfoFromClient::kAutocommitFieldName ==
                   elem.fieldNameStringData()) {
            hasAutoCommit = true;
        } else if (OperationSessionInfoFromClient::kTxnNumberFieldName ==
                   elem.fieldNameStringData()) {
            hasTxnNum = true;
        }
    }

    // The first command sent to a participant must start a transaction, unless it is a transaction
    // command, which don't support the options that start transactions, i.e. startTransaction and
    // readConcern. Otherwise the command must not have a read concern.
    auto cmdName = cmd.firstElement().fieldNameStringData();
    auto service = opCtx->getService();
    bool mustStartTransaction =
        (isFirstStatementInThisParticipant || addingParticipantViaSubRouter) &&
        !isTransactionCommand(service, cmdName);

    // Strip the command of its read concern if it should not have one.
    if (!mustStartTransaction) {
        auto readConcernFieldName = repl::ReadConcernArgs::kReadConcernFieldName;
        if (cmd.hasField(readConcernFieldName) &&
            !sharedOptions.isInternalTransactionForRetryableWrite) {
            cmd = cmd.removeField(readConcernFieldName);
        }
    }

    BSONObjBuilder newCmd = mustStartTransaction
        ? appendFieldsForStartTransaction(
              std::move(cmd),
              sharedOptions.readConcernArgs,
              sharedOptions.atClusterTimeForSnapshotReadConcern,
              sharedOptions.placementConflictTimeForNonSnapshotReadConcern,
              !hasStartTxn,
              addingParticipantViaSubRouter,
              hasTxnCreatedAnyDatabase)
        : appendFieldsForContinueTransaction(
              std::move(cmd),
              sharedOptions.placementConflictTimeForNonSnapshotReadConcern,
              hasTxnCreatedAnyDatabase);

    if (isCoordinator) {
        newCmd.append(kCoordinatorField, true);
    }

    if (!hasAutoCommit) {
        newCmd.append(OperationSessionInfoFromClient::kAutocommitFieldName, false);
    }

    if (!hasTxnNum) {
        newCmd.append(OperationSessionInfoFromClient::kTxnNumberFieldName,
                      sharedOptions.txnNumberAndRetryCounter.getTxnNumber());
    } else {
        auto osi = OperationSessionInfoFromClient::parse(newCmd.asTempObj(),
                                                         IDLParserContext{"OperationSessionInfo"});
        invariant(sharedOptions.txnNumberAndRetryCounter.getTxnNumber() == *osi.getTxnNumber());
    }

    if (auto txnRetryCounter = sharedOptions.txnNumberAndRetryCounter.getTxnRetryCounter();
        txnRetryCounter && !isDefaultTxnRetryCounter(*txnRetryCounter)) {
        newCmd.append(OperationSessionInfoFromClient::kTxnRetryCounterFieldName,
                      *sharedOptions.txnNumberAndRetryCounter.getTxnRetryCounter());
    }

    return newCmd.obj();
}

BSONObj TransactionRouter::appendFieldsForContinueTransaction(
    BSONObj cmdObj,
    const boost::optional<LogicalTime>& placementConflictTimeForNonSnapshotReadConcern,
    bool hasTxnCreatedAnyDatabase) {
    BSONObjBuilder cmdBob;
    auto strippedFields = stripReadConcernAndShardAndDbVersions(cmdObj, &cmdBob);

    // The bulkWrite requires the shard and database versions on an internal field for
    // every write operation, which has different layout compared to a standard command and it
    // requires special handling. Update the shard/db version for every nsInfo entry.
    if (strippedFields.isBulkWriteCommand) {
        setPlacementConflictTimeToBulkWrite(strippedFields.nsInfoEntries,
                                            hasTxnCreatedAnyDatabase,
                                            placementConflictTimeForNonSnapshotReadConcern,
                                            &cmdBob);
    }

    if (auto shardVersion = strippedFields.shardVersion) {
        if (placementConflictTimeForNonSnapshotReadConcern) {
            shardVersion->setPlacementConflictTime(*placementConflictTimeForNonSnapshotReadConcern);
        }

        shardVersion->serialize(ShardVersion::kShardVersionField, &cmdBob);
    }

    if (auto databaseVersion = strippedFields.databaseVersion) {
        setPlacementConflictTimeToDatabaseVersionIfNeeded(
            placementConflictTimeForNonSnapshotReadConcern,
            hasTxnCreatedAnyDatabase,
            *databaseVersion);

        BSONObjBuilder dbvBuilder(cmdBob.subobjStart(DatabaseVersion::kDatabaseVersionField));
        databaseVersion->serialize(&dbvBuilder);
    }
    return cmdBob.obj();
}

TransactionRouter::ParsedParticipantResponseMetadata
TransactionRouter::Router::parseParticipantResponseMetadata(const BSONObj& responseObj) {
    return {.status = getStatusFromCommandResult(responseObj),
            .txnResponseMetadata = TxnResponseMetadata::parse(
                responseObj, IDLParserContext{"processParticipantResponse"})};
}

void TransactionRouter::Router::processParticipantResponse(
    OperationContext* opCtx,
    const ShardId& shardId,
    const TransactionRouter::ParsedParticipantResponseMetadata& parsedMetadata,
    bool forAsyncGetMore) {
    auto participant = getParticipant(shardId);
    invariant(participant, "Participant should exist if processing participant response");

    if (MONGO_unlikely(hangWhenSubRouterHandlesResponseFromAddedParticipant.shouldFail())) {
        hangWhenSubRouterHandlesResponseFromAddedParticipant.pauseWhileSet();
    }

    if (p().terminationInitiated) {
        // Do not process the transaction metadata after commit or abort have been initiated,
        // since a participant's state is partially reset on commit and abort.
        return;
    }

    auto determineReadOnlyValue =
        [&](const ShardId& shardIdToUpdate,
            Participant::ReadOnly readOnlyCurrent,
            boost::optional<bool> readOnlyResponse,
            bool isAdditionalParticipant) -> boost::optional<Participant::ReadOnly> {
        if (!readOnlyResponse) {
            // It's possible not to have a readOnly value for an additional participant shard yet in
            // the following cases:
            // 1. A shard acting as a sub-router targets itself. This shard acting as the
            // participant targeted by the sub-router would include itself as an additional
            // participant in its response back to itself, but the sub-router would not yet track a
            // readOnly value for itself.
            // 2. Some getMore responses, when async getMore machinery (AsyncRequestsMerger) is
            // used. It's possible that a batch is filled before all additional shards have
            // responded. All additional participants will be included in the response, even if they
            // have not responded yet (and thus, don't have a readOnly value yet).
            uassert(8755800,
                    str::stream() << "readOnly is missing from participant " << shardIdToUpdate
                                  << " response metadata",
                    isAdditionalParticipant);

            if (!o().subRouter) {
                uassert(8980600,
                        str::stream()
                            << "readOnly is missing for additional participant " << shardIdToUpdate
                            << " in the response metadata for a non-getMore"
                            << " request",
                        forAsyncGetMore);

                if (readOnlyCurrent == Participant::ReadOnly::kUnset) {
                    // It is safe to assume that this participant has only done reads at this point,
                    // because doing writes as part of a getMore op running in a transaction is
                    // disallowed.

                    LOGV2_DEBUG(8980601,
                                3,
                                "Marking additional participant as read-only participant",
                                "sessionId"_attr = _sessionId(),
                                "txnNumber"_attr = o().txnNumberAndRetryCounter.getTxnNumber(),
                                "txnRetryCounter"_attr =
                                    o().txnNumberAndRetryCounter.getTxnRetryCounter(),
                                "shardId"_attr = shardIdToUpdate);

                    return Participant::ReadOnly::kReadOnly;
                }
            }

            return boost::none;
        }

        // The shard reported readOnly: true
        if (*readOnlyResponse) {
            if (readOnlyCurrent == Participant::ReadOnly::kUnset) {
                LOGV2_DEBUG(22880,
                            3,
                            "Marking shard as read-only participant",
                            "sessionId"_attr = _sessionId(),
                            "txnNumber"_attr = o().txnNumberAndRetryCounter.getTxnNumber(),
                            "txnRetryCounter"_attr =
                                o().txnNumberAndRetryCounter.getTxnRetryCounter(),
                            "shardId"_attr = shardIdToUpdate);

                return Participant::ReadOnly::kReadOnly;
            }

            uassert(51113,
                    str::stream() << "Participant shard " << shardIdToUpdate
                                  << " claimed to be read-only for a transaction after previously "
                                     "claiming to have done a write for the transaction",
                    readOnlyCurrent == Participant::ReadOnly::kReadOnly);
            return boost::none;
        }

        // The shard reported readOnly: false
        if (readOnlyCurrent != Participant::ReadOnly::kNotReadOnly) {
            if (!p().recoveryShardId) {
                LOGV2_DEBUG(22882,
                            3,
                            "Choosing shard as recovery shard",
                            "sessionId"_attr = _sessionId(),
                            "txnNumber"_attr = o().txnNumberAndRetryCounter.getTxnNumber(),
                            "txnRetryCounter"_attr =
                                o().txnNumberAndRetryCounter.getTxnRetryCounter(),
                            "shardId"_attr = shardIdToUpdate);
                p().recoveryShardId = shardIdToUpdate;
            }

            LOGV2_DEBUG(22881,
                        3,
                        "Marking shard has having done a write",
                        "sessionId"_attr = _sessionId(),
                        "txnNumber"_attr = o().txnNumberAndRetryCounter.getTxnNumber(),
                        "txnRetryCounter"_attr = o().txnNumberAndRetryCounter.getTxnRetryCounter(),
                        "shardId"_attr = shardIdToUpdate);

            return Participant::ReadOnly::kNotReadOnly;
        }

        return boost::none;
    };

    auto processAdditionalParticipants = [&](bool okResponse) {
        const auto& additionalParticipants =
            parsedMetadata.txnResponseMetadata.getAdditionalParticipants();
        if (!additionalParticipants)
            return;

        for (auto&& participantElem : *additionalParticipants) {
            auto participantToAdd = participantElem.getShardId();
            Participant::ReadOnly currentReadOnly;
            auto existingParticipant = getParticipant(participantToAdd);
            if (!existingParticipant) {
                auto createdParticipant = _createParticipant(opCtx, participantToAdd);
                currentReadOnly = createdParticipant.readOnly;

                if (!p().isRecoveringCommit) {
                    // Don't update participant stats during recovery since the participant list
                    // isn't known.
                    RouterTransactionsMetrics::get(opCtx)->incrementTotalContactedParticipants();
                }
            } else {
                currentReadOnly = existingParticipant->readOnly;
            }

            if (okResponse) {
                auto readOnlyValue = determineReadOnlyValue(participantToAdd,
                                                            currentReadOnly,
                                                            participantElem.getReadOnly(),
                                                            true /* isAdditionalParticipant */);

                // We only care about the isSubRouter value for participants in the top level
                // transaction router, so we default the value to false for any additional
                // participants.
                _updateParticipant(opCtx, participantToAdd, readOnlyValue, false /* isSubRouter */);
            }
        }
    };
    boost::optional<Participant::ReadOnly> readOnlyValue = boost::none;
    const auto& commandStatus = parsedMetadata.status;
    // WouldChangeOwningShard errors don't abort their transaction and the responses containing them
    // include transaction metadata, so we treat them as successful responses.
    if (!commandStatus.isOK() && commandStatus != ErrorCodes::WouldChangeOwningShard) {
        // We should still add any participants added to the transaction to ensure they will be
        // aborted
        processAdditionalParticipants(false /* okResponse */);
    } else {
        readOnlyValue = determineReadOnlyValue(shardId,
                                               participant->readOnly,
                                               parsedMetadata.txnResponseMetadata.getReadOnly(),
                                               false /* isAdditionalParticipant */);
        // Create any participants added by the shard 'shardId'
        processAdditionalParticipants(true /* okResponse */);
    }

    // Set isSubRouter for this participant
    bool shardIsSubRouter = parsedMetadata.txnResponseMetadata.getAdditionalParticipants() &&
        parsedMetadata.txnResponseMetadata.getAdditionalParticipants()->size() > 0;

    _updateParticipant(opCtx, shardId, readOnlyValue, shardIsSubRouter);
}

boost::optional<LogicalTime> TransactionRouter::Router::getSelectedAtClusterTime() const {
    return o().atClusterTimeForSnapshotReadConcern;
}

boost::optional<LogicalTime> TransactionRouter::Router::getPlacementConflictTime() const {
    invariant(isInitialized());
    return o().placementConflictTimeForNonSnapshotReadConcern;
}

const boost::optional<ShardId>& TransactionRouter::Router::getCoordinatorId() const {
    return o().coordinatorId;
}

const boost::optional<ShardId>& TransactionRouter::Router::getRecoveryShardId() const {
    return p().recoveryShardId;
}

boost::optional<StringMap<boost::optional<bool>>>
TransactionRouter::Router::getAdditionalParticipantsForResponse(OperationContext* opCtx) {
    boost::optional<StringMap<boost::optional<bool>>> participants = boost::none;

    if (!o().subRouter || (opCtx->getTxnNumber() != o().txnNumberAndRetryCounter.getTxnNumber()) ||
        (opCtx->getTxnRetryCounter() &&
         (opCtx->getTxnRetryCounter() != o().txnNumberAndRetryCounter.getTxnRetryCounter()))) {
        return participants;
    }

    participants.emplace();
    for (const auto& participant : o().participants) {
        boost::optional<bool> readOnly = boost::none;
        if (participant.second.readOnly != Participant::ReadOnly::kUnset) {
            readOnly = (participant.second.readOnly == Participant::ReadOnly::kReadOnly);
        }

        participants->try_emplace(participant.first, readOnly);
    }

    return participants;
}

bool TransactionRouter::Router::isSafeToRetryStaleErrors(OperationContext* opCtx) {
    return !getAdditionalParticipantsForResponse(opCtx).has_value();
}

bool TransactionRouter::Router::_isRetryableStmtInARetryableInternalTxn(
    const BSONObj& cmdObj) const {
    if (!isInternalSessionForRetryableWrite(_sessionId())) {
        return false;
    } else if (cmdObj.hasField("stmtId") &&
               (cmdObj.getIntField("stmtId") != kUninitializedStmtId)) {
        return true;
    } else if (cmdObj.hasField("stmtIds")) {
        BSONObj stmtIdsObj = cmdObj.getField("stmtIds").Obj();
        return std::any_of(stmtIdsObj.begin(), stmtIdsObj.end(), [](const BSONElement& stmtIdElem) {
            return stmtIdElem.numberInt() != kUninitializedStmtId;
        });
    }
    return false;
}

BSONObj TransactionRouter::Router::attachTxnFieldsIfNeeded(OperationContext* opCtx,
                                                           const ShardId& shardId,
                                                           const BSONObj& cmdObj) {
    if (opCtx->isActiveTransactionParticipant()) {
        uassert(ErrorCodes::IllegalOperation,
                "The participant cannot add a participant shard while executing a "
                "retryable statement in a retryable internal "
                "transaction.",
                !_isRetryableStmtInARetryableInternalTxn(cmdObj));
    }

    RouterTransactionsMetrics::get(opCtx)->incrementTotalRequestsTargeted();
    const bool hasTxnCreatedAnyDatabase = !p().createdDatabases.empty();
    if (auto txnPart = getParticipant(shardId)) {
        LOGV2_DEBUG(22883,
                    4,
                    "Attaching transaction fields to request for existing participant shard",
                    "sessionId"_attr = _sessionId(),
                    "txnNumber"_attr = o().txnNumberAndRetryCounter.getTxnNumber(),
                    "txnRetryCounter"_attr = o().txnNumberAndRetryCounter.getTxnRetryCounter(),
                    "shardId"_attr = shardId,
                    "request"_attr = redact(cmdObj));
        return txnPart->attachTxnFieldsIfNeeded(opCtx,
                                                cmdObj,
                                                false /* isFirstStatementInThisParticipant */,
                                                false /* addingParticipantViaSubRouter */,
                                                hasTxnCreatedAnyDatabase);
    }

    auto txnPart = _createParticipant(opCtx, shardId);
    LOGV2_DEBUG(22884,
                4,
                "Attaching transaction fields to request for new participant shard",
                "sessionId"_attr = _sessionId(),
                "txnNumber"_attr = o().txnNumberAndRetryCounter.getTxnNumber(),
                "txnRetryCounter"_attr = o().txnNumberAndRetryCounter.getTxnRetryCounter(),
                "shardId"_attr = shardId,
                "request"_attr = redact(cmdObj));
    if (!p().isRecoveringCommit) {
        // Don't update participant stats during recovery since the participant list isn't known.
        RouterTransactionsMetrics::get(opCtx)->incrementTotalContactedParticipants();
    }

    return txnPart.attachTxnFieldsIfNeeded(opCtx,
                                           cmdObj,
                                           true /* isFirstStatementInThisParticipant */,
                                           o().subRouter,
                                           hasTxnCreatedAnyDatabase);
}

boost::optional<TransactionRouter::Participant> TransactionRouter::Router::getParticipant(
    const ShardId& shard) {
    const auto iter = o().participants.find(shard.toString());
    if (iter == o().participants.end())
        return boost::none;

    if (auto& participantAtClusterTime =
            iter->second.sharedOptions.atClusterTimeForSnapshotReadConcern) {
        invariant(*participantAtClusterTime == *o().atClusterTimeForSnapshotReadConcern);
    } else if (auto& participantPlacementConflictTime =
                   iter->second.sharedOptions.placementConflictTimeForNonSnapshotReadConcern) {
        invariant(*participantPlacementConflictTime ==
                  *o().placementConflictTimeForNonSnapshotReadConcern);
    }

    return iter->second;
}

TransactionRouter::Participant TransactionRouter::Router::_createParticipant(
    OperationContext* opCtx, const ShardId& shard) {

    auto& os = o();

    // The first participant is chosen as the coordinator. A sub-router does not need to pick a
    // coordinator, as only a parent router will ever attempt to coordinate a commit.
    auto isFirstParticipant = os.participants.empty();
    if (isFirstParticipant && !o().subRouter) {
        invariant(!os.coordinatorId);
        stdx::lock_guard<Client> lk(*opCtx->getClient());
        o(lk).coordinatorId = shard.toString();
    }

    SharedTransactionOptions sharedOptions = {os.txnNumberAndRetryCounter,
                                              os.apiParameters,
                                              os.readConcernArgs,
                                              os.atClusterTimeForSnapshotReadConcern,
                                              os.placementConflictTimeForNonSnapshotReadConcern,
                                              isInternalSessionForRetryableWrite(_sessionId())};

    stdx::lock_guard<Client> lk(*opCtx->getClient());
    auto resultPair = o(lk).participants.try_emplace(
        shard.toString(),
        TransactionRouter::Participant(isFirstParticipant && !o().subRouter,
                                       p().latestStmtId,
                                       Participant::ReadOnly::kUnset,
                                       std::move(sharedOptions)));

    return resultPair.first->second;
}

void TransactionRouter::Router::_updateParticipant(
    OperationContext* opCtx,
    const ShardId& shard,
    const boost::optional<Participant::ReadOnly> readOnly,
    const boost::optional<bool> isSubRouter) {

    stdx::lock_guard<Client> lk(*opCtx->getClient());
    const auto iter = o(lk).participants.find(shard.toString());
    invariant(iter != o().participants.end());
    const auto currentParticipant = iter->second;

    auto updatedReadOnly = currentParticipant.readOnly;
    auto updatedIsSubRouter = currentParticipant.isSubRouter;

    if (readOnly) {
        updatedReadOnly = *readOnly != Participant::ReadOnly::kUnset ? *readOnly : updatedReadOnly;
    }
    if (isSubRouter) {
        updatedIsSubRouter = *isSubRouter ? *isSubRouter : updatedIsSubRouter;
    }

    auto newParticipant =
        TransactionRouter::Participant(currentParticipant.isCoordinator,
                                       currentParticipant.stmtIdCreatedAt,
                                       updatedReadOnly,
                                       std::move(currentParticipant.sharedOptions),
                                       updatedIsSubRouter);

    o(lk).participants.erase(iter);
    o(lk).participants.try_emplace(shard.toString(), std::move(newParticipant));
}

void TransactionRouter::Router::_assertAbortStatusIsOkOrNoSuchTransaction(
    const AsyncRequestsSender::Response& response) const {
    auto shardResponse = uassertStatusOKWithContext(
        std::move(response.swResponse),
        str::stream() << "Failed to send abort to shard " << response.shardId
                      << " between retries of statement " << p().latestStmtId);

    auto status = getStatusFromCommandResult(shardResponse.data);
    uassert(ErrorCodes::NoSuchTransaction,
            str::stream() << txnIdToString() << " Transaction aborted between retries of statement "
                          << p().latestStmtId << " due to error: " << status
                          << " from shard: " << response.shardId,
            status.isOK() || status.code() == ErrorCodes::NoSuchTransaction);

    // abortTransaction is sent with "local" write concern (w: 1), so there's no need to check for a
    // write concern error.
}

std::vector<ShardId> TransactionRouter::Router::_getPendingParticipants() const {
    std::vector<ShardId> pendingParticipants;
    for (const auto& participant : o().participants) {
        if (participant.second.stmtIdCreatedAt == p().latestStmtId) {
            pendingParticipants.emplace_back(ShardId(participant.first));
        }
    }
    return pendingParticipants;
}

void TransactionRouter::Router::_clearPendingParticipants(OperationContext* opCtx,
                                                          boost::optional<Status> optStatus) {
    const auto pendingParticipants = _getPendingParticipants();

    // If there was a stale shard or db routing error and the transaction is retryable then we don't
    // send abort to any participant to prevent a race between the aborts and the commands retried
    if (!o().subRouter && (!optStatus || !_errorAllowsRetryOnStaleShardOrDb(*optStatus))) {
        // Send abort to each pending participant. This resets their transaction state and
        // guarantees no transactions will be left open if the retry does not re-target any of these
        // shards.
        std::vector<AsyncRequestsSender::Request> abortRequests;
        for (const auto& participant : pendingParticipants) {
            abortRequests.emplace_back(participant,
                                       BSON("abortTransaction"
                                            << 1 << WriteConcernOptions::kWriteConcernField
                                            << WriteConcernOptions().toBSON()));
        }
        auto responses = gatherResponses(opCtx,
                                         DatabaseName::kAdmin,
                                         NamespaceString(DatabaseName::kAdmin),
                                         ReadPreferenceSetting{ReadPreference::PrimaryOnly},
                                         Shard::RetryPolicy::kIdempotent,
                                         abortRequests);

        // Verify each abort succeeded or failed with NoSuchTransaction, which may happen if the
        // transaction was already implicitly aborted on the shard.
        for (const auto& response : responses) {
            _assertAbortStatusIsOkOrNoSuchTransaction(response);
        }
    }
    // Remove each aborted participant from the participant list. Remove after sending abort, so
    // they are not added back to the participant list by the transaction tracking inside the ARS.
    for (const auto& participant : pendingParticipants) {
        // If the participant being removed was chosen as the recovery shard, reset the recovery
        // shard. This is safe because this participant is a pending participant, meaning it
        // cannot have been returned in the recoveryToken on an earlier statement.
        if (p().recoveryShardId && *p().recoveryShardId == participant) {
            p().recoveryShardId.reset();
        }

        stdx::lock_guard<Client> lk(*opCtx->getClient());
        invariant(o(lk).participants.erase(participant));
    }

    // If there are no more participants, also clear the coordinator id because a new one must be
    // chosen by the retry.
    if (o().participants.empty()) {
        stdx::lock_guard<Client> lk(*opCtx->getClient());
        o(lk).coordinatorId.reset();
        return;
    }

    if (!o().subRouter) {
        // If participants were created by an earlier command, the coordinator must be one of them.
        invariant(o().coordinatorId);
        invariant(o().participants.count(*o().coordinatorId) == 1);
    } else {
        // A sub-router does not pick a coordinator.
        invariant(!o().coordinatorId);
    }
}

bool TransactionRouter::Router::canContinueOnStaleShardOrDbError(StringData cmdName,
                                                                 const Status& status) const {
    if (MONGO_unlikely(enableStaleVersionAndSnapshotRetriesWithinTransactions.shouldFail())) {
        // We can always retry on the first overall statement because all targeted participants must
        // be pending, so the retry will restart the local transaction on each one, overwriting any
        // effects from the first attempt.
        if (p().latestStmtId == p().firstStmtId) {
            return true;
        }

        // Only idempotent operations can be retried if the error came from a later statement
        // because non-pending participants targeted by the statement may receive the same statement
        // id more than once, and currently statement ids are not tracked by participants so the
        // operation would be applied each time.
        //
        // Note that the retry will fail if any non-pending participants returned a stale version
        // error during the latest statement, because the error will abort their local transactions
        // but the router's retry will expect them to be in-progress.
        if (alwaysRetryableCmds.count(cmdName)) {
            return true;
        }
    }

    return _errorAllowsRetryOnStaleShardOrDb(status);
}

void TransactionRouter::Router::onStaleShardOrDbError(OperationContext* opCtx,
                                                      StringData cmdName,
                                                      const Status& status) {
    invariant(canContinueOnStaleShardOrDbError(cmdName, status));

    LOGV2_DEBUG(22885,
                3,
                "Clearing pending participants after stale version error",
                "sessionId"_attr = _sessionId(),
                "txnNumber"_attr = o().txnNumberAndRetryCounter.getTxnNumber(),
                "txnRetryCounter"_attr = o().txnNumberAndRetryCounter.getTxnRetryCounter(),
                "error"_attr = redact(status));

    // Remove participants created during the current statement so they are sent the correct options
    // if they are targeted again by the retry.
    _clearPendingParticipants(opCtx, status);

    if (p().latestStmtId != p().firstStmtId) {
        return;
    }

    // Reset the global snapshot timestamp so the retry will select a new one.
    stdx::lock_guard<Client> lk(*opCtx->getClient());
    if (o(lk).atClusterTimeForSnapshotReadConcern) {
        o(lk).atClusterTimeForSnapshotReadConcern.emplace();
    } else {
        o(lk).placementConflictTimeForNonSnapshotReadConcern.emplace();
    }
}

void TransactionRouter::Router::onViewResolutionError(OperationContext* opCtx,
                                                      const NamespaceString& nss) {
    // A parent router can always retry on a view resolution error.
    LOGV2_DEBUG(22886,
                3,
                "Clearing pending participants after view resolution error",
                "sessionId"_attr = _sessionId(),
                "txnNumber"_attr = o().txnNumberAndRetryCounter.getTxnNumber(),
                "txnRetryCounter"_attr = o().txnNumberAndRetryCounter.getTxnRetryCounter(),
                logAttrs(nss));

    // Requests against views are always routed to the primary shard for its database, but the retry
    // on the resolved namespace does not have to re-target the primary, so pending participants
    // should be cleared.
    _clearPendingParticipants(opCtx, boost::none);
}

bool TransactionRouter::Router::canContinueOnSnapshotError() const {
    if (MONGO_unlikely(enableStaleVersionAndSnapshotRetriesWithinTransactions.shouldFail())) {
        return (o().atClusterTimeForSnapshotReadConcern &&
                ((*o().atClusterTimeForSnapshotReadConcern == LogicalTime::kUninitialized) ||
                 p().latestStmtId == p().firstStmtId)) ||
            (o().placementConflictTimeForNonSnapshotReadConcern &&
             ((*o().placementConflictTimeForNonSnapshotReadConcern ==
               LogicalTime::kUninitialized) ||
              p().latestStmtId == p().firstStmtId));
    }

    return false;
}

void TransactionRouter::Router::onSnapshotError(OperationContext* opCtx, const Status& status) {
    invariant(canContinueOnSnapshotError());

    LOGV2_DEBUG(22887,
                3,
                "Clearing pending participants and resetting global snapshot timestamp after "
                "snapshot error",
                "sessionId"_attr = _sessionId(),
                "txnNumber"_attr = o().txnNumberAndRetryCounter.getTxnNumber(),
                "txnRetryCounter"_attr = o().txnNumberAndRetryCounter.getTxnRetryCounter(),
                "error"_attr = redact(status),
                "previousGlobalSnapshotTimestamp"_attr =
                    (o().atClusterTimeForSnapshotReadConcern
                         ? *o().atClusterTimeForSnapshotReadConcern
                         : *o().placementConflictTimeForNonSnapshotReadConcern));

    // The transaction must be restarted on all participants because a new read timestamp will be
    // selected, so clear all pending participants. Snapshot errors are only retryable on the first
    // client statement, so all participants should be cleared, including the coordinator.
    _clearPendingParticipants(opCtx, status);
    invariant(o().participants.empty());
    invariant(!o().coordinatorId);

    // Reset the global snapshot timestamp so the retry will select a new one.
    stdx::lock_guard<Client> lk(*opCtx->getClient());
    if (o(lk).atClusterTimeForSnapshotReadConcern) {
        o(lk).atClusterTimeForSnapshotReadConcern.emplace();
    } else {
        o(lk).placementConflictTimeForNonSnapshotReadConcern.emplace();
    }
}

void TransactionRouter::Router::setAtClusterTimeForStartOrContinue(OperationContext* opCtx) {
    if (o().atClusterTimeForSnapshotReadConcern) {
        if (*o().atClusterTimeForSnapshotReadConcern == LogicalTime::kUninitialized) {
            stdx::lock_guard<Client> lk(*opCtx->getClient());
            auto candidateTime = repl::ReadConcernArgs::get(opCtx).getArgsAtClusterTime();
            uassert(
                8676400, "Missing atClusterTime in readConcernArgs", candidateTime != boost::none);
            setAtClusterTime(_sessionId(),
                             o(lk).txnNumberAndRetryCounter,
                             p().latestStmtId,
                             o(lk).atClusterTimeForSnapshotReadConcern.get_ptr(),
                             repl::ReadConcernArgs::get(opCtx),
                             candidateTime.value());
        }
    } else if (o().placementConflictTimeForNonSnapshotReadConcern) {
        if (*o().placementConflictTimeForNonSnapshotReadConcern == LogicalTime::kUninitialized) {
            stdx::lock_guard<Client> lk(*opCtx->getClient());
            auto candidateTime = repl::ReadConcernArgs::get(opCtx).getArgsAfterClusterTime();
            uassert(8676401,
                    "Missing afterClusterTime in readConcernArgs",
                    candidateTime != boost::none);
            setAtClusterTime(_sessionId(),
                             o(lk).txnNumberAndRetryCounter,
                             p().latestStmtId,
                             o(lk).placementConflictTimeForNonSnapshotReadConcern.get_ptr(),
                             repl::ReadConcernArgs::get(opCtx),
                             candidateTime.value());
        }
    }
}

void TransactionRouter::Router::setDefaultAtClusterTime(OperationContext* opCtx) {
    const auto defaultTime = VectorClock::get(opCtx)->getTime();

    if (o().atClusterTimeForSnapshotReadConcern) {
        if (*o().atClusterTimeForSnapshotReadConcern == LogicalTime::kUninitialized) {
            stdx::lock_guard<Client> lk(*opCtx->getClient());
            setAtClusterTime(_sessionId(),
                             o(lk).txnNumberAndRetryCounter,
                             p().latestStmtId,
                             o(lk).atClusterTimeForSnapshotReadConcern.get_ptr(),
                             repl::ReadConcernArgs::get(opCtx),
                             defaultTime.clusterTime());
        }
    } else if (o().placementConflictTimeForNonSnapshotReadConcern) {
        if (*o().placementConflictTimeForNonSnapshotReadConcern == LogicalTime::kUninitialized) {
            stdx::lock_guard<Client> lk(*opCtx->getClient());

            setAtClusterTime(_sessionId(),
                             o(lk).txnNumberAndRetryCounter,
                             p().latestStmtId,
                             o(lk).placementConflictTimeForNonSnapshotReadConcern.get_ptr(),
                             repl::ReadConcernArgs::get(opCtx),
                             defaultTime.clusterTime());
        }
    }
}

void TransactionRouter::Router::_continueTxn(OperationContext* opCtx,
                                             TxnNumberAndRetryCounter txnNumberAndRetryCounter,
                                             TransactionActions action) {
    invariant(txnNumberAndRetryCounter.getTxnNumber() ==
              o().txnNumberAndRetryCounter.getTxnNumber());
    switch (action) {
        case TransactionActions::kStart: {
            uassert(ErrorCodes::ConflictingOperationInProgress,
                    str::stream() << "txnNumber " << o().txnNumberAndRetryCounter.getTxnNumber()
                                  << " txnRetryCounter "
                                  << o().txnNumberAndRetryCounter.getTxnRetryCounter()
                                  << " for session " << _sessionId() << " already started",
                    isInternalSessionForRetryableWrite(_sessionId()));
            break;
        }
        case TransactionActions::kStartOrContinue:
            // Check that the readConcern matches what is set in the router.
            uassert(ErrorCodes::InvalidOptions,
                    "ReadConcern must match previously-set value on the router.",
                    repl::ReadConcernArgs::get(opCtx).getLevel() == o().readConcernArgs.getLevel());

            // If the participants list is empty on, it is safe to assume the transaction was
            // retried. Reset the state, including the clusterTime for the transaction, in case this
            // is a retry coming from the parent router where the parent router picked a new
            // clusterTime for the transaction.
            if (o().participants.empty()) {
                invariant(opCtx->isActiveTransactionParticipant());
                tassert(8980602,
                        "Transaction sub-router tried to continue a transaction without any "
                        "tracked participants, but had advanced its latestStmtId",
                        p().latestStmtId == p().firstStmtId);
                _resetRouterStateForStartOrContinueTransaction(opCtx, txnNumberAndRetryCounter);
            }

            FMT_FALLTHROUGH;
        case TransactionActions::kContinue: {
            // When a participant shard calls with action kStartOrContinue, the readConcern
            // should already be set in the opCtx, so skip this assert and assignment.
            if (action != TransactionActions::kStartOrContinue) {
                uassert(ErrorCodes::InvalidOptions,
                        "Only the first command in a transaction may specify a readConcern",
                        repl::ReadConcernArgs::get(opCtx).isEmpty());

                APIParameters::get(opCtx) = o().apiParameters;
                repl::ReadConcernArgs::get(opCtx) = o().readConcernArgs;
            }

            // Don't increment latestStmtId if no shards have been targeted, since that implies no
            // statements would have been executed inside this transaction at this point. This can
            // occur when an internal transaction is invoked within a client's transaction that
            // hasn't executed any statements yet.
            if (!o().participants.empty()) {
                ++p().latestStmtId;
            }

            uassert(
                8027900,
                str::stream() << "attempting to continue transaction that was not started lsid: "
                              << _sessionId()
                              << " txnNumber: " << o().txnNumberAndRetryCounter.getTxnNumber(),
                o().atClusterTimeForSnapshotReadConcern ||
                    o().placementConflictTimeForNonSnapshotReadConcern);

            _onContinue(opCtx);
            break;
        }
        case TransactionActions::kCommit:
            ++p().latestStmtId;
            _onContinue(opCtx);
            break;
    }
}

void TransactionRouter::Router::_beginTxn(OperationContext* opCtx,
                                          TxnNumberAndRetryCounter txnNumberAndRetryCounter,
                                          TransactionActions action) {
    invariant(txnNumberAndRetryCounter.getTxnNumber() >
              o().txnNumberAndRetryCounter.getTxnNumber());

    switch (action) {
        case TransactionActions::kStartOrContinue: {
            _resetRouterStateForStartOrContinueTransaction(opCtx, txnNumberAndRetryCounter);
            break;
        }
        case TransactionActions::kStart: {
            _resetRouterStateForStartTransaction(opCtx, txnNumberAndRetryCounter);
            break;
        }
        case TransactionActions::kContinue: {
            uasserted(ErrorCodes::NoSuchTransaction,
                      str::stream()
                          << "cannot continue txnId " << o().txnNumberAndRetryCounter.getTxnNumber()
                          << " for session " << _sessionId() << " with txnRetryCounter "
                          << txnNumberAndRetryCounter.getTxnRetryCounter());
        }
        case TransactionActions::kCommit: {
            _resetRouterState(opCtx, txnNumberAndRetryCounter);
            // If the first action seen by the router for this transaction is to commit, that
            // means that the client is attempting to recover a commit decision.
            p().isRecoveringCommit = true;

            LOGV2_DEBUG(22890,
                        3,
                        "Commit recovery started",
                        "sessionId"_attr = _sessionId(),
                        "txnNumber"_attr = o().txnNumberAndRetryCounter.getTxnNumber(),
                        "txnRetryCounter"_attr = o().txnNumberAndRetryCounter.getTxnRetryCounter());

            break;
        }
    };
}

void TransactionRouter::Router::beginOrContinueTxn(OperationContext* opCtx,
                                                   TxnNumber txnNumber,
                                                   TransactionActions action) {
    const TxnNumberAndRetryCounter txnNumberAndRetryCounter{txnNumber, 0};

    if (txnNumberAndRetryCounter.getTxnNumber() < o().txnNumberAndRetryCounter.getTxnNumber()) {
        // This transaction is older than the transaction currently in progress, so throw an error.
        uasserted(ErrorCodes::TransactionTooOld,
                  str::stream() << "txnNumber " << txnNumberAndRetryCounter.getTxnNumber()
                                << " is less than last txnNumber "
                                << o().txnNumberAndRetryCounter.getTxnNumber()
                                << " seen in session " << _sessionId());
    } else if (txnNumberAndRetryCounter.getTxnNumber() ==
               o().txnNumberAndRetryCounter.getTxnNumber()) {
        // This is the same transaction as the one in progress.
        auto apiParamsFromClient = APIParameters::get(opCtx);
        if (action == TransactionActions::kStartOrContinue ||
            action == TransactionActions::kContinue || action == TransactionActions::kCommit) {
            uassert(
                ErrorCodes::APIMismatchError,
                fmt::format("API parameter mismatch: transaction-continuing command used {}, the "
                            "transaction's first command used {}",
                            apiParamsFromClient.toBSON().toString(),
                            o().apiParameters.toBSON().toString()),
                apiParamsFromClient == o().apiParameters);
        }
        _continueTxn(opCtx, txnNumberAndRetryCounter, action);
    } else {
        // This is a newer transaction
        uassert(ErrorCodes::InterruptedAtShutdown,
                "New transaction cannot be started at shutdown.",
                !SessionCatalog::get(opCtx)->getDisallowNewTransactions());

        _beginTxn(opCtx, txnNumberAndRetryCounter, action);
    }

    _updateLastClientInfo(opCtx->getClient());
}

void TransactionRouter::Router::stash(OperationContext* opCtx, StashReason reason) {
    if (!isInitialized()) {
        return;
    }

    stdx::lock_guard<Client> lk(*opCtx->getClient());

    if (reason == StashReason::kYield) {
        ++o(lk).activeYields;
    }

    auto tickSource = opCtx->getServiceContext()->getTickSource();
    o(lk).metricsTracker->trySetInactive(tickSource, tickSource->getTicks());
}

void TransactionRouter::Router::unstash(OperationContext* opCtx) {
    if (!isInitialized()) {
        return;
    }

    // Validate that the transaction number hasn't changed while we were yielded. This is guaranteed
    // by the activeYields check when beginning a new transaction.
    invariant(opCtx->getTxnNumber(), "Cannot unstash without a transaction number");
    invariant(o().txnNumberAndRetryCounter.getTxnNumber() == opCtx->getTxnNumber(),
              str::stream()
                  << "The requested operation has a different transaction number than the active "
                     "transaction. Active: "
                  << o().txnNumberAndRetryCounter.getTxnNumber()
                  << ", operation: " << *opCtx->getTxnNumber());

    {
        stdx::lock_guard<Client> lg(*opCtx->getClient());
        --o(lg).activeYields;
        invariant(o(lg).activeYields >= 0,
                  str::stream() << "Invalid activeYields: " << o(lg).activeYields);
    }

    auto tickSource = opCtx->getServiceContext()->getTickSource();
    stdx::lock_guard<Client> lk(*opCtx->getClient());
    o(lk).metricsTracker->trySetActive(tickSource, tickSource->getTicks());
}

BSONObj TransactionRouter::Router::_handOffCommitToCoordinator(OperationContext* opCtx) {
    invariant(o().coordinatorId);
    auto coordinatorIter = o().participants.find(*o().coordinatorId);
    invariant(coordinatorIter != o().participants.end());

    std::vector<CommitParticipant> participantList;
    for (const auto& participantEntry : o().participants) {
        CommitParticipant commitParticipant;
        commitParticipant.setShardId(participantEntry.first);
        participantList.push_back(std::move(commitParticipant));
    }

    CoordinateCommitTransaction coordinateCommitCmd;
    coordinateCommitCmd.setDbName(DatabaseName::kAdmin);
    coordinateCommitCmd.setParticipants(participantList);
    coordinateCommitCmd.setWriteConcern(opCtx->getWriteConcern());
    const auto coordinateCommitCmdObj = coordinateCommitCmd.toBSON();

    LOGV2_DEBUG(22891,
                3,
                "Committing using two-phase commit",
                "sessionId"_attr = _sessionId(),
                "txnNumber"_attr = o().txnNumberAndRetryCounter.getTxnNumber(),
                "txnRetryCounter"_attr = o().txnNumberAndRetryCounter.getTxnRetryCounter(),
                "coordinatorShardId"_attr = *o().coordinatorId);

    MultiStatementTransactionRequestsSender ars(
        opCtx,
        Grid::get(opCtx)->getExecutorPool()->getFixedExecutor(),
        DatabaseName::kAdmin,
        {{*o().coordinatorId, coordinateCommitCmdObj}},
        ReadPreferenceSetting{ReadPreference::PrimaryOnly},
        Shard::RetryPolicy::kIdempotent);

    auto response = ars.next();
    invariant(ars.done());
    uassertStatusOK(response.swResponse);

    return response.swResponse.getValue().data;
}

BSONObj TransactionRouter::Router::commitTransaction(
    OperationContext* opCtx, const boost::optional<TxnRecoveryToken>& recoveryToken) {
    invariant(isInitialized());
    uassert(ErrorCodes::IllegalOperation,
            "Transaction sub-router on shard cannot execute commit.",
            !o().subRouter);

    const auto isFirstCommitAttempt = !p().terminationInitiated;
    p().terminationInitiated = true;
    auto commitRes = _commitTransaction(opCtx, recoveryToken, isFirstCommitAttempt);

    auto commitStatus = getStatusFromCommandResult(commitRes);
    auto commitWCStatus = getWriteConcernStatusFromCommandResult(commitRes);

    if (isCommitResultUnknown(commitStatus, commitWCStatus)) {
        // Don't update stats if we don't know the result of the transaction. The client may choose
        // to retry commit, which will update stats if the result is determined.
        //
        // Note that we also don't end the transaction if _commitTransaction() throws, which it
        // should only do on failure to send a request, in which case the commit result is unknown.
        return commitRes;
    }

    if (commitStatus.isOK()) {
        _onSuccessfulCommit(opCtx);
    } else {
        // Note that write concern errors are never considered a fatal commit error because they
        // should be retryable, so it is fine to only pass the top-level status.
        _onNonRetryableCommitError(opCtx, commitStatus);
    }

    return commitRes;
}

BSONObj TransactionRouter::Router::_commitTransaction(
    OperationContext* opCtx,
    const boost::optional<TxnRecoveryToken>& recoveryToken,
    bool isFirstCommitAttempt) {
    if (p().isRecoveringCommit) {
        uassert(50940,
                "Cannot recover the transaction decision without a recoveryToken",
                recoveryToken);
        {
            stdx::lock_guard<Client> lk(*opCtx->getClient());
            o(lk).commitType = CommitType::kRecoverWithToken;
            _onStartCommit(lk, opCtx);
        }

        return _commitWithRecoveryToken(opCtx, *recoveryToken);
    }

    if (o().participants.empty()) {
        // The participants list can be empty if a transaction was began on mongos, but it never
        // ended up targeting any hosts. Such cases are legal for example if a find is issued
        // against a non-existent database.
        uassert(ErrorCodes::IllegalOperation,
                "Cannot commit without participants",
                o().txnNumberAndRetryCounter.getTxnNumber() != kUninitializedTxnNumber);
        {
            stdx::lock_guard<Client> lk(*opCtx->getClient());
            o(lk).commitType = CommitType::kNoShards;
            _onStartCommit(lk, opCtx);
        }

        return BSON("ok" << 1);
    }

    std::vector<ShardId> readOnlyShards;
    std::vector<ShardId> writeShards;
    for (const auto& participant : o().participants) {
        switch (participant.second.readOnly) {
            case Participant::ReadOnly::kUnset:
                uasserted(ErrorCodes::NoSuchTransaction,
                          str::stream()
                              << txnIdToString() << " Failed to commit transaction "
                              << "because a previous statement on the transaction "
                              << "participant " << participant.first << " was unsuccessful.");
            case Participant::ReadOnly::kReadOnly:
                readOnlyShards.push_back(participant.first);
                break;
            case Participant::ReadOnly::kNotReadOnly:
                writeShards.push_back(participant.first);
                break;
        }
    }

    if (o().participants.size() == 1) {
        ShardId shardId = o().participants.cbegin()->first;
        LOGV2_DEBUG(22892,
                    3,
                    "Committing single-shard transaction",
                    "sessionId"_attr = _sessionId(),
                    "txnNumber"_attr = o().txnNumberAndRetryCounter.getTxnNumber(),
                    "txnRetryCounter"_attr = o().txnNumberAndRetryCounter.getTxnRetryCounter(),
                    "shardId"_attr = shardId);

        {
            stdx::lock_guard<Client> lk(*opCtx->getClient());
            o(lk).commitType = CommitType::kSingleShard;
            _onStartCommit(lk, opCtx);
        }


        return sendCommitDirectlyToShards(opCtx, {shardId});
    }

    if (writeShards.size() == 1 && !p().disallowSingleWriteShardCommit) {
        LOGV2_DEBUG(22894,
                    3,
                    "Committing single-write-shard transaction",
                    "sessionId"_attr = _sessionId().getId(),
                    "txnNumber"_attr = o().txnNumberAndRetryCounter.getTxnNumber(),
                    "numReadOnlyShards"_attr = readOnlyShards.size(),
                    "writeShardId"_attr = writeShards.front());
        {
            stdx::lock_guard<Client> lk(*opCtx->getClient());
            o(lk).commitType = CommitType::kSingleWriteShard;
            _onStartCommit(lk, opCtx);
        }

        if (!isFirstCommitAttempt) {
            // For a retried single write shard commit, fall back to the recovery token protocol to
            // guarantee returning the correct outcome. The client should have provided a recovery
            // token, but it isn't necessary since the write shard must be the recovery shard, so we
            // can use a synthetic token instead.
            tassert(4834000, "Expected to have a recovery shard", p().recoveryShardId);
            tassert(4834001,
                    "Expected recovery shard to equal the single write shard",
                    p().recoveryShardId == writeShards[0]);

            TxnRecoveryToken syntheticRecoveryToken;
            syntheticRecoveryToken.setRecoveryShardId(writeShards[0]);

            return _commitWithRecoveryToken(opCtx, syntheticRecoveryToken);
        }

        auto readOnlyShardsResponse = sendCommitDirectlyToShards(opCtx, readOnlyShards);

        auto readOnlyCmdStatus = getStatusFromCommandResult(readOnlyShardsResponse);
        auto readOnlyWCE = getWriteConcernStatusFromCommandResult(readOnlyShardsResponse);
        if (!readOnlyCmdStatus.isOK()) {
            return readOnlyShardsResponse;
        } else if (!readOnlyWCE.isOK()) {
            // Rethrow the write concern error as a command error since the transaction's effects
            // can't be durable as we haven't started commit on the write shard.
            uassertStatusOK(readOnlyWCE);
        }
        return sendCommitDirectlyToShards(opCtx, writeShards);
    }

    if (writeShards.size() == 0) {
        LOGV2_DEBUG(22893,
                    3,
                    "Committing read-only transaction",
                    "sessionId"_attr = _sessionId(),
                    "txnNumber"_attr = o().txnNumberAndRetryCounter.getTxnNumber(),
                    "txnRetryCounter"_attr = o().txnNumberAndRetryCounter.getTxnRetryCounter(),
                    "numParticipantShards"_attr = readOnlyShards.size());
        {
            stdx::lock_guard<Client> lk(*opCtx->getClient());
            o(lk).commitType = CommitType::kReadOnly;
            _onStartCommit(lk, opCtx);
        }

        return sendCommitDirectlyToShards(opCtx, readOnlyShards);
    }

    {
        stdx::lock_guard<Client> lk(*opCtx->getClient());
        o(lk).commitType = CommitType::kTwoPhaseCommit;
        _onStartCommit(lk, opCtx);
    }

    return _handOffCommitToCoordinator(opCtx);
}

// Returns if the opCtx has yielded its session and failed to unyield it, which may happen during
// methods that send network requests at global shutdown when running on a mongod.
bool failedToUnyieldSessionAtShutdown(OperationContext* opCtx) {
    if (!TransactionRouter::get(opCtx)) {
        invariant(globalInShutdownDeprecated());
        return true;
    }
    return false;
}

BSONObj TransactionRouter::Router::abortTransaction(OperationContext* opCtx) {
    invariant(isInitialized());
    uassert(ErrorCodes::IllegalOperation,
            "Transaction sub-router on shard cannot execute abort.",
            !o().subRouter);

    // Update stats on scope exit so the transaction is considered "active" while waiting on abort
    // responses.
    ScopeGuard updateStatsGuard([&] {
        if (failedToUnyieldSessionAtShutdown(opCtx)) {
            // It's unsafe to continue without the session checked out. This should only happen at
            // global shutdown, so it's acceptable to skip updating stats.
            return;
        }
        _onExplicitAbort(opCtx);
    });

    // The router has yet to send any commands to a remote shard for this transaction.
    // Return the same error that would have been returned by a shard.
    uassert(ErrorCodes::NoSuchTransaction,
            "no known command has been sent by this router for this transaction",
            !o().participants.empty());

    p().terminationInitiated = true;

    auto abortCmd = BSON("abortTransaction" << 1 << WriteConcernOptions::kWriteConcernField
                                            << opCtx->getWriteConcern().toBSON());
    std::vector<AsyncRequestsSender::Request> abortRequests;
    for (const auto& participantEntry : o().participants) {
        abortRequests.emplace_back(ShardId(participantEntry.first), abortCmd);
    }

    LOGV2_DEBUG(22895,
                3,
                "Aborting transaction on all participant shards",
                "sessionId"_attr = _sessionId(),
                "txnNumber"_attr = o().txnNumberAndRetryCounter.getTxnNumber(),
                "txnRetryCounter"_attr = o().txnNumberAndRetryCounter.getTxnRetryCounter(),
                "numParticipantShards"_attr = o().participants.size());

    const auto responses = gatherResponses(opCtx,
                                           DatabaseName::kAdmin,
                                           NamespaceString(DatabaseName::kAdmin),
                                           ReadPreferenceSetting{ReadPreference::PrimaryOnly},
                                           Shard::RetryPolicy::kIdempotent,
                                           abortRequests);

    // TODO (SERVER-104090): Replace the following with appendRawResponses when that helper has been
    // fixed to properly return WCE.
    BSONObj lastResult;
    boost::optional<BSONObj> errorResult;
    boost::optional<BSONObj> wceResult;
    for (const auto& response : responses) {
        uassertStatusOK(response.swResponse);

        lastResult = response.swResponse.getValue().data;

        // If any shard returned an error, store that error to be returned. We do not return
        // immediately so we do not accidentally hide a possible writeConcernError that a different
        // participant could return.
        const auto commandStatus = getStatusFromCommandResult(lastResult);
        if (!commandStatus.isOK() && !errorResult) {
            errorResult.emplace(lastResult);
        }

        // If any participant had a writeConcern error, store it to be returned. If a participant
        // had both a normal error and a writeConcern error return that immediately.
        const auto writeConcernStatus = getWriteConcernStatusFromCommandResult(lastResult);
        if (!writeConcernStatus.isOK() && !wceResult) {
            wceResult.emplace(lastResult);
            if (!commandStatus.isOK()) {
                return lastResult;
            }
        }
    }

    if (errorResult) {
        return errorResult.get();
    }

    if (wceResult) {
        return wceResult.get();
    }

    // If all the responses were ok, return the last response.
    return lastResult;
}

void TransactionRouter::Router::implicitlyAbortTransaction(OperationContext* opCtx,
                                                           const Status& status) {
    if (!isInitialized()) {
        // If the transaction hasn't even started then there is nothing to implicitly abort. This
        // can occur in some cases where the transaction hasn't been initialized as it tried to
        // start during a shutdown.
        return;
    }
    uassert(ErrorCodes::IllegalOperation,
            "Transaction sub-router on shard cannot execute implicit abort.",
            !o().subRouter);

    if (o().commitType == CommitType::kTwoPhaseCommit ||
        o().commitType == CommitType::kRecoverWithToken) {
        LOGV2_DEBUG(
            22896,
            3,
            "Not sending implicit abortTransaction to participant shards after error because "
            "coordinating the commit decision may have been handed off to the coordinator shard",
            "sessionId"_attr = _sessionId(),
            "txnNumber"_attr = o().txnNumberAndRetryCounter.getTxnNumber(),
            "txnRetryCounter"_attr = o().txnNumberAndRetryCounter.getTxnRetryCounter(),
            "error"_attr = redact(status));
        return;
    }

    // Update stats on scope exit so the transaction is considered "active" while waiting on abort
    // responses.
    ScopeGuard updateStatsGuard([&] {
        if (failedToUnyieldSessionAtShutdown(opCtx)) {
            // It's unsafe to continue without the session checked out. This should only happen at
            // global shutdown, so it's acceptable to skip updating stats.
            return;
        }
        _onImplicitAbort(opCtx, status);
    });

    if (o().participants.empty()) {
        return;
    }

    p().terminationInitiated = true;

    auto abortCmd = BSON("abortTransaction" << 1 << WriteConcernOptions::kWriteConcernField
                                            << WriteConcernOptions().toBSON());
    std::vector<AsyncRequestsSender::Request> abortRequests;
    for (const auto& participantEntry : o().participants) {
        abortRequests.emplace_back(ShardId(participantEntry.first), abortCmd);
    }

    LOGV2_DEBUG(22897,
                3,
                "Implicitly aborting transaction on all participant shards",
                "sessionId"_attr = _sessionId(),
                "txnNumber"_attr = o().txnNumberAndRetryCounter.getTxnNumber(),
                "txnRetryCounter"_attr = o().txnNumberAndRetryCounter.getTxnRetryCounter(),
                "numParticipantShards"_attr = o().participants.size(),
                "error"_attr = redact(status));

    try {
        // Ignore the responses.
        gatherResponses(opCtx,
                        DatabaseName::kAdmin,
                        NamespaceString(DatabaseName::kAdmin),
                        ReadPreferenceSetting{ReadPreference::PrimaryOnly},
                        Shard::RetryPolicy::kIdempotent,
                        abortRequests);
    } catch (const DBException& ex) {
        LOGV2_DEBUG(22898,
                    3,
                    "Implicitly aborting transaction failed",
                    "sessionId"_attr = _sessionId(),
                    "txnNumber"_attr = o().txnNumberAndRetryCounter.getTxnNumber(),
                    "txnRetryCounter"_attr = o().txnNumberAndRetryCounter.getTxnRetryCounter(),
                    "error"_attr = ex);
        // Ignore any exceptions.
    }
}

std::string TransactionRouter::Router::txnIdToString() const {
    return str::stream() << _sessionId() << ":" << o().txnNumberAndRetryCounter.getTxnNumber();
}

void TransactionRouter::Router::appendRecoveryToken(BSONObjBuilder* builder) const {
    BSONObjBuilder recoveryTokenBuilder(
        builder->subobjStart(CommitTransaction::kRecoveryTokenFieldName));
    TxnRecoveryToken recoveryToken;

    // The recovery shard is chosen on the first statement that did a write (transactions that only
    // did reads do not need to be recovered; they can just be retried) or that returned an
    // additional participant with an empty readOnly value.
    if (p().recoveryShardId) {
        auto recoveryShardReadOnly = o().participants.find(*p().recoveryShardId)->second.readOnly;
        invariant(recoveryShardReadOnly == Participant::ReadOnly::kNotReadOnly);
        recoveryToken.setRecoveryShardId(*p().recoveryShardId);
    }

    recoveryToken.serialize(&recoveryTokenBuilder);
    recoveryTokenBuilder.doneFast();
}

void TransactionRouter::Router::_resetRouterState(
    OperationContext* opCtx, const TxnNumberAndRetryCounter& txnNumberAndRetryCounter) {
    {
        stdx::lock_guard<Client> lk(*opCtx->getClient());

        uassert(ErrorCodes::ConflictingOperationInProgress,
                "Cannot start a new transaction while the previous is yielded",
                o(lk).activeYields == 0);

        o(lk).txnNumberAndRetryCounter.setTxnNumber(txnNumberAndRetryCounter.getTxnNumber());
        o(lk).txnNumberAndRetryCounter.setTxnRetryCounter(
            *txnNumberAndRetryCounter.getTxnRetryCounter());
        o(lk).commitType = CommitType::kNotInitiated;
        p().isRecoveringCommit = false;
        o(lk).participants.clear();
        o(lk).coordinatorId.reset();
        p().recoveryShardId.reset();
        o(lk).apiParameters = {};
        o(lk).readConcernArgs = {};
        o(lk).atClusterTimeForSnapshotReadConcern.reset();
        o(lk).placementConflictTimeForNonSnapshotReadConcern.reset();
        o(lk).abortCause = std::string();
        o(lk).metricsTracker.emplace(opCtx->getServiceContext());
        p().terminationInitiated = false;
        p().createdDatabases.clear();
        p().disallowSingleWriteShardCommit = false;

        auto tickSource = opCtx->getServiceContext()->getTickSource();
        o(lk).metricsTracker->trySetActive(tickSource, tickSource->getTicks());

        // TODO SERVER-37115: Parse statement ids from the client and remember the statement id
        // of the command that started the transaction, if one was included.
        p().latestStmtId = kDefaultFirstStmtId;
        p().firstStmtId = kDefaultFirstStmtId;
    }

    OperationContextSession::observeNewTxnNumberStarted(
        opCtx,
        _sessionId(),
        {txnNumberAndRetryCounter.getTxnNumber(), SessionCatalog::Provenance::kRouter});
};

void TransactionRouter::Router::_resetRouterStateForStartTransaction(
    OperationContext* opCtx, const TxnNumberAndRetryCounter& txnNumberAndRetryCounter) {
    auto& readConcernArgs = repl::ReadConcernArgs::get(opCtx);
    uassert(ErrorCodes::InvalidOptions,
            "The first command in a transaction cannot specify a readConcern level "
            "other than local, majority, or snapshot",
            !readConcernArgs.hasLevel() ||
                isReadConcernLevelAllowedInTransaction(readConcernArgs.getLevel()));

    _resetRouterState(opCtx, txnNumberAndRetryCounter);

    {
        stdx::lock_guard<Client> lk(*opCtx->getClient());
        auto& osw = o(lk);

        osw.apiParameters = APIParameters::get(opCtx);
        osw.readConcernArgs = readConcernArgs;

        if (osw.readConcernArgs.getLevel() == repl::ReadConcernLevel::kSnapshotReadConcern) {
            osw.atClusterTimeForSnapshotReadConcern.emplace();
        } else {
            osw.placementConflictTimeForNonSnapshotReadConcern.emplace();
        }
    }

    LOGV2_DEBUG(22889,
                3,
                "New transaction started",
                "sessionId"_attr = _sessionId(),
                "txnNumber"_attr = o().txnNumberAndRetryCounter.getTxnNumber(),
                "txnRetryCounter"_attr = txnNumberAndRetryCounter.getTxnRetryCounter());
};

void TransactionRouter::Router::_resetRouterStateForStartOrContinueTransaction(
    OperationContext* opCtx, const TxnNumberAndRetryCounter& txnNumberAndRetryCounter) {
    if (repl::ReadConcernArgs::get(opCtx).isEmpty()) {
        uasserted(
            ErrorCodes::IllegalOperation,
            str::stream() << "readConcern must be present when beginning a transaction with a "
                             "sub-router");
    }
    _resetRouterStateForStartTransaction(opCtx, txnNumberAndRetryCounter);
    {
        stdx::lock_guard<Client> lk(*opCtx->getClient());
        o(lk).subRouter = true;
    }
    setAtClusterTimeForStartOrContinue(opCtx);
}

BSONObj TransactionRouter::Router::_commitWithRecoveryToken(OperationContext* opCtx,
                                                            const TxnRecoveryToken& recoveryToken) {
    uassert(ErrorCodes::NoSuchTransaction,
            "Recovery token is empty, meaning the transaction only performed reads and can be "
            "safely retried",
            recoveryToken.getRecoveryShardId());
    const auto& recoveryShardId = *recoveryToken.getRecoveryShardId();

    auto coordinateCommitCmd = [&] {
        CoordinateCommitTransaction coordinateCommitCmd;
        coordinateCommitCmd.setDbName(DatabaseName::kAdmin);
        coordinateCommitCmd.setParticipants({});
        coordinateCommitCmd.setWriteConcern(opCtx->getWriteConcern());

        return coordinateCommitCmd.toBSON();
    }();

    MultiStatementTransactionRequestsSender ars(
        opCtx,
        Grid::get(opCtx)->getExecutorPool()->getFixedExecutor(),
        DatabaseName::kAdmin,
        {{recoveryShardId, coordinateCommitCmd}},
        ReadPreferenceSetting{ReadPreference::PrimaryOnly},
        Shard::RetryPolicy::kIdempotent);

    const auto response = ars.next();
    invariant(ars.done());
    uassertStatusOK(response.swResponse);
    return response.swResponse.getValue().data;
}

void TransactionRouter::Router::_logSlowTransaction(OperationContext* opCtx,
                                                    TerminationCause terminationCause) const {

    logv2::DynamicAttributes attrs;
    BSONObjBuilder parametersBuilder;

    BSONObjBuilder lsidBuilder(parametersBuilder.subobjStart("lsid"));
    _sessionId().serialize(&lsidBuilder);
    lsidBuilder.doneFast();

    parametersBuilder.append("txnNumber", o().txnNumberAndRetryCounter.getTxnNumber());
    parametersBuilder.append("txnRetryCounter", *o().txnNumberAndRetryCounter.getTxnRetryCounter());
    parametersBuilder.append("autocommit", false);

    o().apiParameters.appendInfo(&parametersBuilder);
    if (!o().readConcernArgs.isEmpty()) {
        o().readConcernArgs.appendInfo(&parametersBuilder);
    }

    attrs.add("parameters", parametersBuilder.obj());

    // Since DynamicAttributes (attrs) binds by reference, it is important that the lifetime of this
    // variable lasts until the LOGV2 call at the end of this function.
    std::string globalReadTimestampTemp;
    if (_atClusterTimeHasBeenSet()) {
        globalReadTimestampTemp = o().atClusterTimeForSnapshotReadConcern->toString();
        attrs.add("globalReadTimestamp", globalReadTimestampTemp);
    }

    if (o().commitType != CommitType::kRecoverWithToken) {
        // We don't know the participants if we're recovering the commit.
        attrs.add("numParticipants", o().participants.size());
    }

    if (o().commitType == CommitType::kTwoPhaseCommit) {
        dassert(o().coordinatorId);
        attrs.add("coordinator", *o().coordinatorId);
    }

    auto tickSource = opCtx->getServiceContext()->getTickSource();
    auto curTicks = tickSource->getTicks();

    if (terminationCause == TerminationCause::kCommitted) {
        attrs.add("terminationCause", "committed");

        dassert(o().metricsTracker->commitHasStarted());
        dassert(o().commitType != CommitType::kNotInitiated);
        dassert(o().abortCause.empty());
    } else {
        attrs.add("terminationCause", "aborted");

        dassert(!o().abortCause.empty());
        attrs.add("abortCause", o().abortCause);
    }

    const auto& timingStats = o().metricsTracker->getTimingStats();

    std::string commitTypeTemp;
    if (o().metricsTracker->commitHasStarted()) {
        dassert(o().commitType != CommitType::kNotInitiated);
        commitTypeTemp = commitTypeToString(o().commitType);
        attrs.add("commitType", commitTypeTemp);

        attrs.add("commitDuration", timingStats.getCommitDuration(tickSource, curTicks));
    }

    attrs.add("timeActive", timingStats.getTimeActiveMicros(tickSource, curTicks));

    attrs.add("timeInactive", timingStats.getTimeInactiveMicros(tickSource, curTicks));

    // Total duration of the transaction. Logged at the end of the line for consistency with
    // slow command logging.
    attrs.add("duration",
              duration_cast<Milliseconds>(timingStats.getDuration(tickSource, curTicks)));

    LOGV2(51805, "transaction", attrs);
}

void TransactionRouter::Router::_onImplicitAbort(OperationContext* opCtx, const Status& status) {
    if (o().metricsTracker->commitHasStarted() && !o().metricsTracker->isTrackingOver()) {
        // If commit was started but an end time wasn't set, then we don't know the commit result
        // and can't consider the transaction over until the client retries commit and definitively
        // learns the result. Note that this behavior may lead to no logging in some cases, but
        // should avoid logging an incorrect decision.
        return;
    }

    // Implicit abort may execute multiple times if a misbehaving client keeps sending statements
    // for a txnNumber after receiving an error, so only remember the first abort cause.
    if (o().abortCause.empty()) {
        stdx::lock_guard<Client> lk(*opCtx->getClient());
        o(lk).abortCause = status.codeString();
    }

    _endTransactionTrackingIfNecessary(opCtx, TerminationCause::kAborted);
}

void TransactionRouter::Router::_onExplicitAbort(OperationContext* opCtx) {
    // A behaving client should never try to commit after attempting to abort, so we can consider
    // the transaction terminated as soon as explicit abort is observed.
    if (o().abortCause.empty()) {
        // Note this code means the abort was from a user abortTransaction command.
        stdx::lock_guard<Client> lk(*opCtx->getClient());
        o(lk).abortCause = "abort";
    }

    _endTransactionTrackingIfNecessary(opCtx, TerminationCause::kAborted);
}

void TransactionRouter::Router::_onStartCommit(WithLock wl, OperationContext* opCtx) {
    invariant(o().commitType != CommitType::kNotInitiated);

    if (o().metricsTracker->commitHasStarted() || o().metricsTracker->isTrackingOver()) {
        return;
    }

    auto tickSource = opCtx->getServiceContext()->getTickSource();
    o(wl).metricsTracker->startCommit(
        tickSource, tickSource->getTicks(), o().commitType, o().participants.size());
}

void TransactionRouter::Router::_onNonRetryableCommitError(OperationContext* opCtx,
                                                           Status commitStatus) {
    // If the commit failed with a command error that can't be retried on, the transaction shouldn't
    // be able to eventually commit, so it can be considered over from the router's perspective.
    if (o().abortCause.empty()) {
        stdx::lock_guard<Client> lk(*opCtx->getClient());
        o(lk).abortCause = commitStatus.codeString();
    }
    _endTransactionTrackingIfNecessary(opCtx, TerminationCause::kAborted);
}

void TransactionRouter::Router::_onContinue(OperationContext* opCtx) {
    auto tickSource = opCtx->getServiceContext()->getTickSource();

    stdx::lock_guard<Client> lk(*opCtx->getClient());
    o(lk).metricsTracker->trySetActive(tickSource, tickSource->getTicks());
}

void TransactionRouter::Router::_onSuccessfulCommit(OperationContext* opCtx) {
    _endTransactionTrackingIfNecessary(opCtx, TerminationCause::kCommitted);
}

void TransactionRouter::Router::_endTransactionTrackingIfNecessary(
    OperationContext* opCtx, TerminationCause terminationCause) {
    if (o().metricsTracker->isTrackingOver()) {
        // If the transaction was already ended, don't end it again.
        return;
    }

    auto tickSource = opCtx->getServiceContext()->getTickSource();
    auto curTicks = tickSource->getTicks();

    {
        stdx::lock_guard<Client> lk(*opCtx->getClient());

        // In some error contexts, the transaction may not have been started yet, so try setting the
        // transaction's timing stats to active before ending it below. This is a no-op for already
        // active transactions.
        o(lk).metricsTracker->trySetActive(tickSource, curTicks);

        o(lk).metricsTracker->endTransaction(
            tickSource, curTicks, terminationCause, o().commitType, o().abortCause);
    }

    const auto& timingStats = o().metricsTracker->getTimingStats();
    const auto opDuration =
        duration_cast<Milliseconds>(timingStats.getDuration(tickSource, curTicks));

    if (shouldLogSlowOpWithSampling(opCtx,
                                    MONGO_LOGV2_DEFAULT_COMPONENT,
                                    opDuration,
                                    Milliseconds(serverGlobalParams.slowMS.load()))
            .first) {
        _logSlowTransaction(opCtx, terminationCause);
    }
}

void TransactionRouter::Router::_updateLastClientInfo(Client* client) {
    stdx::lock_guard<Client> lk(*client);
    o(lk).lastClientInfo.update(client);
}

bool TransactionRouter::Router::_errorAllowsRetryOnStaleShardOrDb(const Status& status) const {
    const auto staleInfo = status.extraInfo<StaleConfigInfo>();
    const auto staleDB = status.extraInfo<StaleDbRoutingVersion>();
    const auto shardCannotRefreshDueToLocksHeldInfo =
        status.extraInfo<ShardCannotRefreshDueToLocksHeldInfo>();

    // We can retry on the first operation of stale config or db routing version error if there was
    // at most one participant in the transaction and that participant is not a subrouter.
    // There would only be one request sent, and at this point that request has finished so there
    // can't be any outstanding requests that would race with a retry

    // We cannot retry if the only participant is a subrouter because we will not reset the
    // participant's router state due to its participant list not being empty. Since we do not reset
    // the state, the router's atClusterTime value will be stale with respect to the atClusterTime
    // value set on retry, and we will trigger an assertion.
    bool isOnlyParticipantASubRouter =
        o().participants.size() == 1 ? o().participants.begin()->second.isSubRouter : false;
    return (staleInfo || staleDB || shardCannotRefreshDueToLocksHeldInfo) &&
        o().participants.size() <= 1 && !isOnlyParticipantASubRouter &&
        p().latestStmtId == p().firstStmtId;
}

Microseconds TransactionRouter::TimingStats::getDuration(TickSource* tickSource,
                                                         TickSource::Tick curTicks) const {
    dassert(startTime > 0);

    // If the transaction hasn't ended, return how long it has been running for.
    if (endTime == 0) {
        return tickSource->ticksTo<Microseconds>(curTicks - startTime);
    }
    return tickSource->ticksTo<Microseconds>(endTime - startTime);
}

Microseconds TransactionRouter::TimingStats::getCommitDuration(TickSource* tickSource,
                                                               TickSource::Tick curTicks) const {
    dassert(commitStartTime > 0);

    // If the transaction hasn't ended, return how long commit has been running for.
    if (endTime == 0) {
        return tickSource->ticksTo<Microseconds>(curTicks - commitStartTime);
    }
    return tickSource->ticksTo<Microseconds>(endTime - commitStartTime);
}

Microseconds TransactionRouter::TimingStats::getTimeActiveMicros(TickSource* tickSource,
                                                                 TickSource::Tick curTicks) const {
    dassert(startTime > 0);

    if (lastTimeActiveStart != 0) {
        // The transaction is currently active, so return the active time so far plus the time since
        // the transaction became active.
        return timeActiveMicros + tickSource->ticksTo<Microseconds>(curTicks - lastTimeActiveStart);
    }
    return timeActiveMicros;
}

Microseconds TransactionRouter::TimingStats::getTimeInactiveMicros(
    TickSource* tickSource, TickSource::Tick curTicks) const {
    dassert(startTime > 0);

    auto micros = getDuration(tickSource, curTicks) - getTimeActiveMicros(tickSource, curTicks);
    dassert(micros >= Microseconds(0),
            str::stream() << "timeInactiveMicros should never be negative, was: " << micros);
    return micros;
}

TransactionRouter::MetricsTracker::~MetricsTracker() {
    // If there was an in-progress transaction, clean up its stats. This may happen if a transaction
    // is overriden by a higher txnNumber or its session is reaped.
    if (hasStarted() && !isTrackingOver()) {
        // A transaction was started but not ended, so clean up the appropriate stats for it.
        auto routerTxnMetrics = RouterTransactionsMetrics::get(_service);
        routerTxnMetrics->decrementCurrentOpen();

        if (!isActive()) {
            routerTxnMetrics->decrementCurrentInactive();
        } else {
            routerTxnMetrics->decrementCurrentActive();
        }
    }
}

void TransactionRouter::MetricsTracker::trySetActive(TickSource* tickSource,
                                                     TickSource::Tick curTicks) {
    if (isTrackingOver() || isActive()) {
        // A transaction can't become active if it has already ended or is already active.
        return;
    }

    auto routerTxnMetrics = RouterTransactionsMetrics::get(_service);
    if (!hasStarted()) {
        // If the transaction is becoming active for the first time, also set the transaction's
        // start time.
        timingStats.startTime = curTicks;
        timingStats.startWallClockTime = _service->getPreciseClockSource()->now();

        routerTxnMetrics->incrementCurrentOpen();
        routerTxnMetrics->incrementTotalStarted();
    } else {
        // The transaction was already open, so it must have been inactive.
        routerTxnMetrics->decrementCurrentInactive();
    }

    timingStats.lastTimeActiveStart = curTicks;
    routerTxnMetrics->incrementCurrentActive();
}

void TransactionRouter::MetricsTracker::trySetInactive(TickSource* tickSource,
                                                       TickSource::Tick curTicks) {
    if (isTrackingOver() || !isActive()) {
        // If the transaction is already over or the router has already been stashed, the relevant
        // stats should have been updated earlier. In certain error scenarios, it's possible for a
        // transaction to be stashed twice in a row.
        return;
    }

    timingStats.timeActiveMicros +=
        tickSource->ticksTo<Microseconds>(curTicks - timingStats.lastTimeActiveStart);
    timingStats.lastTimeActiveStart = 0;

    auto routerTxnMetrics = RouterTransactionsMetrics::get(_service);
    routerTxnMetrics->decrementCurrentActive();
    routerTxnMetrics->incrementCurrentInactive();
}

void TransactionRouter::MetricsTracker::startCommit(TickSource* tickSource,
                                                    TickSource::Tick curTicks,
                                                    TransactionRouter::CommitType commitType,
                                                    std::size_t numParticipantsAtCommit) {
    dassert(isActive());

    timingStats.commitStartTime = tickSource->getTicks();
    timingStats.commitStartWallClockTime = _service->getPreciseClockSource()->now();

    auto routerTxnMetrics = RouterTransactionsMetrics::get(_service);
    routerTxnMetrics->incrementCommitInitiated(commitType);
    if (commitType != CommitType::kRecoverWithToken) {
        // We only know the participant list if we're not recovering a decision.
        routerTxnMetrics->addToTotalParticipantsAtCommit(numParticipantsAtCommit);
    }
}

void TransactionRouter::MetricsTracker::endTransaction(
    TickSource* tickSource,
    TickSource::Tick curTicks,
    TransactionRouter::TerminationCause terminationCause,
    TransactionRouter::CommitType commitType,
    StringData abortCause) {
    dassert(isActive());

    timingStats.timeActiveMicros +=
        tickSource->ticksTo<Microseconds>(curTicks - timingStats.lastTimeActiveStart);
    timingStats.lastTimeActiveStart = 0;

    timingStats.endTime = curTicks;

    auto routerTxnMetrics = RouterTransactionsMetrics::get(_service);
    routerTxnMetrics->decrementCurrentOpen();
    routerTxnMetrics->decrementCurrentActive();

    if (terminationCause == TerminationCause::kAborted) {
        dassert(!abortCause.empty());
        routerTxnMetrics->incrementTotalAborted();
        routerTxnMetrics->incrementAbortCauseMap(std::string{abortCause});
    } else {
        dassert(commitType != CommitType::kNotInitiated);
        routerTxnMetrics->incrementTotalCommitted();
        routerTxnMetrics->incrementCommitSuccessful(
            commitType, timingStats.getCommitDuration(tickSource, curTicks));
    }
}

repl::ReadConcernArgs TransactionRouter::reconcileReadConcern(
    const boost::optional<repl::ReadConcernArgs>& cmdLevelReadConcern,
    const repl::ReadConcernArgs& txnLevelReadConcern,
    const boost::optional<LogicalTime>& atClusterTimeForSnapshotReadConcern,
    const boost::optional<LogicalTime>& placementConflictTimeForNonSnapshotReadConcern) {

    invariant(atClusterTimeForSnapshotReadConcern ||
              placementConflictTimeForNonSnapshotReadConcern);
    // Transaction level atClusterTime is not tracked in the readConcern object.
    invariant(!txnLevelReadConcern.wasAtClusterTimeSelected());

    boost::optional<LogicalTime> afterClusterTime;
    boost::optional<LogicalTime> atClusterTime;
    boost::optional<repl::ReadConcernLevel> readConcernLevel;

    // Note: getLevel returns 'local' even not set so we need to check explicitly.
    if (txnLevelReadConcern.hasLevel()) {
        readConcernLevel = txnLevelReadConcern.getLevel();
    }

    if (atClusterTimeForSnapshotReadConcern) {
        invariant(txnLevelReadConcern.hasLevel());
        invariant(txnLevelReadConcern.getLevel() == repl::ReadConcernLevel::kSnapshotReadConcern);

        readConcernLevel = repl::ReadConcernLevel::kSnapshotReadConcern;
        atClusterTime = *atClusterTimeForSnapshotReadConcern;
    } else if (placementConflictTimeForNonSnapshotReadConcern) {
        if (auto txnLevelAfterClusterTime = txnLevelReadConcern.getArgsAfterClusterTime()) {
            // We should have already chosen the higher time when setDefaultAtClusterTime was
            // called.
            tassert(
                7750604,
                str::stream() << "placement conflict time chosen: "
                              << placementConflictTimeForNonSnapshotReadConcern->asTimestamp()
                              << " is lower than the afterClusterTime chosen for the transaction: "
                              << txnLevelAfterClusterTime->asTimestamp(),
                *placementConflictTimeForNonSnapshotReadConcern >= *txnLevelAfterClusterTime);
        }

        afterClusterTime = *placementConflictTimeForNonSnapshotReadConcern;
    }

    if (cmdLevelReadConcern) {
        uassert(7750600,
                str::stream() << "read concern level of the transaction changed from "
                              << repl::readConcernLevels::toString(txnLevelReadConcern.getLevel())
                              << " to "
                              << repl::readConcernLevels::toString(cmdLevelReadConcern->getLevel()),
                txnLevelReadConcern.getLevel() == cmdLevelReadConcern->getLevel());

        if (cmdLevelReadConcern->hasLevel()) {
            readConcernLevel = cmdLevelReadConcern->getLevel();
        }

        if (auto cmdLevelAfterClusterTime = cmdLevelReadConcern->getArgsAfterClusterTime()) {
            if (txnLevelReadConcern.getArgsAfterClusterTime()) {
                uassert(7750601,
                        str::stream()
                            << "afterClusterTime of the transaction moved backwards from "
                            << txnLevelReadConcern.getArgsAfterClusterTime()->asTimestamp()
                            << " to " << cmdLevelAfterClusterTime->asTimestamp(),
                        cmdLevelAfterClusterTime->asTimestamp() >=
                            txnLevelReadConcern.getArgsAfterClusterTime()->asTimestamp());
            }

            if (atClusterTimeForSnapshotReadConcern) {
                // atClusterTime takes precedent over afterClusterTime since specifying both is
                // illegal. Do nothing here since we have already set atClusterTime earlier.
                invariant(atClusterTime);
            } else if (placementConflictTimeForNonSnapshotReadConcern) {
                invariant(afterClusterTime);  // We already set this earlier.

                // We should have already chosen the higher time when setDefaultAtClusterTime was
                // called.
                tassert(7750605,
                        str::stream()
                            << "after cluster time chosen: " << afterClusterTime->asTimestamp()
                            << " is lower than the command level afterClusterTime: "
                            << cmdLevelAfterClusterTime->asTimestamp(),
                        *afterClusterTime >= *cmdLevelAfterClusterTime);
            }
        } else if (auto cmdLevelAtClusterTime = cmdLevelReadConcern->getArgsAtClusterTime()) {
            uassert(7750602,
                    str::stream() << "request specified atClusterTime but the transaction didn't "
                                     "originally have one",
                    atClusterTimeForSnapshotReadConcern);

            uassert(7750603,
                    str::stream() << "atClusterTime of the request: "
                                  << cmdLevelAtClusterTime->asTimestamp()
                                  << " is different from time selected by the transaction: "
                                  << atClusterTimeForSnapshotReadConcern->asTimestamp(),
                    cmdLevelAtClusterTime->asTimestamp() ==
                        atClusterTimeForSnapshotReadConcern->asTimestamp());

            invariant(atClusterTime);  // We already set this earlier.
        }
    }

    repl::ReadConcernArgs finalReadConcern(afterClusterTime, readConcernLevel);
    if (atClusterTime) {
        finalReadConcern.setArgsAtClusterTimeForSnapshot(atClusterTime->asTimestamp());
    }

    return finalReadConcern;
}

BSONObj TransactionRouter::appendFieldsForStartTransaction(
    BSONObj cmdObj,
    const repl::ReadConcernArgs& txnLevelReadConcern,
    const boost::optional<LogicalTime>& atClusterTimeForSnapshotReadConcern,
    const boost::optional<LogicalTime>& placementConflictTimeForNonSnapshotReadConcern,
    bool doAppendStartTransaction,
    bool doAppendStartOrContinueTransaction,
    bool hasTxnCreatedAnyDatabase) {
    BSONObjBuilder cmdBob;

    auto strippedFields = stripReadConcernAndShardAndDbVersions(cmdObj, &cmdBob);

    // The bulkWrite requires the shard and database versions on an internal field for
    // every write operation, which has different layout compared to a standard command and it
    // requires special handling. Update the shard/db version for every nsInfo entry.
    if (strippedFields.isBulkWriteCommand) {
        setPlacementConflictTimeToBulkWrite(strippedFields.nsInfoEntries,
                                            hasTxnCreatedAnyDatabase,
                                            placementConflictTimeForNonSnapshotReadConcern,
                                            &cmdBob);
    }

    const auto finalReadConcern =
        reconcileReadConcern(strippedFields.readConcern,
                             txnLevelReadConcern,
                             atClusterTimeForSnapshotReadConcern,
                             placementConflictTimeForNonSnapshotReadConcern);

    if (finalReadConcern.isSpecified()) {
        finalReadConcern.appendInfo(&cmdBob);
    }

    ShardVersion finalShardVersion;

    if (auto shardVersion = strippedFields.shardVersion) {
        if (placementConflictTimeForNonSnapshotReadConcern) {
            shardVersion->setPlacementConflictTime(*placementConflictTimeForNonSnapshotReadConcern);
        }

        shardVersion->serialize(ShardVersion::kShardVersionField, &cmdBob);
    }

    if (auto databaseVersion = strippedFields.databaseVersion) {
        setPlacementConflictTimeToDatabaseVersionIfNeeded(
            placementConflictTimeForNonSnapshotReadConcern,
            hasTxnCreatedAnyDatabase,
            *databaseVersion);

        BSONObjBuilder dbvBuilder(cmdBob.subobjStart(DatabaseVersion::kDatabaseVersionField));
        databaseVersion->serialize(&dbvBuilder);
    }

    if (doAppendStartOrContinueTransaction) {
        cmdBob.append(OperationSessionInfo::kStartOrContinueTransactionFieldName,
                      doAppendStartOrContinueTransaction);
    } else if (doAppendStartTransaction) {
        cmdBob.append(OperationSessionInfoFromClient::kStartTransactionFieldName, true);
    }


    return cmdBob.obj();
}

}  // namespace mongo
