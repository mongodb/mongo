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

#include "mongo/base/status_with.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/pipeline/document_source_geo_near.h"
#include "mongo/db/pipeline/document_source_lookup.h"
#include "mongo/db/pipeline/expression_context_builder.h"
#include "mongo/db/pipeline/pipeline_d.h"
#include "mongo/db/pipeline/pipeline_factory.h"
#include "mongo/db/query/compiler/optimizer/join/path_resolver.h"

#include <memory>
#include <utility>

namespace mongo::join_ordering {
namespace {
std::unique_ptr<Pipeline> createEmptyPipeline(
    const boost::intrusive_ptr<ExpressionContext>& sourceExpCtx) {
    auto expCtx = makeCopyFromExpressionContext(sourceExpCtx, sourceExpCtx->getNamespaceString());
    pipeline_factory::MakePipelineOptions opts;
    opts.attachCursorSource = false;
    std::vector<BSONObj> emptyPipeline;

    return pipeline_factory::makePipeline(emptyPipeline, expCtx, opts);
}

std::unique_ptr<CanonicalQuery> makeFullScanCQ(boost::intrusive_ptr<ExpressionContext> expCtx) {
    auto fcr = std::make_unique<FindCommandRequest>(expCtx->getNamespaceString());
    return std::make_unique<CanonicalQuery>(CanonicalQueryParams{
        .expCtx = expCtx, .parsedFind = ParsedFindCommandParams{.findCommand = std::move(fcr)}});
}

StatusWith<std::unique_ptr<CanonicalQuery>> makeCQFromLookup(
    DocumentSourceLookUp* stage, boost::intrusive_ptr<ExpressionContext> pipelineExpCtx) {
    auto expCtx = stage->getSubpipelineExpCtx();
    if (stage->hasPipeline()) {
        stage = dynamic_cast<DocumentSourceLookUp*>(stage);
        auto swCQ = createCanonicalQuery(
            expCtx, stage->getFromNs(), stage->getResolvedIntrospectionPipeline());
        if (swCQ.isOK()) {
            auto cq = std::move(swCQ.getValue());
            if (!stage->getResolvedIntrospectionPipeline().getSources().empty()) {
                // We failed to pushdown the whole subpipeline into SBE- bail out without modifying
                // the document source.
                return Status(ErrorCodes::QueryFeatureNotAllowed,
                              "Join reordering is not enabled for $lookups with sub-pipelines that "
                              "can't be fully pushed down into a CanonicalQuery");
            }
            return std::move(cq);
        }
        // Bail out.
        return swCQ;
    }
    return makeFullScanCQ(expCtx);
}

BSONObj resolvedPathToBSON(const ResolvedPath& rp) {
    return BSON("nodeId" << rp.nodeId << "fieldName" << rp.fieldName.fullPath());
}

std::vector<BSONObj> pipelineToBSON(const std::unique_ptr<Pipeline>& pipeline) {
    if (pipeline) {
        return pipeline->serializeToBson();
    } else {
        return {};
    }
}
}  // namespace

bool AggJoinModel::pipelineEligibleForJoinReordering(const Pipeline& pipeline) {
    // Pipelines starting with $geoNear are not eligible.
    if (!pipeline.getSources().empty() &&
        dynamic_cast<DocumentSourceGeoNear*>(pipeline.peekFront())) {
        return false;
    }

    // Since we can reorder base collections, any pipeline with even just one eligible $lookup +
    // $unwind pair could be eligible.
    return std::any_of(pipeline.getSources().begin(), pipeline.getSources().end(), [](auto ds) {
        if (auto* lookup = dynamic_cast<DocumentSourceLookUp*>(ds.get());
            // TODO SERVER-111164: once we start adding edge from $expr we need to remove check for
            // hasLocalFieldForeignFieldJoin().
            lookup != nullptr && lookup->hasUnwindSrc() &&
            lookup->hasLocalFieldForeignFieldJoin()) {
            return true;
        }
        return false;
    });
}

StatusWith<AggJoinModel> AggJoinModel::constructJoinModel(const Pipeline& pipeline) {
    // Try to create a CanonicalQuery. We begin by cloning the pipeline (this includes
    // sub-pipelines!) to ensure that if we bail out, this stays idempotent.
    // TODO SERVER-111383: We should see if we can make createCanonicalQuery() idempotent instead.
    auto expCtx = pipeline.getContext();
    const auto& nss = expCtx->getNamespaceString();
    auto clonedExpCtx = makeCopyFromExpressionContext(expCtx, nss);
    auto suffix = pipeline.clone(clonedExpCtx);

    auto swCQ = createCanonicalQuery(expCtx, nss, *suffix);
    if (!swCQ.isOK()) {
        // Bail out & return the failure status- we failed to generate a CanonicalQuery from a
        // pipeline prefix.
        return swCQ.getStatus();
    }

    // Initialize the JoinGraph & base NodeId.
    JoinGraph graph;
    auto baseNodeId =
        graph.addNode(expCtx->getNamespaceString(), std::move(swCQ.getValue()), boost::none);
    if (!baseNodeId) {
        return Status(ErrorCodes::BadValue, "Failed to create a node for base collection");
    }

    auto prefix = createEmptyPipeline(suffix->getContext());
    std::vector<ResolvedPath> resolvedPaths;
    PathResolver pathResolver{*baseNodeId, resolvedPaths};

    // Go through the pipeline trying to find the maximal chain of join optimization eligible
    // $lookup+$unwinds pairs and turning them into CanonicalQueries. At the end only ineligible for
    // join optimization stages are left in the suffix.
    // If we already reach the maximum number of edges we bail out from building the graph and put
    // the remaining stages into the suffix.
    while (!suffix->getSources().empty() && graph.numNodes() < kMaxNodesInJoin) {
        auto* stage = suffix->getSources().front().get();
        if (auto* lookup = dynamic_cast<DocumentSourceLookUp*>(stage); lookup) {
            // TODO SERVER-111164: once we start adding edge from $expr we need to remove check for
            // hasLocalFieldForeignFieldJoin(). The same check below is kept below for the future
            // needs.
            if (!lookup->hasUnwindSrc() || !lookup->hasLocalFieldForeignFieldJoin()) {
                // We can't push this $lookup to the prefix.
                break;
            }

            if (lookup->hasPipeline()) {
                // TODO SERVER-111910: Enable lookup with sub-pipelines for join-opt.
                break;
            }

            auto swCQ = makeCQFromLookup(lookup, lookup->getSubpipelineExpCtx());
            if (!swCQ.isOK()) {
                break;
            }

            auto foreignNodeId = graph.addNode(
                lookup->getFromNs(), std::move(swCQ.getValue()), lookup->getAsField());

            if (!foreignNodeId) {
                return Status(ErrorCodes::BadValue, "Graph is too big: too many nodes");
            }

            pathResolver.addNode(*foreignNodeId, lookup->getAsField());

            if (lookup->hasLocalFieldForeignFieldJoin()) {
                // The order of resolving the paths are important here: localPathId shouln't be
                // resolved to the foreign collection even if it is prefixed by the foreign
                // collection's embedPath.
                auto localPathId = pathResolver.resolve(*lookup->getLocalField());
                auto foreignPathId =
                    pathResolver.addPath(*foreignNodeId, *lookup->getForeignField());

                auto edgeId = graph.addSimpleEqualityEdge(
                    pathResolver[localPathId].nodeId, *foreignNodeId, localPathId, foreignPathId);
                if (!edgeId) {
                    // Cannot add an edge for existing nodes.
                    return Status(ErrorCodes::BadValue, "Graph is too big: too many edges");
                }
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

    if (graph.numNodes() < 2) {
        // We need at least 1 eligible $lookup and a fully SBE-pushed-down prefix.
        return Status(ErrorCodes::QueryFeatureNotAllowed, "Join reordering not allowed");
    }

    return AggJoinModel(
        std::move(graph), std::move(resolvedPaths), std::move(prefix), std::move(suffix));
}

BSONObj AggJoinModel::toBSON() const {
    BSONObjBuilder result{};
    result.append("graph", graph.toBSON());
    {
        BSONArrayBuilder ab{};
        for (const auto& rp : resolvedPaths) {
            ab.append(resolvedPathToBSON(rp));
        }
        result.append("resolvedPaths", ab.arr());
    }
    result.append("prefix", pipelineToBSON(prefix));
    result.append("suffix", pipelineToBSON(suffix));
    return result.obj();
}
}  // namespace mongo::join_ordering
