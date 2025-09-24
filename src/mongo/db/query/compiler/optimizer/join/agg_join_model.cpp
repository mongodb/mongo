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

#include "mongo/db/query/compiler/optimizer/join/agg_join_model.h"

#include "mongo/db/pipeline/document_source_lookup.h"
#include "mongo/db/pipeline/expression_context_builder.h"
#include "mongo/db/pipeline/pipeline_d.h"
#include "mongo/db/query/compiler/optimizer/join/path_resolver.h"

namespace mongo::join_ordering {
std::unique_ptr<Pipeline> createEmptyPipeline(
    const boost::intrusive_ptr<ExpressionContext>& sourceExpCtx) {
    auto expCtx = makeCopyFromExpressionContext(sourceExpCtx, sourceExpCtx->getNamespaceString());
    MakePipelineOptions opts;
    opts.attachCursorSource = false;
    std::vector<BSONObj> emptyPipeline;

    return Pipeline::makePipeline(emptyPipeline, expCtx, opts);
}

std::unique_ptr<CanonicalQuery> makeFullScanCQ(boost::intrusive_ptr<ExpressionContext> expCtx) {
    auto fcr = std::make_unique<FindCommandRequest>(expCtx->getNamespaceString());
    return std::make_unique<CanonicalQuery>(CanonicalQueryParams{
        .expCtx = expCtx, .parsedFind = ParsedFindCommandParams{.findCommand = std::move(fcr)}});
}

std::unique_ptr<CanonicalQuery> makeCQFromLookup(
    DocumentSourceLookUp* stage, boost::intrusive_ptr<ExpressionContext> pipelineExpCtx) {
    auto expCtx = stage->getSubpipelineExpCtx();
    if (stage->hasPipeline()) {
        // TODO SERVER-111383: Copy the original stage to keep it untocuhed in case of any issue.
        auto workingStage = stage->clone(pipelineExpCtx);
        stage = dynamic_cast<DocumentSourceLookUp*>(workingStage.get());

        stage->getResolvedIntrospectionPipeline().optimizePipeline();
        auto swCQ = createCanonicalQuery(
            expCtx, stage->getFromNs(), stage->getResolvedIntrospectionPipeline());
        const bool allSubPipelineStagesPushedDown =
            stage->getResolvedIntrospectionPipeline().getSources().empty();
        if (swCQ.isOK() && allSubPipelineStagesPushedDown) {
            return std::move(swCQ.getValue());
        }
        // Bail out.
        return nullptr;
    }
    return makeFullScanCQ(expCtx);
}

bool AggJoinModel::canOptimizeWithJoinReordering(const std::unique_ptr<Pipeline>& pipeline) {
    size_t numLookupsWithUnwind = 0;

    for (auto&& stage : pipeline->getSources()) {
        if (auto* lookup = dynamic_cast<DocumentSourceLookUp*>(stage.get());
            // TODO SERVER-111164: once we start adding edge from $expr we need to remove check for
            // hasLocalFieldForeignFieldJoin().
            lookup != nullptr && lookup->hasUnwindSrc() &&
            lookup->hasLocalFieldForeignFieldJoin()) {
            ++numLookupsWithUnwind;
        }
    }

    return numLookupsWithUnwind < 2;
}

AggJoinModel::AggJoinModel(std::unique_ptr<Pipeline> pipeline) {
    suffix = std::move(pipeline);
    if (!canOptimizeWithJoinReordering(suffix)) {
        build();
    }
}

void AggJoinModel::build() {
    prefix = createEmptyPipeline(suffix->getContext());

    auto baseNodeId = makeBaseNode();
    if (!baseNodeId.has_value()) {
        return;
    }

    PathResolver pathResolver{baseNodeId.value(), resolvedPaths};

    // Go through the pipeline trying to find the maximal chain of join optimization eligible
    // $lookup+$unwinds pairs and turning them into CanonicalQueries. At the end only ineligible for
    // join optimization stages are left in the suffix.
    while (!suffix->getSources().empty()) {
        auto* stage = suffix->getSources().front().get();
        if (auto* lookup = dynamic_cast<DocumentSourceLookUp*>(stage); lookup) {
            // TODO SERVER-111164: once we start adding edge from $expr we need to remove check for
            // hasLocalFieldForeignFieldJoin(). The same check below is kept below for the future
            // needs.
            if (!lookup->hasUnwindSrc() || !lookup->hasLocalFieldForeignFieldJoin()) {
                // We can't push this $lookup to the prefix.
                break;
            }

            auto cq = makeCQFromLookup(lookup, lookup->getSubpipelineExpCtx());
            if (!cq) {
                break;
            }

            auto foreignNodeId =
                graph.addNode(lookup->getFromNs(), std::move(cq), lookup->getAsField());
            pathResolver.addNode(foreignNodeId, lookup->getAsField());

            if (lookup->hasLocalFieldForeignFieldJoin()) {
                // The order of resolving the paths are important here: localPathId shouln't be
                // resolved to the foreign collection even if it is prefixed by the foreign
                // collection's embedPath.
                auto localPathId = pathResolver.resolve(*lookup->getLocalField());
                auto foreignPathId =
                    pathResolver.addPath(foreignNodeId, *lookup->getForeignField());

                graph.addSimpleEqualityEdge(
                    pathResolver[localPathId].nodeId, foreignNodeId, localPathId, foreignPathId);
            }

            // TODO SERVER-111164: add edges from $expr's

            auto next = suffix->popFront();
            if (prefix->getSources().empty()) {
                prefix->addInitialSource(std::move(next));
            } else {
                prefix->pushBack(std::move(next));
            }
        } else {
            // Unrecognized stage, give up on building a prefix.
            break;
        }
    }
}

boost::optional<NodeId> AggJoinModel::makeBaseNode() {
    auto expCtx = suffix->getContext();
    const auto& baseColl = expCtx->getNamespaceString();
    auto swCQ = createCanonicalQuery(expCtx, baseColl, *suffix);
    if (!swCQ.isOK()) {
        // Bail out.
        return boost::none;
    }
    return graph.addNode(expCtx->getNamespaceString(), std::move(swCQ.getValue()), boost::none);
}
}  // namespace mongo::join_ordering
