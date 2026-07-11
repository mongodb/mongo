// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/compiler/ce/sampling/math.h"

#include "mongo/unittest/unittest.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQueryCE

namespace mongo::ce {

TEST(ComputeNDV, EnsureNewtonRaphsonNDVDoesNotThrow) {
    // Rather than asserting on the value, we are simply ensuring the algorithm does not fail.
    for (size_t sampleSize = 2; sampleSize < 50; sampleSize++) {
        for (size_t sampleNDV = 1; sampleNDV <= sampleSize; sampleNDV++) {
            LOGV2(11158513,
                  "Testing Newton Raphson NDV with",
                  "sampleSize"_attr = sampleSize,
                  "sampleNDV"_attr = sampleNDV);
            auto res = newtonRaphsonNDV(sampleNDV, sampleSize).toDouble();
            LOGV2(11228301, "Newton Raphson NDV result", "res"_attr = res);
            ASSERT_GTE(res, sampleNDV);
        }
    }
}
}  // namespace mongo::ce
