// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/exec/single_doc_lookup/aggregation_single_document_lookup_executor.h"

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/single_doc_lookup/single_document_lookup_stats.h"
#include "mongo/db/exec/single_doc_lookup/single_document_lookup_stats_test_util.h"
#include "mongo/db/pipeline/aggregation_context_fixture.h"
#include "mongo/db/pipeline/process_interface/stub_mongo_process_interface.h"
#include "mongo/otel/metrics/metrics_test_util.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/uuid.h"

#include <memory>

#include <boost/optional/optional.hpp>

namespace mongo::exec::agg {
namespace {

using HandledStatus = SingleDocumentLookupExecutor::LookupResult::HandledStatus;
using otel::metrics::OtelMetricsCapturer;

/**
 * Captures the arguments lookupSingleDocument() is called with (notably the readConcern the
 * executor builds) and returns a preconfigured result, so we can assert the executor's
 * readConcern construction and found / not-found mapping without a routing environment.
 */
class MockLookupSingleDocumentProcessInterface : public StubMongoProcessInterface {
public:
    boost::optional<Document> lookupSingleDocument(const boost::intrusive_ptr<ExpressionContext>&,
                                                   const NamespaceString& nss,
                                                   boost::optional<UUID> collectionUUID,
                                                   const Document& documentKey,
                                                   boost::optional<BSONObj> readConcern) override {
        ++callCount;
        lastNss = nss;
        lastCollectionUUID = collectionUUID;
        lastDocumentKey = documentKey;
        lastReadConcern = std::move(readConcern);
        return _result;
    }

    boost::optional<Document> _result;

    int callCount = 0;
    NamespaceString lastNss;
    boost::optional<UUID> lastCollectionUUID;
    Document lastDocumentKey;
    boost::optional<BSONObj> lastReadConcern;
};

class AggregationSingleDocumentLookupExecutorTest : public AggregationContextFixture {
protected:
    std::shared_ptr<MockLookupSingleDocumentProcessInterface> installMock(
        boost::optional<Document> result) {
        auto mock = std::make_shared<MockLookupSingleDocumentProcessInterface>();
        mock->_result = std::move(result);
        getExpCtx()->setMongoProcessInterface(mock);
        return mock;
    }

    // The executor records into the process-global aggregation cell; tests that only care about the
    // returned LookupResult share this real cell since recording is a no-op side effect for them.
    AggregationSingleDocumentLookupExecutor makeExecutor() {
        return AggregationSingleDocumentLookupExecutor{
            exec::SingleDocumentLookupStatsRecorder::makeUpdateLookupAggregationRecorder()};
    }

    const NamespaceString nss = NamespaceString::createNamespaceString_forTest("testdb.testcoll");
    const UUID collectionUUID = UUID::gen();
    const Document documentKey = Document{{"_id", 7}};
};

TEST_F(AggregationSingleDocumentLookupExecutorTest, FoundDocumentReturnsKHandledFound) {
    auto mock = installMock(Document{{"_id", 7}, {"v", 1}});
    auto executor = makeExecutor();

    auto result =
        executor.performLookup(getExpCtx(), nss, collectionUUID, documentKey, Timestamp(100, 1));

    ASSERT(result.status == HandledStatus::kDocumentFound);
    ASSERT_BSONOBJ_EQ(result.document->toBson(), BSON("_id" << 7 << "v" << 1));
    ASSERT_EQ(mock->callCount, 1);
}

TEST_F(AggregationSingleDocumentLookupExecutorTest, AbsentDocumentReturnsKHandledNotFound) {
    auto mock = installMock(boost::none);
    auto executor = makeExecutor();

    auto result =
        executor.performLookup(getExpCtx(), nss, collectionUUID, documentKey, Timestamp(100, 1));

    ASSERT(result.status == HandledStatus::kDocumentNotFound);
    ASSERT_FALSE(result.document.has_value());
    ASSERT_EQ(mock->callCount, 1);
}

TEST_F(AggregationSingleDocumentLookupExecutorTest, BuildsMajorityReadConcernFromAfterClusterTime) {
    auto mock = installMock(boost::none);
    auto executor = makeExecutor();

    executor.performLookup(getExpCtx(), nss, collectionUUID, documentKey, Timestamp(100, 1));

    ASSERT(mock->lastReadConcern.has_value());
    ASSERT_BSONOBJ_EQ(*mock->lastReadConcern,
                      BSON("level" << "majority"
                                   << "afterClusterTime" << Timestamp(100, 1)));
}

TEST_F(AggregationSingleDocumentLookupExecutorTest, NoReadConcernWhenAfterClusterTimeAbsent) {
    auto mock = installMock(boost::none);
    auto executor = makeExecutor();

    executor.performLookup(getExpCtx(), nss, collectionUUID, documentKey, boost::none);

    ASSERT_FALSE(mock->lastReadConcern.has_value());
}

TEST_F(AggregationSingleDocumentLookupExecutorTest, ForwardsNssUuidAndDocumentKey) {
    auto mock = installMock(boost::none);
    auto executor = makeExecutor();

    executor.performLookup(getExpCtx(), nss, collectionUUID, documentKey, boost::none);

    ASSERT_EQ(mock->lastNss, nss);
    ASSERT(mock->lastCollectionUUID == boost::optional<UUID>(collectionUUID));
    ASSERT_BSONOBJ_EQ(mock->lastDocumentKey.toBson(), documentKey.toBson());
}

// A found lookup bumps the aggregation cell's 'found' counter and the latency histogram; an absent
// lookup bumps 'notFound' and the histogram. The aggregation executor never declines, so
// 'notHandled' is never touched here.
TEST_F(AggregationSingleDocumentLookupExecutorTest, RecordsFoundAndNotFoundIntoAggregationCell) {
    OtelMetricsCapturer capturer;
    if (!capturer.canReadMetrics()) {
        return;
    }

    const auto before = snapshotAggregationCell(capturer);

    auto executor = makeExecutor();
    installMock(Document{{"_id", 7}, {"v", 1}});
    executor.performLookup(getExpCtx(), nss, collectionUUID, documentKey, Timestamp(100, 1));
    installMock(boost::none);
    executor.performLookup(getExpCtx(), nss, collectionUUID, documentKey, Timestamp(100, 1));

    const auto after = snapshotAggregationCell(capturer);
    ASSERT_EQ(after.found, before.found + 1);
    ASSERT_EQ(after.notFound, before.notFound + 1);
    // The aggregation executor never declines.
    ASSERT_EQ(after.notHandled, before.notHandled);

    // Both the found and not-found lookups record a latency sample; wall-clock magnitude is not
    // asserted (the recorder unit test covers exact sums with synthetic durations).
    ASSERT_EQ(after.latencyCount, before.latencyCount + 2);
    ASSERT_GTE(after.latencySum, before.latencySum);
}

}  // namespace
}  // namespace mongo::exec::agg
