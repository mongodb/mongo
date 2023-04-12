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

#include "mongo/db/pipeline/document_source_telemetry.h"

#include "mongo/bson/timestamp.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/debug_util.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery

namespace mongo {

REGISTER_DOCUMENT_SOURCE_WITH_FEATURE_FLAG(telemetry,
                                           DocumentSourceTelemetry::LiteParsed::parse,
                                           DocumentSourceTelemetry::createFromBson,
                                           AllowedWithApiStrict::kNeverInVersion1,
                                           feature_flags::gFeatureFlagTelemetry);

bool parseTelemetryEmbeddedObject(BSONObj embeddedObj) {
    auto fieldNameRedaction = false;
    if (!embeddedObj.isEmpty()) {
        uassert(ErrorCodes::FailedToParse,
                str::stream()
                    << DocumentSourceTelemetry::kStageName
                    << " parameters object may only contain one field, 'redactIdentifiers'. Found: "
                    << embeddedObj.toString(),
                embeddedObj.nFields() == 1);

        uassert(ErrorCodes::FailedToParse,
                str::stream()
                    << DocumentSourceTelemetry::kStageName
                    << " parameters object may only contain 'redactIdentifiers' option. Found: "
                    << embeddedObj.firstElementFieldName(),
                embeddedObj.hasField("redactIdentifiers"));

        uassert(ErrorCodes::FailedToParse,
                str::stream() << DocumentSourceTelemetry::kStageName
                              << " redactIdentifiers parameter must be boolean. Found type: "
                              << typeName(embeddedObj.firstElementType()),
                embeddedObj.firstElementType() == BSONType::Bool);
        fieldNameRedaction = embeddedObj["redactIdentifiers"].trueValue();
    }
    return fieldNameRedaction;
}

std::unique_ptr<DocumentSourceTelemetry::LiteParsed> DocumentSourceTelemetry::LiteParsed::parse(
    const NamespaceString& nss, const BSONElement& spec) {
    uassert(ErrorCodes::FailedToParse,
            str::stream() << kStageName
                          << " value must be an object. Found: " << typeName(spec.type()),
            spec.type() == BSONType::Object);

    return std::make_unique<DocumentSourceTelemetry::LiteParsed>(
        spec.fieldName(), parseTelemetryEmbeddedObject(spec.embeddedObject()));
}

boost::intrusive_ptr<DocumentSource> DocumentSourceTelemetry::createFromBson(
    BSONElement spec, const boost::intrusive_ptr<ExpressionContext>& pExpCtx) {
    uassert(ErrorCodes::FailedToParse,
            str::stream() << kStageName
                          << " value must be an object. Found: " << typeName(spec.type()),
            spec.type() == BSONType::Object);

    const NamespaceString& nss = pExpCtx->ns;

    uassert(ErrorCodes::InvalidNamespace,
            "$telemetry must be run against the 'admin' database with {aggregate: 1}",
            nss.db() == DatabaseName::kAdmin.db() && nss.isCollectionlessAggregateNS());

    return new DocumentSourceTelemetry(pExpCtx,
                                       parseTelemetryEmbeddedObject(spec.embeddedObject()));
}

Value DocumentSourceTelemetry::serialize(SerializationOptions opts) const {
    // This document source never contains any user information, so no need for any work when
    // redacting.
    return Value{Document{{kStageName, Document{}}}};
}

DocumentSource::GetNextResult DocumentSourceTelemetry::doGetNext() {
    /**
     * We maintain nested iterators:
     * - Outer one over the set of partitions.
     * - Inner one over the set of entries in a "materialized" partition.
     *
     * When an inner iterator is present and contains more elements, we can return the next element.
     * When the inner iterator is exhausted, we move to the next element in the outer iterator and
     * create a new inner iterator. When the outer iterator is exhausted, we have finished iterating
     * over the telemetry store entries.
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

        TelemetryStore& _telemetryStore = getTelemetryStore(getContext()->opCtx);

        // Materialized partition is exhausted, move to the next.
        _currentPartition++;
        if (_currentPartition >= _telemetryStore.numPartitions()) {
            return DocumentSource::GetNextResult::makeEOF();
        }

        // We only keep the partition (which holds a lock) for the time needed to materialize it to
        // a set of Document instances.
        auto&& partition = _telemetryStore.getPartition(_currentPartition);

        // Capture the time at which reading the partition begins to indicate to the caller
        // when the snapshot began.
        const auto partitionReadTime =
            Timestamp{Timestamp(Date_t::now().toMillisSinceEpoch() / 1000, 0)};
        for (auto&& [key, metrics] : *partition) {
            auto swKey = metrics->redactKey(key, _redactIdentifiers, pExpCtx->opCtx);
            if (!swKey.isOK()) {
                LOGV2_DEBUG(7349403,
                            3,
                            "Error encountered when redacting query shape, will not publish "
                            "telemetry for this entry.",
                            "status"_attr = swKey.getStatus());
                if (kDebugBuild) {
                    tasserted(7349401,
                              "Was not able to re-parse telemetry key when reading telemetry.");
                }
                continue;
            }
            _materializedPartition.push_back({{"key", std::move(swKey.getValue())},
                                              {"metrics", metrics->toBSON()},
                                              {"asOf", partitionReadTime}});
        }
    }
}

}  // namespace mongo
