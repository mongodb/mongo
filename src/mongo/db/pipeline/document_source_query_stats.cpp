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
#include "mongo/db/catalog/util/partitioned.h"
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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery

namespace mongo {
namespace {
CounterMetric queryStatsHmacApplicationErrors("queryStats.numHmacApplicationErrors");
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
        if (hmacKeyContainer) {
            hmacKey = std::string(hmacKeyContainer->data(), (size_t)hmacKeyContainer->length());
        }
    }
    return ctor(algorithm, hmacKey);
}

}  // namespace

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

    return parseSpec(spec, [&](TransformAlgorithmEnum algorithm, std::string hmacKey) {
        return new DocumentSourceQueryStats(pExpCtx, algorithm, hmacKey);
    });
}

Value DocumentSourceQueryStats::serialize(SerializationOptions opts) const {
    // This document source never contains any user information, so no need for any work when
    // applying hmac.
    return Value{Document{{kStageName, Document{}}}};
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
    while (true) {
        // First, attempt to exhaust all elements in the materialized partition.
        if (!_materializedPartition.empty()) {
            // Move out of the container reference.
            auto doc = std::move(_materializedPartition.front());
            _materializedPartition.pop_front();
            return {std::move(doc)};
        }

        QueryStatsStore& _queryStatsStore = getQueryStatsStore(getContext()->opCtx);

        // Materialized partition is exhausted, move to the next.
        _currentPartition++;
        if (_currentPartition >= _queryStatsStore.numPartitions()) {
            return DocumentSource::GetNextResult::makeEOF();
        }

        // We only keep the partition (which holds a lock) for the time needed to materialize it to
        // a set of Document instances.
        auto&& partition = _queryStatsStore.getPartition(_currentPartition);

        // Capture the time at which reading the partition begins to indicate to the caller
        // when the snapshot began.
        const auto partitionReadTime =
            Timestamp{Timestamp(Date_t::now().toMillisSinceEpoch() / 1000, 0)};
        for (auto&& [key, metrics] : *partition) {
            try {
                auto queryStatsKey =
                    metrics->computeQueryStatsKey(pExpCtx->opCtx, _algorithm, _hmacKey);
                _materializedPartition.push_back({{"key", std::move(queryStatsKey)},
                                                  {"metrics", metrics->toBSON()},
                                                  {"asOf", partitionReadTime}});
            } catch (const DBException& ex) {
                queryStatsHmacApplicationErrors.increment();
                LOGV2_DEBUG(7349403,
                            3,
                            "Error encountered when applying hmac to query shape, will not publish "
                            "queryStats for this entry.",
                            "status"_attr = ex.toStatus(),
                            "hash"_attr = *key,
                            "representativeQueryShape"_attr =
                                metrics->getRepresentativeQueryShapeForDebug());
                if (kDebugBuild || internalQueryStatsErrorsAreCommandFatal.load()) {
                    auto keyString = std::to_string(*key);
                    auto queryShape = metrics->getRepresentativeQueryShapeForDebug();
                    tasserted(7349401,
                              "Was not able to re-parse queryStats key when reading queryStats. "
                              "Status: " +
                                  ex.toString() + " Hash: " + keyString +
                                  " Query Shape: " + queryShape.toString());
                }
            }
        }
    }
}

}  // namespace mongo
