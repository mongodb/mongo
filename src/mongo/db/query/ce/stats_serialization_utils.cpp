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


#include "mongo/db/query/ce/stats_serialization_utils.h"

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/exec/sbe/values/bson.h"

namespace mongo::stats_serialization_utils {

BSONObj makeStatsBucket(TypeValuePair typeValue,
                        long boundaryCount,
                        long rangeCount,
                        long rangeDistincts,
                        long cumulativeCount,
                        long cumulativeDistincts) {
    BSONObjBuilder bucketBuilder;
    sbe::bson::appendValueToBsonObj(
        bucketBuilder, "upperBoundary", typeValue.first, typeValue.second);
    bucketBuilder.append("boundaryCount", boundaryCount);
    bucketBuilder.append("rangeCount", rangeCount);
    bucketBuilder.append("rangeDistincts", rangeDistincts);
    bucketBuilder.append("cumulativeCount", cumulativeCount);
    bucketBuilder.append("cumulativeDistincts", cumulativeDistincts);
    return bucketBuilder.obj();
}

BSONObj makeStatsPath(StringData path,
                      long documents,
                      boost::optional<long> documentsSampled,
                      boost::optional<double> samplingRate,
                      boost::optional<long> samplesRequested,
                      boost::optional<std::pair<long, long>> boolCount,
                      boost::optional<TypeCount> typeCount,
                      boost::optional<std::list<BSONObj>> scalarHistogram,
                      boost::optional<BSONObj> arrayHistogram) {
    BSONObjBuilder statsBuilder;
    statsBuilder.append("path", path);
    statsBuilder.append("documents", documents);
    if (documentsSampled) {
        statsBuilder.append("documentsSampled", *documentsSampled);
    }
    if (samplingRate) {
        statsBuilder.append("samplingRate", *samplingRate);
    }
    if (samplesRequested) {
        statsBuilder.append("samplesRequested", *samplesRequested);
    }
    if (boolCount) {
        BSONObjBuilder boolCountBuilder = statsBuilder.subobjStart("boolCount");
        boolCountBuilder.append("trueCount", boolCount->first);
        boolCountBuilder.append("falseCount", boolCount->second);
        boolCountBuilder.done();
    }
    if (typeCount) {
        BSONArrayBuilder typeCountBuilder = statsBuilder.subarrayStart("typeCount");
        for (const auto& typeElem : *typeCount) {
            auto typeElemObj = BSON("typeName" << typeElem.first << "count" << typeElem.second);
            typeCountBuilder.append(typeElemObj);
        }
        typeCountBuilder.done();
    }
    if (scalarHistogram) {
        BSONArrayBuilder histArrBuilder = statsBuilder.subarrayStart("scalarHistogram");
        histArrBuilder.append(*scalarHistogram);
        histArrBuilder.done();
    }
    if (arrayHistogram) {
        statsBuilder.append("arrayHistogram", *arrayHistogram);
    }
    return statsBuilder.obj();
}

}  // namespace mongo::stats_serialization_utils
