// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonobj.h"
#include "mongo/db/pipeline/search/search_helper.h"
#include "mongo/util/modules.h"

namespace mongo::search_helpers {
/**
 * Run the given vector search request against mongot and build a cursor object for the cursor
 * returned from mongot.
 */
std::unique_ptr<executor::TaskExecutorCursor> establishVectorSearchCursor(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    const BSONObj& request,
    std::shared_ptr<executor::TaskExecutor> taskExecutor);

/**
 * Wrapper function to run getExplainResponse with vectorSearch command.
 */
BSONObj getVectorSearchExplainResponse(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                       const BSONObj& request,
                                       executor::TaskExecutor* taskExecutor);

/**
 * Returns true if it removed a $sort stage from container that sorts by "vector search score",
 * false otherwise.
 */
bool findAndRemoveSortStage(DocumentSourceContainer::iterator idLookupReplaceRootItr,
                            DocumentSourceContainer* container);

/**
 * Expects to find either an idLookup or replaceRoot stage and return true if it does. Asserts if it
 * finds neither.
 */
bool findIdLookupOrReplaceRootStage(DocumentSource* currStage);

/**
 * Applies a pipeline optimization for vector search stages by removing redundant sort stages.
 *
 * When a vector search stage produces results already sorted by 'vectorSearchScore', any
 * subsequent $sort stage that also sorts by 'vectorSearchScore' is redundant and can be
 * safely removed.
 *
 * This optimization applies when:
 * - The $sort stage comes directly after the idLookup/replaceRoot stage that follows vector search.
 * Special checks were added for idLookup and replaceRoot stages because vectorSearch is expected to
 * desugar into either one of those stages for the second stage in its desugared pipeline.
 * - The $sort stage sorts only by 'vectorSearchScore' metadata
 *
 * @param itr Iterator pointing to the vector search stage in the container
 * @param container The document source container being optimized
 * @return the Iterator pointing to the vector search stage in the container if the optimization was
 * applied, nothing otherwise
 */
boost::optional<DocumentSourceContainer::iterator> applyVectorSearchSortOptimization(
    DocumentSourceContainer::iterator itr, DocumentSourceContainer* container);

/**
 * Extracts and calculates the user limit for vector search optimizations.
 *
 * @param itr Iterator pointing to the vector search stage in the container
 * @param container The document source container being optimized
 * @param currentLimit The current limit value (if any) to be compared with the extracted limit
 * @return the extracted limit if any, nothing otherwise
 */
boost::optional<long long> setVectorSearchLimitForOptimization(
    DocumentSourceContainer::iterator itr,
    DocumentSourceContainer* container,
    boost::optional<long long> currentLimit);
}  // namespace mongo::search_helpers
