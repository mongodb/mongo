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

#include <boost/optional.hpp>

#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/lite_parsed_document_source.h"
#include "mongo/db/pipeline/lite_parsed_pipeline.h"
#include "mongo/db/pipeline/stage_constraints.h"

namespace mongo {

class DocumentSourceUnionWith final : public DocumentSource {
public:
    static constexpr StringData kStageName = "$unionWith"_sd;

    static boost::intrusive_ptr<DocumentSource> createFromBson(
        BSONElement elem, const boost::intrusive_ptr<ExpressionContext>& expCtx);

    class LiteParsed final : public LiteParsedDocumentSourceNestedPipelines {
    public:
        static std::unique_ptr<LiteParsed> parse(const NamespaceString& nss,
                                                 const BSONElement& spec);

        LiteParsed(std::string parseTimeName,
                   NamespaceString foreignNss,
                   boost::optional<LiteParsedPipeline> pipeline)
            : LiteParsedDocumentSourceNestedPipelines(
                  std::move(parseTimeName), std::move(foreignNss), std::move(pipeline)) {}

        PrivilegeVector requiredPrivileges(bool isMongos,
                                           bool bypassDocumentValidation) const override final;
    };

    DocumentSourceUnionWith(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                            std::unique_ptr<Pipeline, PipelineDeleter> pipeline)
        : DocumentSource(kStageName, expCtx), _pipeline(std::move(pipeline)) {}

    ~DocumentSourceUnionWith();

    const char* getSourceName() const final {
        return kStageName.rawData();
    }

    GetModPathsReturn getModifiedPaths() const final {
        // Since we might have a document arrive from the foreign pipeline with the same path as a
        // document in the main pipeline. Without introspecting the sub-pipeline, we must report
        // that all paths have been modified.
        return {GetModPathsReturn::Type::kAllPaths, {}, {}};
    }

    StageConstraints constraints(Pipeline::SplitState) const final {
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

        if (_pipeline) {
            // The constraints of the sub-pipeline determine the constraints of the $unionWith
            // stage. We want to forward the strictest requirements of the stages in the
            // sub-pipeline.
            unionConstraints = StageConstraints::getStrictestConstraints(_pipeline->getSources(),
                                                                         unionConstraints);
        }
        // DocumentSourceUnionWith cannot directly swap with match but it contains custom logic in
        // the doOptimizeAt() member function to allow itself to duplicate any match ahead in the
        // current pipeline and place one copy inside its sub-pipeline and one copy behind in the
        // current pipeline.
        unionConstraints.canSwapWithMatch = false;
        return unionConstraints;
    }

    DepsTracker::State getDependencies(DepsTracker* deps) const final;

    boost::optional<DistributedPlanLogic> distributedPlanLogic() final {
        // {shardsStage, mergingStage, sortPattern}
        return DistributedPlanLogic{nullptr, this, boost::none};
    }

    void addInvolvedCollections(stdx::unordered_set<NamespaceString>* collectionNames) const final;

    void detachFromOperationContext() final;

    void reattachToOperationContext(OperationContext* opCtx) final;

    bool usedDisk() final;

protected:
    GetNextResult doGetNext() final;

    Pipeline::SourceContainer::iterator doOptimizeAt(Pipeline::SourceContainer::iterator itr,
                                                     Pipeline::SourceContainer* container) final;

    boost::intrusive_ptr<DocumentSource> optimize() final {
        _pipeline->optimizePipeline();
        return this;
    }

    void doDispose() final;

private:
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

    Value serialize(boost::optional<ExplainOptions::Verbosity> explain = boost::none) const final;

    void addViewDefinition(NamespaceString nss, std::vector<BSONObj> viewPipeline);

    std::unique_ptr<Pipeline, PipelineDeleter> _pipeline;
    bool _usedDisk = false;
    ExecutionProgress _executionState = ExecutionProgress::kIteratingSource;
};

}  // namespace mongo
