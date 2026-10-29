// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


#include "mongo/db/repl/repl_set_heartbeat_args_v1.h"

#include "mongo/db/repl/optime.h"
#include "mongo/rpc/metadata/repl_set_metadata.h"
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

TEST(ReplSetHeartbeatArgsV1, PrimaryOptimeFieldsParsedFromReplData) {
    // Verify that $replData sub-document is stored in getExtra() when present in the
    // heartbeat command body (placed in request metadata, merged by OP_MSG).
    OpTime primaryOpTime({100, 3}, 5);
    Date_t primaryWallTime = Date_t() + Seconds(12345);
    // Checkpoint timestamp gossiped alongside optimes.
    Timestamp primaryCheckpointTs(90, 1);
    // Committed optime gossiped alongside optimes.
    OpTime primaryCommittedOpTime({95, 2}, 5);

    BSONObjBuilder cmdBuilder;
    cmdBuilder.append(kSetNameFieldName, "mySet");
    cmdBuilder.appendNumber(kConfigVersionFieldName, 1LL);
    cmdBuilder.appendNumber(kConfigTermFieldName, -1LL);
    cmdBuilder.append(kSenderHostFieldName, "host:1");
    cmdBuilder.appendNumber(kSenderIdFieldName, 0LL);
    cmdBuilder.appendNumber(kTermFieldName, 5LL);
    cmdBuilder.append(kPrimaryIdFieldName, 0LL);
    {
        BSONObjBuilder replDataBuilder(cmdBuilder.subobjStart(rpc::kReplSetMetadataFieldName));
        primaryOpTime.append("lastAppliedOpTime", &replDataBuilder);
        replDataBuilder.appendDate("lastAppliedWallTime", primaryWallTime);
        replDataBuilder.append("lastCheckpointTimestamp", primaryCheckpointTs);
        primaryCommittedOpTime.append("lastCommittedOpTime", &replDataBuilder);
    }

    ReplSetHeartbeatArgsV1 parsed;
    ASSERT_OK(parsed.initialize(cmdBuilder.obj()));

    auto extra = parsed.getExtra();
    ASSERT_TRUE(extra.has_value());
    auto opTimeElem = (*extra)["lastAppliedOpTime"];
    auto wallTimeElem = (*extra)["lastAppliedWallTime"];
    auto checkpointElem = (*extra)["lastCheckpointTimestamp"];
    auto committedElem = (*extra)["lastCommittedOpTime"];
    ASSERT_FALSE(opTimeElem.eoo());
    ASSERT_FALSE(wallTimeElem.eoo());
    ASSERT_FALSE(checkpointElem.eoo());
    ASSERT_FALSE(committedElem.eoo());
    ASSERT_EQUALS(OpTime::parseFromOplogEntry(opTimeElem.Obj()).getValue(), primaryOpTime);
    ASSERT_EQUALS(wallTimeElem.Date(), primaryWallTime);
    ASSERT_EQUALS(checkpointElem.timestamp(), primaryCheckpointTs);
    ASSERT_EQUALS(OpTime::parseFromOplogEntry(committedElem.Obj()).getValue(),
                  primaryCommittedOpTime);
}

TEST(ReplSetHeartbeatArgsV1, PrimaryOptimeFieldsAbsentByDefault) {
    // When $replData is absent, getExtra() should be boost::none.
    ReplSetHeartbeatArgsV1 args;
    args.setSetName("mySet");
    args.setConfigVersion(1);
    args.setTerm(5);
    args.setSenderHost(HostAndPort("host:1"));

    BSONObj serialized = args.toBSON();

    ASSERT_FALSE(serialized.hasField(rpc::kReplSetMetadataFieldName));

    ReplSetHeartbeatArgsV1 parsed;
    ASSERT_OK(parsed.initialize(serialized));

    ASSERT_FALSE(parsed.getExtra().has_value());
}

}  // namespace
}  // namespace repl
}  // namespace mongo
