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

#include "mongo/base/error_codes.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/auth/privilege.h"
#include "mongo/db/exec/agg/exec_pipeline.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/exec/plan_stats.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/lite_parsed_document_source.h"
#include "mongo/db/pipeline/lite_parsed_pipeline.h"
#include "mongo/db/pipeline/pipeline.h"
#include "mongo/db/pipeline/stage_constraints.h"
#include "mongo/db/pipeline/variables.h"
#include "mongo/db/query/compiler/dependency_analysis/dependencies.h"
#include "mongo/db/query/explain_options.h"
#include "mongo/db/query/query_shape/serialization_options.h"
#include "mongo/db/stats/counters.h"
#include "mongo/stdx/unordered_set.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/intrusive_counter.h"

#include <algorithm>
#include <list>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {

struct UnionWithSharedState {
    enum ExecutionProgress {
        // We haven't yet iterated 'pSource' to completion.
        kIteratingSource,

        // We finished iterating 'pSource', but haven't started on the sub pipeline and need to do
        // some setup first.
        kStartingSubPipeline,

        // We finished iterating 'pSource' and are now iterating '_pipeline', but haven't finished
        // yet.
        kIteratingSubPipeline,

        // There are no more results.
        kFinished
    };
    std::unique_ptr<Pipeline> _pipeline;
    std::unique_ptr<exec::agg::Pipeline> _execPipeline;
    // The aggregation pipeline defined with the user request, prior to optimization and view
    // resolution.
    ExecutionProgress _executionState = ExecutionProgress::kIteratingSource;

    // $unionWith will execute the subpipeline twice for explain with execution stats - once for
    // results and once for explain info. During the first execution, built in variables (such as
    // SEARCH_META) might be set, which are not allowed to be set at the start of the second
    // execution. We need to store the initial state of the variables to reset them for the second
    // execution
    Variables _variables;
    VariablesParseState _variablesParseState;
};

class DocumentSourceUnionWith final : public DocumentSource {
public:
    static constexpr StringData kStageName = "$unionWith"_sd;

    static boost::intrusive_ptr<DocumentSource> createFromBson(
        BSONElement elem, const boost::intrusive_ptr<ExpressionContext>& expCtx);

    class LiteParsed final : public LiteParsedDocumentSourceNestedPipelines {
    public:
        static std::unique_ptr<LiteParsed> parse(const NamespaceString& nss,
                                                 const BSONElement& spec,
                                                 const LiteParserOptions& options);

        LiteParsed(std::string parseTimeName,
                   NamespaceString foreignNss,
                   boost::optional<LiteParsedPipeline> pipeline)
            : LiteParsedDocumentSourceNestedPipelines(
                  std::move(parseTimeName), std::move(foreignNss), std::move(pipeline)) {}

        PrivilegeVector requiredPrivileges(bool isMongos,
                                           bool bypassDocumentValidation) const final;

        bool requiresAuthzChecks() const override {
            return false;
        }
    };

    DocumentSourceUnionWith(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                            NamespaceString unionNss,
                            std::vector<BSONObj> pipeline);

    // Expose a constructor that skips the parsing step for testing purposes.
    DocumentSourceUnionWith(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                            std::unique_ptr<Pipeline> pipeline);

    DocumentSourceUnionWith(const DocumentSourceUnionWith& original,
                            const boost::intrusive_ptr<ExpressionContext>& newExpCtx);

    ~DocumentSourceUnionWith() override;

    const char* getSourceName() const final {
        return kStageName.data();
    }

    static const Id& id;

    Id getId() const override {
        return id;
    }

    GetModPathsReturn getModifiedPaths() const final {
        // Since we might have a document arrive from the foreign pipeline with the same path as a
        // document in the main pipeline. Without introspecting the sub-pipeline, we must report
        // that all paths have been modified.
        return {GetModPathsReturn::Type::kAllPaths, {}, {}};
    }

    StageConstraints constraints(PipelineSplitState) const final {
        StageConstraints unionConstraints(
            StreamType::kStreaming,
            PositionRequirement::kNone,
            HostTypeRequirement::kAnyShard,
            DiskUseRequirement::kNoDiskUse,
            FacetRequirement::kAllowed,
            TransactionRequirement::kNotAllowed,
            // The check to disallow $unionWith on a sharded collection within $lookup happens
            // outside of the constraints as long as the involved namespaces are reported correctly.
            LookupRequirement::kAllowed,
            UnionRequirement::kAllowed);

        if (_sharedState->_pipeline) {
            // The constraints of the sub-pipeline determine the constraints of the $unionWith
            // stage. We want to forward the strictest requirements of the stages in the
            // sub-pipeline.
            unionConstraints = StageConstraints::getStrictestConstraints(
                _sharedState->_pipeline->getSources(), unionConstraints);
        }
        // DocumentSourceUnionWith cannot directly swap with match but it contains custom logic in
        // the doOptimizeAt() member function to allow itself to duplicate any match ahead in the
        // current pipeline and place one copy inside its sub-pipeline and one copy behind in the
        // current pipeline.
        unionConstraints.canSwapWithMatch = false;
        return unionConstraints;
    }

    DepsTracker::State getDependencies(DepsTracker* deps) const final;

    void addVariableRefs(std::set<Variables::Id>* refs) const final;

    boost::optional<DistributedPlanLogic> distributedPlanLogic() final {
        // {shardsStage, mergingStage, sortPattern}
        return DistributedPlanLogic{nullptr, this, boost::none};
    }

    void addInvolvedCollections(stdx::unordered_set<NamespaceString>* collectionNames) const final;

    void detachSourceFromOperationContext() final;

    void reattachSourceToOperationContext(OperationContext* opCtx) final;

    bool validateSourceOperationContext(const OperationContext* opCtx) const final;

    bool hasNonEmptyPipeline() const {
        return _sharedState->_pipeline && !_sharedState->_pipeline->empty();
    }

    const Pipeline& getPipeline() const {
        tassert(7113100, "Pipeline has been already disposed", _sharedState->_pipeline);
        return *_sharedState->_pipeline;
    }

    Pipeline& getPipeline() {
        tassert(7113101, "Pipeline has been already disposed", _sharedState->_pipeline);
        return *_sharedState->_pipeline;
    }

    boost::intrusive_ptr<DocumentSource> clone(
        const boost::intrusive_ptr<ExpressionContext>& newExpCtx) const final;

    const DocumentSourceContainer* getSubPipeline() const final {
        if (_sharedState->_pipeline) {
            return &_sharedState->_pipeline->getSources();
        }
        return nullptr;
    }

    std::shared_ptr<UnionWithSharedState> getSharedState() const {
        return _sharedState;
    }

protected:
    DocumentSourceContainer::iterator doOptimizeAt(DocumentSourceContainer::iterator itr,
                                                   DocumentSourceContainer* container) final;

    boost::intrusive_ptr<DocumentSource> optimize() final {
        _sharedState->_pipeline->optimizePipeline();
        return this;
    }

private:
    friend exec::agg::StagePtr documentSourceUnionWithToStageFn(
        const boost::intrusive_ptr<const DocumentSource>& documentSource);

    Value serialize(const SerializationOptions& opts = SerializationOptions{}) const final;

    void addViewDefinition(NamespaceString nss, std::vector<BSONObj> viewPipeline);

    std::shared_ptr<UnionWithSharedState> _sharedState;

    // The original, unresolved namespace to union.
    NamespaceString _userNss;

    // The aggregation pipeline defined with the user request, prior to optimization and view
    // resolution.
    std::vector<BSONObj> _userPipeline;

    // Match and/or project stages after a $unionWith can be pushed down into the $unionWith (and
    // the head of the pipeline). If we're doing an explain with execution stats, we will need
    // copies of these stages as they may be pushed down to the find layer.
    std::vector<BSONObj> _pushedDownStages;

    // State that we preserve in the case where we are running explain with 'executionStats' on a
    // $unionWith with a view. Otherwise we wouldn't be able to see details about the execution of
    // the view pipeline in the explain result.
    boost::optional<ResolvedNamespace> _resolvedNsForView;
};

}  // namespace mongo
