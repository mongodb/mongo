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
auto& queryStatsHmacApplicationErrors =
    *MetricBuilder<Counter64>{"queryStats.numHmacApplicationErrors"};
}

REGISTER_DOCUMENT_SOURCE_WITH_FEATURE_FLAG(queryStats,
                                           DocumentSourceQueryStats::LiteParsed::parse,
                                           DocumentSourceQueryStats::createFromBson,
                                           AllowedWithApiStrict::kNeverInVersion1,
                                           feature_flags::gFeatureFlagQueryStats);

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

}  // namespace

BSONObj DocumentSourceQueryStats::computeQueryStatsKey(
    std::shared_ptr<const Key> key, const SerializationContext& serializationContext) const {
    static const auto sha256HmacStringDataHasher = [](std::string key, StringData sd) {
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
    return key->toBson(pExpCtx->opCtx, opts, serializationContext);
}

std::unique_ptr<DocumentSourceQueryStats::LiteParsed> DocumentSourceQueryStats::LiteParsed::parse(
    const NamespaceString& nss, const BSONElement& spec) {
    return parseSpec(spec, [&](TransformAlgorithmEnum algorithm, std::string hmacKey) {
        return std::make_unique<DocumentSourceQueryStats::LiteParsed>(
            spec.fieldName(), nss.tenantId(), algorithm, hmacKey);
    });
}

boost::intrusive_ptr<DocumentSource> DocumentSourceQueryStats::createFromBson(
    BSONElement spec, const boost::intrusive_ptr<ExpressionContext>& pExpCtx) {
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
    auto hmacKey = opts.serializeLiteral(
        BSONBinData(_hmacKey.c_str(), _hmacKey.size(), BinDataType::Sensitive));
    if (opts.literalPolicy == LiteralSerializationPolicy::kToRepresentativeParseableValue) {
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

DocumentSource::GetNextResult DocumentSourceQueryStats::doGetNext() {
    const auto shouldLog = _algorithm != TransformAlgorithmEnum::kNone;
    /**
     * When a CopiedPartition is present (loaded) and contains more elements (QueryStatsEntry), we
     * can process and return the next element in the _currentCopiedPartition.
     *
     * When the current CopiedPartition is exhausted (emptied), we move on to the next
     * partition. Once we have iterated to the end of the valid partitions, we are done iteratiing
     * over all the queryStatsStore entries.
     *
     * We iterate over a copied container (CopiedParitition) containing the entries in
     * the partition to reduce the time under which the partition lock is held.
     */
    auto& queryStatsStore = getQueryStatsStore(getContext()->opCtx);

    while (_currentCopiedPartition.isValidPartitionId(queryStatsStore.numPartitions())) {
        if (!_currentCopiedPartition.isLoaded()) {
            _currentCopiedPartition.load(queryStatsStore);
        }
        // CopiedPartition::load() will throw if any errors occur.
        // Safe to assume _currentCopiedPartition is now loaded.

        // Exhaust all elements in the current copied partition.
        // Use a while loop here to handle cases where toDocument() may fail for a specific
        // QueryStatsEntry, in which case we suppress the thrown exception and continue
        // iterating to the next available entry.
        while (!_currentCopiedPartition.empty()) {
            auto& statsEntries = _currentCopiedPartition.statsEntries;
            const auto& queryStatsEntry = statsEntries.front();
            ON_BLOCK_EXIT([&statsEntries]() { statsEntries.pop_front(); });
            if (auto doc =
                    toDocument(_currentCopiedPartition.getReadTimestamp(), queryStatsEntry)) {
                if (shouldLog) {
                    LOGV2_DEBUG_OPTIONS(7808301,
                                        3,
                                        {logv2::LogTruncation::Disabled},
                                        "Logging all outputs of $queryStats",
                                        "thisOutput"_attr = *doc);
                }
                return std::move(*doc);
            }
        }
        // Once we have exhausted entries in this partition, move on to the next partition.
        _currentCopiedPartition.incrementPartitionId();
    }

    if (shouldLog) {
        LOGV2_DEBUG_OPTIONS(
            7808302, 3, {logv2::LogTruncation::Disabled}, "Finished logging output of $queryStats");
    }
    return DocumentSource::GetNextResult::makeEOF();
}

boost::optional<Document> DocumentSourceQueryStats::toDocument(
    const Date_t& partitionReadTime, const QueryStatsEntry& queryStatsEntry) const {
    const auto& key = queryStatsEntry.key;
    try {
        auto queryStatsKey = computeQueryStatsKey(key, SerializationContext::stateDefault());
        // We use the representative shape to generate the key hash. This avoids returning duplicate
        // hashes if we have bugs that cause two different representative shapes to re-parse into
        // the same debug shape.
        auto representativeShapeKey =
            key->toBson(pExpCtx->opCtx,
                        SerializationOptions::kRepresentativeQueryShapeSerializeOptions,
                        SerializationContext::stateDefault());

        // This SHA256 version of the hash is output to aid in data analytics use cases. In these
        // cases, we often care about comparing hashes from different hosts, potentially on
        // different versions and platforms. The thinking here is that the SHA256 algorithm is more
        // stable across these different environments than the quicker 'absl::HashOf'
        // implementation.
        auto hash = SHA256Block::computeHash((const uint8_t*)representativeShapeKey.objdata(),
                                             representativeShapeKey.objsize())
                        .toString();
        return Document{
            {"key", std::move(queryStatsKey)},
            {"keyHash", hash},
            {"metrics",
             queryStatsEntry.toBSON(feature_flags::gFeatureFlagQueryStatsDataBearingNodes.isEnabled(
                 serverGlobalParams.featureCompatibility.acquireFCVSnapshot()))},
            {"asOf", partitionReadTime}};
    } catch (const DBException& ex) {
        queryStatsHmacApplicationErrors.increment();
        const auto& hash = absl::HashOf(key);
        const auto queryShape = key->universalComponents()._queryShape->toBson(
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
    return {};
}

/**
 * Loads the current CopiedPartition with copies of the QueryStatsEntries located in partition of
 * cache corresponding to the partitionId of the current CopiedPartition. This ensures that the
 * partition mutex is only held for the duration of copying.
 */
void DocumentSourceQueryStats::CopiedPartition::load(QueryStatsStore& queryStatsStore) {
    tassert(7932100,
            "Attempted to load invalid partition.",
            _partitionId < queryStatsStore.numPartitions());
    tassert(7932101, "Partition was already loaded.", !isLoaded());
    // 'statsEntries' should be empty, clear just in case.
    statsEntries.clear();

    // Capture the time at which reading the partition begins.
    _readTimestamp = Date_t::now();
    {
        // We only keep the partition (which holds a lock)
        // for the time needed to collect the metrics (QueryStatsEntry)
        const auto partition = queryStatsStore.getPartition(_partitionId);

        // Note the intentional copy of QueryStatsEntry.
        // This will give us a snapshot of all the metrics we want to report.
        for (auto&& [hash, metrics] : *partition) {
            statsEntries.push_back(metrics);
        }
    }
    _isLoaded = true;
}

bool DocumentSourceQueryStats::CopiedPartition::isLoaded() const {
    return _isLoaded;
}

void DocumentSourceQueryStats::CopiedPartition::incrementPartitionId() {
    // Ensure loaded state is reset when partitionId is incremented.
    ++_partitionId;
    _isLoaded = false;
}

bool DocumentSourceQueryStats::CopiedPartition::isValidPartitionId(
    QueryStatsStore::PartitionId maxNumPartitions) const {
    return _partitionId < maxNumPartitions;
}

const Date_t& DocumentSourceQueryStats::CopiedPartition::getReadTimestamp() const {
    return _readTimestamp;
}

bool DocumentSourceQueryStats::CopiedPartition::empty() const {
    return statsEntries.empty();
}
}  // namespace mongo
