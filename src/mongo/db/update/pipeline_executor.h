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

#include <map>
#include <string>
#include <utility>
#include <vector>

#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/pipeline.h"
#include "mongo/db/update/update_executor.h"
#include "mongo/stdx/memory.h"

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
                              const std::vector<BSONObj>& pipeline);

    /**
     * Replaces the document that 'applyParams.element' belongs to with 'val'. If 'val' does not
     * contain an _id, the _id from the original document is preserved. 'applyParams.element' must
     * be the root of the document. Always returns a result stating that indexes are affected when
     * the replacement is not a noop.
     */
    ApplyResult applyUpdate(ApplyParams applyParams) const final;

    Value serialize() const {
        std::vector<Value> valueArray;
        for (const auto& stage : _pipeline->getSources()) {
            // TODO SERVER-40539: Consider subclassing DocumentSourceQueue with a class that is
            // explicitly skipped when serializing. With that change call Pipeline::serialize()
            // directly.
            if (stage->getSourceName() == "mock"_sd) {
                continue;
            }

            stage->serializeToArray(valueArray);
        }

        return Value(valueArray);
    }

    void acceptVisitor(UpdateNodeVisitor* visitor) final {
        visitor->visit(this);
    }

    void setCollator(const CollatorInterface* collator) final {}

private:
    boost::intrusive_ptr<ExpressionContext> _expCtx;
    std::unique_ptr<Pipeline, PipelineDeleter> _pipeline;
};

}  // namespace mongo
