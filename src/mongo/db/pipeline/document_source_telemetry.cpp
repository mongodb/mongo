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

namespace mongo {
namespace {
const StringData kClearEntriesFieldName = "clearEntries"_sd;
}  // namespace

REGISTER_DOCUMENT_SOURCE(telemetry,
                         DocumentSourceTelemetry::LiteParsed::parse,
                         DocumentSourceTelemetry::createFromBson,
                         AllowedWithApiStrict::kNeverInVersion1);

std::unique_ptr<DocumentSourceTelemetry::LiteParsed> DocumentSourceTelemetry::LiteParsed::parse(
    const NamespaceString& nss, const BSONElement& spec) {
    uassert(ErrorCodes::FailedToParse,
            str::stream() << kStageName
                          << " value must be an object. Found: " << typeName(spec.type()),
            spec.type() == BSONType::Object);

    auto clearEntries = false;

    for (auto&& elem : spec.embeddedObject()) {
        const auto fieldName = elem.fieldNameStringData();
        if (fieldName == "clearEntries"_sd) {
            uassert(
                ErrorCodes::TypeMismatch,
                str::stream() << "The 'clearEntries' parameter of the $telemetry stage must be a "
                                 "boolean. Instead found: "
                              << typeName(elem.type()),
                elem.type() == BSONType::Bool);
            clearEntries = elem.boolean();
        } else {
            uasserted(ErrorCodes::FailedToParse,
                      str::stream()
                          << "Unrecognized option '" << fieldName << "' in $telemetry stage.");
        }
    }

    return std::make_unique<DocumentSourceTelemetry::LiteParsed>(spec.fieldName(), clearEntries);
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
            nss.db() == NamespaceString::kAdminDb && nss.isCollectionlessAggregateNS());

    auto clearEntries = false;

    for (auto&& elem : spec.embeddedObject()) {
        const auto fieldName = elem.fieldNameStringData();
        if (fieldName == "clearEntries"_sd) {
            uassert(
                ErrorCodes::TypeMismatch,
                str::stream() << "The 'clearEntries' parameter of the $telemetry stage must be a "
                                 "boolean. Instead found: "
                              << typeName(elem.type()),
                elem.type() == BSONType::Bool);
            clearEntries = elem.boolean();
        } else {
            uasserted(ErrorCodes::FailedToParse,
                      str::stream()
                          << "Unrecognized option '" << fieldName << "' in $telemetry stage.");
        }
    }

    return new DocumentSourceTelemetry(pExpCtx, clearEntries);
}

Value DocumentSourceTelemetry::serialize(boost::optional<ExplainOptions::Verbosity> explain) const {
    return Value{Document{{kStageName, Document{{kClearEntriesFieldName, Value(_clearEntries)}}}}};
}

void DocumentSourceTelemetry::buildTelemetryStoreIterator() {
    TelemetryStore* telemetryStore = [&]() {
        if (_clearEntries) {
            // Save the telemetry store to a member variable to be destroyed with the document
            // source.
            _telemetryStore = resetTelemetryStore(getContext()->opCtx->getServiceContext());
            return &**_telemetryStore;
        } else {
            return getTelemetryStoreForRead(getContext()->opCtx->getServiceContext()).first;
        }
    }();

    // Here we start a new thread which runs until the document source finishes iterating the
    // telemetry store.
    stdx::thread producer([&, telemetryStore] {
        telemetryStore->forEachPartition(
            [&](const std::function<TelemetryStore::Partition()>& getPartition) {
                // Block here waiting for the queue to be empty. Locking the partition will block
                // telemetry writers. We want to delay lock acquisition as long as possible.
                _queue.waitForEmpty();

                // Now get the locked partition.
                auto partition = getPartition();

                // Capture the time at which reading the partition begins to indicate to the caller
                // when the snapshot began.
                const auto partitionReadTime =
                    Timestamp{Timestamp(Date_t::now().toMillisSinceEpoch() / 1000, 0)};
                for (auto&& [key, metrics] : *partition) {
                    Document d{
                        {{"key", key}, {"metrics", metrics.toBSON()}, {"asOf", partitionReadTime}}};
                    _queue.push(std::move(d));
                }
            });
        _queue.closeProducerEnd();
    });
    producer.detach();
}

DocumentSource::GetNextResult DocumentSourceTelemetry::doGetNext() {
    if (!_initialized) {
        buildTelemetryStoreIterator();
        _initialized = true;
    }

    auto maybeResult = _queue.pop();
    if (maybeResult) {
        return {std::move(*maybeResult)};
    } else {
        return DocumentSource::GetNextResult::makeEOF();
    }
}

}  // namespace mongo
