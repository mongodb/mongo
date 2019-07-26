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

#include "mongo/db/jsobj.h"
#include "mongo/db/repl/read_concern_args.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace repl {
namespace {

TEST(ReadAfterParse, OpTimeOnly) {
    ReadConcernArgs readConcern;
    ASSERT_OK(readConcern.initialize(BSON("find"
                                          << "test" << ReadConcernArgs::kReadConcernFieldName
                                          << BSON(ReadConcernArgs::kAfterOpTimeFieldName
                                                  << BSON(OpTime::kTimestampFieldName
                                                          << Timestamp(20, 30)
                                                          << OpTime::kTermFieldName << 2)))));

    ASSERT_TRUE(readConcern.getArgsOpTime());
    ASSERT_TRUE(!readConcern.getArgsAfterClusterTime());
    auto argsOpTime = readConcern.getArgsOpTime();
    ASSERT_EQ(Timestamp(20, 30), argsOpTime->getTimestamp());
    ASSERT_EQ(2, argsOpTime->getTerm());
    ASSERT(ReadConcernLevel::kLocalReadConcern == readConcern.getLevel());
}

TEST(ReadAfterParse, AfterClusterTimeOnly) {
    ReadConcernArgs readConcern;
    auto afterClusterTime = LogicalTime(Timestamp(20, 30));
    ASSERT_OK(readConcern.initialize(BSON("find"
                                          << "test" << ReadConcernArgs::kReadConcernFieldName
                                          << BSON(ReadConcernArgs::kAfterClusterTimeFieldName
                                                  << afterClusterTime.asTimestamp()))));
    auto argsAfterClusterTime = readConcern.getArgsAfterClusterTime();
    ASSERT_TRUE(argsAfterClusterTime);
    ASSERT_TRUE(!readConcern.getArgsOpTime());
    ASSERT_TRUE(afterClusterTime == *argsAfterClusterTime);
}

TEST(ReadAfterParse, AfterClusterTimeAndLevelLocal) {
    ReadConcernArgs readConcern;
    // Must have level=majority
    auto afterClusterTime = LogicalTime(Timestamp(20, 30));
    ASSERT_OK(
        readConcern.initialize(BSON("find"
                                    << "test" << ReadConcernArgs::kReadConcernFieldName
                                    << BSON(ReadConcernArgs::kAfterClusterTimeFieldName
                                            << afterClusterTime.asTimestamp()
                                            << ReadConcernArgs::kLevelFieldName << "local"))));
    auto argsAfterClusterTime = readConcern.getArgsAfterClusterTime();
    ASSERT_TRUE(argsAfterClusterTime);
    ASSERT_TRUE(!readConcern.getArgsOpTime());
    ASSERT_TRUE(afterClusterTime == *argsAfterClusterTime);
    ASSERT(ReadConcernLevel::kLocalReadConcern == readConcern.getLevel());
}

TEST(ReadAfterParse, AfterClusterTimeAndLevelMajority) {
    ReadConcernArgs readConcern;
    // Must have level=majority
    auto afterClusterTime = LogicalTime(Timestamp(20, 30));
    ASSERT_OK(
        readConcern.initialize(BSON("find"
                                    << "test" << ReadConcernArgs::kReadConcernFieldName
                                    << BSON(ReadConcernArgs::kAfterClusterTimeFieldName
                                            << afterClusterTime.asTimestamp()
                                            << ReadConcernArgs::kLevelFieldName << "majority"))));
    auto argsAfterClusterTime = readConcern.getArgsAfterClusterTime();
    ASSERT_TRUE(argsAfterClusterTime);
    ASSERT_TRUE(!readConcern.getArgsOpTime());
    ASSERT_TRUE(afterClusterTime == *argsAfterClusterTime);
    ASSERT(ReadConcernLevel::kMajorityReadConcern == readConcern.getLevel());
}

TEST(ReadAfterParse, AfterClusterTimeAndLevelSnapshot) {
    ReadConcernArgs readConcern;
    auto afterClusterTime = LogicalTime(Timestamp(20, 30));
    ASSERT_OK(
        readConcern.initialize(BSON("find"
                                    << "test" << ReadConcernArgs::kReadConcernFieldName
                                    << BSON(ReadConcernArgs::kAfterClusterTimeFieldName
                                            << afterClusterTime.asTimestamp()
                                            << ReadConcernArgs::kLevelFieldName << "snapshot"))));
    auto argsAfterClusterTime = readConcern.getArgsAfterClusterTime();
    ASSERT_TRUE(argsAfterClusterTime);
    ASSERT_TRUE(!readConcern.getArgsOpTime());
    ASSERT_TRUE(afterClusterTime == *argsAfterClusterTime);
    ASSERT(ReadConcernLevel::kSnapshotReadConcern == readConcern.getLevel());
}

TEST(ReadAfterParse, AtClusterTimeOnly) {
    ReadConcernArgs readConcern;
    auto atClusterTime = LogicalTime(Timestamp(20, 30));
    ASSERT_EQ(ErrorCodes::InvalidOptions,
              readConcern.initialize(BSON("find"
                                          << "test" << ReadConcernArgs::kReadConcernFieldName
                                          << BSON(ReadConcernArgs::kAtClusterTimeFieldName
                                                  << atClusterTime.asTimestamp()))));
}

TEST(ReadAfterParse, AtClusterTimeAndLevelSnapshot) {
    ReadConcernArgs readConcern;
    auto atClusterTime = LogicalTime(Timestamp(20, 30));
    ASSERT_OK(
        readConcern.initialize(BSON("find"
                                    << "test" << ReadConcernArgs::kReadConcernFieldName
                                    << BSON(ReadConcernArgs::kAtClusterTimeFieldName
                                            << atClusterTime.asTimestamp()
                                            << ReadConcernArgs::kLevelFieldName << "snapshot"))));
    auto argsAtClusterTime = readConcern.getArgsAtClusterTime();
    ASSERT_TRUE(argsAtClusterTime);
    ASSERT_FALSE(readConcern.getArgsOpTime());
    ASSERT_FALSE(readConcern.getArgsAfterClusterTime());
    ASSERT(ReadConcernLevel::kSnapshotReadConcern == readConcern.getLevel());
}

TEST(ReadAfterParse, AtClusterTimeAndLevelMajority) {
    ReadConcernArgs readConcern;
    auto atClusterTime = LogicalTime(Timestamp(20, 30));
    ASSERT_EQ(
        ErrorCodes::InvalidOptions,
        readConcern.initialize(BSON("find"
                                    << "test" << ReadConcernArgs::kReadConcernFieldName
                                    << BSON(ReadConcernArgs::kAtClusterTimeFieldName
                                            << atClusterTime.asTimestamp()
                                            << ReadConcernArgs::kLevelFieldName << "majority"))));
}

TEST(ReadAfterParse, AtClusterTimeAndLevelLocal) {
    ReadConcernArgs readConcern;
    auto atClusterTime = LogicalTime(Timestamp(20, 30));
    ASSERT_EQ(
        ErrorCodes::InvalidOptions,
        readConcern.initialize(BSON("find"
                                    << "test" << ReadConcernArgs::kReadConcernFieldName
                                    << BSON(ReadConcernArgs::kAtClusterTimeFieldName
                                            << atClusterTime.asTimestamp()
                                            << ReadConcernArgs::kLevelFieldName << "local"))));
}

TEST(ReadAfterParse, AtClusterTimeAndLevelAvailable) {
    ReadConcernArgs readConcern;
    auto atClusterTime = LogicalTime(Timestamp(20, 30));
    ASSERT_EQ(
        ErrorCodes::InvalidOptions,
        readConcern.initialize(BSON("find"
                                    << "test" << ReadConcernArgs::kReadConcernFieldName
                                    << BSON(ReadConcernArgs::kAtClusterTimeFieldName
                                            << atClusterTime.asTimestamp()
                                            << ReadConcernArgs::kLevelFieldName << "available"))));
}

TEST(ReadAfterParse, AtClusterTimeAndLevelLinearizable) {
    ReadConcernArgs readConcern;
    auto atClusterTime = LogicalTime(Timestamp(20, 30));
    ASSERT_EQ(ErrorCodes::InvalidOptions,
              readConcern.initialize(BSON("find"
                                          << "test" << ReadConcernArgs::kReadConcernFieldName
                                          << BSON(ReadConcernArgs::kAtClusterTimeFieldName
                                                  << atClusterTime.asTimestamp()
                                                  << ReadConcernArgs::kLevelFieldName
                                                  << "linearizable"))));
}

TEST(ReadAfterParse, LevelMajorityOnly) {
    ReadConcernArgs readConcern;
    ASSERT_OK(
        readConcern.initialize(BSON("find"
                                    << "test" << ReadConcernArgs::kReadConcernFieldName
                                    << BSON(ReadConcernArgs::kLevelFieldName << "majority"))));

    ASSERT_TRUE(!readConcern.getArgsOpTime());
    ASSERT_TRUE(!readConcern.getArgsAfterClusterTime());
    ASSERT_TRUE(ReadConcernLevel::kMajorityReadConcern == readConcern.getLevel());
}

TEST(ReadAfterParse, LevelSnapshotOnly) {
    ReadConcernArgs readConcern;
    ASSERT_OK(
        readConcern.initialize(BSON("find"
                                    << "test" << ReadConcernArgs::kReadConcernFieldName
                                    << BSON(ReadConcernArgs::kLevelFieldName << "snapshot"))));

    ASSERT_TRUE(!readConcern.getArgsOpTime());
    ASSERT_TRUE(!readConcern.getArgsAfterClusterTime());
    ASSERT_TRUE(!readConcern.getArgsAtClusterTime());
    ASSERT_TRUE(ReadConcernLevel::kSnapshotReadConcern == readConcern.getLevel());
}

TEST(ReadAfterParse, ReadCommittedFullSpecification) {
    ReadConcernArgs readConcern;
    auto afterClusterTime = LogicalTime(Timestamp(100, 200));
    ASSERT_NOT_OK(readConcern.initialize(BSON(
        "find"
        << "test" << ReadConcernArgs::kReadConcernFieldName
        << BSON(ReadConcernArgs::kAfterOpTimeFieldName
                << BSON(OpTime::kTimestampFieldName << Timestamp(20, 30) << OpTime::kTermFieldName
                                                    << 2)
                << ReadConcernArgs::kAfterClusterTimeFieldName << afterClusterTime.asTimestamp()
                << ReadConcernArgs::kLevelFieldName << "majority"))));
}

TEST(ReadAfterParse, Empty) {
    ReadConcernArgs readConcern;
    ASSERT_OK(readConcern.initialize(BSON("find"
                                          << "test")));

    ASSERT_TRUE(!readConcern.getArgsOpTime());
    ASSERT_TRUE(!readConcern.getArgsAfterClusterTime());
    ASSERT(ReadConcernLevel::kLocalReadConcern == readConcern.getLevel());
}

TEST(ReadAfterParse, BadRootType) {
    ReadConcernArgs readConcern;
    ASSERT_NOT_OK(
        readConcern.initialize(BSON("find"
                                    << "test" << ReadConcernArgs::kReadConcernFieldName << "x")));
}

TEST(ReadAfterParse, BadAtClusterTimeType) {
    ReadConcernArgs readConcern;
    ASSERT_EQ(ErrorCodes::TypeMismatch,
              readConcern.initialize(BSON("find"
                                          << "test" << ReadConcernArgs::kReadConcernFieldName
                                          << BSON(ReadConcernArgs::kAtClusterTimeFieldName
                                                  << 2 << ReadConcernArgs::kLevelFieldName
                                                  << "snapshot"))));
}

TEST(ReadAfterParse, BadAtClusterTimeValue) {
    ReadConcernArgs readConcern;
    ASSERT_EQ(
        ErrorCodes::InvalidOptions,
        readConcern.initialize(BSON("find"
                                    << "test" << ReadConcernArgs::kReadConcernFieldName
                                    << BSON(ReadConcernArgs::kAtClusterTimeFieldName
                                            << LogicalTime::kUninitialized.asTimestamp()
                                            << ReadConcernArgs::kLevelFieldName << "snapshot"))));
}

TEST(ReadAfterParse, BadOpTimeType) {
    ReadConcernArgs readConcern;
    ASSERT_NOT_OK(
        readConcern.initialize(BSON("find"
                                    << "test" << ReadConcernArgs::kReadConcernFieldName
                                    << BSON(ReadConcernArgs::kAfterOpTimeFieldName << 2))));
}

TEST(ReadAfterParse, OpTimeNotNeededForValidReadConcern) {
    ReadConcernArgs readConcern;
    ASSERT_OK(readConcern.initialize(BSON("find"
                                          << "test" << ReadConcernArgs::kReadConcernFieldName
                                          << BSONObj())));
}

TEST(ReadAfterParse, NoOpTimeTS) {
    ReadConcernArgs readConcern;
    ASSERT_NOT_OK(readConcern.initialize(BSON("find"
                                              << "test" << ReadConcernArgs::kReadConcernFieldName
                                              << BSON(ReadConcernArgs::kAfterOpTimeFieldName
                                                      << BSON(OpTime::kTimestampFieldName << 2)))));
}

TEST(ReadAfterParse, NoOpTimeTerm) {
    ReadConcernArgs readConcern;
    ASSERT_NOT_OK(readConcern.initialize(BSON("find"
                                              << "test" << ReadConcernArgs::kReadConcernFieldName
                                              << BSON(ReadConcernArgs::kAfterOpTimeFieldName
                                                      << BSON(OpTime::kTermFieldName << 2)))));
}

TEST(ReadAfterParse, BadOpTimeTSType) {
    ReadConcernArgs readConcern;
    ASSERT_NOT_OK(readConcern.initialize(BSON("find"
                                              << "test" << ReadConcernArgs::kReadConcernFieldName
                                              << BSON(ReadConcernArgs::kAfterOpTimeFieldName
                                                      << BSON(OpTime::kTimestampFieldName
                                                              << BSON("x" << 1)
                                                              << OpTime::kTermFieldName << 2)))));
}

TEST(ReadAfterParse, BadOpTimeTermType) {
    ReadConcernArgs readConcern;
    ASSERT_NOT_OK(readConcern.initialize(BSON("find"
                                              << "test" << ReadConcernArgs::kReadConcernFieldName
                                              << BSON(ReadConcernArgs::kAfterOpTimeFieldName
                                                      << BSON(OpTime::kTimestampFieldName
                                                              << Timestamp(1, 0)
                                                              << OpTime::kTermFieldName << "y")))));
}

TEST(ReadAfterParse, BadLevelType) {
    ReadConcernArgs readConcern;
    ASSERT_EQ(ErrorCodes::TypeMismatch,
              readConcern.initialize(BSON("find"
                                          << "test" << ReadConcernArgs::kReadConcernFieldName
                                          << BSON(ReadConcernArgs::kLevelFieldName << 7))));
}

TEST(ReadAfterParse, BadLevelValue) {
    ReadConcernArgs readConcern;
    ASSERT_EQ(ErrorCodes::FailedToParse,
              readConcern.initialize(BSON("find"
                                          << "test" << ReadConcernArgs::kReadConcernFieldName
                                          << BSON(ReadConcernArgs::kLevelFieldName
                                                  << "seven is not a real level"))));
}

TEST(ReadAfterParse, BadOption) {
    ReadConcernArgs readConcern;
    ASSERT_EQ(ErrorCodes::InvalidOptions,
              readConcern.initialize(BSON("find"
                                          << "test" << ReadConcernArgs::kReadConcernFieldName
                                          << BSON("asdf" << 1))));
}

TEST(ReadAfterParse, AtClusterTimeAndAfterClusterTime) {
    ReadConcernArgs readConcern;
    auto clusterTime = LogicalTime(Timestamp(20, 30));
    ASSERT_EQ(
        ErrorCodes::InvalidOptions,
        readConcern.initialize(BSON("find"
                                    << "test" << ReadConcernArgs::kReadConcernFieldName
                                    << BSON(ReadConcernArgs::kAtClusterTimeFieldName
                                            << clusterTime.asTimestamp()
                                            << ReadConcernArgs::kAfterClusterTimeFieldName
                                            << clusterTime.asTimestamp()
                                            << ReadConcernArgs::kLevelFieldName << "snapshot"))));
}

TEST(ReadAfterParse, AfterOpTimeAndLevelSnapshot) {
    ReadConcernArgs readConcern;
    ASSERT_EQ(
        ErrorCodes::InvalidOptions,
        readConcern.initialize(BSON("find"
                                    << "test" << ReadConcernArgs::kReadConcernFieldName
                                    << BSON(ReadConcernArgs::kAfterOpTimeFieldName
                                            << BSON(OpTime::kTimestampFieldName
                                                    << Timestamp(20, 30) << OpTime::kTermFieldName
                                                    << 2)
                                            << ReadConcernArgs::kLevelFieldName << "snapshot"))));
}

TEST(ReadAfterSerialize, Empty) {
    BSONObjBuilder builder;
    ReadConcernArgs readConcern;
    readConcern.appendInfo(&builder);

    BSONObj obj(builder.done());

    ASSERT_BSONOBJ_EQ(BSON(ReadConcernArgs::kReadConcernFieldName << BSONObj()), obj);
}

TEST(ReadAfterSerialize, AfterClusterTimeOnly) {
    BSONObjBuilder builder;
    auto afterClusterTime = LogicalTime(Timestamp(20, 30));
    ReadConcernArgs readConcern(afterClusterTime, boost::none);
    readConcern.appendInfo(&builder);

    BSONObj expectedObj(BSON(
        ReadConcernArgs::kReadConcernFieldName
        << BSON(ReadConcernArgs::kAfterClusterTimeFieldName << afterClusterTime.asTimestamp())));

    ASSERT_BSONOBJ_EQ(expectedObj, builder.done());
}

TEST(ReadAfterSerialize, AfterOpTimeOnly) {
    BSONObjBuilder builder;
    ReadConcernArgs readConcern(OpTime(Timestamp(20, 30), 2), boost::none);
    readConcern.appendInfo(&builder);

    BSONObj expectedObj(BSON(ReadConcernArgs::kReadConcernFieldName
                             << BSON(ReadConcernArgs::kAfterOpTimeFieldName << BSON(
                                         OpTime::kTimestampFieldName
                                         << Timestamp(20, 30) << OpTime::kTermFieldName << 2))));

    ASSERT_BSONOBJ_EQ(expectedObj, builder.done());
}

TEST(ReadAfterSerialize, CommitLevelOnly) {
    BSONObjBuilder builder;
    ReadConcernArgs readConcern(ReadConcernLevel::kLocalReadConcern);
    readConcern.appendInfo(&builder);

    BSONObj expectedObj(BSON(ReadConcernArgs::kReadConcernFieldName
                             << BSON(ReadConcernArgs::kLevelFieldName << "local")));

    ASSERT_BSONOBJ_EQ(expectedObj, builder.done());
}

TEST(ReadAfterSerialize, iAfterCLusterTimeAndLevel) {
    BSONObjBuilder builder;
    auto afterClusterTime = LogicalTime(Timestamp(20, 30));
    ReadConcernArgs readConcern(afterClusterTime, ReadConcernLevel::kMajorityReadConcern);
    readConcern.appendInfo(&builder);

    BSONObj expectedObj(BSON(ReadConcernArgs::kReadConcernFieldName
                             << BSON(ReadConcernArgs::kLevelFieldName
                                     << "majority" << ReadConcernArgs::kAfterClusterTimeFieldName
                                     << afterClusterTime.asTimestamp())));

    ASSERT_BSONOBJ_EQ(expectedObj, builder.done());
}

TEST(ReadAfterSerialize, AfterOpTimeAndLevel) {
    BSONObjBuilder builder;
    ReadConcernArgs readConcern(OpTime(Timestamp(20, 30), 2),
                                ReadConcernLevel::kMajorityReadConcern);
    readConcern.appendInfo(&builder);

    BSONObj expectedObj(BSON(ReadConcernArgs::kReadConcernFieldName << BSON(
                                 ReadConcernArgs::kLevelFieldName
                                 << "majority" << ReadConcernArgs::kAfterOpTimeFieldName
                                 << BSON(OpTime::kTimestampFieldName
                                         << Timestamp(20, 30) << OpTime::kTermFieldName << 2))));

    ASSERT_BSONOBJ_EQ(expectedObj, builder.done());
}

TEST(ReadAfterSerialize, AtClusterTimeAndLevelSnapshot) {
    BSONObjBuilder builder;
    ReadConcernArgs readConcern;
    auto atClusterTime = LogicalTime(Timestamp(20, 30));
    ASSERT_OK(readConcern.initialize(BSON("find"
                                          << "test" << ReadConcernArgs::kReadConcernFieldName
                                          << BSON(ReadConcernArgs::kLevelFieldName
                                                  << "snapshot"
                                                  << ReadConcernArgs::kAtClusterTimeFieldName
                                                  << atClusterTime.asTimestamp()))));

    readConcern.appendInfo(&builder);

    BSONObj expectedObj(BSON(ReadConcernArgs::kReadConcernFieldName
                             << BSON(ReadConcernArgs::kLevelFieldName
                                     << "snapshot" << ReadConcernArgs::kAtClusterTimeFieldName
                                     << atClusterTime.asTimestamp())));

    ASSERT_BSONOBJ_EQ(expectedObj, builder.done());
}

TEST(UpconvertReadConcernLevelToSnapshot, EmptyLevel) {
    ReadConcernArgs readConcern;
    ASSERT(ReadConcernLevel::kLocalReadConcern == readConcern.getLevel());

    ASSERT_OK(readConcern.upconvertReadConcernLevelToSnapshot());
    ASSERT(ReadConcernLevel::kSnapshotReadConcern == readConcern.getLevel());
    ASSERT(ReadConcernLevel::kLocalReadConcern == readConcern.getOriginalLevel());
}

TEST(UpconvertReadConcernLevelToSnapshot, LevelLocal) {
    ReadConcernArgs readConcern;
    ASSERT_OK(readConcern.initialize(BSON("find"
                                          << "test" << ReadConcernArgs::kReadConcernFieldName
                                          << BSON(ReadConcernArgs::kLevelFieldName << "local"))));
    ASSERT(ReadConcernLevel::kLocalReadConcern == readConcern.getLevel());

    ASSERT_OK(readConcern.upconvertReadConcernLevelToSnapshot());
    ASSERT(ReadConcernLevel::kSnapshotReadConcern == readConcern.getLevel());
    ASSERT(ReadConcernLevel::kLocalReadConcern == readConcern.getOriginalLevel());
}

TEST(UpconvertReadConcernLevelToSnapshot, LevelMajority) {
    ReadConcernArgs readConcern;
    ASSERT_OK(
        readConcern.initialize(BSON("find"
                                    << "test" << ReadConcernArgs::kReadConcernFieldName
                                    << BSON(ReadConcernArgs::kLevelFieldName << "majority"))));
    ASSERT(ReadConcernLevel::kMajorityReadConcern == readConcern.getLevel());

    ASSERT_OK(readConcern.upconvertReadConcernLevelToSnapshot());
    ASSERT(ReadConcernLevel::kSnapshotReadConcern == readConcern.getLevel());
    ASSERT(ReadConcernLevel::kMajorityReadConcern == readConcern.getOriginalLevel());
}

TEST(UpconvertReadConcernLevelToSnapshot, LevelSnapshot) {
    ReadConcernArgs readConcern;
    ASSERT_OK(
        readConcern.initialize(BSON("find"
                                    << "test" << ReadConcernArgs::kReadConcernFieldName
                                    << BSON(ReadConcernArgs::kLevelFieldName << "snapshot"))));
    ASSERT(ReadConcernLevel::kSnapshotReadConcern == readConcern.getLevel());

    ASSERT_OK(readConcern.upconvertReadConcernLevelToSnapshot());
    ASSERT(ReadConcernLevel::kSnapshotReadConcern == readConcern.getLevel());
    ASSERT(ReadConcernLevel::kSnapshotReadConcern == readConcern.getOriginalLevel());
}

TEST(UpconvertReadConcernLevelToSnapshot, LevelSnapshotWithAtClusterTime) {
    ReadConcernArgs readConcern;
    auto atClusterTime = LogicalTime(Timestamp(20, 30));
    ASSERT_OK(readConcern.initialize(BSON("find"
                                          << "test" << ReadConcernArgs::kReadConcernFieldName
                                          << BSON(ReadConcernArgs::kLevelFieldName
                                                  << "snapshot"
                                                  << ReadConcernArgs::kAtClusterTimeFieldName
                                                  << atClusterTime.asTimestamp()))));
    ASSERT(ReadConcernLevel::kSnapshotReadConcern == readConcern.getLevel());
    ASSERT_TRUE(readConcern.getArgsAtClusterTime());

    ASSERT_OK(readConcern.upconvertReadConcernLevelToSnapshot());
    ASSERT(ReadConcernLevel::kSnapshotReadConcern == readConcern.getLevel());
    ASSERT(ReadConcernLevel::kSnapshotReadConcern == readConcern.getOriginalLevel());
    ASSERT_TRUE(readConcern.getArgsAtClusterTime());
}

TEST(UpconvertReadConcernLevelToSnapshot, AfterClusterTime) {
    ReadConcernArgs readConcern;
    auto afterClusterTime = LogicalTime(Timestamp(20, 30));
    ASSERT_OK(readConcern.initialize(BSON("find"
                                          << "test" << ReadConcernArgs::kReadConcernFieldName
                                          << BSON(ReadConcernArgs::kAfterClusterTimeFieldName
                                                  << afterClusterTime.asTimestamp()))));
    ASSERT(ReadConcernLevel::kLocalReadConcern == readConcern.getLevel());
    ASSERT_TRUE(readConcern.getArgsAfterClusterTime());

    ASSERT_OK(readConcern.upconvertReadConcernLevelToSnapshot());
    ASSERT(ReadConcernLevel::kSnapshotReadConcern == readConcern.getLevel());
    ASSERT(ReadConcernLevel::kLocalReadConcern == readConcern.getOriginalLevel());
    ASSERT_TRUE(readConcern.getArgsAfterClusterTime());
}

TEST(UpconvertReadConcernLevelToSnapshot, LevelAvailable) {
    ReadConcernArgs readConcern;
    ASSERT_OK(
        readConcern.initialize(BSON("find"
                                    << "test" << ReadConcernArgs::kReadConcernFieldName
                                    << BSON(ReadConcernArgs::kLevelFieldName << "available"))));
    ASSERT(ReadConcernLevel::kAvailableReadConcern == readConcern.getLevel());

    ASSERT_NOT_OK(readConcern.upconvertReadConcernLevelToSnapshot());
    ASSERT(ReadConcernLevel::kAvailableReadConcern == readConcern.getLevel());
    ASSERT(ReadConcernLevel::kAvailableReadConcern == readConcern.getOriginalLevel());
}

TEST(UpconvertReadConcernLevelToSnapshot, LevelLinearizable) {
    ReadConcernArgs readConcern;
    ASSERT_OK(
        readConcern.initialize(BSON("find"
                                    << "test" << ReadConcernArgs::kReadConcernFieldName
                                    << BSON(ReadConcernArgs::kLevelFieldName << "linearizable"))));
    ASSERT(ReadConcernLevel::kLinearizableReadConcern == readConcern.getLevel());

    ASSERT_NOT_OK(readConcern.upconvertReadConcernLevelToSnapshot());
    ASSERT(ReadConcernLevel::kLinearizableReadConcern == readConcern.getLevel());
    ASSERT(ReadConcernLevel::kLinearizableReadConcern == readConcern.getOriginalLevel());
}

TEST(UpconvertReadConcernLevelToSnapshot, AfterOpTime) {
    ReadConcernArgs readConcern;
    ASSERT_OK(readConcern.initialize(BSON("find"
                                          << "test" << ReadConcernArgs::kReadConcernFieldName
                                          << BSON(ReadConcernArgs::kAfterOpTimeFieldName
                                                  << BSON(OpTime::kTimestampFieldName
                                                          << Timestamp(20, 30)
                                                          << OpTime::kTermFieldName << 2)))));
    ASSERT(ReadConcernLevel::kLocalReadConcern == readConcern.getLevel());
    ASSERT_TRUE(readConcern.getArgsOpTime());

    ASSERT_NOT_OK(readConcern.upconvertReadConcernLevelToSnapshot());
    ASSERT(ReadConcernLevel::kLocalReadConcern == readConcern.getLevel());
    ASSERT(ReadConcernLevel::kLocalReadConcern == readConcern.getOriginalLevel());
    ASSERT_TRUE(readConcern.getArgsOpTime());
}

}  // unnamed namespace
}  // namespace repl
}  // namespace mongo
