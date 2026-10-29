// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/exec/agg/list_sampled_queries_stage.h"

#include "mongo/db/exec/agg/document_source_to_stage_registry.h"
#include "mongo/db/exec/agg/pipeline_builder.h"
#include "mongo/db/pipeline/document_source_list_sampled_queries.h"
#include "mongo/db/pipeline/expression_context_builder.h"
#include "mongo/db/pipeline/pipeline_factory.h"
#include "mongo/logv2/log.h"

#include <string_view>

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
    std::string_view stageName,
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
            _sharedState->pipeline = pipeline_factory::makePipeline(stages, foreignExpCtx);
            _sharedState->execPipeline = buildPipeline(_sharedState->pipeline->freeze());
        } catch (ExceptionFor<ErrorCodes::NamespaceNotFound>& ex) {
            LOGV2(7807800,
                  "Failed to create aggregation pipeline to list sampled queries",
                  "error"_attr = redact(ex.toStatus()));
            return GetNextResult::makeEOF();
        }
    }

    tassert(11124500,
            "expecting '_sharedState->execPipeline' to be initialized",
            _sharedState->execPipeline);
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
