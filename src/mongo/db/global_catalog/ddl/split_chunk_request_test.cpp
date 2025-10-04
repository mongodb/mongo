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

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/oid.h"
#include "mongo/db/global_catalog/ddl/split_chunk_request_type.h"
#include "mongo/db/global_catalog/type_chunk.h"
#include "mongo/db/namespace_string.h"
#include "mongo/unittest/unittest.h"

#include <memory>
#include <vector>

#include <fmt/format.h>

namespace mongo {
namespace {

using unittest::assertGet;

TEST(SplitChunkRequest, BasicValidConfigCommand) {
    auto request = assertGet(SplitChunkRequest::parseFromConfigCommand(
        BSON("_configsvrCommitChunkSplit" << "TestDB.TestColl"
                                          << "collEpoch" << OID("7fffffff0000000000000001") << "min"
                                          << BSON("a" << 1) << "max" << BSON("a" << 10)
                                          << "splitPoints" << BSON_ARRAY(BSON("a" << 5)) << "shard"
                                          << "shard0000")));
    ASSERT_EQ(NamespaceString::createNamespaceString_forTest("TestDB", "TestColl"),
              request.getNamespace());
    ASSERT_EQ(OID("7fffffff0000000000000001"), request.getEpoch());
    ASSERT(ChunkRange(BSON("a" << 1), BSON("a" << 10)) == request.getChunkRange());
    ASSERT_BSONOBJ_EQ(BSON("a" << 5), request.getSplitPoints().at(0));
    ASSERT_EQ("shard0000", request.getShardName());
}

TEST(SplitChunkRequest, ValidWithMultipleSplits) {
    auto request = assertGet(SplitChunkRequest::parseFromConfigCommand(BSON(
        "_configsvrCommitChunkSplit" << "TestDB.TestColl"
                                     << "collEpoch" << OID("7fffffff0000000000000001") << "min"
                                     << BSON("a" << 1) << "max" << BSON("a" << 10) << "splitPoints"
                                     << BSON_ARRAY(BSON("a" << 5) << BSON("a" << 7)) << "shard"
                                     << "shard0000")));
    ASSERT_EQ(NamespaceString::createNamespaceString_forTest("TestDB", "TestColl"),
              request.getNamespace());
    ASSERT_EQ(OID("7fffffff0000000000000001"), request.getEpoch());
    ASSERT(ChunkRange(BSON("a" << 1), BSON("a" << 10)) == request.getChunkRange());
    ASSERT_BSONOBJ_EQ(BSON("a" << 5), request.getSplitPoints().at(0));
    ASSERT_BSONOBJ_EQ(BSON("a" << 7), request.getSplitPoints().at(1));
    ASSERT_EQ("shard0000", request.getShardName());
}

TEST(SplitChunkRequest, ConfigCommandtoBSON) {
    BSONObj serializedRequest =
        BSON("_configsvrCommitChunkSplit" << "TestDB.TestColl"
                                          << "collEpoch" << OID("7fffffff0000000000000001") << "min"
                                          << BSON("a" << 1) << "max" << BSON("a" << 10)
                                          << "splitPoints" << BSON_ARRAY(BSON("a" << 5)) << "shard"
                                          << "shard0000");
    BSONObj writeConcernObj = BSON("w" << "majority");

    BSONObjBuilder cmdBuilder;
    {
        cmdBuilder.appendElements(serializedRequest);
        cmdBuilder.append("writeConcern", writeConcernObj);
    }

    auto request = assertGet(SplitChunkRequest::parseFromConfigCommand(serializedRequest));
    auto requestToBSON = request.toConfigCommandBSON(writeConcernObj);

    ASSERT_BSONOBJ_EQ(cmdBuilder.obj(), requestToBSON);
}

TEST(SplitChunkRequest, MissingNamespaceErrors) {
    auto request = SplitChunkRequest::parseFromConfigCommand(BSON(
        "collEpoch" << OID("7fffffff0000000000000001") << "min" << BSON("a" << 1) << "max"
                    << BSON("a" << 10) << "splitPoints" << BSON_ARRAY(BSON("a" << 5)) << "shard"
                    << "shard0000"));
    ASSERT_EQ(ErrorCodes::NoSuchKey, request.getStatus());
}

TEST(SplitChunkRequest, MissingCollEpochErrors) {
    auto request = SplitChunkRequest::parseFromConfigCommand(
        BSON("_configsvrCommitChunkSplit" << "TestDB.TestColl"
                                          << "min" << BSON("a" << 1) << "max" << BSON("a" << 10)
                                          << "splitPoints" << BSON_ARRAY(BSON("a" << 5)) << "shard"
                                          << "shard0000"));
    ASSERT_EQ(ErrorCodes::NoSuchKey, request.getStatus());
}

TEST(SplitChunkRequest, MissingChunkToSplitErrors) {
    auto request = SplitChunkRequest::parseFromConfigCommand(
        BSON("_configsvrCommitChunkSplit" << "TestDB.TestColl"
                                          << "collEpoch" << OID("7fffffff0000000000000001") << "max"
                                          << BSON("a" << 10) << "splitPoints"
                                          << BSON_ARRAY(BSON("a" << 5)) << "shard"
                                          << "shard0000"));
    ASSERT_EQ(ErrorCodes::IDLFailedToParse, request.getStatus());
}

TEST(SplitChunkRequest, MissingSplitPointErrors) {
    auto request = SplitChunkRequest::parseFromConfigCommand(
        BSON("_configsvrCommitChunkSplit" << "TestDB.TestColl"
                                          << "collEpoch" << OID("7fffffff0000000000000001") << "min"
                                          << BSON("a" << 1) << "max" << BSON("a" << 10) << "shard"
                                          << "shard0000"));
    ASSERT_EQ(ErrorCodes::NoSuchKey, request.getStatus());
}

TEST(SplitChunkRequest, MissingShardNameErrors) {
    auto request = SplitChunkRequest::parseFromConfigCommand(
        BSON("_configsvrCommitChunkSplit" << "TestDB.TestColl"
                                          << "collEpoch" << OID("7fffffff0000000000000001") << "min"
                                          << BSON("a" << 1) << "max" << BSON("a" << 10)
                                          << "splitPoints" << BSON_ARRAY(BSON("a" << 5))));
    ASSERT_EQ(ErrorCodes::NoSuchKey, request.getStatus());
}

TEST(SplitChunkRequest, WrongNamespaceTypeErrors) {
    auto request = SplitChunkRequest::parseFromConfigCommand(
        BSON("_configsvrCommitChunkSplit" << 1234 << "collEpoch" << OID("7fffffff0000000000000001")
                                          << "min" << BSON("a" << 1) << "max" << BSON("a" << 10)
                                          << "splitPoints" << BSON_ARRAY(BSON("a" << 5)) << "shard"
                                          << "shard0000"));
    ASSERT_EQ(ErrorCodes::TypeMismatch, request.getStatus());
}

TEST(SplitChunkRequest, WrongCollEpochTypeErrors) {
    auto request = SplitChunkRequest::parseFromConfigCommand(
        BSON("_configsvrCommitChunkSplit" << "TestDB.TestColl"
                                          << "collEpoch" << 1234 << "min" << BSON("a" << 1) << "max"
                                          << BSON("a" << 10) << "splitPoints"
                                          << BSON_ARRAY(BSON("a" << 5)) << "shard"
                                          << "shard0000"));
    ASSERT_EQ(ErrorCodes::TypeMismatch, request.getStatus());
}

TEST(SplitChunkRequest, WrongChunkToSplitTypeErrors) {
    auto request = SplitChunkRequest::parseFromConfigCommand(
        BSON("_configsvrCommitChunkSplit" << "TestDB.TestColl"
                                          << "collEpoch" << OID("7fffffff0000000000000001") << "min"
                                          << 1234 << "max" << BSON("a" << 10) << "splitPoints"
                                          << BSON_ARRAY(BSON("a" << 5)) << "shard"
                                          << "shard0000"));
    ASSERT_EQ(ErrorCodes::TypeMismatch, request.getStatus());
}

TEST(SplitChunkRequest, WrongSplitPointTypeErrors) {
    auto request = SplitChunkRequest::parseFromConfigCommand(
        BSON("_configsvrCommitChunkSplit" << "TestDB.TestColl"
                                          << "collEpoch" << OID("7fffffff0000000000000001") << "min"
                                          << BSON("a" << 1) << "max" << BSON("a" << 10)
                                          << "splitPoints" << 1234 << "shard"
                                          << "shard0000"));
    ASSERT_EQ(ErrorCodes::TypeMismatch, request.getStatus());
}

TEST(SplitChunkRequest, WrongShardNameTypeErrors) {
    auto request = SplitChunkRequest::parseFromConfigCommand(
        BSON("_configsvrCommitChunkSplit"
             << "TestDB.TestColl"
             << "collEpoch" << OID("7fffffff0000000000000001") << "min" << BSON("a" << 1) << "max"
             << BSON("a" << 10) << "splitPoints" << BSON_ARRAY(BSON("a" << 5)) << "shard" << 1234));
    ASSERT_EQ(ErrorCodes::TypeMismatch, request.getStatus());
}

TEST(SplitChunkRequest, InvalidNamespaceErrors) {
    auto request = SplitChunkRequest::parseFromConfigCommand(
        BSON("_configsvrCommitChunkSplit" << ""
                                          << "collEpoch" << OID("7fffffff0000000000000001") << "min"
                                          << BSON("a" << 1) << "max" << BSON("a" << 10)
                                          << "splitPoints" << BSON_ARRAY(BSON("a" << 5)) << "shard"
                                          << "shard0000"));
    ASSERT_EQ(ErrorCodes::InvalidNamespace, request.getStatus());
}

TEST(SplitChunkRequest, EmptyChunkToSplitErrors) {
    auto request = SplitChunkRequest::parseFromConfigCommand(
        BSON("_configsvrCommitChunkSplit" << "TestDB.TestColl"
                                          << "collEpoch" << OID("7fffffff0000000000000001") << "min"
                                          << BSONObj() << "max" << BSON("a" << 10) << "splitPoints"
                                          << BSON_ARRAY(BSON("a" << 5)) << "shard"
                                          << "shard0000"));
    ASSERT_EQ(ErrorCodes::BadValue, request.getStatus());
}

TEST(SplitChunkRequest, EmptySplitPointsErrors) {
    auto request = SplitChunkRequest::parseFromConfigCommand(
        BSON("_configsvrCommitChunkSplit" << "TestDB.TestColl"
                                          << "collEpoch" << OID("7fffffff0000000000000001") << "min"
                                          << BSON("a" << 1) << "max" << BSON("a" << 10)
                                          << "splitPoints" << BSONArray() << "shard"
                                          << "shard0000"));
    ASSERT_EQ(ErrorCodes::InvalidOptions, request.getStatus());
}
}  // namespace

}  // namespace mongo
