// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/s/change_streams/control_events.h"

#include "mongo/db/namespace_spec_gen.h"
#include "mongo/unittest/unittest.h"

#include <boost/optional/optional.hpp>

namespace mongo {
namespace {

TEST(ControlEventTest,
     GivenInvalidEventAsDocument_WhenCallingParseControlEvent_ThenParsingThrowsAnException) {
    Document event = Document(BSON("operationType" << "update"));
    ASSERT_THROWS(parseControlEvent(event), DBException);
}

TEST(
    ControlEventTest,
    GivenValidMoveChunkControlEventAsDocument_WhenCallingParseControlEvent_ThenParsingIsSuccessful) {
    Timestamp ts;
    ShardId donorShard("fromShard");
    ShardId recipientShard("toShard");
    Document event = Document(
        BSON("operationType" << MoveChunkControlEvent::opType << "clusterTime" << ts
                             << "operationDescription"
                             << BSON("donor" << donorShard << "recipient" << recipientShard
                                             << "allCollectionChunksMigratedFromDonor" << true)));

    ControlEvent expectedControlEvent = MoveChunkControlEvent{ts, donorShard, recipientShard, true};
    ASSERT_EQ(parseControlEvent(event), expectedControlEvent);
}

TEST(
    ControlEventTest,
    GivenInvalidMoveChunkControlEventAsDocument_WhenCallingParseControlEvent_ThenParsingThrowsAnException) {
    Document event = Document(BSON("operationType" << MoveChunkControlEvent::opType));
    ASSERT_THROWS(parseControlEvent(event), DBException);
}

TEST(
    ControlEventTest,
    GivenValidMovePrimaryControlEventAsDocument_WhenCallingParseControlEvent_ThenParsingIsSuccessful) {
    Timestamp ts;
    ShardId donorShard("fromShard");
    ShardId recipientShard("toShard");
    Document event =
        Document(BSON("operationType" << MovePrimaryControlEvent::opType << "clusterTime" << ts
                                      << "operationDescription"
                                      << BSON("from" << donorShard << "to" << recipientShard)));

    ControlEvent expectedControlEvent = MovePrimaryControlEvent{ts, donorShard, recipientShard};
    ASSERT_EQ(parseControlEvent(event), expectedControlEvent);
}

TEST(
    ControlEventTest,
    GivenInvalidMovePrimaryControlEventAsDocument_WhenCallingParseControlEvent_ThenParsingThrowsAnException) {
    Document event = Document(BSON("operationType" << MovePrimaryControlEvent::opType));
    ASSERT_THROWS(parseControlEvent(event), DBException);
}

TEST(
    ControlEventTest,
    GivenValidNamespacePlacementChangedControlEventAsDocument_WhenCallingParseControlEvent_ThenParsingIsSuccessful) {
    Timestamp ts;

    auto nss = NamespaceString::kDefaultInitialSyncIdNamespace;
    auto nssSpec = [&]() {
        NamespaceSpec nssSpec;
        nssSpec.setDb(nss.dbName());
        nssSpec.setColl(nss.coll());
        return nssSpec;
    }();

    Document event =
        Document(BSON("operationType" << NamespacePlacementChangedControlEvent::opType
                                      << "clusterTime" << ts << "ns" << nssSpec.toBSON()));

    ControlEvent expectedControlEvent = NamespacePlacementChangedControlEvent{ts, nss};
    ASSERT_EQ(parseControlEvent(event), expectedControlEvent);
}

TEST(
    ControlEventTest,
    GivenValidNamespacePlacementChangedControlEventAsDocumentWithDbFieldOnly_WhenCallingParseControlEvent_ThenParsingIsSuccessful) {
    Timestamp ts;

    NamespaceString nss(NamespaceString::kDefaultInitialSyncIdNamespace.dbName());
    auto nssSpec = [&]() {
        NamespaceSpec nssSpec;
        nssSpec.setDb(nss.dbName());
        return nssSpec;
    }();

    Document event =
        Document(BSON("operationType" << NamespacePlacementChangedControlEvent::opType
                                      << "clusterTime" << ts << "ns" << nssSpec.toBSON()));

    ControlEvent expectedControlEvent = NamespacePlacementChangedControlEvent{ts, nss};
    ASSERT_EQ(parseControlEvent(event), expectedControlEvent);
}

TEST(
    ControlEventTest,
    GivenValidNamespacePlacementChangedControlEventAsDocumentForTheWholeCluster_WhenCallingParseControlEvent_ThenParsingIsSuccessful) {
    Timestamp ts;

    Document event =
        Document(BSON("operationType" << NamespacePlacementChangedControlEvent::opType
                                      << "clusterTime" << ts << "ns" << NamespaceSpec().toBSON()));

    ControlEvent expectedControlEvent =
        NamespacePlacementChangedControlEvent{ts, NamespaceString::kEmpty};
    ASSERT_EQ(parseControlEvent(event), expectedControlEvent);
}

TEST(
    ControlEventTest,
    GivenInvalidNamespacePlacementChangedControlEventAsDocumentWithOnlyCollectionBeingProvided_WhenCallingParseControlEvent_ThenParsingThrowsAnException) {
    Document event =
        Document(BSON("operationType" << NamespacePlacementChangedControlEvent::opType));
    ASSERT_THROWS(parseControlEvent(event), DBException);
}

TEST(
    ControlEventTest,
    GivenInvalidNamespacePlacementChangedControlEventAsDocument_WhenCallingParseControlEvent_ThenParsingThrowsAnException) {
    Document event =
        Document(BSON("operationType" << NamespacePlacementChangedControlEvent::opType));
    ASSERT_THROWS(parseControlEvent(event), DBException);
}

TEST(
    ControlEventTest,
    GivenValidDatabaseCreatedControlEventAsDocument_WhenCallingParseControlEvent_ThenParsingIsSuccessful) {
    Timestamp ts;
    DatabaseName dbName = DatabaseName::kConfig;
    Document event = Document(BSON("operationType" << DatabaseCreatedControlEvent::opType
                                                   << "clusterTime" << ts << "fullDocument"
                                                   << BSON("_id" << dbName.toString_forTest())));

    ControlEvent expectedControlEvent = DatabaseCreatedControlEvent{ts, dbName};
    ASSERT_EQ(parseControlEvent(event), expectedControlEvent);
}

TEST(
    ControlEventTest,
    GivenInvalidDatabaseCreatedControlEventAsDocument_WhenCallingParseControlEvent_ThenParsingThrowsAnException) {
    Document event = Document(BSON("operationType" << DatabaseCreatedControlEvent::opType));
    ASSERT_THROWS(parseControlEvent(event), DBException);
}

}  // namespace
}  // namespace mongo
