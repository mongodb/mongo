/**
 *    Copyright (C) 2023-present MongoDB, Inc.
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

#include "mongo/db/pipeline/document_source_internal_all_collection_stats.h"

#include "mongo/db/pipeline/aggregation_context_fixture.h"
#include "mongo/db/pipeline/document_source_coll_stats.h"
#include "mongo/db/pipeline/document_source_coll_stats_gen.h"
#include "mongo/db/pipeline/document_source_internal_all_collection_stats_gen.h"
#include "mongo/db/pipeline/storage_stats_spec_gen.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/intrusive_counter.h"

#include <cstdint>

#include <boost/cstdint.hpp>
#include <boost/move/utility_core.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {
namespace {
using DocumentSourceInternalAllCollectionStatsTest = AggregationContextFixture;

auto representativeShape(const DocumentSourceInternalAllCollectionStats& allCollStatsStage) {
    SerializationOptions opts{.literalPolicy =
                                  LiteralSerializationPolicy::kToRepresentativeParseableValue};
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
