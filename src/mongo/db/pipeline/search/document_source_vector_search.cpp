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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery

#include "mongo/db/pipeline/search/document_source_vector_search.h"

#include "mongo/base/string_data.h"
#include "mongo/db/pipeline/search/document_source_internal_search_id_lookup.h"
#include "mongo/db/pipeline/search/lite_parsed_search.h"
#include "mongo/db/pipeline/search/vector_search_helper.h"
#include "mongo/db/pipeline/skip_and_limit.h"
#include "mongo/db/query/query_shape/serialization_options.h"
#include "mongo/db/query/search/mongot_cursor.h"
#include "mongo/db/query/search/search_task_executors.h"
#include "mongo/db/query/vector_search/filter_validator.h"
#include "mongo/db/s/operation_sharding_state.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery

namespace mongo {

using boost::intrusive_ptr;
using executor::RemoteCommandRequest;
using executor::TaskExecutorCursor;

REGISTER_DOCUMENT_SOURCE_WITH_FEATURE_FLAG(vectorSearch,
                                           LiteParsedSearchStage::parse,
                                           DocumentSourceVectorSearch::createFromBson,
                                           AllowedWithApiStrict::kNeverInVersion1,
                                           feature_flags::gFeatureFlagVectorSearchPublicPreview);

DocumentSourceVectorSearch::DocumentSourceVectorSearch(
    VectorSearchSpec&& request,
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    std::shared_ptr<executor::TaskExecutor> taskExecutor)
    : DocumentSource(kStageName, expCtx),
      _request(std::move(request)),
      _filterExpr(_request.getFilter() ? uassertStatusOK(MatchExpressionParser::parse(
                                             *_request.getFilter(), pExpCtx))
                                       : nullptr),
      _taskExecutor(taskExecutor),
      _limit(_request.getLimit().coerceToLong()) {
    if (_filterExpr) {
        validateVectorSearchFilter(_filterExpr.get());
    }
}

Value DocumentSourceVectorSearch::serialize(const SerializationOptions& opts) const {
    // First, serialize the IDL struct.
    auto baseObj = [&] {
        BSONObjBuilder builder;
        _request.serialize(&builder, opts);
        return builder.obj();
    }();

    // IDL serialization doesn't maintain the 'double' type of the array contents so we need to
    // override it.
    baseObj = baseObj.addFields(
        BSON(VectorSearchSpec::kQueryVectorFieldName << opts.serializeLiteral(
                 _request.getQueryVector(), ImplicitValue(std::vector<double>{1.0}))));

    // IDL doesn't know how to shapify 'limit', serialize it explicitly.
    baseObj = baseObj.addFields(BSON(VectorSearchSpec::kLimitFieldName
                                     << opts.serializeLiteral(_request.getLimit().coerceToLong())));

    // IDL doesn't know how to shapify 'numCandidates'; if it exists, serialize it explicitly.
    if (_request.getNumCandidates()) {
        baseObj = baseObj.addFields(
            BSON(VectorSearchSpec::kNumCandidatesFieldName
                 << opts.serializeLiteral(_request.getNumCandidates()->coerceToLong())));
    }

    if (_filterExpr) {
        // Send the unparsed filter to avoid performing transformations mongot doesn't have
        // implemented. Specifically this refers to operators like $neq and $nin which in a sharded
        // environment are desugared to {$not: {$and: [...]}}. We may want to change this to send
        // the parsed version once mongot supports $not.
        if (opts.literalPolicy == LiteralSerializationPolicy::kUnchanged) {
            baseObj = baseObj.addFields(
                BSON(VectorSearchSpec::kFilterFieldName << _request.getFilter().get()));
        } else {
            baseObj = baseObj.addFields(
                BSON(VectorSearchSpec::kFilterFieldName << _filterExpr->serialize(opts)));
        }
    }

    // We don't want mongos to make a remote call to mongot even though it can generate explain
    // output.
    if (!opts.verbosity || pExpCtx->inMongos) {
        return Value(Document{{kStageName, baseObj}});
    }

    BSONObj explainInfo = _explainResponse.isEmpty()
        ? search_helpers::getVectorSearchExplainResponse(pExpCtx, _request, _taskExecutor.get())
        : _explainResponse;

    baseObj = baseObj.addFields(BSON("explain" << opts.serializeLiteral(explainInfo)));
    return Value(Document{{kStageName, baseObj}});
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
        _explainResponse =
            search_helpers::getVectorSearchExplainResponse(pExpCtx, _request, _taskExecutor.get());
        return DocumentSource::GetNextResult::makeEOF();
    }

    // If this is the first call, establish the cursor.
    if (!_cursor) {
        _cursor.emplace(
            search_helpers::establishVectorSearchCursor(pExpCtx, _request, _taskExecutor));
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

    auto spec = VectorSearchSpec::parse(IDLParserContext(kStageName), elem.embeddedObject());
    uassert(7912700, "$vectorSearch limit must be positive", spec.getLimit().coerceToLong() > 0);

    auto serviceContext = expCtx->opCtx->getServiceContext();
    std::list<intrusive_ptr<DocumentSource>> desugaredPipeline = {
        make_intrusive<DocumentSourceVectorSearch>(
            std::move(spec), expCtx, executor::getMongotTaskExecutor(serviceContext))};

    // Only add an idLookup stage once, when we reach the mongod that will execute the pipeline.
    // Ignore the case where we have a stub 'mongoProcessInterface' because this only occurs during
    // validation/analysis, e.g. for QE and pipeline-style updates.
    if ((expCtx->mongoProcessInterface->isExpectedToExecuteQueries() &&
         !expCtx->mongoProcessInterface->inShardedEnvironment(expCtx->opCtx)) ||
        OperationShardingState::isComingFromRouter(expCtx->opCtx)) {
        desugaredPipeline.insert(std::next(desugaredPipeline.begin()),
                                 make_intrusive<DocumentSourceInternalSearchIdLookUp>(expCtx));
    }
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
            _limit = std::min(_limit, *userLimit);
        }
    }

    return std::next(itr);
}

}  // namespace mongo
