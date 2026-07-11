// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonobj.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/shard_filterer.h"
#include "mongo/db/keypattern.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/pipeline/aggregate_command_gen.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/pipeline.h"
#include "mongo/db/pipeline/process_interface/stub_mongo_process_interface.h"
#include "mongo/db/pipeline/sharded_agg_helpers_targeting_policy.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/modules.h"
#include "mongo/util/uuid.h"

#include <cstddef>
#include <deque>
#include <memory>
#include <utility>
#include <vector>

#include <boost/intrusive_ptr.hpp>
#include <boost/none.hpp>
#include <boost/optional.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {

class StubShardFilterer : public ShardFilterer {
public:
    std::unique_ptr<ShardFilterer> clone() const override {
        MONGO_UNREACHABLE;
    }

    DocumentBelongsResult documentBelongsToMe(const BSONObj& doc) const override {
        MONGO_UNREACHABLE;
    }

    bool isCollectionSharded() const override {
        MONGO_UNREACHABLE;
    }

    const KeyPattern& getKeyPattern() const override {
        MONGO_UNREACHABLE;
    }

    bool keyBelongsToMe(const BSONObj& shardKey) const override {
        MONGO_UNREACHABLE;
    }

    size_t getApproximateSize() const override {
        MONGO_UNREACHABLE;
    }
};

/**
 * A mock MongoProcessInterface which allows mocking a foreign pipeline.
 */
class StubLookupSingleDocumentProcessInterface final : public StubMongoProcessInterface {
public:
    StubLookupSingleDocumentProcessInterface(std::deque<DocumentSource::GetNextResult> mockResults)
        : _mockResults(std::move(mockResults)) {}

    std::unique_ptr<Pipeline> finalizeAndMaybePreparePipelineForExecution(
        const boost::intrusive_ptr<ExpressionContext>& expCtx,
        std::unique_ptr<Pipeline> pipeline,
        bool attachCursorAfterOptimizing,
        std::function<void(Pipeline* pipeline)> optimizePipeline = nullptr,
        ShardTargetingPolicy shardTargetingPolicy = ShardTargetingPolicy::kAllowed,
        boost::optional<BSONObj> readConcern = boost::none,
        bool shouldUseCollectionDefaultCollator = false) final;

    std::unique_ptr<Pipeline> preparePipelineForExecution(
        std::unique_ptr<Pipeline> pipeline,
        ShardTargetingPolicy shardTargetingPolicy = ShardTargetingPolicy::kAllowed,
        boost::optional<BSONObj> readConcern = boost::none) final;

    std::unique_ptr<Pipeline> preparePipelineForExecution(
        const boost::intrusive_ptr<ExpressionContext>& expCtx,
        const AggregateCommandRequest& aggRequest,
        std::unique_ptr<Pipeline> pipeline,
        boost::optional<BSONObj> shardCursorsSortSpec = boost::none,
        ShardTargetingPolicy shardTargetingPolicy = ShardTargetingPolicy::kAllowed,
        boost::optional<BSONObj> readConcern = boost::none,
        bool shouldUseCollectionDefaultCollator = false) final;

    std::unique_ptr<Pipeline> attachCursorSourceToPipelineForLocalRead(
        std::unique_ptr<Pipeline> pipeline,
        boost::optional<const AggregateCommandRequest&> aggRequest = boost::none,
        bool shouldUseCollectionDefaultCollator = false) final;

    std::unique_ptr<Pipeline> finalizeAndAttachCursorToPipelineForLocalRead(
        const boost::intrusive_ptr<ExpressionContext>& expCtx,
        std::unique_ptr<Pipeline> pipeline,
        bool attachCursorAfterOptimizing,
        std::function<void(Pipeline* pipeline)> optimizePipeline = nullptr,
        bool shouldUseCollectionDefaultCollator = false,
        boost::optional<const AggregateCommandRequest&> aggRequest = boost::none) final;

    boost::optional<Document> lookupSingleDocument(
        const boost::intrusive_ptr<ExpressionContext>& expCtx,
        const NamespaceString& nss,
        boost::optional<UUID> collectionUUID,
        const Document& documentKey,
        boost::optional<BSONObj> readConcern) override;

private:
    std::deque<DocumentSource::GetNextResult> _mockResults;
};
}  // namespace mongo
