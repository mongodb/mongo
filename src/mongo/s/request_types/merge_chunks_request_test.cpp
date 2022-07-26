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

#include "mongo/platform/basic.h"

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/s/request_types/merge_chunk_request_gen.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"

namespace mongo {
namespace {

using unittest::assertGet;

ChunkRange chunkRange(BSON("a" << 1), BSON("a" << 10));
IDLParserContext ctx("_configsvrCommitChunksMerge");

TEST(ConfigSvrMergeChunks, BasicValidConfigCommand) {
    auto collUUID = UUID::gen();
    auto request = ConfigSvrMergeChunks::parse(
        ctx,
        BSON("_configsvrCommitChunksMerge"
             << "TestDB.TestColl"
             << "collUUID" << collUUID.toBSON() << "chunkRange" << chunkRange.toBSON() << "shard"
             << "shard0000"
             << "$db"
             << "admin"));
    ASSERT_EQ(NamespaceString("TestDB", "TestColl"), request.getCommandParameter());
    ASSERT_TRUE(collUUID == request.getCollectionUUID());
    ASSERT_TRUE(chunkRange == request.getChunkRange());
    ASSERT_EQ("shard0000", request.getShard().toString());
}

TEST(ConfigSvrMergeChunks, ConfigCommandtoBSON) {
    auto collUUID = UUID::gen();
    BSONObj serializedRequest = BSON("_configsvrCommitChunksMerge"
                                     << "TestDB.TestColl"
                                     << "shard"
                                     << "shard0000"
                                     << "collUUID" << collUUID.toBSON() << "chunkRange"
                                     << chunkRange.toBSON() << "validAfter" << Timestamp{100});
    BSONObj writeConcernObj = BSON("w"
                                   << "majority");

    BSONObjBuilder cmdBuilder;
    {
        cmdBuilder.appendElements(serializedRequest);
        cmdBuilder.append("writeConcern", writeConcernObj);
    }

    auto appendDB = [](const BSONObj& obj) {
        BSONObjBuilder builder;
        builder.appendElements(obj);
        builder.append("$db", "admin");
        return builder.obj();
    };

    auto request = ConfigSvrMergeChunks::parse(ctx, appendDB(serializedRequest));
    auto requestToBSON = request.toBSON(BSON("writeConcern" << writeConcernObj));

    ASSERT_BSONOBJ_EQ(cmdBuilder.obj(), requestToBSON);
}

TEST(ConfigSvrMergeChunks, MissingNameSpaceErrors) {
    auto collUUID = UUID::gen();
    ASSERT_THROWS_CODE(
        ConfigSvrMergeChunks::parse(ctx,
                                    BSON("collUUID" << collUUID.toBSON() << "chunkRange"
                                                    << chunkRange.toBSON() << "shard"
                                                    << "shard0000"
                                                    << "$db"
                                                    << "admin")),
        mongo::DBException,
        40414);
}

TEST(ConfigSvrMergeChunks, MissingcollUUIDErrors) {
    ASSERT_THROWS_CODE(
        ConfigSvrMergeChunks::parse(ctx,
                                    BSON("_configsvrCommitChunksMerge"
                                         << "TestDB.TestColl"
                                         << "chunkRange" << chunkRange.toBSON() << "shard"
                                         << "shard0000"
                                         << "$db"
                                         << "admin")),
        mongo::DBException,
        40414);
    // ASSERT_EQ(ErrorCodes::NoSuchKey, request.getStatus());
}

TEST(ConfigSvrMergeChunks, MissingChunkRangeErrors) {
    auto collUUID = UUID::gen();
    ASSERT_THROWS_CODE(
        ConfigSvrMergeChunks::parse(ctx,
                                    BSON("_configsvrCommitChunksMerge"
                                         << "TestDB.TestColl"
                                         << "collUUID" << collUUID.toBSON() << "shard"
                                         << "shard0000"
                                         << "$db"
                                         << "admin")),
        DBException,
        40414);
}

TEST(ConfigSvrMergeChunks, MissingShardIdErrors) {
    auto collUUID = UUID::gen();
    ASSERT_THROWS_CODE(
        ConfigSvrMergeChunks::parse(ctx,
                                    BSON("_configsvrCommitChunksMerge"
                                         << "TestDB.TestColl"
                                         << "collUUID" << collUUID.toBSON() << "chunkRange"
                                         << chunkRange.toBSON() << "$db"
                                         << "admin")),
        DBException,
        40414);
}

TEST(ConfigSvrMergeChunks, WrongNamespaceTypeErrors) {
    auto collUUID = UUID::gen();
    ASSERT_THROWS_CODE(
        ConfigSvrMergeChunks::parse(ctx,
                                    BSON("_configsvrCommitChunksMerge"
                                         << 1234 << "collUUID" << collUUID.toBSON() << "chunkRange"
                                         << chunkRange.toBSON() << "shard"
                                         << "shard0000"
                                         << "$db"
                                         << "admin")),
        DBException,
        ErrorCodes::TypeMismatch);
}

TEST(ConfigSvrMergeChunks, WrongcollUUIDTypeErrors) {
    ASSERT_THROWS_CODE(ConfigSvrMergeChunks::parse(ctx,
                                                   BSON("_configsvrCommitChunksMerge"
                                                        << "TestDB.TestColl"
                                                        << "collUUID" << 1234 << "chunkRange"
                                                        << chunkRange.toBSON() << "shard"
                                                        << "shard0000"
                                                        << "$db"
                                                        << "admin")),
                       DBException,
                       ErrorCodes::TypeMismatch);
}


TEST(ConfigSvrMergeChunks, WrongChunkRangeTypeErrors) {
    auto collUUID = UUID::gen();
    ASSERT_THROWS_CODE(ConfigSvrMergeChunks::parse(ctx,
                                                   BSON("_configsvrCommitChunksMerge"
                                                        << "TestDB.TestColl"
                                                        << "collUUID" << collUUID.toBSON()
                                                        << "chunkRange" << 1234 << "shard"
                                                        << "shard0000"
                                                        << "$db"
                                                        << "admin")),
                       DBException,
                       ErrorCodes::TypeMismatch);
}

TEST(ConfigSvrMergeChunks, WrongShardIdTypeErrors) {
    auto collUUID = UUID::gen();
    ASSERT_THROWS_CODE(
        ConfigSvrMergeChunks::parse(ctx,
                                    BSON("_configsvrCommitChunksMerge"
                                         << "TestDB.TestColl"
                                         << "collUUID" << collUUID.toBSON() << "chunkRange"
                                         << chunkRange.toBSON() << "shard" << 1234 << "$db"
                                         << "admin")),
        DBException,
        ErrorCodes::TypeMismatch);
}

}  // namespace
}  // namespace mongo
