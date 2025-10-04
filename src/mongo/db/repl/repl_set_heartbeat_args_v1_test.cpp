/**
 *    Copyright (C) 2024-present MongoDB, Inc.
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


#include "repl_set_heartbeat_args_v1.h"

#include "mongo/unittest/unittest.h"

#include <memory>
#include <ostream>

namespace mongo {
namespace repl {
namespace {

namespace {

const std::string kCheckEmptyFieldName = "checkEmpty";
const std::string kConfigVersionFieldName = "configVersion";
const std::string kConfigTermFieldName = "configTerm";
const std::string kHeartbeatVersionFieldName = "hbv";
const std::string kSenderHostFieldName = "from";
const std::string kSenderIdFieldName = "fromId";
const std::string kSetNameFieldName = "replSetHeartbeat";
const std::string kTermFieldName = "term";
const std::string kPrimaryIdFieldName = "primaryId";

}  // namespace

TEST(ReplSetHeartbeatArgsV1, ValidArgsInitializeCorrectly) {
    ReplSetHeartbeatArgsV1 args;
    BSONObj argsObj =
        BSON(kCheckEmptyFieldName << true << kConfigVersionFieldName << 0 << kConfigTermFieldName
                                  << 0 << kHeartbeatVersionFieldName << 1 << kSenderHostFieldName
                                  << "host:1" << kSenderIdFieldName << 0 << kSetNameFieldName << " "
                                  << kTermFieldName << 0 << kPrimaryIdFieldName << 0);
    Status result = args.initialize(argsObj);
    ASSERT_OK(result);
    BSONObjBuilder builder;
    args.addToBSON(&builder);
    ASSERT_BSONOBJ_EQ_UNORDERED(argsObj, builder.obj());
}

TEST(ReplSetHeartbeatArgsV1, ValidArgsInitializeCorrectlyRequiredOnly) {
    ReplSetHeartbeatArgsV1 args;
    BSONObj argsObj =
        BSON(kConfigVersionFieldName << 1 << kSenderHostFieldName << "host:1" << kSetNameFieldName
                                     << " " << kTermFieldName << 1);
    Status result = args.initialize(argsObj);
    ASSERT_OK(result);
    BSONObj targetObj =
        BSON(kConfigVersionFieldName << 1 << kSenderHostFieldName << "host:1" << kSetNameFieldName
                                     << " " << kTermFieldName << 1 << kConfigTermFieldName << -1
                                     << kSenderIdFieldName << -1 << kPrimaryIdFieldName << -1);
    BSONObjBuilder argsBuilder;
    args.addToBSON(&argsBuilder);
    ASSERT_BSONOBJ_EQ_UNORDERED(argsBuilder.obj(), targetObj);
}

TEST(ReplSetHeartbeatArgsV1, CheckEmptyInvalidType) {
    ReplSetHeartbeatArgsV1 args;
    Status result = args.initialize(BSON(
        kCheckEmptyFieldName << "invalid" << kConfigVersionFieldName << 0 << kSenderHostFieldName
                             << " " << kSetNameFieldName << " " << kTermFieldName << 0));
    ASSERT_EQUALS(result, ErrorCodes::TypeMismatch);
}

TEST(ReplSetHeartbeatArgsV1, ConfigVersionInvalidType) {
    ReplSetHeartbeatArgsV1 args;
    Status result = args.initialize(BSON(kConfigVersionFieldName
                                         << "invalid" << kSenderHostFieldName << " "
                                         << kSetNameFieldName << " " << kTermFieldName << 0));
    ASSERT_EQUALS(result, ErrorCodes::TypeMismatch);
}

TEST(ReplSetHeartbeatArgsV1, ConfigTermInvalidType) {
    ReplSetHeartbeatArgsV1 args;
    Status result = args.initialize(BSON(
        kConfigVersionFieldName << 0 << kConfigTermFieldName << "invalid" << kSenderHostFieldName
                                << " " << kSetNameFieldName << " " << kTermFieldName << 0));
    ASSERT_EQUALS(result, ErrorCodes::TypeMismatch);
}

TEST(ReplSetHeartbeatArgsV1, HeartbeatVersionInvalidType) {
    ReplSetHeartbeatArgsV1 args;
    Status result =
        args.initialize(BSON(kConfigVersionFieldName
                             << 0 << kHeartbeatVersionFieldName << "invalid" << kSenderHostFieldName
                             << " " << kSetNameFieldName << " " << kTermFieldName << 0));
    ASSERT_EQUALS(result, ErrorCodes::TypeMismatch);
}

TEST(ReplSetHeartbeatArgsV1, HeartbeatVersionInvalidValue) {
    ReplSetHeartbeatArgsV1 args;
    Status result = args.initialize(BSON(kConfigVersionFieldName
                                         << 0 << kHeartbeatVersionFieldName << 999  // invalid
                                         << kSenderHostFieldName << " " << kSetNameFieldName << " "
                                         << kTermFieldName << 0));
    ASSERT_EQUALS(result, ErrorCodes::duplicateCodeForTest(40666));
}

TEST(ReplSetHeartbeatArgsV1, SenderIdInvalidType) {
    ReplSetHeartbeatArgsV1 args;
    Status result = args.initialize(
        BSON(kConfigVersionFieldName << 0 << kSenderIdFieldName << "invalid" << kSenderHostFieldName
                                     << " " << kSetNameFieldName << " " << kTermFieldName << 0));
    ASSERT_EQUALS(result, ErrorCodes::TypeMismatch);
}

TEST(ReplSetHeartbeatArgsV1, SenderHostInvalidType) {
    ReplSetHeartbeatArgsV1 args;
    Status result = args.initialize(BSON(kConfigVersionFieldName
                                         << 0 << kSenderHostFieldName << 999  // invalid
                                         << kSetNameFieldName << " " << kTermFieldName << 0));
    ASSERT_EQUALS(result, ErrorCodes::TypeMismatch);
}

TEST(ReplSetHeartbeatArgsV1, SenderHostInvalidValue) {
    ReplSetHeartbeatArgsV1 args;
    Status result = args.initialize(BSON(kConfigVersionFieldName << 0 << kSenderHostFieldName
                                                                 << "[invalid]" << kSetNameFieldName
                                                                 << " " << kTermFieldName << 0));
    ASSERT_EQUALS(result, ErrorCodes::FailedToParse);
}

TEST(ReplSetHeartbeatArgsV1, PrimaryIdInvalidType) {
    ReplSetHeartbeatArgsV1 args;
    Status result = args.initialize(BSON(
        kConfigVersionFieldName << 0 << kSenderHostFieldName << " " << kPrimaryIdFieldName
                                << "invalid" << kSetNameFieldName << " " << kTermFieldName << 0));
    ASSERT_EQUALS(result, ErrorCodes::TypeMismatch);
}

TEST(ReplSetHeartbeatArgsV1, TermInvalidType) {
    ReplSetHeartbeatArgsV1 args;
    Status result = args.initialize(BSON(kConfigVersionFieldName << 0 << kSenderHostFieldName << " "
                                                                 << kSetNameFieldName << " "
                                                                 << kTermFieldName << "invalid"));
    ASSERT_EQUALS(result, ErrorCodes::TypeMismatch);
}

TEST(ReplSetHeartbeatArgsV1, SetNameInvalidType) {
    ReplSetHeartbeatArgsV1 args;
    Status result =
        args.initialize(BSON(kConfigVersionFieldName << 0 << kSenderHostFieldName << " "
                                                     << kSetNameFieldName << 999  // invalid
                                                     << kTermFieldName << 0));
    ASSERT_EQUALS(result, ErrorCodes::TypeMismatch);
}

TEST(ReplSetHeartbeatArgs, AcceptsUnknownField) {
    ReplSetHeartbeatArgsV1 args;
    BSONObjBuilder builder =
        BSON(kCheckEmptyFieldName << true << kConfigVersionFieldName << 0 << kConfigTermFieldName
                                  << 0 << kHeartbeatVersionFieldName << 1 << kSenderHostFieldName
                                  << "host:1" << kSenderIdFieldName << 0 << kSetNameFieldName << " "
                                  << kTermFieldName << 0 << kPrimaryIdFieldName << 0);
    builder.append("unknownField", 1);
    BSONObj argsObj = builder.obj();
    ASSERT_OK(args.initialize(argsObj));

    // The serialized object should be the same as the original except for the unknown field.
    BSONObjBuilder builder2;
    args.addToBSON(&builder2);
    builder2.append("unknownField", 1);
    ASSERT_BSONOBJ_EQ_UNORDERED(argsObj, builder2.obj());
}

}  // namespace
}  // namespace repl
}  // namespace mongo
