/**
 *    Copyright (C) 2018-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include "mongo/platform/basic.h"

#include "mongo/util/itoa.h"

#include <cstdint>
#include <type_traits>

#include "mongo/base/string_data.h"
#include "mongo/platform/bits.h"
#include "mongo/util/assert_util.h"

namespace mongo {
namespace {

/**
 * This is a tunable parameter of the ItoA implementation. It can be adjusted for
 * benchmarking to find optimal table size. Currently 4 digits has the best performance.
 * (Changing this number requires adjusting the ALL_DIGITS support macro).
 */
#ifndef ITOA_TABLE_DIGITS
#define ITOA_TABLE_DIGITS 4
#endif

/**
 * Generates a table by calling function-like `macro(d3,d2,d1,d0)` with the 4 decimal
 * digits of the table index. That is, generates the expansion of:
 *
 *     `m(0,0,0,0) m(0,0,0,1) ... m(9,9,9,9)`
 */
// clang-format off
#define ALL_DIGITS(m) \
    D1(m,0) D1(m,1) D1(m,2) D1(m,3) D1(m,4) \
    D1(m,5) D1(m,6) D1(m,7) D1(m,8) D1(m,9)
#define D1(m,d0) \
    D2(m,d0,0) D2(m,d0,1) D2(m,d0,2) D2(m,d0,3) D2(m,d0,4) \
    D2(m,d0,5) D2(m,d0,6) D2(m,d0,7) D2(m,d0,8) D2(m,d0,9)
#define D2(m,d1,d0) \
    D3(m,d1,d0,0) D3(m,d1,d0,1) D3(m,d1,d0,2) D3(m,d1,d0,3) D3(m,d1,d0,4) \
    D3(m,d1,d0,5) D3(m,d1,d0,6) D3(m,d1,d0,7) D3(m,d1,d0,8) D3(m,d1,d0,9)
#define D3(m,d2,d1,d0) \
    D4(m,d2,d1,d0,0) D4(m,d2,d1,d0,1) D4(m,d2,d1,d0,2) D4(m,d2,d1,d0,3) D4(m,d2,d1,d0,4) \
    D4(m,d2,d1,d0,5) D4(m,d2,d1,d0,6) D4(m,d2,d1,d0,7) D4(m,d2,d1,d0,8) D4(m,d2,d1,d0,9)
#define D4(macro,d3,d2,d1,d0) \
    macro(d3,d2,d1,d0)
// clang-format on

constexpr std::size_t pow10(std::size_t n) {
    return n ? 10 * pow10(n - 1) : 1;
}

constexpr std::size_t kTableDigits = ITOA_TABLE_DIGITS;
constexpr std::size_t kTableSize = pow10(kTableDigits);

//  Examples from a 4-digit `gTable`:
//    {1, {'0','0','0','0'}}  //    0
//    {1, {'0','0','0','9'}}  //    9
//    {2, {'0','0','9','9'}}  //   99
//    {3, {'0','9','9','9'}}  //  999
//    {4, {'9','9','9','9'}}  // 9999
struct Entry {
    std::uint8_t width;  // Number of digits to be printed when not zero-padded.
    char s[kTableDigits];
};

template <int D0, int... Dn>
constexpr uint8_t printedWidth() {
    const int kMag = sizeof...(Dn);
    if constexpr (D0 != 0)
        return 1 + kMag;
    else if constexpr (kMag == 0)
        return 1;  // The all-zeros pattern still has 1 digit when printed.
    else
        return printedWidth<Dn...>();
}

template <int... D>
constexpr Entry makeEntry() {
    return Entry{printedWidth<D...>(), {('0' + D)...}};
}

#define ITOA_MAKE_ENTRY(...) makeEntry<__VA_ARGS__>(),

constexpr Entry gTable[] = {ALL_DIGITS(ITOA_MAKE_ENTRY)};
static_assert(std::extent_v<decltype(gTable)> == kTableSize, "gTable has correct size.");

}  // namespace

ItoA::ItoA(std::uint64_t val) {
    if (val < kTableSize) {
        const auto& e = gTable[val];
        std::size_t n = e.width;
        _str = StringData(std::end(e.s) - n, n);
        return;
    }
    char* p = std::end(_buf);
    while (val >= kTableSize) {
        std::size_t idx = val % kTableSize;
        val /= kTableSize;
        const auto& e = gTable[idx];
        p -= kTableDigits;
        memcpy(p, std::end(e.s) - kTableDigits, kTableDigits);
    }
    const auto& e = gTable[val];
    auto n = e.width;
    p -= n;
    memcpy(p, std::end(e.s) - n, n);
    _str = StringData(p, std::end(_buf) - p);
}

}  // namespace mongo
