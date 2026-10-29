// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/pipeline/document_source_internal_convert_bucket_index_stats.h"

#include "mongo/bson/bsontypes.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/lite_parsed_document_source.h"
#include "mongo/db/query/allowed_contexts.h"
#include "mongo/db/timeseries/timeseries_constants.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/intrusive_counter.h"
#include "mongo/util/str.h"

#include <utility>

#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {

REGISTER_LITE_PARSED_DOCUMENT_SOURCE(_internalConvertBucketIndexStats,
                                     InternalConvertBucketIndexStatsLiteParsed::parse,
                                     AllowedWithApiStrict::kInternal);

REGISTER_DOCUMENT_SOURCE_WITH_STAGE_PARAMS_DEFAULT(_internalConvertBucketIndexStats,
                                                   DocumentSourceInternalConvertBucketIndexStats,
                                                   InternalConvertBucketIndexStatsStageParams);

ALLOCATE_DOCUMENT_SOURCE_ID(_internalConvertBucketIndexStats,
                            DocumentSourceInternalConvertBucketIndexStats::id);

DocumentSourceInternalConvertBucketIndexStats::DocumentSourceInternalConvertBucketIndexStats(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    TimeseriesIndexConversionOptions timeseriesOptions)
    : DocumentSource(kStageName, expCtx), _timeseriesOptions(std::move(timeseriesOptions)) {}

boost::intrusive_ptr<DocumentSource> DocumentSourceInternalConvertBucketIndexStats::createFromBson(
    BSONElement specElem, const boost::intrusive_ptr<ExpressionContext>& expCtx) {
    uassert(5480000,
            "$_internalConvertBucketIndexStats specification must be an object",
            specElem.type() == BSONType::object);

    TimeseriesIndexConversionOptions timeseriesOptions;
    for (auto&& elem : specElem.embeddedObject()) {
        auto fieldName = elem.fieldNameStringData();
        if (fieldName == timeseries::kTimeFieldName) {
            uassert(5480001, "timeField field must be a string", elem.type() == BSONType::string);
            timeseriesOptions.timeField = elem.str();
        } else if (fieldName == timeseries::kMetaFieldName) {
            uassert(5480002,
                    str::stream() << "metaField field must be a string, got: " << elem.type(),
                    elem.type() == BSONType::string);
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

Value DocumentSourceInternalConvertBucketIndexStats::serialize(
    const query_shape::SerializationOptions& opts) const {
    MutableDocument out;
    out.addField(timeseries::kTimeFieldName,
                 Value{opts.serializeFieldPathFromString(_timeseriesOptions.timeField)});
    if (_timeseriesOptions.metaField) {
        out.addField(timeseries::kMetaFieldName,
                     Value{opts.serializeFieldPathFromString(*_timeseriesOptions.metaField)});
    }
    return Value(DOC(getSourceName() << out.freeze()));
}

}  // namespace mongo
