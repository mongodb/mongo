// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/util/modules.h"

#include <climits>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <type_traits>
#include <utility>

namespace [[MONGO_MOD_PUBLIC]] mongo {
namespace endian {

/** Like `std::endian`. https://en.cppreference.com/w/cpp/types/endian */
enum class Order {
#if defined(_MSC_VER) && (defined(_M_IX86) || defined(_M_X64))
    kLittle,
    kBig,
    kNative = kLittle,
#elif defined(__GNUC__)
    kLittle = __ORDER_LITTLE_ENDIAN__,
    kBig = __ORDER_BIG_ENDIAN__,
    kNative = __BYTE_ORDER__,
#else
#error "Unsupported compiler or architecture"
#endif  // compiler check
};

namespace detail {

inline std::uint8_t bswap(std::uint8_t v) {
    return v;
}
#if defined(_MSC_VER)
inline std::uint16_t bswap(std::uint16_t v) {
    return _byteswap_ushort(v);
}
inline std::uint32_t bswap(std::uint32_t v) {
    return _byteswap_ulong(v);
}
inline std::uint64_t bswap(std::uint64_t v) {
    return _byteswap_uint64(v);
}
#else   // non-MSVC
inline std::uint16_t bswap(std::uint16_t v) {
    return __builtin_bswap16(v);
}
inline std::uint32_t bswap(std::uint32_t v) {
    return __builtin_bswap32(v);
}
inline std::uint64_t bswap(std::uint64_t v) {
    return __builtin_bswap64(v);
}
#endif  // non-MSVC

/**
 * Same requirements as `std::bit_cast`. `From` is only being trivially copied, but `To` is
 * being trivially constructed, which is a stronger operation.
 * https://en.cppreference.com/w/cpp/numeric/bit_cast
 */
template <typename To,
          typename From,
          std::enable_if_t<sizeof(To) == sizeof(From) &&              //
                               std::is_trivially_copyable_v<From> &&  //
                               std::is_trivial_v<To>,                 //
                           int> = 0>
To bitCast(From t) {
    To u;
    std::memcpy(&u, &t, sizeof(t));
    return u;
}

template <typename T>
using RequireArithmetic = std::enable_if_t<std::is_arithmetic_v<T>, int>;

}  // namespace detail

template <Order From, Order To, typename T>
T convertByteOrder(T t) {
    if constexpr (From == To) {
        return t;
    } else {
        static constexpr std::size_t bits = CHAR_BIT * sizeof(T);
        // clang-format off
        using BitInt = std::conditional_t<bits == 8, std::uint8_t,
                       std::conditional_t<bits == 16, std::uint16_t,
                       std::conditional_t<bits == 32, std::uint32_t,
                       std::conditional_t<bits == 64, std::uint64_t,
                       void>>>>;
        // clang-format on
        return detail::bitCast<T>(detail::bswap(detail::bitCast<BitInt>(t)));
    }
}

template <typename T, detail::RequireArithmetic<T> = 0>
T nativeToBig(T t) {
    return convertByteOrder<Order::kNative, Order::kBig>(t);
}

template <typename T, detail::RequireArithmetic<T> = 0>
T nativeToLittle(T t) {
    return convertByteOrder<Order::kNative, Order::kLittle>(t);
}

template <typename T, detail::RequireArithmetic<T> = 0>
T bigToNative(T t) {
    return convertByteOrder<Order::kBig, Order::kNative>(t);
}

template <typename T, detail::RequireArithmetic<T> = 0>
T littleToNative(T t) {
    return convertByteOrder<Order::kLittle, Order::kNative>(t);
}

}  // namespace endian
}  // namespace mongo
