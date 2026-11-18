// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/s/end_of_transaction_metadata_bm_util.h"

#include "mongo/bson/bsonobj.h"
#include "mongo/db/commands/txn_cmds_gen.h"
#include "mongo/db/end_of_transaction_parameter.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/unittest/unittest.h"

#include <set>

namespace mongo::eot_bm {
namespace {

TEST(EndOfTransactionMetadataBmShapeTest, DenseShapeHasEveryShardOnEveryNamespace) {
    const auto shape = makeShape(/*W*/ 8, /*N*/ 50, /*k*/ 8);
    ASSERT_EQ(8, shape.numWriteParticipants());
    ASSERT_EQ(8U, shape.participants.size());
    ASSERT_EQ(5, shape.numDatabases());
    for (const auto& [shardId, namespaces] : shape.namespacesByShard) {
        ASSERT_EQ(50U, namespaces.size());
    }
}

TEST(EndOfTransactionMetadataBmShapeTest, SparseShapeSpreadsNamespacesOverAllShards) {
    // N * k = 20 >= W, so every shard writes at least one namespace.
    const auto shape = makeShape(/*W*/ 16, /*N*/ 10, /*k*/ 2);
    ASSERT_EQ(16, shape.numWriteParticipants());
    size_t total = 0;
    for (const auto& [shardId, namespaces] : shape.namespacesByShard) {
        total += namespaces.size();
    }
    ASSERT_EQ(20U, total);
}

TEST(EndOfTransactionMetadataBmShapeTest, ShardsThatWriteNothingAreReadOnly) {
    const auto shape = makeShape(/*W*/ 16, /*N*/ 1, /*k*/ 2, /*R*/ 2);
    ASSERT_EQ(2, shape.numWriteParticipants());
    ASSERT_EQ(18U, shape.participants.size());

    // Write participants come first, and no participant is listed twice.
    ASSERT_EQ(shape.writeParticipants(),
              std::vector<ShardId>(shape.participants.begin(), shape.participants.begin() + 2));
    ASSERT_EQ(18U, std::set<ShardId>(shape.participants.begin(), shape.participants.end()).size());
}

TEST(EndOfTransactionMetadataBmShapeTest, NamesHaveRealisticLengths) {
    const auto shape = makeShape(4, 1, 2);
    const auto& [shardId, namespaces] = *shape.namespacesByShard.begin();
    ASSERT_EQ(20U, shardId.toString().size());
    ASSERT_EQ(12U, namespaces.front().db_forTest().size());
    ASSERT_EQ(24U, namespaces.front().coll().size());
}

TEST(EndOfTransactionMetadataBmShapeTest, PrepareReplyCarriesNamespacesOnlyWhenRequested) {
    const auto shape = makeShape(4, 10, 2);
    const auto& shardId = shape.participants.front();
    const auto with = parsePrepareReply(shardId, makePrepareReply(shape, shardId, {1, 1}, true));
    const auto without =
        parsePrepareReply(shardId, makePrepareReply(shape, shardId, {1, 1}, false));
    ASSERT_EQ(shape.namespacesByShard.at(shardId).size(), with.affectedNamespaces.size());
    ASSERT_TRUE(without.affectedNamespaces.empty());
}

TEST(EndOfTransactionMetadataBmShapeTest, ComputedScopesMatchShapeAndRoundTrip) {
    const auto shape = makeShape(/*W*/ 4, /*N*/ 10, /*k*/ 2, /*R*/ 1);
    const auto scopes = computeScopes(shape);

    ASSERT_EQ(shape.writeParticipants(), scopes.getClusterParticipants());
    ASSERT_EQ(10, scopes.getCollectionParticipants().nFields());
    for (const auto& elem : scopes.getCollectionParticipants()) {
        ASSERT_EQ(2, elem.Obj().nFields());
    }
    ASSERT_EQ(1, scopes.getDatabaseParticipants().nFields());

    const auto& shardId = shape.participants.front();
    const auto bson = makeParticipantScopesBSON(scopes, shardId);
    const auto parsed =
        EndOfTransactionScopesInfo::parse(bson, IDLParserContext("endOfTransactionScopes"));
    ASSERT_EQ(shardId, parsed.getShardId());
    ASSERT_BSONOBJ_EQ(bson, parsed.toBSON());
}

TEST(EndOfTransactionMetadataBmShapeTest, ComputeScopesRestoresTheWritePathSetting) {
    ASSERT_FALSE(isEndOfTransactionWritePathEnabled());
    computeScopes(makeShape(2, 1, 2));
    ASSERT_FALSE(isEndOfTransactionWritePathEnabled());
}

}  // namespace
}  // namespace mongo::eot_bm
