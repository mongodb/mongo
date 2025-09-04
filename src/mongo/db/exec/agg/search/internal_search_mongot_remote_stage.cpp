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

#include "mongo/db/exec/agg/search/internal_search_mongot_remote_stage.h"

#include "mongo/db/exec/agg/document_source_to_stage_registry.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/pipeline/search/document_source_internal_search_mongot_remote.h"
#include "mongo/db/pipeline/search/search_helper.h"
#include "mongo/db/query/search/mongot_cursor.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery

namespace mongo {

namespace {
auto withSortKeyMetadata(auto&& resultBson, const auto& stageSpec, const auto& sortKeyGen) {
    // Metadata can't be changed on a Document. Create a MutableDocument to set the sortKey.
    MutableDocument output(Document::fromBsonWithMetaData(std::move(resultBson)));

    // If we have a sortSpec, then we use that to set sortKey. Otherwise, use the 'searchScore'
    // if the document has one.
    if (stageSpec.getSortSpec().has_value()) {
        tassert(7320402,
                "_sortKeyGen must be initialized if _sortSpec is present",
                sortKeyGen.has_value());
        auto sortKey = sortKeyGen->computeSortKeyFromDocument(output.peek());
        output.metadata().setSortKey(sortKey, sortKeyGen->isSingleElementKey());
    } else if (output.metadata().hasSearchScore()) {
        // If this stage is getting metadata documents from mongot, those don't include
        // searchScore.
        output.metadata().setSortKey(Value{output.metadata().getSearchScore()},
                                     true /* isSingleElementKey */);
    }
    return output.freeze();
}
}  // namespace

boost::intrusive_ptr<exec::agg::Stage> documentSourceInternalSearchMongotRemoteToStageFn(
    const boost::intrusive_ptr<DocumentSource>& source) {
    auto documentSource = dynamic_cast<DocumentSourceInternalSearchMongotRemote*>(source.get());

    tassert(10807803, "expected 'DocumentSourceInternalSearchMongotRemote' type", documentSource);

    return make_intrusive<exec::agg::InternalSearchMongotRemoteStage>(
        documentSource->kStageName,
        documentSource->_spec,
        documentSource->getExpCtx(),
        documentSource->_taskExecutor,
        documentSource->getSearchIdLookupMetrics(),
        documentSource->_sharedState);
}

MONGO_FAIL_POINT_DEFINE(failClassicSearch);

REGISTER_AGG_STAGE_MAPPING(internalSearchMongotRemoteStage,
                           DocumentSourceInternalSearchMongotRemote::id,
                           documentSourceInternalSearchMongotRemoteToStageFn);

namespace exec::agg {

InternalSearchMongotRemoteStage::InternalSearchMongotRemoteStage(
    StringData stageName,
    InternalSearchMongotRemoteSpec spec,
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    const std::shared_ptr<executor::TaskExecutor>& taskExecutor,
    const std::shared_ptr<SearchIdLookupMetrics>& searchIdLookupMetrics,
    const std::shared_ptr<InternalSearchMongotRemoteSharedState>& sharedState)
    : Stage(stageName, expCtx),
      _spec(std::move(spec)),
      _taskExecutor(taskExecutor),
      _searchIdLookupMetrics(searchIdLookupMetrics),
      _sharedState(sharedState) {
    if (_spec.getSortSpec().has_value()) {
        _sortKeyGen.emplace(SortPattern{*_spec.getSortSpec(), pExpCtx}, pExpCtx->getCollator());
    }
}

GetNextResult InternalSearchMongotRemoteStage::getNextAfterSetup() {
    auto response = _getNext();
    LOGV2_DEBUG(8569401,
                5,
                "getting next after setup",
                "response"_attr = response.map([](const BSONObj& b) { return redact(b); }));
    auto& opDebug = CurOp::get(pExpCtx->getOperationContext())->debug();

    if (opDebug.msWaitingForMongot) {
        *opDebug.msWaitingForMongot +=
            durationCount<Milliseconds>(_sharedState->_cursor->resetWaitingTime());
    } else {
        opDebug.msWaitingForMongot =
            durationCount<Milliseconds>(_sharedState->_cursor->resetWaitingTime());
    }
    opDebug.mongotBatchNum = _sharedState->_cursor->getBatchNum();

    // The TaskExecutorCursor will store '0' as its CursorId if the cursor to mongot is exhausted.
    //  If we already have a cursorId from a previous call, just use that.
    if (!_cursorId) {
        _cursorId = _sharedState->_cursor->getCursorId();
    }

    opDebug.mongotCursorId = _cursorId;

    if (!response) {
        return GetNextResult::makeEOF();
    }

    ++_docsReturned;
    // Populate $sortKey metadata field so that downstream operators can correctly reason about the
    // sort order. This can be important for mongos, so it can properly merge sort the document
    // stream, or for $rankFusion to calculate the ranks of the results. This metadata can be safely
    // ignored if nobody ends up needing it. It will be stripped out before a response is sent back
    // to a client.
    return withSortKeyMetadata(std::move(response.value()), _spec, _sortKeyGen);
}

std::unique_ptr<executor::TaskExecutorCursor> InternalSearchMongotRemoteStage::establishCursor() {
    // TODO SERVER-94874 We should be able to remove any cursor establishment logic from
    // InternalSearchMongotRemoteStage if we establish the cursors during search_helper
    // pipeline preparation instead.
    auto cursors = mongot_cursor::establishCursorsForSearchStage(
        pExpCtx, _spec, _taskExecutor, boost::none, nullptr, _searchIdLookupMetrics);
    // Should be called only in unsharded scenario, therefore only expect a results cursor and no
    // metadata cursor.
    tassert(5253301, "Expected exactly one cursor from mongot", cursors.size() == 1);
    return std::move(cursors[0]);
}

void InternalSearchMongotRemoteStage::tryToSetSearchMetaVar() {
    // Meta variables will be constant across the query and only need to be set once.
    // This feature was backported and is available on versions after 4.4. Therefore there is no
    // need to check FCV, as downgrading should not cause any issues.
    if (!pExpCtx->variables.hasConstantValue(Variables::kSearchMetaId) && _sharedState->_cursor &&
        _sharedState->_cursor->getCursorVars()) {
        // Variables on the cursor must be an object.
        auto varsObj = Value(_sharedState->_cursor->getCursorVars().value());
        LOGV2_DEBUG(8569400, 4, "Setting meta vars", "varsObj"_attr = redact(varsObj.toString()));
        std::string varName = Variables::getBuiltinVariableName(Variables::kSearchMetaId);
        auto metaVal = varsObj.getDocument().getField(StringData{varName});
        if (!metaVal.missing()) {
            pExpCtx->variables.setReservedValue(Variables::kSearchMetaId, metaVal, true);
            if (metaVal.isObject()) {
                auto metaValDoc = metaVal.getDocument();
                if (!metaValDoc.getField("count").missing()) {
                    auto& opDebug = CurOp::get(pExpCtx->getOperationContext())->debug();
                    opDebug.mongotCountVal = metaValDoc.getField("count").wrap("count");
                }

                if (!metaValDoc.getField(mongot_cursor::kSlowQueryLogFieldName).missing()) {
                    auto& opDebug = CurOp::get(pExpCtx->getOperationContext())->debug();
                    opDebug.mongotSlowQueryLog =
                        metaValDoc.getField(mongot_cursor::kSlowQueryLogFieldName)
                            .wrap(mongot_cursor::kSlowQueryLogFieldName);
                }
            }
        }
    }
}

GetNextResult InternalSearchMongotRemoteStage::doGetNext() {
    if (shouldReturnEOF()) {
        LOGV2_DEBUG(8569404, 4, "Returning EOF from $internalSearchMongotRemote");
        return DocumentSource::GetNextResult::makeEOF();
    }

    // If the collection is sharded we should have a cursor already. Otherwise establish it now.
    if (!_sharedState->_cursor) {
        LOGV2_DEBUG(8569403, 4, "Establishing Cursor");
        _sharedState->_cursor = establishCursor();
        tassert(
            10912600, "Expected to have cursor after cursor establishment", _sharedState->_cursor);
    }
    tryToSetSearchMetaVar();

    return getNextAfterSetup();
}

boost::optional<BSONObj> InternalSearchMongotRemoteStage::_getNext() {
    try {
        return _sharedState->_cursor->getNext(pExpCtx->getOperationContext());
    } catch (DBException& ex) {
        ex.addContext("Remote error from mongot");
        throw;
    }
}

bool InternalSearchMongotRemoteStage::shouldReturnEOF() const {
    if (MONGO_unlikely(searchReturnEofImmediately.shouldFail())) {
        return true;
    }

    if (MONGO_unlikely(failClassicSearch.shouldFail())) {
        uasserted(7942401, "Fail because failClassicSearch is enabled");
    }

    if (_spec.getLimit().has_value() && *_spec.getLimit() != 0 &&
        _docsReturned >= *_spec.getLimit()) {
        return true;
    }

    // Return EOF if pExpCtx->getUUID() is unset here; the collection we are searching over has not
    // been created yet.
    if (!pExpCtx->getUUID()) {
        LOGV2_DEBUG(8569402, 4, "Returning EOF due to lack of UUID");
        return true;
    }

    if (pExpCtx->getExplain() &&
        !feature_flags::gFeatureFlagSearchExplainExecutionStats.isEnabled()) {
        return true;
    }

    return false;
}
}  // namespace exec::agg
}  // namespace mongo
