// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

/**
 * Microbenchmarks for the CPU and byte cost that end-of-transaction (EOT) accounting metadata adds
 * to the write path of a sharded (two-phase commit) transaction.
 *
 * Every stage is benchmarked separately with the EOT write path off ('eot' = 0, same as master)
 * and on ('eot' = 1). The shape of the transaction is given by:
 *   W: write-candidate shards (shards that write at least one namespace are write participants)
 *   N: distinct namespaces (collections) the transaction writes
 *   k: shards per namespace (2 = sparse, W = dense)
 *   R: extra read-only participant shards (commit fan-out only)
 *
 * Nothing here touches the network or storage. Setup checks run once, untimed, and use invariant
 * so that a misconfigured arm or shape can't produce a flattering row.
 */

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/commands/txn_cmds_gen.h"
#include "mongo/db/database_name.h"
#include "mongo/db/end_of_transaction_parameter.h"
#include "mongo/db/generic_argument_util.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/repl/oplog_entry.h"
#include "mongo/db/repl/optime.h"
#include "mongo/db/s/end_of_transaction_metadata_bm_util.h"
#include "mongo/db/s/end_of_transaction_metadata_shard_cache.h"
#include "mongo/db/s/transaction_coordinator_document_gen.h"
#include "mongo/db/s/transaction_coordinator_util.h"
#include "mongo/db/service_context.h"
#include "mongo/db/session/logical_session_id.h"
#include "mongo/db/session/logical_session_id_helpers.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/rpc/op_msg.h"
#include "mongo/unittest/server_parameter_guard.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/time_support.h"

#include <vector>

#include <absl/container/flat_hash_set.h>
#include <benchmark/benchmark.h>

namespace mongo {
namespace {

const Timestamp kPrepareTimestamp(100, 1);
const Timestamp kCommitTimestamp(100, 5);
constexpr TxnNumber kTxnNumber = 7;

// Enables or disables the EOT write path (both the rollout feature flag and the kill switch) for
// the lifetime of the object, and checks that the setting took effect.
class EotArm {
public:
    explicit EotArm(bool enabled)
        : _featureFlag("featureFlagEndOfTransactionChangeEventSynthesis", enabled),
          _allow("allowEndOfTransaction", enabled) {
        invariant(isEndOfTransactionWritePathEnabled() == enabled);
    }

private:
    unittest::ServerParameterGuard _featureFlag;
    unittest::ServerParameterGuard _allow;
};

// A transaction shape: the arguments that describe which shards wrote which namespaces. Every
// benchmark stage runs at each shape in kShapes.
//
// kShapes is a fixed list rather than a full W x N x k grid: at ~23 s per case in CI (3 repetitions
// of at least 5 s each), the 28-shape grid needed about 2.3 hours, longer than the
// benchmarks_sharding task's timeout. The list keeps the use cases the results are reported
// against, plus two shapes that pin the cost per (shard, collection) pair at the extremes of shard
// set reuse. Keep the existing shapes unchanged so their history stays comparable; add new ones at
// the end.
struct ShapeArgs {
    int w;  // write-candidate shards
    int n;  // namespaces written
    int k;  // shards per namespace (2 = sparse, w = dense)
};
constexpr ShapeArgs kShapes[] = {
    {2, 1, 2},      // A: small 2-shard transaction (2 pairs)
    {4, 10, 2},     // B: typical multi-collection app transaction (20 pairs)
    {8, 1, 8},      // C: one collection broadcast to every shard (8 pairs)
    {8, 50, 2},     // D: many collections, targeted writes (100 pairs)
    {8, 50, 8},     // E: proposed worst case, every shard writes every collection (400 pairs)
    {16, 100, 16},  // F: stress upper bound, not a supported shape (1,600 pairs)
    {16, 10, 16},   // dense, many shards: one shared shard set (160 pairs)
    {16, 100, 2},   // sparse, many shards and collections: most distinct shard sets (200 pairs)
};

// Read-only participants add a fixed cost to the commit fan-out only, so they are measured at one
// shape (the largest) rather than at every shape.
constexpr ShapeArgs kReadOnlyShape = {16, 100, 16};
constexpr int kReadOnlyShards = 2;

template <typename F>
void forEachShape(F&& f) {
    for (const auto& s : kShapes) {
        f(s.w, s.n, s.k);
    }
}

// The shapeArgs* functions below register the benchmark arguments. Argument order is W, N, k, [R,]
// eot, named after ShapeArgs::w, n, k, kReadOnlyShards (R) and the EOT arm (0 = off, 1 = on).
// 'eot' is always last so that a run can select one arm with a filter like '/eot:1$'.
//
// Runs each shape with EOT off and on.
void shapeArgsBothArms(benchmark::internal::Benchmark* b) {
    b->ArgNames({"W", "N", "k", "eot"});
    forEachShape([&](int w, int n, int k) {
        for (int eot : {0, 1}) {
            b->Args({w, n, k, eot});
        }
    });
}

// For stages that only run when the write path is enabled. Registered under 'eot:1' so that they
// run in the 'eot = 1' process of an alternated run.
void shapeArgsEnabledOnly(benchmark::internal::Benchmark* b) {
    b->ArgNames({"W", "N", "k", "eot"});
    forEachShape([&](int w, int n, int k) { b->Args({w, n, k, 1}); });
}

// Like shapeArgsBothArms, plus R extra read-only shards: R = 0 at every shape, and R =
// kReadOnlyShards at kReadOnlyShape only.
void shapeArgsWithReadOnly(benchmark::internal::Benchmark* b) {
    b->ArgNames({"W", "N", "k", "R", "eot"});
    forEachShape([&](int w, int n, int k) {
        for (int eot : {0, 1}) {
            b->Args({w, n, k, 0, eot});
        }
    });
    for (int eot : {0, 1}) {
        b->Args({kReadOnlyShape.w, kReadOnlyShape.n, kReadOnlyShape.k, kReadOnlyShards, eot});
    }
}

eot_bm::Shape shapeFromState(const benchmark::State& state, int numReadOnly = 0) {
    return eot_bm::makeShape(static_cast<int>(state.range(0)),
                             static_cast<int>(state.range(1)),
                             static_cast<int>(state.range(2)),
                             numReadOnly);
}

void setShapeCounters(benchmark::State& state, const eot_bm::Shape& shape) {
    state.counters["w_eff"] = shape.numWriteParticipants();
}

// Checks that 'scopes' matches 'shape', so a wrong shape helper or a disabled arm can't produce a
// small, flattering metadata object.
void checkScopesMatchShape(const EndOfTransactionScopesInfoBase& scopes,
                           const eot_bm::Shape& shape) {
    invariant(static_cast<int>(scopes.getClusterParticipants().size()) ==
              shape.numWriteParticipants());
    invariant(scopes.getCollectionParticipants().nFields() == shape.numNamespaces);
    for (const auto& elem : scopes.getCollectionParticipants()) {
        invariant(static_cast<int>(elem.Obj().nFields()) == shape.shardsPerNamespace);
    }
    invariant(scopes.getDatabaseParticipants().nFields() == shape.numDatabases());
}

BSONObj makeCommitObj(const LogicalSessionId& lsid) {
    // Mirrors the common part of sendCommit in transaction_coordinator_util.cpp.
    CommitTransaction commitTransaction;
    commitTransaction.setDbName(DatabaseName::kAdmin);
    commitTransaction.setCommitTimestamp(kCommitTimestamp);
    commitTransaction.setLsid(generic_argument_util::toLogicalSessionFromClient(lsid));
    commitTransaction.setTxnNumber(kTxnNumber);
    commitTransaction.setAutocommit(false);
    commitTransaction.setWriteConcern(defaultMajorityWriteConcernDoNotUse());
    return commitTransaction.toBSON();
}

// The commitTransaction command the coordinator sends to the first write participant.
BSONObj makeWriteParticipantCommitCommand(ServiceContext* service,
                                          const LogicalSessionId& lsid,
                                          const eot_bm::Shape& shape,
                                          bool eot) {
    const auto scopes = eot ? eot_bm::computeScopes(shape) : EndOfTransactionScopesInfoBase{};
    const auto& writers = scopes.getClusterParticipants();
    const absl::flat_hash_set<ShardId> writeParticipantSet(writers.begin(), writers.end());
    return txn::makeCommitCommandForParticipant(makeCommitObj(lsid),
                                                shape.participants.front(),
                                                scopes,
                                                writeParticipantSet,
                                                EndOfTransactionMetadataShardCache::get(service))
        .command;
}

// Mirrors onPreparedTransactionCommit + logCommitOrAbortForPreparedTransaction in
// op_observer_impl.cpp: the fields of the participant's commitTransaction oplog entry.
BSONObj makeCommitOplogEntry(const LogicalSessionId& lsid,
                             const boost::optional<EndOfTransactionScopesInfo>& scopes) {
    repl::MutableOplogEntry oplogEntry;
    oplogEntry.setOpTime(repl::OpTime(Timestamp(100, 6), 1));

    CommitTransactionOplogObject cmdObj;
    cmdObj.setCommitTimestamp(kCommitTimestamp);
    if (scopes) {
        cmdObj.setEndOfTransactionScopes(*scopes);
    }
    oplogEntry.setObject(cmdObj.toBSON());

    oplogEntry.setOpType(repl::OpTypeEnum::kCommand);
    oplogEntry.setNss(NamespaceString::kAdminCommandNamespace);
    oplogEntry.setSessionId(lsid);
    oplogEntry.setTxnNumber(kTxnNumber);
    oplogEntry.setPrevWriteOpTimeInTransaction(repl::OpTime(kPrepareTimestamp, 1));
    oplogEntry.setWallClockTime(Date_t::fromMillisSinceEpoch(1'700'000'000'000));
    return oplogEntry.toBSON();
}

BSONObj makeCoordinatorDocument(const LogicalSessionId& lsid,
                                const eot_bm::Shape& shape,
                                const EndOfTransactionScopesInfoBase* scopes) {
    // Mirrors the document built in persistDecisionBlocking (transaction_coordinator_util.cpp).
    // 'affectedNamespaces' is gated on the legacy feature flag, which is off in both arms.
    OperationSessionInfo sessionInfo;
    sessionInfo.setSessionId(lsid);
    sessionInfo.setTxnNumber(kTxnNumber);

    txn::CoordinatorCommitDecision decision(txn::CommitDecision::kCommit);
    decision.setCommitTimestamp(kCommitTimestamp);

    txn::TransactionCoordinatorDocument doc;
    doc.setId(sessionInfo);
    doc.setParticipants(shape.participants);
    doc.setDecision(decision);
    if (scopes) {
        doc.setEndOfTransactionScopes(*scopes);
    }
    return doc.toBSON();
}

// The coordinator parsing every participant's prepare reply and registering the votes.
void BM_EotPrepareVotes(benchmark::State& state) {
    const bool eot = state.range(3);
    EotArm arm(eot);
    const auto shape = shapeFromState(state);

    std::vector<BSONObj> replies;
    size_t replyBytes = 0;
    for (const auto& shardId : shape.participants) {
        replies.push_back(eot_bm::makePrepareReply(shape, shardId, kPrepareTimestamp, eot));
        replyBytes += replies.back().objsize();
    }

    // Sanity check: the votes only feed the metadata when the arm is on.
    {
        txn::PrepareVoteConsensus consensus(static_cast<int>(shape.participants.size()));
        for (size_t i = 0; i < replies.size(); ++i) {
            consensus.registerVote(eot_bm::parsePrepareReply(shape.participants[i], replies[i]));
        }
        invariant(consensus.decision().getDecision() == txn::CommitDecision::kCommit);
        auto scopes = consensus.releaseEndOfTransactionMetadata();
        if (eot) {
            checkScopesMatchShape(scopes, shape);
        } else {
            invariant(scopes.getClusterParticipants().empty());
        }
    }

    for (auto _ : state) {
        txn::PrepareVoteConsensus consensus(static_cast<int>(shape.participants.size()));
        for (size_t i = 0; i < replies.size(); ++i) {
            consensus.registerVote(eot_bm::parsePrepareReply(shape.participants[i], replies[i]));
        }
        benchmark::DoNotOptimize(consensus);
    }
    setShapeCounters(state, shape);
    state.counters["prepare_reply_bytes"] = replyBytes;
}

// Fills a consensus with every participant's vote, with the write path enabled.
txn::PrepareVoteConsensus makeConsensus(const eot_bm::Shape& shape) {
    txn::PrepareVoteConsensus consensus(static_cast<int>(shape.participants.size()));
    for (const auto& shardId : shape.participants) {
        consensus.registerVote(eot_bm::parsePrepareReply(
            shardId, eot_bm::makePrepareReply(shape, shardId, kPrepareTimestamp, true)));
    }
    return consensus;
}

// The coordinator computing the metadata from the votes. Only runs with the write path on.
void BM_EotReleaseMetadata(benchmark::State& state) {
    EotArm arm(true);
    const auto shape = shapeFromState(state);
    auto consensus = makeConsensus(shape);

    // The benchmark calls releaseEndOfTransactionMetadata repeatedly on one consensus, which is
    // only valid while the function leaves the recorded votes unchanged.
    const auto first = consensus.releaseEndOfTransactionMetadata();
    const auto second = consensus.releaseEndOfTransactionMetadata();
    invariant(first.toBSON().binaryEqual(second.toBSON()));
    checkScopesMatchShape(first, shape);

    for (auto _ : state) {
        auto scopes = consensus.releaseEndOfTransactionMetadata();
        benchmark::DoNotOptimize(scopes);
    }
    setShapeCounters(state, shape);
    state.counters["scopes_bytes"] = first.toBSON().objsize();
}

// The coordinator building the commitTransaction command for every participant.
void BM_EotBuildCommitCommands(benchmark::State& state) {
    const bool eot = state.range(4);
    const auto shape = shapeFromState(state, static_cast<int>(state.range(3)));
    const auto service = ServiceContext::make();
    const auto& metadataShardCache = EndOfTransactionMetadataShardCache::get(service.get());
    const auto lsid = makeLogicalSessionIdForTest();
    const auto scopes = eot ? eot_bm::computeScopes(shape) : EndOfTransactionScopesInfoBase{};

    auto buildAll = [&] {
        // Mirrors sendCommit, minus scheduling the remote commands.
        auto commitObj = makeCommitObj(lsid);
        const auto& writers = scopes.getClusterParticipants();
        const absl::flat_hash_set<ShardId> writeParticipantSet(writers.begin(), writers.end());
        std::vector<BSONObj> commands;
        commands.reserve(shape.participants.size());
        for (const auto& participant : shape.participants) {
            auto [command, withoutScopes] = txn::makeCommitCommandForParticipant(
                commitObj, participant, scopes, writeParticipantSet, metadataShardCache);
            // sendDecisionToShard takes an owned copy of the command.
            commands.push_back(command.getOwned());
            benchmark::DoNotOptimize(withoutScopes);
        }
        return commands;
    };

    // Sanity check: exactly the write participants get the metadata, and only when the arm is on.
    size_t bytes = 0;
    int withScopes = 0;
    for (const auto& command : buildAll()) {
        bytes += command.objsize();
        withScopes += command.hasField(CommitTransaction::kEndOfTransactionScopesFieldName);
    }
    invariant(withScopes == (eot ? shape.numWriteParticipants() : 0));

    for (auto _ : state) {
        auto commands = buildAll();
        benchmark::DoNotOptimize(commands);
    }
    setShapeCounters(state, shape);
    state.counters["commit_wire_bytes"] = bytes;
}

// A write participant parsing the commitTransaction command and storing the metadata for the
// op observer.
void BM_EotParseCommitCommand(benchmark::State& state) {
    const bool eot = state.range(3);
    const auto shape = shapeFromState(state);
    const auto service = ServiceContext::make();
    const auto lsid = makeLogicalSessionIdForTest();
    const auto request = OpMsgRequestBuilder::create(
        boost::none,
        DatabaseName::kAdmin,
        makeWriteParticipantCommitCommand(service.get(), lsid, shape, eot));

    invariant(CommitTransaction::parse(request).getEndOfTransactionScopes().has_value() == eot);

    for (auto _ : state) {
        auto parsed = CommitTransaction::parse(request, IDLParserContext("commitTransaction"));
        // Mirrors txnParticipant.setEndOfTransactionScopes(request().getEndOfTransactionScopes()).
        boost::optional<EndOfTransactionScopesInfo> stored = parsed.getEndOfTransactionScopes();
        benchmark::DoNotOptimize(stored);
    }
    setShapeCounters(state, shape);
    state.counters["command_bytes"] = request.body.objsize();
}

// The metadata a write participant stores from the commitTransaction command.
boost::optional<EndOfTransactionScopesInfo> storedScopes(const eot_bm::Shape& shape, bool eot) {
    if (!eot) {
        return boost::none;
    }
    const auto& shardId = shape.participants.front();
    return EndOfTransactionScopesInfo::parse(
        eot_bm::makeParticipantScopesBSON(eot_bm::computeScopes(shape), shardId),
        IDLParserContext("endOfTransactionScopes"));
}

// A write participant building its commitTransaction oplog entry.
void BM_EotCommitOplogEntry(benchmark::State& state) {
    const bool eot = state.range(3);
    const auto shape = shapeFromState(state);
    const auto lsid = makeLogicalSessionIdForTest();
    const auto stored = storedScopes(shape, eot);

    // Sanity check: the metadata in the entry round-trips to what the coordinator computed.
    const auto entry = makeCommitOplogEntry(lsid, stored);
    const auto o = entry.getObjectField("o");
    invariant(o.hasField(CommitTransactionOplogObject::kEndOfTransactionScopesFieldName) == eot);
    if (eot) {
        const auto parsed = EndOfTransactionScopesInfo::parse(
            o.getObjectField(CommitTransactionOplogObject::kEndOfTransactionScopesFieldName),
            IDLParserContext("endOfTransactionScopes"));
        invariant(parsed.getShardId() == shape.participants.front());
        checkScopesMatchShape(parsed.getBase(), shape);
    }

    for (auto _ : state) {
        auto obj = makeCommitOplogEntry(lsid, stored);
        benchmark::DoNotOptimize(obj);
    }

    // Oplog bytes this transaction's commit adds across the cluster: one commit entry per write
    // participant, plus the coordinator's decision document (written once, on the coordinator's
    // shard). Read-only participants' commit entries are the same in both arms and not counted.
    const auto scopes = eot_bm::computeScopes(shape);
    const auto coordDocBytes =
        makeCoordinatorDocument(lsid, shape, eot ? &scopes : nullptr).objsize();
    setShapeCounters(state, shape);
    state.counters["entry_bytes"] = entry.objsize();
    state.counters["coord_doc_bytes"] = coordDocBytes;
    state.counters["txn_oplog_bytes"] =
        static_cast<double>(entry.objsize()) * shape.numWriteParticipants() + coordDocBytes;
}

// A secondary (or any oplog reader) parsing a commitTransaction entry the way secondary
// apply does (transaction_oplog_application.cpp).
void BM_EotParseCommitOplogEntry(benchmark::State& state) {
    const bool eot = state.range(3);
    const auto shape = shapeFromState(state);
    const auto entry =
        makeCommitOplogEntry(makeLogicalSessionIdForTest(), storedScopes(shape, eot));

    for (auto _ : state) {
        auto oplogEntry = uassertStatusOK(repl::OplogEntry::parse(entry));
        auto commitCommand = CommitTransactionOplogObject::parse(
            oplogEntry.getObject(), IDLParserContext("commitTransaction"));
        benchmark::DoNotOptimize(commitCommand);
    }
    setShapeCounters(state, shape);
    state.counters["entry_bytes"] = entry.objsize();
}

BENCHMARK(BM_EotPrepareVotes)->Apply(shapeArgsBothArms);
BENCHMARK(BM_EotReleaseMetadata)->Apply(shapeArgsEnabledOnly);
BENCHMARK(BM_EotBuildCommitCommands)->Apply(shapeArgsWithReadOnly);
BENCHMARK(BM_EotParseCommitCommand)->Apply(shapeArgsBothArms);
BENCHMARK(BM_EotCommitOplogEntry)->Apply(shapeArgsBothArms);
BENCHMARK(BM_EotParseCommitOplogEntry)->Apply(shapeArgsBothArms);

}  // namespace
}  // namespace mongo
