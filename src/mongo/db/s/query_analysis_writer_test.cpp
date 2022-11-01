/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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

#include "mongo/db/s/query_analysis_writer.h"

#include "mongo/bson/unordered_fields_bsonobj_comparator.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/s/shard_server_test_fixture.h"
#include "mongo/db/update/document_diff_calculator.h"
#include "mongo/idl/server_parameter_test_util.h"
#include "mongo/logv2/log.h"
#include "mongo/s/analyze_shard_key_documents_gen.h"
#include "mongo/unittest/death_test.h"
#include "mongo/util/fail_point.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest

namespace mongo {
namespace analyze_shard_key {
namespace {

TEST(QueryAnalysisWriterBufferTest, AddBasic) {
    auto buffer = QueryAnalysisWriter::Buffer();

    auto doc0 = BSON("a" << 0);
    buffer.add(doc0);
    ASSERT_EQ(buffer.getCount(), 1);
    ASSERT_EQ(buffer.getSize(), doc0.objsize());

    auto doc1 = BSON("a" << BSON_ARRAY(0 << 1 << 2));
    buffer.add(doc1);
    ASSERT_EQ(buffer.getCount(), 2);
    ASSERT_EQ(buffer.getSize(), doc0.objsize() + doc1.objsize());

    ASSERT_BSONOBJ_EQ(buffer.at(0), doc0);
    ASSERT_BSONOBJ_EQ(buffer.at(1), doc1);
}

TEST(QueryAnalysisWriterBufferTest, AddTooLarge) {
    auto buffer = QueryAnalysisWriter::Buffer();

    auto doc = BSON(std::string(BSONObjMaxUserSize, 'a') << 1);
    buffer.add(doc);
    ASSERT_EQ(buffer.getCount(), 0);
    ASSERT_EQ(buffer.getSize(), 0);
}

TEST(QueryAnalysisWriterBufferTest, TruncateBasic) {
    auto testTruncateCommon = [](int oldCount, int newCount) {
        auto buffer = QueryAnalysisWriter::Buffer();

        std::vector<BSONObj> docs;
        for (auto i = 0; i < oldCount; i++) {
            docs.push_back(BSON("a" << i));
        }
        // The documents have the same size.
        auto docSize = docs.back().objsize();

        for (const auto& doc : docs) {
            buffer.add(doc);
        }
        ASSERT_EQ(buffer.getCount(), oldCount);
        ASSERT_EQ(buffer.getSize(), oldCount * docSize);

        buffer.truncate(newCount, (oldCount - newCount) * docSize);
        ASSERT_EQ(buffer.getCount(), newCount);
        ASSERT_EQ(buffer.getSize(), newCount * docSize);
        for (auto i = 0; i < newCount; i++) {
            ASSERT_BSONOBJ_EQ(buffer.at(i), docs[i]);
        }
    };

    testTruncateCommon(10 /* oldCount */, 6 /* newCount */);
    testTruncateCommon(10 /* oldCount */, 0 /* newCount */);  // Truncate all.
    testTruncateCommon(10 /* oldCount */, 9 /* newCount */);  // Truncate one.
}

DEATH_TEST(QueryAnalysisWriterBufferTest, TruncateInvalidIndex_Negative, "invariant") {
    auto buffer = QueryAnalysisWriter::Buffer();

    auto doc = BSON("a" << 0);
    buffer.add(doc);
    ASSERT_EQ(buffer.getCount(), 1);
    ASSERT_EQ(buffer.getSize(), doc.objsize());

    buffer.truncate(-1, doc.objsize());
}

DEATH_TEST(QueryAnalysisWriterBufferTest, TruncateInvalidIndex_Positive, "invariant") {
    auto buffer = QueryAnalysisWriter::Buffer();

    auto doc = BSON("a" << 0);
    buffer.add(doc);
    ASSERT_EQ(buffer.getCount(), 1);
    ASSERT_EQ(buffer.getSize(), doc.objsize());

    buffer.truncate(2, doc.objsize());
}

DEATH_TEST(QueryAnalysisWriterBufferTest, TruncateInvalidSize_Negative, "invariant") {
    auto buffer = QueryAnalysisWriter::Buffer();

    auto doc = BSON("a" << 0);
    buffer.add(doc);
    ASSERT_EQ(buffer.getCount(), 1);
    ASSERT_EQ(buffer.getSize(), doc.objsize());

    buffer.truncate(0, -doc.objsize());
}

DEATH_TEST(QueryAnalysisWriterBufferTest, TruncateInvalidSize_Zero, "invariant") {
    auto buffer = QueryAnalysisWriter::Buffer();

    auto doc = BSON("a" << 0);
    buffer.add(doc);
    ASSERT_EQ(buffer.getCount(), 1);
    ASSERT_EQ(buffer.getSize(), doc.objsize());

    buffer.truncate(0, 0);
}

DEATH_TEST(QueryAnalysisWriterBufferTest, TruncateInvalidSize_Positive, "invariant") {
    auto buffer = QueryAnalysisWriter::Buffer();

    auto doc = BSON("a" << 0);
    buffer.add(doc);
    ASSERT_EQ(buffer.getCount(), 1);
    ASSERT_EQ(buffer.getSize(), doc.objsize());

    buffer.truncate(0, doc.objsize() * 2);
}

struct QueryAnalysisWriterTest : public ShardServerTestFixture {
public:
    void setUp() {
        ShardServerTestFixture::setUp();
        QueryAnalysisWriter::get(operationContext()).onStartup();

        DBDirectClient client(operationContext());
        client.createCollection(nss0.toString());
        client.createCollection(nss1.toString());
    }

    void tearDown() {
        QueryAnalysisWriter::get(operationContext()).onShutdown();
        ShardServerTestFixture::tearDown();
    }

protected:
    UUID getCollectionUUID(const NamespaceString& nss) const {
        auto collectionCatalog = CollectionCatalog::get(operationContext());
        return *collectionCatalog->lookupUUIDByNSS(operationContext(), nss);
    }

    BSONObj makeNonEmptyFilter() {
        return BSON("_id" << UUID::gen());
    }

    BSONObj makeNonEmptyCollation() {
        int strength = rand() % 5 + 1;
        return BSON("locale"
                    << "en_US"
                    << "strength" << strength);
    }

    void deleteSampledQueryDocuments() const {
        DBDirectClient client(operationContext());
        client.remove(NamespaceString::kConfigSampledQueriesNamespace.toString(), BSONObj());
    }

    /**
     * Returns the number of the documents for the collection 'nss' in the config.sampledQueries
     * collection.
     */
    int getSampledQueryDocumentsCount(const NamespaceString& nss) {
        return _getConfigDocumentsCount(NamespaceString::kConfigSampledQueriesNamespace, nss);
    }

    /*
     * Asserts that there is a sampled read query document with the given sample id and that it has
     * the given fields.
     */
    void assertSampledReadQueryDocument(const UUID& sampleId,
                                        const NamespaceString& nss,
                                        SampledReadCommandNameEnum cmdName,
                                        const BSONObj& filter,
                                        const BSONObj& collation) {
        auto doc = _getConfigDocument(NamespaceString::kConfigSampledQueriesNamespace, sampleId);
        auto parsedQueryDoc =
            SampledReadQueryDocument::parse(IDLParserContext("QueryAnalysisWriterTest"), doc);

        ASSERT_EQ(parsedQueryDoc.getNs(), nss);
        ASSERT_EQ(parsedQueryDoc.getCollectionUuid(), getCollectionUUID(nss));
        ASSERT_EQ(parsedQueryDoc.getSampleId(), sampleId);
        ASSERT(parsedQueryDoc.getCmdName() == cmdName);
        auto parsedCmd = SampledReadCommand::parse(IDLParserContext("QueryAnalysisWriterTest"),
                                                   parsedQueryDoc.getCmd());
        ASSERT_BSONOBJ_EQ(parsedCmd.getFilter(), filter);
        ASSERT_BSONOBJ_EQ(parsedCmd.getCollation(), collation);
    }

    const NamespaceString nss0{"testDb", "testColl0"};
    const NamespaceString nss1{"testDb", "testColl1"};

    // Test with both empty and non-empty filter and collation to verify that the
    // QueryAnalysisWriter doesn't require filter or collation to be non-empty.
    const BSONObj emptyFilter{};
    const BSONObj emptyCollation{};

private:
    /**
     * Returns the number of the documents for the collection 'collNss' in the config collection
     * 'configNss'.
     */
    int _getConfigDocumentsCount(const NamespaceString& configNss,
                                 const NamespaceString& collNss) const {
        DBDirectClient client(operationContext());
        return client.count(configNss, BSON("ns" << collNss.toString()));
    }

    /**
     * Returns the document with the given _id in the config collection 'configNss'.
     */
    BSONObj _getConfigDocument(const NamespaceString configNss, const UUID& id) const {
        DBDirectClient client(operationContext());

        FindCommandRequest findRequest{configNss};
        findRequest.setFilter(BSON("_id" << id));
        auto cursor = client.find(std::move(findRequest));
        ASSERT(cursor->more());
        return cursor->next();
    }

    RAIIServerParameterControllerForTest _featureFlagController{"featureFlagAnalyzeShardKey", true};
    FailPointEnableBlock _fp{"disableQueryAnalysisWriter"};
};

DEATH_TEST_F(QueryAnalysisWriterTest, CannotGetIfFeatureFlagNotEnabled, "invariant") {
    RAIIServerParameterControllerForTest _featureFlagController{"featureFlagAnalyzeShardKey",
                                                                false};
    QueryAnalysisWriter::get(operationContext());
}

DEATH_TEST_F(QueryAnalysisWriterTest, CannotGetOnConfigServer, "invariant") {
    serverGlobalParams.clusterRole = ClusterRole::ConfigServer;
    QueryAnalysisWriter::get(operationContext());
}

DEATH_TEST_F(QueryAnalysisWriterTest, CannotGetOnNonShardServer, "invariant") {
    serverGlobalParams.clusterRole = ClusterRole::None;
    QueryAnalysisWriter::get(operationContext());
}

TEST_F(QueryAnalysisWriterTest, NoQueries) {
    auto& writer = QueryAnalysisWriter::get(operationContext());
    writer.flushQueriesForTest(operationContext());
}

TEST_F(QueryAnalysisWriterTest, FindQuery) {
    auto& writer = QueryAnalysisWriter::get(operationContext());

    auto testFindCmdCommon = [&](const BSONObj& filter, const BSONObj& collation) {
        auto sampleId = UUID::gen();

        writer.addFindQuery(sampleId, nss0, filter, collation).get();
        ASSERT_EQ(writer.getQueriesCountForTest(), 1);
        writer.flushQueriesForTest(operationContext());
        ASSERT_EQ(writer.getQueriesCountForTest(), 0);

        ASSERT_EQ(getSampledQueryDocumentsCount(nss0), 1);
        assertSampledReadQueryDocument(
            sampleId, nss0, SampledReadCommandNameEnum::kFind, filter, collation);

        deleteSampledQueryDocuments();
    };

    testFindCmdCommon(makeNonEmptyFilter(), makeNonEmptyCollation());
    testFindCmdCommon(makeNonEmptyFilter(), emptyCollation);
    testFindCmdCommon(emptyFilter, makeNonEmptyCollation());
    testFindCmdCommon(emptyFilter, emptyCollation);
}

TEST_F(QueryAnalysisWriterTest, CountQuery) {
    auto& writer = QueryAnalysisWriter::get(operationContext());

    auto testCountCmdCommon = [&](const BSONObj& filter, const BSONObj& collation) {
        auto sampleId = UUID::gen();

        writer.addCountQuery(sampleId, nss0, filter, collation).get();
        ASSERT_EQ(writer.getQueriesCountForTest(), 1);
        writer.flushQueriesForTest(operationContext());
        ASSERT_EQ(writer.getQueriesCountForTest(), 0);

        ASSERT_EQ(getSampledQueryDocumentsCount(nss0), 1);
        assertSampledReadQueryDocument(
            sampleId, nss0, SampledReadCommandNameEnum::kCount, filter, collation);

        deleteSampledQueryDocuments();
    };

    testCountCmdCommon(makeNonEmptyFilter(), makeNonEmptyCollation());
    testCountCmdCommon(makeNonEmptyFilter(), emptyCollation);
    testCountCmdCommon(emptyFilter, makeNonEmptyCollation());
    testCountCmdCommon(emptyFilter, emptyCollation);
}

TEST_F(QueryAnalysisWriterTest, DistinctQuery) {
    auto& writer = QueryAnalysisWriter::get(operationContext());

    auto testDistinctCmdCommon = [&](const BSONObj& filter, const BSONObj& collation) {
        auto sampleId = UUID::gen();

        writer.addDistinctQuery(sampleId, nss0, filter, collation).get();
        ASSERT_EQ(writer.getQueriesCountForTest(), 1);
        writer.flushQueriesForTest(operationContext());
        ASSERT_EQ(writer.getQueriesCountForTest(), 0);

        ASSERT_EQ(getSampledQueryDocumentsCount(nss0), 1);
        assertSampledReadQueryDocument(
            sampleId, nss0, SampledReadCommandNameEnum::kDistinct, filter, collation);

        deleteSampledQueryDocuments();
    };

    testDistinctCmdCommon(makeNonEmptyFilter(), makeNonEmptyCollation());
    testDistinctCmdCommon(makeNonEmptyFilter(), emptyCollation);
    testDistinctCmdCommon(emptyFilter, makeNonEmptyCollation());
    testDistinctCmdCommon(emptyFilter, emptyCollation);
}

TEST_F(QueryAnalysisWriterTest, AggregateQuery) {
    auto& writer = QueryAnalysisWriter::get(operationContext());

    auto testAggregateCmdCommon = [&](const BSONObj& filter, const BSONObj& collation) {
        auto sampleId = UUID::gen();

        writer.addAggregateQuery(sampleId, nss0, filter, collation).get();
        ASSERT_EQ(writer.getQueriesCountForTest(), 1);
        writer.flushQueriesForTest(operationContext());
        ASSERT_EQ(writer.getQueriesCountForTest(), 0);

        ASSERT_EQ(getSampledQueryDocumentsCount(nss0), 1);
        assertSampledReadQueryDocument(
            sampleId, nss0, SampledReadCommandNameEnum::kAggregate, filter, collation);

        deleteSampledQueryDocuments();
    };

    testAggregateCmdCommon(makeNonEmptyFilter(), makeNonEmptyCollation());
    testAggregateCmdCommon(makeNonEmptyFilter(), emptyCollation);
    testAggregateCmdCommon(emptyFilter, makeNonEmptyCollation());
    testAggregateCmdCommon(emptyFilter, emptyCollation);
}

TEST_F(QueryAnalysisWriterTest, DuplicateQueries) {
    auto& writer = QueryAnalysisWriter::get(operationContext());

    auto findSampleId = UUID::gen();
    auto originalFindFilter = makeNonEmptyFilter();
    auto originalFindCollation = makeNonEmptyCollation();

    auto distinctSampleId = UUID::gen();
    auto originalDistinctFilter = makeNonEmptyFilter();
    auto originalDistinctCollation = makeNonEmptyCollation();

    auto countSampleId = UUID::gen();
    auto originalCountFilter = makeNonEmptyFilter();
    auto originalCountCollation = makeNonEmptyCollation();

    writer.addFindQuery(findSampleId, nss0, originalFindFilter, originalFindCollation).get();
    ASSERT_EQ(writer.getQueriesCountForTest(), 1);
    writer.flushQueriesForTest(operationContext());
    ASSERT_EQ(writer.getQueriesCountForTest(), 0);

    ASSERT_EQ(getSampledQueryDocumentsCount(nss0), 1);
    assertSampledReadQueryDocument(findSampleId,
                                   nss0,
                                   SampledReadCommandNameEnum::kFind,
                                   originalFindFilter,
                                   originalFindCollation);

    writer
        .addDistinctQuery(distinctSampleId, nss0, originalDistinctFilter, originalDistinctCollation)
        .get();
    writer.addFindQuery(findSampleId, nss0, originalFindFilter, originalFindCollation)
        .get();  // This is a duplicate.
    writer.addCountQuery(countSampleId, nss0, originalCountFilter, originalCountCollation).get();
    ASSERT_EQ(writer.getQueriesCountForTest(), 3);
    writer.flushQueriesForTest(operationContext());
    ASSERT_EQ(writer.getQueriesCountForTest(), 0);

    ASSERT_EQ(getSampledQueryDocumentsCount(nss0), 3);
    assertSampledReadQueryDocument(distinctSampleId,
                                   nss0,
                                   SampledReadCommandNameEnum::kDistinct,
                                   originalDistinctFilter,
                                   originalDistinctCollation);
    assertSampledReadQueryDocument(findSampleId,
                                   nss0,
                                   SampledReadCommandNameEnum::kFind,
                                   originalFindFilter,
                                   originalFindCollation);
    assertSampledReadQueryDocument(countSampleId,
                                   nss0,
                                   SampledReadCommandNameEnum::kCount,
                                   originalCountFilter,
                                   originalCountCollation);
}

TEST_F(QueryAnalysisWriterTest, QueriesMultipleBatches_MaxBatchSize) {
    auto& writer = QueryAnalysisWriter::get(operationContext());

    RAIIServerParameterControllerForTest maxBatchSize{"queryAnalysisWriterMaxBatchSize", 2};
    auto numQueries = 5;

    std::vector<std::tuple<UUID, BSONObj, BSONObj>> expectedSampledCmds;
    for (auto i = 0; i < numQueries; i++) {
        auto sampleId = UUID::gen();
        auto filter = makeNonEmptyFilter();
        auto collation = makeNonEmptyCollation();
        writer.addAggregateQuery(sampleId, nss0, filter, collation).get();
        expectedSampledCmds.push_back({sampleId, filter, collation});
    }
    ASSERT_EQ(writer.getQueriesCountForTest(), numQueries);
    writer.flushQueriesForTest(operationContext());
    ASSERT_EQ(writer.getQueriesCountForTest(), 0);

    ASSERT_EQ(getSampledQueryDocumentsCount(nss0), numQueries);
    for (const auto& [sampleId, filter, collation] : expectedSampledCmds) {
        assertSampledReadQueryDocument(
            sampleId, nss0, SampledReadCommandNameEnum::kAggregate, filter, collation);
    }
}

TEST_F(QueryAnalysisWriterTest, QueriesMultipleBatches_MaxBSONObjSize) {
    auto& writer = QueryAnalysisWriter::get(operationContext());

    auto numQueries = 3;
    std::vector<std::tuple<UUID, BSONObj, BSONObj>> expectedSampledCmds;
    for (auto i = 0; i < numQueries; i++) {
        auto sampleId = UUID::gen();
        auto filter = BSON(std::string(BSONObjMaxUserSize / 2, 'a') << 1);
        auto collation = makeNonEmptyCollation();
        writer.addAggregateQuery(sampleId, nss0, filter, collation).get();
        expectedSampledCmds.push_back({sampleId, filter, collation});
    }
    ASSERT_EQ(writer.getQueriesCountForTest(), numQueries);
    writer.flushQueriesForTest(operationContext());
    ASSERT_EQ(writer.getQueriesCountForTest(), 0);

    ASSERT_EQ(getSampledQueryDocumentsCount(nss0), numQueries);
    for (const auto& [sampleId, filter, collation] : expectedSampledCmds) {
        assertSampledReadQueryDocument(
            sampleId, nss0, SampledReadCommandNameEnum::kAggregate, filter, collation);
    }
}

TEST_F(QueryAnalysisWriterTest, FlushAfterAddReadIfExceedsSizeLimit) {
    auto& writer = QueryAnalysisWriter::get(operationContext());

    auto maxMemoryUsageBytes = 1024;
    RAIIServerParameterControllerForTest maxMemoryBytes{"queryAnalysisWriterMaxMemoryUsageBytes",
                                                        maxMemoryUsageBytes};

    auto sampleId0 = UUID::gen();
    auto filter0 = BSON(std::string(maxMemoryUsageBytes / 2, 'a') << 1);
    auto collation0 = makeNonEmptyCollation();

    auto sampleId1 = UUID::gen();
    auto filter1 = BSON(std::string(maxMemoryUsageBytes / 2, 'b') << 1);
    auto collation1 = makeNonEmptyCollation();

    writer.addFindQuery(sampleId0, nss0, filter0, collation0).get();
    ASSERT_EQ(writer.getQueriesCountForTest(), 1);
    // Adding the next query causes the size to exceed the limit.
    writer.addAggregateQuery(sampleId1, nss1, filter1, collation1).get();
    ASSERT_EQ(writer.getQueriesCountForTest(), 0);

    ASSERT_EQ(getSampledQueryDocumentsCount(nss0), 1);
    assertSampledReadQueryDocument(
        sampleId0, nss0, SampledReadCommandNameEnum::kFind, filter0, collation0);
    ASSERT_EQ(getSampledQueryDocumentsCount(nss1), 1);
    assertSampledReadQueryDocument(
        sampleId1, nss1, SampledReadCommandNameEnum::kAggregate, filter1, collation1);
}

TEST_F(QueryAnalysisWriterTest, AddQueriesBackAfterWriteError) {
    auto& writer = QueryAnalysisWriter::get(operationContext());

    auto originalFilter = makeNonEmptyFilter();
    auto originalCollation = makeNonEmptyCollation();
    auto numQueries = 8;

    std::vector<UUID> sampleIds0;
    for (auto i = 0; i < numQueries; i++) {
        sampleIds0.push_back(UUID::gen());
        writer.addFindQuery(sampleIds0[i], nss0, originalFilter, originalCollation).get();
    }
    ASSERT_EQ(writer.getQueriesCountForTest(), numQueries);

    // Force the documents to get inserted in three batches of size 3, 3 and 2, respectively.
    RAIIServerParameterControllerForTest maxBatchSize{"queryAnalysisWriterMaxBatchSize", 3};

    // Hang after inserting the documents in the first batch.
    auto hangFp = globalFailPointRegistry().find("hangAfterCollectionInserts");
    auto hangTimesEntered = hangFp->setMode(FailPoint::alwaysOn, 0);

    auto future = stdx::async(stdx::launch::async, [&] {
        ThreadClient tc(getServiceContext());
        auto opCtx = makeOperationContext();
        writer.flushQueriesForTest(opCtx.get());
    });

    hangFp->waitForTimesEntered(hangTimesEntered + 1);
    // Force the second batch to fail so that it falls back to inserting one document at a time in
    // order, and then force the first and second document in the batch to fail.
    auto failFp = globalFailPointRegistry().find("failCollectionInserts");
    failFp->setMode(FailPoint::nTimes, 3);
    hangFp->setMode(FailPoint::off, 0);

    future.get();
    // Verify that all the documents other than the ones in the first batch got added back to the
    // buffer after the error. That is, the error caused the last document in the second batch to
    // get added to buffer also although it was successfully inserted since the writer did not have
    // a way to tell if the error caused the entire command to fail early.
    ASSERT_EQ(writer.getQueriesCountForTest(), 5);
    ASSERT_EQ(getSampledQueryDocumentsCount(nss0), 4);

    // Flush that remaining documents. If the documents were not added back correctly, some
    // documents would be missing and the checks below would fail.
    writer.flushQueriesForTest(operationContext());
    ASSERT_EQ(writer.getQueriesCountForTest(), 0);

    ASSERT_EQ(getSampledQueryDocumentsCount(nss0), numQueries);
    for (const auto& sampleId : sampleIds0) {
        assertSampledReadQueryDocument(
            sampleId, nss0, SampledReadCommandNameEnum::kFind, originalFilter, originalCollation);
    }
}

TEST_F(QueryAnalysisWriterTest, RemoveDuplicatesFromBufferAfterWriteError) {
    auto& writer = QueryAnalysisWriter::get(operationContext());

    auto originalFilter = makeNonEmptyFilter();
    auto originalCollation = makeNonEmptyCollation();

    auto numQueries0 = 3;

    std::vector<UUID> sampleIds0;
    for (auto i = 0; i < numQueries0; i++) {
        sampleIds0.push_back(UUID::gen());
        writer.addFindQuery(sampleIds0[i], nss0, originalFilter, originalCollation).get();
    }
    ASSERT_EQ(writer.getQueriesCountForTest(), numQueries0);
    writer.flushQueriesForTest(operationContext());
    ASSERT_EQ(writer.getQueriesCountForTest(), 0);

    ASSERT_EQ(getSampledQueryDocumentsCount(nss0), numQueries0);
    for (const auto& sampleId : sampleIds0) {
        assertSampledReadQueryDocument(
            sampleId, nss0, SampledReadCommandNameEnum::kFind, originalFilter, originalCollation);
    }

    auto numQueries1 = 5;

    std::vector<UUID> sampleIds1;
    for (auto i = 0; i < numQueries1; i++) {
        sampleIds1.push_back(UUID::gen());
        writer.addFindQuery(sampleIds1[i], nss1, originalFilter, originalCollation).get();
        // This is a duplicate.
        if (i < numQueries0) {
            writer.addFindQuery(sampleIds0[i], nss0, originalFilter, originalCollation).get();
        }
    }
    ASSERT_EQ(writer.getQueriesCountForTest(), numQueries0 + numQueries1);

    // Force the batch to fail so that it falls back to inserting one document at a time in order.
    auto failFp = globalFailPointRegistry().find("failCollectionInserts");
    failFp->setMode(FailPoint::nTimes, 1);

    // Hang after inserting the first non-duplicate document.
    auto hangFp = globalFailPointRegistry().find("hangAfterCollectionInserts");
    auto hangTimesEntered = hangFp->setMode(FailPoint::alwaysOn, 0);

    auto future = stdx::async(stdx::launch::async, [&] {
        ThreadClient tc(getServiceContext());
        auto opCtx = makeOperationContext();
        writer.flushQueriesForTest(opCtx.get());
    });

    hangFp->waitForTimesEntered(hangTimesEntered + 1);
    // Force the next non-duplicate document to fail to insert.
    failFp->setMode(FailPoint::nTimes, 1);
    hangFp->setMode(FailPoint::off, 0);

    future.get();
    // Verify that the duplicate documents did not get added back to the buffer after the error.
    ASSERT_EQ(writer.getQueriesCountForTest(), numQueries1);
    ASSERT_EQ(getSampledQueryDocumentsCount(nss1), numQueries1 - 1);

    // Flush that remaining documents. If the documents were not added back correctly, the document
    // that previously failed to insert would be missing and the checks below would fail.
    failFp->setMode(FailPoint::off, 0);
    writer.flushQueriesForTest(operationContext());
    ASSERT_EQ(writer.getQueriesCountForTest(), 0);

    ASSERT_EQ(getSampledQueryDocumentsCount(nss1), numQueries1);
    for (const auto& sampleId : sampleIds1) {
        assertSampledReadQueryDocument(
            sampleId, nss1, SampledReadCommandNameEnum::kFind, originalFilter, originalCollation);
    }
}

}  // namespace
}  // namespace analyze_shard_key
}  // namespace mongo
