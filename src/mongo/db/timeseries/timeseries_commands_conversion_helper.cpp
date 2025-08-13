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


#include "mongo/db/timeseries/timeseries_commands_conversion_helper.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/index_builds/commit_quorum_options.h"
#include "mongo/db/index_names.h"
#include "mongo/db/local_catalog/index_descriptor.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/expression_context_builder.h"
#include "mongo/db/query/collation/collator_interface.h"
#include "mongo/db/query/timeseries/bucket_spec.h"
#include "mongo/db/timeseries/timeseries_constants.h"
#include "mongo/db/timeseries/timeseries_index_schema_conversion_functions.h"
#include "mongo/logv2/redaction.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/intrusive_counter.h"
#include "mongo/util/str.h"

#include <memory>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kStorage


namespace mongo::timeseries {

namespace {
NamespaceString makeTimeseriesBucketsNamespace(const NamespaceString& nss) {
    return nss.isTimeseriesBucketsCollection() ? nss : nss.makeTimeseriesBucketsNamespace();
}

/**
 * Converts the key field on time to 'control.min.$timeField' field. Depends on error checking from
 * 'createBucketsSpecFromTimeseriesSpec()' which should be called before this function.
 */
BSONObj convertToTTLTimeField(const BSONObj& origKeyField, StringData timeField) {
    BSONObjBuilder keyBuilder;
    uassert(ErrorCodes::CannotCreateIndex,
            str::stream() << "TTL indexes are single-field indexes, compound indexes do "
                             "not support TTL. Index spec: "
                          << origKeyField,
            origKeyField.nFields() == 1);

    const auto& firstElem = origKeyField.firstElement();
    uassert(ErrorCodes::InvalidOptions,
            "TTL indexes on non-time fields are not supported on time-series collections",
            firstElem.fieldName() == timeField);

    keyBuilder.appendAs(firstElem,
                        str::stream() << timeseries::kControlMinFieldNamePrefix << timeField);
    return keyBuilder.obj();
}
}  // namespace


BSONObj makeTimeseriesCommand(const BSONObj& origCmd,
                              const NamespaceString& ns,
                              const StringData nsFieldName,
                              boost::optional<StringData> appendTimeSeriesFlag) {
    // Translate time-series collection view namespace to bucket namespace.
    const auto bucketNs = ns.makeTimeseriesBucketsNamespace();
    BSONObjBuilder builder;
    for (const auto& entry : origCmd) {
        if (entry.fieldNameStringData() == nsFieldName) {
            builder.append(nsFieldName, bucketNs.coll());
        } else {
            builder.append(entry);
        }
    }

    if (appendTimeSeriesFlag) {
        builder.append(*appendTimeSeriesFlag, true);
    }
    return builder.obj();
}

mongo::BSONObj translateIndexSpecFromLogicalToBuckets(OperationContext* opCtx,
                                                      const NamespaceString& origNs,
                                                      const BSONObj& origIndex,
                                                      const TimeseriesOptions& options) {
    BSONObjBuilder builder;
    BSONObj keyField;
    BSONObj originalKeyField;
    bool isTTLIndex = false;
    bool hasPartialFilterOnMetaField = false;
    bool includeOriginalSpec = false;

    for (const auto& elem : origIndex) {
        if (elem.fieldNameStringData() == IndexDescriptor::kPartialFilterExprFieldName) {
            includeOriginalSpec = true;

            BSONObj pred = elem.Obj();

            // If the createIndexes command specifies a collation for this index, then that
            // collation affects how we should interpret expressions in the partial filter
            // ($gt, $lt, etc).
            if (auto collatorSpec = origIndex[NewIndexSpec::kCollationFieldName]) {
                uasserted(ErrorCodes::IndexOptionsConflict,
                          std::string{"On a time-series collection, partialFilterExpression and "} +
                              NewIndexSpec::kCollationFieldName + " arguments are incompatible"_sd);
            }
            // Since no collation was specified in the command, we know the index collation will
            // match the collection's collation.
            auto collationMatchesDefault = ExpressionContextCollationMatchesDefault::kYes;

            // Even though the index collation will match the collection's collation, we don't
            // know whether or not that collation is simple. However, I think we can correctly
            // rewrite the filter expression without knowing this... Looking up the correct
            // value would require handling mongos and mongod separately.
            auto expCtx = ExpressionContextBuilder{}
                              .opCtx(opCtx)
                              .ns(origNs)
                              .collationMatchesDefault(collationMatchesDefault)
                              .build();
            // We can't know if there won't be extended range values in the collection, so
            // assume there will be.
            expCtx->setRequiresTimeseriesExtendedRangeSupport(true);

            // partialFilterExpression is evaluated against a collection, so there are no
            // computed fields.
            bool haveComputedMetaField = false;

            // partialFilterExpression is evaluated against a collection, so there are no
            // exclusions
            bool includeMetaField = options.getMetaField().has_value();

            // As part of building the index, we verify that the collection does not contain
            // any mixed-schema buckets. So by the time the index is visible to the query
            // planner, this will be true.
            bool assumeNoMixedSchemaData = true;

            // Fixed buckets is dependent on the time-series collection options not changing,
            // this can change throughout the lifetime of the index.
            bool fixedBuckets = false;

            auto [hasMetricPred, bucketPred] =
                BucketSpec::pushdownPredicate(expCtx,
                                              options,
                                              pred,
                                              haveComputedMetaField,
                                              includeMetaField,
                                              assumeNoMixedSchemaData,
                                              BucketSpec::IneligiblePredicatePolicy::kError,
                                              fixedBuckets);

            hasPartialFilterOnMetaField = !hasMetricPred;

            builder.append(IndexDescriptor::kPartialFilterExprFieldName, bucketPred);
            continue;
        }

        if (elem.fieldNameStringData() == IndexDescriptor::kSparseFieldName) {
            // Sparse indexes are only allowed on the time and meta fields.
            auto timeField = options.getTimeField();
            auto metaField = options.getMetaField();

            BSONObj keyPattern = origIndex.getField(NewIndexSpec::kKeyFieldName).Obj();
            for (const auto& keyElem : keyPattern) {
                if (keyElem.fieldNameStringData() == timeField) {
                    continue;
                }

                if (metaField &&
                    (keyElem.fieldNameStringData() == *metaField ||
                     keyElem.fieldNameStringData().starts_with(*metaField + "."))) {
                    continue;
                }

                uasserted(ErrorCodes::InvalidOptions,
                          "Sparse indexes are not supported on time-series measurements");
            }
        }

        if (elem.fieldNameStringData() == IndexDescriptor::kExpireAfterSecondsFieldName) {
            isTTLIndex = true;
            builder.append(elem);
            continue;
        }

        if (elem.fieldNameStringData() == IndexDescriptor::kUniqueFieldName) {
            uassert(ErrorCodes::InvalidOptions,
                    "Unique indexes are not supported on time-series collections",
                    !elem.trueValue());
        }

        if (elem.fieldNameStringData() == NewIndexSpec::kKeyFieldName) {
            originalKeyField = elem.Obj();

            auto pluginName = IndexNames::findPluginName(originalKeyField);
            uassert(ErrorCodes::InvalidOptions,
                    "Text indexes are not supported on time-series collections",
                    pluginName != IndexNames::TEXT);

            auto bucketsIndexSpecWithStatus =
                timeseries::createBucketsIndexSpecFromTimeseriesIndexSpec(options,
                                                                          originalKeyField);
            uassert(ErrorCodes::CannotCreateIndex,
                    bucketsIndexSpecWithStatus.getStatus().toString(),
                    bucketsIndexSpecWithStatus.isOK());

            if (timeseries::shouldIncludeOriginalSpec(
                    options,
                    BSON(NewIndexSpec::kKeyFieldName << bucketsIndexSpecWithStatus.getValue()))) {
                includeOriginalSpec = true;
            }
            keyField = std::move(bucketsIndexSpecWithStatus.getValue());
            continue;
        }

        // Any index option that's not explicitly banned, and not handled specially, we pass
        // through unchanged.
        builder.append(elem);
    }

    if (isTTLIndex) {
        uassert(ErrorCodes::InvalidOptions,
                "TTL indexes on time-series collections require a partialFilterExpression on "
                "the metaField",
                hasPartialFilterOnMetaField);
        keyField = convertToTTLTimeField(originalKeyField, options.getTimeField());
    }
    builder.append(NewIndexSpec::kKeyFieldName, std::move(keyField));

    if (includeOriginalSpec) {
        // Store the original user index definition on the transformed index definition for the
        // time-series buckets collection.
        builder.appendObject(IndexDescriptor::kOriginalSpecFieldName, origIndex.objdata());
    }

    return builder.obj();
}

}  // namespace mongo::timeseries
