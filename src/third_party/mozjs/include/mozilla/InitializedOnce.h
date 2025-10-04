/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

// Class template for objects that can only be initialized once.

#ifndef mozilla_mfbt_initializedonce_h__
#define mozilla_mfbt_initializedonce_h__

#include "mozilla/Assertions.h"
#include "mozilla/Maybe.h"

#include <type_traits>

namespace mozilla {

namespace detail {

enum struct InitWhen { InConstructorOnly, LazyAllowed };
enum struct DestroyWhen { EarlyAllowed, InDestructorOnly };

namespace ValueCheckPolicies {
template <typename T>
struct AllowAnyValue {
  constexpr static bool Check(const T& /*aValue*/) { return true; }
};

template <typename T>
struct ConvertsToTrue {
  constexpr static bool Check(const T& aValue) {
    return static_cast<bool>(aValue);
  }
};
}  // namespace ValueCheckPolicies

// A kind of mozilla::Maybe that can only be initialized and cleared once. It
// cannot be re-initialized. This is a more stateful than a const Maybe<T> in
// that it can be cleared, but much less stateful than a non-const Maybe<T>
// which could be reinitialized multiple times. Can only be used with const T
// to ensure that the contents cannot be modified either.
// TODO: Make constructors constexpr when Maybe's constructors are constexpr
// (Bug 1601336).
template <typename T, InitWhen InitWhenVal, DestroyWhen DestroyWhenVal,
          template <typename> class ValueCheckPolicy =
              ValueCheckPolicies::AllowAnyValue>
class InitializedOnce final {
  static_assert(std::is_const_v<T>);
  using MaybeType = Maybe<std::remove_const_t<T>>;

 public:
  using ValueType = T;

  template <typename Dummy = void>
  explicit constexpr InitializedOnce(
      std::enable_if_t<InitWhenVal == InitWhen::LazyAllowed, Dummy>* =
          nullptr) {}

  // note: aArg0 is named separately here to disallow calling this with no
  // arguments. The default constructor should only be available conditionally
  // and is declared above.
  template <typename Arg0, typename... Args>
  explicit constexpr InitializedOnce(Arg0&& aArg0, Args&&... aArgs)
      : mMaybe{Some(std::remove_const_t<T>{std::forward<Arg0>(aArg0),
                                           std::forward<Args>(aArgs)...})} {
    MOZ_ASSERT(ValueCheckPolicy<T>::Check(*mMaybe));
  }

  InitializedOnce(const InitializedOnce&) = delete;
  InitializedOnce(InitializedOnce&& aOther) : mMaybe{std::move(aOther.mMaybe)} {
    static_assert(DestroyWhenVal == DestroyWhen::EarlyAllowed);
#ifdef DEBUG
    aOther.mWasReset = true;
#endif
  }
  InitializedOnce& operator=(const InitializedOnce&) = delete;
  InitializedOnce& operator=(InitializedOnce&& aOther) {
    static_assert(InitWhenVal == InitWhen::LazyAllowed &&
                  DestroyWhenVal == DestroyWhen::EarlyAllowed);
    MOZ_ASSERT(!mWasReset);
    MOZ_ASSERT(!mMaybe);
    mMaybe.~MaybeType();
    new (&mMaybe) MaybeType{std::move(aOther.mMaybe)};
#ifdef DEBUG
    aOther.mWasReset = true;
#endif
    return *this;
  }

  template <typename... Args, typename Dummy = void>
  constexpr std::enable_if_t<InitWhenVal == InitWhen::LazyAllowed, Dummy> init(
      Args&&... aArgs) {
    MOZ_ASSERT(mMaybe.isNothing());
    MOZ_ASSERT(!mWasReset);
    mMaybe.emplace(std::remove_const_t<T>{std::forward<Args>(aArgs)...});
    MOZ_ASSERT(ValueCheckPolicy<T>::Check(*mMaybe));
  }

  constexpr explicit operator bool() const { return isSome(); }
  constexpr bool isSome() const { return mMaybe.isSome(); }
  constexpr bool isNothing() const { return mMaybe.isNothing(); }

  constexpr T& operator*() const { return *mMaybe; }
  constexpr T* operator->() const { return mMaybe.operator->(); }

  constexpr T& ref() const { return mMaybe.ref(); }

  template <typename Dummy = void>
  std::enable_if_t<DestroyWhenVal == DestroyWhen::EarlyAllowed, Dummy>
  destroy() {
    MOZ_ASSERT(mMaybe.isSome());
    maybeDestroy();
  }

  template <typename Dummy = void>
  std::enable_if_t<DestroyWhenVal == DestroyWhen::EarlyAllowed, Dummy>
  maybeDestroy() {
    mMaybe.reset();
#ifdef DEBUG
    mWasReset = true;
#endif
  }

  template <typename Dummy = T>
  std::enable_if_t<DestroyWhenVal == DestroyWhen::EarlyAllowed, Dummy>
  release() {
    MOZ_ASSERT(mMaybe.isSome());
    auto res = std::move(mMaybe.ref());
    destroy();
    return res;
  }

 private:
  MaybeType mMaybe;
#ifdef DEBUG
  bool mWasReset = false;
#endif
};

template <typename T, InitWhen InitWhenVal, DestroyWhen DestroyWhenVal,
          template <typename> class ValueCheckPolicy>
class LazyInitializer {
 public:
  explicit LazyInitializer(InitializedOnce<T, InitWhenVal, DestroyWhenVal,
                                           ValueCheckPolicy>& aLazyInitialized)
      : mLazyInitialized{aLazyInitialized} {}

  template <typename U>
  LazyInitializer& operator=(U&& aValue) {
    mLazyInitialized.init(std::forward<U>(aValue));
    return *this;
  }

  LazyInitializer(const LazyInitializer&) = delete;
  LazyInitializer& operator=(const LazyInitializer&) = delete;

 private:
  InitializedOnce<T, InitWhenVal, DestroyWhenVal, ValueCheckPolicy>&
      mLazyInitialized;
};

}  // namespace detail

// The following *InitializedOnce* template aliases allow to declare class
// member variables that can only be initialized once, but maybe destroyed
// earlier explicitly than in the containing classes destructor.
// The intention is to restrict the possible state transitions for member
// variables that can almost be const, but not quite. This may be particularly
// useful for classes with a lot of members. Uses in other contexts, e.g. as
// local variables, are possible, but probably seldom useful. They can only be
// instantiated with a const element type. Any misuses that cannot be detected
// at compile time trigger a MOZ_ASSERT at runtime. Individually spelled out
// assertions for these aspects are not necessary, which may improve the
// readability of the code without impairing safety.
//
// The base variant InitializedOnce requires initialization in the constructor,
// but allows early destruction using destroy(), and allow move construction. It
// is similar to Maybe<const T> in some sense, but a Maybe<const T> could be
// reinitialized arbitrarily. InitializedOnce expresses the intent not to do
// this, and prohibits reinitialization.
//
// The Lazy* variants allow default construction, and can be initialized lazily
// using init() in that case, but it cannot be reinitialized either. They do not
// allow early destruction.
//
// The Lazy*EarlyDestructible variants allow lazy initialization, early
// destruction, move construction and move assignment. This should be used only
// when really required.
//
// The *NotNull variants only allow initialization with values that convert to
// bool as true. They are named NotNull because the typical use case is with
// (smart) pointer types, but any other type convertible to bool will also work
// analogously.
//
// There is no variant combining detail::DestroyWhen::InConstructorOnly with
// detail::DestroyWhen::InDestructorOnly because this would be equivalent to a
// const member.
//
// For special cases, e.g. requiring custom value check policies,
// detail::InitializedOnce might be instantiated directly, but be mindful when
// doing this.

template <typename T>
using InitializedOnce =
    detail::InitializedOnce<T, detail::InitWhen::InConstructorOnly,
                            detail::DestroyWhen::EarlyAllowed>;

template <typename T>
using InitializedOnceNotNull =
    detail::InitializedOnce<T, detail::InitWhen::InConstructorOnly,
                            detail::DestroyWhen::EarlyAllowed,
                            detail::ValueCheckPolicies::ConvertsToTrue>;

template <typename T>
using LazyInitializedOnce =
    detail::InitializedOnce<T, detail::InitWhen::LazyAllowed,
                            detail::DestroyWhen::InDestructorOnly>;

template <typename T>
using LazyInitializedOnceNotNull =
    detail::InitializedOnce<T, detail::InitWhen::LazyAllowed,
                            detail::DestroyWhen::InDestructorOnly,
                            detail::ValueCheckPolicies::ConvertsToTrue>;

template <typename T>
using LazyInitializedOnceEarlyDestructible =
    detail::InitializedOnce<T, detail::InitWhen::LazyAllowed,
                            detail::DestroyWhen::EarlyAllowed>;

template <typename T>
using LazyInitializedOnceNotNullEarlyDestructible =
    detail::InitializedOnce<T, detail::InitWhen::LazyAllowed,
                            detail::DestroyWhen::EarlyAllowed,
                            detail::ValueCheckPolicies::ConvertsToTrue>;

template <typename T, detail::InitWhen InitWhenVal,
          detail::DestroyWhen DestroyWhenVal,
          template <typename> class ValueCheckPolicy>
auto do_Init(detail::InitializedOnce<T, InitWhenVal, DestroyWhenVal,
                                     ValueCheckPolicy>& aLazyInitialized) {
  return detail::LazyInitializer(aLazyInitialized);
}

}  // namespace mozilla

#endif
