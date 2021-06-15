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

#include "mongo/platform/basic.h"

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/s/request_types/merge_chunk_request_gen.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

using unittest::assertGet;

IDLParserErrorContext ctx("_configsvrCommitChunkMerge");

TEST(ConfigSvrMergeChunk, BasicValidConfigCommand) {
    auto request = ConfigSvrMergeChunk::parse(
        ctx,
        BSON("_configsvrCommitChunkMerge"
             << "TestDB.TestColl"
             << "shard"
             << "shard0000"
             << "collEpoch" << OID("7fffffff0000000000000001") << "chunkBoundaries"
             << BSON_ARRAY(BSON("a" << 1) << BSON("a" << 5) << BSON("a" << 10)) << "$db"
             << "admin"));
    ASSERT_EQ(NamespaceString("TestDB", "TestColl"), request.getCommandParameter());
    ASSERT_EQ(OID("7fffffff0000000000000001"), request.getEpoch());
    ASSERT_BSONOBJ_EQ(BSON("a" << 1), request.getChunkBoundaries().at(0));
    ASSERT_BSONOBJ_EQ(BSON("a" << 5), request.getChunkBoundaries().at(1));
    ASSERT_BSONOBJ_EQ(BSON("a" << 10), request.getChunkBoundaries().at(2));
    ASSERT_EQ("shard0000", request.getShard().toString());
    ASSERT_EQ("admin", request.getDbName());
}

TEST(ConfigSvrMergeChunk, ConfigCommandtoBSON) {
    BSONObj serializedRequest =
        BSON("_configsvrCommitChunkMerge"
             << "TestDB.TestColl"
             << "shard"
             << "shard0000"
             << "collEpoch" << OID("7fffffff0000000000000001") << "chunkBoundaries"
             << BSON_ARRAY(BSON("a" << 1) << BSON("a" << 5) << BSON("a" << 10)) << "validAfter"
             << Timestamp{100});
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

    auto request = ConfigSvrMergeChunk::parse(ctx, appendDB(serializedRequest));
    request.setWriteConcern(writeConcernObj);
    auto requestToBSON = request.toBSON(BSONObj());

    ASSERT_BSONOBJ_EQ(cmdBuilder.obj(), requestToBSON);
}

TEST(ConfigSvrMergeChunk, MissingNameSpaceErrors) {
    ASSERT_THROWS_CODE(

        ConfigSvrMergeChunk::parse(
            ctx,
            BSON("collEpoch" << OID("7fffffff0000000000000001") << "chunkBoundaries"
                             << BSON_ARRAY(BSON("a" << 1) << BSON("a" << 5) << BSON("a" << 10))
                             << "shard"
                             << "shard0000"
                             << "$db"
                             << "admin")),
        mongo::DBException,
        40414);
}

TEST(ConfigSvrMergeChunk, MissingCollEpochErrors) {
    ASSERT_THROWS_CODE(
        ConfigSvrMergeChunk::parse(
            ctx,
            BSON("_configsvrCommitChunkMerge"
                 << "TestDB.TestColl"
                 << "chunkBoundaries"
                 << BSON_ARRAY(BSON("a" << 1) << BSON("a" << 5) << BSON("a" << 10)) << "shard"
                 << "shard0000"
                 << "$db"
                 << "admin")),
        mongo::DBException,
        40414);
}

TEST(ConfigSvrMergeChunk, MissingChunkBoundariesErrors) {
    ASSERT_THROWS_CODE(

        ConfigSvrMergeChunk::parse(ctx,
                                   BSON("_configsvrCommitChunkMerge"
                                        << "TestDB.TestColl"
                                        << "collEpoch" << OID("7fffffff0000000000000001") << "shard"
                                        << "shard0000"
                                        << "$db"
                                        << "admin")),
        mongo::DBException,
        40414);
}

TEST(ConfigSvrMergeChunk, MissingShardNameErrors) {
    ASSERT_THROWS_CODE(

        ConfigSvrMergeChunk::parse(
            ctx,
            BSON("_configsvrCommitChunkMerge"
                 << "TestDB.TestColl"
                 << "collEpoch" << OID("7fffffff0000000000000001") << "chunkBoundaries"
                 << BSON_ARRAY(BSON("a" << 1) << BSON("a" << 5) << BSON("a" << 10)) << "$db"
                 << "admin")),
        mongo::DBException,
        40414);
}

TEST(ConfigSvrMergeChunk, WrongNamespaceTypeErrors) {
    ASSERT_THROWS_CODE(

        ConfigSvrMergeChunk::parse(
            ctx,
            BSON("_configsvrCommitChunkMerge"
                 << 1234 << "collEpoch" << OID("7fffffff0000000000000001") << "chunkBoundaries"
                 << BSON_ARRAY(BSON("a" << 1) << BSON("a" << 5) << BSON("a" << 10)) << "shard"
                 << "shard0000"
                 << "$db"
                 << "admin")),
        DBException,
        ErrorCodes::TypeMismatch);
}


TEST(ConfigSvrMergeChunk, WrongCollEpochTypeErrors) {
    ASSERT_THROWS_CODE(

        ConfigSvrMergeChunk::parse(
            ctx,
            BSON("_configsvrCommitChunkMerge"
                 << "TestDB.TestColl"
                 << "collEpoch" << 1234 << "chunkBoundaries"
                 << BSON_ARRAY(BSON("a" << 1) << BSON("a" << 5) << BSON("a" << 10)) << "shard"
                 << "shard0000"
                 << "$db"
                 << "admin")),
        DBException,
        ErrorCodes::TypeMismatch);
}

TEST(ConfigSvrMergeChunk, WrongChunkBoundariesTypeErrors) {
    ASSERT_THROWS_CODE(

        ConfigSvrMergeChunk::parse(ctx,
                                   BSON("_configsvrCommitChunkMerge"
                                        << "TestDB.TestColl"
                                        << "collEpoch" << OID("7fffffff0000000000000001")
                                        << "chunkBoundaries" << 1234 << "shard"
                                        << "shard0000"
                                        << "$db"
                                        << "admin")),
        DBException,
        ErrorCodes::TypeMismatch);
}

TEST(ConfigSvrMergeChunk, WrongShardNameTypeErrors) {
    ASSERT_THROWS_CODE(

        ConfigSvrMergeChunk::parse(
            ctx,
            BSON("_configsvrCommitChunkMerge"
                 << "TestDB.TestColl"
                 << "collEpoch" << OID("7fffffff0000000000000001") << "chunkBoundaries"
                 << BSON_ARRAY(BSON("a" << 1) << BSON("a" << 5) << BSON("a" << 10)) << "shard"
                 << 1234 << "$db"
                 << "admin")),
        DBException,
        ErrorCodes::TypeMismatch);
}

//// IDL validators do not work on command value
// TEST(ConfigSvrMergeChunk, InvalidNamespaceErrors) {
//     auto request = MergeChunkRequest::parseFromConfigBSONCommand(
//         BSON("_configsvrCommitChunkMerge"
//              << ""
//              << "collEpoch" << OID("7fffffff0000000000000001") << "chunkBoundaries"
//              << BSON_ARRAY(BSON("a" << 1) << BSON("a" << 5) << BSON("a" << 10)) << "shard"
//              << "shard0000" << "$db" << "admin"));
//     ASSERT_EQ(ErrorCodes::InvalidNamespace, request.getStatus());
// }

TEST(ConfigSvrMergeChunk, EmptyChunkBoundariesErrors) {
    auto req = ConfigSvrMergeChunk::parse(ctx,
                                          BSON("_configsvrCommitChunkMerge"
                                               << "TestDB.TestColl"
                                               << "collEpoch" << OID("7fffffff0000000000000001")
                                               << "chunkBoundaries" << BSONArray() << "shard"
                                               << "shard0000"
                                               << "$db"
                                               << "admin"));
    // trigger validator (bit useless)
    ASSERT_THROWS_CODE(
        req.setChunkBoundaries(req.getChunkBoundaries()), DBException, ErrorCodes::InvalidOptions);
}

TEST(ConfigSvrMergeChunk, TooFewChunkBoundariesErrors) {
    auto req = ConfigSvrMergeChunk::parse(
        ctx,
        BSON("_configsvrCommitChunkMerge"
             << "TestDB.TestColl"
             << "collEpoch" << OID("7fffffff0000000000000001") << "chunkBoundaries"
             << BSON_ARRAY(BSON("a" << 1) << BSON("a" << 10)) << "shard"
             << "shard0000"
             << "$db"
             << "admin"));
    // trigger validator (bit useless)

    ASSERT_THROWS_CODE(
        req.setChunkBoundaries(req.getChunkBoundaries()), DBException, ErrorCodes::InvalidOptions);
}

}  // namespace
}  // namespace mongo
