// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/pipeline/document_source_coll_stats.h"

#include "mongo/db/pipeline/aggregation_context_fixture.h"
#include "mongo/db/pipeline/document_source_coll_stats_gen.h"
#include "mongo/db/pipeline/storage_stats_spec_gen.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/intrusive_counter.h"

#include <cstdint>

namespace mongo {
namespace {
using DocumentSourceCollStatsTest = AggregationContextFixture;

auto representativeShape(const DocumentSourceCollStats& collStatsStage) {
    query_shape::SerializationOptions opts{
        .literalPolicy = query_shape::LiteralSerializationPolicy::kToRepresentativeParseableValue};
    return collStatsStage.serialize(opts).getDocument().toBson();
}

TEST_F(DocumentSourceCollStatsTest, QueryShape) {
    auto spec = DocumentSourceCollStatsSpec();

    auto stage = make_intrusive<DocumentSourceCollStats>(getExpCtx(), spec);
    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({"$collStats":{}})",
        redact(*stage));
    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({"$collStats":{}})",
        representativeShape(*stage));

    spec.setCount(BSONObj());
    spec.setQueryExecStats(BSONObj());
    stage = make_intrusive<DocumentSourceCollStats>(getExpCtx(), spec);
    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({"$collStats":{"count":{},"queryExecStats":{}}})",
        redact(*stage));
    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({"$collStats":{"count":{},"queryExecStats":{}}})",
        representativeShape(*stage));

    auto latencyStats = LatencyStatsSpec();
    latencyStats.setHistograms(true);
    spec.setLatencyStats(latencyStats);
    stage = make_intrusive<DocumentSourceCollStats>(getExpCtx(), spec);
    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({
            "$collStats": {
                "latencyStats": {
                    "histograms": true
                },
                "count": {},
                "queryExecStats": {}
            }
        })",
        redact(*stage));

    auto storageStats = StorageStatsSpec();
    storageStats.setScale(2);
    storageStats.setVerbose(true);
    spec.setStorageStats(storageStats);
    stage = make_intrusive<DocumentSourceCollStats>(getExpCtx(), spec);
    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({
            "$collStats": {
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
        })",
        redact(*stage));

    storageStats.setWaitForLock(false);
    storageStats.setNumericOnly(false);
    spec.setStorageStats(storageStats);
    stage = make_intrusive<DocumentSourceCollStats>(getExpCtx(), spec);
    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({
            "$collStats": {
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
        })",
        redact(*stage));
}
}  // namespace
}  // namespace mongo
