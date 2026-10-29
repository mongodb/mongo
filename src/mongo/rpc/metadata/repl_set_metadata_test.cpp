// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/rpc/metadata/repl_set_metadata.h"

#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/timestamp.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/duration.h"
#include "mongo/util/time_support.h"

namespace mongo {
namespace rpc {
namespace {

using repl::OpTime;
using repl::OpTimeAndWallTime;

static const OpTime opTime(Timestamp(1234, 100), 5);
static const OpTime opTime2(Timestamp(7777, 100), 6);
static const Date_t committedWallTime = Date_t() + Seconds(opTime.getSecs());
static const ReplSetMetadata metadata(
    3, {opTime, committedWallTime}, opTime2, 6, 0, OID("abcdefabcdefabcdefabcdef"), -1, false);

TEST(ReplResponseMetadataTest, ReplicaSetIdNotSet) {
    ASSERT_FALSE(ReplSetMetadata(3, OpTimeAndWallTime(), OpTime(), 6, 0, OID(), -1, false)
                     .hasReplicaSetId());
}

TEST(ReplResponseMetadataTest, Roundtrip) {
    ASSERT_EQ(opTime, metadata.getLastOpCommitted().opTime);
    ASSERT_EQ(committedWallTime, metadata.getLastOpCommitted().wallTime);
    ASSERT_EQ(opTime2, metadata.getLastOpVisible());
    ASSERT_TRUE(metadata.hasReplicaSetId());

    BSONObjBuilder builder;
    metadata.writeToMetadata(&builder).transitional_ignore();

    BSONObj expectedObj(
        BSON(kReplSetMetadataFieldName
             << BSON("term" << 3 << "lastOpCommitted"
                            << BSON("ts" << opTime.getTimestamp() << "t" << opTime.getTerm())
                            << "lastCommittedWall" << committedWallTime << "lastOpVisible"
                            << BSON("ts" << opTime2.getTimestamp() << "t" << opTime2.getTerm())
                            << "configVersion" << 6 << "configTerm" << 0 << "replicaSetId"
                            << metadata.getReplicaSetId() << "syncSourceIndex" << -1 << "isPrimary"
                            << false)));

    BSONObj serializedObj = builder.obj();
    ASSERT_BSONOBJ_EQ(expectedObj, serializedObj);

    // Verify that we allow unknown fields.
    BSONObjBuilder bob;
    bob.appendElements(serializedObj);
    bob.append("unknownField", 1);

    auto cloneStatus = ReplSetMetadata::readFromMetadata(bob.obj());
    ASSERT_OK(cloneStatus.getStatus());

    const auto& clonedMetadata = cloneStatus.getValue();
    ASSERT_EQ(opTime, clonedMetadata.getLastOpCommitted().opTime);
    ASSERT_EQ(opTime2, clonedMetadata.getLastOpVisible());
    ASSERT_EQ(committedWallTime, clonedMetadata.getLastOpCommitted().wallTime);
    ASSERT_EQ(metadata.getConfigVersion(), clonedMetadata.getConfigVersion());
    ASSERT_EQ(metadata.getConfigTerm(), clonedMetadata.getConfigTerm());
    ASSERT_EQ(metadata.getReplicaSetId(), clonedMetadata.getReplicaSetId());

    BSONObjBuilder clonedBuilder;
    clonedMetadata.writeToMetadata(&clonedBuilder).transitional_ignore();

    BSONObj clonedSerializedObj = clonedBuilder.obj();
    ASSERT_BSONOBJ_EQ(expectedObj, clonedSerializedObj);
}

TEST(ReplResponseMetadataTest, MetadataCanBeConstructedWhenMissingOplogQueryMetadataFields) {
    BSONObj obj(BSON(kReplSetMetadataFieldName << BSON(
                         "term" << 3 << "configVersion" << 6 << "configTerm" << 2 << "replicaSetId"
                                << metadata.getReplicaSetId() << "lastCommittedWall"
                                << committedWallTime << "isPrimary" << false)));

    auto status = ReplSetMetadata::readFromMetadata(obj);
    ASSERT_OK(status.getStatus());

    const auto& metadata = status.getValue();
    ASSERT_EQ(metadata.getConfigVersion(), 6);
    ASSERT_EQ(metadata.getConfigTerm(), 2);
    ASSERT_EQ(metadata.getReplicaSetId(), metadata.getReplicaSetId());
    ASSERT_EQ(metadata.getTerm(), 3);
}
}  // unnamed namespace
}  // namespace rpc
}  // namespace mongo
