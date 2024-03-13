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


#include "mongo/db/timeseries/timeseries_index_schema_conversion_functions.h"

#include <algorithm>
#include <boost/container/small_vector.hpp>
#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional.hpp>
#include <cstddef>
#include <memory>
#include <string>
#include <tuple>
#include <utility>

#include <boost/optional/optional.hpp>

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/db/catalog/index_catalog_entry.h"
#include "mongo/db/feature_flag.h"
#include "mongo/db/index/index_descriptor.h"
#include "mongo/db/index_names.h"
#include "mongo/db/matcher/expression_algo.h"
#include "mongo/db/matcher/expression_parser.h"
#include "mongo/db/matcher/extensions_callback_noop.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/server_options.h"
#include "mongo/db/storage/storage_parameters_gen.h"
#include "mongo/db/timeseries/timeseries_constants.h"
#include "mongo/db/timeseries/timeseries_gen.h"
#include "mongo/logv2/redaction.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/intrusive_counter.h"
#include "mongo/util/str.h"
#include "mongo/util/string_map.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kStorage


namespace mongo::timeseries {

namespace {

bool isIndexOnControl(StringData field) {
    return field.startsWith(timeseries::kControlMinFieldNamePrefix) ||
        field.startsWith(timeseries::kControlMaxFieldNamePrefix);
}

/**
 * Takes the index specification field name, such as 'control.max.x.y', or 'control.min.z' and
 * returns a pair of the prefix ('control.min.' or 'control.max.') and key ('x.y' or 'z').
 */
std::pair<std::string, std::string> extractControlPrefixAndKey(StringData field) {
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

/**
 * Converts an event-level index spec to a bucket-level index spec.
 *
 * If the input is not a valid index spec, this function must either:
 *  - return an error Status
 *  - return an invalid index spec
 */
StatusWith<BSONObj> createBucketsSpecFromTimeseriesSpec(const TimeseriesOptions& timeseriesOptions,
                                                        const BSONObj& timeseriesIndexSpecBSON,
                                                        bool isShardKeySpec) {
    if (timeseriesIndexSpecBSON.isEmpty()) {
        return {ErrorCodes::BadValue, "Empty object is not a valid index spec"_sd};
    }
    if (timeseriesIndexSpecBSON.firstElement().fieldNameStringData() == "$hint"_sd ||
        timeseriesIndexSpecBSON.firstElement().fieldNameStringData() == "$natural"_sd) {
        return {
            ErrorCodes::BadValue,
            str::stream() << "Invalid index spec (perhaps it's a valid hint, that was incorrectly "
                          << "passed to createBucketsSpecFromTimeseriesSpec): "
                          << timeseriesIndexSpecBSON};
    }

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
                            << "Invalid " << (isShardKeySpec ? "shard key" : "index spec")
                            << " for time-series collection: " << redact(timeseriesIndexSpecBSON)
                            << ". " << (isShardKeySpec ? "Shard keys" : "Indexes")
                            << " on the time field must be ascending or descending "
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

        // 2dsphere indexes on measurements are allowed, but need to be re-written to
        // point to the data field and use the special 2dsphere_bucket index type.
        if (elem.valueStringData() == IndexNames::GEO_2DSPHERE) {
            builder.append(str::stream() << timeseries::kBucketDataFieldName << "."
                                         << elem.fieldNameStringData(),
                           IndexNames::GEO_2DSPHERE_BUCKET);
            continue;
        }

        // No other special index types are allowed on timeseries measurements.
        if (!elem.isNumber()) {
            return {
                ErrorCodes::BadValue,
                str::stream() << "Invalid index spec for time-series collection: "
                              << redact(timeseriesIndexSpecBSON)
                              << ". Indexes on measurement fields must be ascending or descending "
                                 "(numbers only), or '2dsphere': "
                              << elem};
        }

        if (elem.number() >= 0) {
            // For ascending key patterns, the { control.min.elem: 1, control.max.elem: 1 }
            // compound index is created.
            builder.appendAs(
                elem, str::stream() << timeseries::kControlMinFieldNamePrefix << elem.fieldName());
            builder.appendAs(
                elem, str::stream() << timeseries::kControlMaxFieldNamePrefix << elem.fieldName());
        } else if (elem.number() < 0) {
            // For descending key patterns, the { control.max.elem: -1, control.min.elem: -1 }
            // compound index is created.
            builder.appendAs(
                elem, str::stream() << timeseries::kControlMaxFieldNamePrefix << elem.fieldName());
            builder.appendAs(
                elem, str::stream() << timeseries::kControlMinFieldNamePrefix << elem.fieldName());
        }
    }

    return builder.obj();
}

/**
 * Maps the buckets collection index spec 'bucketsIndexSpecBSON' to the index schema of the
 * time-series collection using the information provided in 'timeseriesOptions'.
 *
 * If 'bucketsIndexSpecBSON' does not match a valid time-series index format, then boost::none is
 * returned.
 *
 * Conversion Example:
 * On a time-series collection with 'tm' time field and 'mm' metadata field,
 * we may see a compound index on the underlying bucket collection mapped from:
 * {
 *     'meta.tag1': 1,
 *     'control.min.tm': 1,
 *     'control.max.tm': 1
 * }
 * to an index on the time-series collection:
 * {
 *     'mm.tag1': 1,
 *     'tm': 1
 * }
 */
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

        if (elem.fieldNameStringData().startsWith(timeseries::kBucketDataFieldName + ".") &&
            elem.valueStringData() == IndexNames::GEO_2DSPHERE_BUCKET) {
            builder.append(
                elem.fieldNameStringData().substr(timeseries::kBucketDataFieldName.size() + 1),
                IndexNames::GEO_2DSPHERE);
            continue;
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

        if (firstControlFieldPrefix == timeseries::kControlMinFieldNamePrefix &&
            secondControlFieldPrefix == timeseries::kControlMaxFieldNamePrefix &&
            firstControlFieldKey == secondControlFieldKey && firstOrdering >= 0) {
            // Ascending index.
            builder.appendAs(nextElem, firstControlFieldKey);
            continue;
        } else if (firstControlFieldPrefix == timeseries::kControlMaxFieldNamePrefix &&
                   secondControlFieldPrefix == timeseries::kControlMinFieldNamePrefix &&
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

/**
 * Maps a buckets shard key spec to a spec for the index backing the shard key on the buckets
 * collection using the information provided in 'timeseriesOptions'. The shard key on the buckets
 * collection should already be rewritten to use bucket collection field names and should be valid.
 *
 * If 'bucketShardKeySpecBSON' does not match a valid time-series shard key format, then boost::none
 * is returned.
 *
 * Conversion Example:
 * On a time-series collection with 'tm' time field and 'mm' metadata field,
 * we may see a compound shard key on the underlying bucket collection mapped from:
 * {
 *     'meta.tag1': 1,
 *     'control.min.tm': 1,
 * }
 * to an index on the buckets collection:
 * {
 *     'meta.tag1': 1,
 *     'control.min.tm': 1,
 *     'control.max.tm': 1
 * }
 */
boost::optional<BSONObj> createBucketsIndexSpecFromBucketsShardKeySpec(
    const TimeseriesOptions& timeseriesOptions, const BSONObj& bucketShardKeySpecBSON) {

    // The shard key has already been validated. Therefore, the index backing the shard key must be
    // valid. If not, this means  we have passed shard key validation, but have created an
    // invalid index backing the shard key. We will return boost::none if the index is invalid and
    // the caller of the function will verify an index was returned.
    if (bucketShardKeySpecBSON.isEmpty()) {
        return {};
    }

    if (bucketShardKeySpecBSON.firstElement().fieldNameStringData() == "$hint"_sd ||
        bucketShardKeySpecBSON.firstElement().fieldNameStringData() == "$natural"_sd) {
        return {};
    }

    auto timeField = timeseriesOptions.getTimeField();
    auto metaField = timeseriesOptions.getMetaField();
    const std::string controlMinTimeField = str::stream()
        << timeseries::kControlMinFieldNamePrefix << timeField;

    BSONObjBuilder builder;
    for (const auto& elem : bucketShardKeySpecBSON) {
        // Unsharded collections can appear with the key '{_id:1}'. We will return the key as is in
        // that case.
        if (elem.fieldNameStringData() == timeseries::kBucketIdFieldName) {
            if (!elem.isNumber() || elem.number() != 1 || bucketShardKeySpecBSON.nFields() != 1) {
                return {};
            }
            return builder.append(elem).obj();
        }

        // We expect the index backing the shard key to already be "buckets-encoded". This means
        // that indexSpecBSON should be "{control.min.time:1}".
        if (elem.fieldNameStringData() == controlMinTimeField) {
            if (!elem.isNumber() || elem.number() < 0) {
                // Shard keys on the time field must be ascending.
                return {};
            }

            // Append the 'control.min.time' element and add the 'control.max.time' element.
            builder.append(elem);
            builder.appendAs(elem,
                             str::stream() << timeseries::kControlMaxFieldNamePrefix << timeField);
            continue;
        }

        // If the index is on the meta field, the index should already be buckets-encoded and thus
        // be rewritten as "meta".
        if (metaField) {
            if (elem.fieldNameStringData() == timeseries::kBucketMetaFieldName) {
                builder.append(elem);
                continue;
            }

            // Time-series indexes on sub-documents of the 'metaField' are allowed.
            if (elem.fieldNameStringData().startsWith(timeseries::kBucketMetaFieldName + ".")) {
                builder.append(elem);
                continue;
            }
        }

        // Shard keys are only allowed on the meta field or time field.
        return {};
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

boost::optional<BSONObj> createBucketsShardKeyIndexFromBucketsShardKeySpec(
    const TimeseriesOptions& timeseriesOptions, const BSONObj& timeseriesShardKeySpecBSON) {
    return createBucketsIndexSpecFromBucketsShardKeySpec(timeseriesOptions,
                                                         timeseriesShardKeySpecBSON);
}

boost::optional<BSONObj> createTimeseriesIndexFromBucketsIndexSpec(
    const TimeseriesOptions& timeseriesOptions, const BSONObj& bucketsIndexSpecBSON) {
    return createTimeseriesIndexSpecFromBucketsIndexSpec(timeseriesOptions, bucketsIndexSpecBSON);
}

boost::optional<BSONObj> createTimeseriesIndexFromBucketsIndex(
    const TimeseriesOptions& timeseriesOptions, const BSONObj& bucketsIndex) {

    if (bucketsIndex.hasField(kOriginalSpecFieldName)) {
        // This buckets index has the original user index definition available, return it if the
        // time-series metric indexes feature flag is enabled. If the feature flag isn't enabled,
        // the reverse mapping mechanism will be used. This is necessary to skip returning any
        // incompatible indexes created when the feature flag was enabled.
        BSONObj intermediateObj = bucketsIndex.getObjectField(kOriginalSpecFieldName);
        return intermediateObj.addField(bucketsIndex[IndexDescriptor::k2dsphereVersionFieldName]);
    }
    if (bucketsIndex.hasField(kKeyFieldName)) {
        auto timeseriesKeyValue = createTimeseriesIndexSpecFromBucketsIndexSpec(
            timeseriesOptions, bucketsIndex.getField(kKeyFieldName).Obj());
        if (timeseriesKeyValue) {
            // This creates a BSONObj copy with the kOriginalSpecFieldName field removed, if it
            // exists, and modifies the kKeyFieldName field to timeseriesKeyValue.
            BSONObj intermediateObj =
                bucketsIndex.removeFields(StringDataSet{kOriginalSpecFieldName});
            return intermediateObj.addFields(BSON(kKeyFieldName << timeseriesKeyValue.value()),
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

bool shouldIncludeOriginalSpec(const TimeseriesOptions& timeseriesOptions,
                               const BSONObj& bucketsIndex) {
    if (!bucketsIndex.hasField(kKeyFieldName)) {
        return false;
    }

    auto timeField = timeseriesOptions.getTimeField();
    auto metaField = timeseriesOptions.getMetaField();

    const std::string controlMinTimeField = str::stream()
        << timeseries::kControlMinFieldNamePrefix << timeField;
    const std::string controlMaxTimeField = str::stream()
        << timeseries::kControlMaxFieldNamePrefix << timeField;

    const auto& bucketsIndexSpecBSON = bucketsIndex.getField(kKeyFieldName).Obj();
    for (const auto& elem : bucketsIndexSpecBSON) {
        // 'control.min.time' and 'control.max.time' are both ok.
        if (elem.fieldNameStringData() == controlMinTimeField) {
            continue;
        } else if (elem.fieldNameStringData() == controlMaxTimeField) {
            continue;
        }

        // Metadata, and subfields of metadata, are both ok.
        if (metaField) {
            if (elem.fieldNameStringData() == timeseries::kBucketMetaFieldName) {
                continue;
            }

            if (elem.fieldNameStringData().startsWith(timeseries::kBucketMetaFieldName + ".")) {
                continue;
            }
        }

        // Found a non-time, non-metadata field, which means a 5.0 server would not understand this
        // index. That means it's fine to also include 'originalSpec', because this index would have
        // to be dropped before downgrading to 5.0 anyway.
        return true;
    }

    // All fields are either time or metadata. That means a 5.0 server will understand this index.
    // Since 5.0 does not know about the 'originalSpec' field, don't include it: we don't want to
    // complicate downgrade for this index.
    return false;
}

bool doesBucketsIndexIncludeMeasurement(OperationContext* opCtx,
                                        const NamespaceString& bucketNs,
                                        const TimeseriesOptions& timeseriesOptions,
                                        const BSONObj& bucketsIndex) {
    tassert(5916306,
            str::stream() << "Index spec has no 'key': " << bucketsIndex.toString(),
            bucketsIndex.hasField(kKeyFieldName));

    auto timeField = timeseriesOptions.getTimeField();
    auto metaField = timeseriesOptions.getMetaField();

    const std::string controlMinTimeField = str::stream()
        << timeseries::kControlMinFieldNamePrefix << timeField;
    const std::string controlMaxTimeField = str::stream()
        << timeseries::kControlMaxFieldNamePrefix << timeField;
    static const std::string idField = "_id";

    auto isMeasurementField = [&](StringData name) -> bool {
        if (name == controlMinTimeField || name == controlMaxTimeField) {
            return false;
        }

        if (metaField) {
            if (name == timeseries::kBucketMetaFieldName ||
                name.startsWith(timeseries::kBucketMetaFieldName + ".")) {
                return false;
            }
        }

        return true;
    };

    // Check index key.
    const BSONObj keyObj = bucketsIndex.getField(kKeyFieldName).Obj();
    for (const auto& elem : keyObj) {
        if (isMeasurementField(elem.fieldNameStringData()))
            return true;
    }

    // Check partial filter expression.
    if (auto filterElem = bucketsIndex[kPartialFilterExpressionFieldName]) {
        tassert(5916302,
                str::stream() << "Partial filter expression is not an object: " << filterElem,
                filterElem.type() == BSONType::Object);

        auto expCtx = make_intrusive<ExpressionContext>(opCtx, nullptr /* collator */, bucketNs);

        MatchExpressionParser::AllowedFeatureSet allowedFeatures =
            MatchExpressionParser::kDefaultSpecialFeatures;

        // TODO SERVER-53380 convert to tassertStatusOK.
        auto statusWithFilter = MatchExpressionParser::parse(
            filterElem.Obj(), expCtx, ExtensionsCallbackNoop{}, allowedFeatures);
        tassert(5916303,
                str::stream() << "Partial filter expression failed to parse: "
                              << statusWithFilter.getStatus(),
                statusWithFilter.isOK());
        auto filter = std::move(statusWithFilter.getValue());

        if (!expression::isOnlyDependentOnConst(*filter,
                                                {std::string{timeseries::kBucketMetaFieldName},
                                                 controlMinTimeField,
                                                 controlMaxTimeField,
                                                 idField})) {
            // Partial filter expression depends on a non-time, non-metadata field.
            return true;
        }
    }

    return false;
}

bool isHintIndexKey(const BSONObj& obj) {
    if (obj.isEmpty())
        return false;
    StringData fieldName = obj.firstElement().fieldNameStringData();
    if (fieldName == "$hint"_sd)
        return false;
    if (fieldName == "$natural"_sd)
        return false;

    return true;
}

boost::optional<BSONObj> getIndexSupportingReopeningQuery(OperationContext* opCtx,
                                                          const IndexCatalog* indexCatalog,
                                                          const TimeseriesOptions& tsOptions) {
    const std::string controlTimeField =
        timeseries::kControlMinFieldNamePrefix.toString() + tsOptions.getTimeField();

    // Populate a vector of index key fields which we check against existing indexes.
    boost::container::small_vector<std::string, 2> expectedPrefix;
    if (tsOptions.getMetaField().has_value()) {
        expectedPrefix.push_back(kBucketMetaFieldName.toString());
    }
    expectedPrefix.push_back(controlTimeField);

    auto indexIt = indexCatalog->getIndexIterator(opCtx, IndexCatalog::InclusionPolicy::kReady);
    while (indexIt->more()) {
        auto indexEntry = indexIt->next();
        auto indexDesc = indexEntry->descriptor();

        // We cannot use a partial index when querying buckets to reopen.
        if (indexDesc->isPartial()) {
            continue;
        }

        auto indexKey = indexDesc->keyPattern();
        size_t index = 0;
        for (auto& elem : indexKey) {
            // The index must include the meta and time field (in that order), but may have
            // additional fields included.
            //
            // In cases where there collections do not have a meta field specified, an index on time
            // suffices.
            if (elem.fieldName() != expectedPrefix.at(index)) {
                break;
            }
            index++;
            if (index == expectedPrefix.size()) {
                return BSON("$hint" << indexDesc->indexName());
            }
        }
    }

    return boost::none;
}

}  // namespace mongo::timeseries
