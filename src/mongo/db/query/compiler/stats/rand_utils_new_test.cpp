/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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

#include "mongo/db/query/compiler/stats/rand_utils_new.h"

#include "mongo/db/exec/sbe/values/value.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"

#include <algorithm>
#include <memory>
#include <random>

namespace mongo::stats {

bool all_components_non_zero(const SBEValue& val) {
    ASSERT(val.getTag() == sbe::value::TypeTags::ObjectId);
    auto view = sbe::value::getObjectIdView(val.getValue());
    for (size_t i = 0; i < view->size(); ++i) {
        if (view->at(i) == uint8_t(0)) {
            return false;
        }
    }
    return true;
}

TEST(RandUtilsNewTest, ObjectIdAllPartsCanBeNonzero) {
    // Make the test deterministic. This is acceptable because the goal is to prove that it is
    // possible to generate a object id with all nonzero components.
    std::mt19937_64 seed(42);
    MixedDistributionDescriptor uniform{{DistrType::kUniform, 1.0}};
    TypeDistrVector td;
    td.push_back(std::make_unique<ObjectIdDistribution>(uniform, 0.2, 100));
    DatasetDescriptorNew desc{std::move(td), seed};
    const std::vector<SBEValue> randValues = desc.genRandomDataset(10);
    ASSERT(std::find_if(randValues.begin(), randValues.end(), all_components_non_zero) !=
           randValues.end());
}

}  // namespace mongo::stats
