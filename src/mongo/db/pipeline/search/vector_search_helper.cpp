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
#include "mongo/db/pipeline/search/vector_search_helper.h"

#include "mongo/db/pipeline/search/search_helper.h"
#include "mongo/db/query/search/mongot_cursor.h"

namespace mongo {
namespace {
executor::RemoteCommandRequest getRemoteCommandRequestForVectorSearchQuery(
    const boost::intrusive_ptr<ExpressionContext>& expCtx, const BSONObj& request) {
    BSONObjBuilder cmdBob;
    cmdBob.append(mongot_cursor::kVectorSearchCmd, expCtx->getNamespaceString().coll());
    uassert(7828001,
            str::stream()
                << "A uuid is required for a vector search query, but was missing. Got namespace "
                << expCtx->getNamespaceString().toStringForErrorMsg(),
            expCtx->getUUID());
    expCtx->getUUID().value().appendToBuilder(&cmdBob, mongot_cursor::kCollectionUuidField);
    if (expCtx->getExplain()) {
        cmdBob.append("explain",
                      BSON("verbosity" << ExplainOptions::verbosityString(*expCtx->getExplain())));
    }

    // Attempt to get the view from the request.
    boost::optional<SearchQueryViewSpec> view = search_helpers::getViewFromBSONObj(request);
    if (view) {
        // mongot only expects the view's name but request currently holds the entire view object.
        // Set the view name and remove the view object from the request.
        cmdBob.append(mongot_cursor::kViewNameField, view->getName());
        request.removeField(search_helpers::kViewFieldName);
    }

    auto commandObj = cmdBob.obj();

    // Copy over all fields from the original object for passthrough.
    return mongot_cursor::getRemoteCommandRequest(
        expCtx->getOperationContext(), expCtx->getNamespaceString(), commandObj.addFields(request));
}
}  // namespace

namespace search_helpers {
std::unique_ptr<executor::TaskExecutorCursor> establishVectorSearchCursor(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    const BSONObj& request,
    std::shared_ptr<executor::TaskExecutor> taskExecutor) {
    // Note that we always pre-fetch the next batch here. This is because we generally expect
    // everything to fit into one batch, since we give mongot the exact upper bound initially - we
    // will only see multiple batches if this upper bound doesn't fit in 16MB. This should be a rare
    // enough case that it shouldn't overwhelm mongot to pre-fetch.
    auto getMoreStrategy = std::make_unique<executor::DefaultTaskExecutorCursorGetMoreStrategy>(
        /*batchSize*/ boost::none,
        /*preFetchNextBatch*/ true);
    auto cursors = mongot_cursor::establishCursors(
        expCtx,
        getRemoteCommandRequestForVectorSearchQuery(expCtx, request),
        taskExecutor,
        std::move(getMoreStrategy));
    // Should always have one results cursor.
    tassert(7828000, "Expected exactly one cursor from mongot", cursors.size() == 1);
    return std::move(cursors.front());
}

BSONObj getVectorSearchExplainResponse(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                       const BSONObj& spec,
                                       executor::TaskExecutor* taskExecutor) {
    auto request = getRemoteCommandRequestForVectorSearchQuery(expCtx, spec);
    return mongot_cursor::getExplainResponse(expCtx.get(), request, taskExecutor);
}
}  // namespace search_helpers
}  // namespace mongo
