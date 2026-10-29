// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/exec/agg/writer_stage.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/merge_processor.h"
#include "mongo/s/write_ops/batched_command_request.h"
#include "mongo/util/modules.h"

#include <string_view>
#include <utility>

#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo::exec::agg {


/**
 * This class handles the execution part of the single document transformation aggregation stage
 * and is part of the execution pipeline. Its construction is based on
 * DocumentSourceSingleDocumentTransformation, which handles the optimization part.
 *
 * TODO SERVER-112777: Remove 'atlas_streams' dependency on reference to 'BatchObject' type.
 */
class [[MONGO_MOD_NEEDS_REPLACEMENT]] MergeStage final
    : public WriterStage<MongoProcessInterface::BatchObject> {
public:
    MergeStage(std::string_view stageName,
               const boost::intrusive_ptr<ExpressionContext>& pExpCtx,
               NamespaceString outputNs,
               std::shared_ptr<std::set<FieldPath>> mergeOnFields,
               bool mergeOnFieldsIncludesId,
               std::shared_ptr<MergeProcessor> mergeProcessor)
        : WriterStage<MongoProcessInterface::BatchObject>(stageName, pExpCtx, std::move(outputNs)),
          _mergeOnFields(std::move(mergeOnFields)),
          _mergeOnFieldsIncludesId(mergeOnFieldsIncludesId),
          _mergeProcessor(std::move(mergeProcessor)) {};

    void initialize() override {
        // This implies that the stage will soon start to write, so it's safe to verify the target
        // collection placement version. This is done here instead of parse time since it requires
        // that locks are not held.
        const auto& collectionPlacementVersion = _mergeProcessor->getCollectionPlacementVersion();
        if (!pExpCtx->getInRouter() && collectionPlacementVersion) {
            // If a router has sent us a target placement version, we need to be sure we are
            // prepared to act as a router which is at least as recent as that router.
            pExpCtx->getMongoProcessInterface()->checkRoutingInfoEpochOrThrow(
                pExpCtx, _outputNs, *collectionPlacementVersion);
        }
    }

    BatchedCommandRequest makeBatchedWriteRequest() const override;
    std::pair<BatchObject, int> makeBatchObject(Document doc) const override;

    bool shouldFlush(size_t batchSize) const final;

private:
    void flush(BatchedCommandRequest bcr, BatchedObjects batch) override;
    void waitWhileFailPointEnabled() override;

    // Holds the fields used for uniquely identifying documents. There must exist a unique index
    // with this key pattern. Default is "_id" for unsharded collections, and "_id" plus the shard
    // key for sharded collections.
    std::shared_ptr<std::set<FieldPath>> _mergeOnFields;

    // True if '_mergeOnFields' contains the _id. We store this as a separate boolean to avoid
    // repeated lookups into the set.
    bool _mergeOnFieldsIncludesId;

    std::shared_ptr<MergeProcessor> _mergeProcessor;
};

}  // namespace mongo::exec::agg
