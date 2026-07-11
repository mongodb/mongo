// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/pipeline/document_source_query_stats.h"

#include "mongo/base/data_range.h"
#include "mongo/base/error_codes.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/db/pipeline/document_source_query_stats_gen.h"
#include "mongo/db/query/allowed_contexts.h"
#include "mongo/db/query/query_feature_flags_gen.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/logv2/log.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

#include <cstddef>

#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQueryStats

namespace mongo {

REGISTER_LITE_PARSED_DOCUMENT_SOURCE_WITH_FEATURE_FLAG(queryStats,
                                                       DocumentSourceQueryStats::LiteParsed::parse,
                                                       AllowedWithApiStrict::kNeverInVersion1,
                                                       &feature_flags::gFeatureFlagQueryStats);

REGISTER_DOCUMENT_SOURCE_WITH_STAGE_PARAMS_DEFAULT(queryStats,
                                                   DocumentSourceQueryStats,
                                                   QueryStatsStageParams);

ALLOCATE_DOCUMENT_SOURCE_ID(queryStats, DocumentSourceQueryStats::id)

namespace {

/**
 * Parse the spec object calling the `ctor` with the TransformAlgorithm enum algorithm and
 * std::string hmacKey arguments.
 */
template <typename Ctor>
auto parseSpec(const BSONElement& spec, const Ctor& ctor) {
    uassert(ErrorCodes::FailedToParse,
            str::stream() << DocumentSourceQueryStats::kStageName
                          << " value must be an object. Found: " << typeName(spec.type()),
            spec.type() == BSONType::object);
    BSONObj obj = spec.embeddedObject();
    TransformAlgorithmEnum algorithm = TransformAlgorithmEnum::kNone;
    std::string hmacKey;
    auto parsed = DocumentSourceQueryStatsSpec::parse(
        obj, IDLParserContext(std::string{DocumentSourceQueryStats::kStageName}));
    boost::optional<TransformIdentifiersSpec> transformIdentifiers =
        parsed.getTransformIdentifiers();

    if (transformIdentifiers) {
        algorithm = transformIdentifiers->getAlgorithm();
        boost::optional<ConstDataRange> hmacKeyContainer = transformIdentifiers->getHmacKey();
        uassert(ErrorCodes::FailedToParse,
                str::stream() << "The 'hmacKey' parameter of the $queryStats stage must be "
                                 "specified when applying the hmac-sha-256 algorithm",
                algorithm != TransformAlgorithmEnum::kHmacSha256 ||
                    hmacKeyContainer != boost::none);
        hmacKey = std::string(hmacKeyContainer->data(), (size_t)hmacKeyContainer->length());
    }
    return ctor(algorithm, hmacKey);
}

}  // namespace

std::unique_ptr<DocumentSourceQueryStats::LiteParsed> DocumentSourceQueryStats::LiteParsed::parse(
    const NamespaceString& nss, const BSONElement& spec, const LiteParserOptions& options) {
    return parseSpec(spec, [&](TransformAlgorithmEnum algorithm, std::string hmacKey) {
        return std::make_unique<DocumentSourceQueryStats::LiteParsed>(
            spec, nss.tenantId(), algorithm, hmacKey);
    });
}

boost::intrusive_ptr<DocumentSource> DocumentSourceQueryStats::createFromBson(
    BSONElement spec, const boost::intrusive_ptr<ExpressionContext>& pExpCtx) {
    const NamespaceString& nss = pExpCtx->getNamespaceString();

    uassert(ErrorCodes::InvalidNamespace,
            "$queryStats must be run against the 'admin' database with {aggregate: 1}",
            nss.isAdminDB() && nss.isCollectionlessAggregateNS());

    // Prevent logging during a pipeline parse that won't execute a query.
    if (pExpCtx->getMongoProcessInterface()->isExpectedToExecuteQueries()) {
        LOGV2_DEBUG_OPTIONS(7808300,
                            1,
                            {logv2::LogTruncation::Disabled},
                            "Logging invocation $queryStats",
                            "commandSpec"_attr =
                                spec.Obj().redact(BSONObj::RedactLevel::sensitiveOnly));
    }

    return parseSpec(spec, [&](TransformAlgorithmEnum algorithm, std::string hmacKey) {
        return new DocumentSourceQueryStats(pExpCtx, algorithm, hmacKey);
    });
}

Value DocumentSourceQueryStats::serialize(const query_shape::SerializationOptions& opts) const {
    auto hmacKey = opts.serializeLiteral(
        BSONBinData(_hmacKey.c_str(), _hmacKey.size(), BinDataType::Sensitive));
    if (opts.isReplacingLiteralsWithRepresentativeValues()) {
        // The default shape for a BinData under this policy is empty and has sub-type 0 (general).
        // This doesn't quite work for us since we assert when we parse that it is at least 32 bytes
        // and also is sub-type 8 (sensitive).
        hmacKey =
            Value(BSONBinData("xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx", 32, BinDataType::Sensitive));
    }
    return Value{Document{
        {kStageName,
         _transformIdentifiers
             ? Document{{"transformIdentifiers",
                         Document{{"algorithm", idl::serialize(_algorithm)}, {"hmacKey", hmacKey}}}}
             : Document{}}}};
}

}  // namespace mongo
