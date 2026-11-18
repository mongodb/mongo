// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/s/end_of_transaction_metadata_bm_util.h"

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/commands/txn_two_phase_commit_cmds_gen.h"
#include "mongo/db/database_name.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/unittest/server_parameter_guard.h"
#include "mongo/util/assert_util.h"

#include <fmt/format.h>

namespace mongo::eot_bm {

std::vector<ShardId> Shape::writeParticipants() const {
    std::vector<ShardId> result;
    result.reserve(namespacesByShard.size());
    for (const auto& [shardId, _] : namespacesByShard) {
        result.push_back(shardId);
    }
    return result;
}

Shape makeShape(int numWriteCandidates,
                int numNamespaces,
                int shardsPerNamespace,
                int numReadOnly) {
    invariant(numWriteCandidates > 0 && numWriteCandidates < 100);
    invariant(shardsPerNamespace > 0 && shardsPerNamespace <= numWriteCandidates);
    invariant(numNamespaces > 0 && numReadOnly >= 0);
    invariant(numWriteCandidates + numReadOnly < 100);

    Shape shape;
    shape.numWriteCandidates = numWriteCandidates;
    shape.numNamespaces = numNamespaces;
    shape.shardsPerNamespace = shardsPerNamespace;
    shape.numReadOnly = numReadOnly;

    auto shardName = [](int i) {
        // Atlas shard ids look like 'atlas-<id>-shard-<n>'.
        return ShardId(fmt::format("atlas-k3x9q-shard-{:02}", i));
    };

    for (int i = 0; i < numNamespaces; ++i) {
        const auto nss = NamespaceString::createNamespaceString_forTest(
            fmt::format("eot_bench_{:02}", i / Shape::kNamespacesPerDatabase),
            fmt::format("orders_by_region_{:07}", i));
        for (int j = 0; j < shardsPerNamespace; ++j) {
            const int shard = (i * shardsPerNamespace + j) % numWriteCandidates;
            shape.namespacesByShard[shardName(shard)].push_back(nss);
        }
    }

    // Write participants first, then the candidates that wrote nothing, then the extra read-only
    // shards. Shard names are zero-padded, so this is also shard id order within each group.
    shape.participants = shape.writeParticipants();
    for (int i = 0; i < numWriteCandidates + numReadOnly; ++i) {
        const auto shardId = shardName(i);
        if (!shape.namespacesByShard.contains(shardId)) {
            shape.participants.push_back(shardId);
        }
    }
    return shape;
}

BSONObj makePrepareReply(const Shape& shape,
                         const ShardId& shardId,
                         Timestamp prepareTimestamp,
                         bool withAffectedNamespaces) {
    PrepareReply reply;
    reply.setPrepareTimestamp(prepareTimestamp);
    if (withAffectedNamespaces) {
        // Mirrors the participant's createResponse in txn_two_phase_commit_cmds.cpp.
        const auto it = shape.namespacesByShard.find(shardId);
        reply.setAffectedNamespaces(
            it == shape.namespacesByShard.end() ? std::vector<NamespaceString>{} : it->second);
    }
    BSONObjBuilder builder;
    reply.serialize(&builder);
    builder.append("ok", 1.0);
    return builder.obj();
}

txn::PrepareResponse parsePrepareReply(const ShardId& shardId, const BSONObj& replyObj) {
    // Mirrors the OK-reply branch of sendPrepareToShard in transaction_coordinator_util.cpp.
    auto reply = PrepareReply::parse(replyObj, IDLParserContext("PrepareReply"));
    invariant(reply.getPrepareTimestamp());
    return txn::PrepareResponse{
        shardId,
        txn::PrepareVote::kCommit,
        *reply.getPrepareTimestamp(),
        reply.getAffectedNamespaces().value_or(std::vector<NamespaceString>{}),
        boost::none};
}

EndOfTransactionScopesInfoBase computeScopes(const Shape& shape) {
    unittest::ServerParameterGuard featureFlag("featureFlagEndOfTransactionChangeEventSynthesis",
                                               true);
    unittest::ServerParameterGuard allow("allowEndOfTransaction", true);

    txn::PrepareVoteConsensus consensus(static_cast<int>(shape.participants.size()));
    for (const auto& shardId : shape.participants) {
        consensus.registerVote(parsePrepareReply(
            shardId, makePrepareReply(shape, shardId, Timestamp(1, 1), /*with*/ true)));
    }
    return consensus.releaseEndOfTransactionMetadata();
}

BSONObj makeParticipantScopesBSON(const EndOfTransactionScopesInfoBase& scopes,
                                  const ShardId& shardId) {
    EndOfTransactionScopesInfo info;
    info.setBase(scopes);
    info.setShardId(shardId);
    return info.toBSON();
}

}  // namespace mongo::eot_bm
