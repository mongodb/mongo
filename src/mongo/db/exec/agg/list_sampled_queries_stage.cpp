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

#include "mongo/db/exec/agg/list_sampled_queries_stage.h"

#include "mongo/db/exec/agg/document_source_to_stage_registry.h"
#include "mongo/db/exec/agg/pipeline_builder.h"
#include "mongo/db/pipeline/document_source_list_sampled_queries.h"
#include "mongo/db/pipeline/expression_context_builder.h"
#include "mongo/logv2/log.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding

namespace mongo {

using namespace analyze_shard_key;

boost::intrusive_ptr<exec::agg::Stage> documentSourceListSampledQueriesToStageFn(
    const boost::intrusive_ptr<DocumentSource>& documentSource) {
    auto* listSampledQueriesDS =
        dynamic_cast<DocumentSourceListSampledQueries*>(documentSource.get());

    tassert(10816900, "expected 'DocumentSourceListSampledQueries' type", listSampledQueriesDS);

    return make_intrusive<exec::agg::ListSampledQueriesStage>(
        listSampledQueriesDS->kStageName,
        listSampledQueriesDS->getExpCtx(),
        listSampledQueriesDS->_spec.getNamespace(),
        listSampledQueriesDS->_sharedState);
}

namespace exec::agg {

REGISTER_AGG_STAGE_MAPPING(listSampledQueries,
                           DocumentSourceListSampledQueries::id,
                           documentSourceListSampledQueriesToStageFn)

ListSampledQueriesStage::ListSampledQueriesStage(
    StringData stageName,
    const boost::intrusive_ptr<ExpressionContext>& pExpCtx,
    boost::optional<mongo::NamespaceString> nss,
    std::shared_ptr<ListSampledQueriesSharedState> sharedState)
    : Stage(stageName, pExpCtx), _nss(std::move(nss)), _sharedState(std::move(sharedState)) {}

void ListSampledQueriesStage::detachFromOperationContext() {
    if (_sharedState->pipeline) {
        tassert(10713700,
                "expecting '_execPipeline' to be initialized when '_pipeline' is initialized",
                _sharedState->execPipeline);
        _sharedState->execPipeline->detachFromOperationContext();
        _sharedState->pipeline->detachFromOperationContext();
    }
}

void ListSampledQueriesStage::reattachToOperationContext(OperationContext* opCtx) {
    if (_sharedState->pipeline) {
        tassert(10713702,
                "expecting '_execPipeline' to be initialized when '_pipeline' is initialized",
                _sharedState->execPipeline);
        _sharedState->execPipeline->reattachToOperationContext(opCtx);
        _sharedState->pipeline->reattachToOperationContext(opCtx);
    }
}

GetNextResult ListSampledQueriesStage::doGetNext() {
    if (_sharedState->pipeline == nullptr) {
        auto foreignExpCtx =
            makeCopyFromExpressionContext(pExpCtx, NamespaceString::kConfigSampledQueriesNamespace);
        std::vector<BSONObj> stages;
        if (_nss) {
            uassertStatusOK(validateNamespace(*_nss));
            stages.push_back(
                BSON("$match" << BSON(SampledQueryDocument::kNsFieldName
                                      << NamespaceStringUtil::serialize(
                                             *_nss, SerializationContext::stateDefault()))));
        }
        try {
            _sharedState->pipeline = mongo::Pipeline::makePipeline(stages, foreignExpCtx);
            _sharedState->execPipeline = buildPipeline(_sharedState->pipeline->freeze());
        } catch (ExceptionFor<ErrorCodes::NamespaceNotFound>& ex) {
            LOGV2(7807800,
                  "Failed to create aggregation pipeline to list sampled queries",
                  "error"_attr = redact(ex.toStatus()));
            return GetNextResult::makeEOF();
        }
    }

    if (auto doc = _sharedState->execPipeline->getNext()) {
        auto queryDoc = SampledQueryDocument::parse(
            doc->toBson(), IDLParserContext(DocumentSourceListSampledQueries::kStageName));
        DocumentSourceListSampledQueriesResponse response;
        response.setSampledQueryDocument(std::move(queryDoc));
        return {Document(response.toBSON())};
    }

    return GetNextResult::makeEOF();
}

}  // namespace exec::agg
}  // namespace mongo
