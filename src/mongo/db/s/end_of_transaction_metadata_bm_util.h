// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/commands/txn_cmds_gen.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/s/transaction_coordinator_util.h"
#include "mongo/db/sharding_environment/shard_id.h"
#include "mongo/util/modules.h"

#include <map>
#include <vector>

/**
 * Test-only helpers shared by the end-of-transaction (EOT) metadata microbenchmarks. They build the
 * participant/namespace shape of a sharded transaction, from which the benchmarks derive the
 * prepare votes, the coordinator's accounting metadata, and the commit commands and oplog entries.
 */
namespace mongo {
namespace [[MONGO_MOD_PUBLIC]] eot_bm {

/**
 * The shape of one sharded transaction.
 *
 * 'numWriteCandidates' shards are eligible to write; namespace i is written by the
 * 'shardsPerNamespace' consecutive shards starting at (i * shardsPerNamespace) mod
 * numWriteCandidates. Shards that end up writing nothing (possible when
 * numNamespaces * shardsPerNamespace < numWriteCandidates) are read-only participants, as are the
 * 'numReadOnly' extra shards. Namespace i lives in database i / kNamespacesPerDatabase.
 */
struct Shape {
    static constexpr int kNamespacesPerDatabase = 10;

    int numWriteCandidates = 0;
    int numNamespaces = 0;
    int shardsPerNamespace = 0;
    int numReadOnly = 0;

    // Every participant, write participants first, in shard id order within each group.
    std::vector<ShardId> participants;

    // Namespaces written by each write participant. Shards absent from the map are read-only.
    std::map<ShardId, std::vector<NamespaceString>> namespacesByShard;

    int numWriteParticipants() const {
        return static_cast<int>(namespacesByShard.size());
    }

    int numDatabases() const {
        return (numNamespaces + kNamespacesPerDatabase - 1) / kNamespacesPerDatabase;
    }

    std::vector<ShardId> writeParticipants() const;
};

/**
 * Shard ids are 20 characters, the length of Atlas shard ids ("atlas-k3x9q-shard-07"), database
 * names 12 ("eot_bench_03"), and collection names 24 ("orders_by_region_0000042"), so that byte
 * counts are realistic.
 */
Shape makeShape(int numWriteCandidates,
                int numNamespaces,
                int shardsPerNamespace,
                int numReadOnly = 0);

/**
 * The prepareTransaction reply 'shardId' sends, as the coordinator receives it (including "ok").
 * The reply carries 'affectedNamespaces' iff 'withAffectedNamespaces', which is what the
 * participant does when the EOT write path is enabled. Read-only shards send an empty list.
 */
BSONObj makePrepareReply(const Shape& shape,
                         const ShardId& shardId,
                         Timestamp prepareTimestamp,
                         bool withAffectedNamespaces);

/**
 * Turns a prepare reply into the vote the coordinator registers, the same way sendPrepareToShard
 * does for an OK reply.
 */
txn::PrepareResponse parsePrepareReply(const ShardId& shardId, const BSONObj& reply);

/**
 * Computes the coordinator's accounting metadata for 'shape' with the production code
 * (PrepareVoteConsensus::registerVote and releaseEndOfTransactionMetadata). Enables the EOT write
 * path for the duration of the call, so the result doesn't depend on the caller's settings.
 */
EndOfTransactionScopesInfoBase computeScopes(const Shape& shape);

/**
 * The 'endOfTransactionScopes' object that 'shardId' writes into its commitTransaction oplog entry.
 */
BSONObj makeParticipantScopesBSON(const EndOfTransactionScopesInfoBase& scopes,
                                  const ShardId& shardId);

}  // namespace eot_bm
}  // namespace mongo
