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

#include "mongo/db/index/index_descriptor.h"
#include "mongo/db/index_names.h"
#include "mongo/db/pipeline/document_source_internal_unpack_bucket.h"
#include "mongo/db/query/collation/collator_factory_interface.h"
#include "mongo/db/query/collation/collator_interface.h"
#include "mongo/db/storage/storage_parameters_gen.h"
#include "mongo/db/timeseries/timeseries_constants.h"
#include "mongo/db/timeseries/timeseries_index_schema_conversion_functions.h"
#include "mongo/logv2/log.h"
#include "mongo/logv2/redaction.h"

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

CreateIndexesCommand makeTimeseriesCreateIndexesCommand(OperationContext* opCtx,
                                                        const CreateIndexesCommand& origCmd,
                                                        const TimeseriesOptions& options) {
    const auto& origNs = origCmd.getNamespace();
    const auto& origIndexes = origCmd.getIndexes();

    std::vector<mongo::BSONObj> indexes;
    for (const auto& origIndex : origIndexes) {
        BSONObjBuilder builder;
        BSONObj keyField;
        BSONObj originalKeyField;
        bool isTTLIndex = false;
        bool hasPartialFilterOnMetaField = false;
        bool includeOriginalSpec = false;

        for (const auto& elem : origIndex) {
            if (elem.fieldNameStringData() == IndexDescriptor::kPartialFilterExprFieldName) {
                if (feature_flags::gTimeseriesMetricIndexes.isEnabled(
                        serverGlobalParams.featureCompatibility)) {
                    includeOriginalSpec = true;
                } else {
                    uasserted(ErrorCodes::InvalidOptions,
                              "Partial indexes are not supported on time-series collections");
                }

                uassert(ErrorCodes::CannotCreateIndex,
                        "Partial indexes on time-series collections require FCV 5.3",
                        feature_flags::gTimeseriesMetricIndexes.isEnabled(
                            serverGlobalParams.featureCompatibility));
                BSONObj pred = elem.Obj();

                // If the createIndexes command specifies a collation for this index, then that
                // collation affects how we should interpret expressions in the partial filter
                // ($gt, $lt, etc).
                if (auto collatorSpec = origIndex[NewIndexSpec::kCollationFieldName]) {
                    uasserted(
                        ErrorCodes::IndexOptionsConflict,
                        std::string{"On a time-series collection, partialFilterExpression and "} +
                            NewIndexSpec::kCollationFieldName + " arguments are incompatible"_sd);
                }
                // Since no collation was specified in the command, we know the index collation will
                // match the collection's collation.
                auto collationMatchesDefault = ExpressionContext::CollationMatchesDefault::kYes;

                // Even though the index collation will match the collection's collation, we don't
                // know whether or not that collation is simple. However, I think we can correctly
                // rewrite the filter expression without knowing this... Looking up the correct
                // value would require handling mongos and mongod separately.
                std::unique_ptr<CollatorInterface> collator{nullptr};

                auto expCtx = make_intrusive<ExpressionContext>(opCtx, std::move(collator), origNs);
                expCtx->collationMatchesDefault = collationMatchesDefault;

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

                auto [hasMetricPred, bucketPred] =
                    BucketSpec::pushdownPredicate(expCtx,
                                                  options,
                                                  pred,
                                                  haveComputedMetaField,
                                                  includeMetaField,
                                                  assumeNoMixedSchemaData,
                                                  BucketSpec::IneligiblePredicatePolicy::kError);

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
                         keyElem.fieldNameStringData().startsWith(*metaField + "."))) {
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
                        str::stream() << bucketsIndexSpecWithStatus.getStatus().toString()
                                      << " Command request: " << redact(origCmd.toBSON({})),
                        bucketsIndexSpecWithStatus.isOK());

                if (timeseries::shouldIncludeOriginalSpec(
                        options,
                        BSON(NewIndexSpec::kKeyFieldName
                             << bucketsIndexSpecWithStatus.getValue()))) {
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
                    "TTL indexes are not supported on time-series collections",
                    feature_flags::gTimeseriesScalabilityImprovements.isEnabled(
                        serverGlobalParams.featureCompatibility));
            uassert(ErrorCodes::InvalidOptions,
                    "TTL indexes on time-series collections require a partialFilterExpression on "
                    "the metaField",
                    hasPartialFilterOnMetaField);
            keyField = convertToTTLTimeField(originalKeyField, options.getTimeField());
        }
        builder.append(NewIndexSpec::kKeyFieldName, std::move(keyField));

        if (feature_flags::gTimeseriesMetricIndexes.isEnabled(
                serverGlobalParams.featureCompatibility) &&
            includeOriginalSpec) {
            // Store the original user index definition on the transformed index definition for the
            // time-series buckets collection.
            builder.appendObject(IndexDescriptor::kOriginalSpecFieldName, origIndex.objdata());
        }

        indexes.push_back(builder.obj());
    }

    auto ns = makeTimeseriesBucketsNamespace(origNs);
    auto cmd = CreateIndexesCommand(ns, std::move(indexes));
    cmd.setV(origCmd.getV());
    cmd.setIgnoreUnknownIndexOptions(origCmd.getIgnoreUnknownIndexOptions());
    cmd.setCommitQuorum(origCmd.getCommitQuorum());

    return cmd;
}

DropIndexes makeTimeseriesDropIndexesCommand(OperationContext* opCtx,
                                             const DropIndexes& origCmd,
                                             const TimeseriesOptions& options) {
    const auto& origNs = origCmd.getNamespace();
    auto ns = makeTimeseriesBucketsNamespace(origNs);

    const auto& origIndex = origCmd.getIndex();
    if (auto keyPtr = stdx::get_if<BSONObj>(&origIndex)) {
        auto bucketsIndexSpecWithStatus =
            timeseries::createBucketsIndexSpecFromTimeseriesIndexSpec(options, *keyPtr);

        uassert(ErrorCodes::IndexNotFound,
                str::stream() << bucketsIndexSpecWithStatus.getStatus().toString()
                              << " Command request: " << redact(origCmd.toBSON({})),
                bucketsIndexSpecWithStatus.isOK());

        DropIndexes dropIndexCmd(ns);
        dropIndexCmd.setDropIndexesRequest({std::move(bucketsIndexSpecWithStatus.getValue())});
        return dropIndexCmd;
    }

    DropIndexes dropIndexCmd(ns);
    dropIndexCmd.setDropIndexesRequest(origIndex);
    return dropIndexCmd;
}

}  // namespace mongo::timeseries
