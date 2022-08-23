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

#include "mongo/util/base64.h"

#include "mongo/util/assert_util.h"

#include <array>
#include <cstdint>
#include <iostream>

namespace mongo::base64 {
namespace {

constexpr unsigned char kInvalid = ~0;

constexpr std::size_t search(StringData table, int c) {
    for (std::size_t i = 0; i < table.size(); ++i)
        if (table[i] == c)
            return i;
    return kInvalid;
}

template <std::size_t... Cs>
constexpr auto invertTable(StringData table, std::index_sequence<Cs...>) {
    return std::array<unsigned char, sizeof...(Cs)>{
        {static_cast<unsigned char>(search(table, Cs))...}};
}

constexpr StringData kEncodeTable =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/"_sd;

constexpr auto kDecodeTable = invertTable(kEncodeTable, std::make_index_sequence<256>{});

bool valid(unsigned char x) {
    return kDecodeTable[x] != kInvalid;
}

template <typename Writer>
void encodeImpl(Writer&& write, StringData in) {
    const char* data = in.rawData();
    std::size_t size = in.size();
    auto readOctet = [&data] { return static_cast<std::uint8_t>(*data++); };
    auto encodeSextet = [](unsigned x) { return kEncodeTable[x & 0b11'1111]; };

    std::array<char, 512> buf;
    std::array<char, 512>::iterator p;
    std::uint32_t accum;

    for (std::size_t fullGroups = size / 3; fullGroups;) {
        std::size_t chunkGroups = std::min(fullGroups, sizeof(buf) / 4);
        fullGroups -= chunkGroups;
        p = buf.begin();
        while (chunkGroups--) {
            accum = 0;
            accum |= readOctet() << (8 * (2 - 0));
            accum |= readOctet() << (8 * (2 - 1));
            accum |= readOctet() << (8 * (2 - 2));
            *p++ = encodeSextet(accum >> (6 * (3 - 0)));
            *p++ = encodeSextet(accum >> (6 * (3 - 1)));
            *p++ = encodeSextet(accum >> (6 * (3 - 2)));
            *p++ = encodeSextet(accum >> (6 * (3 - 3)));
        }

        write(buf.data(), p - buf.begin());
    }

    switch (size % 3) {
        case 2:
            p = buf.begin();
            accum = 0;
            accum |= readOctet() << (8 * (2 - 0));
            accum |= readOctet() << (8 * (2 - 1));
            *p++ = encodeSextet(accum >> (6 * (3 - 0)));
            *p++ = encodeSextet(accum >> (6 * (3 - 1)));
            *p++ = encodeSextet(accum >> (6 * (3 - 2)));
            *p++ = '=';
            write(buf.data(), p - buf.begin());
            break;
        case 1:
            p = buf.begin();
            accum = 0;
            accum |= readOctet() << (8 * (2 - 0));
            *p++ = encodeSextet(accum >> (6 * (3 - 0)));
            *p++ = encodeSextet(accum >> (6 * (3 - 1)));
            *p++ = '=';
            *p++ = '=';
            write(buf.data(), p - buf.begin());
            break;
        case 0:
            break;
    }
}

template <typename Writer>
void decodeImpl(const Writer& write, StringData in) {
    const char* data = in.rawData();
    std::size_t size = in.size();
    if (size == 0)
        return;
    uassert(10270, "invalid base64", size % 4 == 0);

    auto decodeSextet = [](char x) {
        auto c = kDecodeTable[static_cast<unsigned char>(x)];
        uassert(40537, "Invalid base64 character", c != kInvalid);
        return c;
    };

    std::array<char, 512> buf;
    std::array<char, 512>::iterator p;
    std::uint32_t accum;

    // All but the final group to avoid '='-related conditionals in the bulk path.
    for (std::size_t groups = size / 4 - 1; groups;) {
        std::size_t chunkGroups = std::min(groups, buf.size() / 3);
        groups -= chunkGroups;
        p = buf.begin();
        while (chunkGroups--) {
            accum = 0;
            accum |= decodeSextet(*data++) << (6 * (3 - 0));
            accum |= decodeSextet(*data++) << (6 * (3 - 1));
            accum |= decodeSextet(*data++) << (6 * (3 - 2));
            accum |= decodeSextet(*data++) << (6 * (3 - 3));
            *p++ = (accum >> (8 * (2 - 0))) & 0xff;
            *p++ = (accum >> (8 * (2 - 1))) & 0xff;
            *p++ = (accum >> (8 * (2 - 2))) & 0xff;
        }
        write(buf.data(), p - buf.begin());
    }

    {
        // Final group might have some equal signs
        std::size_t nbits = 24;
        if (data[3] == '=') {
            nbits -= 8;
            if (data[2] == '=')
                nbits -= 8;
        }
        accum = 0;
        accum |= decodeSextet(*data++) << (6 * (3 - 0));
        accum |= decodeSextet(*data++) << (6 * (3 - 1));
        if (nbits > (6 * 2))
            accum |= decodeSextet(*data++) << (6 * (3 - 2));
        if (nbits > (6 * 3))
            accum |= decodeSextet(*data++) << (6 * (3 - 3));

        p = buf.begin();
        if (nbits > (8 * 0))
            *p++ = accum >> (8 * (2 - 0));
        if (nbits > (8 * 1))
            *p++ = accum >> (8 * (2 - 1));
        if (nbits > (8 * 2))
            *p++ = accum >> (8 * (2 - 2));
        write(buf.data(), p - buf.begin());
    }
}

}  // namespace

std::string encode(StringData in) {
    std::string r;
    r.reserve(encodedLength(in.size()));
    encodeImpl([&](const char* s, std::size_t n) { r.append(s, s + n); }, in);
    return r;
}

std::string decode(StringData in) {
    std::string r;
    r.reserve(in.size() / 4 * 3);
    decodeImpl([&](const char* s, std::size_t n) { r.append(s, s + n); }, in);
    return r;
}

void encode(std::stringstream& ss, StringData in) {
    encodeImpl([&](const char* s, std::size_t n) { ss.write(s, n); }, in);
}

void decode(std::stringstream& ss, StringData in) {
    decodeImpl([&](const char* s, std::size_t n) { ss.write(s, n); }, in);
}

void encode(fmt::memory_buffer& buffer, StringData in) {
    buffer.reserve(buffer.size() + encodedLength(in.size()));
    encodeImpl([&](const char* s, std::size_t n) { buffer.append(s, s + n); }, in);
}

void decode(fmt::memory_buffer& buffer, StringData in) {
    buffer.reserve(buffer.size() + in.size() / 4 * 3);
    decodeImpl([&](const char* s, std::size_t n) { buffer.append(s, s + n); }, in);
}


bool validate(StringData s) {
    if (s.size() % 4) {
        return false;
    }
    if (s.empty()) {
        return true;
    }

    using std::begin;
    using std::end;

    auto const unwindTerminator = [](auto it) { return (*(it - 1) == '=') ? (it - 1) : it; };
    auto const e = unwindTerminator(unwindTerminator(end(s)));

    return e == std::find_if(begin(s), e, [](const char ch) { return !valid(ch); });
}

}  // namespace mongo::base64
