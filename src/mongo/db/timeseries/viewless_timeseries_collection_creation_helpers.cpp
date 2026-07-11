// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/timeseries/viewless_timeseries_collection_creation_helpers.h"

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/index_builds/index_builds_coordinator.h"
#include "mongo/db/timeseries/timeseries_constants.h"
#include "mongo/db/timeseries/timeseries_index_schema_conversion_functions.h"

#include <string_view>

namespace mongo::timeseries {

BSONObj generateTimeseriesValidator(int bucketVersion, std::string_view timeField) {
    if (bucketVersion != timeseries::kTimeseriesControlCompressedSortedVersion &&
        bucketVersion != timeseries::kTimeseriesControlUncompressedVersion &&
        bucketVersion != timeseries::kTimeseriesControlCompressedUnsortedVersion) {
        MONGO_UNREACHABLE_TASSERT(10083502);
    }
    // '$jsonSchema' : {
    //     bsonType: 'object',
    //     required: ['_id', 'control', 'data'],
    //     properties: {
    //         _id: {bsonType: 'objectId'},
    //         control: {
    //             bsonType: 'object',
    //             required: ['version', 'min', 'max'],
    //             properties: {
    //                 version: {bsonType: 'number'},
    //                 min: {
    //                     bsonType: 'object',
    //                     required: ['%s'],
    //                     properties: {'%s': {bsonType: 'date'}}
    //                 },
    //                 max: {
    //                     bsonType: 'object',
    //                     required: ['%s'],
    //                     properties: {'%s': {bsonType: 'date'}}
    //                 },
    //                 closed: {bsonType: 'bool'},
    //                 count: {bsonType: 'number', minimum: 1} // only if bucketVersion ==
    //                 // timeseries::kTimeseriesControlCompressedSortedVersion or
    //                 // timeseries::kTimeseriesControlCompressedUnsortedVersion
    //             },
    //             additionalProperties: false // only if bucketVersion ==
    //             // timeseries::kTimeseriesControlCompressedSortedVersion or
    //             // timeseries::kTimeseriesControlCompressedUnsortedVersion
    //         },
    //         data: {bsonType: 'object'},
    //         meta: {}
    //     },
    //     additionalProperties: false
    //   }
    BSONObjBuilder validator;
    BSONObjBuilder schema(validator.subobjStart("$jsonSchema"));
    schema.append("bsonType", "object");
    schema.append("required",
                  BSON_ARRAY("_id" << "control"
                                   << "data"));
    {
        BSONObjBuilder properties(schema.subobjStart("properties"));
        {
            BSONObjBuilder _id(properties.subobjStart("_id"));
            _id.append("bsonType", "objectId");
            _id.done();
        }
        {
            BSONObjBuilder control(properties.subobjStart("control"));
            control.append("bsonType", "object");
            control.append("required",
                           BSON_ARRAY("version" << "min"
                                                << "max"));
            {
                BSONObjBuilder innerProperties(control.subobjStart("properties"));
                {
                    BSONObjBuilder version(innerProperties.subobjStart("version"));
                    version.append("bsonType", "number");
                    version.done();
                }
                {
                    BSONObjBuilder min(innerProperties.subobjStart("min"));
                    min.append("bsonType", "object");
                    min.append("required", BSON_ARRAY(timeField));
                    BSONObjBuilder minProperties(min.subobjStart("properties"));
                    BSONObjBuilder timeFieldObj(minProperties.subobjStart(timeField));
                    timeFieldObj.append("bsonType", "date");
                    timeFieldObj.done();
                    minProperties.done();
                    min.done();
                }

                {
                    BSONObjBuilder max(innerProperties.subobjStart("max"));
                    max.append("bsonType", "object");
                    max.append("required", BSON_ARRAY(timeField));
                    BSONObjBuilder maxProperties(max.subobjStart("properties"));
                    BSONObjBuilder timeFieldObj(maxProperties.subobjStart(timeField));
                    timeFieldObj.append("bsonType", "date");
                    timeFieldObj.done();
                    maxProperties.done();
                    max.done();
                }
                {
                    BSONObjBuilder closed(innerProperties.subobjStart("closed"));
                    closed.append("bsonType", "bool");
                    closed.done();
                }
                if (bucketVersion == timeseries::kTimeseriesControlCompressedSortedVersion ||
                    bucketVersion == timeseries::kTimeseriesControlCompressedUnsortedVersion) {
                    BSONObjBuilder count(innerProperties.subobjStart("count"));
                    count.append("bsonType", "number");
                    count.append("minimum", 1);
                    count.done();
                }
                innerProperties.done();
            }
            if (bucketVersion == timeseries::kTimeseriesControlCompressedSortedVersion ||
                bucketVersion == timeseries::kTimeseriesControlCompressedUnsortedVersion) {
                control.append("additionalProperties", false);
            }
            control.done();
        }
        {
            BSONObjBuilder data(properties.subobjStart("data"));
            data.append("bsonType", "object");
            data.done();
        }
        properties.append("meta", BSONObj{});
        properties.done();
    }
    schema.append("additionalProperties", false);
    schema.done();
    return validator.obj();
}

void validateTimeseriesValidator(const BSONObj& validator, std::string_view timeField) {
    for (auto bucketVersion = timeseries::kTimeseriesControlLatestVersion;
         bucketVersion > timeseries::kTimeseriesControlMinVersion;
         --bucketVersion) {
        auto autoGeneratedValidator = generateTimeseriesValidator(bucketVersion, timeField);
        if (autoGeneratedValidator.woCompare(validator) == 0) {
            // the given validator is equal to the auto generate one
            return;
        }
    }
    uasserted(ErrorCodes::InvalidOptions,
              fmt::format("The provided validator does not match any validator schema expected "
                          "for a time-series collection with time field '{}'.",
                          timeField));
}

Status createDefaultTimeseriesIndex(OperationContext* opCtx,
                                    CollectionWriter& collection,
                                    BSONObj collator) {
    auto tsOptions = collection->getCollectionOptions().timeseries;
    if (!tsOptions->getMetaField()) {
        return Status::OK();
    }

    StatusWith<BSONObj> swBucketsSpec = timeseries::createBucketsIndexSpecFromTimeseriesIndexSpec(
        *tsOptions, BSON(*tsOptions->getMetaField() << 1 << tsOptions->getTimeField() << 1));
    if (!swBucketsSpec.isOK()) {
        return swBucketsSpec.getStatus();
    }

    const std::string indexName = str::stream()
        << *tsOptions->getMetaField() << "_1_" << tsOptions->getTimeField() << "_1";

    BSONObjBuilder builder;
    builder.append("v", 2);
    builder.append("name", indexName);
    builder.append("key", swBucketsSpec.getValue());

    // Add the collection collation when building the default timeseries index if the collation is
    // non-empty.
    if (!collator.isEmpty()) {
        builder.append("collation", collator);
    }

    IndexBuildsCoordinator::createIndexesOnEmptyCollection(opCtx,
                                                           collection,
                                                           {builder.obj()},
                                                           /*fromMigrate=*/false);
    return Status::OK();
}
}  // namespace mongo::timeseries
