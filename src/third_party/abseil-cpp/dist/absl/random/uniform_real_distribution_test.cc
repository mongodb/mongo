// Copyright 2017 The Abseil Authors.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      https://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "absl/random/uniform_real_distribution.h"

#include <cmath>
#include <cstdint>
#include <iterator>
#include <random>
#include <sstream>
#include <string>
#include <type_traits>
#include <vector>

#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "absl/base/internal/raw_logging.h"
#include "absl/numeric/internal/representation.h"
#include "absl/random/internal/chi_square.h"
#include "absl/random/internal/distribution_test_util.h"
#include "absl/random/internal/pcg_engine.h"
#include "absl/random/internal/sequence_urbg.h"
#include "absl/random/random.h"
#include "absl/strings/str_cat.h"

// NOTES:
// * Some documentation on generating random real values suggests that
//   it is possible to use std::nextafter(b, DBL_MAX) to generate a value on
//   the closed range [a, b]. Unfortunately, that technique is not universally
//   reliable due to floating point quantization.
//
// * absl::uniform_real_distribution<float> generates between 2^28 and 2^29
//   distinct floating point values in the range [0, 1).
//
// * absl::uniform_real_distribution<float> generates at least 2^23 distinct
//   floating point values in the range [1, 2). This should be the same as
//   any other range covered by a single exponent in IEEE 754.
//
// * absl::uniform_real_distribution<double> generates more than 2^52 distinct
//   values in the range [0, 1), and should generate at least 2^52 distinct
//   values in the range of [1, 2).
//

namespace {

template <typename RealType>
class UniformRealDistributionTest : public ::testing::Test {};

// double-double arithmetic is not supported well by either GCC or Clang; see
// https://gcc.gnu.org/bugzilla/show_bug.cgi?id=99048,
// https://bugs.llvm.org/show_bug.cgi?id=49131, and
// https://bugs.llvm.org/show_bug.cgi?id=49132. Don't bother running these tests
// with double doubles until compiler support is better.
using RealTypes =
    std::conditional<absl::numeric_internal::IsDoubleDouble(),
                     ::testing::Types<float, double>,
                     ::testing::Types<float, double, long double>>::type;

TYPED_TEST_SUITE(UniformRealDistributionTest, RealTypes);

TYPED_TEST(UniformRealDistributionTest, ParamSerializeTest) {
  using param_type =
      typename absl::uniform_real_distribution<TypeParam>::param_type;

  constexpr const TypeParam a{1152921504606846976};

  constexpr int kCount = 1000;
  absl::InsecureBitGen gen;
  for (const auto& param : {
           param_type(),
           param_type(TypeParam(2.0), TypeParam(2.0)),  // Same
           param_type(TypeParam(-0.1), TypeParam(0.1)),
           param_type(TypeParam(0.05), TypeParam(0.12)),
           param_type(TypeParam(-0.05), TypeParam(0.13)),
           param_type(TypeParam(-0.05), TypeParam(-0.02)),
           // double range = 0
           // 2^60 , 2^60 + 2^6
           param_type(a, TypeParam(1152921504606847040)),
           // 2^60 , 2^60 + 2^7
           param_type(a, TypeParam(1152921504606847104)),
           // double range = 2^8
           // 2^60 , 2^60 + 2^8
           param_type(a, TypeParam(1152921504606847232)),
           // float range = 0
           // 2^60 , 2^60 + 2^36
           param_type(a, TypeParam(1152921573326323712)),
           // 2^60 , 2^60 + 2^37
           param_type(a, TypeParam(1152921642045800448)),
           // float range = 2^38
           // 2^60 , 2^60 + 2^38
           param_type(a, TypeParam(1152921779484753920)),
           // Limits
           param_type(0, std::numeric_limits<TypeParam>::max()),
           param_type(std::numeric_limits<TypeParam>::lowest(), 0),
           param_type(0, std::numeric_limits<TypeParam>::epsilon()),
           param_type(-std::numeric_limits<TypeParam>::epsilon(),
                      std::numeric_limits<TypeParam>::epsilon()),
           param_type(std::numeric_limits<TypeParam>::epsilon(),
                      2 * std::numeric_limits<TypeParam>::epsilon()),
       }) {
    // Validate parameters.
    const auto a = param.a();
    const auto b = param.b();
    absl::uniform_real_distribution<TypeParam> before(a, b);
    EXPECT_EQ(before.a(), param.a());
    EXPECT_EQ(before.b(), param.b());

    {
      absl::uniform_real_distribution<TypeParam> via_param(param);
      EXPECT_EQ(via_param, before);
    }

    std::stringstream ss;
    ss << before;
    absl::uniform_real_distribution<TypeParam> after(TypeParam(1.0),
                                                     TypeParam(3.1));

    EXPECT_NE(before.a(), after.a());
    EXPECT_NE(before.b(), after.b());
    EXPECT_NE(before.param(), after.param());
    EXPECT_NE(before, after);

    ss >> after;

    EXPECT_EQ(before.a(), after.a());
    EXPECT_EQ(before.b(), after.b());
    EXPECT_EQ(before.param(), after.param());
    EXPECT_EQ(before, after);

    // Smoke test.
    auto sample_min = after.max();
    auto sample_max = after.min();
    for (int i = 0; i < kCount; i++) {
      auto sample = after(gen);
      // Failure here indicates a bug in uniform_real_distribution::operator(),
      // or bad parameters--range too large, etc.
      if (after.min() == after.max()) {
        EXPECT_EQ(sample, after.min());
      } else {
        EXPECT_GE(sample, after.min());
        EXPECT_LT(sample, after.max());
      }
      if (sample > sample_max) {
        sample_max = sample;
      }
      if (sample < sample_min) {
        sample_min = sample;
      }
    }

    if (!std::is_same<TypeParam, long double>::value) {
      // static_cast<double>(long double) can overflow.
      std::string msg = absl::StrCat("Range: ", static_cast<double>(sample_min),
                                     ", ", static_cast<double>(sample_max));
      ABSL_RAW_LOG(INFO, "%s", msg.c_str());
    }
  }
}

#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable:4756)  // Constant arithmetic overflow.
#endif
TYPED_TEST(UniformRealDistributionTest, ViolatesPreconditionsDeathTest) {
#if GTEST_HAS_DEATH_TEST
  // Hi < Lo
  EXPECT_DEBUG_DEATH(
      { absl::uniform_real_distribution<TypeParam> dist(10.0, 1.0); }, "");

  // Hi - Lo > numeric_limits<>::max()
  EXPECT_DEBUG_DEATH(
      {
        absl::uniform_real_distribution<TypeParam> dist(
            std::numeric_limits<TypeParam>::lowest(),
            std::numeric_limits<TypeParam>::max());
      },
      "");
#endif  // GTEST_HAS_DEATH_TEST
#if defined(NDEBUG)
  // opt-mode, for invalid parameters, will generate a garbage value,
  // but should not enter an infinite loop.
  absl::InsecureBitGen gen;
  {
    absl::uniform_real_distribution<TypeParam> dist(10.0, 1.0);
    auto x = dist(gen);
    EXPECT_FALSE(std::isnan(x)) << x;
  }
  {
    absl::uniform_real_distribution<TypeParam> dist(
        std::numeric_limits<TypeParam>::lowest(),
        std::numeric_limits<TypeParam>::max());
    auto x = dist(gen);
    // Infinite result.
    EXPECT_FALSE(std::isfinite(x)) << x;
  }
#endif  // NDEBUG
}
#ifdef _MSC_VER
#pragma warning(pop)  // warning(disable:4756)
#endif

TYPED_TEST(UniformRealDistributionTest, TestMoments) {
  constexpr int kSize = 1000000;
  std::vector<double> values(kSize);

  // We use a fixed bit generator for distribution accuracy tests.  This allows
  // these tests to be deterministic, while still testing the qualify of the
  // implementation.
  absl::random_internal::pcg64_2018_engine rng{0x2B7E151628AED2A6};

  absl::uniform_real_distribution<TypeParam> dist;
  for (int i = 0; i < kSize; i++) {
    values[i] = dist(rng);
  }

  const auto moments =
      absl::random_internal::ComputeDistributionMoments(values);
  EXPECT_NEAR(0.5, moments.mean, 0.01);
  EXPECT_NEAR(1 / 12.0, moments.variance, 0.015);
  EXPECT_NEAR(0.0, moments.skewness, 0.02);
  EXPECT_NEAR(9 / 5.0, moments.kurtosis, 0.015);
}

TYPED_TEST(UniformRealDistributionTest, ChiSquaredTest50) {
  using absl::random_internal::kChiSquared;
  using param_type =
      typename absl::uniform_real_distribution<TypeParam>::param_type;

  constexpr size_t kTrials = 100000;
  constexpr int kBuckets = 50;
  constexpr double kExpected =
      static_cast<double>(kTrials) / static_cast<double>(kBuckets);

  // 1-in-100000 threshold, but remember, there are about 8 tests
  // in this file. And the test could fail for other reasons.
  // Empirically validated with --runs_per_test=10000.
  const int kThreshold =
      absl::random_internal::ChiSquareValue(kBuckets - 1, 0.999999);

  // We use a fixed bit generator for distribution accuracy tests.  This allows
  // these tests to be deterministic, while still testing the qualify of the
  // implementation.
  absl::random_internal::pcg64_2018_engine rng{0x2B7E151628AED2A6};

  for (const auto& param : {param_type(0, 1), param_type(5, 12),
                            param_type(-5, 13), param_type(-5, -2)}) {
    const double min_val = param.a();
    const double max_val = param.b();
    const double factor = kBuckets / (max_val - min_val);

    std::vector<int32_t> counts(kBuckets, 0);
    absl::uniform_real_distribution<TypeParam> dist(param);
    for (size_t i = 0; i < kTrials; i++) {
      auto x = dist(rng);
      auto bucket = static_cast<size_t>((x - min_val) * factor);
      counts[bucket]++;
    }

    double chi_square = absl::random_internal::ChiSquareWithExpected(
        std::begin(counts), std::end(counts), kExpected);
    if (chi_square > kThreshold) {
      double p_value =
          absl::random_internal::ChiSquarePValue(chi_square, kBuckets);

      // Chi-squared test failed. Output does not appear to be uniform.
      std::string msg;
      for (const auto& a : counts) {
        absl::StrAppend(&msg, a, "\n");
      }
      absl::StrAppend(&msg, kChiSquared, " p-value ", p_value, "\n");
      absl::StrAppend(&msg, "High ", kChiSquared, " value: ", chi_square, " > ",
                      kThreshold);
      ABSL_RAW_LOG(INFO, "%s", msg.c_str());
      FAIL() << msg;
    }
  }
}

TYPED_TEST(UniformRealDistributionTest, StabilityTest) {
  // absl::uniform_real_distribution stability relies only on
  // random_internal::RandU64ToDouble and random_internal::RandU64ToFloat.
  absl::random_internal::sequence_urbg urbg(
      {0x0003eb76f6f7f755ull, 0xFFCEA50FDB2F953Bull, 0xC332DDEFBE6C5AA5ull,
       0x6558218568AB9702ull, 0x2AEF7DAD5B6E2F84ull, 0x1521B62829076170ull,
       0xECDD4775619F1510ull, 0x13CCA830EB61BD96ull, 0x0334FE1EAA0363CFull,
       0xB5735C904C70A239ull, 0xD59E9E0BCBAADE14ull, 0xEECC86BC60622CA7ull});

  std::vector<int> output(12);

  absl::uniform_real_distribution<TypeParam> dist;
  std::generate(std::begin(output), std::end(output), [&] {
    return static_cast<int>(TypeParam(1000000) * dist(urbg));
  });

  EXPECT_THAT(
      output,  //
      testing::ElementsAre(59, 999246, 762494, 395876, 167716, 82545, 925251,
                           77341, 12527, 708791, 834451, 932808));
}

TEST(UniformRealDistributionTest, AlgorithmBounds) {
  absl::uniform_real_distribution<double> dist;

  {
    // This returns the smallest value >0 from absl::uniform_real_distribution.
    absl::random_internal::sequence_urbg urbg({0x0000000000000001ull});
    double a = dist(urbg);
    EXPECT_EQ(a, 5.42101086242752217004e-20);
  }

  {
    // This returns a value very near 0.5 from absl::uniform_real_distribution.
    absl::random_internal::sequence_urbg urbg({0x7fffffffffffffefull});
    double a = dist(urbg);
    EXPECT_EQ(a, 0.499999999999999944489);
  }
  {
    // This returns a value very near 0.5 from absl::uniform_real_distribution.
    absl::random_internal::sequence_urbg urbg({0x8000000000000000ull});
    double a = dist(urbg);
    EXPECT_EQ(a, 0.5);
  }

  {
    // This returns the largest value <1 from absl::uniform_real_distribution.
    absl::random_internal::sequence_urbg urbg({0xFFFFFFFFFFFFFFEFull});
    double a = dist(urbg);
    EXPECT_EQ(a, 0.999999999999999888978);
  }
  {
    // This *ALSO* returns the largest value <1.
    absl::random_internal::sequence_urbg urbg({0xFFFFFFFFFFFFFFFFull});
    double a = dist(urbg);
    EXPECT_EQ(a, 0.999999999999999888978);
  }
}

}  // namespace
