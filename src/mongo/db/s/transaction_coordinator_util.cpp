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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kTransaction

#include "mongo/platform/basic.h"

#include "mongo/db/s/transaction_coordinator_util.h"

#include "mongo/client/remote_command_retry_scheduler.h"
#include "mongo/db/commands/txn_cmds_gen.h"
#include "mongo/db/commands/txn_two_phase_commit_cmds_gen.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/ops/write_ops.h"
#include "mongo/db/repl/repl_client_info.h"
#include "mongo/db/write_concern.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/util/fail_point_service.h"
#include "mongo/util/log.h"

namespace mongo {
namespace txn {
namespace {

MONGO_FAIL_POINT_DEFINE(hangBeforeWaitingForParticipantListWriteConcern);
MONGO_FAIL_POINT_DEFINE(hangBeforeWaitingForDecisionWriteConcern);

MONGO_FAIL_POINT_DEFINE(hangBeforeWritingParticipantList);
MONGO_FAIL_POINT_DEFINE(hangBeforeWritingDecision);
MONGO_FAIL_POINT_DEFINE(hangBeforeDeletingCoordinatorDoc);

using ResponseStatus = executor::TaskExecutor::ResponseStatus;

const WriteConcernOptions kMajorityWriteConcern(WriteConcernOptions::kMajority,
                                                WriteConcernOptions::SyncMode::UNSET,
                                                WriteConcernOptions::kNoTimeout);

const Backoff kExponentialBackoff(Seconds(1), Milliseconds::max());

const ReadPreferenceSetting kPrimaryReadPreference{ReadPreference::PrimaryOnly};

BSONArray buildParticipantListMatchesConditions(const std::vector<ShardId>& participantList) {
    BSONArrayBuilder barr;
    for (const auto& participant : participantList) {
        barr.append(participant.toString());
    }

    const long long participantListLength = participantList.size();
    BSONObj participantListHasSize = BSON(TransactionCoordinatorDocument::kParticipantsFieldName
                                          << BSON("$size" << participantListLength));

    BSONObj participantListContains =
        BSON(TransactionCoordinatorDocument::kParticipantsFieldName << BSON("$all" << barr.arr()));

    return BSON_ARRAY(participantListContains << participantListHasSize);
}

std::string buildParticipantListString(const std::vector<ShardId>& participantList) {
    StringBuilder ss;
    ss << "[";
    for (const auto& participant : participantList) {
        ss << participant << " ";
    }
    ss << "]";
    return ss.str();
}

bool shouldRetryPersistingCoordinatorState(const Status& responseStatus) {
    // Writes to the local node are expected to succeed if this node is still primary, so *only*
    // retry if the write was explicitly interrupted (we do not allow a user to stop a commit
    // coordination by issuing killOp, since that can stall resources across the cluster. However,
    // the write may also fail if the coordinator document has been manually tampered with; in this
    // case we do not retry, since the write is unlikely to succeed the second time.
    return responseStatus == ErrorCodes::Interrupted;
}

}  // namespace

namespace {
void persistParticipantListBlocking(OperationContext* opCtx,
                                    const LogicalSessionId& lsid,
                                    TxnNumber txnNumber,
                                    const std::vector<ShardId>& participantList) {
    LOG(3) << "Going to write participant list for " << lsid.getId() << ':' << txnNumber;

    if (MONGO_FAIL_POINT(hangBeforeWritingParticipantList)) {
        LOG(0) << "Hit hangBeforeWritingParticipantList failpoint";
        MONGO_FAIL_POINT_PAUSE_WHILE_SET_OR_INTERRUPTED(opCtx, hangBeforeWritingParticipantList);
    }

    OperationSessionInfo sessionInfo;
    sessionInfo.setSessionId(lsid);
    sessionInfo.setTxnNumber(txnNumber);

    DBDirectClient client(opCtx);

    // Throws if serializing the request or deserializing the response fails.
    const auto commandResponse = client.runCommand([&] {
        write_ops::Update updateOp(NamespaceString::kTransactionCoordinatorsNamespace);
        updateOp.setUpdates({[&] {
            write_ops::UpdateOpEntry entry;

            // Ensure that the document for the (lsid, txnNumber) either has no participant list or
            // has the same participant list. The document may have the same participant list if an
            // earlier attempt to write the participant list failed waiting for writeConcern.
            BSONObj noParticipantList = BSON(TransactionCoordinatorDocument::kParticipantsFieldName
                                             << BSON("$exists" << false));
            BSONObj sameParticipantList =
                BSON("$and" << buildParticipantListMatchesConditions(participantList));
            entry.setQ(BSON(TransactionCoordinatorDocument::kIdFieldName
                            << sessionInfo.toBSON()
                            << "$or"
                            << BSON_ARRAY(noParticipantList << sameParticipantList)));

            // Update with participant list.
            TransactionCoordinatorDocument doc;
            doc.setId(std::move(sessionInfo));
            doc.setParticipants(std::move(participantList));
            entry.setU(doc.toBSON());

            entry.setUpsert(true);
            return entry;
        }()});
        return updateOp.serialize({});
    }());

    const auto upsertStatus = getStatusFromWriteCommandReply(commandResponse->getCommandReply());

    // Convert a DuplicateKey error to an anonymous error.
    if (upsertStatus.code() == ErrorCodes::DuplicateKey) {
        // Attempt to include the document for this (lsid, txnNumber) in the error message, if one
        // exists. Note that this is best-effort: the document may have been deleted or manually
        // changed since the update above ran.
        const auto doc = client.findOne(
            NamespaceString::kTransactionCoordinatorsNamespace.toString(),
            QUERY(TransactionCoordinatorDocument::kIdFieldName << sessionInfo.toBSON()));
        uasserted(51025,
                  str::stream() << "While attempting to write participant list "
                                << buildParticipantListString(participantList)
                                << " for "
                                << lsid.getId()
                                << ':'
                                << txnNumber
                                << ", found document with a different participant list: "
                                << doc);
    }

    // Throw any other error.
    uassertStatusOK(upsertStatus);

    LOG(3) << "Wrote participant list for " << lsid.getId() << ':' << txnNumber;

    if (MONGO_FAIL_POINT(hangBeforeWaitingForParticipantListWriteConcern)) {
        LOG(0) << "Hit hangBeforeWaitingForParticipantListWriteConcern failpoint";
        MONGO_FAIL_POINT_PAUSE_WHILE_SET_OR_INTERRUPTED(
            opCtx, hangBeforeWaitingForParticipantListWriteConcern);
    }

    WriteConcernResult unusedWCResult;
    uassertStatusOK(
        waitForWriteConcern(opCtx,
                            repl::ReplClientInfo::forClient(opCtx->getClient()).getLastOp(),
                            kMajorityWriteConcern,
                            &unusedWCResult));
}
}  // namespace

Future<void> persistParticipantsList(txn::AsyncWorkScheduler& scheduler,
                                     const LogicalSessionId& lsid,
                                     TxnNumber txnNumber,
                                     const txn::ParticipantsList& participants) {
    return txn::doWhile(
        scheduler,
        boost::none /* no need for a backoff */,
        [](const Status& s) { return shouldRetryPersistingCoordinatorState(s); },
        [&scheduler, lsid, txnNumber, participants] {
            return scheduler.scheduleWork([lsid, txnNumber, participants](OperationContext* opCtx) {
                persistParticipantListBlocking(opCtx, lsid, txnNumber, participants);
            });
        });
}

void PrepareVoteConsensus::registerVote(const PrepareResponse& vote) {
    if (vote.vote == PrepareVote::kCommit) {
        ++_numCommitVotes;
        _maxPrepareTimestamp = std::max(_maxPrepareTimestamp, *vote.prepareTimestamp);
    } else if (vote.vote == PrepareVote::kAbort) {
        ++_numAbortVotes;
    } else {
        ++_numNoVotes;
    }
}

CoordinatorCommitDecision PrepareVoteConsensus::decision() const {
    invariant(_numShards == _numCommitVotes + _numAbortVotes + _numNoVotes);

    CoordinatorCommitDecision decision;
    if (_numCommitVotes == _numShards) {
        decision.setDecision(CommitDecision::kCommit);
        decision.setCommitTimestamp(_maxPrepareTimestamp);
    } else {
        decision.setDecision(CommitDecision::kAbort);
    }
    return decision;
}

Future<PrepareVoteConsensus> sendPrepare(ServiceContext* service,
                                         txn::AsyncWorkScheduler& scheduler,
                                         const LogicalSessionId& lsid,
                                         TxnNumber txnNumber,
                                         const txn::ParticipantsList& participants) {
    PrepareTransaction prepareTransaction;
    prepareTransaction.setDbName(NamespaceString::kAdminDb);
    auto prepareObj = prepareTransaction.toBSON(
        BSON("lsid" << lsid.toBSON() << "txnNumber" << txnNumber << "autocommit" << false
                    << WriteConcernOptions::kWriteConcernField
                    << WriteConcernOptions::Majority));

    std::vector<Future<PrepareResponse>> responses;

    // Send prepare to all participants asynchronously and collect their future responses in a
    // vector of responses.
    auto prepareScheduler = scheduler.makeChildScheduler();

    for (const auto& participant : participants) {
        responses.emplace_back(
            sendPrepareToShard(service, *prepareScheduler, participant, prepareObj));
    }

    // Asynchronously aggregate all prepare responses to find the decision and max prepare timestamp
    // (used for commit), stopping the aggregation and preventing any further retries as soon as an
    // abort decision is received. Return a future containing the result.
    return txn::collect(
        std::move(responses),
        // Initial value
        PrepareVoteConsensus{int(participants.size())},
        // Aggregates an incoming response (next) with the existing aggregate value (result)
        [prepareScheduler = std::move(prepareScheduler)](PrepareVoteConsensus & result,
                                                         const PrepareResponse& next) {
            result.registerVote(next);

            if (next.vote == PrepareVote::kAbort) {
                prepareScheduler->shutdown(
                    {ErrorCodes::TransactionCoordinatorReachedAbortDecision,
                     str::stream() << "Received abort vote from " << next.shardId});
            }

            return txn::ShouldStopIteration::kNo;
        });
}

namespace {
void persistDecisionBlocking(OperationContext* opCtx,
                             const LogicalSessionId& lsid,
                             TxnNumber txnNumber,
                             const std::vector<ShardId>& participantList,
                             const boost::optional<Timestamp>& commitTimestamp) {
    LOG(3) << "Going to write decision " << (commitTimestamp ? "commit" : "abort") << " for "
           << lsid.getId() << ':' << txnNumber;

    if (MONGO_FAIL_POINT(hangBeforeWritingDecision)) {
        LOG(0) << "Hit hangBeforeWritingDecision failpoint";
        MONGO_FAIL_POINT_PAUSE_WHILE_SET_OR_INTERRUPTED(opCtx, hangBeforeWritingDecision);
    }

    OperationSessionInfo sessionInfo;
    sessionInfo.setSessionId(lsid);
    sessionInfo.setTxnNumber(txnNumber);

    DBDirectClient client(opCtx);

    // Throws if serializing the request or deserializing the response fails.
    const auto commandResponse = client.runCommand([&] {
        write_ops::Update updateOp(NamespaceString::kTransactionCoordinatorsNamespace);
        updateOp.setUpdates({[&] {
            write_ops::UpdateOpEntry entry;

            // Ensure that the document for the (lsid, txnNumber) has the same participant list and
            // either has no decision or the same decision. The document may have the same decision
            // if an earlier attempt to write the decision failed waiting for writeConcern.
            BSONObj noDecision = BSON(TransactionCoordinatorDocument::kDecisionFieldName
                                      << BSON("$exists" << false));
            BSONObj sameDecision;
            if (commitTimestamp) {
                sameDecision = BSON(TransactionCoordinatorDocument::kDecisionFieldName
                                    << BSON(TransactionCoordinatorDocument::kDecisionFieldName
                                            << "commit"
                                            << "commitTimestamp"
                                            << *commitTimestamp));
            } else {
                sameDecision =
                    BSON(TransactionCoordinatorDocument::kDecisionFieldName
                         << BSON(TransactionCoordinatorDocument::kDecisionFieldName << "abort"));
            }
            entry.setQ(BSON(TransactionCoordinatorDocument::kIdFieldName
                            << sessionInfo.toBSON()
                            << "$and"
                            << buildParticipantListMatchesConditions(participantList)
                            << "$or"
                            << BSON_ARRAY(noDecision << sameDecision)));

            // Update with decision.
            TransactionCoordinatorDocument doc;
            doc.setId(sessionInfo);
            doc.setParticipants(std::move(participantList));
            txn::CoordinatorCommitDecision decision;
            if (commitTimestamp) {
                decision.setDecision(CommitDecision::kCommit);
                decision.setCommitTimestamp(commitTimestamp);
            } else {
                decision.setDecision(CommitDecision::kAbort);
            }
            doc.setDecision(decision);
            entry.setU(doc.toBSON());

            return entry;
        }()});
        return updateOp.serialize({});
    }());

    const auto commandReply = commandResponse->getCommandReply();
    uassertStatusOK(getStatusFromWriteCommandReply(commandReply));

    // If no document matched, throw an anonymous error. (The update itself will not have thrown an
    // error, because it's legal for an update to match no documents.)
    if (commandReply.getIntField("n") != 1) {
        // Attempt to include the document for this (lsid, txnNumber) in the error message, if one
        // exists. Note that this is best-effort: the document may have been deleted or manually
        // changed since the update above ran.
        const auto doc = client.findOne(
            NamespaceString::kTransactionCoordinatorsNamespace.toString(),
            QUERY(TransactionCoordinatorDocument::kIdFieldName << sessionInfo.toBSON()));
        uasserted(51026,
                  str::stream() << "While attempting to write decision "
                                << (commitTimestamp ? "'commit'" : "'abort'")
                                << " for"
                                << lsid.getId()
                                << ':'
                                << txnNumber
                                << ", either failed to find document for this lsid:txnNumber or "
                                   "document existed with a different participant list, decision "
                                   "or commitTimestamp: "
                                << doc);
    }

    LOG(3) << "Wrote decision " << (commitTimestamp ? "commit" : "abort") << " for " << lsid.getId()
           << ':' << txnNumber;

    if (MONGO_FAIL_POINT(hangBeforeWaitingForDecisionWriteConcern)) {
        LOG(0) << "Hit hangBeforeWaitingForDecisionWriteConcern failpoint";
        MONGO_FAIL_POINT_PAUSE_WHILE_SET_OR_INTERRUPTED(opCtx,
                                                        hangBeforeWaitingForDecisionWriteConcern);
    }

    WriteConcernResult unusedWCResult;
    uassertStatusOK(
        waitForWriteConcern(opCtx,
                            repl::ReplClientInfo::forClient(opCtx->getClient()).getLastOp(),
                            kMajorityWriteConcern,
                            &unusedWCResult));
}
}  // namespace

Future<void> persistDecision(txn::AsyncWorkScheduler& scheduler,
                             const LogicalSessionId& lsid,
                             TxnNumber txnNumber,
                             const txn::ParticipantsList& participants,
                             const boost::optional<Timestamp>& commitTimestamp) {
    return txn::doWhile(
        scheduler,
        boost::none /* no need for a backoff */,
        [](const Status& s) { return shouldRetryPersistingCoordinatorState(s); },
        [&scheduler, lsid, txnNumber, participants, commitTimestamp] {
            return scheduler.scheduleWork(
                [lsid, txnNumber, participants, commitTimestamp](OperationContext* opCtx) {
                    persistDecisionBlocking(opCtx, lsid, txnNumber, participants, commitTimestamp);
                });
        });
}

Future<void> sendCommit(ServiceContext* service,
                        txn::AsyncWorkScheduler& scheduler,
                        const LogicalSessionId& lsid,
                        TxnNumber txnNumber,
                        const txn::ParticipantsList& participants,
                        Timestamp commitTimestamp) {
    CommitTransaction commitTransaction;
    commitTransaction.setDbName(NamespaceString::kAdminDb);
    commitTransaction.setCommitTimestamp(commitTimestamp);
    auto commitObj = commitTransaction.toBSON(
        BSON("lsid" << lsid.toBSON() << "txnNumber" << txnNumber << "autocommit" << false
                    << WriteConcernOptions::kWriteConcernField
                    << WriteConcernOptions::Majority));

    std::vector<Future<void>> responses;
    for (const auto& participant : participants) {
        responses.push_back(sendDecisionToShard(service, scheduler, participant, commitObj));
    }
    return txn::whenAll(responses);
}

Future<void> sendAbort(ServiceContext* service,
                       txn::AsyncWorkScheduler& scheduler,
                       const LogicalSessionId& lsid,
                       TxnNumber txnNumber,
                       const txn::ParticipantsList& participants) {
    AbortTransaction abortTransaction;
    abortTransaction.setDbName(NamespaceString::kAdminDb);
    auto abortObj = abortTransaction.toBSON(
        BSON("lsid" << lsid.toBSON() << "txnNumber" << txnNumber << "autocommit" << false
                    << WriteConcernOptions::kWriteConcernField
                    << WriteConcernOptions::Majority));

    std::vector<Future<void>> responses;
    for (const auto& participant : participants) {
        responses.push_back(sendDecisionToShard(service, scheduler, participant, abortObj));
    }
    return txn::whenAll(responses);
}

namespace {
void deleteCoordinatorDocBlocking(OperationContext* opCtx,
                                  const LogicalSessionId& lsid,
                                  TxnNumber txnNumber) {
    LOG(3) << "Going to delete coordinator doc for " << lsid.getId() << ':' << txnNumber;

    if (MONGO_FAIL_POINT(hangBeforeDeletingCoordinatorDoc)) {
        LOG(0) << "Hit hangBeforeDeletingCoordinatorDoc failpoint";
        MONGO_FAIL_POINT_PAUSE_WHILE_SET_OR_INTERRUPTED(opCtx, hangBeforeDeletingCoordinatorDoc);
    }

    OperationSessionInfo sessionInfo;
    sessionInfo.setSessionId(lsid);
    sessionInfo.setTxnNumber(txnNumber);

    DBDirectClient client(opCtx);

    // Throws if serializing the request or deserializing the response fails.
    auto commandResponse = client.runCommand([&] {
        write_ops::Delete deleteOp(NamespaceString::kTransactionCoordinatorsNamespace);
        deleteOp.setDeletes({[&] {
            write_ops::DeleteOpEntry entry;

            // Ensure the document is only deleted after a decision has been made.
            BSONObj abortDecision =
                BSON(TransactionCoordinatorDocument::kDecisionFieldName << "abort");
            BSONObj commitDecision =
                BSON(TransactionCoordinatorDocument::kDecisionFieldName << "commit");
            entry.setQ(BSON(TransactionCoordinatorDocument::kIdFieldName
                            << sessionInfo.toBSON()
                            << TransactionCoordinatorDocument::kDecisionFieldName
                            << BSON("$exists" << true)));

            entry.setMulti(false);
            return entry;
        }()});
        return deleteOp.serialize({});
    }());

    const auto commandReply = commandResponse->getCommandReply();
    uassertStatusOK(getStatusFromWriteCommandReply(commandReply));

    // If no document matched, throw an anonymous error. (The delete itself will not have thrown an
    // error, because it's legal for a delete to match no documents.)
    if (commandReply.getIntField("n") != 1) {
        // Attempt to include the document for this (lsid, txnNumber) in the error message, if one
        // exists. Note that this is best-effort: the document may have been deleted or manually
        // changed since the update above ran.
        const auto doc = client.findOne(
            NamespaceString::kTransactionCoordinatorsNamespace.toString(),
            QUERY(TransactionCoordinatorDocument::kIdFieldName << sessionInfo.toBSON()));
        uasserted(51027,
                  str::stream() << "While attempting to delete document for " << lsid.getId() << ':'
                                << txnNumber
                                << ", either failed to find document for this lsid:txnNumber or "
                                   "document existed without a decision: "
                                << doc);
    }

    LOG(3) << "Deleted coordinator doc for " << lsid.getId() << ':' << txnNumber;
}
}  // namespace

Future<void> deleteCoordinatorDoc(txn::AsyncWorkScheduler& scheduler,
                                  const LogicalSessionId& lsid,
                                  TxnNumber txnNumber) {
    return txn::doWhile(scheduler,
                        boost::none /* no need for a backoff */,
                        [](const Status& s) { return shouldRetryPersistingCoordinatorState(s); },
                        [&scheduler, lsid, txnNumber] {
                            return scheduler.scheduleWork(
                                [lsid, txnNumber](OperationContext* opCtx) {
                                    deleteCoordinatorDocBlocking(opCtx, lsid, txnNumber);
                                });
                        });
}

std::vector<TransactionCoordinatorDocument> readAllCoordinatorDocs(OperationContext* opCtx) {
    std::vector<TransactionCoordinatorDocument> allCoordinatorDocs;

    DBDirectClient client(opCtx);
    auto coordinatorDocsCursor =
        client.query(NamespaceString::kTransactionCoordinatorsNamespace, Query{});

    while (coordinatorDocsCursor->more()) {
        // TODO (SERVER-38307): Try/catch around parsing the document and skip the document if it
        // fails to parse.
        auto nextDecision = TransactionCoordinatorDocument::parse(IDLParserErrorContext(""),
                                                                  coordinatorDocsCursor->next());
        allCoordinatorDocs.push_back(nextDecision);
    }

    return allCoordinatorDocs;
}

Future<PrepareResponse> sendPrepareToShard(ServiceContext* service,
                                           txn::AsyncWorkScheduler& scheduler,
                                           const ShardId& shardId,
                                           const BSONObj& commandObj) {
    const bool isLocalShard = (shardId == txn::getLocalShardId(service));

    auto f = txn::doWhile(
        scheduler,
        kExponentialBackoff,
        [](const StatusWith<PrepareResponse>& swPrepareResponse) {
            // *Always* retry until hearing a conclusive response or being told to stop via a
            // coordinator-specific code.
            return !swPrepareResponse.isOK() &&
                swPrepareResponse != ErrorCodes::TransactionCoordinatorSteppingDown &&
                swPrepareResponse != ErrorCodes::TransactionCoordinatorReachedAbortDecision;
        },
        [&scheduler, shardId, isLocalShard, commandObj = commandObj.getOwned() ] {
            LOG(3) << "Coordinator going to send command " << commandObj << " to "
                   << (isLocalShard ? " local " : "") << " shard " << shardId;

            return scheduler.scheduleRemoteCommand(shardId, kPrimaryReadPreference, commandObj)
                .then([ shardId, commandObj = commandObj.getOwned() ](ResponseStatus response) {
                    auto status = getStatusFromCommandResult(response.data);
                    auto wcStatus = getWriteConcernStatusFromCommandResult(response.data);

                    // There must be no writeConcern error in order for us to interpret the command
                    // response.
                    if (!wcStatus.isOK()) {
                        status = wcStatus;
                    }

                    if (status.isOK()) {
                        auto prepareTimestampField = response.data["prepareTimestamp"];
                        if (prepareTimestampField.eoo() ||
                            prepareTimestampField.timestamp().isNull()) {
                            LOG(0) << "Coordinator shard received an OK response to "
                                      "prepareTransaction "
                                      "without a prepareTimestamp from shard "
                                   << shardId << ", which is not expected behavior. "
                                                 "Interpreting the response from "
                                   << shardId << " as a vote to abort";
                            return PrepareResponse{shardId, PrepareVote::kAbort, boost::none};
                        }

                        LOG(3) << "Coordinator shard received a vote to commit from shard "
                               << shardId
                               << " with prepareTimestamp: " << prepareTimestampField.timestamp();
                        return PrepareResponse{
                            shardId, PrepareVote::kCommit, prepareTimestampField.timestamp()};
                    }

                    LOG(3) << "Coordinator shard received " << status << " from shard " << shardId
                           << " for " << commandObj;

                    if (ErrorCodes::isVoteAbortError(status.code())) {
                        return PrepareResponse{shardId, PrepareVote::kAbort, boost::none};
                    }

                    uassertStatusOK(status);
                    MONGO_UNREACHABLE;
                })
                .onError<ErrorCodes::ShardNotFound>([shardId, isLocalShard](const Status&) {
                    invariant(!isLocalShard);
                    // ShardNotFound may indicate that the participant shard has been removed (it
                    // could also mean the participant shard was recently added and this node
                    // refreshed its ShardRegistry from a stale config secondary).
                    //
                    // Since this node can't know which is the case, it is safe to pessimistically
                    // treat ShardNotFound as a vote to abort, which is always safe since the node
                    // must then send abort.
                    return Future<PrepareResponse>::makeReady(
                        {shardId, CommitDecision::kAbort, boost::none});
                });
        });

    return std::move(f).onError<ErrorCodes::TransactionCoordinatorReachedAbortDecision>(
        [shardId](const Status&) {
            LOG(3) << "Prepare stopped retrying due to retrying being cancelled";
            return PrepareResponse{shardId, boost::none, boost::none};
        });
}

Future<void> sendDecisionToShard(ServiceContext* service,
                                 txn::AsyncWorkScheduler& scheduler,
                                 const ShardId& shardId,
                                 const BSONObj& commandObj) {
    const bool isLocalShard = (shardId == txn::getLocalShardId(service));

    return txn::doWhile(
        scheduler,
        kExponentialBackoff,
        [](const Status& s) {
            // *Always* retry until hearing a conclusive response or being told to stop via a
            // coordinator-specific code.
            return !s.isOK() && s != ErrorCodes::TransactionCoordinatorSteppingDown;
        },
        [&scheduler, shardId, isLocalShard, commandObj = commandObj.getOwned() ] {
            LOG(3) << "Coordinator going to send command " << commandObj << " to "
                   << (isLocalShard ? "local" : "") << " shard " << shardId;

            return scheduler.scheduleRemoteCommand(shardId, kPrimaryReadPreference, commandObj)
                .then([ shardId, commandObj = commandObj.getOwned() ](ResponseStatus response) {
                    auto status = getStatusFromCommandResult(response.data);
                    auto wcStatus = getWriteConcernStatusFromCommandResult(response.data);

                    // There must be no writeConcern error in order for us to interpret the command
                    // response.
                    if (!wcStatus.isOK()) {
                        status = wcStatus;
                    }

                    LOG(3) << "Coordinator shard received " << status << " in response to "
                           << commandObj << " from shard " << shardId;

                    if (ErrorCodes::isVoteAbortError(status.code())) {
                        // Interpret voteAbort errors as an ack.
                        status = Status::OK();
                    }

                    return status;
                })
                .onError<ErrorCodes::ShardNotFound>([isLocalShard](const Status& s) {
                    invariant(!isLocalShard);
                    // TODO (SERVER-38918): Unlike for prepare, there is no pessimistic way to
                    // handle ShardNotFound. It's not safe to treat ShardNotFound as an ack, because
                    // this node may have refreshed its ShardRegistry from a stale config secondary.
                    fassert(51068, false);
                });
        });
}

}  // namespace txn
}  // namespace mongo
