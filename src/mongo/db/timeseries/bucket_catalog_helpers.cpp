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

#include "mongo/db/timeseries/bucket_catalog_helpers.h"
#include "mongo/db/timeseries/timeseries_constants.h"
#include "mongo/logv2/redaction.h"

namespace mongo::timeseries {

namespace {

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

}  // namespace

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

}  // namespace mongo::timeseries
