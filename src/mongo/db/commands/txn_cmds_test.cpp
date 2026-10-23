// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

/**
 * IDL-parse-time validation tests for the 'commitTransaction' oplog object. These exercise only
 * the IDL parser generated from src/mongo/db/commands/txn_cmds.idl -- no opCtx, no catalog, no
 * fixture.
 */

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/commands/txn_cmds_gen.h"
#include "mongo/db/sharding_environment/shard_id.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

TEST(EndOfTransactionScopesInfoTest, RoundTripsPopulatedMetadata) {
    EndOfTransactionScopesInfo info;
    info.setShardId(ShardId("shardA"));
    info.setCommitFloor(Timestamp(100, 5));
    info.setCollectionParticipants(
        BSON("db.collA" << BSON_ARRAY("shardA" << "shardB") << "db.collB" << BSON_ARRAY("shardB")));
    info.setDatabaseParticipants(BSON("db" << BSON_ARRAY("shardA" << "shardB")));
    info.setClusterParticipants(std::vector<ShardId>{ShardId("shardA"), ShardId("shardB")});

    auto parsed = EndOfTransactionScopesInfo::parse(info.toBSON(),
                                                    IDLParserContext("endOfTransactionScopes"));

    ASSERT_EQ(ShardId("shardA"), parsed.getShardId());
    ASSERT_EQ(Timestamp(100, 5), parsed.getCommitFloor());
    ASSERT_BSONOBJ_EQ(
        parsed.getCollectionParticipants(),
        BSON("db.collA" << BSON_ARRAY("shardA" << "shardB") << "db.collB" << BSON_ARRAY("shardB")));
    ASSERT_BSONOBJ_EQ(parsed.getDatabaseParticipants(),
                      BSON("db" << BSON_ARRAY("shardA" << "shardB")));
    ASSERT_EQ(2U, parsed.getClusterParticipants().size());
    ASSERT_EQ(ShardId("shardA"), parsed.getClusterParticipants().at(0));
    ASSERT_EQ(ShardId("shardB"), parsed.getClusterParticipants().at(1));

    auto bson = parsed.toBSON();
    ASSERT_BSONOBJ_EQ(
        bson,
        BSON("shardId" << "shardA" << "commitFloor" << Timestamp(100, 5) << "collectionParticipants"
                       << BSON("db.collA" << BSON_ARRAY("shardA" << "shardB") << "db.collB"
                                          << BSON_ARRAY("shardB"))
                       << "databaseParticipants" << BSON("db" << BSON_ARRAY("shardA" << "shardB"))
                       << "clusterParticipants" << BSON_ARRAY("shardA" << "shardB")));
}

TEST(CommitTransactionOplogObjectParseTest, ParsesWithoutEndOfTransactionScopesField) {
    auto obj = BSON("commitTransaction" << 1 << "count" << 5LL);
    auto parsed =
        CommitTransactionOplogObject::parse(obj, IDLParserContext("commitTransactionOplogEntry"));
    ASSERT_FALSE(parsed.getEndOfTransactionScopes().has_value());

    auto bson = parsed.toBSON();
    ASSERT_FALSE(bson.hasField("endOfTransactionScopes"));
}

TEST(CommitTransactionOplogObjectParseTest,
     IgnoresArbitrarySubFieldsInEndOfTransactionScopesField) {
    auto scopes =
        BSON("shardId" << "shardA" << "commitFloor" << Timestamp(100, 5) << "collectionParticipants"
                       << BSON("db.collA" << BSON_ARRAY("shardA" << "shardB"))
                       << "databaseParticipants" << BSON("db" << BSON_ARRAY("shardA"))
                       << "clusterParticipants" << BSON_ARRAY("shardA" << "shardB")
                       << "someFutureScope" << "collectionA" << "anotherFutureField" << 42
                       << "nestedFutureField" << BSON("x" << 1 << "y" << 2));
    auto obj = BSON("commitTransaction" << 1 << "endOfTransactionScopes" << scopes);
    auto parsed =
        CommitTransactionOplogObject::parse(obj, IDLParserContext("commitTransactionOplogEntry"));
    ASSERT_TRUE(parsed.getEndOfTransactionScopes().has_value());

    auto bson = parsed.toBSON();
    ASSERT_TRUE(bson.hasField("endOfTransactionScopes"));
    ASSERT_BSONOBJ_EQ(
        bson.getField("endOfTransactionScopes").Obj(),
        scopes.removeFields({"someFutureScope", "anotherFutureField", "nestedFutureField"}));
}

TEST(CommitTransactionOplogObjectParseTest, ParsingYieldsIdenticalResults) {
    auto scopes =
        BSON("shardId" << "shardC" << "commitFloor" << Timestamp(42, 23) << "collectionParticipants"
                       << BSON("db.collA" << BSON_ARRAY("shardA" << "shardB"))
                       << "databaseParticipants" << BSON("db" << BSON_ARRAY("shardA"))
                       << "clusterParticipants" << BSON_ARRAY("shardA" << "shardB" << "shardC"));

    auto validateEndOfTransactionScopes = [](auto&& parsed) {
        ASSERT_EQ(ShardId("shardC"), parsed.getShardId());
        ASSERT_EQ(Timestamp(42, 23), parsed.getCommitFloor());
        ASSERT_BSONOBJ_EQ(parsed.getCollectionParticipants(),
                          BSON("db.collA" << BSON_ARRAY("shardA" << "shardB")));
        ASSERT_BSONOBJ_EQ(parsed.getDatabaseParticipants(), BSON("db" << BSON_ARRAY("shardA")));
        ASSERT_EQ(3U, parsed.getClusterParticipants().size());
        ASSERT_EQ(ShardId("shardA"), parsed.getClusterParticipants().at(0));
        ASSERT_EQ(ShardId("shardB"), parsed.getClusterParticipants().at(1));
        ASSERT_EQ(ShardId("shardC"), parsed.getClusterParticipants().at(2));
    };

    // Parse top-level commitTransaction oplog entry.
    auto obj = BSON("commitTransaction" << 1 << "endOfTransactionScopes" << scopes);
    auto parsedCommit =
        CommitTransactionOplogObject::parse(obj, IDLParserContext("commitTransactionOplogEntry"));
    ASSERT_TRUE(parsedCommit.getEndOfTransactionScopes().has_value());
    validateEndOfTransactionScopes(parsedCommit.getEndOfTransactionScopes().value());

    // Parse "endOfTransactionScopes" field from top-level entry.
    auto parsedEndOfTransaction =
        EndOfTransactionScopesInfo::parse(scopes, IDLParserContext("endOfTransactionScopes"));
    validateEndOfTransactionScopes(parsedEndOfTransaction);
    ASSERT_BSONOBJ_EQ(scopes, parsedEndOfTransaction.toBSON());
}

}  // namespace
}  // namespace mongo
