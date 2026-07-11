// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/rpc/metadata/oplog_query_metadata.h"

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
