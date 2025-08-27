/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQueryStats

namespace mongo {

REGISTER_DOCUMENT_SOURCE_WITH_FEATURE_FLAG(queryStats,
                                           DocumentSourceQueryStats::LiteParsed::parse,
                                           DocumentSourceQueryStats::createFromBson,
                                           AllowedWithApiStrict::kNeverInVersion1,
                                           &feature_flags::gFeatureFlagQueryStats);
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
            spec.fieldName(), nss.tenantId(), algorithm, hmacKey);
    });
}

boost::intrusive_ptr<DocumentSource> DocumentSourceQueryStats::createFromBson(
    BSONElement spec, const boost::intrusive_ptr<ExpressionContext>& pExpCtx) {
    const NamespaceString& nss = pExpCtx->getNamespaceString();

    uassert(ErrorCodes::InvalidNamespace,
            "$queryStats must be run against the 'admin' database with {aggregate: 1}",
            nss.isAdminDB() && nss.isCollectionlessAggregateNS());

    LOGV2_DEBUG_OPTIONS(7808300,
                        1,
                        {logv2::LogTruncation::Disabled},
                        "Logging invocation $queryStats",
                        "commandSpec"_attr =
                            spec.Obj().redact(BSONObj::RedactLevel::sensitiveOnly));
    return parseSpec(spec, [&](TransformAlgorithmEnum algorithm, std::string hmacKey) {
        return new DocumentSourceQueryStats(pExpCtx, algorithm, hmacKey);
    });
}

Value DocumentSourceQueryStats::serialize(const SerializationOptions& opts) const {
    auto hmacKey = opts.serializeLiteral(
        BSONBinData(_hmacKey.c_str(), _hmacKey.size(), BinDataType::Sensitive));
    if (opts.isReplacingLiteralsWithRepresentativeValues()) {
        // The default shape for a BinData under this policy is empty and has sub-type 0 (general).
        // This doesn't quite work for us since we assert when we parse that it is at least 32 bytes
        // and also is sub-type 8 (sensitive).
        hmacKey =
            Value(BSONBinData("xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx", 32, BinDataType::Sensitive));
    }
    return Value{
        Document{{kStageName,
                  _transformIdentifiers
                      ? Document{{"transformIdentifiers",
                                  Document{{"algorithm", TransformAlgorithm_serializer(_algorithm)},
                                           {"hmacKey", hmacKey}}}}
                      : Document{}}}};
}

}  // namespace mongo
