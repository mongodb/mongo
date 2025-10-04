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

#include "mongo/rpc/metadata/oplog_query_metadata.h"

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/timestamp.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/duration.h"
#include "mongo/util/time_support.h"

#include <utility>
#include <vector>

namespace mongo {
namespace rpc {
namespace {

using repl::OpTime;

TEST(ReplResponseMetadataTest, OplogQueryMetadataRoundtrip) {
    OpTime opTime1(Timestamp(1234, 100), 5);
    Date_t committedWall = Date_t() + Seconds(opTime1.getSecs());
    OpTime opTime2(Timestamp(7777, 101), 6);
    OplogQueryMetadata metadata({opTime1, committedWall}, opTime2, opTime2, 6, 12, -1, "");

    ASSERT_EQ(opTime1, metadata.getLastOpCommitted().opTime);
    ASSERT_EQ(committedWall, metadata.getLastOpCommitted().wallTime);
    ASSERT_EQ(opTime2, metadata.getLastOpApplied());
    ASSERT_EQ(opTime2, metadata.getLastOpWritten());
    ASSERT_TRUE(metadata.hasPrimaryIndex());

    BSONObjBuilder builder;
    metadata.writeToMetadata(&builder).transitional_ignore();

    BSONObj expectedObj(
        BSON(kOplogQueryMetadataFieldName
             << BSON("lastOpCommitted"
                     << BSON("ts" << opTime1.getTimestamp() << "t" << opTime1.getTerm())
                     << "lastCommittedWall" << committedWall << "lastOpApplied"
                     << BSON("ts" << opTime2.getTimestamp() << "t" << opTime2.getTerm())
                     << "lastOpWritten"
                     << BSON("ts" << opTime2.getTimestamp() << "t" << opTime2.getTerm()) << "rbid"
                     << 6 << "primaryIndex" << 12 << "syncSourceIndex" << -1 << "syncSourceHost"
                     << "")));

    BSONObj serializedObj = builder.obj();
    ASSERT_BSONOBJ_EQ(expectedObj, serializedObj);

    // Verify that we allow unknown fields.
    BSONObjBuilder bob;
    bob.appendElements(serializedObj);
    bob.append("unknownField", 1);

    auto cloneStatus = OplogQueryMetadata::readFromMetadata(bob.obj());
    ASSERT_OK(cloneStatus.getStatus());

    const auto& clonedMetadata = cloneStatus.getValue();
    ASSERT_EQ(opTime1, clonedMetadata.getLastOpCommitted().opTime);
    ASSERT_EQ(opTime2, clonedMetadata.getLastOpApplied());
    ASSERT_EQ(opTime2, clonedMetadata.getLastOpWritten());
    ASSERT_EQ(committedWall, clonedMetadata.getLastOpCommitted().wallTime);
    ASSERT_EQ(metadata.getRBID(), clonedMetadata.getRBID());
    ASSERT_EQ(metadata.getSyncSourceIndex(), clonedMetadata.getSyncSourceIndex());
    ASSERT_TRUE(clonedMetadata.hasPrimaryIndex());

    BSONObjBuilder clonedBuilder;
    clonedMetadata.writeToMetadata(&clonedBuilder).transitional_ignore();

    BSONObj clonedSerializedObj = clonedBuilder.obj();
    ASSERT_BSONOBJ_EQ(expectedObj, clonedSerializedObj);
}

TEST(ReplResponseMetadataTest, OplogQueryMetadataRoundtripNoLastOpWritten) {
    OpTime opTime1(Timestamp(1234, 100), 5);
    Date_t committedWall = Date_t() + Seconds(opTime1.getSecs());
    OpTime opTime2(Timestamp(7777, 101), 6);

    BSONObj expectedObj(
        BSON(kOplogQueryMetadataFieldName
             << BSON("lastOpCommitted"
                     << BSON("ts" << opTime1.getTimestamp() << "t" << opTime1.getTerm())
                     << "lastCommittedWall" << committedWall << "lastOpApplied"
                     << BSON("ts" << opTime2.getTimestamp() << "t" << opTime2.getTerm())
                     << "lastOpWritten"
                     << BSON("ts" << opTime2.getTimestamp() << "t" << opTime2.getTerm()) << "rbid"
                     << 6 << "primaryIndex" << 12 << "syncSourceIndex" << -1 << "syncSourceHost"
                     << "")));

    // no lastOpWritten
    BSONObj inputObj(
        BSON(kOplogQueryMetadataFieldName
             << BSON("lastOpCommitted"
                     << BSON("ts" << opTime1.getTimestamp() << "t" << opTime1.getTerm())
                     << "lastCommittedWall" << committedWall << "lastOpApplied"
                     << BSON("ts" << opTime2.getTimestamp() << "t" << opTime2.getTerm()) << "rbid"
                     << 6 << "primaryIndex" << 12 << "syncSourceIndex" << -1 << "syncSourceHost"
                     << "")));

    BSONObjBuilder bob;
    bob.appendElements(inputObj);

    auto cloneStatus = OplogQueryMetadata::readFromMetadata(bob.obj());
    ASSERT_OK(cloneStatus.getStatus());

    const auto& clonedMetadata = cloneStatus.getValue();
    ASSERT_EQ(opTime1, clonedMetadata.getLastOpCommitted().opTime);
    ASSERT_EQ(opTime2, clonedMetadata.getLastOpApplied());
    ASSERT_EQ(opTime2, clonedMetadata.getLastOpWritten());
    ASSERT_EQ(committedWall, clonedMetadata.getLastOpCommitted().wallTime);
    ASSERT_TRUE(clonedMetadata.hasPrimaryIndex());

    BSONObjBuilder clonedBuilder;
    clonedMetadata.writeToMetadata(&clonedBuilder).transitional_ignore();

    BSONObj clonedSerializedObj = clonedBuilder.obj();
    ASSERT_BSONOBJ_EQ(expectedObj, clonedSerializedObj);
}

TEST(ReplResponseMetadataTest, OplogQueryMetadataHasPrimaryIndex) {
    for (auto [currentPrimaryIndex, hasPrimaryIndex] :
         std::vector<std::pair<int, bool>>{{-1, false}, {0, true}, {1, true}}) {
        OplogQueryMetadata oqm(
            {OpTime(), Date_t()}, OpTime(), OpTime(), 1, currentPrimaryIndex, -1, "");
        ASSERT_EQUALS(hasPrimaryIndex, oqm.hasPrimaryIndex());
    }
}

}  // unnamed namespace
}  // namespace rpc
}  // namespace mongo
