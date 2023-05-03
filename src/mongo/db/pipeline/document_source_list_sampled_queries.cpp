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

#include "mongo/db/dbdirectclient.h"
#include "mongo/logv2/log.h"
#include "mongo/s/analyze_shard_key_documents_gen.h"
#include "mongo/s/analyze_shard_key_feature_flag_gen.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding

namespace mongo {
namespace analyze_shard_key {

REGISTER_DOCUMENT_SOURCE_WITH_FEATURE_FLAG(listSampledQueries,
                                           DocumentSourceListSampledQueries::LiteParsed::parse,
                                           DocumentSourceListSampledQueries::createFromBson,
                                           AllowedWithApiStrict::kNeverInVersion1,
                                           analyze_shard_key::gFeatureFlagAnalyzeShardKey);

boost::intrusive_ptr<DocumentSource> DocumentSourceListSampledQueries::createFromBson(
    BSONElement specElem, const boost::intrusive_ptr<ExpressionContext>& pExpCtx) {
    const NamespaceString& nss = pExpCtx->ns;
    uassert(ErrorCodes::InvalidNamespace,
            "$listSampledQueries must be run against the 'admin' database with {aggregate: 1}",
            nss.db() == DatabaseName::kAdmin.db() && nss.isCollectionlessAggregateNS());
    uassert(6876001,
            str::stream() << kStageName << " must take a nested object but found: " << specElem,
            specElem.type() == BSONType::Object);
    auto spec = DocumentSourceListSampledQueriesSpec::parse(IDLParserContext(kStageName),
                                                            specElem.embeddedObject());

    return make_intrusive<DocumentSourceListSampledQueries>(pExpCtx, std::move(spec));
}

Value DocumentSourceListSampledQueries::serialize(SerializationOptions opts) const {
    if (opts.applyHmacToIdentifiers || opts.replacementForLiteralArgs) {
        MONGO_UNIMPLEMENTED_TASSERT(6876002);
    }

    return Value(Document{{getSourceName(), _spec.toBSON()}});
}

DocumentSource::GetNextResult DocumentSourceListSampledQueries::doGetNext() {
    if (_finished) {
        return GetNextResult::makeEOF();
    }

    auto ns = _spec.getNamespace();
    if (_cursor == nullptr) {
        FindCommandRequest findRequest{NamespaceString::kConfigSampledQueriesNamespace};
        if (ns) {
            findRequest.setFilter(BSON(SampledQueryDocument::kNsFieldName << ns->toString()));
        }

        DBDirectClient client(pExpCtx->opCtx);
        _cursor = client.find(std::move(findRequest));
    }

    if (_cursor->more()) {
        const auto obj = _cursor->next().getOwned();
        const auto doc = SampledQueryDocument::parse(
            IDLParserContext(DocumentSourceListSampledQueries::kStageName), obj);
        DocumentSourceListSampledQueriesResponse response;
        response.setSampledQueryDocument(doc);
        return {Document(response.toBSON())};
    }
    _finished = true;
    return GetNextResult::makeEOF();
}

}  // namespace analyze_shard_key
}  // namespace mongo
