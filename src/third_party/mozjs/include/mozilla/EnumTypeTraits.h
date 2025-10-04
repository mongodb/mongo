/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/* Type traits for enums. */

#ifndef mozilla_EnumTypeTraits_h
#define mozilla_EnumTypeTraits_h

#include <stddef.h>
#include <type_traits>

namespace mozilla {

namespace detail {

template <size_t EnumSize, bool EnumSigned, size_t StorageSize,
          bool StorageSigned>
struct EnumFitsWithinHelper;

// Signed enum, signed storage.
template <size_t EnumSize, size_t StorageSize>
struct EnumFitsWithinHelper<EnumSize, true, StorageSize, true>
    : public std::integral_constant<bool, (EnumSize <= StorageSize)> {};

// Signed enum, unsigned storage.
template <size_t EnumSize, size_t StorageSize>
struct EnumFitsWithinHelper<EnumSize, true, StorageSize, false>
    : public std::integral_constant<bool, false> {};

// Unsigned enum, signed storage.
template <size_t EnumSize, size_t StorageSize>
struct EnumFitsWithinHelper<EnumSize, false, StorageSize, true>
    : public std::integral_constant<bool, (EnumSize * 2 <= StorageSize)> {};

// Unsigned enum, unsigned storage.
template <size_t EnumSize, size_t StorageSize>
struct EnumFitsWithinHelper<EnumSize, false, StorageSize, false>
    : public std::integral_constant<bool, (EnumSize <= StorageSize)> {};

}  // namespace detail

/*
 * Type trait that determines whether the enum type T can fit within the
 * integral type Storage without data loss. This trait should be used with
 * caution with an enum type whose underlying type has not been explicitly
 * specified: for such enums, the C++ implementation is free to choose a type
 * no smaller than int whose range encompasses all possible values of the enum.
 * So for an enum with only small non-negative values, the underlying type may
 * be either int or unsigned int, depending on the whims of the implementation.
 */
template <typename T, typename Storage>
struct EnumTypeFitsWithin
    : public detail::EnumFitsWithinHelper<
          sizeof(T),
          std::is_signed<typename std::underlying_type<T>::type>::value,
          sizeof(Storage), std::is_signed<Storage>::value> {
  static_assert(std::is_enum<T>::value, "must provide an enum type");
  static_assert(std::is_integral<Storage>::value,
                "must provide an integral type");
};

/**
 * Get the underlying value of an enum, but typesafe.
 *
 * example:
 *
 *   enum class Pet : int16_t {
 *     Cat,
 *     Dog,
 *     Fish
 *   };
 *   enum class Plant {
 *     Flower,
 *     Tree,
 *     Vine
 *   };
 *   UnderlyingValue(Pet::Fish) -> int16_t(2)
 *   UnderlyingValue(Plant::Tree) -> int(1)
 */
template <typename T>
inline constexpr auto UnderlyingValue(const T v) {
  static_assert(std::is_enum_v<T>);
  return static_cast<typename std::underlying_type<T>::type>(v);
}

/*
 * Specialize either MaxContiguousEnumValue or MaxEnumValue to provide the
 * highest enum member value for an enum class. Note that specializing
 * MaxContiguousEnumValue will make MaxEnumValue just take its value from the
 * MaxContiguousEnumValue specialization.
 *
 * Specialize MinContiguousEnumValue and MaxContiguousEnumValue to provide both
 * lowest and highest enum member values for an enum class with contiguous
 * values.
 *
 * Each specialization of these structs should define "static constexpr" member
 * variable named "value".
 *
 * example:
 *
 *   enum ExampleEnum
 *   {
 *     CAT = 0,
 *     DOG,
 *     HAMSTER
 *   };
 *
 *   template <>
 *   struct MaxEnumValue<ExampleEnum>
 *   {
 *     static constexpr ExampleEnumvalue = HAMSTER;
 *   };
 */

template <typename T>
struct MinContiguousEnumValue {
  static constexpr T value = static_cast<T>(0);
};

template <typename T>
struct MaxContiguousEnumValue;

template <typename T>
struct MaxEnumValue {
  static constexpr auto value = MaxContiguousEnumValue<T>::value;
};

// Provides the min and max values for a contiguous enum (requires at least
// MaxContiguousEnumValue to be defined).
template <typename T>
struct ContiguousEnumValues {
  static constexpr auto min = MinContiguousEnumValue<T>::value;
  static constexpr auto max = MaxContiguousEnumValue<T>::value;
};

// Provides the total number of values for a contiguous enum (requires at least
// MaxContiguousEnumValue to be defined).
template <typename T>
struct ContiguousEnumSize {
  static constexpr size_t value =
      UnderlyingValue(ContiguousEnumValues<T>::max) + 1 -
      UnderlyingValue(ContiguousEnumValues<T>::min);
};

}  // namespace mozilla

#endif /* mozilla_EnumTypeTraits_h */
