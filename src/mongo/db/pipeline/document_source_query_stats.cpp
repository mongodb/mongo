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

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>
#include <cstddef>
#include <list>

#include "mongo/base/data_range.h"
#include "mongo/base/error_codes.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/commands/server_status_metric.h"
#include "mongo/db/feature_flag.h"
#include "mongo/db/pipeline/document_source_query_stats_gen.h"
#include "mongo/db/query/allowed_contexts.h"
#include "mongo/db/query/lru_key_value.h"
#include "mongo/db/query/query_feature_flags_gen.h"
#include "mongo/db/query/query_knobs_gen.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/logv2/log.h"
#include "mongo/logv2/log_attr.h"
#include "mongo/logv2/log_component.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/debug_util.h"
#include "mongo/util/intrusive_counter.h"
#include "mongo/util/str.h"
#include "mongo/util/time_support.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQueryStats

namespace mongo {
namespace {
CounterMetric queryStatsHmacApplicationErrors("queryStats.numHmacApplicationErrors");
}

// TODO SERVER-79494 Use REGISTER_DOCUMENT_SOURCE_WITH_FEATURE_FLAG
REGISTER_DOCUMENT_SOURCE(queryStats,
                         DocumentSourceQueryStats::LiteParsed::parse,
                         DocumentSourceQueryStats::createFromBson,
                         AllowedWithApiStrict::kNeverInVersion1);

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
            spec.type() == BSONType::Object);
    BSONObj obj = spec.embeddedObject();
    TransformAlgorithmEnum algorithm = TransformAlgorithmEnum::kNone;
    std::string hmacKey;
    auto parsed = DocumentSourceQueryStatsSpec::parse(
        IDLParserContext(DocumentSourceQueryStats::kStageName.toString()), obj);
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


/**
 * Given a partition, it copies the QueryStatsEntries located in partition of cache into a
 * vector of pairs that contain the cache key and a corresponding QueryStatsEntry. This ensures
 * that the partition mutex is only held for the duration of copying.
 */
std::vector<QueryStatsEntry> copyPartition(const QueryStatsStore::Partition& partition) {
    // Note the intentional copy of QueryStatsEntry and intentional additional shared pointer
    // reference to the key generator. This will give us a snapshot of all the metrics we want to
    // report, and keep the keyGenerator around even if the entry gets evicted.
    std::vector<QueryStatsEntry> currKeyMetrics;
    for (auto&& [hash, metrics] : *partition) {
        currKeyMetrics.push_back(metrics);
    }
    return currKeyMetrics;
}

}  // namespace

BSONObj DocumentSourceQueryStats::computeQueryStatsKey(
    std::shared_ptr<const KeyGenerator> keyGenerator,
    const SerializationContext& serializationContext) const {
    static const auto sha256HmacStringDataHasher = [](std::string key, const StringData& sd) {
        auto hashed = SHA256Block::computeHmac(
            (const uint8_t*)key.data(), key.size(), (const uint8_t*)sd.rawData(), sd.size());
        return hashed.toString();
    };

    auto opts = SerializationOptions{};
    opts.literalPolicy = LiteralSerializationPolicy::kToDebugTypeString;
    if (_algorithm == TransformAlgorithmEnum::kHmacSha256) {
        opts.transformIdentifiers = true;
        opts.transformIdentifiersCallback = [&](StringData sd) {
            return sha256HmacStringDataHasher(_hmacKey, sd);
        };
    }
    return keyGenerator->generate(pExpCtx->opCtx, opts, serializationContext);
}

std::unique_ptr<DocumentSourceQueryStats::LiteParsed> DocumentSourceQueryStats::LiteParsed::parse(
    const NamespaceString& nss, const BSONElement& spec) {
    // TODO SERVER-79494 Remove this manual feature flag check once we're registering doc source
    // with REGISTER_DOCUMENT_SOURCE_WITH_FEATURE_FLAG
    uassert(ErrorCodes::QueryFeatureNotAllowed,
            "$queryStats is not allowed in the current configuration. You may need to enable the "
            "correponding feature flag",
            query_stats::isQueryStatsFeatureEnabled(/*requiresFullQueryStatsFeatureFlag*/ false));

    return parseSpec(spec, [&](TransformAlgorithmEnum algorithm, std::string hmacKey) {
        return std::make_unique<DocumentSourceQueryStats::LiteParsed>(
            spec.fieldName(), nss.tenantId(), algorithm, hmacKey);
    });
}

boost::intrusive_ptr<DocumentSource> DocumentSourceQueryStats::createFromBson(
    BSONElement spec, const boost::intrusive_ptr<ExpressionContext>& pExpCtx) {
    // TODO SERVER-79494 Remove this manual feature flag check once we're registering doc source
    // with REGISTER_DOCUMENT_SOURCE_WITH_FEATURE_FLAG
    uassert(ErrorCodes::QueryFeatureNotAllowed,
            "$queryStats is not allowed in the current configuration. You may need to enable the "
            "correponding feature flag",
            query_stats::isQueryStatsFeatureEnabled(/*requiresFullQueryStatsFeatureFlag*/ false));

    const NamespaceString& nss = pExpCtx->ns;

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
    // This document source never contains any user information, so serialization options do not
    // apply.
    return Value{Document{
        {kStageName,
         _transformIdentifiers
             ? Document{{"transformIdentifiers",
                         Document{
                             {"algorithm", TransformAlgorithm_serializer(_algorithm)},
                             {"hmacKey",
                              opts.serializeLiteral(BSONBinData(
                                  _hmacKey.c_str(), _hmacKey.size(), BinDataType::Sensitive))}}}}
             : Document{}}}};
}

DocumentSource::GetNextResult DocumentSourceQueryStats::doGetNext() {
    /**
     * We maintain nested iterators:
     * - Outer one over the set of partitions.
     * - Inner one over the set of entries in a "materialized" partition.
     *
     * When an inner iterator is present and contains more elements, we can return the next element.
     * When the inner iterator is exhausted, we move to the next element in the outer iterator and
     * create a new inner iterator. When the outer iterator is exhausted, we have finished iterating
     * over the queryStats store entries.
     *
     * The inner iterator iterates over a materialized container of all entries in the partition.
     * This is done to reduce the time under which the partition lock is held.
     */
    bool shouldLog = _algorithm != TransformAlgorithmEnum::kNone;
    while (true) {
        // First, attempt to exhaust all elements in the materialized partition.
        if (!_materializedPartition.empty()) {
            // Move out of the container reference.
            auto doc = std::move(_materializedPartition.front());
            _materializedPartition.pop_front();
            if (shouldLog) {
                LOGV2_DEBUG_OPTIONS(7808301,
                                    3,
                                    {logv2::LogTruncation::Disabled},
                                    "Logging all outputs of $queryStats",
                                    "thisOutput"_attr = doc);
            }
            return {std::move(doc)};
        }

        QueryStatsStore& _queryStatsStore = getQueryStatsStore(getContext()->opCtx);

        // Materialized partition is exhausted, move to the next.
        _currentPartition++;
        if (_currentPartition >= _queryStatsStore.numPartitions()) {
            if (shouldLog) {
                LOGV2_DEBUG_OPTIONS(7808302,
                                    3,
                                    {logv2::LogTruncation::Disabled},
                                    "Finished logging outout of $queryStats");
            }
            return DocumentSource::GetNextResult::makeEOF();
        }

        // Capture the time at which reading the partition begins to indicate to the caller
        // when the snapshot began.
        const auto partitionReadTime =
            Timestamp{Timestamp(Date_t::now().toMillisSinceEpoch() / 1000, 0)};

        // We only keep the partition (which holds a lock) for the time needed to collect the key
        // and metric pairs
        auto currKeyMetrics = copyPartition(_queryStatsStore.getPartition(_currentPartition));

        for (auto&& metrics : currKeyMetrics) {
            const auto& keyGenerator = metrics.keyGenerator;
            const auto& hash = absl::HashOf(keyGenerator);
            try {
                auto queryStatsKey =
                    computeQueryStatsKey(keyGenerator, SerializationContext::stateDefault());
                _materializedPartition.push_back({{"key", std::move(queryStatsKey)},
                                                  {"metrics", metrics.toBSON()},
                                                  {"asOf", partitionReadTime}});
            } catch (const DBException& ex) {
                queryStatsHmacApplicationErrors.increment();
                const auto queryShape = keyGenerator->universalComponents()._queryShape->toBson(
                    pExpCtx->opCtx,
                    SerializationOptions::kRepresentativeQueryShapeSerializeOptions,
                    SerializationContext::stateDefault());
                LOGV2_DEBUG(7349403,
                            3,
                            "Error encountered when applying hmac to query shape, will not publish "
                            "queryStats for this entry.",
                            "status"_attr = ex.toStatus(),
                            "hash"_attr = hash,
                            "debugQueryShape"_attr = queryShape);

                if (kDebugBuild || internalQueryStatsErrorsAreCommandFatal.load()) {
                    auto keyString = std::to_string(hash);
                    tasserted(7349401,
                              str::stream() << "Was not able to re-parse queryStats key when "
                                               "reading queryStats.Status "
                                            << ex.toString() << " Hash: " << keyString
                                            << " Query Shape: " << queryShape.toString());
                }
            }
        }
    }
}

}  // namespace mongo
