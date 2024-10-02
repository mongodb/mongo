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
#include "mongo/db/query/cursor_response_gen.h"
#include "mongo/db/query/search/mongot_cursor.h"
#include "mongo/db/query/search/search_task_executors.h"

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
    std::for_each(stages.begin(), stages.end(), [&](const auto& stage) {
        cloned.push_back(stage->clone(expCtx));
    });
    return cloned;
}
}  // namespace

REGISTER_DOCUMENT_SOURCE_CONDITIONALLY(searchMeta,
                                       LiteParsedSearchStage::parse,
                                       DocumentSourceSearchMeta::createFromBson,
                                       AllowedWithApiStrict::kNeverInVersion1,
                                       AllowedWithClientType::kAny,
                                       boost::none,
                                       true);

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

executor::TaskExecutorCursor DocumentSourceSearchMeta::establishCursor() {
    auto cursors = mongot_cursor::establishCursorsForSearchMetaStage(
        pExpCtx, getSearchQuery(), getTaskExecutor(), getIntermediateResultsProtocolVersion());
    if (cursors.size() == 1) {
        const auto& cursor = *cursors.begin();
        tassert(6448010,
                "If there's one cursor we expect to get SEARCH_META from the attached vars",
                !getIntermediateResultsProtocolVersion() && !cursor.getType() &&
                    cursor.getCursorVars());
        return std::move(*cursors.begin());
    }
    for (auto&& cursor : cursors) {
        auto maybeCursorType = cursor.getType();
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
    if (pExpCtx->needsMerge) {
        // When we are merging $searchMeta we have established a cursor which only returns metadata
        // results (see 'establishCursor()'). So just iterate that cursor normally.
        return DocumentSourceInternalSearchMongotRemote::getNextAfterSetup();
    }

    if (!_returnedAlready) {
        tryToSetSearchMetaVar();
        auto& vars = pExpCtx->variables;
        tassert(6448005,
                "Expected SEARCH_META to be set for $searchMeta stage",
                vars.hasConstantValue(Variables::kSearchMetaId) &&
                    vars.getValue(Variables::kSearchMetaId).isObject());
        _returnedAlready = true;
        return {vars.getValue(Variables::kSearchMetaId).getDocument()};
    }
    return GetNextResult::makeEOF();
}

std::list<intrusive_ptr<DocumentSource>> DocumentSourceSearchMeta::createFromBson(
    BSONElement elem, const intrusive_ptr<ExpressionContext>& expCtx) {
    mongot_cursor::throwIfNotRunningWithMongotHostConfigured(expCtx);

    uassert(ErrorCodes::FailedToParse,
            str::stream() << "$searchMeta value must be an object. Found: "
                          << typeName(elem.type()),
            elem.type() == BSONType::Object);

    auto specObj = elem.embeddedObject();

    // Note that the $searchMeta stage has two parsing options: one for the user visible stage and
    // the second (longer) form which is serialized from mongos to the shards and includes more
    // information such as merging pipeline.

    // Avoid any calls to mongot during desugaring.
    if (expCtx->isParsingViewDefinition) {
        auto executor = executor::getMongotTaskExecutor(expCtx->opCtx->getServiceContext());
        return {make_intrusive<DocumentSourceSearchMeta>(specObj.getOwned(), expCtx, executor)};
    }

    // If we have this field it suggests we were serialized from a mongos process. We should parse
    // out of the IDL spec format, rather than just expecting only the mongot query (as a user would
    // provide).
    if (specObj.hasField(InternalSearchMongotRemoteSpec::kMongotQueryFieldName)) {
        auto params = InternalSearchMongotRemoteSpec::parse(IDLParserContext(kStageName), specObj);
        LOGV2_DEBUG(8569405, 4, "Parsing as $internalSearchMongotRemote", "params"_attr = params);
        auto executor = executor::getMongotTaskExecutor(expCtx->opCtx->getServiceContext());
        return {make_intrusive<DocumentSourceSearchMeta>(std::move(params), expCtx, executor)};
    }

    // Otherwise, we need to call this helper to determine if this is a sharded environment. If so,
    // we need to consult a mongot to construct such a merging pipeline for us to use later.
    return search_helpers::createInitialSearchPipeline<DocumentSourceSearchMeta>(specObj, expCtx);
}

}  // namespace mongo
