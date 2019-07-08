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

#include <array>
#include <cstdint>
#include <iostream>
#include <string>

#include "mongo/base/string_data.h"
#include "mongo/util/assert_util.h"

namespace mongo {

namespace {

constexpr std::size_t kTableDigits = 4;

constexpr std::size_t pow10(std::size_t N) {
    std::size_t r = 1;
    for (std::size_t i = 0; i < N; ++i) {
        r *= 10;
    }
    return r;
}

constexpr char digitAtPow10(std::size_t i, std::size_t pos) {
    auto x = i / pow10(pos) % 10;
    return "0123456789"[x];
}

struct Entry {
    std::uint8_t n;
    std::array<char, kTableDigits> s;
};

template <std::size_t... Ds>
constexpr Entry makeEntry(std::size_t i, std::index_sequence<Ds...>) {
    std::uint8_t mag = 1;
    for (std::size_t p = 1; p < kTableDigits; ++p)
        if (i / pow10(p))
            ++mag;
    return {mag,
            {
                digitAtPow10(i, kTableDigits - 1 - Ds)...,
            }};
}

constexpr auto makeEntry(std::size_t i) {
    return makeEntry(i, std::make_index_sequence<kTableDigits>());
}

constexpr std::size_t kTableSize = pow10(kTableDigits);

template <std::size_t... Is>
constexpr std::array<Entry, kTableSize> makeTable(std::index_sequence<Is...>) {
    return {
        makeEntry(Is)...,
    };
}

constexpr auto gTable = makeTable(std::make_index_sequence<kTableSize>());

}  // namespace

ItoA::ItoA(std::uint64_t val) {
    if (val < kTableSize) {
        const auto& e = gTable[val];
        _str = StringData(e.s.end() - e.n, e.n);
        return;
    }
    char* p = std::end(_buf);
    while (val >= kTableSize) {
        auto r = val % kTableSize;
        val /= kTableSize;
        const auto& e = gTable[r];
        p -= kTableDigits;
        memcpy(p, e.s.begin(), kTableDigits);
    }
    {
        const auto& e = gTable[val];
        std::size_t n = e.n;
        auto si = e.s.end();
        while (n--)
            *--p = *--si;
    }

    _str = StringData(p, std::end(_buf) - p);
}

}  // namespace mongo
