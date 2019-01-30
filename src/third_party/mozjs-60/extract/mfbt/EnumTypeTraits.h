/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/* Type traits for enums. */

#ifndef mozilla_EnumTypeTraits_h
#define mozilla_EnumTypeTraits_h

#include <type_traits>

namespace mozilla {

namespace detail {

template<size_t EnumSize, bool EnumSigned, size_t StorageSize, bool StorageSigned>
struct EnumFitsWithinHelper;

// Signed enum, signed storage.
template<size_t EnumSize, size_t StorageSize>
struct EnumFitsWithinHelper<EnumSize, true, StorageSize, true>
  : public std::integral_constant<bool, (EnumSize <= StorageSize)>
{};

// Signed enum, unsigned storage.
template<size_t EnumSize, size_t StorageSize>
struct EnumFitsWithinHelper<EnumSize, true, StorageSize, false>
  : public std::integral_constant<bool, false>
{};

// Unsigned enum, signed storage.
template<size_t EnumSize, size_t StorageSize>
struct EnumFitsWithinHelper<EnumSize, false, StorageSize, true>
  : public std::integral_constant<bool, (EnumSize * 2 <= StorageSize)>
{};

// Unsigned enum, unsigned storage.
template<size_t EnumSize, size_t StorageSize>
struct EnumFitsWithinHelper<EnumSize, false, StorageSize, false>
  : public std::integral_constant<bool, (EnumSize <= StorageSize)>
{};

} // namespace detail

/*
 * Type trait that determines whether the enum type T can fit within the
 * integral type Storage without data loss. This trait should be used with
 * caution with an enum type whose underlying type has not been explicitly
 * specified: for such enums, the C++ implementation is free to choose a type
 * no smaller than int whose range encompasses all possible values of the enum.
 * So for an enum with only small non-negative values, the underlying type may
 * be either int or unsigned int, depending on the whims of the implementation.
 */
template<typename T, typename Storage>
struct EnumTypeFitsWithin
  : public detail::EnumFitsWithinHelper<
      sizeof(T),
      std::is_signed<typename std::underlying_type<T>::type>::value,
      sizeof(Storage),
      std::is_signed<Storage>::value
    >
{
  static_assert(std::is_enum<T>::value, "must provide an enum type");
  static_assert(std::is_integral<Storage>::value, "must provide an integral type");
};

/*
 * Provides information about highest enum member value.
 * Each specialization of struct MaxEnumValue should define
 * "static constexpr unsigned int value".
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
 *     static constexpr unsigned int value = static_cast<unsigned int>(HAMSTER);
 *   };
 */
template <typename T>
struct MaxEnumValue;  // no need to define the primary template

} // namespace mozilla

#endif /* mozilla_EnumTypeTraits_h */
