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

#include "mongo/db/pipeline/search/document_source_vector_search.h"

#include "mongo/base/string_data.h"
#include "mongo/db/pipeline/document_source_internal_shard_filter.h"
#include "mongo/db/pipeline/search/document_source_internal_search_id_lookup.h"
#include "mongo/db/pipeline/search/lite_parsed_search.h"
#include "mongo/db/pipeline/search/vector_search_helper.h"
#include "mongo/db/pipeline/skip_and_limit.h"
#include "mongo/db/query/query_shape/serialization_options.h"
#include "mongo/db/query/search/mongot_cursor.h"
#include "mongo/db/query/search/search_task_executors.h"
#include "mongo/db/s/operation_sharding_state.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery

namespace mongo {

using boost::intrusive_ptr;
using executor::RemoteCommandRequest;

REGISTER_DOCUMENT_SOURCE_WITH_FEATURE_FLAG(vectorSearch,
                                           LiteParsedSearchStage::parse,
                                           DocumentSourceVectorSearch::createFromBson,
                                           AllowedWithApiStrict::kNeverInVersion1,
                                           feature_flags::gFeatureFlagVectorSearchPublicPreview);

DocumentSourceVectorSearch::DocumentSourceVectorSearch(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    std::shared_ptr<executor::TaskExecutor> taskExecutor,
    BSONObj originalSpec)
    : DocumentSource(kStageName, expCtx),
      _taskExecutor(taskExecutor),
      _originalSpec(originalSpec.getOwned()) {
    if (auto limitElem = _originalSpec.getField(kLimitFieldName)) {
        uassert(
            8575100, "Expected limit field to be a number in $vectorSearch", limitElem.isNumber());
        _limit = limitElem.safeNumberLong();
        uassert(7912700, "Expected limit to be positive", *_limit > 0);
    }
    if (auto filterElem = originalSpec.getField(kFilterFieldName)) {
        uassertStatusOK(MatchExpressionParser::parse(filterElem.Obj(), expCtx));
    }
}

Value DocumentSourceVectorSearch::serialize(const SerializationOptions& opts) const {
    if (opts.literalPolicy != LiteralSerializationPolicy::kUnchanged) {
        return Value(Document{{kStageName, opts.serializeLiteral(_originalSpec)}});
    }

    // We don't want mongos to make a remote call to mongot even though it can generate explain
    // output.
    if (!opts.verbosity || pExpCtx->inMongos) {
        return Value(Document{{kStageName, _originalSpec}});
    }

    BSONObj explainInfo = _explainResponse.isEmpty()
        ? search_helpers::getVectorSearchExplainResponse(
              pExpCtx, _originalSpec, _taskExecutor.get())
        : _explainResponse;

    auto explainObj =
        _originalSpec.addFields(BSON("explain" << opts.serializeLiteral(explainInfo)));
    return Value(Document{{kStageName, explainObj}});
}

boost::optional<BSONObj> DocumentSourceVectorSearch::getNext() {
    try {
        return _cursor->getNext(pExpCtx->opCtx);
    } catch (DBException& ex) {
        ex.addContext("Remote error from mongot");
        throw;
    }
}

DocumentSource::GetNextResult DocumentSourceVectorSearch::getNextAfterSetup() {
    auto response = getNext();
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

    // Populate $sortKey metadata field so that mongos can properly merge sort the document stream.
    if (pExpCtx->needsMerge) {
        // Metadata can't be changed on a Document. Create a MutableDocument to set the sortKey.
        MutableDocument output(Document::fromBsonWithMetaData(response.value()));

        tassert(7828500,
                "Expected vector search distance to be present",
                output.metadata().hasVectorSearchScore());
        output.metadata().setSortKey(Value{output.metadata().getVectorSearchScore()},
                                     true /* isSingleElementKey */);
        return output.freeze();
    }
    return Document::fromBsonWithMetaData(response.value());
}

DocumentSource::GetNextResult DocumentSourceVectorSearch::doGetNext() {
    // Return EOF if pExpCtx->uuid is unset here; the collection we are searching over has not been
    // created yet.
    if (!pExpCtx->uuid) {
        return DocumentSource::GetNextResult::makeEOF();
    }

    if (pExpCtx->explain) {
        _explainResponse = search_helpers::getVectorSearchExplainResponse(
            pExpCtx, _originalSpec, _taskExecutor.get());
        return DocumentSource::GetNextResult::makeEOF();
    }

    // If this is the first call, establish the cursor.
    if (!_cursor) {
        _cursor.emplace(
            search_helpers::establishVectorSearchCursor(pExpCtx, _originalSpec, _taskExecutor));
    }

    return getNextAfterSetup();
}

std::list<intrusive_ptr<DocumentSource>> DocumentSourceVectorSearch::createFromBson(
    BSONElement elem, const intrusive_ptr<ExpressionContext>& expCtx) {
    mongot_cursor::throwIfNotRunningWithMongotHostConfigured(expCtx);

    uassert(ErrorCodes::FailedToParse,
            str::stream() << kStageName
                          << " value must be an object. Found: " << typeName(elem.type()),
            elem.type() == BSONType::Object);

    auto serviceContext = expCtx->opCtx->getServiceContext();
    std::list<intrusive_ptr<DocumentSource>> desugaredPipeline = {
        make_intrusive<DocumentSourceVectorSearch>(
            expCtx, executor::getMongotTaskExecutor(serviceContext), elem.embeddedObject())};

    // TODO: SERVER-85426 Remove this block of code (it's the original location id lookup
    // was added to $vectorSearch, but that needed to be changed to support sharded
    // $unionWith $vectorSearch)
    if (!enableUnionWithVectorSearch.load()) {
        auto shardFilterer = DocumentSourceInternalShardFilter::buildIfNecessary(expCtx);
        // Only add an idLookup stage once, when we reach the mongod that will execute the pipeline.
        // Ignore the case where we have a stub 'mongoProcessInterface' because this only occurs
        // during validation/analysis, e.g. for QE and pipeline-style updates.
        if ((expCtx->mongoProcessInterface->isExpectedToExecuteQueries() &&
             !expCtx->mongoProcessInterface->inShardedEnvironment(expCtx->opCtx)) ||
            OperationShardingState::isComingFromRouter(expCtx->opCtx)) {
            desugaredPipeline.insert(std::next(desugaredPipeline.begin()),
                                     make_intrusive<DocumentSourceInternalSearchIdLookUp>(
                                         expCtx, 0, buildExecShardFilterPolicy(shardFilterer)));
            if (shardFilterer)
                desugaredPipeline.push_back(std::move(shardFilterer));
        }
    }

    return desugaredPipeline;
}


std::list<intrusive_ptr<DocumentSource>> DocumentSourceVectorSearch::desugar() {
    auto executor = executor::getMongotTaskExecutor(pExpCtx->opCtx->getServiceContext());

    std::list<intrusive_ptr<DocumentSource>> desugaredPipeline = {
        make_intrusive<DocumentSourceVectorSearch>(pExpCtx, executor, _originalSpec.getOwned())};

    auto shardFilterer = DocumentSourceInternalShardFilter::buildIfNecessary(pExpCtx);
    auto idLookupStage = make_intrusive<DocumentSourceInternalSearchIdLookUp>(
        pExpCtx, _limit.value_or(0), buildExecShardFilterPolicy(shardFilterer));
    desugaredPipeline.insert(std::next(desugaredPipeline.begin()), idLookupStage);
    if (shardFilterer)
        desugaredPipeline.push_back(std::move(shardFilterer));

    return desugaredPipeline;
}

Pipeline::SourceContainer::iterator DocumentSourceVectorSearch::doOptimizeAt(
    Pipeline::SourceContainer::iterator itr, Pipeline::SourceContainer* container) {
    auto stageItr = std::next(itr);
    // Only attempt to get the limit from the query if there are further stages in the pipeline.
    if (stageItr != container->end()) {
        // Move past the $internalSearchIdLookup stage, if it is next.
        auto nextIdLookup = dynamic_cast<DocumentSourceInternalSearchIdLookUp*>(stageItr->get());
        if (nextIdLookup) {
            ++stageItr;
        }
        // Calculate the extracted limit without modifying the rest of the pipeline.
        if (auto userLimit = getUserLimit(stageItr, container)) {
            _limit = _limit ? std::min(*_limit, *userLimit) : *userLimit;
        }
    }

    return std::next(itr);
}

}  // namespace mongo
