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


#include "mongo/db/exec/agg/search/search_meta_stage.h"

#include "mongo/db/exec/agg/document_source_to_stage_registry.h"
#include "mongo/db/pipeline/search/document_source_search_meta.h"

namespace mongo {

boost::intrusive_ptr<exec::agg::Stage> documentSourceSearchMetaToStageFn(
    const boost::intrusive_ptr<DocumentSource>& source) {
    auto documentSource = dynamic_cast<DocumentSourceSearchMeta*>(source.get());

    tassert(10807801, "expected 'DocumentSourceSearchMeta' type", documentSource);

    return make_intrusive<exec::agg::SearchMetaStage>(documentSource->kStageName,
                                                      documentSource->_spec,
                                                      documentSource->getExpCtx(),
                                                      documentSource->_taskExecutor,
                                                      documentSource->getSearchIdLookupMetrics(),
                                                      documentSource->_sharedState);
}

namespace exec::agg {

REGISTER_AGG_STAGE_MAPPING(searchMetaStage,
                           DocumentSourceSearchMeta::id,
                           documentSourceSearchMetaToStageFn);

GetNextResult SearchMetaStage::getNextAfterSetup() {
    if (pExpCtx->getNeedsMerge()) {
        // When we are merging $searchMeta we have established a cursor which only returns metadata
        // results (see 'establishCursor()'). So just iterate that cursor normally.
        return InternalSearchMongotRemoteStage::getNextAfterSetup();
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

std::unique_ptr<executor::TaskExecutorCursor> SearchMetaStage::establishCursor() {
    // TODO SERVER-94875 We should be able to remove any cursor establishment logic from
    // DocumentSourceSearchMeta if we establish the cursors during search_helper
    // pipeline preparation instead.
    auto cursors =
        mongot_cursor::establishCursorsForSearchMetaStage(pExpCtx,
                                                          getSearchQuery(),
                                                          getTaskExecutor(),
                                                          getIntermediateResultsProtocolVersion(),
                                                          nullptr,
                                                          getView());

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
}  // namespace exec::agg
}  // namespace mongo
