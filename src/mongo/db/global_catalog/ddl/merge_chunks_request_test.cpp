// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/base/error_codes.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/oid.h"
#include "mongo/bson/simple_bsonobj_comparator.h"
#include "mongo/db/generic_argument_util.h"
#include "mongo/db/global_catalog/ddl/merge_chunk_request_gen.h"
#include "mongo/db/global_catalog/type_chunk.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/sharding_environment/shard_id.h"
#include "mongo/db/write_concern_options.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/uuid.h"

#include <memory>

namespace mongo {
namespace {

mongo::BSONObj min = BSON("a" << 1);
mongo::BSONObj max = BSON("a" << 10);
ChunkRange chunkRange(min, max);
IDLParserContext ctx("_configsvrCommitChunksMerge");

TEST(ConfigSvrMergeChunks, BasicValidConfigCommand) {
    auto collUUID = UUID::gen();
    auto request = ConfigSvrMergeChunks::parse(
        BSON("_configsvrCommitChunksMerge" << "TestDB.TestColl"
                                           << "collUUID" << collUUID.toBSON() << "chunkRange"
                                           << chunkRange.toBSON() << "shard"
                                           << "shard0000"
                                           << "$db"
                                           << "admin"),
        ctx);
    ASSERT_EQ(NamespaceString::createNamespaceString_forTest("TestDB", "TestColl"),
              request.getCommandParameter());
    ASSERT_TRUE(collUUID == request.getCollectionUUID());
    ASSERT_TRUE(chunkRange == request.getChunkRange());
    ASSERT_EQ("shard0000", request.getShard().toString());
}

TEST(ConfigSvrMergeChunks, ConfigCommandtoBSON) {
    auto collUUID = UUID::gen();
    BSONObj serializedRequest =
        BSON("_configsvrCommitChunksMerge" << "TestDB.TestColl"
                                           << "shard"
                                           << "shard0000"
                                           << "collUUID" << collUUID.toBSON() << "chunkRange"
                                           << chunkRange.toBSON());
    auto writeConcern = defaultMajorityWriteConcernDoNotUse();

    BSONObjBuilder cmdBuilder;
    {
        cmdBuilder.appendElements(serializedRequest);
        cmdBuilder.append("writeConcern", writeConcern.toBSON());
    }

    auto appendDB = [](const BSONObj& obj) {
        BSONObjBuilder builder;
        builder.appendElements(obj);
        builder.append("$db", "admin");
        return builder.obj();
    };

    auto request = ConfigSvrMergeChunks::parse(appendDB(serializedRequest), ctx);
    request.setWriteConcern(writeConcern);
    auto requestToBSON = request.toBSON();

    ASSERT_BSONOBJ_EQ(cmdBuilder.obj(), requestToBSON);
}

TEST(ConfigSvrMergeChunks, MissingNameSpaceErrors) {
    auto collUUID = UUID::gen();
    ASSERT_THROWS_CODE(
        ConfigSvrMergeChunks::parse(BSON("collUUID" << collUUID.toBSON() << "chunkRange"
                                                    << chunkRange.toBSON() << "shard"
                                                    << "shard0000"
                                                    << "$db"
                                                    << "admin"),
                                    ctx),
        mongo::DBException,
        ErrorCodes::IDLFailedToParse);
}

TEST(ConfigSvrMergeChunks, MissingCollUUIDErrors) {
    ASSERT_THROWS_CODE(
        ConfigSvrMergeChunks::parse(BSON("_configsvrCommitChunksMerge"
                                         << "TestDB.TestColl"
                                         << "chunkRange" << chunkRange.toBSON() << "shard"
                                         << "shard0000"
                                         << "$db"
                                         << "admin"),
                                    ctx),
        mongo::DBException,
        ErrorCodes::IDLFailedToParse);
    // ASSERT_EQ(ErrorCodes::NoSuchKey, request.getStatus());
}

TEST(ConfigSvrMergeChunks, MissingChunkRangeErrors) {
    auto collUUID = UUID::gen();
    ASSERT_THROWS_CODE(
        ConfigSvrMergeChunks::parse(BSON("_configsvrCommitChunksMerge"
                                         << "TestDB.TestColl"
                                         << "collUUID" << collUUID.toBSON() << "shard"
                                         << "shard0000"
                                         << "$db"
                                         << "admin"),
                                    ctx),
        DBException,
        ErrorCodes::IDLFailedToParse);
}

TEST(ConfigSvrMergeChunks, MissingShardIdErrors) {
    auto collUUID = UUID::gen();
    ASSERT_THROWS_CODE(
        ConfigSvrMergeChunks::parse(BSON("_configsvrCommitChunksMerge"
                                         << "TestDB.TestColl"
                                         << "collUUID" << collUUID.toBSON() << "chunkRange"
                                         << chunkRange.toBSON() << "$db"
                                         << "admin"),
                                    ctx),
        DBException,
        ErrorCodes::IDLFailedToParse);
}

TEST(ConfigSvrMergeChunks, WrongNamespaceTypeErrors) {
    auto collUUID = UUID::gen();
    ASSERT_THROWS_CODE(
        ConfigSvrMergeChunks::parse(BSON("_configsvrCommitChunksMerge"
                                         << 1234 << "collUUID" << collUUID.toBSON() << "chunkRange"
                                         << chunkRange.toBSON() << "shard"
                                         << "shard0000"
                                         << "$db"
                                         << "admin"),
                                    ctx),
        DBException,
        ErrorCodes::TypeMismatch);
}

TEST(ConfigSvrMergeChunks, WrongCollUUIDTypeErrors) {
    ASSERT_THROWS_CODE(ConfigSvrMergeChunks::parse(BSON("_configsvrCommitChunksMerge"
                                                        << "TestDB.TestColl"
                                                        << "collUUID" << 1234 << "chunkRange"
                                                        << chunkRange.toBSON() << "shard"
                                                        << "shard0000"
                                                        << "$db"
                                                        << "admin"),
                                                   ctx),
                       DBException,
                       ErrorCodes::TypeMismatch);
}


TEST(ConfigSvrMergeChunks, WrongChunkRangeTypeErrors) {
    auto collUUID = UUID::gen();
    ASSERT_THROWS_CODE(ConfigSvrMergeChunks::parse(BSON("_configsvrCommitChunksMerge"
                                                        << "TestDB.TestColl"
                                                        << "collUUID" << collUUID.toBSON()
                                                        << "chunkRange" << 1234 << "shard"
                                                        << "shard0000"
                                                        << "$db"
                                                        << "admin"),
                                                   ctx),
                       DBException,
                       ErrorCodes::TypeMismatch);
}

TEST(ConfigSvrMergeChunks, WrongShardIdTypeErrors) {
    auto collUUID = UUID::gen();
    ASSERT_THROWS_CODE(
        ConfigSvrMergeChunks::parse(BSON("_configsvrCommitChunksMerge"
                                         << "TestDB.TestColl"
                                         << "collUUID" << collUUID.toBSON() << "chunkRange"
                                         << chunkRange.toBSON() << "shard" << 1234 << "$db"
                                         << "admin"),
                                    ctx),
        DBException,
        ErrorCodes::TypeMismatch);
}

TEST(ShardsvrMergeChunks, BasicValidConfigCommand) {
    std::vector<mongo::BSONObj> bounds;
    bounds.push_back(min);
    bounds.push_back(max);

    OID epoch = OID::gen();
    auto request = ShardsvrMergeChunks::parse(BSON("mergeChunks" << "TestDB.TestColl"
                                                                 << "bounds" << bounds << "epoch"
                                                                 << epoch << "$db"
                                                                 << "admin"),
                                              ctx);
    ASSERT_EQ(NamespaceString::createNamespaceString_forTest("TestDB", "TestColl"),
              request.getCommandParameter());

    const auto requestBounds = request.getBounds();
    ASSERT_TRUE(SimpleBSONObjComparator::kInstance.evaluate(min == requestBounds[0]));
    ASSERT_TRUE(SimpleBSONObjComparator::kInstance.evaluate(max == requestBounds[1]));
}

TEST(ShardsvrMergeChunks, MissingNameSpaceErrors) {
    std::vector<mongo::BSONObj> bounds;
    bounds.push_back(min);
    bounds.push_back(max);

    OID epoch = OID::gen();
    ASSERT_THROWS_CODE(
        ShardsvrMergeChunks::parse(BSON("bounds" << bounds << "epoch" << epoch << "$db"
                                                 << "admin"),
                                   ctx),
        mongo::DBException,
        ErrorCodes::IDLFailedToParse);
}

TEST(ShardsvrMergeChunks, MissingBoundErrors) {
    std::vector<mongo::BSONObj> bounds;
    bounds.push_back(min);
    bounds.push_back(max);

    OID epoch = OID::gen();
    ASSERT_THROWS_CODE(ShardsvrMergeChunks::parse(BSON("mergeChunks" << "TestDB.TestColl"
                                                                     << "epoch" << epoch << "$db"
                                                                     << "admin"),
                                                  ctx),
                       DBException,
                       ErrorCodes::IDLFailedToParse);
}

TEST(ShardsvrMergeChunks, MissingEpochErrors) {
    std::vector<mongo::BSONObj> bounds;
    bounds.push_back(min);
    bounds.push_back(max);

    ASSERT_THROWS_CODE(ShardsvrMergeChunks::parse(BSON("mergeChunks" << "TestDB.TestColl"
                                                                     << "bounds" << bounds << "$db"
                                                                     << "admin"),
                                                  ctx),
                       DBException,
                       ErrorCodes::IDLFailedToParse);
}

TEST(ShardsvrMergeChunks, WrongNamespaceTypeErrors) {
    std::vector<mongo::BSONObj> bounds;
    bounds.push_back(min);
    bounds.push_back(max);

    OID epoch = OID::gen();
    ASSERT_THROWS_CODE(ShardsvrMergeChunks::parse(BSON("mergeChunks" << 12345 << "bounds" << bounds
                                                                     << "epoch" << epoch << "$db"
                                                                     << "admin"),
                                                  ctx),
                       DBException,
                       ErrorCodes::TypeMismatch);
}

TEST(ShardsvrMergeChunks, WrongBoundTypeErrors) {
    std::vector<mongo::BSONObj> bounds;
    bounds.push_back(min);
    bounds.push_back(max);

    OID epoch = OID::gen();
    ASSERT_THROWS_CODE(ShardsvrMergeChunks::parse(BSON("mergeChunks" << "TestDB.TestColl"
                                                                     << "bounds" << 1234 << "epoch"
                                                                     << epoch << "$db"
                                                                     << "admin"),
                                                  ctx),
                       DBException,
                       ErrorCodes::TypeMismatch);
}

TEST(ClusterMergeChunks, BasicValidConfigCommand) {
    std::vector<mongo::BSONObj> bounds;
    bounds.push_back(min);
    bounds.push_back(max);

    auto request = ClusterMergeChunks::parse(BSON("mergeChunks" << "TestDB.TestColl"
                                                                << "bounds" << bounds << "$db"
                                                                << "admin"),
                                             ctx);

    ASSERT_EQ(NamespaceString::createNamespaceString_forTest("TestDB", "TestColl"),
              request.getCommandParameter());

    const auto requestBounds = request.getBounds();
    ASSERT_TRUE(SimpleBSONObjComparator::kInstance.evaluate(min == requestBounds[0]));
    ASSERT_TRUE(SimpleBSONObjComparator::kInstance.evaluate(max == requestBounds[1]));
}


TEST(ClusterMergeChunks, MissingNameSpaceErrors) {
    std::vector<mongo::BSONObj> bounds;
    bounds.push_back(min);
    bounds.push_back(max);

    ASSERT_THROWS_CODE(ClusterMergeChunks::parse(BSON("bounds" << bounds << "$db"
                                                               << "admin"),
                                                 ctx),
                       mongo::DBException,
                       ErrorCodes::IDLFailedToParse);
}


TEST(ClusterMergeChunks, MissingBoundErrors) {
    std::vector<mongo::BSONObj> bounds;
    bounds.push_back(min);
    bounds.push_back(max);

    ASSERT_THROWS_CODE(ClusterMergeChunks::parse(BSON("mergeChunks" << "TestDB.TestColl"
                                                                    << "$db"
                                                                    << "admin"),
                                                 ctx),
                       DBException,
                       ErrorCodes::IDLFailedToParse);
}

TEST(ClusterMergeChunks, WrongNamespaceTypeErrors) {
    std::vector<mongo::BSONObj> bounds;
    bounds.push_back(min);
    bounds.push_back(max);

    ASSERT_THROWS_CODE(
        ClusterMergeChunks::parse(BSON("mergeChunks" << 12345 << "bounds" << bounds << "$db"
                                                     << "admin"),
                                  ctx),
        DBException,
        ErrorCodes::TypeMismatch);
}

TEST(ClusterMergeChunks, WrongBoundTypeErrors) {
    std::vector<mongo::BSONObj> bounds;
    bounds.push_back(min);
    bounds.push_back(max);

    ASSERT_THROWS_CODE(ClusterMergeChunks::parse(BSON("mergeChunks" << "TestDB.TestColl"
                                                                    << "bounds" << 1234 << "$db"
                                                                    << "admin"),
                                                 ctx),
                       DBException,
                       ErrorCodes::TypeMismatch);
}

}  // namespace
}  // namespace mongo
