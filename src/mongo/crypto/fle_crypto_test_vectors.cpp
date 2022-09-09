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

#include "mongo/platform/basic.h"

#include <algorithm>
#include <cstdint>
#include <iostream>
#include <limits>
#include <sstream>
#include <string>
#include <tuple>
#include <vector>

#include "mongo/base/data_range.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/bson/json.h"
#include "mongo/config.h"
#include "mongo/crypto/fle_crypto.h"
#include "mongo/db/operation_context.h"
#include "mongo/logv2/log.h"
#include "mongo/rpc/object_check.h"
#include "mongo/unittest/bson_test_util.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/hex.h"
#include "mongo/util/time_support.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest

namespace mongo {

// Edge calculator

template <typename T>
struct EdgeCalcTestVector {
    std::function<std::unique_ptr<Edges>(T, boost::optional<T>, boost::optional<T>, int)> getEdgesT;
    T value;
    boost::optional<T> min, max;
    int sparsity;
    stdx::unordered_set<std::string> expectedEdges;

    bool validate() const {
        auto edgeCalc = getEdgesT(value, min, max, sparsity);
        auto edges = edgeCalc->get();
        for (const std::string& ee : expectedEdges) {
            if (!std::any_of(edges.begin(), edges.end(), [ee](auto edge) { return edge == ee; })) {
                LOGV2_ERROR(6775120,
                            "Expected edge not found",
                            "expected-edge"_attr = ee,
                            "value"_attr = value,
                            "min"_attr = min,
                            "max"_attr = max,
                            "sparsity"_attr = sparsity,
                            "edges"_attr = edges,
                            "expected-edges"_attr = expectedEdges);
                return false;
            }
        }

        for (const StringData& edgeSd : edges) {
            std::string edge = edgeSd.toString();
            if (std::all_of(expectedEdges.begin(), expectedEdges.end(), [edge](auto ee) {
                    return edge != ee;
                })) {
                LOGV2_ERROR(6775121,
                            "An edge was found that is not expected",
                            "edge"_attr = edge,
                            "value"_attr = value,
                            "min"_attr = min,
                            "max"_attr = max,
                            "sparsity"_attr = sparsity,
                            "edges"_attr = edges,
                            "expected-edges"_attr = expectedEdges);
                return false;
            }
        }

        if (edges.size() != expectedEdges.size()) {
            LOGV2_ERROR(6775122,
                        "Unexpected number of elements in edges. Check for duplicates",
                        "value"_attr = value,
                        "min"_attr = min,
                        "max"_attr = max,
                        "sparsity"_attr = sparsity,
                        "edges"_attr = edges,
                        "expected-edges"_attr = expectedEdges);
            return false;
        }

        return true;
    }
};

TEST(EdgeCalcTest, Int32_TestVectors) {
    std::vector<EdgeCalcTestVector<int32_t>> testVectors = {
#include "test_vectors/edges_int32.cstruct"
    };
    for (const auto& testVector : testVectors) {
        ASSERT_TRUE(testVector.validate());
    }
}

TEST(EdgeCalcTest, Int64_TestVectors) {
    std::vector<EdgeCalcTestVector<int64_t>> testVectors = {
#include "test_vectors/edges_int64.cstruct"
    };
    for (const auto& testVector : testVectors) {
        ASSERT_TRUE(testVector.validate());
    }
}

TEST(EdgeCalcTest, Double_TestVectors) {
    std::vector<EdgeCalcTestVector<double>> testVectors = {
#include "test_vectors/edges_double.cstruct"
    };
    for (const auto& testVector : testVectors) {
        ASSERT_TRUE(testVector.validate());
    }
}

template <typename T>
struct MinCoverTestVector {
    T rangeMin, rangeMax;
    T min, max;
    int sparsity;
    const char* expect;

    bool validate(
        std::function<std::vector<std::string>(T, T, boost::optional<T>, boost::optional<T>, int)>
            algo) const {
        auto result = algo(rangeMin, rangeMax, min, max, sparsity);

        std::stringstream ss(expect);
        std::vector<std::string> vexpect;
        std::string item;
        while (std::getline(ss, item, '\n') && !item.empty()) {
            vexpect.push_back(std::move(item));
        }

        if (std::equal(result.begin(), result.end(), vexpect.begin())) {
            return true;
        }

        LOGV2_ERROR(6860020,
                    "MinCover algorithm produced unexpected result",
                    "rangeMin"_attr = rangeMin,
                    "rangeMax"_attr = rangeMax,
                    "min"_attr = min,
                    "max"_attr = max,
                    "sparsity"_attr = sparsity,
                    "expect"_attr = expect,
                    "got"_attr = result);
        return false;
    }
};


#pragma clang optimize off

TEST(MinCoverCalcTest, Int32_TestVectors) {
    const MinCoverTestVector<int32_t> testVectors[] = {
#include "test_vectors/mincover_int32.cstruct"
    };
    for (const auto& testVector : testVectors) {
        ASSERT_TRUE(testVector.validate(minCoverInt32));
    }
}

TEST(MinCoverCalcTest, Int64_TestVectors) {
    const MinCoverTestVector<int64_t> testVectors[] = {
#include "test_vectors/mincover_int64.cstruct"
    };
    for (const auto& testVector : testVectors) {
        ASSERT_TRUE(testVector.validate(minCoverInt64));
    }
}

TEST(MinCoverCalcTest, Double_TestVectors) {
    MinCoverTestVector<double> testVectors[] = {
#include "test_vectors/mincover_double.cstruct"
    };
    for (const auto& testVector : testVectors) {
        ASSERT_TRUE(testVector.validate(minCoverDouble));
    }
}

#pragma clang optimize on

}  // namespace mongo
