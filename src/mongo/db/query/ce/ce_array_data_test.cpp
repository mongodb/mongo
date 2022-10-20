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

#include <vector>

#include "mongo/db/exec/sbe/values/value.h"
#include "mongo/db/query/ce/array_histogram.h"
#include "mongo/db/query/ce/ce_test_utils.h"
#include "mongo/db/query/ce/histogram_estimation.h"
#include "mongo/db/query/query_test_service_context.h"
#include "mongo/unittest/unittest.h"

namespace mongo::ce {
namespace {

using namespace sbe;

/**
 * Structure representing a range query and its estimated and actual cardinalities.
 * Used to record hand-crafted queries over a pre-generated dataset.
 */
struct QuerySpec {
    // Low bound of the query range.
    int32_t low;
    // Upper bound of the query range.
    int32_t high;
    // Estimated cardinality of $match query.
    double estMatch;
    // Actual cardinality of $match query.
    double actMatch;
    // Estimated cardinality of $elemMatch query.
    double estElemMatch;
    // Actual cardinality of $elemMatch query.
    double actElemMatch;
};

static std::pair<double, double> computeErrors(size_t actualCard, double estimatedCard) {
    double error = estimatedCard - actualCard;
    double relError = (actualCard == 0) ? (estimatedCard == 0 ? 0.0 : -1.0) : error / actualCard;
    return std::make_pair(error, relError);
}

static std::string serializeQuery(QuerySpec& q, bool isElemMatch) {
    std::ostringstream os;
    os << "{$match: {a: {";
    if (isElemMatch) {
        os << "$elemMatch: {";
    }
    os << "$gt: " << q.low;
    os << ", $lt: " << q.high;
    if (isElemMatch) {
        os << "}";
    }
    os << "}}}\n";
    return os.str();
}

static std::string computeRMSE(std::vector<QuerySpec>& querySet, bool isElemMatch) {
    double rms = 0.0, relRms = 0.0, meanAbsSelErr = 0.0;
    size_t trialSize = querySet.size();
    const size_t dataSize = 1000;

    std::ostringstream os;
    os << "\nQueries:\n";
    for (auto& q : querySet) {
        double estimatedCard = isElemMatch ? q.estElemMatch : q.estMatch;
        double actualCard = isElemMatch ? q.actElemMatch : q.actMatch;

        auto [error, relError] = computeErrors(actualCard, estimatedCard);
        rms += error * error;
        relRms += relError * relError;
        meanAbsSelErr += std::abs(error);
        os << serializeQuery(q, isElemMatch);
        os << "Estimated: " << estimatedCard << " Actual " << actualCard << " (Error: " << error
           << " RelError: " << relError << ")\n\n";
    }
    rms = std::sqrt(rms / trialSize);
    relRms = std::sqrt(relRms / trialSize);
    meanAbsSelErr /= (trialSize * dataSize);

    os << "=====" << (isElemMatch ? " ElemMatch errors: " : "Match errors:") << "=====\n";
    os << "RMSE : " << rms << " RelRMSE : " << relRms
       << " MeanAbsSelectivityError: " << meanAbsSelErr << std::endl;
    return os.str();
}

TEST(EstimatorArrayDataTest, Histogram1000ArraysSmall10Buckets) {
    std::vector<BucketData> scalarData{{}};
    const ScalarHistogram scalarHist = createHistogram(scalarData);

    std::vector<BucketData> minData{{0, 5.0, 0.0, 0.0},
                                    {553, 2.0, 935.0, 303.0},
                                    {591, 4.0, 2.0, 1.0},
                                    {656, 2.0, 21.0, 12.0},
                                    {678, 3.0, 6.0, 3.0},
                                    {693, 2.0, 1.0, 1.0},
                                    {730, 1.0, 6.0, 3.0},
                                    {788, 1.0, 2.0, 2.0},
                                    {847, 2.0, 4.0, 1.0},
                                    {867, 1.0, 0.0, 0.0}};

    const ScalarHistogram aMinHist = createHistogram(minData);

    std::vector<BucketData> maxData{{117, 1.0, 0.0, 0.0},
                                    {210, 1.0, 1.0, 1.0},
                                    {591, 1.0, 8.0, 4.0},
                                    {656, 1.0, 0.0, 0.0},
                                    {353, 2.0, 18.0, 9.0},
                                    {610, 5.0, 125.0, 65.0},
                                    {733, 8.0, 134.0, 53.0},
                                    {768, 6.0, 50.0, 16.0},
                                    {957, 8.0, 448.0, 137.0},
                                    {1000, 7.0, 176.0, 40.0}};

    const ScalarHistogram aMaxHist = createHistogram(maxData);

    std::vector<BucketData> uniqueData{{0, 5.0, 0.0, 0.0},
                                       {16, 11.0, 74.0, 13.0},
                                       {192, 13.0, 698.0, 148.0},
                                       {271, 9.0, 312.0, 70.0},
                                       {670, 7.0, 1545.0, 355.0},
                                       {712, 9.0, 159.0, 32.0},
                                       {776, 11.0, 247.0, 54.0},
                                       {869, 9.0, 361.0, 85.0},
                                       {957, 8.0, 323.0, 76.0},
                                       {1000, 7.0, 188.0, 40.0}};

    const ScalarHistogram aUniqueHist = createHistogram(uniqueData);

    std::map<value::TypeTags, size_t> typeCounts;
    std::map<value::TypeTags, size_t> arrayTypeCounts;
    // Dataset generated as 1000 arrays of size between 3 to 5.
    typeCounts.insert({value::TypeTags::Array, 1000});
    arrayTypeCounts.insert({value::TypeTags::NumberInt32, 3996});

    const ArrayHistogram arrHist(scalarHist,
                                 typeCounts,
                                 aUniqueHist,
                                 aMinHist,
                                 aMaxHist,
                                 arrayTypeCounts,
                                 0 /* emptyArrayCount */);

    std::vector<QuerySpec> querySet{{10, 20, 35.7, 93.0, 37.8, 39.0},
                                    {10, 60, 103.3, 240.0, 158.0, 196.0},
                                    {320, 330, 554.5, 746.0, 26.0, 30.0},
                                    {320, 400, 672.9, 832.0, 231.5, 298.0},
                                    {980, 990, 88.8, 101.0, 36.5, 41.0},
                                    {970, 1050, 129.7, 141.0, 129.7, 141.0}};

    for (const auto q : querySet) {
        // $match query, includeScalar = true.
        double estCard = estimateCardRange(arrHist,
                                           false /* lowInclusive */,
                                           value::TypeTags::NumberInt32,
                                           sbe::value::bitcastFrom<int32_t>(q.low),
                                           false /* highInclusive */,
                                           value::TypeTags::NumberInt32,
                                           sbe::value::bitcastFrom<int32_t>(q.high),
                                           true /* includeScalar */);
        ASSERT_APPROX_EQUAL(estCard, q.estMatch, 0.1);

        // $elemMatch query, includeScalar = false.
        estCard = estimateCardRange(arrHist,
                                    false /* lowInclusive */,
                                    value::TypeTags::NumberInt32,
                                    sbe::value::bitcastFrom<int32_t>(q.low),
                                    false /* highInclusive */,
                                    value::TypeTags::NumberInt32,
                                    sbe::value::bitcastFrom<int32_t>(q.high),
                                    false /* includeScalar */);
        ASSERT_APPROX_EQUAL(estCard, q.estElemMatch, 0.1);
    }
    std::cout << computeRMSE(querySet, false /* isElemMatch */) << std::endl;
    std::cout << computeRMSE(querySet, true /* isElemMatch */) << std::endl;
}

TEST(EstimatorArrayDataTest, Histogram1000ArraysLarge10Buckets) {
    std::vector<BucketData> scalarData{{}};
    const ScalarHistogram scalarHist = createHistogram(scalarData);

    std::vector<BucketData> minData{{0, 2.0, 0.0, 0.0},
                                    {1324, 4.0, 925.0, 408.0},
                                    {1389, 5.0, 7.0, 5.0},
                                    {1521, 2.0, 16.0, 10.0},
                                    {1621, 2.0, 13.0, 7.0},
                                    {1852, 5.0, 10.0, 9.0},
                                    {1864, 2.0, 0.0, 0.0},
                                    {1971, 1.0, 3.0, 3.0},
                                    {2062, 2.0, 0.0, 0.0},
                                    {2873, 1.0, 0.0, 0.0}};

    const ScalarHistogram aMinHist = createHistogram(minData);

    std::vector<BucketData> maxData{{2261, 1.0, 0.0, 0.0},
                                    {2673, 1.0, 0.0, 0.0},
                                    {2930, 1.0, 1.0, 1.0},
                                    {3048, 2.0, 2.0, 2.0},
                                    {3128, 3.0, 1.0, 1.0},
                                    {3281, 2.0, 0.0, 0.0},
                                    {3378, 2.0, 7.0, 5.0},
                                    {3453, 4.0, 2.0, 2.0},
                                    {3763, 6.0, 44.0, 23.0},
                                    {5000, 1.0, 920.0, 416.0}};

    const ScalarHistogram aMaxHist = createHistogram(maxData);

    std::vector<BucketData> uniqueData{{0, 2.0, 0.0, 0.0},
                                       {1106, 9.0, 1970.0, 704.0},
                                       {1542, 11.0, 736.0, 280.0},
                                       {3267, 6.0, 3141.0, 1097.0},
                                       {3531, 6.0, 461.0, 175.0},
                                       {3570, 7.0, 48.0, 20.0},
                                       {4573, 8.0, 1851.0, 656.0},
                                       {4619, 6.0, 65.0, 30.0},
                                       {4782, 5.0, 265.0, 99.0},
                                       {5000, 1.0, 342.0, 135.0}};

    const ScalarHistogram aUniqueHist = createHistogram(uniqueData);

    std::map<value::TypeTags, size_t> typeCounts;
    std::map<value::TypeTags, size_t> arrayTypeCounts;
    // Dataset generated as 1000 arrays of size between 8 to 10.
    typeCounts.insert({value::TypeTags::Array, 1000});
    arrayTypeCounts.insert({value::TypeTags::NumberInt32, 8940});

    const ArrayHistogram arrHist(scalarHist,
                                 typeCounts,
                                 aUniqueHist,
                                 aMinHist,
                                 aMaxHist,
                                 arrayTypeCounts,
                                 0 /* emptyArrayCount */);

    std::vector<QuerySpec> querySet{{10, 20, 13.7, 39.0, 9.7, 26.0},
                                    {10, 60, 41.6, 108.0, 55.7, 101.0},
                                    {1000, 1010, 705.4, 861.0, 9.7, 7.0},
                                    {1000, 1050, 733.3, 884.0, 55.7, 87.0},
                                    {3250, 3300, 988.0, 988.0, 59.3, 86.0},
                                    {4970, 4980, 23.3, 53.0, 8.5, 16.0}};

    for (const auto q : querySet) {
        // $match query, includeScalar = true.
        double estCard = estimateCardRange(arrHist,
                                           false /* lowInclusive */,
                                           value::TypeTags::NumberInt32,
                                           sbe::value::bitcastFrom<int32_t>(q.low),
                                           false /* highInclusive */,
                                           value::TypeTags::NumberInt32,
                                           sbe::value::bitcastFrom<int32_t>(q.high),
                                           true /* includeScalar */);
        ASSERT_APPROX_EQUAL(estCard, q.estMatch, 0.1);

        // $elemMatch query, includeScalar = false.
        estCard = estimateCardRange(arrHist,
                                    false /* lowInclusive */,
                                    value::TypeTags::NumberInt32,
                                    sbe::value::bitcastFrom<int32_t>(q.low),
                                    false /* highInclusive */,
                                    value::TypeTags::NumberInt32,
                                    sbe::value::bitcastFrom<int32_t>(q.high),
                                    false /* includeScalar */);
        ASSERT_APPROX_EQUAL(estCard, q.estElemMatch, 0.1);
    }
    std::cout << computeRMSE(querySet, false /* isElemMatch */) << std::endl;
    std::cout << computeRMSE(querySet, true /* isElemMatch */) << std::endl;
}
}  // namespace
}  // namespace mongo::ce
