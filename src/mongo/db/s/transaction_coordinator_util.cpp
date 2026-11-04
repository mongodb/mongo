// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


#include "mongo/db/s/transaction_coordinator_util.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/status_with.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/util/builder.h"
#include "mongo/bson/util/builder_fwd.h"
#include "mongo/client/dbclient_cursor.h"
#include "mongo/client/read_preference.h"
#include "mongo/db/admission/execution_control/execution_admission_context.h"
#include "mongo/db/admission/ticketing/admission_context.h"
#include "mongo/db/commands/txn_cmds_gen.h"
#include "mongo/db/commands/txn_two_phase_commit_cmds_gen.h"
#include "mongo/db/database_name.h"
#include "mongo/db/database_name_util.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/end_of_transaction_parameter.h"
#include "mongo/db/generic_argument_util.h"
#include "mongo/db/namespace_string_util.h"
#include "mongo/db/query/collation/collator_factory_interface.h"
#include "mongo/db/query/find_command.h"
#include "mongo/db/query/write_ops/write_ops_gen.h"
#include "mongo/db/query/write_ops/write_ops_parsers.h"
#include "mongo/db/repl/change_stream_oplog_notification.h"
#include "mongo/db/repl/repl_client_info.h"
#include "mongo/db/s/transaction_coordinator_futures_util.h"
#include "mongo/db/s/transaction_coordinator_worker_curop_repository.h"
#include "mongo/db/server_feature_flags_gen.h"
#include "mongo/db/server_options.h"
#include "mongo/db/session/logical_session_id_helpers.h"
#include "mongo/db/shard_role/transaction_resources.h"
#include "mongo/db/write_concern_options.h"
#include "mongo/executor/task_executor.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/logv2/log.h"
#include "mongo/logv2/log_severity_suppressor.h"
#include "mongo/platform/compiler.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/rpc/reply_interface.h"
#include "mongo/rpc/unique_message.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/decorable.h"
#include "mongo/util/duration.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/future_impl.h"
#include "mongo/util/str.h"
#include "mongo/util/time_support.h"
#include "mongo/util/uuid.h"

#include <algorithm>
#include <cstdint>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <type_traits>

#include <absl/container/flat_hash_set.h>
#include <boost/cstdint.hpp>
#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

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
        return updateOp.serialize();
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
            return scheduler.scheduleWork(
                [lsid, txnNumberAndRetryCounter, participants](OperationContext* opCtx) {
                    // Skip ticket acquisition in order to prevent possible deadlock when
                    // participants are in the prepared state. See SERVER-82883 and SERVER-60682.
                    ScopedAdmissionPriority<ExecutionAdmissionContext> skipTicketAcquisition(
                        opCtx, AdmissionContext::Priority::kExempt);
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
        // Only record the per-shard namespaces when the end-of-transaction write path is enabled,
        // since they are only used to build the accounting metadata. Read-only participants (no
        // affected namespaces) are excluded from the participant sets.
        if (isEndOfTransactionWritePathEnabled() && !vote.affectedNamespaces.empty()) {
            _namespacesByShard[vote.shardId] = vote.affectedNamespaces;
        }
    } else {
        vote.vote == PrepareVote::kAbort ? ++_numAbortVotes : ++_numNoVotes;

        if (!_abortStatus)
            _abortStatus.emplace(*vote.abortReason);
    }
}

namespace {
// The end-of-transaction accounting metadata is serialized as a sub-document of the coordinator's
// commit decision document (config.transactionCoordinators) and of every participant's
// commitTransaction command, so an unbounded one could produce a document or command that can never
// be written or sent, leaving the transaction (and its locks) stuck forever. Both enclosing
// documents are bounded by BSONObjMaxUserSize, so cap the metadata at that size minus a margin for
// the fields they hold besides the metadata.
//
// The margin is derived from the other fields of the two enclosing documents. In the
// commitTransaction command they are all scalars (commitTransaction, commitTimestamp, count and the
// session/transaction envelope), well below 1 KB. In the coordinator document the largest sibling
// field is 'participants' (one shard id, ~20 bytes, per participant), which stays below 100 KB even
// for a cluster with several thousand shards; 'affectedNamespaces' mirrors the metadata's
// namespaces but stores each namespace once without the shard lists, so it is smaller per
// namespace. A 1 MB margin therefore covers every other field with room to spare, matching the
// padding used for the analyzeShardKey response (kMaxBSONObjSizeMostCommonValues).
constexpr int kEndOfTransactionMetadataMarginBytes = 1000 * 1024;
constexpr int kMaxEndOfTransactionMetadataBytes =
    BSONObjMaxUserSize - kEndOfTransactionMetadataMarginBytes;

// The resulting ~15 MB limit is effectively unreachable in real-world use: the metadata only grows
// with the number of distinct namespaces a single transaction writes to, and the shards that wrote
// each one. Saturating it would require a single transaction to write to several hundred thousand
// distinct collections, which no real workload does. The check is a safety net against a
// pathological transaction, not a limit users are expected to hit.

// Serializes a map from key to a set of shard ids into a BSON object with sorted shard-id arrays.
BSONObj shardSetsToBSON(const std::map<std::string, std::set<ShardId>>& shardSets) {
    BSONObjBuilder builder;
    for (const auto& [key, shards] : shardSets) {
        BSONArrayBuilder array(builder.subarrayStart(key));
        for (const auto& shard : shards) {
            array << shard.toString();
        }
    }
    return builder.obj();
}
}  // namespace

EndOfTransactionScopesInfoBase PrepareVoteConsensus::releaseEndOfTransactionMetadata() {
    EndOfTransactionScopesInfoBase metadata;

    std::vector<ShardId> clusterParticipants;
    clusterParticipants.reserve(_namespacesByShard.size());
    std::map<std::string, std::set<ShardId>> collectionParticipants;
    std::map<std::string, std::set<ShardId>> databaseParticipants;
    for (const auto& [shardId, namespaces] : _namespacesByShard) {
        clusterParticipants.push_back(shardId);
        for (const auto& nss : namespaces) {
            collectionParticipants[NamespaceStringUtil::serialize(
                                       nss, SerializationContext::stateDefault())]
                .insert(shardId);
            databaseParticipants[DatabaseNameUtil::serialize(nss.dbName(),
                                                             SerializationContext::stateDefault())]
                .insert(shardId);
        }
    }

    metadata.setCollectionParticipants(shardSetsToBSON(collectionParticipants));
    metadata.setDatabaseParticipants(shardSetsToBSON(databaseParticipants));
    metadata.setClusterParticipants(std::move(clusterParticipants));

    // Serialize the metadata to measure its size.
    const auto metadataSize = metadata.toBSON().objsize();
    if (metadataSize > kMaxEndOfTransactionMetadataBytes) {
        // Drop the metadata rather than persist or send something that cannot fit. Change streams
        // then simply cannot synthesize an endOfTransaction event for this transaction.
        LOGV2_WARNING(13790400,
                      "Dropping oversized end-of-transaction accounting metadata",
                      "metadataSizeBytes"_attr = metadataSize,
                      "maxSizeBytes"_attr = kMaxEndOfTransactionMetadataBytes);
        return EndOfTransactionScopesInfoBase{};
    }
    return metadata;
}

bool isEndOfTransactionMetadataConsistentWithParticipants(
    const EndOfTransactionScopesInfoBase& metadata, const std::vector<ShardId>& participants) {
    absl::flat_hash_set<ShardId> participantSet(participants.begin(), participants.end());
    absl::flat_hash_set<ShardId> clusterParticipantSet;
    for (const auto& shardId : metadata.getClusterParticipants()) {
        if (!participantSet.contains(shardId)) {
            return false;
        }
        clusterParticipantSet.insert(shardId);
    }

    // The collection and database participant maps must be well-formed objects whose arrays contain
    // only shard ids that are themselves cluster participants.
    auto isMapConsistent = [&clusterParticipantSet](const BSONObj& map) {
        for (const auto& [key, value] : map) {
            if (value.type() != BSONType::array) {
                return false;
            }
            for (const auto& shardElement : value.Obj()) {
                if (shardElement.type() != BSONType::string ||
                    !clusterParticipantSet.contains(
                        ShardId(std::string(shardElement.valueStringData())))) {
                    return false;
                }
            }
        }
        return true;
    };

    return isMapConsistent(metadata.getCollectionParticipants()) &&
        isMapConsistent(metadata.getDatabaseParticipants());
}

CoordinatorCommitDecision PrepareVoteConsensus::decision() const {
    invariant(_numShards == _numCommitVotes + _numAbortVotes + _numNoVotes,
              str::stream() << "numShards: " << _numShards << ", numCommitVotes: "
                            << _numCommitVotes << ", numAbortVotes: " << _numAbortVotes
                            << ", numNoVotes: " << _numNoVotes);

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
    prepareTransaction.setLsid(generic_argument_util::toLogicalSessionFromClient(lsid));
    prepareTransaction.setTxnNumber(txnNumberAndRetryCounter.getTxnNumber());
    prepareTransaction.setAutocommit(false);
    prepareTransaction.setWriteConcern(defaultMajorityWriteConcernDoNotUse());
    if (auto txnRetryCounter = txnNumberAndRetryCounter.getTxnRetryCounter();
        txnRetryCounter && !isDefaultTxnRetryCounter(*txnRetryCounter)) {
        prepareTransaction.setTxnRetryCounter(*txnRetryCounter);
    }
    apiParams.setInfo(prepareTransaction);
    auto prepareObj = prepareTransaction.toBSON();

    std::vector<Future<PrepareResponse>> responses;
    responses.reserve(participants.size());

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
                                     std::vector<NamespaceString> affectedNamespaces,
                                     const EndOfTransactionScopesInfoBase& endOfTransactionScopes) {
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
                // The caller decides whether the accounting metadata is part of the decision by
                // passing a non-empty scopes object. Do not re-read the kill switch here, so the
                // persisted document always matches what the coordinator sends to the participants.
                if (decision.getDecision() == CommitDecision::kCommit &&
                    !endOfTransactionScopes.getClusterParticipants().empty()) {
                    doc.setEndOfTransactionScopes(endOfTransactionScopes);
                }
                return doc.toBSON();
            }()));

            return entry;
        }()});
        return updateOp.serialize();
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
                                     const std::vector<NamespaceString>& affectedNamespaces,
                                     const EndOfTransactionScopesInfoBase& endOfTransactionScopes) {
    return txn::doWhile(
        scheduler,
        boost::none /* no need for a backoff */,
        [](const StatusWith<repl::OpTime>& s) { return shouldRetryPersistingCoordinatorState(s); },
        [&scheduler,
         lsid,
         txnNumberAndRetryCounter,
         participants,
         decision,
         affectedNamespaces,
         endOfTransactionScopes] {
            return scheduler.scheduleWork(
                [lsid,
                 txnNumberAndRetryCounter,
                 participants = participants,
                 decision,
                 affectedNamespaces,
                 endOfTransactionScopes](OperationContext* opCtx) mutable {
                    // Do not acquire a storage ticket in order to avoid unnecessary serialization
                    // with other prepared transactions that are holding a storage ticket
                    // themselves; see SERVER-60682.
                    ScopedAdmissionPriority<ExecutionAdmissionContext> setTicketAquisition(
                        opCtx, AdmissionContext::Priority::kExempt);
                    getTransactionCoordinatorWorkerCurOpRepository()->set(
                        opCtx, lsid, txnNumberAndRetryCounter, CoordinatorAction::kWritingDecision);
                    return persistDecisionBlocking(opCtx,
                                                   lsid,
                                                   txnNumberAndRetryCounter,
                                                   std::move(participants),
                                                   decision,
                                                   std::move(affectedNamespaces),
                                                   endOfTransactionScopes);
                });
        });
}

Future<void> sendCommit(ServiceContext* service,
                        txn::AsyncWorkScheduler& scheduler,
                        const LogicalSessionId& lsid,
                        const TxnNumberAndRetryCounter& txnNumberAndRetryCounter,
                        const APIParameters& apiParams,
                        const txn::ParticipantsList& participants,
                        Timestamp commitTimestamp,
                        const EndOfTransactionScopesInfoBase& endOfTransactionScopes) {
    CommitTransaction commitTransaction;
    commitTransaction.setDbName(DatabaseName::kAdmin);
    commitTransaction.setCommitTimestamp(commitTimestamp);
    commitTransaction.setLsid(generic_argument_util::toLogicalSessionFromClient(lsid));
    commitTransaction.setTxnNumber(txnNumberAndRetryCounter.getTxnNumber());
    commitTransaction.setAutocommit(false);
    commitTransaction.setWriteConcern(defaultMajorityWriteConcernDoNotUse());
    if (auto txnRetryCounter = txnNumberAndRetryCounter.getTxnRetryCounter();
        txnRetryCounter && !isDefaultTxnRetryCounter(*txnRetryCounter)) {
        commitTransaction.setTxnRetryCounter(*txnRetryCounter);
    }
    apiParams.setInfo(commitTransaction);
    auto commitObj = commitTransaction.toBSON();

    // The caller decides whether the accounting metadata is part of the durable decision by passing
    // a non-empty scopes object. Do not re-read the kill switch here, so the commands sent to the
    // participants always match the persisted decision.
    const bool haveEndOfTransactionScopes =
        !endOfTransactionScopes.getClusterParticipants().empty();

    // The metadata is attached only to write participants. Read-only participants (which prepare
    // and commit an empty transaction) have no transaction changes to order, so their commit
    // entries are left un-augmented.
    const auto& writeParticipants = endOfTransactionScopes.getClusterParticipants();
    absl::flat_hash_set<ShardId> writeParticipantSet;
    if (haveEndOfTransactionScopes) {
        writeParticipantSet.insert(writeParticipants.begin(), writeParticipants.end());
    }

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
    responses.reserve(participants.size());

    for (const auto& participant : participants) {
        BSONObj commandObj = commitObj;
        boost::optional<BSONObj> commandObjWithoutEndOfTransactionScopes;

        // Attach the metadata only to write participants.
        if (haveEndOfTransactionScopes && writeParticipantSet.contains(participant)) {
            EndOfTransactionScopesInfo scopes;
            scopes.setBase(endOfTransactionScopes);
            scopes.setShardId(participant);

            BSONObjBuilder builder;
            builder.appendElements(commitObj);
            {
                BSONObjBuilder scopesBuilder(
                    builder.subobjStart(CommitTransaction::kEndOfTransactionScopesFieldName));
                scopes.serialize(&scopesBuilder);
            }
            commandObj = builder.obj();
            commandObjWithoutEndOfTransactionScopes = commitObj;
        }

        responses.push_back(
            sendDecisionToShard(service,
                                scheduler,
                                lsid,
                                txnNumberAndRetryCounter,
                                participant,
                                commandObj,
                                operationContextFn,
                                std::move(commandObjWithoutEndOfTransactionScopes)));
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
    abortTransaction.setLsid(generic_argument_util::toLogicalSessionFromClient(lsid));
    abortTransaction.setTxnNumber(txnNumberAndRetryCounter.getTxnNumber());
    abortTransaction.setAutocommit(false);
    abortTransaction.setWriteConcern(defaultMajorityWriteConcernDoNotUse());
    if (auto txnRetryCounter = txnNumberAndRetryCounter.getTxnRetryCounter();
        txnRetryCounter && !isDefaultTxnRetryCounter(*txnRetryCounter)) {
        abortTransaction.setTxnRetryCounter(*txnRetryCounter);
    }
    apiParams.setInfo(abortTransaction);
    auto abortObj = abortTransaction.toBSON();

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
    responses.reserve(participants.size());

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
        // Use w: 1 since the delete is meant to be best-effort to avoid blocking the remaining
        // steps in the TransactionCoordinator. If the write gets rolled back, the next primary
        // would resume the coordinator with existing decision which would run to completion
        // immediately and try to delete the doc again.
        // TODO (SERVER-120584): Re-evaluate if the deletion of transaction coordinator doc should
        // continue to use w: 1.
        deleteOp.setWriteConcern(WriteConcernOptions(/*numNodes=*/1,
                                                     WriteConcernOptions::SyncMode::UNSET,
                                                     WriteConcernOptions::Timeout::kNoTimeoutVal));
        return deleteOp.serialize();
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
                    // Skip ticket acquisition to avoid a bottleneck on deleting the coordinator doc
                    // in cases of high transaction concurrency.
                    ScopedAdmissionPriority<ExecutionAdmissionContext> skipTicketAcquisition(
                        opCtx, AdmissionContext::Priority::kExempt);
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
        auto nextDecision = TransactionCoordinatorDocument::parse(coordinatorDocsCursor->next(),
                                                                  IDLParserContext(""));
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
                            PrepareReply::parse(response.data, IDLParserContext("PrepareReply"));
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
                                 OperationContextFn operationContextFn,
                                 boost::optional<BSONObj> commandObjWithoutEndOfTransactionScopes) {
    const bool isLocalShard = (shardId == txn::getLocalShardId(service));

    // Shared between the loop body (which reads the command to send) and the response handler
    // (which may replace it) so that the metadata can be stripped across a retry.
    auto currentCommand = std::make_shared<BSONObj>(commandObj.getOwned());
    auto commandWithoutMetadata = std::make_shared<boost::optional<BSONObj>>(
        std::move(commandObjWithoutEndOfTransactionScopes));

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
         service,
         currentCommand,
         commandWithoutMetadata] {
            LOGV2_DEBUG(22481,
                        3,
                        "Coordinator going to send command to shard",
                        "sessionId"_attr = lsid,
                        "txnNumberAndRetryCounter"_attr = txnNumberAndRetryCounter,
                        "command"_attr = *currentCommand,
                        "localOrRemote"_attr = (isLocalShard ? "local" : "remote"),
                        "shardId"_attr = shardId);

            return scheduler
                .scheduleRemoteCommand(
                    shardId, kPrimaryReadPreference, *currentCommand, operationContextFn)
                .then([lsid,
                       txnNumberAndRetryCounter,
                       shardId,
                       service,
                       currentCommand,
                       commandWithoutMetadata](ResponseStatus response) {
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
                                "command"_attr = *currentCommand,
                                "shardId"_attr = shardId);

                    if (ErrorCodes::isTwoPhaseDecisionAckError(status.code())) {
                        // Interpret these errors as a successful ack.
                        LOGV2_DEBUG(
                            10613800,
                            3,
                            "Coordinator shard received an error from a shard which will be "
                            "interpreted as a successful acknowledgment.",
                            "sessionId"_attr = lsid,
                            "txnNumberAndRetryCounter"_attr = txnNumberAndRetryCounter,
                            "status"_attr = status,
                            "command"_attr = *currentCommand,
                            "shardId"_attr = shardId);
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
