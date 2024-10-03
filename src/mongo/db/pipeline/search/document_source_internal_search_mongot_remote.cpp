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

#include "mongo/db/pipeline/search/document_source_internal_search_mongot_remote.h"

#include "mongo/db/curop.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/pipeline/document_source_limit.h"
#include "mongo/db/pipeline/document_source_single_document_transformation.h"
#include "mongo/db/pipeline/document_source_skip.h"
#include "mongo/db/pipeline/search/document_source_internal_search_id_lookup.h"
#include "mongo/db/pipeline/search/lite_parsed_search.h"
#include "mongo/db/pipeline/search/search_helper.h"
#include "mongo/db/pipeline/skip_and_limit.h"
#include "mongo/db/query/search/mongot_cursor.h"
#include "mongo/db/query/search/mongot_cursor_getmore_strategy.h"
#include "mongo/db/query/search/mongot_options.h"
#include "mongo/db/query/search/search_task_executors.h"
#include "mongo/db/service_context.h"
#include "mongo/executor/remote_command_request.h"
#include "mongo/executor/task_executor_cursor.h"
#include "mongo/logv2/log.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/transport/transport_layer.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery

namespace mongo {
MONGO_FAIL_POINT_DEFINE(failClassicSearch);

using executor::TaskExecutorCursor;

DocumentSourceInternalSearchMongotRemote::DocumentSourceInternalSearchMongotRemote(
    InternalSearchMongotRemoteSpec spec,
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    std::shared_ptr<executor::TaskExecutor> taskExecutor)
    : DocumentSource(kStageName, expCtx),
      _mergingPipeline(spec.getMergingPipeline().has_value()
                           ? mongo::Pipeline::parse(*spec.getMergingPipeline(), expCtx)
                           : nullptr),
      _spec(std::move(spec)),
      _taskExecutor(taskExecutor) {
    LOGV2_DEBUG(9497006,
                5,
                "Creating DocumentSourceInternalSearchMongotRemote",
                "spec"_attr = _spec.toBSON());
    if (_spec.getSortSpec().has_value()) {
        _sortKeyGen.emplace(SortPattern{*_spec.getSortSpec(), pExpCtx}, pExpCtx->getCollator());
    }
}

const char* DocumentSourceInternalSearchMongotRemote::getSourceName() const {
    return kStageName.rawData();
}

Value DocumentSourceInternalSearchMongotRemote::addMergePipelineIfNeeded(
    Value innerSpecVal, const SerializationOptions& opts) const {
    if (!innerSpecVal.isObject()) {
        // We've redacted the interesting parts of the stage, return early.
        return innerSpecVal;
    }
    if ((!opts.verbosity || pExpCtx->inRouter) &&
        _spec.getMetadataMergeProtocolVersion().has_value() && _mergingPipeline) {
        MutableDocument innerSpec{innerSpecVal.getDocument()};
        innerSpec[InternalSearchMongotRemoteSpec::kMergingPipelineFieldName] =
            Value(_mergingPipeline->serialize(opts));
        return innerSpec.freezeToValue();
    }
    return innerSpecVal;
}

Value DocumentSourceInternalSearchMongotRemote::serializeWithoutMergePipeline(
    const SerializationOptions& opts) const {
    // Though router can generate explain output, it should never make a remote call to the mongot.
    if (!opts.verbosity || pExpCtx->inRouter) {
        if (_spec.getMetadataMergeProtocolVersion().has_value()) {
            // TODO SERVER-90941 The IDL should be able to handle this serialization once we
            // populate the query_shape field.
            MutableDocument spec;
            spec.addField(InternalSearchMongotRemoteSpec::kMongotQueryFieldName,
                          opts.serializeLiteral(_spec.getMongotQuery()));
            spec.addField(InternalSearchMongotRemoteSpec::kMetadataMergeProtocolVersionFieldName,
                          opts.serializeLiteral(*_spec.getMetadataMergeProtocolVersion()));
            // In a non-sharded scenario we don't need to pass the limit around as the limit stage
            // will do equivalent work. In a sharded scenario we want the limit to get to the
            // shards, so we serialize it. We serialize it in this block as all sharded search
            // queries have a protocol version.
            // This is the limit that we copied, and does not replace the real limit stage later in
            // the pipeline.
            spec.addField(InternalSearchMongotRemoteSpec::kLimitFieldName,
                          opts.serializeLiteral((long long)_spec.getLimit().get_value_or(0)));
            if (_spec.getSortSpec().has_value()) {
                spec.addField(InternalSearchMongotRemoteSpec::kSortSpecFieldName,
                              opts.serializeLiteral(*_spec.getSortSpec()));
            }
            spec.addField(InternalSearchMongotRemoteSpec::kRequiresSearchMetaCursorFieldName,
                          opts.serializeLiteral(_spec.getRequiresSearchMetaCursor()));
            return spec.freezeToValue();
        } else {
            // mongod/mongos don't know how to read a search query, so we can't redact the correct
            // field paths and literals. Treat the entire query as a literal. We don't know what
            // operators were used in the search query, so generate an entirely redacted document.
            return opts.serializeLiteral(_spec.getMongotQuery());
        }
    }
    // If the query is an explain that executed the query, we obtain the explain object from the
    // taskExecutorCursor. Otherwise, we need to obtain the explain
    // object now. We also obtain the getMoreStrategy which contains the batchSizeHistory.
    boost::optional<BSONObj> explainResponse = boost::none;
    std::shared_ptr<executor::MongotTaskExecutorCursorGetMoreStrategy> mongotGetMoreStrategy =
        nullptr;
    if (_cursor) {
        explainResponse = _cursor->getCursorExplain();
        mongotGetMoreStrategy =
            dynamic_pointer_cast<executor::MongotTaskExecutorCursorGetMoreStrategy>(
                _cursor->getOptions().getMoreStrategy);
    }

    BSONObj explainInfo = explainResponse.value_or_eval([&] {
        return mongot_cursor::getSearchExplainResponse(
            pExpCtx.get(), _spec.getMongotQuery(), _taskExecutor.get());
    });

    MutableDocument mDoc;
    mDoc.addField(InternalSearchMongotRemoteSpec::kMongotQueryFieldName,
                  opts.serializeLiteral(_spec.getMongotQuery()));
    // We should not need to redact when explaining, but treat it as a literal just in case.
    mDoc.addField("explain", opts.serializeLiteral(explainInfo));
    // Limit is relevant for explain.
    if (_spec.getLimit().has_value() && *_spec.getLimit() != 0) {
        mDoc.addField(InternalSearchMongotRemoteSpec::kLimitFieldName,
                      opts.serializeLiteral((long long)*_spec.getLimit()));
    }
    if (_spec.getSortSpec().has_value()) {
        mDoc.addField(InternalSearchMongotRemoteSpec::kSortSpecFieldName,
                      opts.serializeLiteral(*_spec.getSortSpec()));
    }
    if (_spec.getMongotDocsRequested().has_value()) {
        mDoc.addField(InternalSearchMongotRemoteSpec::kMongotDocsRequestedFieldName,
                      opts.serializeLiteral((long long)*_spec.getMongotDocsRequested()));
    }
    mDoc.addField(InternalSearchMongotRemoteSpec::kRequiresSearchMetaCursorFieldName,
                  opts.serializeLiteral(_spec.getRequiresSearchMetaCursor()));

    if (mongotGetMoreStrategy) {
        if (const auto& batchSizeHistory = mongotGetMoreStrategy->getBatchSizeHistory();
            batchSizeHistory.size() > 0) {
            std::vector<Value> serializedBatchSizeHistory(batchSizeHistory.size());
            std::transform(
                batchSizeHistory.begin(),
                batchSizeHistory.end(),
                serializedBatchSizeHistory.begin(),
                [&](auto batchSize) -> Value { return Value(opts.serializeLiteral(batchSize)); });
            mDoc.addField("internalMongotBatchSizeHistory",
                          Value(std::move(serializedBatchSizeHistory)));
        }
    }
    return mDoc.freezeToValue();
}

Value DocumentSourceInternalSearchMongotRemote::serialize(const SerializationOptions& opts) const {
    auto innerSpecVal = serializeWithoutMergePipeline(opts);
    return Value(
        Document{{getSourceName(), addMergePipelineIfNeeded(std::move(innerSpecVal), opts)}});
}

boost::optional<BSONObj> DocumentSourceInternalSearchMongotRemote::_getNext() {
    try {
        return _cursor->getNext(pExpCtx->opCtx);
    } catch (DBException& ex) {
        ex.addContext("Remote error from mongot");
        throw;
    }
}

bool DocumentSourceInternalSearchMongotRemote::shouldReturnEOF() {
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

    // Return EOF if pExpCtx->uuid is unset here; the collection we are searching over has not been
    // created yet.
    if (!pExpCtx->uuid) {
        LOGV2_DEBUG(8569402, 4, "Returning EOF due to lack of UUID");
        return true;
    }

    if (pExpCtx->explain &&
        !feature_flags::gFeatureFlagSearchExplainExecutionStats.isEnabled(
            serverGlobalParams.featureCompatibility.acquireFCVSnapshot())) {
        return true;
    }

    return false;
}

void DocumentSourceInternalSearchMongotRemote::tryToSetSearchMetaVar() {
    // Meta variables will be constant across the query and only need to be set once.
    // This feature was backported and is available on versions after 4.4. Therefore there is no
    // need to check FCV, as downgrading should not cause any issues.
    if (!pExpCtx->variables.hasConstantValue(Variables::kSearchMetaId) && _cursor &&
        _cursor->getCursorVars()) {
        // Variables on the cursor must be an object.
        auto varsObj = Value(_cursor->getCursorVars().value());
        LOGV2_DEBUG(8569400, 4, "Setting meta vars", "varsObj"_attr = varsObj);
        auto metaVal = varsObj.getDocument().getField(
            Variables::getBuiltinVariableName(Variables::kSearchMetaId));
        if (!metaVal.missing()) {
            pExpCtx->variables.setReservedValue(Variables::kSearchMetaId, metaVal, true);
            if (metaVal.isObject()) {
                auto metaValDoc = metaVal.getDocument();
                if (!metaValDoc.getField("count").missing()) {
                    auto& opDebug = CurOp::get(pExpCtx->opCtx)->debug();
                    opDebug.mongotCountVal = metaValDoc.getField("count").wrap("count");
                }

                if (!metaValDoc.getField(mongot_cursor::kSlowQueryLogFieldName).missing()) {
                    auto& opDebug = CurOp::get(pExpCtx->opCtx)->debug();
                    opDebug.mongotSlowQueryLog =
                        metaValDoc.getField(mongot_cursor::kSlowQueryLogFieldName)
                            .wrap(mongot_cursor::kSlowQueryLogFieldName);
                }
            }
        }
    }
}

DocumentSource::GetNextResult DocumentSourceInternalSearchMongotRemote::getNextAfterSetup() {
    auto response = _getNext();
    LOGV2_DEBUG(8569401, 5, "getting next after setup", "response"_attr = response);
    auto& opDebug = CurOp::get(pExpCtx->opCtx)->debug();

    if (opDebug.msWaitingForMongot) {
        *opDebug.msWaitingForMongot += durationCount<Milliseconds>(_cursor->resetWaitingTime());
    } else {
        opDebug.msWaitingForMongot = durationCount<Milliseconds>(_cursor->resetWaitingTime());
    }
    opDebug.mongotBatchNum = _cursor->getBatchNum();

    // The TaskExecutorCursor will store '0' as its CursorId if the cursor to mongot is exhausted.
    // If we already have a cursorId from a previous call, just use that.
    if (!_cursorId) {
        _cursorId = _cursor->getCursorId();
    }

    opDebug.mongotCursorId = _cursorId;

    if (!response) {
        return DocumentSource::GetNextResult::makeEOF();
    }

    ++_docsReturned;
    // Populate $sortKey metadata field so that mongos can properly merge sort the document stream.
    if (pExpCtx->needsMerge) {
        // Metadata can't be changed on a Document. Create a MutableDocument to set the sortKey.
        MutableDocument output(Document::fromBsonWithMetaData(response.value()));

        // If we have a sortSpec, then we use that to set sortKey. Otherwise, use the 'searchScore'
        // if the document has one.
        if (_spec.getSortSpec().has_value()) {
            tassert(7320402,
                    "_sortKeyGen must be initialized if _sortSpec is present",
                    _sortKeyGen.has_value());
            auto sortKey = _sortKeyGen->computeSortKeyFromDocument(Document(*response));
            output.metadata().setSortKey(sortKey, _sortKeyGen->isSingleElementKey());
        } else if (output.metadata().hasSearchScore()) {
            // If this stage is getting metadata documents from mongot, those don't include
            // searchScore.
            output.metadata().setSortKey(Value{output.metadata().getSearchScore()},
                                         true /* isSingleElementKey */);
        }
        return output.freeze();
    }
    return Document::fromBsonWithMetaData(response.value());
}

std::unique_ptr<executor::TaskExecutorCursor>
DocumentSourceInternalSearchMongotRemote::establishCursor() {
    // TODO SERVER-94874 We should be able to remove any cursor establishment logic from
    // DocumentSourceInternalSearchMongotRemote if we establish the cursors during search_helper
    // pipeline preparation instead.
    auto cursors = mongot_cursor::establishCursorsForSearchStage(
        pExpCtx, _spec, _taskExecutor, boost::none, nullptr, getSearchIdLookupMetrics());
    // Should be called only in unsharded scenario, therefore only expect a results cursor and no
    // metadata cursor.
    tassert(5253301, "Expected exactly one cursor from mongot", cursors.size() == 1);
    return std::move(cursors[0]);
}

DocumentSource::GetNextResult DocumentSourceInternalSearchMongotRemote::doGetNext() {
    if (shouldReturnEOF()) {
        LOGV2_DEBUG(8569404, 4, "Returning EOF from $internalSearchMongotRemote");
        return DocumentSource::GetNextResult::makeEOF();
    }

    // If the collection is sharded we should have a cursor already. Otherwise establish it now.
    if (!_cursor && !_dispatchedQuery) {
        LOGV2_DEBUG(8569403, 4, "Establishing Cursor");
        _cursor = establishCursor();
        _dispatchedQuery = true;
    }
    tryToSetSearchMetaVar();

    return getNextAfterSetup();
}
}  // namespace mongo
