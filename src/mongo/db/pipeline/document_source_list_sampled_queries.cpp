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

#include <vector>

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/pipeline/sharded_agg_helpers_targeting_policy.h"
#include "mongo/db/query/allowed_contexts.h"
#include "mongo/logv2/log.h"
#include "mongo/logv2/log_attr.h"
#include "mongo/logv2/log_component.h"
#include "mongo/logv2/redaction.h"
#include "mongo/s/analyze_shard_key_common_gen.h"
#include "mongo/s/analyze_shard_key_documents_gen.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/intrusive_counter.h"
#include "mongo/util/namespace_string_util.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding

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
            specElem.type() == BSONType::Object);
    auto spec = DocumentSourceListSampledQueriesSpec::parse(IDLParserContext(kStageName),
                                                            specElem.embeddedObject());

    return make_intrusive<DocumentSourceListSampledQueries>(pExpCtx, std::move(spec));
}

Value DocumentSourceListSampledQueries::serialize(const SerializationOptions& opts) const {
    return Value(Document{{getSourceName(), _spec.toBSON(opts)}});
}

DocumentSource::GetNextResult DocumentSourceListSampledQueries::doGetNext() {
    if (_pipeline == nullptr) {
        auto foreignExpCtx = pExpCtx->copyWith(NamespaceString::kConfigSampledQueriesNamespace);
        std::vector<BSONObj> stages;
        if (auto& nss = _spec.getNamespace()) {
            uassertStatusOK(validateNamespace(*nss));
            stages.push_back(
                BSON("$match" << BSON(SampledQueryDocument::kNsFieldName
                                      << NamespaceStringUtil::serialize(
                                             *nss, SerializationContext::stateDefault()))));
        }
        try {
            _pipeline = Pipeline::makePipeline(stages, foreignExpCtx);
        } catch (ExceptionFor<ErrorCodes::NamespaceNotFound>& ex) {
            LOGV2(7807800,
                  "Failed to create aggregation pipeline to list sampled queries",
                  "error"_attr = redact(ex.toStatus()));
            return GetNextResult::makeEOF();
        }
    }

    if (auto doc = _pipeline->getNext()) {
        auto queryDoc = SampledQueryDocument::parse(
            IDLParserContext(DocumentSourceListSampledQueries::kStageName), doc->toBson());
        DocumentSourceListSampledQueriesResponse response;
        response.setSampledQueryDocument(std::move(queryDoc));
        return {Document(response.toBSON())};
    }

    return GetNextResult::makeEOF();
}

void DocumentSourceListSampledQueries::detachFromOperationContext() {
    if (_pipeline) {
        _pipeline->detachFromOperationContext();
    }
}

void DocumentSourceListSampledQueries::reattachToOperationContext(OperationContext* opCtx) {
    if (_pipeline) {
        _pipeline->reattachToOperationContext(opCtx);
    }
}

std::unique_ptr<DocumentSourceListSampledQueries::LiteParsed>
DocumentSourceListSampledQueries::LiteParsed::parse(const NamespaceString& nss,
                                                    const BSONElement& specElem) {
    uassert(6876000,
            str::stream() << kStageName << " must take a nested object but found: " << specElem,
            specElem.type() == BSONType::Object);
    uassert(ErrorCodes::IllegalOperation,
            str::stream() << kStageName << " is not supported on a standalone mongod",
            serverGlobalParams.clusterRole.hasExclusively(ClusterRole::RouterServer) ||
                repl::ReplicationCoordinator::get(getGlobalServiceContext())
                    ->getSettings()
                    .isReplSet());
    uassert(ErrorCodes::IllegalOperation,
            str::stream() << kStageName << " is not supported on a multitenant replica set",
            !gMultitenancySupport);

    auto spec = DocumentSourceListSampledQueriesSpec::parse(IDLParserContext(kStageName),
                                                            specElem.embeddedObject());
    if (spec.getNamespace()) {
        uassertStatusOK(validateNamespace(*spec.getNamespace()));
    }
    return std::make_unique<LiteParsed>(specElem.fieldName(), nss, std::move(spec));
}

}  // namespace analyze_shard_key
}  // namespace mongo
