// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/config.h"  // IWYU pragma: keep
#include "mongo/crypto/fle_crypto.h"
#include "mongo/logv2/log.h"
#include "mongo/platform/decimal128.h"
#include "mongo/rpc/object_check.h"  // IWYU pragma: keep
#include "mongo/stdx/unordered_set.h"
#include "mongo/unittest/unittest.h"

#include <cstdint>
#include <functional>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <absl/container/node_hash_set.h>
#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>

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

        for (std::string_view edgeSd : edges) {
            std::string edge = std::string{edgeSd};
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
#include "mongo/crypto/test_vectors/edges_int32.cstruct.h"  // IWYU pragma: keep
    };
    for (const auto& testVector : testVectors) {
        ASSERT_TRUE(testVector.validate());
    }
}

TEST(EdgeCalcTest, Int64_TestVectors) {
    std::vector<EdgeCalcTestVector<int64_t>> testVectors = {
#include "mongo/crypto/test_vectors/edges_int64.cstruct.h"  // IWYU pragma: keep
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
#include "mongo/crypto/test_vectors/edges_double.cstruct.h"  // IWYU pragma: keep
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
#include "mongo/crypto/test_vectors/edges_decimal128.cstruct.h"  // IWYU pragma: keep
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
#include "mongo/crypto/test_vectors/mincover_int32.cstruct.h"  // IWYU pragma: keep
    };
    for (const auto& testVector : testVectors) {
        ASSERT_TRUE(testVector.validate(minCoverInt32));
    }
}

TEST(MinCoverCalcTest, Int64_TestVectors) {
    const MinCoverTestVector<int64_t> testVectors[] = {
#include "mongo/crypto/test_vectors/mincover_int64.cstruct.h"  // IWYU pragma: keep
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
#include "mongo/crypto/test_vectors/mincover_double.cstruct.h"  // IWYU pragma: keep
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
#include "mongo/crypto/test_vectors/mincover_decimal128.cstruct.h"  // IWYU pragma: keep
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
#include "mongo/crypto/test_vectors/mincover_double_precision.cstruct.h"  // IWYU pragma: keep
    };
    for (const auto& testVector : testVectors) {
        ASSERT_TRUE(testVector.validate(minCoverDouble));
    }
}

TEST(MinCoverCalcPrecisionTest, Decimal128_TestVectors) {
    MinCoverTestVectorPrecision<Decimal128> testVectors[] = {
#include "mongo/crypto/test_vectors/mincover_decimal128_precision.cstruct.h"  // IWYU pragma: keep
    };
    for (const auto& testVector : testVectors) {
        ASSERT_TRUE(testVector.validate(minCoverDecimal128));
    }
}
}  // namespace mongo
