// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/pipeline/document_source_internal_all_collection_stats.h"

#include "mongo/db/pipeline/aggregation_context_fixture.h"
#include "mongo/db/pipeline/document_source_coll_stats.h"
#include "mongo/db/pipeline/document_source_coll_stats_gen.h"
#include "mongo/db/pipeline/document_source_internal_all_collection_stats_gen.h"
#include "mongo/db/pipeline/storage_stats_spec_gen.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/intrusive_counter.h"

#include <cstdint>


namespace mongo {
namespace {
using DocumentSourceInternalAllCollectionStatsTest = AggregationContextFixture;

auto representativeShape(const DocumentSourceInternalAllCollectionStats& allCollStatsStage) {
    query_shape::SerializationOptions opts{
        .literalPolicy = query_shape::LiteralSerializationPolicy::kToRepresentativeParseableValue};
    return allCollStatsStage.serialize(opts).getDocument().toBson();
}

TEST_F(DocumentSourceInternalAllCollectionStatsTest, QueryShape) {
    auto innerSpec = DocumentSourceCollStatsSpec();
    auto spec = DocumentSourceInternalAllCollectionStatsSpec();
    spec.setStats(innerSpec);

    auto stage = make_intrusive<DocumentSourceInternalAllCollectionStats>(getExpCtx(), spec);
    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({"$_internalAllCollectionStats":{"stats":{}}})",
        redact(*stage));

    spec.getStats()->setCount(BSONObj());
    spec.getStats()->setQueryExecStats(BSONObj());
    stage = make_intrusive<DocumentSourceInternalAllCollectionStats>(getExpCtx(), spec);
    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({"$_internalAllCollectionStats":{"stats":{"count":{},"queryExecStats":{}}}})",
        redact(*stage));

    auto latencyStats = LatencyStatsSpec();
    latencyStats.setHistograms(true);
    spec.getStats()->setLatencyStats(latencyStats);
    stage = make_intrusive<DocumentSourceInternalAllCollectionStats>(getExpCtx(), spec);
    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({
            "$_internalAllCollectionStats": {
                "stats": {
                    "latencyStats": {
                        "histograms": true
                    },
                    "count": {},
                    "queryExecStats": {}
                }
            }
        })",
        redact(*stage));

    auto storageStats = StorageStatsSpec();
    storageStats.setScale(2);
    storageStats.setVerbose(true);
    spec.getStats()->setStorageStats(storageStats);
    stage = make_intrusive<DocumentSourceInternalAllCollectionStats>(getExpCtx(), spec);
    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({
            "$_internalAllCollectionStats": {
                "stats": {
                    "latencyStats": {
                        "histograms": true
                    },
                    "storageStats": {
                        "scale": "?number",
                        "verbose": true,
                        "waitForLock": true,
                        "numericOnly": false
                    },
                    "count": {},
                    "queryExecStats": {}
                }
            }
        })",
        redact(*stage));

    storageStats.setWaitForLock(false);
    storageStats.setNumericOnly(false);
    spec.getStats()->setStorageStats(storageStats);
    stage = make_intrusive<DocumentSourceInternalAllCollectionStats>(getExpCtx(), spec);
    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({
            "$_internalAllCollectionStats": {
                "stats": {
                    "latencyStats": {
                        "histograms": true
                    },
                    "storageStats": {
                        "scale": "?number",
                        "verbose": true,
                        "waitForLock": false,
                        "numericOnly": false
                    },
                    "count": {},
                    "queryExecStats": {}
                }
            }
        })",
        redact(*stage));
}
}  // namespace
}  // namespace mongo
