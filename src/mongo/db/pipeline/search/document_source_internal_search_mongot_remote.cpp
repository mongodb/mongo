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

#include <boost/none.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery

namespace mongo {

ALLOCATE_DOCUMENT_SOURCE_ID(_internalSearchMongotRemote,
                            DocumentSourceInternalSearchMongotRemote::id)

DocumentSourceInternalSearchMongotRemote::DocumentSourceInternalSearchMongotRemote(
    InternalSearchMongotRemoteSpec spec,
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    std::shared_ptr<executor::TaskExecutor> taskExecutor)
    : DocumentSource(kStageName, expCtx),
      _mergingPipeline(spec.getMergingPipeline().has_value()
                           ? mongo::Pipeline::parse(*spec.getMergingPipeline(), expCtx)
                           : nullptr),
      _sharedState(std::make_shared<InternalSearchMongotRemoteSharedState>()),
      _spec(std::move(spec)),
      _taskExecutor(taskExecutor) {
    LOGV2_DEBUG(9497006,
                5,
                "Creating DocumentSourceInternalSearchMongotRemote",
                "spec"_attr = redact(_spec.toBSON()));
}

const char* DocumentSourceInternalSearchMongotRemote::getSourceName() const {
    return kStageName.data();
}

Value DocumentSourceInternalSearchMongotRemote::addMergePipelineIfNeeded(
    Value innerSpecVal, const SerializationOptions& opts) const {
    if (!innerSpecVal.isObject()) {
        // We've redacted the interesting parts of the stage, return early.
        return innerSpecVal;
    }
    if ((!opts.isSerializingForExplain() || getExpCtx()->getInRouter()) &&
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
    if (!opts.isSerializingForExplain() || getExpCtx()->getInRouter()) {
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
    // TODO SERVER-107930: Remove '_sharedState' and the need of a cursor once
    // InternalSearchMongotRemoteStage::getExplainOutput() is implemented.
    boost::optional<BSONObj> explainResponse = boost::none;
    std::shared_ptr<executor::MongotTaskExecutorCursorGetMoreStrategy> mongotGetMoreStrategy =
        nullptr;
    if (_sharedState->_cursor) {
        explainResponse = _sharedState->_cursor->getCursorExplain();
        mongotGetMoreStrategy =
            dynamic_pointer_cast<executor::MongotTaskExecutorCursorGetMoreStrategy>(
                _sharedState->_cursor->getOptions().getMoreStrategy);
    }

    mongot_cursor::OptimizationFlags optFlags = search_helpers::isSearchMetaStage(this)
        ? mongot_cursor::getOptimizationFlagsForSearchMeta()
        : mongot_cursor::getOptimizationFlagsForSearch();

    BSONObj explainInfo = explainResponse.value_or_eval([&] {
        return mongot_cursor::getSearchExplainResponse(
            getExpCtx().get(), _spec.getMongotQuery(), _taskExecutor.get(), optFlags);
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

DepsTracker::State DocumentSourceInternalSearchMongotRemote::getDependencies(
    DepsTracker* deps) const {
    // This stage doesn't currently support tracking field dependencies since mongot is
    // responsible for determining what fields to return. We do need to track metadata
    // dependencies though, so downstream stages know they are allowed to access "searchScore"
    // metadata.
    // TODO SERVER-101100 Implement logic for dependency analysis.

    deps->setMetadataAvailable(DocumentMetadataFields::kSearchScore);
    if (hasScoreDetails()) {
        deps->setMetadataAvailable(DocumentMetadataFields::kSearchScoreDetails);
    }

    if (hasSearchRootDocumentId()) {
        deps->setMetadataAvailable(DocumentMetadataFields::kSearchRootDocumentId);
    }

    return DepsTracker::State::NOT_SUPPORTED;
}
}  // namespace mongo
