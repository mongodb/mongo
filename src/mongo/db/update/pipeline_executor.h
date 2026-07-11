// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonobj.h"
#include "mongo/db/exec/agg/exec_pipeline.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/pipeline.h"
#include "mongo/db/update/update_executor.h"
#include "mongo/util/intrusive_counter.h"
#include "mongo/util/modules.h"

#include <memory>
#include <vector>

#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {

/**
 * An UpdateExecutor representing a pipeline-style update.
 */
class PipelineExecutor : public UpdateExecutor {

public:
    /**
     * Initializes the node with an aggregation pipeline definition.
     */
    explicit PipelineExecutor(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                              const std::vector<BSONObj>& pipeline,
                              boost::optional<BSONObj> constants = boost::none);

    /**
     * Replaces the document that 'applyParams.element' belongs to with 'val'. If 'val' does not
     * contain an _id, the _id from the original document is preserved. 'applyParams.element' must
     * be the root of the document. Always returns a result stating that indexes are affected when
     * the replacement is not a noop.
     */
    ApplyResult applyUpdate(ApplyParams applyParams) const final;

    Value serialize() const final;
    Value serialize(const query_shape::SerializationOptions& opts) const;

    bool getCheckExistenceForDiffInsertOperations() const final {
        return _checkExistenceForDiffInsertOperations;
    }

private:
    // Sets to true if the pipeline contains '$_internalApplyOplogUpdate'.
    bool _checkExistenceForDiffInsertOperations = false;

    boost::intrusive_ptr<ExpressionContext> _expCtx;
    std::unique_ptr<Pipeline> _pipeline;
    std::unique_ptr<exec::agg::Pipeline> _execPipeline;
};

}  // namespace mongo
