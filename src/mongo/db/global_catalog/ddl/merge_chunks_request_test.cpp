/**
 *    Copyright (C) 2021-present MongoDB, Inc.
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

#include "mongo/base/error_codes.h"
#include "mongo/base/string_data.h"
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
