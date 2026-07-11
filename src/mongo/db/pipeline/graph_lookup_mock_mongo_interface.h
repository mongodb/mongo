// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/bson/json.h"
#include "mongo/bson/unordered_fields_bsonobj_comparator.h"
#include "mongo/db/database_name.h"
#include "mongo/db/exec/agg/document_source_to_stage_registry.h"
#include "mongo/db/exec/agg/mock_stage.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/document_value/document_value_test_util.h"
#include "mongo/db/pipeline/aggregate_command_gen.h"
#include "mongo/db/pipeline/document_source_graph_lookup.h"
#include "mongo/db/pipeline/document_source_mock.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/pipeline/process_interface/standalone_process_interface.h"
#include "mongo/db/pipeline/serverless_aggregation_context_fixture.h"
#include "mongo/db/pipeline/sharded_agg_helpers_targeting_policy.h"
#include "mongo/db/service_context_d_test_fixture.h"
#include "mongo/db/stats/counters.h"
#include "mongo/db/tenant_id.h"
#include "mongo/platform/atomic.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/modules.h"
#include "mongo/util/str.h"

#include <algorithm>
#include <deque>
#include <initializer_list>

#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {
namespace test {
/**
 * A MongoProcessInterface use for testing that supports making pipelines with an initial
 * DocumentSourceMock source.
 */
class GraphLookUpMockMongoInterface final : public StandaloneProcessInterface {
public:
    GraphLookUpMockMongoInterface(std::deque<DocumentSource::GetNextResult> results);

    std::unique_ptr<Pipeline> finalizeAndMaybePreparePipelineForExecution(
        const boost::intrusive_ptr<ExpressionContext>& expCtx,
        std::unique_ptr<Pipeline> pipeline,
        bool attachCursorAfterOptimizing,
        std::function<void(Pipeline* pipeline)> optimizePipeline,
        ShardTargetingPolicy shardTargetingPolicy,
        boost::optional<BSONObj> readConcern,
        bool shouldUseCollectionDefaultCollator) final;

    std::unique_ptr<Pipeline> preparePipelineForExecution(
        std::unique_ptr<Pipeline> pipeline,
        ShardTargetingPolicy shardTargetingPolicy = ShardTargetingPolicy::kAllowed,
        boost::optional<BSONObj> readConcern = boost::none) final;

    std::unique_ptr<mongo::Pipeline> preparePipelineForExecution(
        const boost::intrusive_ptr<ExpressionContext>& expCtx,
        const AggregateCommandRequest& aggRequest,
        std::unique_ptr<Pipeline> pipeline,
        boost::optional<BSONObj> shardCursorsSortSpec = boost::none,
        ShardTargetingPolicy shardTargetingPolicy = ShardTargetingPolicy::kAllowed,
        boost::optional<BSONObj> readConcern = boost::none,
        bool shouldUseCollectionDefaultCollator = false) final;

    /**
     * Causes the next 'count' calls to finalizeAndMaybePreparePipelineForExecution to throw
     * CommandOnShardedViewNotSupportedOnMongod, simulating a foreign view that resolves to a
     * sharded view (possibly repeatedly, as under concurrent view remapping).
     */
    void setShardedViewResolutionThrowCount(int count) {
        _shardedViewThrowCount = count;
    }

private:
    std::deque<DocumentSource::GetNextResult> _results;
    int _shardedViewThrowCount = 0;
};

}  // namespace test
}  // namespace mongo
