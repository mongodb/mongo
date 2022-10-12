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

#include "mongo/db/query/ce/maxdiff_test_utils.h"

#include "mongo/db/query/ce/array_histogram.h"
#include "mongo/db/query/ce/max_diff.h"

namespace mongo::ce {

ScalarHistogram createHistogram(const std::vector<BucketData>& data) {
    sbe::value::Array array;
    for (const auto& item : data) {
        const auto [tag, val] = makeInt64Value(item._v);
        array.push_back(tag, val);
    }

    value::Array bounds;
    std::vector<Bucket> buckets;

    double cumulativeFreq = 0.0;
    double cumulativeNDV = 0.0;

    for (size_t i = 0; i < data.size(); i++) {
        const auto [tag, val] = array.getAt(i);
        bounds.push_back(tag, val);

        const auto& item = data.at(i);
        cumulativeFreq += item._equalFreq + item._rangeFreq;
        cumulativeNDV += item._ndv + 1.0;
        buckets.emplace_back(
            item._equalFreq, item._rangeFreq, cumulativeFreq, item._ndv, cumulativeNDV);
    }

    return {std::move(bounds), std::move(buckets)};
}


static std::vector<std::string> convertToJSON(const std::vector<SBEValue>& input) {
    std::vector<std::string> result;

    for (size_t i = 0; i < input.size(); i++) {
        const auto [objTag, objVal] = value::makeNewObject();
        value::ValueGuard vg(objTag, objVal);

        const auto [tag, val] = input[i].get();
        // Copy the value because objVal owns its value, and the ValueGuard releases not only
        // objVal, but also its Value (in the case below - copyVal).
        const auto [copyTag, copyVal] = value::copyValue(tag, val);
        value::getObjectView(objVal)->push_back("a", copyTag, copyVal);

        std::ostringstream os;
        os << std::make_pair(objTag, objVal);
        result.push_back(os.str());
    }

    return result;
}

size_t getActualCard(OperationContext* opCtx,
                     const std::vector<SBEValue>& input,
                     const std::string& query) {
    return mongo::optimizer::runPipeline(opCtx, query, convertToJSON(input)).size();
}

std::string makeMatchExpr(const SBEValue& val, EstimationType cmpOp) {
    std::stringstream matchExpr;
    std::string cmpOpName = estimationTypeName.at(cmpOp);
    matchExpr << "[{$match: {a: {$" << cmpOpName << ": " << val.get() << "}}}]";
    return matchExpr.str();
}

ScalarHistogram makeHistogram(std::vector<SBEValue>& randData, size_t nBuckets) {
    sortValueVector(randData);
    const DataDistribution& dataDistrib = getDataDistribution(randData);
    return genMaxDiffHistogram(dataDistrib, nBuckets);
}

std::string printValueArray(const std::vector<SBEValue>& values) {
    std::stringstream strStream;
    for (size_t i = 0; i < values.size(); ++i) {
        strStream << " " << values[i].get();
    }
    return strStream.str();
}

std::string plotArrayEstimator(const ArrayHistogram& estimator, const std::string& header) {
    std::ostringstream os;
    os << header << "\n";
    if (!estimator.getScalar().empty()) {
        os << "Scalar histogram:\n" << estimator.getScalar().plot();
    }
    if (!estimator.getArrayUnique().empty()) {
        os << "Array unique histogram:\n" << estimator.getArrayUnique().plot();
    }
    if (!estimator.getArrayMin().empty()) {
        os << "Array min histogram:\n" << estimator.getArrayMin().plot();
    }
    if (!estimator.getArrayMax().empty()) {
        os << "Array max histogram:\n" << estimator.getArrayMax().plot();
    }
    if (!estimator.getTypeCounts().empty()) {
        os << "Per scalar data type value counts: ";
        for (auto tagCount : estimator.getTypeCounts()) {
            os << tagCount.first << "=" << tagCount.second << " ";
        }
    }
    if (!estimator.getArrayTypeCounts().empty()) {
        os << "\nPer array data type value counts: ";
        for (auto tagCount : estimator.getArrayTypeCounts()) {
            os << tagCount.first << "=" << tagCount.second << " ";
        }
    }
    if (estimator.isArray()) {
        os << "\nEmpty array count: " << estimator.getEmptyArrayCount();
    }
    os << "\n";

    return os.str();
}

}  // namespace mongo::ce
