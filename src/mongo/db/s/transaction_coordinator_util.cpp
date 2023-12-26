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


#include <absl/container/flat_hash_set.h>
#include <algorithm>
#include <boost/cstdint.hpp>
#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>
#include <cstdint>
#include <memory>
#include <type_traits>

#include "mongo/base/error_codes.h"
#include "mongo/base/status_with.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/util/builder.h"
#include "mongo/bson/util/builder_fwd.h"
#include "mongo/client/dbclient_cursor.h"
#include "mongo/client/read_preference.h"
#include "mongo/db/commands/txn_cmds_gen.h"
#include "mongo/db/commands/txn_two_phase_commit_cmds_gen.h"
#include "mongo/db/database_name.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/ops/write_ops_gen.h"
#include "mongo/db/ops/write_ops_parsers.h"
#include "mongo/db/query/collation/collator_factory_interface.h"
#include "mongo/db/query/find_command.h"
#include "mongo/db/repl/change_stream_oplog_notification.h"
#include "mongo/db/repl/repl_client_info.h"
#include "mongo/db/s/transaction_coordinator_futures_util.h"
#include "mongo/db/s/transaction_coordinator_util.h"
#include "mongo/db/s/transaction_coordinator_worker_curop_repository.h"
#include "mongo/db/server_feature_flags_gen.h"
#include "mongo/db/server_options.h"
#include "mongo/db/session/logical_session_id_helpers.h"
#include "mongo/db/transaction_resources.h"
#include "mongo/db/write_concern_options.h"
#include "mongo/executor/task_executor.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/logv2/log.h"
#include "mongo/logv2/log_attr.h"
#include "mongo/logv2/log_component.h"
#include "mongo/logv2/redaction.h"
#include "mongo/platform/compiler.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/rpc/reply_interface.h"
#include "mongo/rpc/unique_message.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/concurrency/admission_context.h"
#include "mongo/util/decorable.h"
#include "mongo/util/duration.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/future_impl.h"
#include "mongo/util/str.h"
#include "mongo/util/time_support.h"
#include "mongo/util/uuid.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTransaction


namespace mongo {
namespace txn {
namespace {

MONGO_FAIL_POINT_DEFINE(hangBeforeWritingParticipantList);
MONGO_FAIL_POINT_DEFINE(hangBeforeSendingPrepare);
MONGO_FAIL_POINT_DEFINE(hangBeforeWritingDecision);
MONGO_FAIL_POINT_DEFINE(hangBeforeSendingCommit);
MONGO_FAIL_POINT_DEFINE(hangBeforeSendingAbort);
MONGO_FAIL_POINT_DEFINE(hangBeforeWritingEndOfTransaction);
MONGO_FAIL_POINT_DEFINE(hangBeforeDeletingCoordinatorDoc);
MONGO_FAIL_POINT_DEFINE(hangAfterDeletingCoordinatorDoc);

using ResponseStatus = executor::TaskExecutor::ResponseStatus;
using CoordinatorAction = TransactionCoordinatorWorkerCurOpRepository::CoordinatorAction;

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

template <typename T>
bool shouldRetryPersistingCoordinatorState(const StatusWith<T>& responseStatus) {
    return !responseStatus.isOK() &&
        responseStatus != ErrorCodes::TransactionCoordinatorSteppingDown;
}

}  // namespace

namespace {
repl::OpTime persistParticipantListBlocking(
    OperationContext* opCtx,
    const LogicalSessionId& lsid,
    const TxnNumberAndRetryCounter& txnNumberAndRetryCounter,
    const std::vector<ShardId>& participantList) {
    LOGV2_DEBUG(22463,
                3,
                "Going to write participant list",
                "sessionId"_attr = lsid,
                "txnNumberAndRetryCounter"_attr = txnNumberAndRetryCounter);

    if (MONGO_unlikely(hangBeforeWritingParticipantList.shouldFail())) {
        LOGV2(22464, "Hit hangBeforeWritingParticipantList failpoint");
        hangBeforeWritingParticipantList.pauseWhileSet(opCtx);
    }

    OperationSessionInfo sessionInfo;
    sessionInfo.setSessionId(lsid);
    sessionInfo.setTxnNumber(txnNumberAndRetryCounter.getTxnNumber());
    if (auto txnRetryCounter = txnNumberAndRetryCounter.getTxnRetryCounter();
        txnRetryCounter && !isDefaultTxnRetryCounter(*txnRetryCounter)) {
        sessionInfo.setTxnRetryCounter(*txnNumberAndRetryCounter.getTxnRetryCounter());
    }

    DBDirectClient client(opCtx);

    // Throws if serializing the request or deserializing the response fails.
    const auto commandResponse = client.runCommand([&] {
        write_ops::UpdateCommandRequest updateOp(
            NamespaceString::kTransactionCoordinatorsNamespace);
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
                            << sessionInfo.toBSON() << "$or"
                            << BSON_ARRAY(noParticipantList << sameParticipantList)));

            // Update with participant list.
            TransactionCoordinatorDocument doc;
            doc.setId(std::move(sessionInfo));
            doc.setParticipants(participantList);
            entry.setU(write_ops::UpdateModification::parseFromClassicUpdate(doc.toBSON()));

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
            NamespaceString::kTransactionCoordinatorsNamespace,
            BSON(TransactionCoordinatorDocument::kIdFieldName << sessionInfo.toBSON()));
        uasserted(51025,
                  str::stream() << "While attempting to write participant list "
                                << buildParticipantListString(participantList) << " for "
                                << lsid.getId() << ':' << txnNumberAndRetryCounter.toBSON()
                                << ", found document with a different participant list: " << doc);
    }

    // Throw any other error.
    uassertStatusOK(upsertStatus);

    LOGV2_DEBUG(22465,
                3,
                "Wrote participant list",
                "sessionId"_attr = lsid,
                "txnNumberAndRetryCounter"_attr = txnNumberAndRetryCounter);

    return repl::ReplClientInfo::forClient(opCtx->getClient()).getLastOp();
}
}  // namespace

Future<repl::OpTime> persistParticipantsList(
    txn::AsyncWorkScheduler& scheduler,
    const LogicalSessionId& lsid,
    const TxnNumberAndRetryCounter& txnNumberAndRetryCounter,
    const txn::ParticipantsList& participants) {
    return txn::doWhile(
        scheduler,
        boost::none /* no need for a backoff */,
        [](const StatusWith<repl::OpTime>& s) { return shouldRetryPersistingCoordinatorState(s); },
        [&scheduler, lsid, txnNumberAndRetryCounter, participants] {
            return scheduler.scheduleWork([lsid, txnNumberAndRetryCounter, participants](
                                              OperationContext* opCtx) {
                // Skip ticket acquisition in order to prevent possible deadlock when
                // participants are in the prepared state. See SERVER-82883 and SERVER-60682.
                ScopedAdmissionPriorityForLock skipTicketAcquisition(
                    shard_role_details::getLocker(opCtx), AdmissionContext::Priority::kImmediate);
                getTransactionCoordinatorWorkerCurOpRepository()->set(
                    opCtx,
                    lsid,
                    txnNumberAndRetryCounter,
                    CoordinatorAction::kWritingParticipantList);
                return persistParticipantListBlocking(
                    opCtx, lsid, txnNumberAndRetryCounter, participants);
            });
        });
}

void PrepareVoteConsensus::registerVote(const PrepareResponse& vote) {
    if (vote.vote == PrepareVote::kCommit) {
        ++_numCommitVotes;
        _maxPrepareTimestamp = std::max(_maxPrepareTimestamp, *vote.prepareTimestamp);
        _affectedNamespaces.insert(vote.affectedNamespaces.begin(), vote.affectedNamespaces.end());
    } else {
        vote.vote == PrepareVote::kAbort ? ++_numAbortVotes : ++_numNoVotes;

        if (!_abortStatus)
            _abortStatus.emplace(*vote.abortReason);
    }
}

CoordinatorCommitDecision PrepareVoteConsensus::decision() const {
    invariant(_numShards == _numCommitVotes + _numAbortVotes + _numNoVotes);

    CoordinatorCommitDecision decision;
    if (_numCommitVotes == _numShards) {
        decision.setDecision(CommitDecision::kCommit);
        decision.setCommitTimestamp(_maxPrepareTimestamp);
    } else {
        invariant(_abortStatus);
        decision.setDecision(CommitDecision::kAbort);
        decision.setAbortStatus(*_abortStatus);
    }
    return decision;
}

Future<PrepareVoteConsensus> sendPrepare(ServiceContext* service,
                                         txn::AsyncWorkScheduler& scheduler,
                                         const LogicalSessionId& lsid,
                                         const TxnNumberAndRetryCounter& txnNumberAndRetryCounter,
                                         const APIParameters& apiParams,
                                         const txn::ParticipantsList& participants) {
    PrepareTransaction prepareTransaction;
    prepareTransaction.setDbName(DatabaseName::kAdmin);
    BSONObjBuilder bob(BSON("lsid" << lsid.toBSON() << "txnNumber"
                                   << txnNumberAndRetryCounter.getTxnNumber() << "autocommit"
                                   << false << WriteConcernOptions::kWriteConcernField
                                   << WriteConcernOptions::Majority));
    if (auto txnRetryCounter = txnNumberAndRetryCounter.getTxnRetryCounter();
        txnRetryCounter && !isDefaultTxnRetryCounter(*txnRetryCounter)) {
        bob.append(OperationSessionInfoFromClient::kTxnRetryCounterFieldName, *txnRetryCounter);
    }
    apiParams.appendInfo(&bob);
    auto prepareObj = prepareTransaction.toBSON(bob.obj());

    std::vector<Future<PrepareResponse>> responses;

    // Send prepare to all participants asynchronously and collect their future responses in a
    // vector of responses.
    auto prepareScheduler = scheduler.makeChildScheduler();

    OperationContextFn operationContextFn = [lsid,
                                             txnNumberAndRetryCounter](OperationContext* opCtx) {
        invariant(opCtx);
        getTransactionCoordinatorWorkerCurOpRepository()->set(
            opCtx, lsid, txnNumberAndRetryCounter, CoordinatorAction::kSendingPrepare);

        if (MONGO_unlikely(hangBeforeSendingPrepare.shouldFail())) {
            LOGV2(22466, "Hit hangBeforeSendingPrepare failpoint");
            hangBeforeSendingPrepare.pauseWhileSet(opCtx);
        }
    };

    for (const auto& participant : participants) {
        responses.emplace_back(sendPrepareToShard(service,
                                                  *prepareScheduler,
                                                  lsid,
                                                  txnNumberAndRetryCounter,
                                                  participant,
                                                  prepareObj,
                                                  operationContextFn));
    }

    // Asynchronously aggregate all prepare responses to find the decision and max prepare timestamp
    // (used for commit), stopping the aggregation and preventing any further retries as soon as an
    // abort decision is received. Return a future containing the result.
    return txn::collect(
               std::move(responses),
               // Initial value
               PrepareVoteConsensus{int(participants.size())},
               // Aggregates an incoming response (next) with the existing aggregate value (result)
               [&prepareScheduler = *prepareScheduler, txnNumberAndRetryCounter](
                   PrepareVoteConsensus& result, const PrepareResponse& next) {
                   result.registerVote(next);

                   if (next.vote == PrepareVote::kAbort) {
                       LOGV2_DEBUG(5141701,
                                   1,
                                   "Received abort prepare vote from node",
                                   "shardId"_attr = next.shardId,
                                   "txnNumberAndRetryCounter"_attr = txnNumberAndRetryCounter,
                                   "error"_attr = (next.abortReason.has_value()
                                                       ? next.abortReason.value().reason()
                                                       : ""));
                       prepareScheduler.shutdown(
                           {ErrorCodes::TransactionCoordinatorReachedAbortDecision,
                            str::stream() << "Received abort vote from " << next.shardId});
                   }

                   return txn::ShouldStopIteration::kNo;
               })
        .tapAll([prepareScheduler = std::move(prepareScheduler)](auto&& unused) mutable {
            // Destroy the prepare scheduler before the rest of the future chain has executed so
            // that any parent schedulers can be destroyed without dangling children
            prepareScheduler.reset();
        });
}

namespace {
repl::OpTime persistDecisionBlocking(OperationContext* opCtx,
                                     const LogicalSessionId& lsid,
                                     const TxnNumberAndRetryCounter& txnNumberAndRetryCounter,
                                     std::vector<ShardId> participantList,
                                     const txn::CoordinatorCommitDecision& decision,
                                     std::vector<NamespaceString> affectedNamespaces) {
    const bool isCommit = decision.getDecision() == txn::CommitDecision::kCommit;
    LOGV2_DEBUG(22467,
                3,
                "{sessionId}:{txnNumberAndRetryCounter} Going to write decision {decision}",
                "sessionId"_attr = lsid,
                "txnNumberAndRetryCounter"_attr = txnNumberAndRetryCounter,
                "decision"_attr = (isCommit ? "commit" : "abort"));

    if (MONGO_unlikely(hangBeforeWritingDecision.shouldFail())) {
        LOGV2(22468, "Hit hangBeforeWritingDecision failpoint");
        hangBeforeWritingDecision.pauseWhileSet(opCtx);
    }

    OperationSessionInfo sessionInfo;
    sessionInfo.setSessionId(lsid);
    sessionInfo.setTxnNumber(txnNumberAndRetryCounter.getTxnNumber());
    if (auto txnRetryCounter = txnNumberAndRetryCounter.getTxnRetryCounter();
        txnRetryCounter && !isDefaultTxnRetryCounter(*txnRetryCounter)) {
        sessionInfo.setTxnRetryCounter(*txnNumberAndRetryCounter.getTxnRetryCounter());
    }

    DBDirectClient client(opCtx);

    // Throws if serializing the request or deserializing the response fails.
    const auto commandResponse = client.runCommand([&] {
        write_ops::UpdateCommandRequest updateOp(
            NamespaceString::kTransactionCoordinatorsNamespace);
        updateOp.setUpdates({[&] {
            write_ops::UpdateOpEntry entry;

            // Ensure that the document for the (lsid, txnNumber) has the same participant list and
            // either has no decision or the same decision. The document may have the same decision
            // if an earlier attempt to write the decision failed waiting for writeConcern.
            BSONObj noDecision = BSON(TransactionCoordinatorDocument::kDecisionFieldName
                                      << BSON("$exists" << false));
            BSONObj sameDecision =
                BSON(TransactionCoordinatorDocument::kDecisionFieldName << decision.toBSON());

            entry.setQ(BSON(TransactionCoordinatorDocument::kIdFieldName
                            << sessionInfo.toBSON() << "$and"
                            << buildParticipantListMatchesConditions(participantList) << "$or"
                            << BSON_ARRAY(noDecision << sameDecision)));

            entry.setUpsert(true);
            entry.setU(write_ops::UpdateModification::parseFromClassicUpdate([&] {
                TransactionCoordinatorDocument doc;
                doc.setId(sessionInfo);
                doc.setParticipants(std::move(participantList));
                doc.setDecision(decision);
                if (decision.getDecision() == CommitDecision::kCommit &&
                    feature_flags::gFeatureFlagEndOfTransactionChangeEvent.isEnabled(
                        serverGlobalParams.featureCompatibility.acquireFCVSnapshot())) {
                    doc.setAffectedNamespaces(std::move(affectedNamespaces));
                }
                return doc.toBSON();
            }()));

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
            NamespaceString::kTransactionCoordinatorsNamespace,
            BSON(TransactionCoordinatorDocument::kIdFieldName << sessionInfo.toBSON()));
        uasserted(51026,
                  str::stream() << "While attempting to write decision "
                                << (isCommit ? "'commit'" : "'abort'") << " for" << lsid.getId()
                                << ':' << txnNumberAndRetryCounter.toBSON()
                                << ", either failed to find document for this lsid:txnNumber or "
                                   "document existed with a different participant list, decision "
                                   "or commitTimestamp: "
                                << doc);
    }

    LOGV2_DEBUG(22469,
                3,
                "Wrote decision",
                "sessionId"_attr = lsid,
                "txnNumberAndRetryCounter"_attr = txnNumberAndRetryCounter,
                "decision"_attr = (isCommit ? "commit" : "abort"));

    return repl::ReplClientInfo::forClient(opCtx->getClient()).getLastOp();
}
}  // namespace

Future<repl::OpTime> persistDecision(txn::AsyncWorkScheduler& scheduler,
                                     const LogicalSessionId& lsid,
                                     const TxnNumberAndRetryCounter& txnNumberAndRetryCounter,
                                     const txn::ParticipantsList& participants,
                                     const txn::CoordinatorCommitDecision& decision,
                                     const std::vector<NamespaceString>& affectedNamespaces) {
    return txn::doWhile(
        scheduler,
        boost::none /* no need for a backoff */,
        [](const StatusWith<repl::OpTime>& s) { return shouldRetryPersistingCoordinatorState(s); },
        [&scheduler, lsid, txnNumberAndRetryCounter, participants, decision, affectedNamespaces] {
            return scheduler.scheduleWork([lsid,
                                           txnNumberAndRetryCounter,
                                           participants = participants,
                                           decision,
                                           affectedNamespaces = affectedNamespaces](
                                              OperationContext* opCtx) mutable {
                // Do not acquire a storage ticket in order to avoid unnecessary serialization
                // with other prepared transactions that are holding a storage ticket
                // themselves; see SERVER-60682.
                ScopedAdmissionPriorityForLock setTicketAquisition(
                    shard_role_details::getLocker(opCtx), AdmissionContext::Priority::kImmediate);
                getTransactionCoordinatorWorkerCurOpRepository()->set(
                    opCtx, lsid, txnNumberAndRetryCounter, CoordinatorAction::kWritingDecision);
                return persistDecisionBlocking(opCtx,
                                               lsid,
                                               txnNumberAndRetryCounter,
                                               std::move(participants),
                                               decision,
                                               std::move(affectedNamespaces));
            });
        });
}

Future<void> sendCommit(ServiceContext* service,
                        txn::AsyncWorkScheduler& scheduler,
                        const LogicalSessionId& lsid,
                        const TxnNumberAndRetryCounter& txnNumberAndRetryCounter,
                        const APIParameters& apiParams,
                        const txn::ParticipantsList& participants,
                        Timestamp commitTimestamp) {
    CommitTransaction commitTransaction;
    commitTransaction.setDbName(DatabaseName::kAdmin);
    commitTransaction.setCommitTimestamp(commitTimestamp);
    BSONObjBuilder bob(BSON("lsid" << lsid.toBSON() << "txnNumber"
                                   << txnNumberAndRetryCounter.getTxnNumber() << "autocommit"
                                   << false << WriteConcernOptions::kWriteConcernField
                                   << WriteConcernOptions::Majority));
    if (auto txnRetryCounter = txnNumberAndRetryCounter.getTxnRetryCounter();
        txnRetryCounter && !isDefaultTxnRetryCounter(*txnRetryCounter)) {
        bob.append(OperationSessionInfoFromClient::kTxnRetryCounterFieldName, *txnRetryCounter);
    }
    apiParams.appendInfo(&bob);
    auto commitObj = commitTransaction.toBSON(bob.obj());

    OperationContextFn operationContextFn = [lsid,
                                             txnNumberAndRetryCounter](OperationContext* opCtx) {
        invariant(opCtx);
        getTransactionCoordinatorWorkerCurOpRepository()->set(
            opCtx, lsid, txnNumberAndRetryCounter, CoordinatorAction::kSendingCommit);

        if (MONGO_unlikely(hangBeforeSendingCommit.shouldFail())) {
            LOGV2(22470, "Hit hangBeforeSendingCommit failpoint");
            hangBeforeSendingCommit.pauseWhileSet(opCtx);
        }
    };

    std::vector<Future<void>> responses;
    for (const auto& participant : participants) {
        responses.push_back(sendDecisionToShard(service,
                                                scheduler,
                                                lsid,
                                                txnNumberAndRetryCounter,
                                                participant,
                                                commitObj,
                                                operationContextFn));
    }
    return txn::whenAll(responses);
}

Future<void> sendAbort(ServiceContext* service,
                       txn::AsyncWorkScheduler& scheduler,
                       const LogicalSessionId& lsid,
                       const TxnNumberAndRetryCounter& txnNumberAndRetryCounter,
                       const APIParameters& apiParams,
                       const txn::ParticipantsList& participants) {
    AbortTransaction abortTransaction;
    abortTransaction.setDbName(DatabaseName::kAdmin);
    BSONObjBuilder bob(BSON("lsid" << lsid.toBSON() << "txnNumber"
                                   << txnNumberAndRetryCounter.getTxnNumber() << "autocommit"
                                   << false << WriteConcernOptions::kWriteConcernField
                                   << WriteConcernOptions::Majority));
    if (auto txnRetryCounter = txnNumberAndRetryCounter.getTxnRetryCounter();
        txnRetryCounter && !isDefaultTxnRetryCounter(*txnRetryCounter)) {
        bob.append(OperationSessionInfoFromClient::kTxnRetryCounterFieldName, *txnRetryCounter);
    }
    apiParams.appendInfo(&bob);
    auto abortObj = abortTransaction.toBSON(bob.obj());

    OperationContextFn operationContextFn = [lsid,
                                             txnNumberAndRetryCounter](OperationContext* opCtx) {
        invariant(opCtx);
        getTransactionCoordinatorWorkerCurOpRepository()->set(
            opCtx, lsid, txnNumberAndRetryCounter, CoordinatorAction::kSendingAbort);

        if (MONGO_unlikely(hangBeforeSendingAbort.shouldFail())) {
            LOGV2(22471, "Hit hangBeforeSendingAbort failpoint");
            hangBeforeSendingAbort.pauseWhileSet(opCtx);
        }
    };

    std::vector<Future<void>> responses;
    for (const auto& participant : participants) {
        responses.push_back(sendDecisionToShard(service,
                                                scheduler,
                                                lsid,
                                                txnNumberAndRetryCounter,
                                                participant,
                                                abortObj,
                                                operationContextFn));
    }
    return txn::whenAll(responses);
}

namespace {
void deleteCoordinatorDocBlocking(OperationContext* opCtx,
                                  const LogicalSessionId& lsid,
                                  const TxnNumberAndRetryCounter& txnNumberAndRetryCounter) {
    LOGV2_DEBUG(22472,
                3,
                "Going to delete coordinator doc",
                "sessionId"_attr = lsid,
                "txnNumberAndRetryCounter"_attr = txnNumberAndRetryCounter);

    if (MONGO_unlikely(hangBeforeDeletingCoordinatorDoc.shouldFail())) {
        LOGV2(22473, "Hit hangBeforeDeletingCoordinatorDoc failpoint");
        hangBeforeDeletingCoordinatorDoc.pauseWhileSet(opCtx);
    }

    OperationSessionInfo sessionInfo;
    sessionInfo.setSessionId(lsid);
    sessionInfo.setTxnNumber(txnNumberAndRetryCounter.getTxnNumber());
    if (auto txnRetryCounter = txnNumberAndRetryCounter.getTxnRetryCounter();
        txnRetryCounter && !isDefaultTxnRetryCounter(*txnRetryCounter)) {
        sessionInfo.setTxnRetryCounter(*txnNumberAndRetryCounter.getTxnRetryCounter());
    }

    DBDirectClient client(opCtx);

    // Throws if serializing the request or deserializing the response fails.
    auto commandResponse = client.runCommand([&] {
        write_ops::DeleteCommandRequest deleteOp(
            NamespaceString::kTransactionCoordinatorsNamespace);
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
            NamespaceString::kTransactionCoordinatorsNamespace,
            BSON(TransactionCoordinatorDocument::kIdFieldName << sessionInfo.toBSON()));
        uasserted(51027,
                  str::stream() << "While attempting to delete document for " << lsid.getId() << ':'
                                << txnNumberAndRetryCounter.toBSON()
                                << ", either failed to find document for this lsid:txnNumber or "
                                   "document existed without a decision: "
                                << doc);
    }

    LOGV2_DEBUG(22474,
                3,
                "Deleted coordinator doc",
                "sessionId"_attr = lsid,
                "txnNumberAndRetryCounter"_attr = txnNumberAndRetryCounter);

    hangAfterDeletingCoordinatorDoc.execute([&](const BSONObj& data) {
        LOGV2(22475, "Hit hangAfterDeletingCoordinatorDoc failpoint");
        if (!data["useUninterruptibleSleep"].eoo()) {
            hangAfterDeletingCoordinatorDoc.pauseWhileSet();
        } else {
            hangAfterDeletingCoordinatorDoc.pauseWhileSet(opCtx);
        }
    });
}
}  // namespace

Future<void> deleteCoordinatorDoc(txn::AsyncWorkScheduler& scheduler,
                                  const LogicalSessionId& lsid,
                                  const TxnNumberAndRetryCounter& txnNumberAndRetryCounter) {
    return txn::doWhile(
        scheduler,
        boost::none /* no need for a backoff */,
        [](const Status& s) { return s == ErrorCodes::Interrupted; },
        [&scheduler, lsid, txnNumberAndRetryCounter] {
            return scheduler.scheduleWork(
                [lsid, txnNumberAndRetryCounter](OperationContext* opCtx) {
                    getTransactionCoordinatorWorkerCurOpRepository()->set(
                        opCtx,
                        lsid,
                        txnNumberAndRetryCounter,
                        CoordinatorAction::kDeletingCoordinatorDoc);
                    deleteCoordinatorDocBlocking(opCtx, lsid, txnNumberAndRetryCounter);
                });
        });
}

std::vector<TransactionCoordinatorDocument> readAllCoordinatorDocs(OperationContext* opCtx) {
    std::vector<TransactionCoordinatorDocument> allCoordinatorDocs;

    DBDirectClient client(opCtx);
    FindCommandRequest findRequest{NamespaceString::kTransactionCoordinatorsNamespace};
    auto coordinatorDocsCursor = client.find(std::move(findRequest));

    while (coordinatorDocsCursor->more()) {
        // TODO (SERVER-38307): Try/catch around parsing the document and skip the document if it
        // fails to parse.
        auto nextDecision = TransactionCoordinatorDocument::parse(IDLParserContext(""),
                                                                  coordinatorDocsCursor->next());
        allCoordinatorDocs.push_back(nextDecision);
    }

    return allCoordinatorDocs;
}

Future<PrepareResponse> sendPrepareToShard(ServiceContext* service,
                                           txn::AsyncWorkScheduler& scheduler,
                                           const LogicalSessionId& lsid,
                                           const TxnNumberAndRetryCounter& txnNumberAndRetryCounter,
                                           const ShardId& shardId,
                                           const BSONObj& commandObj,
                                           OperationContextFn operationContextFn) {
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
        [&scheduler,
         lsid,
         txnNumberAndRetryCounter,
         shardId,
         isLocalShard,
         commandObj = commandObj.getOwned(),
         operationContextFn] {
            LOGV2_DEBUG(22476,
                        3,
                        "Coordinator going to send command to shard",
                        "sessionId"_attr = lsid,
                        "txnNumberAndRetryCounter"_attr = txnNumberAndRetryCounter,
                        "command"_attr = commandObj,
                        "localOrRemote"_attr = (isLocalShard ? "local" : "remote"),
                        "shardId"_attr = shardId);

            return scheduler
                .scheduleRemoteCommand(
                    shardId, kPrimaryReadPreference, commandObj, operationContextFn)
                .then([lsid, txnNumberAndRetryCounter, shardId, commandObj = commandObj.getOwned()](
                          ResponseStatus response) {
                    auto status = getStatusFromCommandResult(response.data);
                    auto wcStatus = getWriteConcernStatusFromCommandResult(response.data);

                    // There must be no writeConcern error in order for us to interpret the command
                    // response.
                    if (!wcStatus.isOK()) {
                        status = wcStatus;
                    }

                    if (status.isOK()) {
                        auto reply =
                            PrepareReply::parse(IDLParserContext("PrepareReply"), response.data);
                        if (!reply.getPrepareTimestamp()) {
                            Status abortStatus(ErrorCodes::Error(50993),
                                               str::stream()
                                                   << "Coordinator shard received an OK response "
                                                      "to prepareTransaction without a "
                                                      "prepareTimestamp from shard "
                                                   << shardId
                                                   << ", which is not an expected behavior. "
                                                      "Interpreting the response as vote to abort");
                            LOGV2(22477,
                                  "Coordinator received error from transaction participant",
                                  "sessionId"_attr = lsid,
                                  "txnNumberAndRetryCounter"_attr = txnNumberAndRetryCounter,
                                  "error"_attr = redact(abortStatus));

                            return PrepareResponse{
                                shardId, PrepareVote::kAbort, boost::none, {}, abortStatus};
                        }

                        LOGV2_DEBUG(
                            22478,
                            3,
                            "Coordinator shard received a vote to commit from participant shard",
                            "sessionId"_attr = lsid,
                            "txnNumberAndRetryCounter"_attr = txnNumberAndRetryCounter,
                            "shardId"_attr = shardId,
                            "prepareTimestamp"_attr = reply.getPrepareTimestamp(),
                            "affectedNamespaces"_attr = reply.getAffectedNamespaces());

                        return PrepareResponse{
                            shardId,
                            PrepareVote::kCommit,
                            *reply.getPrepareTimestamp(),
                            reply.getAffectedNamespaces().value_or(std::vector<NamespaceString>{}),
                            boost::none};
                    }

                    LOGV2_DEBUG(22479,
                                3,
                                "Coordinator shard received response from shard",
                                "sessionId"_attr = lsid,
                                "txnNumberAndRetryCounter"_attr = txnNumberAndRetryCounter,
                                "error"_attr = status,
                                "shardId"_attr = shardId,
                                "command"_attr = commandObj);

                    if (ErrorCodes::isVoteAbortError(status.code())) {
                        return PrepareResponse{
                            shardId,
                            PrepareVote::kAbort,
                            boost::none,
                            {},
                            status.withContext(str::stream() << "from shard " << shardId)};
                    }

                    uassertStatusOK(status);
                    MONGO_UNREACHABLE;
                })
                .onError<ErrorCodes::ShardNotFound>([shardId, isLocalShard](const Status& status) {
                    invariant(!isLocalShard);
                    // ShardNotFound may indicate that the participant shard has been removed (it
                    // could also mean the participant shard was recently added and this node
                    // refreshed its ShardRegistry from a stale config secondary).
                    //
                    // Since this node can't know which is the case, it is safe to pessimistically
                    // treat ShardNotFound as a vote to abort, which is always safe since the node
                    // must then send abort.
                    return Future<PrepareResponse>::makeReady(
                        {shardId, CommitDecision::kAbort, boost::none, {}, status});
                });
        });

    return std::move(f).onError<ErrorCodes::TransactionCoordinatorReachedAbortDecision>(
        [lsid, txnNumberAndRetryCounter, shardId](const Status& status) {
            LOGV2_DEBUG(22480,
                        3,
                        "Prepare stopped retrying due to retrying being cancelled",
                        "sessionId"_attr = lsid,
                        "txnNumberAndRetryCounter"_attr = txnNumberAndRetryCounter);
            return PrepareResponse{shardId,
                                   boost::none,
                                   boost::none,
                                   {},
                                   Status(ErrorCodes::NoSuchTransaction, status.reason())};
        });
}

Future<void> sendDecisionToShard(ServiceContext* service,
                                 txn::AsyncWorkScheduler& scheduler,
                                 const LogicalSessionId& lsid,
                                 const TxnNumberAndRetryCounter& txnNumberAndRetryCounter,
                                 const ShardId& shardId,
                                 const BSONObj& commandObj,
                                 OperationContextFn operationContextFn) {
    const bool isLocalShard = (shardId == txn::getLocalShardId(service));

    return txn::doWhile(
        scheduler,
        kExponentialBackoff,
        [](const Status& s) {
            // *Always* retry until hearing a conclusive response or being told to stop via a
            // coordinator-specific code.
            return !s.isOK() && s != ErrorCodes::TransactionCoordinatorSteppingDown;
        },
        [&scheduler,
         lsid,
         txnNumberAndRetryCounter,
         shardId,
         isLocalShard,
         operationContextFn,
         commandObj = commandObj.getOwned()] {
            LOGV2_DEBUG(22481,
                        3,
                        "Coordinator going to send command to shard",
                        "sessionId"_attr = lsid,
                        "txnNumberAndRetryCounter"_attr = txnNumberAndRetryCounter,
                        "command"_attr = commandObj,
                        "localOrRemote"_attr = (isLocalShard ? "local" : "remote"),
                        "shardId"_attr = shardId);

            return scheduler
                .scheduleRemoteCommand(
                    shardId, kPrimaryReadPreference, commandObj, operationContextFn)
                .then([lsid, txnNumberAndRetryCounter, shardId, commandObj = commandObj.getOwned()](
                          ResponseStatus response) {
                    auto status = getStatusFromCommandResult(response.data);
                    auto wcStatus = getWriteConcernStatusFromCommandResult(response.data);

                    // There must be no writeConcern error in order for us to interpret the command
                    // response.
                    if (!wcStatus.isOK()) {
                        status = wcStatus;
                    }

                    LOGV2_DEBUG(22482,
                                3,
                                "Coordinator shard received response from shard",
                                "sessionId"_attr = lsid,
                                "txnNumberAndRetryCounter"_attr = txnNumberAndRetryCounter,
                                "status"_attr = status,
                                "command"_attr = commandObj,
                                "shardId"_attr = shardId);

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

Future<void> writeEndOfTransaction(txn::AsyncWorkScheduler& scheduler,
                                   const LogicalSessionId& lsid,
                                   const TxnNumberAndRetryCounter& txnNumberAndRetryCounter,
                                   const std::vector<NamespaceString>& affectedNamespaces) {
    if (MONGO_unlikely(hangBeforeWritingEndOfTransaction.shouldFail())) {
        LOGV2(8288302, "Hit hangBeforeWritingEndOfTransaction failpoint");
        hangBeforeWritingEndOfTransaction.pauseWhileSet();
    }

    if (!feature_flags::gFeatureFlagEndOfTransactionChangeEvent.isEnabled(
            serverGlobalParams.featureCompatibility.acquireFCVSnapshot())) {
        return Future<void>::makeReady();
    }
    return scheduler.scheduleWork(
        [lsid, txnNumberAndRetryCounter, affectedNamespaces](OperationContext* opCtx) {
            getTransactionCoordinatorWorkerCurOpRepository()->set(
                opCtx, lsid, txnNumberAndRetryCounter, CoordinatorAction::kWritingEndOfTransaction);
            notifyChangeStreamOnEndOfTransaction(
                opCtx, lsid, txnNumberAndRetryCounter.getTxnNumber(), affectedNamespaces);
        });
}

std::string txnIdToString(const LogicalSessionId& lsid,
                          const TxnNumberAndRetryCounter& txnNumberAndRetryCounter) {
    str::stream ss;
    ss << lsid.getId() << ':' << txnNumberAndRetryCounter.getTxnNumber();
    if (auto retryCounter = txnNumberAndRetryCounter.getTxnRetryCounter()) {
        ss << ':' << *retryCounter;
    }
    return ss;
}

}  // namespace txn
}  // namespace mongo
