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

#pragma once

#include "mongo/bson/bsonobj.h"
#include "mongo/db/exec/agg/exec_pipeline.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/plan_stats.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/pipeline/document_source_match.h"
#include "mongo/db/pipeline/document_source_unwind.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/field_path.h"
#include "mongo/db/pipeline/pipeline.h"
#include "mongo/db/pipeline/sequential_document_cache.h"
#include "mongo/db/pipeline/variables.h"

#include <memory>
#include <vector>

#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {

void lookupPipeValidator(const Pipeline& pipeline);

class LookUpSharedState {
public:
    LookUpSharedState(const NamespaceString& fromNs,
                      const FieldPath& as,
                      const boost::intrusive_ptr<ExpressionContext>& expCtx,
                      bool isForeignShardedLookupAllowed)
        : fromNs(fromNs),
          as(as),
          variables(expCtx->variables),
          variablesParseState(expCtx->variablesParseState.copyWith(variables.useIdGenerator())),
          isForeignShardedLookupAllowed(isForeignShardedLookupAllowed) {}

    LookUpSharedState(const NamespaceString& fromNs,
                      const FieldPath& as,
                      const boost::optional<BSONObj>& additionalFilter,
                      const boost::optional<FieldPath>& localField,
                      const boost::optional<FieldPath>& foreignField,
                      const boost::optional<size_t>& fieldMatchPipelineIdx,
                      const Variables& variables,
                      const VariablesParseState& variablesParseState,
                      const boost::intrusive_ptr<ExpressionContext>& fromExpCtx,
                      const std::vector<BSONObj>& resolvedPipeline,
                      const boost::optional<std::vector<BSONObj>>& userPipeline,
                      const std::vector<LetVariable>& letVariables,
                      bool isForeignShardedLookupAllowed)
        : fromNs(fromNs),
          as(as),
          additionalFilter(additionalFilter),
          localField(localField),
          foreignField(foreignField),
          fieldMatchPipelineIdx(fieldMatchPipelineIdx),
          variables(variables),
          variablesParseState(variablesParseState),
          fromExpCtx(fromExpCtx),
          resolvedPipeline(resolvedPipeline),
          userPipeline(userPipeline),
          letVariables(letVariables),
          isForeignShardedLookupAllowed(isForeignShardedLookupAllowed) {}

    /**
     * Builds the $lookup pipeline and resolves any variables using the passed 'inputDoc',
     * adding a cursor and/or cache source as appropriate.
     */
    // TODO SERVER-84208: Refactor this method so as to clearly separate the logic for the
    // streams engine from the logic for the classic $lookup..
    template <bool isStreamsEngine = false>
    std::unique_ptr<Pipeline> buildPipeline(
        const boost::intrusive_ptr<ExpressionContext>& fromExpCtx,
        const Document& inputDoc,
        const boost::intrusive_ptr<ExpressionContext>& expCtx);

    const std::vector<LetVariable>& getLetVariables() const {
        return letVariables;
    }

    NamespaceString fromNs;

    // Path to the "as" field of the $lookup where the matches output array will be created.
    FieldPath as;

    boost::optional<BSONObj> additionalFilter;

    // For use when $lookup is specified with localField/foreignField syntax.
    boost::optional<FieldPath> localField;
    boost::optional<FieldPath> foreignField;

    // Indicates the index in '_sharedState->resolvedPipeline' where the
    // local/foreignField $match resides.
    boost::optional<size_t> fieldMatchPipelineIdx;

    // Holds 'let' defined variables defined both in this stage and in parent pipelines. These
    // are copied to the '_sharedState->fromExpCtx' ExpressionContext's 'variables' and
    // 'variablesParseState' for use in foreign pipeline execution.
    Variables variables;
    VariablesParseState variablesParseState;

    // The ExpressionContext used when performing aggregation pipelines against the
    // '_resolvedNs' namespace.
    boost::intrusive_ptr<ExpressionContext> fromExpCtx;

    // The aggregation pipeline to perform against the '_resolvedNs' namespace. Referenced view
    // namespaces have been resolved.
    std::vector<BSONObj> resolvedPipeline;
    // The aggregation pipeline defined with the user request, prior to optimization and view
    // resolution. If the user did not define a pipeline this will be 'boost::none'.
    boost::optional<std::vector<BSONObj>> userPipeline;

    // Holds 'let' variables defined in $lookup stage. 'let' variables are stored in the vector
    // in order to ensure the stability in the query shape serialization.
    std::vector<LetVariable> letVariables;

    // A pipeline parsed from _sharedState->resolvedPipeline at creation time, intended to
    // support introspective functions. If sub-$lookup stages are present, their pipelines are
    // constructed recursively.
    std::unique_ptr<Pipeline> resolvedIntrospectionPipeline;

    // TODO: SERVER-109080 Move 'cache' completely to LookUpStage
    // Caches documents returned by the non-correlated prefix of the $lookup pipeline during the
    // first iteration, up to a specified size limit in bytes. If this limit is not exceeded by
    // the time we hit EOF, subsequent iterations of the pipeline will draw from the cache
    // rather than from a cursor source.
    boost::optional<SequentialDocumentCache> cache;

    DocumentSourceLookupStats stats;

    boost::intrusive_ptr<DocumentSourceUnwind> unwindSrc;
    boost::intrusive_ptr<DocumentSourceMatch> matchSrc;

    std::unique_ptr<Pipeline> pipeline;
    std::unique_ptr<exec::agg::Pipeline> execPipeline;

    bool isForeignShardedLookupAllowed;

private:
    /**
     * Resolves let defined variables against 'localDoc' and stores the results in 'variables'.
     */
    void resolveLetVariables(const Document& localDoc,
                             Variables* variables,
                             const boost::intrusive_ptr<ExpressionContext>& expCtx);

    /**
     * Builds the $lookup pipeline using the resolved view definition for a sharded foreign view
     * and updates the '_sharedState->resolvedPipeline', as well as
     * '_sharedState->fieldMatchPipelineIdx' in the case of a 'foreign' join.
     */
    std::unique_ptr<Pipeline> buildPipelineFromViewDefinition(
        std::vector<BSONObj> serializedPipeline, ResolvedNamespace resolvedNamespace);

    /**
     * Method to add a DocumentSourceSequentialDocumentCache stage and optimize the pipeline to
     * move the cache to its final position.
     */
    void addCacheStageAndOptimize(Pipeline& pipeline);
};

}  // namespace mongo
