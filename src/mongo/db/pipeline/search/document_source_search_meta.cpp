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

#include "mongo/db/pipeline/search/document_source_search_meta.h"

#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/pipeline.h"
#include "mongo/db/pipeline/search/document_source_internal_search_mongot_remote.h"
#include "mongo/db/pipeline/search/lite_parsed_search.h"
#include "mongo/db/pipeline/search/search_helper.h"
#include "mongo/db/query/client_cursor/cursor_response_gen.h"
#include "mongo/db/query/search/mongot_cursor.h"
#include "mongo/db/query/search/search_task_executors.h"

#include <boost/optional/optional.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery

namespace mongo {

using boost::intrusive_ptr;
using std::list;

namespace {
/**
 * Helper to go through the list and clone each stage. Very similar to Pipeline::clone, but doesn't
 * necessitate use of the Pipeline type, which has auto-dispose behaviors which can cause problems.
 */
auto cloneEachOne(std::list<boost::intrusive_ptr<DocumentSource>> stages, const auto& expCtx) {
    std::list<boost::intrusive_ptr<DocumentSource>> cloned;
    for (const auto& stage : stages) {
        cloned.push_back(stage->clone(expCtx));
    }
    return cloned;
}
}  // namespace

REGISTER_DOCUMENT_SOURCE_CONDITIONALLY(searchMeta,
                                       LiteParsedSearchStage::parse,
                                       DocumentSourceSearchMeta::createFromBson,
                                       AllowedWithApiStrict::kNeverInVersion1,
                                       AllowedWithClientType::kAny,
                                       nullptr,  // featureFlag
                                       true);
ALLOCATE_DOCUMENT_SOURCE_ID(searchMeta, DocumentSourceSearchMeta::id)

boost::optional<DocumentSource::DistributedPlanLogic>
DocumentSourceSearchMeta::distributedPlanLogic() {
    DistributedPlanLogic logic;
    logic.shardsStage = this;
    tassert(6448011, "Expected merging pipeline to be set already", _mergingPipeline);
    // Please note that it's important for the merging stages to be copied so that we don't share a
    // pointer to them. If we share a pointer, it can lead to a bug where this $searchMeta stage is
    // serialized and sent to a remote shard, which causes "_mergingPipeline" to go out of scope and
    // dispose() each stage. That screws up the other pointers to these stages who now have disposed
    // DocumentSources which are expected to immediately return EOF.
    logic.mergingStages = cloneEachOne(_mergingPipeline->getSources(), pExpCtx);
    return logic;
}

std::unique_ptr<executor::TaskExecutorCursor> DocumentSourceSearchMeta::establishCursor() {
    if (_view) {
        // This function will throw if the view violates validation rules for supporting
        // mongot-indexed views.
        search_helpers::validateViewPipeline(*_view);
    }

    // TODO SERVER-94875 We should be able to remove any cursor establishment logic from
    // DocumentSourceSearchMeta if we establish the cursors during search_helper
    // pipeline preparation instead.
    auto cursors = mongot_cursor::establishCursorsForSearchMetaStage(
        pExpCtx,
        getSearchQuery(),
        getTaskExecutor(),
        getIntermediateResultsProtocolVersion(),
        nullptr,
        _view ? boost::make_optional(_view->getNss()) : boost::none);

    // TODO SERVER-91594: Since mongot will no longer only return explain, remove this block.
    // Mongot can return only an explain object or an explain with a cursor. If mongot returned
    // the explain object only, the cursor will not have attached vars. Since there's a
    // possibility of not having vars for explain, we skip the check.
    if (pExpCtx->getExplain() && cursors.size() == 1) {
        return std::move(*cursors.begin());
    }
    if (cursors.size() == 1) {
        const auto& cursor = *cursors.begin();
        tassert(6448010,
                "If there's one cursor we expect to get SEARCH_META from the attached vars",
                !getIntermediateResultsProtocolVersion() && !cursor->getType() &&
                    cursor->getCursorVars());
        return std::move(*cursors.begin());
    }
    for (auto&& cursor : cursors) {
        auto maybeCursorType = cursor->getType();
        tassert(6448008, "Expected every mongot cursor to come back with a type", maybeCursorType);
        if (*maybeCursorType == CursorTypeEnum::SearchMetaResult) {
            // Note this may leak the other cursor(s). Should look into whether we can
            // killCursors.
            return std::move(cursor);
        }
    }
    tasserted(6448009, "Expected to get a metadata cursor back from mongot, found none");
}

DocumentSource::GetNextResult DocumentSourceSearchMeta::getNextAfterSetup() {
    if (pExpCtx->getNeedsMerge()) {
        // When we are merging $searchMeta we have established a cursor which only returns metadata
        // results (see 'establishCursor()'). So just iterate that cursor normally.
        return DocumentSourceInternalSearchMongotRemote::getNextAfterSetup();
    }

    if (!_returnedAlready) {
        tryToSetSearchMetaVar();
        auto& vars = pExpCtx->variables;

        // TODO SERVER-91594: Remove this explain specific block.
        // If mongot only returns an explain object, it will not have any attached vars and we
        // should return EOF.
        if (pExpCtx->getExplain() && !vars.hasConstantValue(Variables::kSearchMetaId)) {
            return GetNextResult::makeEOF();
        }
        tassert(6448005,
                "Expected SEARCH_META to be set for $searchMeta stage",
                vars.hasConstantValue(Variables::kSearchMetaId) &&
                    vars.getValue(Variables::kSearchMetaId).isObject());
        _returnedAlready = true;
        return {vars.getValue(Variables::kSearchMetaId).getDocument()};
    }
    return GetNextResult::makeEOF();
}

namespace {
InternalSearchMongotRemoteSpec prepareInternalSearchMetaMongotSpec(
    const BSONObj& spec, const intrusive_ptr<ExpressionContext>& expCtx) {
    if (spec.hasField(InternalSearchMongotRemoteSpec::kMongotQueryFieldName)) {
        // The existence of this field name indicates that this spec was already serialized from a
        // mongos process. Parse out of the IDL spec format, rather than just expecting only the
        // mongot query (as a user would provide).
        auto params = InternalSearchMongotRemoteSpec::parseOwned(
            IDLParserContext(DocumentSourceSearchMeta::kStageName), spec.getOwned());
        LOGV2_DEBUG(8569405,
                    4,
                    "Parsing as $internalSearchMongotRemote",
                    "params"_attr = redact(params.toBSON()));
        return params;
    }

    // See note in DocumentSourceSearch::createFromBson() about making sure mongotQuery is owned
    // within the mongot remote spec.
    InternalSearchMongotRemoteSpec internalSpec(spec.getOwned());

    if (expCtx->getIsParsingViewDefinition()) {
        // $searchMeta is possible to be parsed from the user visible stage.  In this case, just
        // return the mongot query itself parsed into IDL.
        return internalSpec;
    }

    uassert(6600901,
            "Running $searchMeta command in non-allowed context (update pipeline)",
            !expCtx->getIsParsingPipelineUpdate());

    // If 'searchReturnEofImmediately' is set, we return this stage as is because we don't expect to
    // return any results. More precisely, we wish to avoid calling 'planShardedSearch' when no
    // mongot is set up.
    if (expCtx->getMongoProcessInterface()->isExpectedToExecuteQueries() &&
        expCtx->getMongoProcessInterface()->inShardedEnvironment(expCtx->getOperationContext()) &&
        !MONGO_unlikely(searchReturnEofImmediately.shouldFail())) {
        // This query is executing in a sharded environment. We need to consult a mongot to
        // construct such a merging pipeline for us to use later. Send a planShardedSearch command
        // to mongot to get the relevant planning information, including the metadata merging
        // pipeline and the optional merge sort spec.
        search_helpers::planShardedSearch(expCtx, &internalSpec);
    } else {
        // This is an unsharded environment or there is no mongot. If the case is the former, this
        // is only called from user pipelines during desugaring of $search/$searchMeta, so the
        // `specObj` should be the search query itself. If 'searchReturnEofImmediately' is set, we
        // return this stage as is because we don't expect to return any results. More precisely, we
        // wish to avoid calling 'planShardedSearch' when no mongot is set up.
    }

    return internalSpec;
}
}  // namespace

std::list<intrusive_ptr<DocumentSource>> DocumentSourceSearchMeta::createFromBson(
    BSONElement elem, const intrusive_ptr<ExpressionContext>& expCtx) {
    mongot_cursor::throwIfNotRunningWithMongotHostConfigured(expCtx);

    uassert(ErrorCodes::FailedToParse,
            str::stream() << "$searchMeta value must be an object. Found: "
                          << typeName(elem.type()),
            elem.type() == BSONType::object);

    auto specObj = elem.embeddedObject();
    auto view = search_helpers::getViewFromBSONObj(expCtx, specObj);
    auto executor =
        executor::getMongotTaskExecutor(expCtx->getOperationContext()->getServiceContext());
    auto internalRemoteSpec = prepareInternalSearchMetaMongotSpec(specObj, expCtx);

    return {make_intrusive<DocumentSourceSearchMeta>(
        std::move(internalRemoteSpec), expCtx, executor, view)};
}

}  // namespace mongo
