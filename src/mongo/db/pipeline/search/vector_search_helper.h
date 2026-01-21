/**
 *    Copyright (C) 2023-present MongoDB, Inc.
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
 * TODO SERVER-96068: Generalize this optimization to handle cases where stages that preserve
 * sort order come between the vector search stage and the $sort stage.
 *
 * @param itr Iterator pointing to the vector search stage in the container
 * @param container The document source container being optimized
 * @return the Iterator pointing to the vector search stage in the container if the optimization was
 * applied, nothing otherwise
 */
boost::optional<DocumentSourceContainer::iterator> applyVectorSearchSortOptimization(
    DocumentSourceContainer::iterator itr, DocumentSourceContainer* container);
}  // namespace mongo::search_helpers
