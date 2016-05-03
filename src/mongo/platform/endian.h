/*    Copyright 2014 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects
 *    for all of the code used other than as permitted herein. If you modify
 *    file(s) with this exception, you may extend this exception to your
 *    version of the file(s), but you are not obligated to do so. If you do not
 *    wish to do so, delete this exception statement from your version. If you
 *    delete this exception statement from all source files in the program,
 *    then also delete it in the license file.
 */

#pragma once

#include <climits>
#include <cstdint>
#include <cstring>
#include <type_traits>

#include "mongo/config.h"
#include "mongo/platform/decimal128.h"

#pragma push_macro("MONGO_UINT16_SWAB")
#pragma push_macro("MONGO_UINT32_SWAB")
#pragma push_macro("MONGO_UINT64_SWAB")
#pragma push_macro("htobe16")
#pragma push_macro("htobe32")
#pragma push_macro("htobe64")
#pragma push_macro("htole16")
#pragma push_macro("htole32")
#pragma push_macro("htole64")
#pragma push_macro("be16toh")
#pragma push_macro("be32toh")
#pragma push_macro("be64toh")
#pragma push_macro("le16toh")
#pragma push_macro("le32toh")
#pragma push_macro("le64toh")

#undef MONGO_UINT16_SWAB
#undef MONGO_UINT32_SWAB
#undef MONGO_UINT64_SWAB
#undef htobe16
#undef htobe32
#undef htobe64
#undef htole16
#undef htole32
#undef htole64
#undef be16toh
#undef be32toh
#undef be64toh
#undef le16toh
#undef le32toh
#undef le64toh

#define MONGO_LITTLE_ENDIAN 1234
#define MONGO_BIG_ENDIAN 4321

#if defined(_MSC_VER)
#include <cstdlib>
#define MONGO_UINT16_SWAB(v) _byteswap_ushort(v)
#define MONGO_UINT32_SWAB(v) _byteswap_ulong(v)
#define MONGO_UINT64_SWAB(v) _byteswap_uint64(v)
#elif defined(__clang__) && defined(__clang_major__) && defined(__clang_minor__) && \
    (__clang_major__ >= 3) && (__clang_minor__ >= 1)
#if __has_builtin(__builtin_bswap16)
#define MONGO_UINT16_SWAB(v) __builtin_bswap16(v)
#endif
#if __has_builtin(__builtin_bswap32)
#define MONGO_UINT32_SWAB(v) __builtin_bswap32(v)
#endif
#if __has_builtin(__builtin_bswap64)
#define MONGO_UINT64_SWAB(v) __builtin_bswap64(v)
#endif
#elif defined(__GNUC__) && (__GNUC__ >= 4)
#if __GNUC__ >= 4 && defined(__GNUC_MINOR__) && __GNUC_MINOR__ >= 3
#define MONGO_UINT32_SWAB(v) __builtin_bswap32(v)
#define MONGO_UINT64_SWAB(v) __builtin_bswap64(v)
#endif
#if __GNUC__ >= 4 && defined(__GNUC_MINOR__) && __GNUC_MINOR__ >= 8
#define MONGO_UINT16_SWAB(v) __builtin_bswap16(v)
#endif
#elif defined(__sun)
#include <sys/byteorder.h>
#define MONGO_UINT16_SWAB(v) BSWAP_16(v)
#define MONGO_UINT32_SWAB(v) BSWAP_32(v)
#define MONGO_UINT64_SWAB(v) BSWAP_64(v)
#endif

#ifndef MONGO_UINT16_SWAB
#define MONGO_UINT16_SWAB(v) endian::bswap_slow16(v)
#endif

#ifndef MONGO_UINT32_SWAB
#define MONGO_UINT32_SWAB(v) endian::bswap_slow32(v)
#endif

#ifndef MONGO_UINT64_SWAB
#define MONGO_UINT64_SWAB(v) endian::bswap_slow64(v)
#endif

#if MONGO_CONFIG_BYTE_ORDER == MONGO_LITTLE_ENDIAN
#define htobe16(v) MONGO_UINT16_SWAB(v)
#define htobe32(v) MONGO_UINT32_SWAB(v)
#define htobe64(v) MONGO_UINT64_SWAB(v)
#define htole16(v) (v)
#define htole32(v) (v)
#define htole64(v) (v)
#define be16toh(v) MONGO_UINT16_SWAB(v)
#define be32toh(v) MONGO_UINT32_SWAB(v)
#define be64toh(v) MONGO_UINT64_SWAB(v)
#define le16toh(v) (v)
#define le32toh(v) (v)
#define le64toh(v) (v)
#elif MONGO_CONFIG_BYTE_ORDER == MONGO_BIG_ENDIAN
#define htobe16(v) (v)
#define htobe32(v) (v)
#define htobe64(v) (v)
#define htole16(v) MONGO_UINT16_SWAB(v)
#define htole32(v) MONGO_UINT32_SWAB(v)
#define htole64(v) MONGO_UINT64_SWAB(v)
#define be16toh(v) (v)
#define be32toh(v) (v)
#define be64toh(v) (v)
#define le16toh(v) MONGO_UINT16_SWAB(v)
#define le32toh(v) MONGO_UINT32_SWAB(v)
#define le64toh(v) MONGO_UINT64_SWAB(v)
#else
#error \
    "The endianness of target architecture is unknown. " \
        "Please define MONGO_CONFIG_BYTE_ORDER"
#endif

namespace mongo {
namespace endian {

static inline uint16_t bswap_slow16(uint16_t v) {
    return ((v & 0x00FF) << 8) | ((v & 0xFF00) >> 8);
}

static inline uint32_t bswap_slow32(uint32_t v) {
    return ((v & 0x000000FFUL) << 24) | ((v & 0x0000FF00UL) << 8) | ((v & 0x00FF0000UL) >> 8) |
        ((v & 0xFF000000UL) >> 24);
}

static inline uint64_t bswap_slow64(uint64_t v) {
    return ((v & 0x00000000000000FFULL) << 56) | ((v & 0x000000000000FF00ULL) << 40) |
        ((v & 0x0000000000FF0000ULL) << 24) | ((v & 0x00000000FF000000ULL) << 8) |
        ((v & 0x000000FF00000000ULL) >> 8) | ((v & 0x0000FF0000000000ULL) >> 24) |
        ((v & 0x00FF000000000000ULL) >> 40) | ((v & 0xFF00000000000000ULL) >> 56);
}

template <typename T>
struct ByteOrderConverter;

template <>
struct ByteOrderConverter<uint8_t> {
    typedef uint8_t T;

    inline static T nativeToBig(T t) {
        return t;
    }

    inline static T bigToNative(T t) {
        return t;
    }

    inline static T nativeToLittle(T t) {
        return t;
    }

    inline static T littleToNative(T t) {
        return t;
    }
};

template <>
struct ByteOrderConverter<uint16_t> {
    typedef uint16_t T;

    inline static T nativeToBig(T t) {
        return htobe16(t);
    }

    inline static T bigToNative(T t) {
        return be16toh(t);
    }

    inline static T nativeToLittle(T t) {
        return htole16(t);
    }

    inline static T littleToNative(T t) {
        return le16toh(t);
    }
};

template <>
struct ByteOrderConverter<uint32_t> {
    typedef uint32_t T;

    inline static T nativeToBig(T t) {
        return htobe32(t);
    }

    inline static T bigToNative(T t) {
        return be32toh(t);
    }

    inline static T nativeToLittle(T t) {
        return htole32(t);
    }

    inline static T littleToNative(T t) {
        return le32toh(t);
    }
};

template <>
struct ByteOrderConverter<uint64_t> {
    typedef uint64_t T;

    inline static T nativeToBig(T t) {
        return htobe64(t);
    }

    inline static T bigToNative(T t) {
        return be64toh(t);
    }

    inline static T nativeToLittle(T t) {
        return htole64(t);
    }

    inline static T littleToNative(T t) {
        return le64toh(t);
    }
};

template <>
struct ByteOrderConverter<int8_t> {
    typedef int8_t T;

    inline static T nativeToBig(T t) {
        return t;
    }

    inline static T bigToNative(T t) {
        return t;
    }

    inline static T nativeToLittle(T t) {
        return t;
    }

    inline static T littleToNative(T t) {
        return t;
    }
};

template <>
struct ByteOrderConverter<int16_t> {
    typedef int16_t T;

    inline static T nativeToBig(T t) {
        return htobe16(static_cast<uint16_t>(t));
    }

    inline static T bigToNative(T t) {
        return be16toh(static_cast<uint16_t>(t));
    }

    inline static T nativeToLittle(T t) {
        return htole16(static_cast<uint16_t>(t));
    }

    inline static T littleToNative(T t) {
        return le16toh(static_cast<uint16_t>(t));
    }
};

template <>
struct ByteOrderConverter<int32_t> {
    typedef int32_t T;

    inline static T nativeToBig(T t) {
        return htobe32(static_cast<uint32_t>(t));
    }

    inline static T bigToNative(T t) {
        return be32toh(static_cast<uint32_t>(t));
    }

    inline static T nativeToLittle(T t) {
        return htole32(static_cast<uint32_t>(t));
    }

    inline static T littleToNative(T t) {
        return le32toh(static_cast<uint32_t>(t));
    }
};

template <>
struct ByteOrderConverter<int64_t> {
    typedef int64_t T;

    inline static T nativeToBig(T t) {
        return htobe64(static_cast<uint64_t>(t));
    }

    inline static T bigToNative(T t) {
        return be64toh(static_cast<uint64_t>(t));
    }

    inline static T nativeToLittle(T t) {
        return htole64(static_cast<uint64_t>(t));
    }

    inline static T littleToNative(T t) {
        return le64toh(static_cast<uint64_t>(t));
    }
};

template <>
struct ByteOrderConverter<float> {
    typedef float T;

    inline static T nativeToBig(T t) {
        static_assert(sizeof(T) == sizeof(uint32_t), "sizeof(T) == sizeof(uint32_t)");

        uint32_t temp;
        std::memcpy(&temp, &t, sizeof(t));
        temp = htobe32(temp);
        std::memcpy(&t, &temp, sizeof(t));
        return t;
    }

    inline static T bigToNative(T t) {
        uint32_t temp;
        std::memcpy(&temp, &t, sizeof(t));
        temp = be32toh(temp);
        std::memcpy(&t, &temp, sizeof(t));
        return t;
    }

    inline static T nativeToLittle(T t) {
        uint32_t temp;
        std::memcpy(&temp, &t, sizeof(t));
        temp = htole32(temp);
        std::memcpy(&t, &temp, sizeof(t));
        return t;
    }

    inline static T littleToNative(T t) {
        uint32_t temp;
        std::memcpy(&temp, &t, sizeof(t));
        temp = le32toh(temp);
        std::memcpy(&t, &temp, sizeof(t));
        return t;
    }
};

template <>
struct ByteOrderConverter<double> {
    typedef double T;

    inline static T nativeToBig(T t) {
        static_assert(sizeof(T) == sizeof(uint64_t), "sizeof(T) == sizeof(uint64_t)");

        uint64_t temp;
        std::memcpy(&temp, &t, sizeof(t));
        temp = htobe64(temp);
        std::memcpy(&t, &temp, sizeof(t));
        return t;
    }

    inline static T bigToNative(T t) {
        uint64_t temp;
        std::memcpy(&temp, &t, sizeof(t));
        temp = be64toh(temp);
        std::memcpy(&t, &temp, sizeof(t));
        return t;
    }

    inline static T nativeToLittle(T t) {
        uint64_t temp;
        std::memcpy(&temp, &t, sizeof(t));
        temp = htole64(temp);
        std::memcpy(&t, &temp, sizeof(t));
        return t;
    }

    inline static T littleToNative(T t) {
        uint64_t temp;
        std::memcpy(&temp, &t, sizeof(t));
        temp = le64toh(temp);
        std::memcpy(&t, &temp, sizeof(t));
        return t;
    }
};

template <>
struct ByteOrderConverter<Decimal128::Value> {
    typedef Decimal128::Value T;

    inline static T nativeToBig(T t) {
        ByteOrderConverter<uint64_t>::nativeToBig(t.low64);
        ByteOrderConverter<uint64_t>::nativeToBig(t.high64);
        return t;
    }

    inline static T bigToNative(T t) {
        ByteOrderConverter<uint64_t>::bigToNative(t.low64);
        ByteOrderConverter<uint64_t>::bigToNative(t.high64);
        return t;
    }

    inline static T nativeToLittle(T t) {
        ByteOrderConverter<uint64_t>::nativeToLittle(t.low64);
        ByteOrderConverter<uint64_t>::nativeToLittle(t.high64);
        return t;
    }

    inline static T littleToNative(T t) {
        ByteOrderConverter<uint64_t>::littleToNative(t.low64);
        ByteOrderConverter<uint64_t>::littleToNative(t.high64);
        return t;
    }
};

// Use a typemape to normalize non-fixed-width integral types to the associated fixed width
// types.

template <typename T>
struct IntegralTypeMap {
    typedef T type;
};

template <>
struct IntegralTypeMap<signed char> {
    static_assert(CHAR_BIT == 8, "CHAR_BIT == 8");
    typedef int8_t type;
};

template <>
struct IntegralTypeMap<unsigned char> {
    static_assert(CHAR_BIT == 8, "CHAR_BIT == 8");
    typedef uint8_t type;
};

template <>
struct IntegralTypeMap<char> {
    static_assert(CHAR_BIT == 8, "CHAR_BIT == 8");
    typedef std::conditional<std::is_signed<char>::value, int8_t, uint8_t>::type type;
};

template <>
struct IntegralTypeMap<long long> {
    static_assert(sizeof(long long) == sizeof(int64_t), "sizeof(long long) == sizeof(int64_t)");
    typedef int64_t type;
};

template <>
struct IntegralTypeMap<unsigned long long> {
    static_assert(sizeof(unsigned long long) == sizeof(uint64_t),
                  "sizeof(unsigned long long) == sizeof(uint64_t)");
    typedef uint64_t type;
};

template <typename T>
inline T nativeToBig(T t) {
    return ByteOrderConverter<typename IntegralTypeMap<T>::type>::nativeToBig(t);
}

template <typename T>
inline T bigToNative(T t) {
    return ByteOrderConverter<typename IntegralTypeMap<T>::type>::bigToNative(t);
}

template <typename T>
inline T nativeToLittle(T t) {
    return ByteOrderConverter<typename IntegralTypeMap<T>::type>::nativeToLittle(t);
}

template <typename T>
inline T littleToNative(T t) {
    return ByteOrderConverter<typename IntegralTypeMap<T>::type>::littleToNative(t);
}

}  // namespace endian
}  // namespace mongo

#undef MONGO_UINT16_SWAB
#undef MONGO_UINT32_SWAB
#undef MONGO_UINT64_SWAB
#undef htobe16
#undef htobe32
#undef htobe64
#undef htole16
#undef htole32
#undef htole64
#undef be16toh
#undef be32toh
#undef be64toh
#undef le16toh
#undef le32toh
#undef le64toh

#pragma pop_macro("MONGO_UINT16_SWAB")
#pragma pop_macro("MONGO_UINT32_SWAB")
#pragma pop_macro("MONGO_UINT64_SWAB")
#pragma pop_macro("htobe16")
#pragma pop_macro("htobe32")
#pragma pop_macro("htobe64")
#pragma pop_macro("htole16")
#pragma pop_macro("htole32")
#pragma pop_macro("htole64")
#pragma pop_macro("be16toh")
#pragma pop_macro("be32toh")
#pragma pop_macro("be64toh")
#pragma pop_macro("le16toh")
#pragma pop_macro("le32toh")
#pragma pop_macro("le64toh")
