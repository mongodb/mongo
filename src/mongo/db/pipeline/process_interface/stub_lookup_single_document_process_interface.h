/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

    std::unique_ptr<Pipeline> preparePipelineForExecution(
        Pipeline* ownedPipeline,
        ShardTargetingPolicy shardTargetingPolicy = ShardTargetingPolicy::kAllowed,
        boost::optional<BSONObj> readConcern = boost::none) final;

    std::unique_ptr<Pipeline> preparePipelineForExecution(
        const boost::intrusive_ptr<ExpressionContext>& expCtx,
        const AggregateCommandRequest& aggRequest,
        Pipeline* pipeline,
        boost::optional<BSONObj> shardCursorsSortSpec = boost::none,
        ShardTargetingPolicy shardTargetingPolicy = ShardTargetingPolicy::kAllowed,
        boost::optional<BSONObj> readConcern = boost::none,
        bool shouldUseCollectionDefaultCollator = false) final;

    std::unique_ptr<Pipeline> attachCursorSourceToPipelineForLocalRead(
        Pipeline* ownedPipeline,
        boost::optional<const AggregateCommandRequest&> aggRequest = boost::none,
        bool shouldUseCollectionDefaultCollator = false,
        ExecShardFilterPolicy shardFilterPolicy = AutomaticShardFiltering{}) final;

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
