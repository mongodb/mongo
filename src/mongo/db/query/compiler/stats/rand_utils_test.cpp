// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/compiler/stats/rand_utils.h"

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
    DatasetDescriptor desc{std::move(td), seed};
    const std::vector<SBEValue> randValues = desc.genRandomDataset(10);
    ASSERT(std::find_if(randValues.begin(), randValues.end(), all_components_non_zero) !=
           randValues.end());
}

}  // namespace mongo::stats
