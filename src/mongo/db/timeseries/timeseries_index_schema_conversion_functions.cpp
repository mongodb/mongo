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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kStorage

#include "mongo/db/timeseries/timeseries_index_schema_conversion_functions.h"

#include "mongo/db/timeseries/timeseries_constants.h"
#include "mongo/db/timeseries/timeseries_gen.h"
#include "mongo/logv2/log.h"
#include "mongo/logv2/redaction.h"

namespace mongo::timeseries {

namespace {
StatusWith<BSONObj> createBucketsSpecFromTimeseriesSpec(const TimeseriesOptions& timeseriesOptions,
                                                        const BSONObj& timeseriesIndexSpecBSON,
                                                        bool isShardKeySpec) {
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
                builder.appendAs(
                    elem, str::stream() << timeseries::kControlMinFieldNamePrefix << timeField);
                if (!isShardKeySpec) {
                    builder.appendAs(
                        elem, str::stream() << timeseries::kControlMaxFieldNamePrefix << timeField);
                }
            } else {
                builder.appendAs(
                    elem, str::stream() << timeseries::kControlMaxFieldNamePrefix << timeField);
                builder.appendAs(
                    elem, str::stream() << timeseries::kControlMinFieldNamePrefix << timeField);
            }
            continue;
        }

        if (!metaField) {
            return {ErrorCodes::BadValue,
                    str::stream() << "Invalid index spec for time-series collection: "
                                  << redact(timeseriesIndexSpecBSON)
                                  << ". Indexes are only allowed on the '" << timeField
                                  << "' field, no other data fields are supported: " << elem};
        }

        if (elem.fieldNameStringData() == *metaField) {
            // The time-series 'metaField' field name always maps to a field named
            // timeseries::kBucketMetaFieldName on the underlying buckets collection.
            builder.appendAs(elem, timeseries::kBucketMetaFieldName);
            continue;
        }

        // Lastly, time-series indexes on sub-documents of the 'metaField' are allowed.
        if (elem.fieldNameStringData().startsWith(*metaField + ".")) {
            builder.appendAs(elem,
                             str::stream()
                                 << timeseries::kBucketMetaFieldName << "."
                                 << elem.fieldNameStringData().substr(metaField->size() + 1));
            continue;
        }

        return {ErrorCodes::BadValue,
                str::stream() << "Invalid index spec for time-series collection: "
                              << redact(timeseriesIndexSpecBSON)
                              << ". Indexes are only supported on the '" << *metaField << "' and '"
                              << timeField << "' fields: " << elem};
    }

    return builder.obj();
}
}  // namespace

StatusWith<BSONObj> createBucketsIndexSpecFromTimeseriesIndexSpec(
    const TimeseriesOptions& timeseriesOptions, const BSONObj& timeseriesIndexSpecBSON) {
    return createBucketsSpecFromTimeseriesSpec(timeseriesOptions, timeseriesIndexSpecBSON, false);
}

StatusWith<BSONObj> createBucketsShardKeySpecFromTimeseriesShardKeySpec(
    const TimeseriesOptions& timeseriesOptions, const BSONObj& timeseriesShardKeySpecBSON) {
    return createBucketsSpecFromTimeseriesSpec(timeseriesOptions, timeseriesShardKeySpecBSON, true);
}

boost::optional<BSONObj> createTimeseriesIndexSpecFromBucketsIndexSpec(
    const TimeseriesOptions& timeseriesOptions, const BSONObj& bucketsIndexSpecBSON) {
    auto timeField = timeseriesOptions.getTimeField();
    auto metaField = timeseriesOptions.getMetaField();

    const std::string controlMinTimeField = str::stream()
        << timeseries::kControlMinFieldNamePrefix << timeField;
    const std::string controlMaxTimeField = str::stream()
        << timeseries::kControlMaxFieldNamePrefix << timeField;

    BSONObjBuilder builder;
    for (const auto& elem : bucketsIndexSpecBSON) {
        // The index specification on the time field is ascending or descending.
        if (elem.fieldNameStringData() == controlMinTimeField) {
            if (!elem.isNumber()) {
                // This index spec on the underlying buckets collection is not valid for
                // time-series. Therefore, we will not convert the index spec.
                return {};
            }

            builder.appendAs(elem, timeField);
            continue;
        } else if (elem.fieldNameStringData() == controlMaxTimeField) {
            // Skip 'control.max.<timeField>' since the 'control.min.<timeField>' field is
            // sufficient to determine whether the index is ascending or descending.
            continue;
        }

        if (!metaField) {
            // 'elem' is an invalid index spec field for this time-series collection. It does not
            // match the time field and there is no metaField set. Therefore, we will not convert
            // the index spec.
            return {};
        }

        if (elem.fieldNameStringData() == timeseries::kBucketMetaFieldName) {
            builder.appendAs(elem, *metaField);
            continue;
        }

        if (elem.fieldNameStringData().startsWith(timeseries::kBucketMetaFieldName + ".")) {
            builder.appendAs(elem,
                             str::stream() << *metaField << "."
                                           << elem.fieldNameStringData().substr(
                                                  timeseries::kBucketMetaFieldName.size() + 1));
            continue;
        }

        // 'elem' is an invalid index spec field for this time-series collection. It matches neither
        // the time field  nor the metaField field. Therefore, we will not convert the index spec.
        return {};
    }

    return builder.obj();
}

boost::optional<BSONObj> createTimeseriesIndexFromBucketsIndex(
    const TimeseriesOptions& timeseriesOptions, const BSONObj& bucketsIndex) {
    if (bucketsIndex.hasField(kKeyFieldName)) {
        auto timeseriesKeyValue = createTimeseriesIndexSpecFromBucketsIndexSpec(
            timeseriesOptions, bucketsIndex.getField(kKeyFieldName).Obj());
        if (timeseriesKeyValue) {
            // This creates a bsonobj copy with a modified kKeyFieldName field set to
            // timeseriesKeyValue.
            return bucketsIndex.addFields(BSON(kKeyFieldName << timeseriesKeyValue.get()),
                                          StringDataSet{kKeyFieldName});
        }
    }
    return boost::none;
}

std::list<BSONObj> createTimeseriesIndexesFromBucketsIndexes(
    const TimeseriesOptions& timeseriesOptions, const std::list<BSONObj>& bucketsIndexes) {
    std::list<BSONObj> indexSpecs;
    for (const auto& bucketsIndex : bucketsIndexes) {
        auto timeseriesIndex =
            createTimeseriesIndexFromBucketsIndex(timeseriesOptions, bucketsIndex);
        if (timeseriesIndex) {
            indexSpecs.push_back(timeseriesIndex->getOwned());
        }
    }
    return indexSpecs;
}

}  // namespace mongo::timeseries
