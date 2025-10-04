/**
 *    Copyright (C) 2019-present MongoDB, Inc.
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
#include "mongo/db/exec/agg/exec_pipeline.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/pipeline.h"
#include "mongo/db/update/update_executor.h"
#include "mongo/util/intrusive_counter.h"

#include <map>
#include <memory>
#include <string>
#include <utility>
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
