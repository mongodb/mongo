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

#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/document_source_single_document_transformation.h"
#include "mongo/db/pipeline/document_source_sort.h"
#include "mongo/db/pipeline/search/search_helper.h"
#include "mongo/db/query/search/mongot_cursor.h"
#include "mongo/db/transaction/internal_transaction_metrics.h"

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

bool findAndRemoveSortStage(DocumentSourceContainer::iterator idLookupReplaceRootItr,
                            DocumentSourceContainer* container) {
    auto isSortOnVectorSearchMeta = [](const SortPattern& sortPattern) -> bool {
        return isSortOnSingleMetaField(sortPattern,
                                       (1 << DocumentMetadataFields::MetaType::kVectorSearchScore));
    };

    auto sortItr = std::next(idLookupReplaceRootItr);
    if (sortItr == container->end()) {
        return false;
    }

    auto sortStage = dynamic_cast<DocumentSourceSort*>(sortItr->get());
    if (!sortStage) {
        return false;
    }

    // A $sort stage has been found directly after the desugared
    // vectorSearch pipeline. $vectorSearch results are always sorted by
    // 'vectorSearchScore', so if the $sort stage is also sorted by
    // 'vectorSearchScore', the $sort stage is redundant and can safely be
    // removed.
    if (!(isSortOnVectorSearchMeta(sortStage->getSortKeyPattern()))) {
        return false;
    }
    // Optimization successful.
    container->remove(*sortItr);
    return true;
}

bool findIdLookupOrReplaceRootStage(DocumentSource* currStage) {
    if (dynamic_cast<DocumentSourceInternalSearchIdLookUp*>(currStage)) {
        return true;
    }
    if (auto replaceRootStage =
            dynamic_cast<DocumentSourceSingleDocumentTransformation*>(currStage);
        replaceRootStage &&
        replaceRootStage->getTransformerType() ==
            TransformerInterface::TransformerType::kReplaceRoot) {
        return true;
    }
    return false;
}

boost::optional<DocumentSourceContainer::iterator> applyVectorSearchSortOptimization(
    DocumentSourceContainer::iterator itr, DocumentSourceContainer* container) {
    // Attempt to remove a $sort stage that sorts by 'vectorSearchScore' directly after the
    // $vectorSearch stage.
    if (findAndRemoveSortStage(itr, container)) {
        return itr;  // Return the same pointer in case there are other optimizations to still be
                     // applied.
    }

    auto nextStageItr =
        std::next(itr);  // The iterator `itr` is pointed at the $vectorSearch extension stage so
    // when it is advanced, the returned iterator will point at the next
    // stage (either an idLookup or replaceRoot stage).

    if (nextStageItr == container->end()) {
        return boost::none;
    }

    // Attempt to remove a $sort stage that sorts by 'vectorSearchScore' directly after the
    // desugared $vectorSearch stage.
    if (findIdLookupOrReplaceRootStage(nextStageItr->get()) &&
        findAndRemoveSortStage(nextStageItr, container)) {
        return itr;  // Return the same pointer in case there are other optimizations to still be
                     // applied.
    }

    return boost::none;
}
}  // namespace search_helpers
}  // namespace mongo
