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

#include "mongo/s/change_streams/change_stream_reader_builder_impl.h"

#include "mongo/db/exec/matcher/matcher.h"
#include "mongo/db/pipeline/change_stream.h"
#include "mongo/db/pipeline/change_stream_test_helpers.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/query/compiler/parsers/matcher/expression_parser.h"
#include "mongo/db/repl/change_stream_oplog_notification.h"
#include "mongo/db/service_context_test_fixture.h"
#include "mongo/db/sharding_environment/shard_id.h"
#include "mongo/s/change_streams/all_databases_change_stream_shard_targeter_impl.h"
#include "mongo/s/change_streams/collection_change_stream_shard_targeter_impl.h"
#include "mongo/s/change_streams/database_change_stream_shard_targeter_impl.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

repl::OplogEntry buildInsertOplogEntry(const NamespaceString& nss, BSONObj o) {
    return change_stream_test_helper::makeOplogEntry(repl::OpTypeEnum::kInsert, nss, o);
}

class ChangeStreamReaderBuilderImplTest : public ServiceContextTest {
public:
    ChangeStreamReaderBuilderImplTest()
        : ServiceContextTest(
              std::make_unique<ScopedGlobalServiceContextForTest>(false /* shouldSetupTL */)) {
        _opCtx = makeOperationContext();
        _expCtx = boost::intrusive_ptr<ExpressionContextForTest>(new ExpressionContextForTest());
    }

    auto* opCtx() {
        return _opCtx.get();
    }

    auto expCtx() {
        return _expCtx;
    }

    ChangeStreamReaderBuilderImpl& readerBuilder() {
        return _changeStreamReaderBuilder;
    }

    ChangeStream collChangeStream(const NamespaceString& nss) const {
        return ChangeStream(ChangeStreamReadMode::kStrict, ChangeStreamType::kCollection, nss);
    }

    ChangeStream dbChangeStream(const NamespaceString& nss) const {
        return ChangeStream(ChangeStreamReadMode::kStrict, ChangeStreamType::kDatabase, nss);
    }

    ChangeStream clusterChangeStream() const {
        return ChangeStream(
            ChangeStreamReadMode::kStrict, ChangeStreamType::kAllDatabases, boost::none /* nss */);
    }

    bool assertMoveChunk(const ChangeStream& changeStream,
                         const MatchExpression& matchExpr,
                         const NamespaceString& nss,
                         ShardId fromShard,
                         ShardId toShard) {
        // buildMoveChunkOplogEntries() may build multiple oplog entries. The MatchExpression should
        // only potentially match the last one depending on the 'nss' attribute.
        auto moveChunkOplogEntriesWithMatchingNss =
            buildMoveChunkOplogEntries(opCtx(),
                                       nss,
                                       boost::none /* collUUID */,
                                       fromShard,
                                       toShard,
                                       true /* noMoreCollectionChunksOnDonor */,
                                       false /* firstCollectionChunkOnRecipient */);
        auto lastMoveChunkOplogEntryIt = --moveChunkOplogEntriesWithMatchingNss.end();
        for (auto it = moveChunkOplogEntriesWithMatchingNss.begin();
             it != lastMoveChunkOplogEntryIt;
             ++it) {
            auto moveChunkWithMatchingNssBSON = it->toBSON();
            BSONMatchableDocument matchingDoc(moveChunkWithMatchingNssBSON);
            ASSERT_FALSE(exec::matcher::matches(&matchExpr, &matchingDoc));
        }

        auto moveChunkWithMatchingNssBSON = lastMoveChunkOplogEntryIt->toBSON();
        BSONMatchableDocument matchingDoc(moveChunkWithMatchingNssBSON);
        return exec::matcher::matches(&matchExpr, &matchingDoc);
    }

    void assertMoveChunkTrue(const ChangeStream& changeStream,
                             const MatchExpression& matchExpr,
                             const NamespaceString& nss,
                             ShardId fromShard,
                             ShardId toShard) {
        ASSERT_TRUE(assertMoveChunk(changeStream, matchExpr, nss, fromShard, toShard));
    }

    void assertMoveChunkFalse(const ChangeStream& changeStream,
                              const MatchExpression& matchExpr,
                              const NamespaceString& nss,
                              ShardId fromShard,
                              ShardId toShard) {
        ASSERT_FALSE(assertMoveChunk(changeStream, matchExpr, nss, fromShard, toShard));
    }

private:
    ServiceContext::UniqueOperationContext _opCtx;
    boost::intrusive_ptr<ExpressionContext> _expCtx;
    ChangeStreamReaderBuilderImpl _changeStreamReaderBuilder;
};

TEST_F(
    ChangeStreamReaderBuilderImplTest,
    Given_CollectionChangeStream_When_BuildingShardTargeter_Then_ReturnsCollectionShardTargeter) {
    auto nss = NamespaceString::createNamespaceString_forTest("testDB.testCollection");
    ASSERT(dynamic_cast<CollectionChangeStreamShardTargeterImpl*>(
        readerBuilder().buildShardTargeter(opCtx(), collChangeStream(nss)).get()));
}

TEST_F(ChangeStreamReaderBuilderImplTest,
       Given_DatabaseChangeStream_When_BuildingShardTargeter_Then_ReturnsDatabaseShardTargeter) {
    auto dbNss = NamespaceString::createNamespaceString_forTest("testDB.");
    ASSERT(dynamic_cast<DatabaseChangeStreamShardTargeterImpl*>(
        readerBuilder().buildShardTargeter(opCtx(), dbChangeStream(dbNss)).get()));
}

TEST_F(ChangeStreamReaderBuilderImplTest,
       Given_ClusterChangeStream_When_BuildingShardTargeter_Then_ReturnsAllDatabasesShardTargeter) {
    ASSERT(dynamic_cast<AllDatabasesChangeStreamShardTargeterImpl*>(
        readerBuilder().buildShardTargeter(opCtx(), clusterChangeStream()).get()));
}

TEST_F(
    ChangeStreamReaderBuilderImplTest,
    Given_DataShard_When_CallingGetControlEventTypesOnDataShard_Then_ReturnsMoveChunkMovePrimaryNamespacePlacementChangedEventTypes) {
    auto nss = NamespaceString::createNamespaceString_forTest("testDB.testCollection");
    auto dbNss = NamespaceString::createNamespaceString_forTest("testDB.");
    std::set<std::string> expectedControlTypes{
        "moveChunk", "movePrimary", "namespacePlacementChanged"};
    ASSERT_EQ(expectedControlTypes,
              readerBuilder().getControlEventTypesOnDataShard(opCtx(), collChangeStream(nss)));
    ASSERT_EQ(expectedControlTypes,
              readerBuilder().getControlEventTypesOnDataShard(opCtx(), dbChangeStream(dbNss)));
    ASSERT_EQ(expectedControlTypes,
              readerBuilder().getControlEventTypesOnDataShard(opCtx(), clusterChangeStream()));
}

TEST_F(
    ChangeStreamReaderBuilderImplTest,
    Given_ConfigServer_When_CallingGetControlEventTypesOnConfigServer_Then_ReturnsInsertEventType) {
    auto nss = NamespaceString::createNamespaceString_forTest("testDB.testCollection");
    auto dbNss = NamespaceString::createNamespaceString_forTest("testDB.");
    std::set<std::string> expectedControlTypes{"insert"};
    ASSERT_EQ(expectedControlTypes,
              readerBuilder().getControlEventTypesOnConfigServer(opCtx(), collChangeStream(nss)));
    ASSERT_EQ(expectedControlTypes,
              readerBuilder().getControlEventTypesOnConfigServer(opCtx(), dbChangeStream(dbNss)));
    ASSERT_EQ(expectedControlTypes,
              readerBuilder().getControlEventTypesOnConfigServer(opCtx(), clusterChangeStream()));
}

TEST_F(
    ChangeStreamReaderBuilderImplTest,
    Given_CollectionChangeStream_When_BuildingControlEventFilterForDataShard_Then_ItMatchesOnlyRelevantOplogEntries) {
    ShardId fromShard("from");
    ShardId toShard("to");
    auto matchingNss = NamespaceString::createNamespaceString_forTest("testDB.testCollection");
    auto nonMatchingNss =
        NamespaceString::createNamespaceString_forTest("nonTestDB.nonTestCollection");
    auto changeStream = collChangeStream(matchingNss);
    auto controlEventFilter =
        readerBuilder().buildControlEventFilterForDataShard(opCtx(), changeStream);
    auto matchExpr = MatchExpressionParser::parseAndNormalize(controlEventFilter, expCtx());

    // Ensure 'controlEventFilter' matches only those movePrimary events that affect the relevant
    // db.
    {
        auto movePrimaryWithMatchingOplogEntryBSON =
            buildMovePrimaryOplogEntry(opCtx(), matchingNss.dbName(), fromShard, toShard).toBSON();
        BSONMatchableDocument matchingDoc(movePrimaryWithMatchingOplogEntryBSON);
        ASSERT_TRUE(exec::matcher::matches(matchExpr.get(), &matchingDoc));

        auto movePrimaryWithoutMatchingOplogEntryBSON =
            buildMovePrimaryOplogEntry(opCtx(), DatabaseName::kConfig, fromShard, toShard).toBSON();
        BSONMatchableDocument nonMatchingDoc(movePrimaryWithoutMatchingOplogEntryBSON);
        ASSERT_NE(changeStream.getNamespace()->dbName(), DatabaseName::kConfig);
        ASSERT_FALSE(exec::matcher::matches(matchExpr.get(), &nonMatchingDoc));
    }

    // Ensure 'controlEventFilter' matches only those namespacePlacementChanged events that affect
    // the relevant namespace.
    {
        // Ensure full collection namespaces are matched accordingly.
        {
            NamespacePlacementChanged eventWithMatchingNss(matchingNss, Timestamp(100));
            auto namespacePlacementChangedWithMatchingOplogEntryBSON =
                buildNamespacePlacementChangedOplogEntry(opCtx(), eventWithMatchingNss).toBSON();
            BSONMatchableDocument matchingDoc(namespacePlacementChangedWithMatchingOplogEntryBSON);
            ASSERT_EQ(*changeStream.getNamespace(), matchingNss);
            ASSERT_TRUE(exec::matcher::matches(matchExpr.get(), &matchingDoc));

            NamespacePlacementChanged eventWithoutMatchingNss(nonMatchingNss, Timestamp(100));
            auto namespacePlacementChangedWithoutMatchingOplogEntryBSON =
                buildNamespacePlacementChangedOplogEntry(opCtx(), eventWithoutMatchingNss).toBSON();
            BSONMatchableDocument nonMatchingDoc(
                namespacePlacementChangedWithoutMatchingOplogEntryBSON);
            ASSERT_NE(*changeStream.getNamespace(), nonMatchingNss);
            ASSERT_FALSE(exec::matcher::matches(matchExpr.get(), &nonMatchingDoc));
        }

        // Ensure db only namespaces are matched accordingly.
        {
            const auto nssWithMatchingDb = NamespaceString(
                DatabaseName::createDatabaseName_forTest(boost::none /* tenantId*/, "testDB"));
            NamespacePlacementChanged eventWithMatchingNss(nssWithMatchingDb, Timestamp(100));
            auto namespacePlacementChangedWithMatchingOplogEntryBSON =
                buildNamespacePlacementChangedOplogEntry(opCtx(), eventWithMatchingNss).toBSON();
            BSONMatchableDocument matchingDoc(namespacePlacementChangedWithMatchingOplogEntryBSON);
            ASSERT_TRUE(exec::matcher::matches(matchExpr.get(), &matchingDoc));

            const auto nssWithNonMatchingDb = NamespaceString(
                DatabaseName::createDatabaseName_forTest(boost::none /* tenantId */, "nonTestDB"));
            NamespacePlacementChanged eventWithoutMatchingNss(nssWithNonMatchingDb, Timestamp(100));
            auto namespacePlacementChangedWithoutMatchingOplogEntryBSON =
                buildNamespacePlacementChangedOplogEntry(opCtx(), eventWithoutMatchingNss).toBSON();
            BSONMatchableDocument nonMatchingDoc(
                namespacePlacementChangedWithoutMatchingOplogEntryBSON);
            ASSERT_FALSE(exec::matcher::matches(matchExpr.get(), &nonMatchingDoc));
        }

        // Ensure cluster case is matched accordingly.
        {
            const auto emptyNss = NamespaceString::kEmpty;
            NamespacePlacementChanged eventWithEmptyNss(emptyNss, Timestamp(100));
            auto namespacePlacementChangedWithEmptyNssOplogEntryBSON =
                buildNamespacePlacementChangedOplogEntry(opCtx(), eventWithEmptyNss).toBSON();
            BSONMatchableDocument matchingDoc(namespacePlacementChangedWithEmptyNssOplogEntryBSON);
            ASSERT_TRUE(exec::matcher::matches(matchExpr.get(), &matchingDoc));
        }
    }

    // Ensure 'controlEventFilter' matches only those moveChunk events that affect the relevant
    // namespace.
    assertMoveChunkTrue(changeStream, *matchExpr, matchingNss, fromShard, toShard);
    assertMoveChunkFalse(changeStream, *matchExpr, nonMatchingNss, fromShard, toShard);
}

TEST_F(
    ChangeStreamReaderBuilderImplTest,
    Given_DatabaseChangeStream_When_BuildingControlEventFilterForDataShard_Then_ItMatchesOnlyRelevantOplogEntries) {
    ShardId fromShard("from");
    ShardId toShard("to");
    auto matchingNss1 = NamespaceString::createNamespaceString_forTest("testDB.testCollection1");
    auto matchingNss2 = NamespaceString::createNamespaceString_forTest("testDB.testCollection2");
    auto nonMatchingNss =
        NamespaceString::createNamespaceString_forTest("nonTestDB.nonTestCollection");
    auto changeStream = dbChangeStream(NamespaceString::createNamespaceString_forTest("testDB."));
    auto controlEventFilter =
        readerBuilder().buildControlEventFilterForDataShard(opCtx(), changeStream);
    auto matchExpr = MatchExpressionParser::parseAndNormalize(controlEventFilter, expCtx());

    // Ensure 'controlEventFilter' matches only those movePrimary events that affect the relevant
    // db.
    {
        auto movePrimaryWithMatchingNss1OplogEntryBSON =
            buildMovePrimaryOplogEntry(opCtx(), matchingNss1.dbName(), fromShard, toShard).toBSON();
        BSONMatchableDocument matchingDoc1(movePrimaryWithMatchingNss1OplogEntryBSON);
        ASSERT_TRUE(exec::matcher::matches(matchExpr.get(), &matchingDoc1));

        auto movePrimaryWithMatchingNss2OplogEntryBSON =
            buildMovePrimaryOplogEntry(opCtx(), matchingNss2.dbName(), fromShard, toShard).toBSON();
        BSONMatchableDocument matchingDoc2(movePrimaryWithMatchingNss2OplogEntryBSON);
        ASSERT_TRUE(exec::matcher::matches(matchExpr.get(), &matchingDoc2));

        auto movePrimaryWithoutMatchingOplogEntryBSON =
            buildMovePrimaryOplogEntry(opCtx(), DatabaseName::kConfig, fromShard, toShard).toBSON();
        BSONMatchableDocument nonMatchingDoc(movePrimaryWithoutMatchingOplogEntryBSON);
        ASSERT_NE(changeStream.getNamespace()->dbName(), DatabaseName::kConfig);
        ASSERT_FALSE(exec::matcher::matches(matchExpr.get(), &nonMatchingDoc));
    }

    // Ensure 'controlEventFilter' matches only those namespacePlacementChanged events that affect
    // the relevant namespace.
    {
        // Ensure full collection namespaces are matched accordingly.
        {
            NamespacePlacementChanged eventWithMatchingNss1(matchingNss1, Timestamp(100));
            auto namespacePlacementChangedWithMatchingNss1OplogEntryBSON =
                buildNamespacePlacementChangedOplogEntry(opCtx(), eventWithMatchingNss1).toBSON();
            BSONMatchableDocument matchingDoc1(
                namespacePlacementChangedWithMatchingNss1OplogEntryBSON);
            ASSERT_TRUE(exec::matcher::matches(matchExpr.get(), &matchingDoc1));

            NamespacePlacementChanged eventWithMatchingNss2(matchingNss2, Timestamp(100));
            auto namespacePlacementChangedWithMatchingNss2OplogEntryBSON =
                buildNamespacePlacementChangedOplogEntry(opCtx(), eventWithMatchingNss2).toBSON();
            BSONMatchableDocument matchingDoc2(
                namespacePlacementChangedWithMatchingNss2OplogEntryBSON);
            ASSERT_TRUE(exec::matcher::matches(matchExpr.get(), &matchingDoc2));

            NamespacePlacementChanged eventWithoutMatchingNss(nonMatchingNss, Timestamp(100));
            auto namespacePlacementChangedWithoutMatchingOplogEntryBSON =
                buildNamespacePlacementChangedOplogEntry(opCtx(), eventWithoutMatchingNss).toBSON();
            BSONMatchableDocument nonMatchingDoc(
                namespacePlacementChangedWithoutMatchingOplogEntryBSON);
            ASSERT_NE(*changeStream.getNamespace(), nonMatchingNss);
            ASSERT_FALSE(exec::matcher::matches(matchExpr.get(), &nonMatchingDoc));
        }

        // Ensure db only namespaces are matched accordingly.
        {
            const auto nssWithMatchingDb = NamespaceString(
                DatabaseName::createDatabaseName_forTest(boost::none /* tenantId*/, "testDB"));
            NamespacePlacementChanged eventWithMatchingNss(nssWithMatchingDb, Timestamp(100));
            auto namespacePlacementChangedWithMatchingOplogEntryBSON =
                buildNamespacePlacementChangedOplogEntry(opCtx(), eventWithMatchingNss).toBSON();
            BSONMatchableDocument matchingDoc(namespacePlacementChangedWithMatchingOplogEntryBSON);
            ASSERT_TRUE(exec::matcher::matches(matchExpr.get(), &matchingDoc));

            const auto nssWithNonMatchingDb = NamespaceString(
                DatabaseName::createDatabaseName_forTest(boost::none /* tenantId */, "nonTestDB"));
            NamespacePlacementChanged eventWithoutMatchingNss(nssWithNonMatchingDb, Timestamp(100));
            auto namespacePlacementChangedWithoutMatchingOplogEntryBSON =
                buildNamespacePlacementChangedOplogEntry(opCtx(), eventWithoutMatchingNss).toBSON();
            BSONMatchableDocument nonMatchingDoc(
                namespacePlacementChangedWithoutMatchingOplogEntryBSON);
            ASSERT_FALSE(exec::matcher::matches(matchExpr.get(), &nonMatchingDoc));
        }

        // Ensure cluster case is matched accordingly.
        {
            const auto emptyNss = NamespaceString::kEmpty;
            NamespacePlacementChanged eventWithEmptyNss(emptyNss, Timestamp(100));
            auto namespacePlacementChangedWithEmptyNssOplogEntryBSON =
                buildNamespacePlacementChangedOplogEntry(opCtx(), eventWithEmptyNss).toBSON();
            BSONMatchableDocument matchingDoc(namespacePlacementChangedWithEmptyNssOplogEntryBSON);
            ASSERT_TRUE(exec::matcher::matches(matchExpr.get(), &matchingDoc));
        }
    }

    // Ensure 'controlEventFilter' matches only those moveChunk events that affect the relevant
    // namespace.
    assertMoveChunkTrue(changeStream, *matchExpr, matchingNss1, fromShard, toShard);
    assertMoveChunkTrue(changeStream, *matchExpr, matchingNss2, fromShard, toShard);
    assertMoveChunkFalse(changeStream, *matchExpr, nonMatchingNss, fromShard, toShard);
}

TEST_F(
    ChangeStreamReaderBuilderImplTest,
    Given_AllDatabasesChangeStream_When_BuildingControlEventFilterForDataShard_Then_ItMatchesOnlyRelevantOplogEntries) {
    ShardId fromShard("from");
    ShardId toShard("to");
    auto nss1 = NamespaceString::createNamespaceString_forTest("testDB.testCollection");
    auto nss2 = NamespaceString::createNamespaceString_forTest("nonTestDB.nonTestCollection");
    auto changeStream = clusterChangeStream();
    auto controlEventFilter =
        readerBuilder().buildControlEventFilterForDataShard(opCtx(), changeStream);
    auto matchExpr = MatchExpressionParser::parseAndNormalize(controlEventFilter, expCtx());

    // Ensure 'controlEventFilter' matches all movePrimary events.
    {
        auto movePrimaryForNss1OplogEntryBSON =
            buildMovePrimaryOplogEntry(opCtx(), nss1.dbName(), fromShard, toShard).toBSON();
        BSONMatchableDocument matchingDocForNss1(movePrimaryForNss1OplogEntryBSON);
        ASSERT_TRUE(exec::matcher::matches(matchExpr.get(), &matchingDocForNss1));

        auto movePrimaryForNss2OplogEntryBSON =
            buildMovePrimaryOplogEntry(opCtx(), nss2.dbName(), fromShard, toShard).toBSON();
        BSONMatchableDocument matchingDocForNss2(movePrimaryForNss2OplogEntryBSON);
        ASSERT_TRUE(exec::matcher::matches(matchExpr.get(), &matchingDocForNss2));
    }

    // Ensure 'controlEventFilter' matches all namespacePlacementChanged events.
    {
        // Ensure various nss are matched accordingly.
        {
            NamespacePlacementChanged eventWithNss1(nss1, Timestamp(100));
            auto namespacePlacementChangedWithNss1OplogEntryBSON =
                buildNamespacePlacementChangedOplogEntry(opCtx(), eventWithNss1).toBSON();
            BSONMatchableDocument matchingDoc1(namespacePlacementChangedWithNss1OplogEntryBSON);
            ASSERT_TRUE(exec::matcher::matches(matchExpr.get(), &matchingDoc1));

            NamespacePlacementChanged eventWithNss2(nss2, Timestamp(100));
            auto namespacePlacementChangedWithNss2OplogEntryBSON =
                buildNamespacePlacementChangedOplogEntry(opCtx(), eventWithNss2).toBSON();
            BSONMatchableDocument matchingDoc2(namespacePlacementChangedWithNss2OplogEntryBSON);
            ASSERT_TRUE(exec::matcher::matches(matchExpr.get(), &matchingDoc2));
        }

        // Ensure cluster case is matched accordingly.
        {
            const auto emptyNss = NamespaceString::kEmpty;
            NamespacePlacementChanged eventWithEmptyNss(emptyNss, Timestamp(100));
            auto namespacePlacementChangedWithEmptyNssOplogEntryBSON =
                buildNamespacePlacementChangedOplogEntry(opCtx(), eventWithEmptyNss).toBSON();
            BSONMatchableDocument matchingDoc(namespacePlacementChangedWithEmptyNssOplogEntryBSON);
            ASSERT_TRUE(exec::matcher::matches(matchExpr.get(), &matchingDoc));
        }
    }

    // Ensure 'controlEventFilter' matches all moveChunk events.
    assertMoveChunkTrue(changeStream, *matchExpr, nss1, fromShard, toShard);
    assertMoveChunkTrue(changeStream, *matchExpr, nss2, fromShard, toShard);
}

TEST_F(
    ChangeStreamReaderBuilderImplTest,
    Given_CollectionChangeStream_When_BuildingControlEventFilterForConfigServer_Then_ItMatchesOnlyRelevantOplogEntries) {
    auto configDatabasesNss = NamespaceString::kConfigDatabasesNamespace;
    auto configSessionsNss = NamespaceString::kLogicalSessionsNamespace;
    auto matchingNss = NamespaceString::createNamespaceString_forTest("testDB.testCollection");
    auto nonMatchingNss =
        NamespaceString::createNamespaceString_forTest("nonTestDB.nonTestCollection");
    auto changeStream = collChangeStream(matchingNss);
    auto controlEventFilter =
        readerBuilder().buildControlEventFilterForConfigServer(opCtx(), changeStream);
    auto matchExpr = MatchExpressionParser::parseAndNormalize(controlEventFilter, expCtx());

    // Ensure that database creation event with 'matchingNss' will match the 'controlEventFilter'.
    auto dbCreatedWithMatchingDbOplogEntryBSON =
        buildInsertOplogEntry(configDatabasesNss,
                              BSON("_id" << matchingNss.dbName().toString_forTest()))
            .getEntry()
            .toBSON();
    BSONMatchableDocument matchingDoc(dbCreatedWithMatchingDbOplogEntryBSON);
    ASSERT_TRUE(exec::matcher::matches(matchExpr.get(), &matchingDoc));

    // Ensure that database creation event with 'nonMatchingNss' will not match the
    // 'controlEventFilter'.
    auto dbCreatedWithoutMatchingDbOplogEntryBSON =
        buildInsertOplogEntry(configDatabasesNss,
                              BSON("_id" << nonMatchingNss.dbName().toString_forTest()))
            .getEntry()
            .toBSON();
    BSONMatchableDocument nonMatchingDoc1(dbCreatedWithoutMatchingDbOplogEntryBSON);
    ASSERT_FALSE(exec::matcher::matches(matchExpr.get(), &nonMatchingDoc1));

    // Ensure that inserts to different namespace are not caught by the filter.
    auto insertIntoDifferentNssOplogEntryBSON =
        buildInsertOplogEntry(configSessionsNss,
                              BSON("_id" << matchingNss.dbName().toString_forTest()))
            .getEntry()
            .toBSON();
    BSONMatchableDocument nonMatchingDoc2(insertIntoDifferentNssOplogEntryBSON);
    ASSERT_FALSE(exec::matcher::matches(matchExpr.get(), &nonMatchingDoc2));
}

TEST_F(
    ChangeStreamReaderBuilderImplTest,
    Given_DatabaseChangeStream_When_BuildingControlEventFilterForConfigServer_Then_ItMatchesOnlyRelevantOplogEntries) {
    auto configDatabasesNss = NamespaceString::kConfigDatabasesNamespace;
    auto configSessionsNss = NamespaceString::kLogicalSessionsNamespace;
    auto matchingNss = NamespaceString::createNamespaceString_forTest("testDB.testCollection");
    auto nonMatchingNss =
        NamespaceString::createNamespaceString_forTest("nonTestDB.nonTestCollection");
    auto changeStream = dbChangeStream(NamespaceString::createNamespaceString_forTest("testDB."));
    auto controlEventFilter =
        readerBuilder().buildControlEventFilterForConfigServer(opCtx(), changeStream);
    auto matchExpr = MatchExpressionParser::parseAndNormalize(controlEventFilter, expCtx());

    // Ensure that database creation event with 'matchingNss' will match the 'controlEventFilter'.
    auto dbCreatedWithMatchingDbOplogEntryBSON =
        buildInsertOplogEntry(configDatabasesNss,
                              BSON("_id" << matchingNss.dbName().toString_forTest()))
            .getEntry()
            .toBSON();
    BSONMatchableDocument matchingDoc(dbCreatedWithMatchingDbOplogEntryBSON);
    ASSERT_TRUE(exec::matcher::matches(matchExpr.get(), &matchingDoc));

    // Ensure that database creation event with 'nonMatchingNss' will not match the
    // 'controlEventFilter'.
    auto dbCreatedWithoutMatchingDbOplogEntryBSON =
        buildInsertOplogEntry(configDatabasesNss,
                              BSON("_id" << nonMatchingNss.dbName().toString_forTest()))
            .getEntry()
            .toBSON();
    BSONMatchableDocument nonMatchingDoc1(dbCreatedWithoutMatchingDbOplogEntryBSON);
    ASSERT_FALSE(exec::matcher::matches(matchExpr.get(), &nonMatchingDoc1));

    // Ensure that inserts to different namespace are not caught by the filter.
    auto insertIntoDifferentNssOplogEntryBSON =
        buildInsertOplogEntry(configSessionsNss,
                              BSON("_id" << matchingNss.dbName().toString_forTest()))
            .getEntry()
            .toBSON();
    BSONMatchableDocument nonMatchingDoc2(insertIntoDifferentNssOplogEntryBSON);
    ASSERT_FALSE(exec::matcher::matches(matchExpr.get(), &nonMatchingDoc2));
}

TEST_F(
    ChangeStreamReaderBuilderImplTest,
    Given_AllDatabasesChangeStream_When_BuildingControlEventFilterForConfigServer_Then_ItMatchesOnlyRelevantOplogEntries) {
    auto configDatabasesNss = NamespaceString::kConfigDatabasesNamespace;
    auto configSessionsNss = NamespaceString::kLogicalSessionsNamespace;
    auto matchingNss1 = NamespaceString::createNamespaceString_forTest("testDB.testCollection");
    auto matchingNss2 =
        NamespaceString::createNamespaceString_forTest("nonTestDB.nonTestCollection");
    auto changeStream = clusterChangeStream();
    auto controlEventFilter =
        readerBuilder().buildControlEventFilterForConfigServer(opCtx(), changeStream);
    auto matchExpr = MatchExpressionParser::parseAndNormalize(controlEventFilter, expCtx());

    // Ensure that different database creation events will match the 'controlEventFilter'.
    {
        auto dbCreated1OplogEntryBSON =
            buildInsertOplogEntry(configDatabasesNss,
                                  BSON("_id" << matchingNss1.dbName().toString_forTest()))
                .getEntry()
                .toBSON();
        BSONMatchableDocument matchingDoc1(dbCreated1OplogEntryBSON);
        ASSERT_TRUE(exec::matcher::matches(matchExpr.get(), &matchingDoc1));

        auto dbCreated2OplogEntryBSON =
            buildInsertOplogEntry(configDatabasesNss,
                                  BSON("_id" << matchingNss2.dbName().toString_forTest()))
                .getEntry()
                .toBSON();
        BSONMatchableDocument matchingDoc2(dbCreated2OplogEntryBSON);
        ASSERT_TRUE(exec::matcher::matches(matchExpr.get(), &matchingDoc2));
    }

    // Ensure that inserts to different namespace are not caught by the filter.
    auto insertIntoDifferentNssOplogEntryBSON =
        buildInsertOplogEntry(configSessionsNss,
                              BSON("_id" << matchingNss1.dbName().toString_forTest()))
            .getEntry()
            .toBSON();
    BSONMatchableDocument nonMatchingDoc(insertIntoDifferentNssOplogEntryBSON);
    ASSERT_FALSE(exec::matcher::matches(matchExpr.get(), &nonMatchingDoc));
}

}  // namespace
}  // namespace mongo
