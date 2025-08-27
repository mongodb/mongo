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

#include "mongo/db/pipeline/document_source_list_sampled_queries.h"

#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/query/allowed_contexts.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/intrusive_counter.h"

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {
namespace analyze_shard_key {

REGISTER_DOCUMENT_SOURCE(listSampledQueries,
                         DocumentSourceListSampledQueries::LiteParsed::parse,
                         DocumentSourceListSampledQueries::createFromBson,
                         AllowedWithApiStrict::kNeverInVersion1);
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

Value DocumentSourceListSampledQueries::serialize(const SerializationOptions& opts) const {
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
    return std::make_unique<LiteParsed>(specElem.fieldName(), nss, std::move(spec));
}

}  // namespace analyze_shard_key
}  // namespace mongo
