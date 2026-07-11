// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/s/document_source_analyze_shard_key_read_write_distribution.h"

#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/global_catalog/type_collection_common_types_gen.h"
#include "mongo/db/query/allowed_contexts.h"
#include "mongo/util/intrusive_counter.h"

#include <algorithm>

#include <absl/container/flat_hash_map.h>
#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding

namespace mongo {
namespace analyze_shard_key {

REGISTER_LITE_PARSED_DOCUMENT_SOURCE(
    _analyzeShardKeyReadWriteDistribution,
    DocumentSourceAnalyzeShardKeyReadWriteDistribution::LiteParsed::parse,
    AllowedWithApiStrict::kNeverInVersion1);

REGISTER_DOCUMENT_SOURCE_WITH_STAGE_PARAMS_DEFAULT(
    analyzeShardKeyReadWriteDistribution,
    DocumentSourceAnalyzeShardKeyReadWriteDistribution,
    AnalyzeShardKeyReadWriteDistributionStageParams);

ALLOCATE_DOCUMENT_SOURCE_ID(_analyzeShardKeyReadWriteDistribution,
                            DocumentSourceAnalyzeShardKeyReadWriteDistribution::id)

boost::intrusive_ptr<DocumentSource>
DocumentSourceAnalyzeShardKeyReadWriteDistribution::createFromBson(
    BSONElement specElem, const boost::intrusive_ptr<ExpressionContext>& pExpCtx) {
    uassert(6875701,
            str::stream() << kStageName << " must take a nested object but found: " << specElem,
            specElem.type() == BSONType::object);
    auto spec = DocumentSourceAnalyzeShardKeyReadWriteDistributionSpec::parse(
        specElem.embeddedObject(), IDLParserContext(kStageName));

    return make_intrusive<DocumentSourceAnalyzeShardKeyReadWriteDistribution>(pExpCtx,
                                                                              std::move(spec));
}

Value DocumentSourceAnalyzeShardKeyReadWriteDistribution::serialize(
    const query_shape::SerializationOptions& opts) const {
    return Value(Document{{getSourceName(), _spec.toBSON(opts)}});
}

std::unique_ptr<DocumentSourceAnalyzeShardKeyReadWriteDistribution::LiteParsed>
DocumentSourceAnalyzeShardKeyReadWriteDistribution::LiteParsed::parse(
    const NamespaceString& nss, const BSONElement& specElem, const LiteParserOptions& options) {
    uassert(
        ErrorCodes::IllegalOperation,
        str::stream() << kStageName << " is not supported on a standalone mongod",
        repl::ReplicationCoordinator::get(getGlobalServiceContext())->getSettings().isReplSet());
    uassert(ErrorCodes::IllegalOperation,
            str::stream() << kStageName << " is not supported on a multitenant replica set",
            !gMultitenancySupport);
    uassert(6875700,
            str::stream() << kStageName << " must take a nested object but found: " << specElem,
            specElem.type() == BSONType::object);
    uassertStatusOK(validateNamespace(nss));

    auto spec = DocumentSourceAnalyzeShardKeyReadWriteDistributionSpec::parse(
        specElem.embeddedObject(), IDLParserContext(kStageName));
    return std::make_unique<LiteParsed>(specElem, nss, std::move(spec));
}

}  // namespace analyze_shard_key
}  // namespace mongo
