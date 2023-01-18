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

#include <deque>

#include "mongo/db/pipeline/document_source_queue.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/pipeline/stage_constraints.h"

namespace mongo {

/**
 * A mock DocumentSource which is useful for testing. In addition to re-spooling documents like
 * DocumentSourceQueue, it tracks some state about which methods have been called.
 */
class DocumentSourceMock : public DocumentSourceQueue {
public:
    static boost::intrusive_ptr<DocumentSourceMock> createForTest(
        const boost::intrusive_ptr<ExpressionContext>& expCtx);

    static boost::intrusive_ptr<DocumentSourceMock> createForTest(
        Document doc, const boost::intrusive_ptr<ExpressionContext>& expCtx);

    /**
     * Convenience constructor that works with a vector of BSONObj or vector of Documents.
     */
    template <typename Doc>
    static boost::intrusive_ptr<DocumentSourceMock> createForTest(
        const std::vector<Doc>& docs, const boost::intrusive_ptr<ExpressionContext>& expCtx) {
        std::deque<GetNextResult> results;
        for (auto&& doc : docs) {
            results.emplace_back(Document(doc));
        }
        return new DocumentSourceMock(std::move(results), expCtx);
    }

    static boost::intrusive_ptr<DocumentSourceMock> createForTest(
        const GetNextResult& result, const boost::intrusive_ptr<ExpressionContext>& expCtx);
    static boost::intrusive_ptr<DocumentSourceMock> createForTest(
        std::deque<GetNextResult> results, const boost::intrusive_ptr<ExpressionContext>& expCtx);

    static boost::intrusive_ptr<DocumentSourceMock> createForTest(
        const char* json, const boost::intrusive_ptr<ExpressionContext>& expCtx);
    static boost::intrusive_ptr<DocumentSourceMock> createForTest(
        const std::initializer_list<const char*>& jsons,
        const boost::intrusive_ptr<ExpressionContext>& expCtx);

    DocumentSourceMock(std::deque<GetNextResult>, const boost::intrusive_ptr<ExpressionContext>&);

    Value serialize(
        boost::optional<ExplainOptions::Verbosity> explain = boost::none) const override {
        // Unlike the queue, it's okay to serialize this stage for testing purposes.
        return Value(Document{{getSourceName(), Document()}});
    }

    const char* getSourceName() const override;

    size_t size() const;

    void reattachToOperationContext(OperationContext* opCtx) {
        isDetachedFromOpCtx = false;
    }

    void detachFromOperationContext() {
        isDetachedFromOpCtx = true;
    }

    boost::intrusive_ptr<DocumentSource> optimize() override {
        isOptimized = true;
        return this;
    }

    StageConstraints constraints(Pipeline::SplitState pipeState) const override {
        return mockConstraints;
    }

    /**
     * This stage does not modify anything.
     */
    GetModPathsReturn getModifiedPaths() const override {
        return {GetModPathsReturn::Type::kFiniteSet, OrderedPathSet{}, {}};
    }

    boost::optional<DistributedPlanLogic> distributedPlanLogic() override {
        return boost::none;
    }

    bool isDisposed = false;
    bool isDetachedFromOpCtx = false;
    bool isOptimized = false;
    StageConstraints mockConstraints;


protected:
    GetNextResult doGetNext() override {
        invariant(!isDisposed);
        invariant(!isDetachedFromOpCtx);
        return DocumentSourceQueue::doGetNext();
    }

    void doDispose() override {
        isDisposed = true;
    }
};

}  // namespace mongo
