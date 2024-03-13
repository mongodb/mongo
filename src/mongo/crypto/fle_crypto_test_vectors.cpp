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

#include <cstdint>
#include <functional>
#include <iostream>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <absl/container/node_hash_set.h>
#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>

#include "mongo/base/string_data.h"
#include "mongo/config.h"  // IWYU pragma: keep
#include "mongo/crypto/fle_crypto.h"
#include "mongo/logv2/log.h"
#include "mongo/logv2/log_attr.h"
#include "mongo/logv2/log_component.h"
#include "mongo/platform/decimal128.h"
#include "mongo/rpc/object_check.h"  // IWYU pragma: keep
#include "mongo/stdx/unordered_set.h"
#include "mongo/unittest/assert.h"
#include "mongo/unittest/framework.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest

namespace mongo {

// Edge calculator

template <typename T>
struct EdgeCalcTestVector {
    std::function<std::unique_ptr<Edges>(T, boost::optional<T>, boost::optional<T>, int, int)>
        getEdgesT;
    T value;
    boost::optional<T> min, max;
    int sparsity;
    stdx::unordered_set<std::string> expectedEdges;

    bool validate() const {
        auto edgeCalc = getEdgesT(value, min, max, sparsity, 0);
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

        for (StringData edgeSd : edges) {
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
#include "test_vectors/edges_int32.cstruct"  // IWYU pragma: keep
    };
    for (const auto& testVector : testVectors) {
        ASSERT_TRUE(testVector.validate());
    }
}

TEST(EdgeCalcTest, Int64_TestVectors) {
    std::vector<EdgeCalcTestVector<int64_t>> testVectors = {
#include "test_vectors/edges_int64.cstruct"  // IWYU pragma: keep
    };
    for (const auto& testVector : testVectors) {
        ASSERT_TRUE(testVector.validate());
    }
}

std::unique_ptr<Edges> getEdgesDoubleForTest(double value,
                                             boost::optional<double> min,
                                             boost::optional<double> max,
                                             int sparsity,
                                             int trimFactor) {
    // The non-precision test vectors set min/max which is not allowed
    return getEdgesDouble(value, boost::none, boost::none, boost::none, sparsity, trimFactor);
}

TEST(EdgeCalcTest, Double_TestVectors) {
    std::vector<EdgeCalcTestVector<double>> testVectors = {
#include "test_vectors/edges_double.cstruct"  // IWYU pragma: keep
    };
    for (const auto& testVector : testVectors) {
        ASSERT_TRUE(testVector.validate());
    }
}


std::unique_ptr<Edges> getEdgesDecimal128ForTest(Decimal128 value,
                                                 boost::optional<Decimal128> min,
                                                 boost::optional<Decimal128> max,
                                                 int sparsity,
                                                 int trimFactor) {

    // The non-precision test vectors set min/max which is not allowed
    return getEdgesDecimal128(value, boost::none, boost::none, boost::none, sparsity, trimFactor);
}


TEST(EdgeCalcTest, Decimal128_TestVectors) {
    std::vector<EdgeCalcTestVector<Decimal128>> testVectors = {
#include "test_vectors/edges_decimal128.cstruct"  // IWYU pragma: keep
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
    std::string expect;

    bool validate(
        std::function<std::vector<std::string>(
            T, bool, T, bool, boost::optional<T>, boost::optional<T>, int, int)> algo) const {
        auto result = algo(rangeMin, true, rangeMax, true, min, max, sparsity, 0);

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
#include "test_vectors/mincover_int32.cstruct"  // IWYU pragma: keep
    };
    for (const auto& testVector : testVectors) {
        ASSERT_TRUE(testVector.validate(minCoverInt32));
    }
}

TEST(MinCoverCalcTest, Int64_TestVectors) {
    const MinCoverTestVector<int64_t> testVectors[] = {
#include "test_vectors/mincover_int64.cstruct"  // IWYU pragma: keep
    };
    for (const auto& testVector : testVectors) {
        ASSERT_TRUE(testVector.validate(minCoverInt64));
    }
}

std::vector<std::string> minCoverDoubleForTest(double lowerBound,
                                               bool includeLowerBound,
                                               double upperBound,
                                               bool includeUpperBound,
                                               boost::optional<double> min,
                                               boost::optional<double> max,
                                               int sparsity,
                                               int trimFactor) {
    // The non-precision test vectors set min/max which is not allowed
    return minCoverDouble(lowerBound,
                          includeLowerBound,
                          upperBound,
                          includeUpperBound,
                          boost::none,
                          boost::none,
                          boost::none,
                          sparsity,
                          trimFactor);
}

TEST(MinCoverCalcTest, Double_TestVectors) {
    MinCoverTestVector<double> testVectors[] = {
#include "test_vectors/mincover_double.cstruct"  // IWYU pragma: keep
    };
    for (const auto& testVector : testVectors) {
        ASSERT_TRUE(testVector.validate(minCoverDoubleForTest));
    }
}

std::vector<std::string> minCoverDecimal128ForTest(Decimal128 lowerBound,
                                                   bool includeLowerBound,
                                                   Decimal128 upperBound,
                                                   bool includeUpperBound,
                                                   boost::optional<Decimal128> min,
                                                   boost::optional<Decimal128> max,
                                                   int sparsity,
                                                   int trimFactor) {

    // The non-precision test vectors set min/max which is not allowed
    return minCoverDecimal128(lowerBound,
                              includeLowerBound,
                              upperBound,
                              includeUpperBound,
                              boost::none,
                              boost::none,
                              boost::none,
                              sparsity,
                              trimFactor);
}


TEST(MinCoverCalcTest, Decimal128_TestVectors) {
    MinCoverTestVector<Decimal128> testVectors[] = {
#include "test_vectors/mincover_decimal128.cstruct"  // IWYU pragma: keep
    };
    for (const auto& testVector : testVectors) {
        ASSERT_TRUE(testVector.validate(minCoverDecimal128ForTest));
    }
}

#pragma clang optimize on


template <typename T>
struct MinCoverTestVectorPrecision {
    T rangeMin, rangeMax;
    T min, max;
    int sparsity;
    uint32_t precision;
    std::string expect;

    bool validate(std::function<std::vector<std::string>(
                      T, bool, T, bool, boost::optional<T>, boost::optional<T>, uint32_t, int, int)>
                      algo) const {
        auto result = algo(rangeMin, true, rangeMax, true, min, max, precision, sparsity, 0);

        std::stringstream ss(expect);
        std::vector<std::string> vexpect;
        std::string item;
        while (std::getline(ss, item, '\n') && !item.empty()) {
            vexpect.push_back(std::move(item));
        }

        if (std::equal(result.begin(), result.end(), vexpect.begin())) {
            return true;
        }

        LOGV2_ERROR(6966809,
                    "MinCover algorithm produced unexpected result",
                    "rangeMin"_attr = rangeMin,
                    "rangeMax"_attr = rangeMax,
                    "min"_attr = min,
                    "max"_attr = max,
                    "sparsity"_attr = sparsity,
                    "precision"_attr = precision,
                    "expect"_attr = expect,
                    "got"_attr = result);
        return false;
    }
};

TEST(MinCoverCalcPrecisionTest, Double_TestVectors) {
    MinCoverTestVectorPrecision<double> testVectors[] = {
#include "test_vectors/mincover_double_precision.cstruct"  // IWYU pragma: keep
    };
    for (const auto& testVector : testVectors) {
        ASSERT_TRUE(testVector.validate(minCoverDouble));
    }
}

TEST(MinCoverCalcPrecisionTest, Decimal128_TestVectors) {
    MinCoverTestVectorPrecision<Decimal128> testVectors[] = {
#include "test_vectors/mincover_decimal128_precision.cstruct"  // IWYU pragma: keep
    };
    for (const auto& testVector : testVectors) {
        ASSERT_TRUE(testVector.validate(minCoverDecimal128));
    }
}
}  // namespace mongo
