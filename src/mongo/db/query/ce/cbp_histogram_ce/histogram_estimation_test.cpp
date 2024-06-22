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

#include <memory>
#include <tuple>
#include <vector>

#include "mongo/db/query/ce/cbp_histogram_ce/histogram_common.h"
#include "mongo/db/query/ce/cbp_histogram_ce/histogram_predicate_estimation.h"
#include "mongo/db/query/ce/cbp_histogram_ce/test_helpers.h"
#include "mongo/unittest/assert.h"
#include "mongo/unittest/framework.h"


namespace mongo::optimizer::cbp::ce {
namespace {

TEST(EstimatorTest, ManualHistogram) {
    std::vector<BucketData> data{{0, 1.0, 1.0, 1.0},
                                 {10, 1.0, 10.0, 5.0},
                                 {20, 3.0, 15.0, 3.0},
                                 {30, 1.0, 10.0, 4.0},
                                 {40, 2.0, 0.0, 0.0},
                                 {50, 1.0, 10.0, 5.0}};
    const stats::ScalarHistogram& hist = createHistogram(data);

    ASSERT_EQ(55.0, cbp::ce::getTotals(hist).card);

    ASSERT_EQ(1.0,
              estimateCardinalityScalarHistogramInteger(hist, 0, cbp::ce::EstimationType::kEqual));
}

}  // namespace
}  // namespace mongo::optimizer::cbp::ce
