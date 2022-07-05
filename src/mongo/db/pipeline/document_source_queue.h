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

#include <deque>

#include "mongo/db/pipeline/document_source.h"
namespace mongo {

/**
 * A DocumentSource which re-spools a queue of documents loaded into it. This stage does not
 * retrieve any input from an earlier stage. It can be useful to adapt the usual pull-based model of
 * a pipeline to more of a push-based model by pushing documents to feed through the pipeline into
 * this queue stage.
 */
class DocumentSourceQueue : public DocumentSource {
public:
    static constexpr StringData kStageName = "$queue"_sd;

    static boost::intrusive_ptr<DocumentSourceQueue> create(
        const boost::intrusive_ptr<ExpressionContext>& expCtx,
        boost::optional<StringData> aliasStageName = boost::none);

    DocumentSourceQueue(std::deque<GetNextResult> results,
                        const boost::intrusive_ptr<ExpressionContext>& expCtx,
                        boost::optional<StringData> aliasStageName = boost::none);
    ~DocumentSourceQueue() override = default;

    const char* getSourceName() const override;

    Value serialize(boost::optional<ExplainOptions::Verbosity> explain) const override;

    StageConstraints constraints(Pipeline::SplitState pipeState) const override {
        StageConstraints constraints{StreamType::kStreaming,
                                     PositionRequirement::kFirst,
                                     HostTypeRequirement::kLocalOnly,
                                     DiskUseRequirement::kNoDiskUse,
                                     FacetRequirement::kNotAllowed,
                                     TransactionRequirement::kAllowed,
                                     LookupRequirement::kAllowed,
                                     UnionRequirement::kAllowed};
        constraints.requiresInputDocSource = false;
        constraints.isIndependentOfAnyCollection = true;
        return constraints;
    }

    /**
     * This stage does not modify anything.
     */
    GetModPathsReturn getModifiedPaths() const override {
        return {GetModPathsReturn::Type::kFiniteSet, OrderedPathSet{}, {}};
    }

    /**
     * This stage does not depend on anything.
     */
    DepsTracker::State getDependencies(DepsTracker* deps) const override {
        return DepsTracker::SEE_NEXT;
    }

    boost::optional<DistributedPlanLogic> distributedPlanLogic() override {
        return boost::none;
    }

    template <class... Args>
    GetNextResult& emplace_back(Args&&... args) {
        return _queue.emplace_back(std::forward<Args>(args)...);
    }

    void push_back(GetNextResult&& result) {
        _queue.push_back(std::move(result));
    }

    void push_back(const GetNextResult& result) {
        _queue.push_back(result);
    }

    static boost::intrusive_ptr<DocumentSource> createFromBson(
        BSONElement elem, const boost::intrusive_ptr<ExpressionContext>& expCtx);

protected:
    GetNextResult doGetNext() override;
    // Return documents from front of queue.
    std::deque<GetNextResult> _queue;

    // An optional alias name is provided for cases like $documents where we want an error message
    // to indicate the name the user provided, not the internal $queue name.
    boost::optional<StringData> _aliasStageName = boost::none;
};

}  // namespace mongo
