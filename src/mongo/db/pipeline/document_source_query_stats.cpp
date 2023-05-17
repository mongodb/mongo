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

#include "mongo/bson/bsontypes.h"
#include "mongo/bson/timestamp.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/debug_util.h"

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
 * Try to parse the applyHmacToIdentifiers property from the element.
 */
boost::optional<bool> parseApplyHmacToIdentifiers(const BSONElement& el) {
    if (el.fieldNameStringData() == "applyHmacToIdentifiers"_sd) {
        auto type = el.type();
        uassert(ErrorCodes::FailedToParse,
                str::stream() << DocumentSourceQueryStats::kStageName
                              << " applyHmacToIdentifiers parameter must be boolean. Found type: "
                              << typeName(type),
                type == BSONType::Bool);
        return el.trueValue();
    }
    return boost::none;
}

/**
 * Try to parse the `hmacKey' property from the element.
 */
boost::optional<std::string> parseHmacKey(const BSONElement& el) {
    if (el.fieldNameStringData() == "hmacKey"_sd) {
        auto type = el.type();
        if (el.isBinData(BinDataType::BinDataGeneral)) {
            int len;
            auto data = el.binData(len);
            uassert(ErrorCodes::FailedToParse,
                    str::stream() << DocumentSourceQueryStats::kStageName
                                  << "hmacKey must be greater than or equal to 32 bytes",
                    len >= 32);
            return {{data, (size_t)len}};
        }
        uasserted(ErrorCodes::FailedToParse,
                  str::stream()
                      << DocumentSourceQueryStats::kStageName
                      << " hmacKey parameter must be bindata of length 32 or greater. Found type: "
                      << typeName(type));
    }
    return boost::none;
}

/**
 * Parse the spec object calling the `ctor` with the bool applyHmacToIdentifiers and std::string
 * hmacKey arguments.
 */
template <typename Ctor>
auto parseSpec(const BSONElement& spec, const Ctor& ctor) {
    uassert(ErrorCodes::FailedToParse,
            str::stream() << DocumentSourceQueryStats::kStageName
                          << " value must be an object. Found: " << typeName(spec.type()),
            spec.type() == BSONType::Object);

    bool applyHmacToIdentifiers = false;
    std::string hmacKey;
    for (auto&& el : spec.embeddedObject()) {
        if (auto maybeApplyHmacToIdentifiers = parseApplyHmacToIdentifiers(el);
            maybeApplyHmacToIdentifiers) {
            applyHmacToIdentifiers = *maybeApplyHmacToIdentifiers;
        } else if (auto maybeHmacKey = parseHmacKey(el); maybeHmacKey) {
            hmacKey = *maybeHmacKey;
        } else {
            uasserted(ErrorCodes::FailedToParse,
                      str::stream()
                          << DocumentSourceQueryStats::kStageName
                          << " parameters object may only contain 'applyHmacToIdentifiers' or "
                             "'hmacKey' options. Found: "
                          << el.fieldName());
        }
    }

    return ctor(applyHmacToIdentifiers, hmacKey);
}

}  // namespace

std::unique_ptr<DocumentSourceQueryStats::LiteParsed> DocumentSourceQueryStats::LiteParsed::parse(
    const NamespaceString& nss, const BSONElement& spec) {
    return parseSpec(spec, [&](bool applyHmacToIdentifiers, std::string hmacKey) {
        return std::make_unique<DocumentSourceQueryStats::LiteParsed>(
            spec.fieldName(), applyHmacToIdentifiers, hmacKey);
    });
}

boost::intrusive_ptr<DocumentSource> DocumentSourceQueryStats::createFromBson(
    BSONElement spec, const boost::intrusive_ptr<ExpressionContext>& pExpCtx) {
    const NamespaceString& nss = pExpCtx->ns;

    uassert(ErrorCodes::InvalidNamespace,
            "$queryStats must be run against the 'admin' database with {aggregate: 1}",
            nss.db() == DatabaseName::kAdmin.db() && nss.isCollectionlessAggregateNS());

    return parseSpec(spec, [&](bool applyHmacToIdentifiers, std::string hmacKey) {
        return new DocumentSourceQueryStats(pExpCtx, applyHmacToIdentifiers, hmacKey);
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
                auto queryStatsKey = metrics->computeQueryStatsKey(
                    pExpCtx->opCtx, _applyHmacToIdentifiers, _hmacKey);
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
                            "hash"_attr = key);
                if (kDebugBuild) {
                    tasserted(7349401,
                              "Was not able to re-parse queryStats key when reading queryStats.");
                }
            }
        }
    }
}

}  // namespace mongo
