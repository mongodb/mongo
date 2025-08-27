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

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {

REGISTER_DOCUMENT_SOURCE(_internalConvertBucketIndexStats,
                         LiteParsedDocumentSourceDefault::parse,
                         DocumentSourceInternalConvertBucketIndexStats::createFromBson,
                         AllowedWithApiStrict::kInternal);
ALLOCATE_DOCUMENT_SOURCE_ID(_internalConvertBucketIndexStats,
                            DocumentSourceInternalConvertBucketIndexStats::id)

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
    const SerializationOptions& opts) const {
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
