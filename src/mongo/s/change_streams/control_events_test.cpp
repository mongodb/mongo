/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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
