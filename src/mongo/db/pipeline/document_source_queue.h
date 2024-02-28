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
#include <set>
#include <utility>

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/pipeline/dependencies.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/pipeline.h"
#include "mongo/db/pipeline/stage_constraints.h"
#include "mongo/db/pipeline/variables.h"
#include "mongo/db/query/query_shape/serialization_options.h"
#include "mongo/db/query/util/deferred.h"

namespace mongo {

/**
 * A DocumentSource which re-spools a queue of documents loaded into it. This stage does not
 * retrieve any input from an earlier stage. It can consume either a normal queue of documents or
 * a deferred queue, a lambda which lazily generates a queue of documents when required.
 *
 * This stage can also be useful to adapt the usual pull-based model of a pipeline to more of a
 * push-based model by pushing documents to feed through the pipeline into this queue stage.
 */
class DocumentSourceQueue : public DocumentSource {
public:
    using DeferredQueue = DeferredFn<std::deque<GetNextResult>>;

    static constexpr StringData kStageName = "$queue"_sd;

    static boost::intrusive_ptr<DocumentSourceQueue> create(
        const boost::intrusive_ptr<ExpressionContext>& expCtx,
        boost::optional<StringData> stageNameOverride = boost::none);

    /*
     * Construct a 'DocumentSourceQueue' stage with either a:
     * (1) An eagerly initialized 'std::deque' containing 'GetNextResult' objects.
     * (2) A zero argument lambda, which returns a 'std::deque' of 'GetNextResult' objects.
     *
     * Additionally, the DocumentSource behaviour can be customized by providing any of the
     * following optional parameters:
     *
     * 'stageNameOverride' - the name to be displayed in error messages, instead of the internal
     * '$queue' one.
     *
     * 'serializeOverride' - the 'Value' to be returned for 'serialize()' calls instead of
     * serializing the queue contents.
     *
     * 'constraintsOverride' - the 'StageConstraints' to be reported by 'constraints()' calls
     * instead of the default ones.
     */
    DocumentSourceQueue(DeferredQueue results,
                        const boost::intrusive_ptr<ExpressionContext>& expCtx,
                        boost::optional<StringData> stageNameOverride = boost::none,
                        boost::optional<Value> serializeOverride = boost::none,
                        boost::optional<StageConstraints> constraintsOverride = boost::none);

    ~DocumentSourceQueue() override = default;

    const char* getSourceName() const override;

    Value serialize(const SerializationOptions& opts = SerializationOptions{}) const override;

    StageConstraints constraints(Pipeline::SplitState pipeState) const override {
        if (_constraintsOverride.has_value()) {
            return *_constraintsOverride;
        }

        StageConstraints constraints{StreamType::kStreaming,
                                     PositionRequirement::kFirst,
                                     HostTypeRequirement::kRunOnceAnyNode,
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
        return _queue->emplace_back(std::forward<Args>(args)...);
    }

    void push_back(GetNextResult&& result) {
        _queue->push_back(std::move(result));
    }

    void push_back(const GetNextResult& result) {
        _queue->push_back(result);
    }

    void addVariableRefs(std::set<Variables::Id>* refs) const final {}

    static boost::intrusive_ptr<DocumentSource> createFromBson(
        BSONElement elem, const boost::intrusive_ptr<ExpressionContext>& expCtx);

protected:
    // Documents are always returned starting from the front.
    GetNextResult doGetNext() override;

    DeferredQueue _queue;

    // An optional alias name is provided for cases like $documents where we want an error message
    // to indicate the name the user provided, not the internal $queue name.
    boost::optional<StringData> _stageNameOverride = boost::none;

    // An optional value provided for cases like '$querySettings' and '$indexStats' where it's
    // desireable to serialize the stage as something other than '$queue'.
    boost::optional<Value> _serializeOverride = boost::none;

    // An optional 'StageConstraints' override useful for cases such as '$indexStats' where fine
    // grained over the constraints are needed.
    boost::optional<StageConstraints> _constraintsOverride = boost::none;
};

}  // namespace mongo
