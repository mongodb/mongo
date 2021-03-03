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

#include "mongo/db/timeseries/timeseries_index_schema_conversion_functions.h"

#include "mongo/db/catalog/database_holder.h"
#include "mongo/db/pipeline/document_source_internal_unpack_bucket.h"
#include "mongo/db/views/view_catalog.h"
#include "mongo/logv2/redaction.h"

namespace mongo {

namespace timeseries {

boost::optional<TimeseriesOptions> getTimeseriesOptions(OperationContext* opCtx,
                                                        const NamespaceString& nss) {
    auto viewCatalog = DatabaseHolder::get(opCtx)->getViewCatalog(opCtx, nss.db());
    if (!viewCatalog) {
        return {};
    }

    auto view = viewCatalog->lookupWithoutValidatingDurableViews(opCtx, nss.ns());
    if (!view) {
        return {};
    }

    // Return a copy of the time-series options so that we do not refer to the internal state of
    // 'viewCatalog' after it goes out of scope.
    return view->timeseries();
}

StatusWith<BSONObj> convertTimeseriesIndexSpecToBucketsIndexSpec(
    const TimeseriesOptions& timeseriesOptions, const BSONObj& timeseriesIndexSpecBSON) {
    auto timeField = timeseriesOptions.getTimeField();
    auto metaField = timeseriesOptions.getMetaField();

    BSONObjBuilder builder;
    for (const auto& elem : timeseriesIndexSpecBSON) {
        if (elem.fieldNameStringData() == timeField) {
            // The index requested on the time field must be a number for an ascending or descending
            // index specification. Note: further validation is expected of the caller, such as
            // eventually calling index_key_validate::validateKeyPattern() on the spec.
            if (!elem.isNumber()) {
                return {ErrorCodes::BadValue,
                        str::stream()
                            << "Invalid index spec for time-series collection: "
                            << redact(timeseriesIndexSpecBSON)
                            << ". Indexes on the time field must be ascending or descending "
                               "(numbers only): "
                            << elem};
            }

            // The time-series index on the 'timeField' is converted into a compound time index on
            // the buckets collection for more efficient querying of buckets.
            if (elem.number() >= 0) {
                builder.appendAs(elem, str::stream() << "control.min." << timeField);
                builder.appendAs(elem, str::stream() << "control.max." << timeField);
            } else {
                builder.appendAs(elem, str::stream() << "control.max." << timeField);
                builder.appendAs(elem, str::stream() << "control.min." << timeField);
            }
            continue;
        }

        if (!metaField) {
            return {ErrorCodes::BadValue,
                    str::stream() << "Invalid index spec for time-series collection: "
                                  << redact(timeseriesIndexSpecBSON) << ". Index must be on the '"
                                  << timeField << "' field: " << elem};
        }

        if (elem.fieldNameStringData() == *metaField) {
            // The time-series 'metaField' field name always maps to a field named
            // BucketUnpacker::kBucketMetaFieldName on the underlying buckets collection.
            builder.appendAs(elem, BucketUnpacker::kBucketMetaFieldName);
            continue;
        }

        // Lastly, time-series indexes on sub-documents of the 'metaField' are allowed.
        if (elem.fieldNameStringData().startsWith(*metaField + ".")) {
            builder.appendAs(elem,
                             str::stream()
                                 << BucketUnpacker::kBucketMetaFieldName << "."
                                 << elem.fieldNameStringData().substr(metaField->size() + 1));
            continue;
        }

        return {ErrorCodes::BadValue,
                str::stream() << "Invalid index spec for time-series collection: "
                              << redact(timeseriesIndexSpecBSON)
                              << ". Index must be either on the '" << *metaField << "' or '"
                              << timeField << "' fields: " << elem};
    }

    return builder.obj();
}

}  // namespace timeseries
}  // namespace mongo
