/**
 *    Copyright (C) 2015 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
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

TEST(ReadAfterParse, ReadAfterOnly) {
    ReadConcernArgs readAfterOpTime;
    ASSERT_OK(readAfterOpTime.initialize(BSON(
        "find"
        << "test"
        << ReadConcernArgs::kReadConcernFieldName
        << BSON(ReadConcernArgs::kAfterOpTimeFieldName
                << BSON(OpTime::kTimestampFieldName << Timestamp(20, 30) << OpTime::kTermFieldName
                                                    << 2)))));

    ASSERT_EQ(Timestamp(20, 30), readAfterOpTime.getOpTime().getTimestamp());
    ASSERT_EQ(2, readAfterOpTime.getOpTime().getTerm());
    ASSERT(ReadConcernLevel::kLocalReadConcern == readAfterOpTime.getLevel());
}

TEST(ReadAfterParse, ReadCommitLevelOnly) {
    ReadConcernArgs readAfterOpTime;
    ASSERT_OK(
        readAfterOpTime.initialize(BSON("find"
                                        << "test"
                                        << ReadConcernArgs::kReadConcernFieldName
                                        << BSON(ReadConcernArgs::kLevelFieldName << "majority"))));

    ASSERT_TRUE(readAfterOpTime.getOpTime().isNull());
    ASSERT_TRUE(ReadConcernLevel::kMajorityReadConcern == readAfterOpTime.getLevel());
}

TEST(ReadAfterParse, ReadCommittedFullSpecification) {
    ReadConcernArgs readAfterOpTime;
    ASSERT_OK(readAfterOpTime.initialize(BSON(
        "find"
        << "test"
        << ReadConcernArgs::kReadConcernFieldName
        << BSON(ReadConcernArgs::kAfterOpTimeFieldName
                << BSON(OpTime::kTimestampFieldName << Timestamp(20, 30) << OpTime::kTermFieldName
                                                    << 2)
                << ReadConcernArgs::kLevelFieldName
                << "majority"))));

    ASSERT_EQ(Timestamp(20, 30), readAfterOpTime.getOpTime().getTimestamp());
    ASSERT_EQ(2, readAfterOpTime.getOpTime().getTerm());
    ASSERT(ReadConcernLevel::kMajorityReadConcern == readAfterOpTime.getLevel());
}

TEST(ReadAfterParse, Empty) {
    ReadConcernArgs readAfterOpTime;
    ASSERT_OK(readAfterOpTime.initialize(BSON("find"
                                              << "test")));

    ASSERT(readAfterOpTime.getOpTime().getTimestamp().isNull());
    ASSERT(ReadConcernLevel::kLocalReadConcern == readAfterOpTime.getLevel());
}

TEST(ReadAfterParse, BadRootType) {
    ReadConcernArgs readAfterOpTime;
    ASSERT_NOT_OK(readAfterOpTime.initialize(BSON("find"
                                                  << "test"
                                                  << ReadConcernArgs::kReadConcernFieldName
                                                  << "x")));
}

TEST(ReadAfterParse, BadOpTimeType) {
    ReadConcernArgs readAfterOpTime;
    ASSERT_NOT_OK(
        readAfterOpTime.initialize(BSON("find"
                                        << "test"
                                        << ReadConcernArgs::kReadConcernFieldName
                                        << BSON(ReadConcernArgs::kAfterOpTimeFieldName << 2))));
}

TEST(ReadAfterParse, OpTimeNotNeededForValidReadConcern) {
    ReadConcernArgs readAfterOpTime;
    ASSERT_OK(readAfterOpTime.initialize(BSON("find"
                                              << "test"
                                              << ReadConcernArgs::kReadConcernFieldName
                                              << BSONObj())));
}

TEST(ReadAfterParse, NoOpTimeTS) {
    ReadConcernArgs readAfterOpTime;
    ASSERT_NOT_OK(
        readAfterOpTime.initialize(BSON("find"
                                        << "test"
                                        << ReadConcernArgs::kReadConcernFieldName
                                        << BSON(ReadConcernArgs::kAfterOpTimeFieldName
                                                << BSON(OpTime::kTimestampFieldName << 2)))));
}

TEST(ReadAfterParse, NoOpTimeTerm) {
    ReadConcernArgs readAfterOpTime;
    ASSERT_NOT_OK(readAfterOpTime.initialize(BSON("find"
                                                  << "test"
                                                  << ReadConcernArgs::kReadConcernFieldName
                                                  << BSON(ReadConcernArgs::kAfterOpTimeFieldName
                                                          << BSON(OpTime::kTermFieldName << 2)))));
}

TEST(ReadAfterParse, BadOpTimeTSType) {
    ReadConcernArgs readAfterOpTime;
    ASSERT_NOT_OK(readAfterOpTime.initialize(
        BSON("find"
             << "test"
             << ReadConcernArgs::kReadConcernFieldName
             << BSON(ReadConcernArgs::kAfterOpTimeFieldName
                     << BSON(OpTime::kTimestampFieldName << BSON("x" << 1) << OpTime::kTermFieldName
                                                         << 2)))));
}

TEST(ReadAfterParse, BadOpTimeTermType) {
    ReadConcernArgs readAfterOpTime;
    ASSERT_NOT_OK(readAfterOpTime.initialize(BSON(
        "find"
        << "test"
        << ReadConcernArgs::kReadConcernFieldName
        << BSON(ReadConcernArgs::kAfterOpTimeFieldName
                << BSON(OpTime::kTimestampFieldName << Timestamp(1, 0) << OpTime::kTermFieldName
                                                    << "y")))));
}

TEST(ReadAfterParse, BadLevelType) {
    ReadConcernArgs readAfterOpTime;
    ASSERT_EQ(ErrorCodes::TypeMismatch,
              readAfterOpTime.initialize(BSON("find"
                                              << "test"
                                              << ReadConcernArgs::kReadConcernFieldName
                                              << BSON(ReadConcernArgs::kLevelFieldName << 7))));
}

TEST(ReadAfterParse, BadLevelValue) {
    ReadConcernArgs readAfterOpTime;
    ASSERT_EQ(ErrorCodes::FailedToParse,
              readAfterOpTime.initialize(BSON("find"
                                              << "test"
                                              << ReadConcernArgs::kReadConcernFieldName
                                              << BSON(ReadConcernArgs::kLevelFieldName
                                                      << "seven is not a real level"))));
}

TEST(ReadAfterParse, BadOption) {
    ReadConcernArgs readAfterOpTime;
    ASSERT_EQ(ErrorCodes::InvalidOptions,
              readAfterOpTime.initialize(BSON("find"
                                              << "test"
                                              << ReadConcernArgs::kReadConcernFieldName
                                              << BSON("asdf" << 1))));
}

TEST(ReadAfterSerialize, Empty) {
    BSONObjBuilder builder;
    ReadConcernArgs readAfterOpTime;
    readAfterOpTime.appendInfo(&builder);

    BSONObj obj(builder.done());

    ASSERT_EQ(BSON(ReadConcernArgs::kReadConcernFieldName << BSONObj()), obj);
}

TEST(ReadAfterSerialize, ReadAfterOnly) {
    BSONObjBuilder builder;
    ReadConcernArgs readAfterOpTime(OpTime(Timestamp(20, 30), 2), boost::none);
    readAfterOpTime.appendInfo(&builder);

    BSONObj expectedObj(BSON(
        ReadConcernArgs::kReadConcernFieldName << BSON(
            ReadConcernArgs::kAfterOpTimeFieldName << BSON(
                OpTime::kTimestampFieldName << Timestamp(20, 30) << OpTime::kTermFieldName << 2))));

    ASSERT_EQ(expectedObj, builder.done());
}

TEST(ReadAfterSerialize, CommitLevelOnly) {
    BSONObjBuilder builder;
    ReadConcernArgs readAfterOpTime(boost::none, ReadConcernLevel::kLocalReadConcern);
    readAfterOpTime.appendInfo(&builder);

    BSONObj expectedObj(BSON(ReadConcernArgs::kReadConcernFieldName
                             << BSON(ReadConcernArgs::kLevelFieldName << "local")));

    ASSERT_EQ(expectedObj, builder.done());
}

TEST(ReadAfterSerialize, FullSpecification) {
    BSONObjBuilder builder;
    ReadConcernArgs readAfterOpTime(OpTime(Timestamp(20, 30), 2),
                                    ReadConcernLevel::kMajorityReadConcern);
    readAfterOpTime.appendInfo(&builder);

    BSONObj expectedObj(BSON(
        ReadConcernArgs::kReadConcernFieldName
        << BSON(ReadConcernArgs::kLevelFieldName
                << "majority"
                << ReadConcernArgs::kAfterOpTimeFieldName
                << BSON(OpTime::kTimestampFieldName << Timestamp(20, 30) << OpTime::kTermFieldName
                                                    << 2))));

    ASSERT_EQ(expectedObj, builder.done());
}

}  // unnamed namespace
}  // namespace repl
}  // namespace mongo
