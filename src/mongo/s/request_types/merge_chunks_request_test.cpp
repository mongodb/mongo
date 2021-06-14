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
#include "mongo/s/request_types/merge_chunks_request_type.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

using unittest::assertGet;

ChunkRange chunkRange(BSON("a" << 1), BSON("a" << 10));

TEST(MergeChunksRequest, BasicValidConfigCommand) {
    auto collUUID = UUID::gen();
    auto request = assertGet(MergeChunksRequest::parseFromConfigCommand(
        BSON("_configsvrCommitChunksMerge"
             << "TestDB.TestColl"
             << "collUUID" << collUUID.toBSON() << "chunkRange" << chunkRange.toBSON() << "shard"
             << "shard0000")));
    ASSERT_EQ(NamespaceString("TestDB", "TestColl"), request.getNamespace());
    ASSERT_TRUE(collUUID == request.getCollectionUUID());
    ASSERT_TRUE(chunkRange == request.getChunkRange());
    ASSERT_EQ("shard0000", request.getShardId().toString());
}

TEST(MergeChunksRequest, ConfigCommandtoBSON) {
    auto collUUID = UUID::gen();
    BSONObj serializedRequest =
        BSON("_configsvrCommitChunksMerge"
             << "TestDB.TestColl"
             << "collUUID" << collUUID.toBSON() << "chunkRange" << chunkRange.toBSON() << "shard"
             << "shard0000"
             << "validAfter" << Timestamp{100});
    BSONObj writeConcernObj = BSON("w"
                                   << "majority");

    BSONObjBuilder cmdBuilder;
    {
        cmdBuilder.appendElements(serializedRequest);
        cmdBuilder.append("writeConcern", writeConcernObj);
    }

    auto request = assertGet(MergeChunksRequest::parseFromConfigCommand(serializedRequest));
    auto requestToBSON = request.toConfigCommandBSON(writeConcernObj);

    ASSERT_BSONOBJ_EQ(cmdBuilder.obj(), requestToBSON);
}

TEST(MergeChunksRequest, MissingNameSpaceErrors) {
    auto collUUID = UUID::gen();
    auto request = MergeChunksRequest::parseFromConfigCommand(
        BSON("collUUID" << collUUID.toBSON() << "chunkRange" << chunkRange.toBSON() << "shard"
                        << "shard0000"));
    ASSERT_EQ(ErrorCodes::NoSuchKey, request.getStatus());
}

TEST(MergeChunksRequest, MissingcollUUIDErrors) {
    auto request = MergeChunksRequest::parseFromConfigCommand(BSON("_configsvrCommitChunksMerge"
                                                                   << "TestDB.TestColl"
                                                                   << "chunkRange"
                                                                   << chunkRange.toBSON() << "shard"
                                                                   << "shard0000"));
    ASSERT_EQ(ErrorCodes::NoSuchKey, request.getStatus());
}

TEST(MergeChunksRequest, MissingChunkRangeErrors) {
    auto collUUID = UUID::gen();
    auto request = MergeChunksRequest::parseFromConfigCommand(BSON("_configsvrCommitChunksMerge"
                                                                   << "TestDB.TestColl"
                                                                   << "collUUID"
                                                                   << collUUID.toBSON() << "shard"
                                                                   << "shard0000"));
    ASSERT_EQ(ErrorCodes::NoSuchKey, request.getStatus());
}

TEST(MergeChunksRequest, MissingShardIdErrors) {
    auto collUUID = UUID::gen();
    auto request = MergeChunksRequest::parseFromConfigCommand(
        BSON("_configsvrCommitChunksMerge"
             << "TestDB.TestColl"
             << "collUUID" << collUUID.toBSON() << "chunkRange" << chunkRange.toBSON()));
    ASSERT_EQ(ErrorCodes::NoSuchKey, request.getStatus());
}

TEST(MergeChunksRequest, WrongNamespaceTypeErrors) {
    auto collUUID = UUID::gen();
    auto request = MergeChunksRequest::parseFromConfigCommand(
        BSON("_configsvrCommitChunksMerge" << 1234 << "collUUID" << collUUID.toBSON()
                                           << "chunkRange" << chunkRange.toBSON() << "shard"
                                           << "shard0000"));
    ASSERT_EQ(ErrorCodes::TypeMismatch, request.getStatus());
}

TEST(MergeChunksRequest, WrongcollUUIDTypeErrors) {
    auto request = MergeChunksRequest::parseFromConfigCommand(
        BSON("_configsvrCommitChunksMerge"
             << "TestDB.TestColl"
             << "collUUID" << 1234 << "chunkRange" << chunkRange.toBSON() << "shard"
             << "shard0000"));
    ASSERT_EQ(ErrorCodes::TypeMismatch, request.getStatus());
}

TEST(MergeChunksRequest, WrongChunkRangeTypeErrors) {
    auto collUUID = UUID::gen();
    auto request = MergeChunksRequest::parseFromConfigCommand(
        BSON("_configsvrCommitChunksMerge"
             << "TestDB.TestColl"
             << "collUUID" << collUUID.toBSON() << "chunkRange" << 1234 << "shard"
             << "shard0000"));
    ASSERT_EQ(ErrorCodes::TypeMismatch, request.getStatus());
}

TEST(MergeChunksRequest, WrongShardIdTypeErrors) {
    auto collUUID = UUID::gen();
    auto request = MergeChunksRequest::parseFromConfigCommand(
        BSON("_configsvrCommitChunksMerge"
             << "TestDB.TestColl"
             << "collUUID" << collUUID.toBSON() << "chunkRange" << chunkRange.toBSON() << "shard"
             << 1234));
    ASSERT_EQ(ErrorCodes::TypeMismatch, request.getStatus());
}

TEST(MergeChunksRequest, InvalidNamespaceErrors) {
    auto collUUID = UUID::gen();
    auto request = MergeChunksRequest::parseFromConfigCommand(
        BSON("_configsvrCommitChunksMerge"
             << ""
             << "collUUID" << collUUID.toBSON() << "chunkRange" << chunkRange.toBSON() << "shard"
             << "shard0000"));
    ASSERT_EQ(ErrorCodes::InvalidNamespace, request.getStatus());
}

}  // namespace
}  // namespace mongo
