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

#pragma once

#include <string>
#include <vector>


#include "mongo/db/exec/sbe/abt/sbe_abt_test_util.h"
#include "mongo/db/exec/sbe/values/value.h"
#include "mongo/db/query/ce/histogram_estimation.h"
#include "mongo/db/query/ce/scalar_histogram.h"
#include "mongo/db/query/ce/value_utils.h"

namespace mongo::ce {

class ArrayHistogram;

/**
    Given a list of SBE values and a query, create a collection containing the data,
    and count the results from the supplied query.
 */
size_t getActualCard(OperationContext* opCtx,
                     const std::vector<SBEValue>& input,
                     const std::string& query);

/**
    Given a value and a comparison operator, generate a match expression reflecting
    x cmpOp val.
*/
std::string makeMatchExpr(const SBEValue& val, EstimationType cmpOp);

/**
    Given a vector of values, create a histogram reflection the distribution of the vector
    with the supplied number of buckets.
*/
ScalarHistogram makeHistogram(std::vector<SBEValue>& randData, size_t nBuckets);

/**
    Serialize a vector of values.
*/
std::string printValueArray(const std::vector<SBEValue>& values);

/**
    Plot a set of statistics as stored in ArrayHistogram.
*/
std::string plotArrayEstimator(const ArrayHistogram& estimator, const std::string& header);

}  // namespace mongo::ce
