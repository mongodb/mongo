/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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

#include "mongo/db/timeseries/bucket_catalog/bucket_catalog_helpers.h"

#include "mongo/db/dbdirectclient.h"
#include "mongo/db/record_id_helpers.h"
#include "mongo/db/storage/storage_parameters_gen.h"
#include "mongo/db/timeseries/bucket_catalog/bucket_catalog.h"
#include "mongo/db/timeseries/timeseries_constants.h"
#include "mongo/logv2/log.h"
#include "mongo/logv2/redaction.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kStorage

namespace mongo::timeseries::bucket_catalog {

namespace {

void normalizeArray(BSONArrayBuilder* builder, const BSONObj& obj);
void normalizeObject(BSONObjBuilder* builder, const BSONObj& obj);

StatusWith<std::pair<const BSONObj, const BSONObj>> extractMinAndMax(const BSONObj& bucketDoc) {
    const BSONObj& controlObj = bucketDoc.getObjectField(kBucketControlFieldName);
    if (controlObj.isEmpty()) {
        return {ErrorCodes::BadValue,
                str::stream() << "The control field is empty or not an object: "
                              << redact(bucketDoc)};
    }

    const BSONObj& minObj = controlObj.getObjectField(kBucketControlMinFieldName);
    const BSONObj& maxObj = controlObj.getObjectField(kBucketControlMaxFieldName);
    if (minObj.isEmpty() || maxObj.isEmpty()) {
        return {ErrorCodes::BadValue,
                str::stream() << "The control min and/or max fields are empty or not objects: "
                              << redact(bucketDoc)};
    }

    return std::make_pair(minObj, maxObj);
}

void normalizeArray(BSONArrayBuilder* builder, const BSONObj& obj) {
    for (auto& arrayElem : obj) {
        if (arrayElem.type() == BSONType::Array) {
            BSONArrayBuilder subArray = builder->subarrayStart();
            normalizeArray(&subArray, arrayElem.Obj());
        } else if (arrayElem.type() == BSONType::Object) {
            BSONObjBuilder subObject = builder->subobjStart();
            normalizeObject(&subObject, arrayElem.Obj());
        } else {
            builder->append(arrayElem);
        }
    }
}

void normalizeObject(BSONObjBuilder* builder, const BSONObj& obj) {
    // BSONObjIteratorSorted provides an abstraction similar to what this function does. However it
    // is using a lexical comparison that is slower than just doing a binary comparison of the field
    // names. That is all we need here as we are looking to create something that is binary
    // comparable no matter of field order provided by the user.

    // Helper that extracts the necessary data from a BSONElement that we can sort and re-construct
    // the same BSONElement from.
    struct Field {
        BSONElement element() const {
            return BSONElement(fieldName.rawData() - 1,  // Include type byte before field name
                               fieldName.size() + 1,     // Include null terminator after field name
                               totalSize);
        }
        bool operator<(const Field& rhs) const {
            return fieldName < rhs.fieldName;
        }
        StringData fieldName;
        int totalSize;
    };

    // Put all elements in a buffer, sort it and then continue normalize in sorted order
    auto num = obj.nFields();
    static constexpr std::size_t kNumStaticFields = 16;
    boost::container::small_vector<Field, kNumStaticFields> fields;
    fields.resize(num);
    BSONObjIterator bsonIt(obj);
    int i = 0;
    while (bsonIt.more()) {
        auto elem = bsonIt.next();
        fields[i++] = {elem.fieldNameStringData(), elem.size()};
    }
    auto it = fields.begin();
    auto end = fields.end();
    std::sort(it, end);
    for (; it != end; ++it) {
        auto elem = it->element();
        if (elem.type() == BSONType::Array) {
            BSONArrayBuilder subArray(builder->subarrayStart(elem.fieldNameStringData()));
            normalizeArray(&subArray, elem.Obj());
        } else if (elem.type() == BSONType::Object) {
            BSONObjBuilder subObject(builder->subobjStart(elem.fieldNameStringData()));
            normalizeObject(&subObject, elem.Obj());
        } else {
            builder->append(elem);
        }
    }
}

}  // namespace


BSONObj generateReopeningFilters(const Date_t& time,
                                 boost::optional<BSONElement> metadata,
                                 const std::string& controlMinTimePath,
                                 int64_t bucketMaxSpanSeconds) {
    // The bucket must be uncompressed.
    auto versionFilter = BSON(kControlVersionPath << kTimeseriesControlDefaultVersion);

    // The bucket cannot be closed (aka open for new measurements).
    auto closedFlagFilter =
        BSON("$or" << BSON_ARRAY(BSON(kControlClosedPath << BSON("$exists" << false))
                                 << BSON(kControlClosedPath << false)));

    // The measurement meta field must match the bucket 'meta' field. If the field is not specified
    // we can only insert into buckets which also do not have a meta field.

    BSONObj metaFieldFilter;
    if (metadata && (*metadata).ok()) {
        BSONObjBuilder builder;
        builder.appendAs(*metadata, kBucketMetaFieldName);
        metaFieldFilter = builder.obj();
    } else {
        metaFieldFilter = BSON(kBucketMetaFieldName << BSON("$exists" << false));
    }

    // (minimumTs <= measurementTs) && (minimumTs + maxSpanSeconds > measurementTs)
    auto measurementMaxDifference = time - Seconds(bucketMaxSpanSeconds);
    auto lowerBound = BSON(controlMinTimePath << BSON("$lte" << time));
    auto upperBound = BSON(controlMinTimePath << BSON("$gt" << measurementMaxDifference));
    auto timeRangeFilter = BSON("$and" << BSON_ARRAY(lowerBound << upperBound));

    return BSON("$and" << BSON_ARRAY(versionFilter << closedFlagFilter << timeRangeFilter
                                                   << metaFieldFilter));
}

StatusWith<MinMax> generateMinMaxFromBucketDoc(const BSONObj& bucketDoc,
                                               const StringData::ComparatorInterface* comparator) {
    auto swDocs = extractMinAndMax(bucketDoc);
    if (!swDocs.isOK()) {
        return swDocs.getStatus();
    }

    const auto& [minObj, maxObj] = swDocs.getValue();

    try {
        return MinMax::parseFromBSON(minObj, maxObj, comparator);
    } catch (...) {
        return exceptionToStatus();
    }
}

StatusWith<Schema> generateSchemaFromBucketDoc(const BSONObj& bucketDoc,
                                               const StringData::ComparatorInterface* comparator) {
    auto swDocs = extractMinAndMax(bucketDoc);
    if (!swDocs.isOK()) {
        return swDocs.getStatus();
    }

    const auto& [minObj, maxObj] = swDocs.getValue();

    try {
        return Schema::parseFromBSON(minObj, maxObj, comparator);
    } catch (...) {
        return exceptionToStatus();
    }
}

StatusWith<Date_t> extractTime(const BSONObj& doc, StringData timeFieldName) {
    auto timeElem = doc[timeFieldName];
    if (!timeElem || BSONType::Date != timeElem.type()) {
        return {ErrorCodes::BadValue,
                str::stream() << "'" << timeFieldName << "' must be present and contain a "
                              << "valid BSON UTC datetime value"};
    }
    return timeElem.Date();
}


StatusWith<std::pair<Date_t, BSONElement>> extractTimeAndMeta(const BSONObj& doc,
                                                              StringData timeFieldName,
                                                              StringData metaFieldName) {
    // Iterate the document once, checking for both fields.
    BSONElement timeElem;
    BSONElement metaElem;
    for (auto&& el : doc) {
        if (!timeElem && el.fieldNameStringData() == timeFieldName) {
            timeElem = el;
            if (metaElem) {
                break;
            }
        } else if (!metaElem && el.fieldNameStringData() == metaFieldName) {
            metaElem = el;
            if (timeElem) {
                break;
            }
        }
    }

    if (!timeElem || BSONType::Date != timeElem.type()) {
        return {ErrorCodes::BadValue,
                str::stream() << "'" << timeFieldName << "' must be present and contain a "
                              << "valid BSON UTC datetime value"};
    }
    auto time = timeElem.Date();

    return std::make_pair(time, metaElem);
}

void normalizeMetadata(BSONObjBuilder* builder,
                       const BSONElement& elem,
                       boost::optional<StringData> as) {
    if (elem.type() == BSONType::Array) {
        BSONArrayBuilder subArray(
            builder->subarrayStart(as.has_value() ? as.value() : elem.fieldNameStringData()));
        normalizeArray(&subArray, elem.Obj());
    } else if (elem.type() == BSONType::Object) {
        BSONObjBuilder subObject(
            builder->subobjStart(as.has_value() ? as.value() : elem.fieldNameStringData()));
        normalizeObject(&subObject, elem.Obj());
    } else {
        if (as.has_value()) {
            builder->appendAs(elem, as.value());
        } else {
            builder->append(elem);
        }
    }
}

BSONObj findDocFromOID(OperationContext* opCtx, const Collection* coll, const OID& bucketId) {
    Snapshotted<BSONObj> bucketObj;
    auto rid = record_id_helpers::keyForOID(bucketId);
    auto foundDoc = coll->findDoc(opCtx, rid, &bucketObj);

    return (foundDoc) ? bucketObj.value() : BSONObj();
}

void handleDirectWrite(OperationContext* opCtx, const NamespaceString& ns, const OID& bucketId) {
    // Ensure we have the view namespace, as that's what the BucketCatalog operates on.
    NamespaceString resolvedNs =
        ns.isTimeseriesBucketsCollection() ? ns.getTimeseriesViewNamespace() : ns;

    // First notify the BucketCatalog that we intend to start a direct write, so we can conflict
    // with any already-prepared operation, and also block bucket reopening if it's enabled.
    auto& bucketCatalog = BucketCatalog::get(opCtx);
    bucketCatalog.directWriteStart(resolvedNs, bucketId);

    // Then register callbacks so we can let the BucketCatalog know that we are done with our
    // direct write after the actual write takes place (or is abandoned), and allow reopening.
    opCtx->recoveryUnit()->onCommit(
        [svcCtx = opCtx->getServiceContext(), resolvedNs, bucketId](boost::optional<Timestamp>) {
            auto& bucketCatalog = BucketCatalog::get(svcCtx);
            bucketCatalog.directWriteFinish(resolvedNs, bucketId);
        });
    opCtx->recoveryUnit()->onRollback(
        [svcCtx = opCtx->getServiceContext(), resolvedNs, bucketId]() {
            auto& bucketCatalog = BucketCatalog::get(svcCtx);
            bucketCatalog.directWriteFinish(resolvedNs, bucketId);
        });
}

}  // namespace mongo::timeseries::bucket_catalog
