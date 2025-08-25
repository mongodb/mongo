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

#pragma once

#include "mongo/base/string_data.h"
#include "mongo/db/exec/agg/writer_stage.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/merge_processor.h"
#include "mongo/s/write_ops/batched_command_request.h"

#include <utility>

#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo::exec::agg {


/**
 * This class handles the execution part of the single document transformation aggregation stage
 * and is part of the execution pipeline. Its construction is based on
 * DocumentSourceSingleDocumentTransformation, which handles the optimization part.
 */
class MergeStage final : public WriterStage<MongoProcessInterface::BatchObject> {
public:
    MergeStage(StringData stageName,
               const boost::intrusive_ptr<ExpressionContext>& pExpCtx,
               NamespaceString outputNs,
               std::shared_ptr<std::set<FieldPath>> mergeOnFields,
               bool mergeOnFieldsIncludesId,
               std::shared_ptr<MergeProcessor> mergeProcessor)
        : WriterStage<MongoProcessInterface::BatchObject>(
              stageName.data(), pExpCtx, std::move(outputNs)),
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
