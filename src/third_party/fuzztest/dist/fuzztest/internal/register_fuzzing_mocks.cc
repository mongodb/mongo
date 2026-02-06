// Copyright 2025 Google LLC
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "./fuzztest/internal/register_fuzzing_mocks.h"

#include <algorithm>
#include <climits>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <tuple>
#include <type_traits>

#include "absl/base/fast_type_id.h"
#include "absl/functional/function_ref.h"
#include "absl/log/absl_check.h"
#include "absl/random/bernoulli_distribution.h"
#include "absl/random/beta_distribution.h"
#include "absl/random/distributions.h"
#include "absl/random/exponential_distribution.h"
#include "absl/random/gaussian_distribution.h"
#include "absl/random/log_uniform_int_distribution.h"
#include "absl/random/poisson_distribution.h"
#include "absl/random/zipf_distribution.h"
#include "absl/types/span.h"

namespace fuzztest::internal {
namespace {

// Reference type to consume bytes from a data stream; these are used by
// the fuzzing bitgen distribution implementations.
struct DataStreamConsumer {
  // This is a reference to the fuzzing data stream since the mutations
  // (src.remove_prefix(...), etc.) are applied to the source stream.
  absl::Span<const uint8_t>& src;

  // Consumes up to num_bytes from the head of the data stream.
  size_t ConsumeHead(void* destination, size_t num_bytes) {
    num_bytes = std::min(num_bytes, src.size());
    std::memcpy(destination, src.data(), num_bytes);
    src.remove_prefix(num_bytes);
    return num_bytes;
  }

  // Consumes up to num_bytes from the tail of the data stream.
  size_t ConsumeTail(void* destination, size_t num_bytes) {
    num_bytes = std::min(num_bytes, src.size());
    std::memcpy(destination, src.data() + src.size() - num_bytes, num_bytes);
    src.remove_suffix(num_bytes);
    return num_bytes;
  }

  // Consumes a T from the head of the data stream.
  template <typename T>
  T ConsumeHead() {
    std::conditional_t<std::is_same_v<T, bool>, uint8_t, T> x{};
    ConsumeHead(&x, sizeof(x));
    if constexpr (std::is_same_v<T, bool>) {
      return static_cast<bool>(x & 1);
    } else {
      return x;
    }
  }

  // Consumes a T from the tail of the data stream.
  template <typename T>
  T ConsumeTail() {
    std::conditional_t<std::is_same_v<T, bool>, uint8_t, T> x{};
    ConsumeTail(&x, sizeof(x));
    if constexpr (std::is_same_v<T, bool>) {
      return static_cast<bool>(x & 1);
    } else {
      return x;
    }
  }

  // Returns a real value in the range [0.0, 1.0].
  template <typename T>
  T ConsumeProbability() {
    static_assert(std::is_floating_point_v<T> && sizeof(T) <= sizeof(uint64_t),
                  "A floating point type is required.");
    using IntegralType =
        typename std::conditional_t<(sizeof(T) <= sizeof(uint32_t)), uint32_t,
                                    uint64_t>;
    auto int_value = ConsumeTail<IntegralType>();
    return static_cast<T>(int_value) /
           static_cast<T>(std::numeric_limits<IntegralType>::max());
  }

  // Returns a value in the closed-closed range [min, max].
  template <typename T>
  T ConsumeValueInRange(T min, T max) {
    ABSL_CHECK_LE(min, max);

    if (min == max) return min;

    // Return the min or max value more frequently.
    uint8_t byte = ConsumeHead<uint8_t>();
    if (byte == 0) {
      return min;
    } else if (byte == 1) {
      return max;
    }
    byte >>= 1;

    return ConsumeValueInRangeImpl<T>(min, max, byte);
  }

 private:
  // Returns a real value in the range [min, max]
  template <typename T>
  std::enable_if_t<std::is_floating_point_v<T>, T>  //
  ConsumeValueInRangeImpl(T min, T max, uint8_t byte) {
    static_assert(sizeof(T) <= sizeof(uint64_t), "Unsupported float type.");
    // Returns a floating point value in the given range by consuming bytes
    // from the input data. If there's no input data left, returns |min|. Note
    // that |min| must be less than or equal to |max|.
    T range = .0;
    T result = min;
    constexpr T zero(.0);
    if (max > zero && min < zero && max > min + std::numeric_limits<T>::max()) {
      // The diff |max - min| would overflow the given floating point type.
      // Use the half of the diff as the range and consume a bool to decide
      // whether the result is in the first of the second part of the diff.
      range = (max / 2.0) - (min / 2.0);
      if (byte & 1) {
        result += range;
      }
    } else {
      range = max - min;
    }
    return result + range * ConsumeProbability<T>();
  }

  // Returns an integral value in the range [min, max]
  template <typename T>
  std::enable_if_t<std::is_integral_v<T>, T>  //
  ConsumeValueInRangeImpl(T min, T max, uint8_t) {
    static_assert(sizeof(T) <= sizeof(uint64_t), "Unsupported integral type.");

    // Use the biggest type possible to hold the range and the result.
    uint64_t range = static_cast<uint64_t>(max) - static_cast<uint64_t>(min);
    uint64_t result = 0;
    size_t offset = 0;
    while (offset < sizeof(T) * CHAR_BIT && (range >> offset) > 0 &&
           !src.empty()) {
      uint8_t byte = src.back();
      src.remove_suffix(1);
      result = (result << CHAR_BIT) | byte;
      offset += CHAR_BIT;
    }

    // Avoid division by 0, in case |range + 1| results in overflow.
    if (range != std::numeric_limits<decltype(range)>::max()) {
      result = result % (range + 1);
    }

    return static_cast<T>(static_cast<uint64_t>(min) + result);
  }
};

// -----------------------------------------------------------------------------

// Bernoulli
struct ImplBernoulli : public DataStreamConsumer {
  using DistrT = absl::bernoulli_distribution;
  using ArgTupleT = std::tuple<double>;
  using ResultT = bool;

  ResultT operator()(double p) {
    // Just generate a boolean; mostly ignoring p.
    // The 0/1 cases are special cased to avoid returning false on constants.
    if (p == 0.0) {
      return false;
    } else if (p == 1.0) {
      return true;
    } else {
      return ConsumeHead<bool>();
    }
  }
};

// Beta
template <typename RealType>
struct ImplBeta : public DataStreamConsumer {
  using DistrT = absl::beta_distribution<RealType>;
  using ArgTupleT = std::tuple<RealType, RealType>;
  using ResultT = RealType;

  ResultT operator()(RealType a, RealType b) {
    if (!src.empty()) {
      auto x = ConsumeTail<RealType>();
      if (std::isfinite(x)) {
        return x;
      }
    }
    return a / (a + b);  // mean
  }
};

// Exponential
template <typename RealType>
struct ImplExponential : public DataStreamConsumer {
  using DistrT = absl::exponential_distribution<RealType>;
  using ArgTupleT = std::tuple<RealType>;
  using ResultT = RealType;

  ResultT operator()(RealType lambda) {
    if (!src.empty()) {
      auto x = ConsumeTail<RealType>();
      if (std::isfinite(x)) {
        return x;
      }
    }
    return RealType{1} / lambda;  // mean
  }
};

// Gaussian
template <typename RealType>
struct ImplGaussian : public DataStreamConsumer {
  using DistrT = absl::gaussian_distribution<RealType>;
  using ArgTupleT = std::tuple<RealType, RealType>;
  using ResultT = RealType;

  ResultT operator()(RealType mean, RealType sigma) {
    if (src.empty()) return mean;
    const auto ten_sigma = sigma * 10;
    RealType min = mean - ten_sigma;
    RealType max = mean + ten_sigma;
    return ConsumeValueInRange<RealType>(min, max);
  }
};

// LogUniform
template <typename IntType>
struct ImplLogUniform : public DataStreamConsumer {
  using DistrT = absl::log_uniform_int_distribution<IntType>;
  using ArgTupleT = std::tuple<IntType, IntType, IntType>;
  using ResultT = IntType;

  ResultT operator()(IntType a, IntType b, IntType) {
    if (src.empty()) return a;
    return ConsumeValueInRange<IntType>(a, b);
  }
};

// Poisson
template <typename IntType>
struct ImplPoisson : public DataStreamConsumer {
  using DistrT = absl::poisson_distribution<IntType>;
  using ArgTupleT = std::tuple<double>;
  using ResultT = IntType;

  ResultT operator()(double) {
    if (src.empty()) return 0;
    return ConsumeValueInRange<IntType>(0, std::numeric_limits<IntType>::max());
  }
};

// Zipf
template <typename IntType>
struct ImplZipf : public DataStreamConsumer {
  using DistrT = absl::zipf_distribution<IntType>;
  using ArgTupleT = std::tuple<IntType, double, double>;
  using ResultT = IntType;

  ResultT operator()(IntType a, double, double) {
    if (src.empty()) return 0;
    return ConsumeValueInRange<IntType>(0, a);
  }
};

// Uniform
template <typename R>
struct ImplUniform : public DataStreamConsumer {
  using DistrT = absl::random_internal::UniformDistributionWrapper<R>;
  using ResultT = R;

  ResultT operator()(absl::IntervalClosedClosedTag, R min, R max) {
    if (src.empty()) return min;
    return ConsumeValueInRange<R>(min, max);
  }

  ResultT operator()(absl::IntervalClosedOpenTag, R min, R max) {
    if (src.empty()) return min;
    if constexpr (std::is_floating_point_v<R>) {
      max = std::nexttoward(max, min);
      return ConsumeValueInRange<R>(min, max);
    } else {
      max--;
      return ConsumeValueInRange<R>(min, max);
    }
  }

  ResultT operator()(absl::IntervalOpenOpenTag, R min, R max) {
    if (src.empty()) return min;
    if constexpr (std::is_floating_point_v<R>) {
      min = std::nexttoward(min, max);
      max = std::nexttoward(max, min);
      return ConsumeValueInRange<R>(min, max);
    } else {
      min++;
      max--;
      return ConsumeValueInRange<R>(min, max);
    }
  }

  ResultT operator()(absl::IntervalOpenClosedTag, R min, R max) {
    if (src.empty()) return min;
    if constexpr (std::is_floating_point_v<R>) {
      min = std::nexttoward(min, max);
      return ConsumeValueInRange<R>(min, max);
    } else {
      min++;
      return ConsumeValueInRange<R>(min, max);
    }
  }

  ResultT operator()(R min, R max) {
    return operator()(absl::IntervalClosedOpen, min, max);
  }

  ResultT operator()() {
    static_assert(std::is_unsigned_v<R>);
    if (src.empty()) return 0;
    return ConsumeTail<R>();
  }
};

// -----------------------------------------------------------------------------

// InvokeFuzzFunction is a type-erased function pointer which is responsible for
// casting the args_tuple and result parameters to the correct types and then
// invoking the implementation functor. It is important that the ArgsTupleT and
// ResultT types match the types of the distribution and the implementation
// functions, so the HandleFuzzedFunction overloads are used to determine the
// correct types.
template <typename FuzzFunctionT, typename ResultT, typename ArgTupleT>
void InvokeFuzzFunction(absl::Span<const uint8_t>& src, void* args_tuple,
                        void* result) {
  FuzzFunctionT fn{src};
  *static_cast<ResultT*>(result) =
      absl::apply(fn, *static_cast<ArgTupleT*>(args_tuple));
}

template <typename FuzzFunctionT>
void HandleFuzzedFunctionX(
    absl::FunctionRef<void(absl::FastTypeIdType, TypeErasedFuzzFunctionT)>
        register_fn) {
  using DistrT = typename FuzzFunctionT::DistrT;
  using ArgTupleT = typename FuzzFunctionT::ArgTupleT;
  using ResultT = typename FuzzFunctionT::ResultT;
  using KeyT = ResultT(DistrT, ArgTupleT);

  register_fn(absl::FastTypeId<KeyT>(),
              &InvokeFuzzFunction<FuzzFunctionT, ResultT, ArgTupleT>);
}

template <typename FuzzFunctionT, typename... Args>
void HandleFuzzedFunctionU(
    absl::FunctionRef<void(absl::FastTypeIdType, TypeErasedFuzzFunctionT)>
        register_fn) {
  using DistrT = typename FuzzFunctionT::DistrT;
  using ArgTupleT = std::tuple<std::decay_t<Args>...>;
  using ResultT = typename FuzzFunctionT::ResultT;
  using KeyT = ResultT(DistrT, ArgTupleT);

  register_fn(absl::FastTypeId<KeyT>(),
              &InvokeFuzzFunction<FuzzFunctionT, ResultT, ArgTupleT>);
}

// -----------------------------------------------------------------------------
// X_ macros to invoke X_IMPL_T macros for each type.
// -----------------------------------------------------------------------------

#define X_SINT(Impl)                                  \
  if constexpr (std::is_signed_v<char> &&             \
                !std::is_same_v<char, signed char>) { \
    X_IMPL_T(char, Impl);                             \
  }                                                   \
  X_IMPL_T(signed char, Impl);                        \
  X_IMPL_T(short, Impl);     /*NOLINT*/               \
  X_IMPL_T(long, Impl);      /*NOLINT*/               \
  X_IMPL_T(long long, Impl); /*NOLINT*/               \
  X_IMPL_T(int, Impl)

#define X_UINT(Impl)                                    \
  if constexpr (std::is_unsigned_v<char> &&             \
                !std::is_same_v<char, unsigned char>) { \
    X_IMPL_T(char, Impl);                               \
  }                                                     \
  X_IMPL_T(unsigned char, Impl);                        \
  X_IMPL_T(unsigned short, Impl);     /*NOLINT*/        \
  X_IMPL_T(unsigned long, Impl);      /*NOLINT*/        \
  X_IMPL_T(unsigned long long, Impl); /*NOLINT*/        \
  X_IMPL_T(unsigned int, Impl)

#define X_REAL(Impl)     \
  X_IMPL_T(float, Impl); \
  X_IMPL_T(double, Impl)

#define X_XINT(Impl) \
  X_SINT(Impl);      \
  X_UINT(Impl)

#define X_ALL(Impl) \
  X_SINT(Impl);     \
  X_UINT(Impl);     \
  X_REAL(Impl)

}  // namespace

// Registers the fuzzing functions into the fuzztest mock map.
void RegisterAbslRandomFuzzingMocks(
    absl::FunctionRef<void(absl::FastTypeIdType, TypeErasedFuzzFunctionT)>
        register_fn) {
#define X_IMPL_T(T, Impl) HandleFuzzedFunctionX<Impl<T>>(register_fn)

  HandleFuzzedFunctionX<ImplBernoulli>(register_fn);

  X_REAL(ImplBeta);
  X_REAL(ImplExponential);
  X_REAL(ImplGaussian);
  X_XINT(ImplLogUniform);
  X_XINT(ImplPoisson);
  X_XINT(ImplZipf);

#undef X_IMPL_T
#define X_IMPL_T(T, Impl)                                              \
  HandleFuzzedFunctionU<Impl<T>, absl::IntervalOpenOpenTag, T, T>(     \
      register_fn);                                                    \
  HandleFuzzedFunctionU<Impl<T>, absl::IntervalOpenClosedTag, T, T>(   \
      register_fn);                                                    \
  HandleFuzzedFunctionU<Impl<T>, absl::IntervalClosedOpenTag, T, T>(   \
      register_fn);                                                    \
  HandleFuzzedFunctionU<Impl<T>, absl::IntervalClosedClosedTag, T, T>( \
      register_fn);                                                    \
  HandleFuzzedFunctionU<Impl<T>, T, T>(register_fn)

  X_ALL(ImplUniform);

#undef X_IMPL_T
#define X_IMPL_T(T, Impl) HandleFuzzedFunctionU<Impl<T>>(register_fn)

  X_UINT(ImplUniform);

#undef X_IMPL_T
}

}  // namespace fuzztest::internal
