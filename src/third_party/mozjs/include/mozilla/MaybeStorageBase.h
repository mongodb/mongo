/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/* Internal storage class used e.g. by Maybe and Result. This file doesn't
 * contain any public declarations. */

#ifndef mfbt_MaybeStorageBase_h
#define mfbt_MaybeStorageBase_h

#include <type_traits>
#include <utility>

namespace mozilla::detail {

template <typename T>
constexpr bool IsTriviallyDestructibleAndCopyable =
    std::is_trivially_destructible_v<T> &&
    (std::is_trivially_copy_constructible_v<T> ||
     !std::is_copy_constructible_v<T>);

template <typename T, bool TriviallyDestructibleAndCopyable =
                          IsTriviallyDestructibleAndCopyable<T>>
struct MaybeStorageBase;

template <typename T>
struct MaybeStorageBase<T, false> {
 protected:
  using NonConstT = std::remove_const_t<T>;

  union Union {
    Union() {}
    explicit Union(const T& aVal) : val{aVal} {}
    template <typename U,
              typename = std::enable_if_t<std::is_move_constructible_v<U>>>
    explicit Union(U&& aVal) : val{std::forward<U>(aVal)} {}
    template <typename... Args>
    explicit Union(std::in_place_t, Args&&... aArgs)
        : val{std::forward<Args>(aArgs)...} {}

    ~Union() {}

    NonConstT val;
  } mStorage;

 public:
  constexpr MaybeStorageBase() = default;
  explicit MaybeStorageBase(const T& aVal) : mStorage{aVal} {}
  explicit MaybeStorageBase(T&& aVal) : mStorage{std::move(aVal)} {}
  template <typename... Args>
  explicit MaybeStorageBase(std::in_place_t, Args&&... aArgs)
      : mStorage{std::in_place, std::forward<Args>(aArgs)...} {}

  const T* addr() const { return &mStorage.val; }
  T* addr() { return &mStorage.val; }
};

template <typename T>
struct MaybeStorageBase<T, true> {
 protected:
  using NonConstT = std::remove_const_t<T>;

  union Union {
    constexpr Union() : empty() {}
    constexpr explicit Union(const T& aVal) : val{aVal} {}
    constexpr explicit Union(T&& aVal) : val{std::move(aVal)} {}
    template <typename... Args>
    constexpr explicit Union(std::in_place_t, Args&&... aArgs)
        : val{std::forward<Args>(aArgs)...} {}

    NonConstT val;
    char empty;
  } mStorage;

 public:
  constexpr MaybeStorageBase() = default;
  constexpr explicit MaybeStorageBase(const T& aVal) : mStorage{aVal} {}
  constexpr explicit MaybeStorageBase(T&& aVal) : mStorage{std::move(aVal)} {}

  template <typename... Args>
  constexpr explicit MaybeStorageBase(std::in_place_t, Args&&... aArgs)
      : mStorage{std::in_place, std::forward<Args>(aArgs)...} {}

  constexpr const T* addr() const { return &mStorage.val; }
  constexpr T* addr() { return &mStorage.val; }
};

}  // namespace mozilla::detail

#endif
