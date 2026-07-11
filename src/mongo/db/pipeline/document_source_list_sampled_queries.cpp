// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/pipeline/document_source_list_sampled_queries.h"

#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/query/allowed_contexts.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/intrusive_counter.h"

#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {
namespace analyze_shard_key {

REGISTER_LITE_PARSED_DOCUMENT_SOURCE(listSampledQueries,
                                     DocumentSourceListSampledQueries::LiteParsed::parse,
                                     AllowedWithApiStrict::kNeverInVersion1);

REGISTER_DOCUMENT_SOURCE_WITH_STAGE_PARAMS_DEFAULT(listSampledQueries,
                                                   DocumentSourceListSampledQueries,
                                                   ListSampledQueriesStageParams);

ALLOCATE_DOCUMENT_SOURCE_ID(listSampledQueries, DocumentSourceListSampledQueries::id)

boost::intrusive_ptr<DocumentSource> DocumentSourceListSampledQueries::createFromBson(
    BSONElement specElem, const boost::intrusive_ptr<ExpressionContext>& pExpCtx) {
    const NamespaceString& nss = pExpCtx->getNamespaceString();
    uassert(ErrorCodes::InvalidNamespace,
            "$listSampledQueries must be run against the 'admin' database with {aggregate: 1}",
            nss.isAdminDB() && nss.isCollectionlessAggregateNS());
    uassert(6876001,
            str::stream() << kStageName << " must take a nested object but found: " << specElem,
            specElem.type() == BSONType::object);
    auto spec = DocumentSourceListSampledQueriesSpec::parse(specElem.embeddedObject(),
                                                            IDLParserContext(kStageName));

    return make_intrusive<DocumentSourceListSampledQueries>(pExpCtx, std::move(spec));
}

Value DocumentSourceListSampledQueries::serialize(
    const query_shape::SerializationOptions& opts) const {
    return Value(Document{{getSourceName(), _spec.toBSON(opts)}});
}

void DocumentSourceListSampledQueries::detachSourceFromOperationContext() {
    if (_sharedState->pipeline) {
        tassert(10713701,
                "expecting '_execPipeline' to be initialized when '_pipeline' is initialized",
                _sharedState->execPipeline);
        _sharedState->execPipeline->detachFromOperationContext();
        _sharedState->pipeline->detachFromOperationContext();
    }
}

void DocumentSourceListSampledQueries::reattachSourceToOperationContext(OperationContext* opCtx) {
    if (_sharedState->pipeline) {
        tassert(10713703,
                "expecting '_execPipeline' to be initialized when '_pipeline' is initialized",
                _sharedState->execPipeline);
        _sharedState->execPipeline->reattachToOperationContext(opCtx);
        _sharedState->pipeline->reattachToOperationContext(opCtx);
    }
}

std::unique_ptr<DocumentSourceListSampledQueries::LiteParsed>
DocumentSourceListSampledQueries::LiteParsed::parse(const NamespaceString& nss,
                                                    const BSONElement& specElem,
                                                    const LiteParserOptions& options) {
    uassert(6876000,
            str::stream() << kStageName << " must take a nested object but found: " << specElem,
            specElem.type() == BSONType::object);
    uassert(ErrorCodes::IllegalOperation,
            str::stream() << kStageName << " is not supported on a standalone mongod",
            serverGlobalParams.clusterRole.hasExclusively(ClusterRole::RouterServer) ||
                repl::ReplicationCoordinator::get(getGlobalServiceContext())
                    ->getSettings()
                    .isReplSet());
    uassert(ErrorCodes::IllegalOperation,
            str::stream() << kStageName << " is not supported on a multitenant replica set",
            !gMultitenancySupport);

    auto spec = DocumentSourceListSampledQueriesSpec::parse(specElem.embeddedObject(),
                                                            IDLParserContext(kStageName));
    if (spec.getNamespace()) {
        uassertStatusOK(validateNamespace(*spec.getNamespace()));
    }
    return std::make_unique<LiteParsed>(specElem, nss, std::move(spec));
}

}  // namespace analyze_shard_key
}  // namespace mongo
