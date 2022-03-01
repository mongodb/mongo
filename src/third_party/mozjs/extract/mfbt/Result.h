/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/* A type suitable for returning either a value or an error from a function. */

#ifndef mozilla_Result_h
#define mozilla_Result_h

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <type_traits>
#include "mozilla/Assertions.h"
#include "mozilla/Attributes.h"
#include "mozilla/CompactPair.h"
#include "mozilla/MaybeStorageBase.h"

namespace mozilla {

/**
 * Empty struct, indicating success for operations that have no return value.
 * For example, if you declare another empty struct `struct OutOfMemory {};`,
 * then `Result<Ok, OutOfMemory>` represents either success or OOM.
 */
struct Ok {};

/**
 * A tag used to differentiate between GenericErrorResult created by the Err
 * function (completely new error) and GenericErrorResult created by the
 * Result::propagateErr function (propagated error). This can be used to track
 * error propagation and eventually produce error stacks for logging/debugging
 * purposes.
 */
struct ErrorPropagationTag {};

template <typename E>
class GenericErrorResult;
template <typename V, typename E>
class Result;

namespace detail {

enum class PackingStrategy {
  Variant,
  NullIsOk,
  LowBitTagIsError,
  PackedVariant,
};

template <typename T>
struct UnusedZero;

template <typename V, typename E, PackingStrategy Strategy>
class ResultImplementation;

template <typename V>
struct EmptyWrapper : V {
  constexpr EmptyWrapper() = default;
  explicit constexpr EmptyWrapper(const V&) {}
  explicit constexpr EmptyWrapper(std::in_place_t) {}

  constexpr V* addr() { return this; }
  constexpr const V* addr() const { return this; }
};

// The purpose of AlignedStorageOrEmpty is to make an empty class look like
// std::aligned_storage_t for the purposes of the PackingStrategy::NullIsOk
// specializations of ResultImplementation below. We can't use
// std::aligned_storage_t itself with an empty class, since it would no longer
// be empty.
template <typename V>
using AlignedStorageOrEmpty =
    std::conditional_t<std::is_empty_v<V>, EmptyWrapper<V>,
                       MaybeStorageBase<V>>;

template <typename V, typename E>
class ResultImplementationNullIsOkBase {
 protected:
  using ErrorStorageType = typename UnusedZero<E>::StorageType;

  static constexpr auto kNullValue = UnusedZero<E>::nullValue;

  static_assert(std::is_trivially_copyable_v<ErrorStorageType>);

  // XXX This can't be statically asserted in general, if ErrorStorageType is
  // not a basic type. With C++20 bit_cast, we could probably re-add such as
  // assertion. static_assert(kNullValue == decltype(kNullValue)(0));

  CompactPair<AlignedStorageOrEmpty<V>, ErrorStorageType> mValue;

 public:
  explicit constexpr ResultImplementationNullIsOkBase(const V& aSuccessValue)
      : mValue(aSuccessValue, kNullValue) {}
  explicit constexpr ResultImplementationNullIsOkBase(V&& aSuccessValue)
      : mValue(std::move(aSuccessValue), kNullValue) {}
  template <typename... Args>
  explicit constexpr ResultImplementationNullIsOkBase(std::in_place_t,
                                                      Args&&... aArgs)
      : mValue(std::piecewise_construct,
               std::tuple(std::in_place, std::forward<Args>(aArgs)...),
               std::tuple(kNullValue)) {}
  explicit constexpr ResultImplementationNullIsOkBase(E aErrorValue)
      : mValue(std::piecewise_construct, std::tuple<>(),
               std::tuple(UnusedZero<E>::Store(std::move(aErrorValue)))) {
    MOZ_ASSERT(mValue.second() != kNullValue);
  }

  constexpr ResultImplementationNullIsOkBase(
      ResultImplementationNullIsOkBase&& aOther)
      : mValue(std::piecewise_construct, std::tuple<>(),
               std::tuple(aOther.mValue.second())) {
    if constexpr (!std::is_empty_v<V>) {
      if (isOk()) {
        new (mValue.first().addr()) V(std::move(*aOther.mValue.first().addr()));
      }
    }
  }
  ResultImplementationNullIsOkBase& operator=(
      ResultImplementationNullIsOkBase&& aOther) {
    if constexpr (!std::is_empty_v<V>) {
      if (isOk()) {
        mValue.first().addr()->~V();
      }
    }
    mValue.second() = std::move(aOther.mValue.second());
    if constexpr (!std::is_empty_v<V>) {
      if (isOk()) {
        new (mValue.first().addr()) V(std::move(*aOther.mValue.first().addr()));
      }
    }
    return *this;
  }

  constexpr bool isOk() const { return mValue.second() == kNullValue; }

  constexpr const V& inspect() const { return *mValue.first().addr(); }
  constexpr V unwrap() { return std::move(*mValue.first().addr()); }

  constexpr decltype(auto) inspectErr() const {
    return UnusedZero<E>::Inspect(mValue.second());
  }
  constexpr E unwrapErr() { return UnusedZero<E>::Unwrap(mValue.second()); }
};

template <typename V, typename E,
          bool IsVTriviallyDestructible = std::is_trivially_destructible_v<V>>
class ResultImplementationNullIsOk;

template <typename V, typename E>
class ResultImplementationNullIsOk<V, E, true>
    : public ResultImplementationNullIsOkBase<V, E> {
 public:
  using ResultImplementationNullIsOkBase<V,
                                         E>::ResultImplementationNullIsOkBase;
};

template <typename V, typename E>
class ResultImplementationNullIsOk<V, E, false>
    : public ResultImplementationNullIsOkBase<V, E> {
 public:
  using ResultImplementationNullIsOkBase<V,
                                         E>::ResultImplementationNullIsOkBase;

  ResultImplementationNullIsOk(ResultImplementationNullIsOk&&) = default;
  ResultImplementationNullIsOk& operator=(ResultImplementationNullIsOk&&) =
      default;

  ~ResultImplementationNullIsOk() {
    if (this->isOk()) {
      this->mValue.first().addr()->~V();
    }
  }
};

/**
 * Specialization for when the success type is default-constructible and the
 * error type is a value type which can never have the value 0 (as determined by
 * UnusedZero<>).
 */
template <typename V, typename E>
class ResultImplementation<V, E, PackingStrategy::NullIsOk>
    : public ResultImplementationNullIsOk<V, E> {
 public:
  using ResultImplementationNullIsOk<V, E>::ResultImplementationNullIsOk;
};

template <size_t S>
using UnsignedIntType = std::conditional_t<
    S == 1, std::uint8_t,
    std::conditional_t<
        S == 2, std::uint16_t,
        std::conditional_t<S == 3 || S == 4, std::uint32_t,
                           std::conditional_t<S <= 8, std::uint64_t, void>>>>;

/**
 * Specialization for when alignment permits using the least significant bit
 * as a tag bit.
 */
template <typename V, typename E>
class ResultImplementation<V, E, PackingStrategy::LowBitTagIsError> {
  static_assert(std::is_trivially_copyable_v<V> &&
                std::is_trivially_destructible_v<V>);
  static_assert(std::is_trivially_copyable_v<E> &&
                std::is_trivially_destructible_v<E>);

  static constexpr size_t kRequiredSize = std::max(sizeof(V), sizeof(E));

  using StorageType = UnsignedIntType<kRequiredSize>;

#if defined(__clang__)
  alignas(std::max(alignof(V), alignof(E))) StorageType mBits;
#else
  // Some gcc versions choke on using std::max with alignas, see
  // https://gcc.gnu.org/bugzilla/show_bug.cgi?id=94929 (and this seems to have
  // regressed in some gcc 9.x version before being fixed again) Keeping the
  // code above since we would eventually drop this when we no longer support
  // gcc versions with the bug.
  alignas(alignof(V) > alignof(E) ? alignof(V) : alignof(E)) StorageType mBits;
#endif

 public:
  explicit constexpr ResultImplementation(V aValue) : mBits(0) {
    if constexpr (!std::is_empty_v<V>) {
      std::memcpy(&mBits, &aValue, sizeof(V));
      MOZ_ASSERT((mBits & 1) == 0);
    } else {
      (void)aValue;
    }
  }
  explicit constexpr ResultImplementation(E aErrorValue) : mBits(1) {
    if constexpr (!std::is_empty_v<E>) {
      std::memcpy(&mBits, &aErrorValue, sizeof(E));
      MOZ_ASSERT((mBits & 1) == 0);
      mBits |= 1;
    } else {
      (void)aErrorValue;
    }
  }

  constexpr bool isOk() const { return (mBits & 1) == 0; }

  constexpr V inspect() const {
    V res;
    std::memcpy(&res, &mBits, sizeof(V));
    return res;
  }
  constexpr V unwrap() { return inspect(); }

  constexpr E inspectErr() const {
    const auto bits = mBits ^ 1;
    E res;
    std::memcpy(&res, &bits, sizeof(E));
    return res;
  }
  constexpr E unwrapErr() { return inspectErr(); }
};

// Return true if any of the struct can fit in a word.
template <typename V, typename E>
struct IsPackableVariant {
  struct VEbool {
    V v;
    E e;
    bool ok;
  };
  struct EVbool {
    E e;
    V v;
    bool ok;
  };

  using Impl =
      std::conditional_t<sizeof(VEbool) <= sizeof(EVbool), VEbool, EVbool>;

  static const bool value = sizeof(Impl) <= sizeof(uintptr_t);
};

/**
 * Specialization for when both type are not using all the bytes, in order to
 * use one byte as a tag.
 */
template <typename V, typename E>
class ResultImplementation<V, E, PackingStrategy::PackedVariant> {
  using Impl = typename IsPackableVariant<V, E>::Impl;
  Impl data;

 public:
  explicit constexpr ResultImplementation(V aValue) {
    data.v = std::move(aValue);
    data.ok = true;
  }
  explicit constexpr ResultImplementation(E aErrorValue) {
    data.e = std::move(aErrorValue);
    data.ok = false;
  }

  constexpr bool isOk() const { return data.ok; }

  constexpr const V& inspect() const { return data.v; }
  constexpr V unwrap() { return std::move(data.v); }

  constexpr const E& inspectErr() const { return data.e; }
  constexpr E unwrapErr() { return std::move(data.e); }
};

// To use nullptr as a special value, we need the counter part to exclude zero
// from its range of valid representations.
//
// By default assume that zero can be represented.
template <typename T>
struct UnusedZero {
  static const bool value = false;
};

// This template can be used as a helper for specializing UnusedZero for scoped
// enum types which never use 0 as an error value, e.g.
//
// namespace mozilla::detail {
//
// template <>
// struct UnusedZero<MyEnumType> : UnusedZeroEnum<MyEnumType> {};
//
// }  // namespace mozilla::detail
//
template <typename T>
struct UnusedZeroEnum {
  using StorageType = std::underlying_type_t<T>;

  static constexpr bool value = true;
  static constexpr StorageType nullValue = 0;

  static constexpr T Inspect(const StorageType& aValue) {
    return static_cast<T>(aValue);
  }
  static constexpr T Unwrap(StorageType aValue) {
    return static_cast<T>(aValue);
  }
  static constexpr StorageType Store(T aValue) {
    return static_cast<StorageType>(aValue);
  }
};

// A bit of help figuring out which of the above specializations to use.
//
// We begin by safely assuming types don't have a spare bit, unless they are
// empty.
template <typename T>
struct HasFreeLSB {
  static const bool value = std::is_empty_v<T>;
};

// As an incomplete type, void* does not have a spare bit.
template <>
struct HasFreeLSB<void*> {
  static const bool value = false;
};

// The lowest bit of a properly-aligned pointer is always zero if the pointee
// type is greater than byte-aligned. That bit is free to use if it's masked
// out of such pointers before they're dereferenced.
template <typename T>
struct HasFreeLSB<T*> {
  static const bool value = (alignof(T) & 1) == 0;
};

// Select one of the previous result implementation based on the properties of
// the V and E types.
template <typename V, typename E>
struct SelectResultImpl {
  static const PackingStrategy value =
      (HasFreeLSB<V>::value && HasFreeLSB<E>::value)
          ? PackingStrategy::LowBitTagIsError
      : (UnusedZero<E>::value && sizeof(E) <= sizeof(uintptr_t))
          ? PackingStrategy::NullIsOk
      : (std::is_default_constructible_v<V> &&
         std::is_default_constructible_v<E> && IsPackableVariant<V, E>::value)
          ? PackingStrategy::PackedVariant
          : PackingStrategy::Variant;

  using Type = ResultImplementation<V, E, value>;
};

template <typename T>
struct IsResult : std::false_type {};

template <typename V, typename E>
struct IsResult<Result<V, E>> : std::true_type {};

}  // namespace detail

template <typename V, typename E>
constexpr auto ToResult(Result<V, E>&& aValue)
    -> decltype(std::forward<Result<V, E>>(aValue)) {
  return std::forward<Result<V, E>>(aValue);
}

/**
 * Result<V, E> represents the outcome of an operation that can either succeed
 * or fail. It contains either a success value of type V or an error value of
 * type E.
 *
 * All Result methods are const, so results are basically immutable.
 * This is just like Variant<V, E> but with a slightly different API, and the
 * following cases are optimized so Result can be stored more efficiently:
 *
 * - If both the success and error types do not use their least significant bit,
 * are trivially copyable and destructible, Result<V, E> is guaranteed to be as
 * large as the larger type. This is determined via the HasFreeLSB trait. By
 * default, empty classes (in particular Ok) and aligned pointer types are
 * assumed to have a free LSB, but you can specialize this trait for other
 * types. If the success type is empty, the representation is guaranteed to be
 * all zero bits on success. Do not change this representation! There is JIT
 * code that depends on it. (Implementation note: The lowest bit is used as a
 * tag bit: 0 to indicate the Result's bits are a success value, 1 to indicate
 * the Result's bits (with the 1 masked out) encode an error value)
 *
 * - Else, if the error type can't have a all-zero bits representation and is
 * not larger than a pointer, a CompactPair is used to represent this rather
 * than a Variant. This has shown to be better optimizable, and the template
 * code is much simpler than that of Variant, so it should also compile faster.
 * Whether an error type can't be all-zero bits, is determined via the
 * UnusedZero trait. MFBT doesn't declare any public type UnusedZero, but
 * nsresult is declared UnusedZero in XPCOM.
 *
 * The purpose of Result is to reduce the screwups caused by using `false` or
 * `nullptr` to indicate errors.
 * What screwups? See <https://bugzilla.mozilla.org/show_bug.cgi?id=912928> for
 * a partial list.
 *
 * Result<const V, E> or Result<V, const E> are not meaningful. The success or
 * error values in a Result instance are non-modifiable in-place anyway. This
 * guarantee must also be maintained when evolving Result. They can be
 * unwrap()ped, but this loses const qualification. However, Result<const V, E>
 * or Result<V, const E> may be misleading and prevent movability. Just use
 * Result<V, E>. (Result<const V*, E> may make sense though, just Result<const
 * V* const, E> is not possible.)
 */
template <typename V, typename E>
class MOZ_MUST_USE_TYPE Result final {
  // See class comment on Result<const V, E> and Result<V, const E>.
  static_assert(!std::is_const_v<V>);
  static_assert(!std::is_const_v<E>);
  static_assert(!std::is_reference_v<V>);
  static_assert(!std::is_reference_v<E>);

  using Impl = typename detail::SelectResultImpl<V, E>::Type;

  Impl mImpl;

 public:
  using ok_type = V;
  using err_type = E;

  /** Create a success result. */
  MOZ_IMPLICIT constexpr Result(V&& aValue) : mImpl(std::forward<V>(aValue)) {
    MOZ_ASSERT(isOk());
  }

  /** Create a success result. */
  MOZ_IMPLICIT constexpr Result(const V& aValue) : mImpl(aValue) {
    MOZ_ASSERT(isOk());
  }

  /** Create a success result in-place. */
  template <typename... Args>
  explicit constexpr Result(std::in_place_t, Args&&... aArgs)
      : mImpl(std::in_place, std::forward<Args>(aArgs)...) {
    MOZ_ASSERT(isOk());
  }

  /** Create an error result. */
  explicit constexpr Result(E aErrorValue) : mImpl(std::move(aErrorValue)) {
    MOZ_ASSERT(isErr());
  }

  /**
   * Create a (success/error) result from another (success/error) result with a
   * different but convertible error type. */
  template <typename E2,
            typename = std::enable_if_t<std::is_convertible_v<E2, E>>>
  MOZ_IMPLICIT constexpr Result(Result<V, E2>&& aOther)
      : mImpl(aOther.isOk() ? Impl{aOther.unwrap()}
                            : Impl{aOther.unwrapErr()}) {}

  /**
   * Implementation detail of MOZ_TRY().
   * Create an error result from another error result.
   */
  template <typename E2>
  MOZ_IMPLICIT constexpr Result(GenericErrorResult<E2>&& aErrorResult)
      : mImpl(std::move(aErrorResult.mErrorValue)) {
    static_assert(std::is_convertible_v<E2, E>, "E2 must be convertible to E");
    MOZ_ASSERT(isErr());
  }

  /**
   * Implementation detail of MOZ_TRY().
   * Create an error result from another error result.
   */
  template <typename E2>
  MOZ_IMPLICIT constexpr Result(const GenericErrorResult<E2>& aErrorResult)
      : mImpl(aErrorResult.mErrorValue) {
    static_assert(std::is_convertible_v<E2, E>, "E2 must be convertible to E");
    MOZ_ASSERT(isErr());
  }

  Result(const Result&) = delete;
  Result(Result&&) = default;
  Result& operator=(const Result&) = delete;
  Result& operator=(Result&&) = default;

  /** True if this Result is a success result. */
  constexpr bool isOk() const { return mImpl.isOk(); }

  /** True if this Result is an error result. */
  constexpr bool isErr() const { return !mImpl.isOk(); }

  /** Take the success value from this Result, which must be a success result.
   */
  constexpr V unwrap() {
    MOZ_ASSERT(isOk());
    return mImpl.unwrap();
  }

  /**
   * Take the success value from this Result, which must be a success result.
   * If it is an error result, then return the aValue.
   */
  constexpr V unwrapOr(V aValue) {
    return MOZ_LIKELY(isOk()) ? mImpl.unwrap() : std::move(aValue);
  }

  /** Take the error value from this Result, which must be an error result. */
  constexpr E unwrapErr() {
    MOZ_ASSERT(isErr());
    return mImpl.unwrapErr();
  }

  /** See the success value from this Result, which must be a success result. */
  constexpr decltype(auto) inspect() const {
    static_assert(!std::is_reference_v<
                      std::invoke_result_t<decltype(&Impl::inspect), Impl>> ||
                  std::is_const_v<std::remove_reference_t<
                      std::invoke_result_t<decltype(&Impl::inspect), Impl>>>);
    MOZ_ASSERT(isOk());
    return mImpl.inspect();
  }

  /** See the error value from this Result, which must be an error result. */
  constexpr decltype(auto) inspectErr() const {
    static_assert(
        !std::is_reference_v<
            std::invoke_result_t<decltype(&Impl::inspectErr), Impl>> ||
        std::is_const_v<std::remove_reference_t<
            std::invoke_result_t<decltype(&Impl::inspectErr), Impl>>>);
    MOZ_ASSERT(isErr());
    return mImpl.inspectErr();
  }

  /** Propagate the error value from this Result, which must be an error result.
   *
   * This can be used to propagate an error from a function call to the caller
   * with a different value type, but the same error type:
   *
   *    Result<T1, E> Func1() {
   *       Result<T2, E> res = Func2();
   *       if (res.isErr()) { return res.propagateErr(); }
   *    }
   */
  constexpr GenericErrorResult<E> propagateErr() {
    MOZ_ASSERT(isErr());
    return GenericErrorResult<E>{mImpl.unwrapErr(), ErrorPropagationTag{}};
  }

  /**
   * Map a function V -> V2 over this result's success variant. If this result
   * is an error, do not invoke the function and propagate the error.
   *
   * Mapping over success values invokes the function to produce a new success
   * value:
   *
   *     // Map Result<int, E> to another Result<int, E>
   *     Result<int, E> res(5);
   *     Result<int, E> res2 = res.map([](int x) { return x * x; });
   *     MOZ_ASSERT(res.isOk());
   *     MOZ_ASSERT(res2.unwrap() == 25);
   *
   *     // Map Result<const char*, E> to Result<size_t, E>
   *     Result<const char*, E> res("hello, map!");
   *     Result<size_t, E> res2 = res.map(strlen);
   *     MOZ_ASSERT(res.isOk());
   *     MOZ_ASSERT(res2.unwrap() == 11);
   *
   * Mapping over an error does not invoke the function and propagates the
   * error:
   *
   *     Result<V, int> res(5);
   *     MOZ_ASSERT(res.isErr());
   *     Result<V2, int> res2 = res.map([](V v) { ... });
   *     MOZ_ASSERT(res2.isErr());
   *     MOZ_ASSERT(res2.unwrapErr() == 5);
   */
  template <typename F>
  constexpr auto map(F f) -> Result<std::invoke_result_t<F, V>, E> {
    using RetResult = Result<std::invoke_result_t<F, V>, E>;
    return MOZ_LIKELY(isOk()) ? RetResult(f(unwrap())) : RetResult(unwrapErr());
  }

  /**
   * Map a function E -> E2 over this result's error variant. If this result is
   * a success, do not invoke the function and move the success over.
   *
   * Mapping over error values invokes the function to produce a new error
   * value:
   *
   *     // Map Result<V, int> to another Result<V, int>
   *     Result<V, int> res(5);
   *     Result<V, int> res2 = res.mapErr([](int x) { return x * x; });
   *     MOZ_ASSERT(res2.isErr());
   *     MOZ_ASSERT(res2.unwrapErr() == 25);
   *
   *     // Map Result<V, const char*> to Result<V, size_t>
   *     Result<V, const char*> res("hello, mapErr!");
   *     Result<V, size_t> res2 = res.mapErr(strlen);
   *     MOZ_ASSERT(res2.isErr());
   *     MOZ_ASSERT(res2.unwrapErr() == 14);
   *
   * Mapping over a success does not invoke the function and moves the success:
   *
   *     Result<int, E> res(5);
   *     MOZ_ASSERT(res.isOk());
   *     Result<int, E2> res2 = res.mapErr([](E e) { ... });
   *     MOZ_ASSERT(res2.isOk());
   *     MOZ_ASSERT(res2.unwrap() == 5);
   */
  template <typename F>
  constexpr auto mapErr(F f) {
    using RetResult = Result<V, std::invoke_result_t<F, E>>;
    return MOZ_UNLIKELY(isErr()) ? RetResult(f(unwrapErr()))
                                 : RetResult(unwrap());
  }

  /**
   * Map a function E -> Result<V, E2> over this result's error variant. If
   * this result is a success, do not invoke the function and move the success
   * over.
   *
   * `orElse`ing over error values invokes the function to produce a new
   * result:
   *
   *     // `orElse` Result<V, int> error variant to another Result<V, int>
   *     // error variant or Result<V, int> success variant
   *     auto orElse = [](int x) -> Result<V, int> {
   *       if (x != 6) {
   *         return Err(x * x);
   *       }
   *       return V(...);
   *     };
   *
   *     Result<V, int> res(5);
   *     auto res2 = res.orElse(orElse);
   *     MOZ_ASSERT(res2.isErr());
   *     MOZ_ASSERT(res2.unwrapErr() == 25);
   *
   *     Result<V, int> res3(6);
   *     auto res4 = res3.orElse(orElse);
   *     MOZ_ASSERT(res4.isOk());
   *     MOZ_ASSERT(res4.unwrap() == ...);
   *
   *     // `orElse` Result<V, const char*> error variant to Result<V, size_t>
   *     // error variant or Result<V, size_t> success variant
   *     auto orElse = [](const char* s) -> Result<V, size_t> {
   *       if (strcmp(s, "foo")) {
   *         return Err(strlen(s));
   *       }
   *       return V(...);
   *     };
   *
   *     Result<V, const char*> res("hello, orElse!");
   *     auto res2 = res.orElse(orElse);
   *     MOZ_ASSERT(res2.isErr());
   *     MOZ_ASSERT(res2.unwrapErr() == 14);
   *
   *     Result<V, const char*> res3("foo");
   *     auto res4 = ress.orElse(orElse);
   *     MOZ_ASSERT(res4.isOk());
   *     MOZ_ASSERT(res4.unwrap() == ...);
   *
   * `orElse`ing over a success does not invoke the function and moves the
   * success:
   *
   *     Result<int, E> res(5);
   *     MOZ_ASSERT(res.isOk());
   *     Result<int, E2> res2 = res.orElse([](E e) { ... });
   *     MOZ_ASSERT(res2.isOk());
   *     MOZ_ASSERT(res2.unwrap() == 5);
   */
  template <typename F>
  auto orElse(F f) -> Result<V, typename std::invoke_result_t<F, E>::err_type> {
    return MOZ_UNLIKELY(isErr()) ? f(unwrapErr()) : unwrap();
  }

  /**
   * Given a function V -> Result<V2, E>, apply it to this result's success
   * value and return its result. If this result is an error value, it is
   * propagated.
   *
   * This is sometimes called "flatMap" or ">>=" in other contexts.
   *
   * `andThen`ing over success values invokes the function to produce a new
   * result:
   *
   *     Result<const char*, Error> res("hello, andThen!");
   *     Result<HtmlFreeString, Error> res2 = res.andThen([](const char* s) {
   *       return containsHtmlTag(s)
   *         ? Result<HtmlFreeString, Error>(Error("Invalid: contains HTML"))
   *         : Result<HtmlFreeString, Error>(HtmlFreeString(s));
   *       }
   *     });
   *     MOZ_ASSERT(res2.isOk());
   *     MOZ_ASSERT(res2.unwrap() == HtmlFreeString("hello, andThen!");
   *
   * `andThen`ing over error results does not invoke the function, and just
   * propagates the error result:
   *
   *     Result<int, const char*> res("some error");
   *     auto res2 = res.andThen([](int x) { ... });
   *     MOZ_ASSERT(res2.isErr());
   *     MOZ_ASSERT(res.unwrapErr() == res2.unwrapErr());
   */
  template <typename F, typename = std::enable_if_t<detail::IsResult<
                            std::invoke_result_t<F, V&&>>::value>>
  constexpr auto andThen(F f) -> std::invoke_result_t<F, V&&> {
    return MOZ_LIKELY(isOk()) ? f(unwrap()) : propagateErr();
  }
};

/**
 * A type that auto-converts to an error Result. This is like a Result without
 * a success type. It's the best return type for functions that always return
 * an error--functions designed to build and populate error objects. It's also
 * useful in error-handling macros; see MOZ_TRY for an example.
 */
template <typename E>
class MOZ_MUST_USE_TYPE GenericErrorResult {
  E mErrorValue;

  template <typename V, typename E2>
  friend class Result;

 public:
  explicit constexpr GenericErrorResult(const E& aErrorValue)
      : mErrorValue(aErrorValue) {}

  explicit constexpr GenericErrorResult(E&& aErrorValue)
      : mErrorValue(std::move(aErrorValue)) {}

  constexpr GenericErrorResult(const E& aErrorValue, const ErrorPropagationTag&)
      : GenericErrorResult(aErrorValue) {}

  constexpr GenericErrorResult(E&& aErrorValue, const ErrorPropagationTag&)
      : GenericErrorResult(std::move(aErrorValue)) {}
};

template <typename E>
inline constexpr auto Err(E&& aErrorValue) {
  return GenericErrorResult<std::decay_t<E>>(std::forward<E>(aErrorValue));
}

}  // namespace mozilla

/**
 * MOZ_TRY(expr) is the C++ equivalent of Rust's `try!(expr);`. First, it
 * evaluates expr, which must produce a Result value. On success, it
 * discards the result altogether. On error, it immediately returns an error
 * Result from the enclosing function.
 */
#define MOZ_TRY(expr)                                   \
  do {                                                  \
    auto mozTryTempResult_ = ::mozilla::ToResult(expr); \
    if (MOZ_UNLIKELY(mozTryTempResult_.isErr())) {      \
      return mozTryTempResult_.propagateErr();          \
    }                                                   \
  } while (0)

/**
 * MOZ_TRY_VAR(target, expr) is the C++ equivalent of Rust's `target =
 * try!(expr);`. First, it evaluates expr, which must produce a Result value. On
 * success, the result's success value is assigned to target. On error,
 * immediately returns the error result. |target| must be an lvalue.
 */
#define MOZ_TRY_VAR(target, expr)                     \
  do {                                                \
    auto mozTryVarTempResult_ = (expr);               \
    if (MOZ_UNLIKELY(mozTryVarTempResult_.isErr())) { \
      return mozTryVarTempResult_.propagateErr();     \
    }                                                 \
    (target) = mozTryVarTempResult_.unwrap();         \
  } while (0)

#endif  // mozilla_Result_h
