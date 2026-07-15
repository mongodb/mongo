// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/repl/repl_set_heartbeat_response.h"

#include "mongo/base/error_codes.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/db/commands.h"
#include "mongo/db/database_name.h"
#include "mongo/db/repl/repl_set_config.h"
#include "mongo/db/repl/repl_settings.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/repl/replication_coordinator_mock.h"
#include "mongo/db/service_context_d_test_fixture.h"
#include "mongo/rpc/op_msg_rpc_impls.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/clock_source_mock.h"
#include "mongo/util/duration.h"
#include "mongo/util/future.h"

#include <memory>
#include <ostream>

namespace mongo {
namespace repl {
namespace {


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
    OpTime writtenOpTime = OpTime(Timestamp(50), 0);
    Date_t writtenWallTime = Date_t() + Seconds(writtenOpTime.getSecs());
    Timestamp lastStableRecoveryTimestamp = Timestamp(9);
    ASSERT_EQUALS(false, hbResponse.hasState());
    ASSERT_EQUALS(false, hbResponse.hasElectionTime());
    ASSERT_EQUALS(false, hbResponse.hasDurableOpTime());
    ASSERT_EQUALS(false, hbResponse.hasAppliedOpTime());
    ASSERT_EQUALS(false, hbResponse.hasWrittenOpTime());
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
    // set config term.
    hbResponse.setConfigTerm(1);
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
    // set writtenOpTime
    hbResponse.setWrittenOpTimeAndWallTime({writtenOpTime, writtenWallTime});
    fieldsSet += 2;  // OpTime and WallTime are separate fields
    hbResponse.setLastStableRecoveryTimestamp(lastStableRecoveryTimestamp);
    ++fieldsSet;
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
    ASSERT_EQUALS(true, hbResponse.hasWrittenOpTime());
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
    ASSERT_EQUALS(writtenOpTime, hbResponse.getWrittenOpTime());
    ASSERT_EQUALS(writtenWallTime, hbResponse.getWrittenOpTimeAndWallTime().wallTime);
    ASSERT_EQUALS(lastStableRecoveryTimestamp, hbResponse.getLastStableRecoveryTimestamp());
    ASSERT_EQUALS(config.toBSON().toString(), hbResponse.getConfig().toBSON().toString());

    hbResponseObj = hbResponse.toBSON();
    ASSERT_EQUALS(fieldsSet, hbResponseObj.nFields());
    ASSERT_EQUALS("rs0", hbResponseObj["set"].String());
    ASSERT_EQUALS(1, hbResponseObj["v"].Number());
    ASSERT_EQUALS(1, hbResponseObj["configTerm"].Number());
    ASSERT_EQUALS(Timestamp(10, 0), hbResponseObj["electionTime"].timestamp());
    ASSERT_EQUALS(Timestamp(0, 50), hbResponseObj["opTime"]["ts"].timestamp());
    ASSERT_EQUALS(Timestamp(0, 10), hbResponseObj["durableOpTime"]["ts"].timestamp());
    ASSERT_EQUALS(config.toBSON().toString(), hbResponseObj["config"].Obj().toString());
    ASSERT_EQUALS(2, hbResponseObj["state"].numberLong());
    ASSERT_EQUALS("syncTarget:27017", hbResponseObj["syncingTo"].String());

    // Verify that we allow an unknown field.
    BSONObjBuilder hbResponseBob;
    hbResponseBob.appendElements(hbResponseObj);
    hbResponseBob.append("unknownField", 1);
    auto cmdObj = hbResponseBob.obj();

    initializeResult = hbResponseObjRoundTripChecker.initialize(cmdObj, 0);
    ASSERT_EQUALS(Status::OK(), initializeResult);
    ASSERT_EQUALS(hbResponseObj.toString(), hbResponseObjRoundTripChecker.toBSON().toString());
}

TEST(ReplSetHeartbeatResponse, InitializeWrongElectionTimeType) {
    ReplSetHeartbeatResponse hbResponse;
    BSONObj initializerObj = BSON("ok" << 1.0 << "electionTime"
                                       << "hello");
    Status result = hbResponse.initialize(initializerObj, 0);
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
    Status result = hbResponse.initialize(initializerObj, 0);
    ASSERT_EQUALS(ErrorCodes::TypeMismatch, result);
    ASSERT_EQUALS("\"durableOpTime\" had the wrong type. Expected object, found string",
                  result.reason());

    BSONObj initializerObj2 = BSON("ok" << 1.0 << "durableOpTime" << OpTime().getTimestamp());
    Status result2 = hbResponse.initialize(initializerObj2, 0);
    ASSERT_EQUALS(ErrorCodes::TypeMismatch, result2);
    ASSERT_EQUALS("\"durableOpTime\" had the wrong type. Expected object, found timestamp",
                  result2.reason());
}

TEST(ReplSetHeartbeatResponse, InitializeNoDurableWallTime) {
    ReplSetHeartbeatResponse hbResponse;
    BSONObj initializerObj =
        BSON("ok" << 1.0 << "durableOpTime" << OpTime(Timestamp(100, 0), 0).toBSON() << "opTime"
                  << OpTime(Timestamp(100, 0), 0).toBSON());
    Status result = hbResponse.initialize(initializerObj, 0);
    ASSERT_EQUALS(ErrorCodes::NoSuchKey, result);
    ASSERT_EQUALS("Missing expected field \"durableWallTime\"", result.reason());
}

TEST(ReplSetHeartbeatResponse, InitializeWrongAppliedOpTimeType) {
    ReplSetHeartbeatResponse hbResponse;
    BSONObj initializerObj =
        BSON("ok" << 1.0 << "durableOpTime" << OpTime(Timestamp(100, 0), 0).toBSON()
                  << "durableWallTime" << Date_t() + Seconds(100) << "opTime"
                  << "hello");
    Status result = hbResponse.initialize(initializerObj, 0);
    ASSERT_EQUALS(ErrorCodes::TypeMismatch, result);
    ASSERT_EQUALS("\"opTime\" had the wrong type. Expected object, found string", result.reason());

    initializerObj = BSON("ok" << 1.0 << "durableOpTime" << OpTime(Timestamp(100, 0), 0).toBSON()
                               << "durableWallTime" << Date_t() + Seconds(100) << "opTime"
                               << OpTime().getTimestamp());
    result = hbResponse.initialize(initializerObj, 0);
    ASSERT_EQUALS(ErrorCodes::TypeMismatch, result);
    ASSERT_EQUALS("\"opTime\" had the wrong type. Expected object, found timestamp",
                  result.reason());
}

TEST(ReplSetHeartbeatResponse, InitializeNoAppliedWallTime) {
    ReplSetHeartbeatResponse hbResponse;
    BSONObj initializerObj = BSON(
        "ok" << 1.0 << "durableOpTime" << OpTime(Timestamp(100, 0), 0).toBSON() << "durableWallTime"
             << Date_t() + Seconds(100) << "opTime" << OpTime(Timestamp(100, 0), 0).toBSON());
    Status result = hbResponse.initialize(initializerObj, 0);
    ASSERT_EQUALS(ErrorCodes::NoSuchKey, result);
    ASSERT_EQUALS("Missing expected field \"wallTime\"", result.reason());
}

TEST(ReplSetHeartbeatResponse, InitializeWrongWrittenOpTimeType) {
    ReplSetHeartbeatResponse hbResponse;
    BSONObj initializerObj = BSON(
        "ok" << 1.0 << "durableOpTime" << OpTime(Timestamp(100, 0), 0).toBSON() << "durableWallTime"
             << Date_t() + Seconds(100) << "opTime" << OpTime(Timestamp(100, 0), 0).toBSON()
             << "wallTime" << Date_t() + Seconds(100) << "writtenOpTime"
             << "hello");
    Status result = hbResponse.initialize(initializerObj, 0);
    ASSERT_EQUALS(ErrorCodes::TypeMismatch, result);
    ASSERT_EQUALS("\"writtenOpTime\" had the wrong type. Expected object, found string",
                  result.reason());

    initializerObj =
        BSON("ok" << 1.0 << "durableOpTime" << OpTime(Timestamp(100, 0), 0).toBSON()
                  << "durableWallTime" << Date_t() + Seconds(100) << "opTime"
                  << OpTime(Timestamp(100, 0), 0).toBSON() << "wallTime" << Date_t() + Seconds(100)
                  << "writtenOpTime" << OpTime().getTimestamp());
    result = hbResponse.initialize(initializerObj, 0);
    ASSERT_EQUALS(ErrorCodes::TypeMismatch, result);
    ASSERT_EQUALS("\"writtenOpTime\" had the wrong type. Expected object, found timestamp",
                  result.reason());
}

TEST(ReplSetHeartbeatResponse, InitializeNoWrittenOpTime) {
    ReplSetHeartbeatResponse hbResponse;
    BSONObj initializerObj = BSON(
        "ok" << 1.0 << "durableOpTime" << OpTime(Timestamp(100, 0), 0).toBSON() << "durableWallTime"
             << Date_t() + Seconds(100) << "opTime" << OpTime(Timestamp(100, 0), 0).toBSON()
             << "wallTime" << Date_t() + Seconds(100) << "v" << 2);
    Status result = hbResponse.initialize(initializerObj, 0);
    ASSERT_EQUALS(ErrorCodes::OK, result);
    ASSERT_EQUALS("", result.reason());
}

TEST(ReplSetHeartbeatResponse, InitializeNoWrittenWallTime) {
    ReplSetHeartbeatResponse hbResponse;
    BSONObj initializerObj =
        BSON("ok" << 1.0 << "durableOpTime" << OpTime(Timestamp(100, 0), 0).toBSON()
                  << "durableWallTime" << Date_t() + Seconds(100) << "opTime"
                  << OpTime(Timestamp(100, 0), 0).toBSON() << "wallTime" << Date_t() + Seconds(100)
                  << "writtenOpTime" << OpTime(Timestamp(100, 0), 0).toBSON() << "v" << 2);
    Status result = hbResponse.initialize(initializerObj, 0);
    ASSERT_EQUALS(ErrorCodes::OK, result);
    ASSERT_EQUALS("", result.reason());
}

TEST(ReplSetHeartbeatResponse, InitializeWrongLastStableRecoveryTimestampType) {
    ReplSetHeartbeatResponse hbResponse;
    BSONObj initializerObj =
        BSON("ok" << 1.0 << "durableOpTime" << OpTime(Timestamp(100, 0), 0).toBSON()
                  << "durableWallTime" << Date_t() + Seconds(100) << "opTime"
                  << OpTime(Timestamp(100, 0), 0).toBSON() << "wallTime" << Date_t() + Seconds(100)
                  << "v" << 2 << "lastStableRecoveryTimestamp" << 12345LL);
    Status result = hbResponse.initialize(initializerObj, 0);
    ASSERT_EQUALS(ErrorCodes::TypeMismatch, result);
    ASSERT_STRING_CONTAINS(result.reason(), "Expected \"lastStableRecoveryTimestamp\" field");
}

TEST(ReplSetHeartbeatResponse, InitializeNoLastStableRecoveryTimestamp) {
    // Verifies backward compatibility: responses which don't report lastStableRecoveryTimestamp
    // (older versions) still initialize successfully and hasLastStableRecoveryTimestamp() returns
    // false.
    ReplSetHeartbeatResponse hbResponse;
    BSONObj initializerObj = BSON(
        "ok" << 1.0 << "durableOpTime" << OpTime(Timestamp(100, 0), 0).toBSON() << "durableWallTime"
             << Date_t() + Seconds(100) << "opTime" << OpTime(Timestamp(100, 0), 0).toBSON()
             << "wallTime" << Date_t() + Seconds(100) << "v" << 2);
    ASSERT_OK(hbResponse.initialize(initializerObj, 0));
    ASSERT_FALSE(hbResponse.hasLastStableRecoveryTimestamp());
}

TEST(ReplSetHeartbeatResponse, InitializeMemberStateWrongType) {
    ReplSetHeartbeatResponse hbResponse;
    BSONObj initializerObj =
        BSON("ok" << 1.0 << "durableOpTime" << OpTime(Timestamp(100, 0), 0).toBSON()
                  << "durableWallTime" << Date_t() + Seconds(100) << "opTime"
                  << OpTime(Timestamp(100, 0), 0).toBSON() << "wallTime" << Date_t() + Seconds(100)
                  << "writtenOpTime" << OpTime(Timestamp(100, 0), 0).toBSON() << "writtenWallTime"
                  << Date_t() + Seconds(100) << "state"
                  << "hello");
    Status result = hbResponse.initialize(initializerObj, 0);
    ASSERT_EQUALS(ErrorCodes::TypeMismatch, result);
    ASSERT_EQUALS(
        "Expected \"state\" field in response to replSetHeartbeat command to "
        "have type NumberInt or NumberLong, but found type string",
        result.reason());
}

TEST(ReplSetHeartbeatResponse, InitializeMemberStateTooLow) {
    ReplSetHeartbeatResponse hbResponse;
    BSONObj initializerObj =
        BSON("ok" << 1.0 << "durableOpTime" << OpTime(Timestamp(100, 0), 0).toBSON()
                  << "durableWallTime" << Date_t() + Seconds(100) << "opTime"
                  << OpTime(Timestamp(100, 0), 0).toBSON() << "wallTime" << Date_t() + Seconds(100)
                  << "writtenOpTime" << OpTime(Timestamp(100, 0), 0).toBSON() << "writtenWallTime"
                  << Date_t() + Seconds(100) << "state" << -1);
    Status result = hbResponse.initialize(initializerObj, 0);
    ASSERT_EQUALS(ErrorCodes::BadValue, result);
    ASSERT_EQUALS(
        "Value for \"state\" in response to replSetHeartbeat is out of range; "
        "legal values are non-negative and no more than 10",
        result.reason());
}

TEST(ReplSetHeartbeatResponse, InitializeMemberStateTooHigh) {
    ReplSetHeartbeatResponse hbResponse;
    BSONObj initializerObj =
        BSON("ok" << 1.0 << "durableOpTime" << OpTime(Timestamp(100, 0), 0).toBSON()
                  << "durableWallTime" << Date_t() + Seconds(100) << "opTime"
                  << OpTime(Timestamp(100, 0), 0).toBSON() << "wallTime" << Date_t() + Seconds(100)
                  << "writtenOpTime" << OpTime(Timestamp(100, 0), 0).toBSON() << "writtenWallTime"
                  << Date_t() + Seconds(100) << "state" << 11);
    Status result = hbResponse.initialize(initializerObj, 0);
    ASSERT_EQUALS(ErrorCodes::BadValue, result);
    ASSERT_EQUALS(
        "Value for \"state\" in response to replSetHeartbeat is out of range; "
        "legal values are non-negative and no more than 10",
        result.reason());
}

TEST(ReplSetHeartbeatResponse, InitializeVersionWrongType) {
    ReplSetHeartbeatResponse hbResponse;
    BSONObj initializerObj =
        BSON("ok" << 1.0 << "durableOpTime" << OpTime(Timestamp(100, 0), 0).toBSON()
                  << "durableWallTime" << Date_t() + Seconds(100) << "opTime"
                  << OpTime(Timestamp(100, 0), 0).toBSON() << "wallTime" << Date_t() + Seconds(100)
                  << "writtenOpTime" << OpTime(Timestamp(100, 0), 0).toBSON() << "writtenWallTime"
                  << Date_t() + Seconds(100) << "v"
                  << "hello");
    Status result = hbResponse.initialize(initializerObj, 0);
    ASSERT_EQUALS(ErrorCodes::TypeMismatch, result);
    ASSERT_EQUALS(
        "Expected \"v\" field in response to replSetHeartbeat to "
        "have type NumberInt/NumberLong, but found string",
        result.reason());
}

TEST(ReplSetHeartbeatResponse, InitializeReplSetNameWrongType) {
    ReplSetHeartbeatResponse hbResponse;
    BSONObj initializerObj =
        BSON("ok" << 1.0 << "durableOpTime" << OpTime(Timestamp(100, 0), 0).toBSON()
                  << "durableWallTime" << Date_t() + Seconds(100) << "opTime"
                  << OpTime(Timestamp(100, 0), 0).toBSON() << "wallTime" << Date_t() + Seconds(100)
                  << "writtenOpTime" << OpTime(Timestamp(100, 0), 0).toBSON() << "writtenWallTime"
                  << Date_t() + Seconds(100) << "v"
                  << 2  // needs a version to get this far in initialize()
                  << "set" << 4);
    Status result = hbResponse.initialize(initializerObj, 0);
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
                  << "writtenOpTime" << OpTime(Timestamp(100, 0), 0).toBSON() << "writtenWallTime"
                  << Date_t() + Seconds(100) << "v"
                  << 2  // needs a version to get this far in initialize()
                  << "syncingTo" << 4);
    Status result = hbResponse.initialize(initializerObj, 0);
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
                  << "writtenOpTime" << OpTime(Timestamp(100, 0), 0).toBSON() << "writtenWallTime"
                  << Date_t() + Seconds(100) << "v"
                  << 2  // needs a version to get this far in initialize()
                  << "config" << 4);
    Status result = hbResponse.initialize(initializerObj, 0);
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
                  << "writtenOpTime" << OpTime(Timestamp(100, 0), 0).toBSON() << "writtenWallTime"
                  << Date_t() + Seconds(100) << "v"
                  << 2  // needs a version to get this far in initialize()
                  << "config" << BSON("illegalFieldName" << 2));
    Status result = hbResponse.initialize(initializerObj, 0);
    ASSERT_NOT_OK(result);
    ASSERT_STRING_CONTAINS(result.reason(), "'ReplSetConfig.illegalFieldName' is an unknown field");
}

TEST(ReplSetHeartbeatResponse, NoConfigStillInitializing) {
    ReplSetHeartbeatResponse hbResp;
    // When a node's config state is either kConfigPreStart or kConfigStartingUp,
    // then it responds to the heartbeat request with an error code ErrorCodes::NotYetInitialized.
    BSONObj initializerObj =
        BSON("ok" << 0.0 << "code" << ErrorCodes::NotYetInitialized << "errmsg"
                  << "Received heartbeat while still initializing replication system.");
    Status result = hbResp.initialize(initializerObj, 0);
    ASSERT_EQUALS(ErrorCodes::NotYetInitialized, result.code());
}

TEST(ReplSetHeartbeatResponse, InvalidResponseOpTimeMissesConfigVersion) {
    ReplSetHeartbeatResponse hbResp;
    Status result = hbResp.initialize(
        BSON("ok" << 1.0 << "durableOpTime" << OpTime(Timestamp(100, 0), 0).toBSON()
                  << "durableWallTime" << Date_t() + Seconds(100) << "opTime"
                  << OpTime(Timestamp(100, 0), 0).toBSON() << "wallTime" << Date_t() + Seconds(100)
                  << "writtenOpTime" << OpTime(Timestamp(100, 0), 0).toBSON() << "writtenWallTime"
                  << Date_t() + Seconds(100)),
        0);
    ASSERT_EQUALS(ErrorCodes::NoSuchKey, result.code());
    ASSERT_TRUE(stringContains(result.reason(), "\"v\""))
        << result.reason() << " doesn't contain 'v' field required error msg";
}

TEST(ReplSetHeartbeatResponse, MismatchedReplicaSetNames) {
    ReplSetHeartbeatResponse hbResponse;
    BSONObj initializerObj =
        BSON("ok" << 0.0 << "code" << ErrorCodes::InconsistentReplicaSetNames << "errmsg"
                  << "replica set name doesn't match.");
    Status result = hbResponse.initialize(initializerObj, 0);
    ASSERT_EQUALS(ErrorCodes::InconsistentReplicaSetNames, result.code());
}

TEST(ReplSetHeartbeatResponse, AuthFailure) {
    ReplSetHeartbeatResponse hbResp;
    std::string errMsg = "Unauthorized";
    Status result = hbResp.initialize(
        BSON("ok" << 0.0 << "errmsg" << errMsg << "code" << ErrorCodes::Unauthorized), 0);
    ASSERT_EQUALS(ErrorCodes::Unauthorized, result.code());
    ASSERT_EQUALS(errMsg, result.reason());
}

TEST(ReplSetHeartbeatResponse, ServerError) {
    ReplSetHeartbeatResponse hbResp;
    std::string errMsg = "Random Error";
    Status result = hbResp.initialize(BSON("ok" << 0.0 << "errmsg" << errMsg), 0);
    ASSERT_EQUALS(ErrorCodes::UnknownError, result.code());
    ASSERT_EQUALS(errMsg, result.reason());
}

// ============================================================
// CmdReplSetHeartbeat exhaust streaming tests
// ============================================================

// Lets the test choose whether getNextHeartbeatNotificationFuture() resolves immediately
// (simulating a lastApplied or checkpoint advance) or stays pending (simulating no progress), so we
// can drive both exhaust paths.
class ExhaustReplCoordMock : public ReplicationCoordinatorMock {
public:
    using ReplicationCoordinatorMock::ReplicationCoordinatorMock;

    SharedSemiFuture<void> getNextHeartbeatNotificationFuture() override {
        return _future;
    }

    void setFuture(SharedSemiFuture<void> f) {
        _future = std::move(f);
    }

private:
    SharedSemiFuture<void> _future = SemiFuture<void>::makeReady().share();
};

class ExhaustHeartbeatTest : public ServiceContextMongoDTest {
public:
    ExhaustHeartbeatTest() : ServiceContextMongoDTest(Options{}.useMockClock(true)) {}

    void setUp() override {
        ServiceContextMongoDTest::setUp();

        ReplSettings replSettings;
        replSettings.setReplSetString("rs0/host1:1");

        auto mock = std::make_unique<ExhaustReplCoordMock>(getServiceContext(), replSettings);
        _replCoord = mock.get();
        ReplicationCoordinator::set(getServiceContext(), std::move(mock));

        // Use a 2-second heartbeat interval so we can advance the mock clock past it cleanly.
        _replCoord->setGetConfigReturnValue(ReplSetConfig::parse(
            BSON("_id" << "rs0" << "version" << 1 << "members"
                       << BSON_ARRAY(BSON("_id" << 0 << "host"
                                                << "host1:1"))
                       << "settings" << BSON("heartbeatIntervalMillis" << 2000))));
    }

protected:
    ClockSourceMock* mockClock() {
        return static_cast<ClockSourceMock*>(getServiceContext()->getFastClockSource());
    }

    BasicCommand* heartbeatCmd() {
        auto* cmd = CommandHelpers::findCommand(getService(), "replSetHeartbeat");
        ASSERT(cmd) << "replSetHeartbeat command not found; ensure repl_set_commands is linked";
        return checked_cast<BasicCommand*>(cmd);
    }

    // Minimal valid cmdObj: "replSetHeartbeat" doubles as the set-name field.
    BSONObj heartbeatCmdObj() const {
        return BSON("replSetHeartbeat" << "rs0"
                                       << "configVersion" << 1LL << "from"
                                       << "host1:1"
                                       << "term" << 1LL);
    }

    ExhaustReplCoordMock* _replCoord = nullptr;
};

// When getNextHeartbeatNotificationFuture() resolves before the interval deadline, the node keeps
// the exhaust stream open so the primary gets a prompt response for each reported-value advance.
TEST_F(ExhaustHeartbeatTest, FutureResolved_StreamStaysOpen) {
    _replCoord->setFuture(SemiFuture<void>::makeReady().share());

    auto opCtx = cc().makeOperationContext();
    opCtx->setExhaust(true);

    rpc::OpMsgReplyBuilder replyBuilder;
    heartbeatCmd()->runWithReplyBuilder(
        opCtx.get(), DatabaseName::kAdmin, heartbeatCmdObj(), &replyBuilder);

    ASSERT_TRUE(replyBuilder.shouldRunAgainForExhaust())
        << "expected exhaust stream to remain open when optime advanced before interval expired";
}

// When the heartbeat interval deadline expires before getNextHeartbeatNotificationFuture()
// resolves, the node closes the exhaust stream so the primary is forced to issue a fresh request
// carrying updated $replData gossip.
TEST_F(ExhaustHeartbeatTest, IntervalExpired_StreamCloses) {
    // First call with a ready future: initialises the per-Client streamDeadline to now() + 2s and
    // verifies the normal path works.
    _replCoord->setFuture(SemiFuture<void>::makeReady().share());
    {
        auto opCtx = cc().makeOperationContext();
        opCtx->setExhaust(true);
        rpc::OpMsgReplyBuilder replyBuilder;
        heartbeatCmd()->runWithReplyBuilder(
            opCtx.get(), DatabaseName::kAdmin, heartbeatCmdObj(), &replyBuilder);
        ASSERT_TRUE(replyBuilder.shouldRunAgainForExhaust());
    }

    // Advance the mock clock past the 2-second deadline cached on the Client decoration.
    // ClockSourceMock's waitForConditionUntil() returns timeout immediately when deadline <= now(),
    // so runWithDeadline will time out on the very first interrupt check without blocking.
    mockClock()->advance(Milliseconds(2001));

    // Switch to a pending future. With the clock past the cached deadline, runWithDeadline will
    // observe MaxTimeMSExpired immediately and return without calling setNextInvocation.
    auto [promise, pendingFuture] = makePromiseFuture<void>();
    _replCoord->setFuture(std::move(pendingFuture).share());

    auto opCtx2 = cc().makeOperationContext();
    opCtx2->setExhaust(true);
    rpc::OpMsgReplyBuilder replyBuilder2;
    heartbeatCmd()->runWithReplyBuilder(
        opCtx2.get(), DatabaseName::kAdmin, heartbeatCmdObj(), &replyBuilder2);

    ASSERT_FALSE(replyBuilder2.shouldRunAgainForExhaust())
        << "expected exhaust stream to close when interval deadline expired";
}

}  // namespace
}  // namespace repl
}  // namespace mongo
