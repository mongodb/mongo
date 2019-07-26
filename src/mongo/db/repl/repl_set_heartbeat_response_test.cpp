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


#include "mongo/db/repl/repl_set_heartbeat_response.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"

namespace mongo {
namespace repl {
namespace {

using std::unique_ptr;

bool stringContains(const std::string& haystack, const std::string& needle) {
    return haystack.find(needle) != std::string::npos;
}

TEST(ReplSetHeartbeatResponse, DefaultConstructThenSlowlyBuildToFullObj) {
    int fieldsSet = 1;
    ReplSetHeartbeatResponse hbResponse;
    ReplSetHeartbeatResponse hbResponseObjRoundTripChecker;
    OpTime durableOpTime = OpTime(Timestamp(10), 0);
    Date_t durableWallTime = Date_t() + Seconds(durableOpTime.getSecs());
    OpTime appliedOpTime = OpTime(Timestamp(50), 0);
    Date_t appliedWallTime = Date_t() + Seconds(appliedOpTime.getSecs());
    ASSERT_EQUALS(false, hbResponse.hasState());
    ASSERT_EQUALS(false, hbResponse.hasElectionTime());
    ASSERT_EQUALS(false, hbResponse.hasDurableOpTime());
    ASSERT_EQUALS(false, hbResponse.hasAppliedOpTime());
    ASSERT_EQUALS(false, hbResponse.hasConfig());
    ASSERT_EQUALS("", hbResponse.getReplicaSetName());
    ASSERT_EQUALS(HostAndPort(), hbResponse.getSyncingTo());
    ASSERT_EQUALS(-1, hbResponse.getConfigVersion());

    BSONObj hbResponseObj = hbResponse.toBSON();
    ASSERT_EQUALS(fieldsSet, hbResponseObj.nFields());

    Status initializeResult = Status::OK();
    ASSERT_EQUALS(hbResponseObj.toString(), hbResponseObjRoundTripChecker.toString());

    // set version
    hbResponse.setConfigVersion(1);
    ++fieldsSet;
    // set setname
    hbResponse.setSetName("rs0");
    ++fieldsSet;
    // set electionTime
    hbResponse.setElectionTime(Timestamp(10, 0));
    ++fieldsSet;
    // set durableOpTime
    hbResponse.setDurableOpTimeAndWallTime({durableOpTime, durableWallTime});
    fieldsSet += 2;  // OpTime and WallTime are separate fields
    // set appliedOpTime
    hbResponse.setAppliedOpTimeAndWallTime({appliedOpTime, appliedWallTime});
    fieldsSet += 2;  // OpTime and WallTime are separate fields
    // set config
    ReplSetConfig config;
    hbResponse.setConfig(config);
    ++fieldsSet;
    // set state
    hbResponse.setState(MemberState(MemberState::RS_SECONDARY));
    ++fieldsSet;
    // set syncingTo
    hbResponse.setSyncingTo(HostAndPort("syncTarget"));
    ++fieldsSet;
    ASSERT_EQUALS(true, hbResponse.hasState());
    ASSERT_EQUALS(true, hbResponse.hasElectionTime());
    ASSERT_EQUALS(true, hbResponse.hasDurableOpTime());
    ASSERT_EQUALS(true, hbResponse.hasAppliedOpTime());
    ASSERT_EQUALS(true, hbResponse.hasConfig());
    ASSERT_EQUALS("rs0", hbResponse.getReplicaSetName());
    ASSERT_EQUALS(MemberState(MemberState::RS_SECONDARY).toString(),
                  hbResponse.getState().toString());
    ASSERT_EQUALS(HostAndPort("syncTarget"), hbResponse.getSyncingTo());
    ASSERT_EQUALS(1, hbResponse.getConfigVersion());
    ASSERT_EQUALS(Timestamp(10, 0), hbResponse.getElectionTime());
    ASSERT_EQUALS(durableOpTime, hbResponse.getDurableOpTime());
    ASSERT_EQUALS(durableWallTime, hbResponse.getDurableOpTimeAndWallTime().wallTime);
    ASSERT_EQUALS(appliedOpTime, hbResponse.getAppliedOpTime());
    ASSERT_EQUALS(appliedWallTime, hbResponse.getAppliedOpTimeAndWallTime().wallTime);
    ASSERT_EQUALS(config.toBSON().toString(), hbResponse.getConfig().toBSON().toString());

    hbResponseObj = hbResponse.toBSON();
    ASSERT_EQUALS(fieldsSet, hbResponseObj.nFields());
    ASSERT_EQUALS("rs0", hbResponseObj["set"].String());
    ASSERT_EQUALS(1, hbResponseObj["v"].Number());
    ASSERT_EQUALS(Timestamp(10, 0), hbResponseObj["electionTime"].timestamp());
    ASSERT_EQUALS(Timestamp(0, 50), hbResponseObj["opTime"]["ts"].timestamp());
    ASSERT_EQUALS(Timestamp(0, 10), hbResponseObj["durableOpTime"]["ts"].timestamp());
    ASSERT_EQUALS(config.toBSON().toString(), hbResponseObj["config"].Obj().toString());
    ASSERT_EQUALS(2, hbResponseObj["state"].numberLong());
    ASSERT_EQUALS("syncTarget:27017", hbResponseObj["syncingTo"].String());

    initializeResult =
        hbResponseObjRoundTripChecker.initialize(hbResponseObj, 0, /*requireWallTime*/ true);
    ASSERT_EQUALS(Status::OK(), initializeResult);
    ASSERT_EQUALS(hbResponseObj.toString(), hbResponseObjRoundTripChecker.toBSON().toString());
}

TEST(ReplSetHeartbeatResponse, InitializeWrongElectionTimeType) {
    ReplSetHeartbeatResponse hbResponse;
    BSONObj initializerObj = BSON("ok" << 1.0 << "electionTime"
                                       << "hello");
    Status result = hbResponse.initialize(initializerObj, 0, /*requireWallTime*/ true);
    ASSERT_EQUALS(ErrorCodes::TypeMismatch, result);
    ASSERT_EQUALS(
        "Expected \"electionTime\" field in response to replSetHeartbeat command to "
        "have type Date, but found type string",
        result.reason());
}

TEST(ReplSetHeartbeatResponse, InitializeWrongDurableOpTimeType) {
    ReplSetHeartbeatResponse hbResponse;
    BSONObj initializerObj = BSON("ok" << 1.0 << "durableOpTime"
                                       << "hello");
    Status result = hbResponse.initialize(initializerObj, 0, /*requireWallTime*/ true);
    ASSERT_EQUALS(ErrorCodes::TypeMismatch, result);
    ASSERT_EQUALS("\"durableOpTime\" had the wrong type. Expected object, found string",
                  result.reason());

    BSONObj initializerObj2 = BSON("ok" << 1.0 << "durableOpTime" << OpTime().getTimestamp());
    Status result2 = hbResponse.initialize(initializerObj2, 0, /*requireWallTime*/ true);
    ASSERT_EQUALS(ErrorCodes::TypeMismatch, result2);
    ASSERT_EQUALS("\"durableOpTime\" had the wrong type. Expected object, found timestamp",
                  result2.reason());
}

TEST(ReplSetHeartbeatResponse, InitializeNoDurableWallTime) {
    ReplSetHeartbeatResponse hbResponse;
    BSONObj initializerObj =
        BSON("ok" << 1.0 << "durableOpTime" << OpTime(Timestamp(100, 0), 0).toBSON() << "opTime"
                  << OpTime(Timestamp(100, 0), 0).toBSON());
    Status result = hbResponse.initialize(initializerObj, 0, /*requireWallTime*/ true);
    ASSERT_EQUALS(ErrorCodes::NoSuchKey, result);
    ASSERT_EQUALS("Missing expected field \"durableWallTime\"", result.reason());
}

TEST(ReplSetHeartbeatResponse, InitializeWrongAppliedOpTimeType) {
    ReplSetHeartbeatResponse hbResponse;
    BSONObj initializerObj =
        BSON("ok" << 1.0 << "durableOpTime" << OpTime(Timestamp(100, 0), 0).toBSON()
                  << "durableWallTime" << Date_t() + Seconds(100) << "opTime"
                  << "hello");
    Status result = hbResponse.initialize(initializerObj, 0, /*requireWallTime*/ true);
    ASSERT_EQUALS(ErrorCodes::TypeMismatch, result);
    ASSERT_EQUALS("\"opTime\" had the wrong type. Expected object, found string", result.reason());

    initializerObj = BSON("ok" << 1.0 << "durableOpTime" << OpTime(Timestamp(100, 0), 0).toBSON()
                               << "durableWallTime" << Date_t() + Seconds(100) << "opTime"
                               << OpTime().getTimestamp());
    result = hbResponse.initialize(initializerObj, 0, /*requireWallTime*/ true);
    ASSERT_EQUALS(ErrorCodes::TypeMismatch, result);
    ASSERT_EQUALS("\"opTime\" had the wrong type. Expected object, found timestamp",
                  result.reason());
}

TEST(ReplSetHeartbeatResponse, InitializeNoAppliedWallTime) {
    ReplSetHeartbeatResponse hbResponse;
    BSONObj initializerObj = BSON(
        "ok" << 1.0 << "durableOpTime" << OpTime(Timestamp(100, 0), 0).toBSON() << "durableWallTime"
             << Date_t() + Seconds(100) << "opTime" << OpTime(Timestamp(100, 0), 0).toBSON());
    Status result = hbResponse.initialize(initializerObj, 0, /*requireWallTime*/ true);
    ASSERT_EQUALS(ErrorCodes::NoSuchKey, result);
    ASSERT_EQUALS("Missing expected field \"wallTime\"", result.reason());
}

TEST(ReplSetHeartbeatResponse, InitializeMemberStateWrongType) {
    ReplSetHeartbeatResponse hbResponse;
    BSONObj initializerObj = BSON(
        "ok" << 1.0 << "durableOpTime" << OpTime(Timestamp(100, 0), 0).toBSON() << "durableWallTime"
             << Date_t() + Seconds(100) << "opTime" << OpTime(Timestamp(100, 0), 0).toBSON()
             << "wallTime" << Date_t() + Seconds(100) << "state"
             << "hello");
    Status result = hbResponse.initialize(initializerObj, 0, /*requireWallTime*/ true);
    ASSERT_EQUALS(ErrorCodes::TypeMismatch, result);
    ASSERT_EQUALS(
        "Expected \"state\" field in response to replSetHeartbeat command to "
        "have type NumberInt or NumberLong, but found type string",
        result.reason());
}

TEST(ReplSetHeartbeatResponse, InitializeMemberStateTooLow) {
    ReplSetHeartbeatResponse hbResponse;
    BSONObj initializerObj = BSON(
        "ok" << 1.0 << "durableOpTime" << OpTime(Timestamp(100, 0), 0).toBSON() << "durableWallTime"
             << Date_t() + Seconds(100) << "opTime" << OpTime(Timestamp(100, 0), 0).toBSON()
             << "wallTime" << Date_t() + Seconds(100) << "state" << -1);
    Status result = hbResponse.initialize(initializerObj, 0, /*requireWallTime*/ true);
    ASSERT_EQUALS(ErrorCodes::BadValue, result);
    ASSERT_EQUALS(
        "Value for \"state\" in response to replSetHeartbeat is out of range; "
        "legal values are non-negative and no more than 10",
        result.reason());
}

TEST(ReplSetHeartbeatResponse, InitializeMemberStateTooHigh) {
    ReplSetHeartbeatResponse hbResponse;
    BSONObj initializerObj = BSON(
        "ok" << 1.0 << "durableOpTime" << OpTime(Timestamp(100, 0), 0).toBSON() << "durableWallTime"
             << Date_t() + Seconds(100) << "opTime" << OpTime(Timestamp(100, 0), 0).toBSON()
             << "wallTime" << Date_t() + Seconds(100) << "state" << 11);
    Status result = hbResponse.initialize(initializerObj, 0, /*requireWallTime*/ true);
    ASSERT_EQUALS(ErrorCodes::BadValue, result);
    ASSERT_EQUALS(
        "Value for \"state\" in response to replSetHeartbeat is out of range; "
        "legal values are non-negative and no more than 10",
        result.reason());
}

TEST(ReplSetHeartbeatResponse, InitializeVersionWrongType) {
    ReplSetHeartbeatResponse hbResponse;
    BSONObj initializerObj = BSON(
        "ok" << 1.0 << "durableOpTime" << OpTime(Timestamp(100, 0), 0).toBSON() << "durableWallTime"
             << Date_t() + Seconds(100) << "opTime" << OpTime(Timestamp(100, 0), 0).toBSON()
             << "wallTime" << Date_t() + Seconds(100) << "v"
             << "hello");
    Status result = hbResponse.initialize(initializerObj, 0, /*requireWallTime*/ true);
    ASSERT_EQUALS(ErrorCodes::TypeMismatch, result);
    ASSERT_EQUALS(
        "Expected \"v\" field in response to replSetHeartbeat to "
        "have type NumberInt, but found string",
        result.reason());
}

TEST(ReplSetHeartbeatResponse, InitializeReplSetNameWrongType) {
    ReplSetHeartbeatResponse hbResponse;
    BSONObj initializerObj =
        BSON("ok" << 1.0 << "durableOpTime" << OpTime(Timestamp(100, 0), 0).toBSON()
                  << "durableWallTime" << Date_t() + Seconds(100) << "opTime"
                  << OpTime(Timestamp(100, 0), 0).toBSON() << "wallTime" << Date_t() + Seconds(100)
                  << "v" << 2  // needs a version to get this far in initialize()
                  << "set" << 4);
    Status result = hbResponse.initialize(initializerObj, 0, /*requireWallTime*/ true);
    ASSERT_EQUALS(ErrorCodes::TypeMismatch, result);
    ASSERT_EQUALS(
        "Expected \"set\" field in response to replSetHeartbeat to "
        "have type String, but found int",
        result.reason());
}

TEST(ReplSetHeartbeatResponse, InitializeSyncingToWrongType) {
    ReplSetHeartbeatResponse hbResponse;
    BSONObj initializerObj =
        BSON("ok" << 1.0 << "durableOpTime" << OpTime(Timestamp(100, 0), 0).toBSON()
                  << "durableWallTime" << Date_t() + Seconds(100) << "opTime"
                  << OpTime(Timestamp(100, 0), 0).toBSON() << "wallTime" << Date_t() + Seconds(100)
                  << "v" << 2  // needs a version to get this far in initialize()
                  << "syncingTo" << 4);
    Status result = hbResponse.initialize(initializerObj, 0, /*requireWallTime*/ true);
    ASSERT_EQUALS(ErrorCodes::TypeMismatch, result);
    ASSERT_EQUALS(
        "Expected \"syncingTo\" field in response to replSetHeartbeat to "
        "have type String, but found int",
        result.reason());
}

TEST(ReplSetHeartbeatResponse, InitializeConfigWrongType) {
    ReplSetHeartbeatResponse hbResponse;
    BSONObj initializerObj =
        BSON("ok" << 1.0 << "durableOpTime" << OpTime(Timestamp(100, 0), 0).toBSON()
                  << "durableWallTime" << Date_t() + Seconds(100) << "opTime"
                  << OpTime(Timestamp(100, 0), 0).toBSON() << "wallTime" << Date_t() + Seconds(100)
                  << "v" << 2  // needs a version to get this far in initialize()
                  << "config" << 4);
    Status result = hbResponse.initialize(initializerObj, 0, /*requireWallTime*/ true);
    ASSERT_EQUALS(ErrorCodes::TypeMismatch, result);
    ASSERT_EQUALS(
        "Expected \"config\" in response to replSetHeartbeat to "
        "have type Object, but found int",
        result.reason());
}

TEST(ReplSetHeartbeatResponse, InitializeBadConfig) {
    ReplSetHeartbeatResponse hbResponse;
    BSONObj initializerObj =
        BSON("ok" << 1.0 << "durableOpTime" << OpTime(Timestamp(100, 0), 0).toBSON()
                  << "durableWallTime" << Date_t() + Seconds(100) << "opTime"
                  << OpTime(Timestamp(100, 0), 0).toBSON() << "wallTime" << Date_t() + Seconds(100)
                  << "v" << 2  // needs a version to get this far in initialize()
                  << "config" << BSON("illegalFieldName" << 2));
    Status result = hbResponse.initialize(initializerObj, 0, /*requireWallTime*/ true);
    ASSERT_EQUALS(ErrorCodes::BadValue, result);
    ASSERT_EQUALS("Unexpected field illegalFieldName in replica set configuration",
                  result.reason());
}

TEST(ReplSetHeartbeatResponse, NoConfigStillInitializing) {
    ReplSetHeartbeatResponse hbResp;
    // When a node's config state is either kConfigPreStart or kConfigStartingUp,
    // then it responds to the heartbeat request with an error code ErrorCodes::NotYetInitialized.
    BSONObj initializerObj =
        BSON("ok" << 0.0 << "code" << ErrorCodes::NotYetInitialized << "errmsg"
                  << "Received heartbeat while still initializing replication system.");
    Status result = hbResp.initialize(initializerObj, 0, /*requireWallTime*/ true);
    ASSERT_EQUALS(ErrorCodes::NotYetInitialized, result.code());
}

TEST(ReplSetHeartbeatResponse, InvalidResponseOpTimeMissesConfigVersion) {
    ReplSetHeartbeatResponse hbResp;
    Status result = hbResp.initialize(BSON("ok" << 1.0 << "durableOpTime"
                                                << OpTime(Timestamp(100, 0), 0).toBSON()
                                                << "durableWallTime" << Date_t() + Seconds(100)
                                                << "opTime" << OpTime(Timestamp(100, 0), 0).toBSON()
                                                << "wallTime" << Date_t() + Seconds(100)),
                                      0,
                                      /*requireWallTime*/ true);
    ASSERT_EQUALS(ErrorCodes::NoSuchKey, result.code());
    ASSERT_TRUE(stringContains(result.reason(), "\"v\""))
        << result.reason() << " doesn't contain 'v' field required error msg";
}

TEST(ReplSetHeartbeatResponse, MismatchedReplicaSetNames) {
    ReplSetHeartbeatResponse hbResponse;
    BSONObj initializerObj =
        BSON("ok" << 0.0 << "code" << ErrorCodes::InconsistentReplicaSetNames << "errmsg"
                  << "replica set name doesn't match.");
    Status result = hbResponse.initialize(initializerObj, 0, /*requireWallTime*/ true);
    ASSERT_EQUALS(ErrorCodes::InconsistentReplicaSetNames, result.code());
}

TEST(ReplSetHeartbeatResponse, AuthFailure) {
    ReplSetHeartbeatResponse hbResp;
    std::string errMsg = "Unauthorized";
    Status result = hbResp.initialize(
        BSON("ok" << 0.0 << "errmsg" << errMsg << "code" << ErrorCodes::Unauthorized),
        0,
        /*requireWallTime*/ true);
    ASSERT_EQUALS(ErrorCodes::Unauthorized, result.code());
    ASSERT_EQUALS(errMsg, result.reason());
}

TEST(ReplSetHeartbeatResponse, ServerError) {
    ReplSetHeartbeatResponse hbResp;
    std::string errMsg = "Random Error";
    Status result =
        hbResp.initialize(BSON("ok" << 0.0 << "errmsg" << errMsg), 0, /*requireWallTime*/ true);
    ASSERT_EQUALS(ErrorCodes::UnknownError, result.code());
    ASSERT_EQUALS(errMsg, result.reason());
}

}  // namespace
}  // namespace repl
}  // namespace mongo
