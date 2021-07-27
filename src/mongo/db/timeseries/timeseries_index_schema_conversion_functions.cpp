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

#include "mongo/platform/basic.h"

#include "mongo/db/timeseries/timeseries_index_schema_conversion_functions.h"

#include "mongo/db/storage/storage_parameters_gen.h"
#include "mongo/db/timeseries/timeseries_constants.h"
#include "mongo/db/timeseries/timeseries_gen.h"
#include "mongo/logv2/log.h"
#include "mongo/logv2/redaction.h"

namespace mongo {

namespace timeseries {

namespace {

bool isIndexOnControl(const StringData& field) {
    return field.startsWith(timeseries::kControlMinFieldNamePrefix) ||
        field.startsWith(timeseries::kControlMaxFieldNamePrefix);
}

/**
 * Takes the index specification field name, such as 'control.max.x.y', or 'control.min.z' and
 * returns a pair of the prefix ('control.min.' or 'control.max.') and key ('x.y' or 'z').
 */
std::pair<std::string, std::string> extractControlPrefixAndKey(const StringData& field) {
    // Can't use rfind() due to dotted fields such as 'control.max.x.y'.
    size_t numDotsFound = 0;
    auto fieldIt = std::find_if(field.begin(), field.end(), [&numDotsFound](const char c) {
        if (c == '.') {
            numDotsFound++;
        }

        return numDotsFound == 2;
    });

    invariant(numDotsFound == 2 && fieldIt != field.end());
    return {std::string(field.begin(), fieldIt + 1), std::string(fieldIt + 1, field.end())};
}

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

        if (metaField) {
            if (elem.fieldNameStringData() == *metaField) {
                // The time-series 'metaField' field name always maps to a field named
                // timeseries::kBucketMetaFieldName on the underlying buckets collection.
                builder.appendAs(elem, timeseries::kBucketMetaFieldName);
                continue;
            }

            // Time-series indexes on sub-documents of the 'metaField' are allowed.
            if (elem.fieldNameStringData().startsWith(*metaField + ".")) {
                builder.appendAs(elem,
                                 str::stream()
                                     << timeseries::kBucketMetaFieldName << "."
                                     << elem.fieldNameStringData().substr(metaField->size() + 1));
                continue;
            }
        }

        if (!feature_flags::gTimeseriesMetricIndexes.isEnabledAndIgnoreFCV()) {
            auto reason = str::stream();
            reason << "Invalid index spec for time-series collection: "
                   << redact(timeseriesIndexSpecBSON) << ". ";
            reason << "Indexes are only supported on the '" << timeField << "' ";
            if (metaField) {
                reason << "and '" << *metaField << "' fields. ";
            } else {
                reason << "field. ";
            }
            reason << "Attempted to create an index on the field '" << elem.fieldName() << "'.";
            return {ErrorCodes::BadValue, reason};
        }

        // Indexes on measurement fields are only supported when the 'gTimeseriesMetricIndexes'
        // feature flag is enabled.
        if (!elem.isNumber()) {
            return {
                ErrorCodes::BadValue,
                str::stream() << "Invalid index spec for time-series collection: "
                              << redact(timeseriesIndexSpecBSON)
                              << ". Indexes on measurement fields must be ascending or descending "
                                 "(numbers only): "
                              << elem};
        }

        if (elem.number() >= 0) {
            // For ascending key patterns, the { control.max.elem: 1, control.min.elem: 1 }
            // compound index is created.
            builder.appendAs(
                elem, str::stream() << timeseries::kControlMaxFieldNamePrefix << elem.fieldName());
            builder.appendAs(
                elem, str::stream() << timeseries::kControlMinFieldNamePrefix << elem.fieldName());
        } else if (elem.number() < 0) {
            // For descending key patterns, the { control.min.elem: -1, control.max.elem: -1 }
            // compound index is created.
            builder.appendAs(
                elem, str::stream() << timeseries::kControlMinFieldNamePrefix << elem.fieldName());
            builder.appendAs(
                elem, str::stream() << timeseries::kControlMaxFieldNamePrefix << elem.fieldName());
        }
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
    for (auto elemIt = bucketsIndexSpecBSON.begin(); elemIt != bucketsIndexSpecBSON.end();
         ++elemIt) {
        const auto& elem = *elemIt;
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

        if (metaField) {
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
        }

        if (!feature_flags::gTimeseriesMetricIndexes.isEnabledAndIgnoreFCV()) {
            // 'elem' is an invalid index spec field for this time-series collection. It matches
            // neither the time field nor the metaField field. Therefore, we will not convert the
            // index spec.
            return {};
        }

        if (!isIndexOnControl(elem.fieldNameStringData())) {
            // Only indexes on the control field are allowed beyond this point. We will not convert
            // the index spec.
            return {};
        }

        // Indexes on measurement fields are built as compound indexes on the two 'control.min' and
        // 'control.max' fields. We use the BSON iterator to lookahead when doing the reverse
        // mapping for these indexes.
        const auto firstOrdering = elem.number();
        std::string firstControlFieldPrefix;
        std::string firstControlFieldKey;
        std::tie(firstControlFieldPrefix, firstControlFieldKey) =
            extractControlPrefixAndKey(elem.fieldNameStringData());

        elemIt++;
        if (elemIt == bucketsIndexSpecBSON.end()) {
            // This measurement index spec on the underlying buckets collection is not valid for
            // time-series as the compound index is incomplete. We will not convert the index spec.
            return {};
        }

        const auto& nextElem = *elemIt;
        if (!isIndexOnControl(nextElem.fieldNameStringData())) {
            // Only indexes on the control field are allowed beyond this point. We will not convert
            // the index spec.
            return {};
        }

        const auto secondOrdering = nextElem.number();
        std::string secondControlFieldPrefix;
        std::string secondControlFieldKey;
        std::tie(secondControlFieldPrefix, secondControlFieldKey) =
            extractControlPrefixAndKey(nextElem.fieldNameStringData());

        if (firstOrdering != secondOrdering) {
            // The compound index has a mixed ascending and descending key pattern. Do not convert
            // the index spec.
            return {};
        }

        if (firstControlFieldPrefix == timeseries::kControlMaxFieldNamePrefix &&
            secondControlFieldPrefix == timeseries::kControlMinFieldNamePrefix &&
            firstControlFieldKey == secondControlFieldKey && firstOrdering >= 0) {
            // Ascending index.
            builder.appendAs(nextElem, firstControlFieldKey);
            continue;
        } else if (firstControlFieldPrefix == timeseries::kControlMinFieldNamePrefix &&
                   secondControlFieldPrefix == timeseries::kControlMaxFieldNamePrefix &&
                   firstControlFieldKey == secondControlFieldKey && firstOrdering < 0) {
            // Descending index.
            builder.appendAs(nextElem, firstControlFieldKey);
            continue;
        } else {
            // This measurement index spec on the underlying buckets collection is not valid for
            // time-series as the compound index has the wrong ordering. We will not convert the
            // index spec.
            return {};
        }
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

}  // namespace timeseries
}  // namespace mongo
