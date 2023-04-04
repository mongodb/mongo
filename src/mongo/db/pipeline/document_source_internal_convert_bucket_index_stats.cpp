/**
 *    Copyright (C) 2021-present MongoDB, Inc.
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

#include "mongo/platform/basic.h"

#include "mongo/db/pipeline/document_source_internal_convert_bucket_index_stats.h"

#include "mongo/bson/bsonobj.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/list_indexes_gen.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/lite_parsed_document_source.h"
#include "mongo/db/timeseries/timeseries_constants.h"
#include "mongo/db/timeseries/timeseries_gen.h"
#include "mongo/db/timeseries/timeseries_index_schema_conversion_functions.h"

namespace mongo {

namespace {

/**
 * Maps the buckets collection $indexStats result 'bucketsIndexSpecBSON' to the $indexStats format
 * of the time-series collection using the information provided in 'bucketSpec'.
 *
 * The top-level field 'key' for the key pattern is repeated once in the $indexStats format under
 * the 'spec' field:
 *
 * {
 *     name: 'myindex',
 *     key: <key pattern>,
 *     host: 'myhost:myport',
 *     accesses: {
 *         ops: NumberLong(...),
 *         since: ISODate(...),
 *     },
 *     spec: {
 *         v: 2,
 *         key: <key pattern>,
 *         name: 'myindex'
 *     }
 * }
 *
 * The duplication of the 'key' field is due to how CommonMongodProcessInterface::getIndexStats()
 * includes both CollectionIndexUsageTracker::IndexUsageStats::indexKey and the complete index spec
 * from IndexCatalog::getEntry().
 */
BSONObj makeTimeseriesIndexStats(const TimeseriesConversionOptions& bucketSpec,
                                 const BSONObj& bucketsIndexStatsBSON) {
    TimeseriesOptions timeseriesOptions(bucketSpec.timeField);
    if (bucketSpec.metaField) {
        timeseriesOptions.setMetaField(StringData(*bucketSpec.metaField));
    }
    BSONObjBuilder builder;
    for (const auto& elem : bucketsIndexStatsBSON) {
        if (elem.fieldNameStringData() == ListIndexesReplyItem::kKeyFieldName) {
            // This field is appended below.
            continue;
        }
        if (elem.fieldNameStringData() == ListIndexesReplyItem::kSpecFieldName) {
            auto timeseriesSpec =
                timeseries::createTimeseriesIndexFromBucketsIndex(timeseriesOptions, elem.Obj());
            if (!timeseriesSpec) {
                return {};
            }

            builder.append(ListIndexesReplyItem::kSpecFieldName, *timeseriesSpec);
            builder.append(ListIndexesReplyItem::kKeyFieldName,
                           timeseriesSpec->getObjectField(IndexDescriptor::kKeyPatternFieldName));
            continue;
        }
        builder.append(elem);
    }
    return builder.obj();
}

}  // namespace

REGISTER_DOCUMENT_SOURCE(_internalConvertBucketIndexStats,
                         LiteParsedDocumentSourceDefault::parse,
                         DocumentSourceInternalConvertBucketIndexStats::createFromBson,
                         AllowedWithApiStrict::kInternal);

DocumentSourceInternalConvertBucketIndexStats::DocumentSourceInternalConvertBucketIndexStats(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    TimeseriesConversionOptions timeseriesOptions)
    : DocumentSource(kStageName, expCtx), _timeseriesOptions(std::move(timeseriesOptions)) {}

boost::intrusive_ptr<DocumentSource> DocumentSourceInternalConvertBucketIndexStats::createFromBson(
    BSONElement specElem, const boost::intrusive_ptr<ExpressionContext>& expCtx) {
    uassert(5480000,
            "$_internalConvertBucketIndexStats specification must be an object",
            specElem.type() == Object);

    TimeseriesConversionOptions timeseriesOptions;
    for (auto&& elem : specElem.embeddedObject()) {
        auto fieldName = elem.fieldNameStringData();
        if (fieldName == timeseries::kTimeFieldName) {
            uassert(5480001, "timeField field must be a string", elem.type() == BSONType::String);
            timeseriesOptions.timeField = elem.str();
        } else if (fieldName == timeseries::kMetaFieldName) {
            uassert(5480002,
                    str::stream() << "metaField field must be a string, got: " << elem.type(),
                    elem.type() == BSONType::String);
            timeseriesOptions.metaField = elem.str();
        } else {
            uasserted(5480003,
                      str::stream()
                          << "unrecognized parameter to $_internalConvertBucketIndexStats: "
                          << fieldName);
        }
    }

    // Check that none of the required arguments are missing.
    uassert(5480004,
            "The $_internalConvertBucketIndexStats stage requires a timeField parameter",
            specElem[timeseries::kTimeFieldName].ok());

    return make_intrusive<DocumentSourceInternalConvertBucketIndexStats>(
        expCtx, std::move(timeseriesOptions));
}

Value DocumentSourceInternalConvertBucketIndexStats::serialize(SerializationOptions opts) const {
    if (opts.redactIdentifiers || opts.replacementForLiteralArgs) {
        MONGO_UNIMPLEMENTED_TASSERT(7484337);
    }

    MutableDocument out;
    out.addField(timeseries::kTimeFieldName, Value{_timeseriesOptions.timeField});
    if (_timeseriesOptions.metaField) {
        out.addField(timeseries::kMetaFieldName, Value{*_timeseriesOptions.metaField});
    }
    return Value(DOC(getSourceName() << out.freeze()));
}

DocumentSource::GetNextResult DocumentSourceInternalConvertBucketIndexStats::doGetNext() {
    auto nextResult = pSource->getNext();
    if (nextResult.isAdvanced()) {
        auto bucketStats = nextResult.getDocument().toBson();

        // Convert $indexStats results to the time-series schema.
        auto timeseriesStats = makeTimeseriesIndexStats(_timeseriesOptions, bucketStats);
        // Skip this index if the conversion failed.
        if (timeseriesStats.isEmpty()) {
            return GetNextResult::makePauseExecution();
        }
        return Document(timeseriesStats);
    }

    return nextResult;
}
}  // namespace mongo
