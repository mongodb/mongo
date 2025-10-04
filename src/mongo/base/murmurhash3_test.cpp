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

#include "mongo/base/data_type_endian.h"
#include "mongo/base/data_view.h"
#include "mongo/base/string_data.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/murmur3.h"

#include <array>
#include <cstdint>
#include <string>
#include <utility>

#define TEST_STRING32(str, seed, expected) \
    ASSERT_EQUALS(murmur3<sizeof(uint32_t)>(StringData{str}, seed), expected)

#define TEST_STRING64(str, seed, expected) \
    ASSERT_EQUALS(murmur3<sizeof(uint64_t)>(StringData{str}, seed), expected)

#define TEST_STRING128(str, seed, a, b)                \
    do {                                               \
        auto pair = compute128(StringData{str}, seed); \
        ASSERT_EQUALS(pair.first, a);                  \
        ASSERT_EQUALS(pair.second, b);                 \
    } while (0)

namespace mongo {
namespace {

std::pair<uint64_t, uint64_t> compute128(StringData input, uint32_t seed) {
    std::array<char, 16> hash;
    murmur3(input, seed, hash);
    return {ConstDataView(hash.data()).read<LittleEndian<uint64_t>>(),
            ConstDataView(hash.data()).read<LittleEndian<uint64_t>>(8)};
}

TEST(MurmurHash3, TestVectors32) {
    TEST_STRING32("", 0, 0ULL);

    TEST_STRING32("", 1ULL, 0x514E28B7ULL);
    TEST_STRING32("", 0xffffffffULL, 0x81F16F39ULL);    // make sure seed value is handled unsigned
    TEST_STRING32("\0\0\0\0"_sd, 0ULL, 0x2362F9DEULL);  // make sure we handle embedded nulls


    TEST_STRING32("aaaa", 0x9747b28cULL, 0x5A97808AULL);  // one full chunk
    TEST_STRING32("aaa", 0x9747b28cULL, 0x283E0130ULL);   // three characters
    TEST_STRING32("aa", 0x9747b28cULL, 0x5D211726ULL);    // two characters
    TEST_STRING32("a", 0x9747b28cULL, 0x7FA09EA6ULL);     // one character

    // Endian order within the chunks
    TEST_STRING32("abcd", 0x9747b28cULL, 0xF0478627ULL);  // one full chunk
    TEST_STRING32("abc", 0x9747b28cULL, 0xC84A62DDULL);
    TEST_STRING32("ab", 0x9747b28cULL, 0x74875592ULL);
    TEST_STRING32("a", 0x9747b28cULL, 0x7FA09EA6ULL);

    TEST_STRING32("Hello, world!", 0x9747b28cULL, 0x24884CBAULL);

    // Make sure you handle UTF-8 high characters. A bcrypt implementation messed this up
    TEST_STRING32("ππππππππ", 0x9747b28cULL, 0xD58063C1ULL);  // U+03C0: Greek Small Letter Pi

    // String of 256 characters.
    // Make sure you don't store string lengths in a char, and overflow at 255 bytes (as OpenBSD's
    // canonical BCrypt implementation did)
    TEST_STRING32(std::string(256, 'a'), 0x9747b28cULL, 0x37405BDCULL);
}


TEST(MurmurHash3, TestVectors128) {
    TEST_STRING128("", 0, 0ULL, 0ULL);

    TEST_STRING128("", 1ULL, 5048724184180415669ULL, 5864299874987029891ULL);
    // Make sure seed value is handled unsigned.
    TEST_STRING128("", 0xffffffffULL, 7706185961851046380ULL, 9616347466054386795ULL);
    // Make sure we handle embedded nulls.
    TEST_STRING128("\0\0\0\0"_sd, 0ULL, 14961230494313510588ULL, 6383328099726337777ULL);

    // One full chunk.
    TEST_STRING128("aaaa", 0x9747b28cULL, 13033599803469372400ULL, 11949150323828610719ULL);
    // Three characters.
    TEST_STRING128("aaa", 0x9747b28cULL, 10278871841506805355ULL, 17952965428487426844ULL);
    // Two characters.
    TEST_STRING128("aa", 0x9747b28cULL, 1343929393636293407ULL, 16804672932933964801ULL);
    // One character.
    TEST_STRING128("a", 0x9747b28cULL, 6694838689256856093ULL, 11415968713816993796ULL);

    // Endian order within the chunks
    TEST_STRING128("abcd", 0x9747b28cULL, 5310993687375067025ULL, 9979528070057666491ULL);
    TEST_STRING128("abc", 0x9747b28cULL, 3982135406228655836ULL, 14835035517329147071ULL);
    TEST_STRING128("ab", 0x9747b28cULL, 9526501539032868875ULL, 9131386788375312171ULL);
    TEST_STRING128("a", 0x9747b28cULL, 6694838689256856093ULL, 11415968713816993796ULL);

    TEST_STRING128(
        "Hello, world!", 0x9747b28cULL, 17132966038248896814ULL, 17896881015324243642ULL);

    // Make sure to handle UTF-8 high characters. A bcrypt implementation messed this up. Here we
    // use U+03C0: Greek Small Letter Pi.
    TEST_STRING128("ππππππππ", 0x9747b28cULL, 10874605236735318559ULL, 17921841414653337979ULL);

    // String of 256 characters. Make sure you don't store string lengths in a char, and overflow at
    // 255 bytes (as OpenBSD's canonical BCrypt implementation did).
    TEST_STRING128(
        std::string(256, 'a'), 0x9747b28cULL, 557766291455132100ULL, 14184293241195392597ULL);
}

// Output of the 64-bit version of murmur3() should be the same as the first 8 bytes of the 128-bit
// version.
TEST(MurmurHash3, TestVectors64) {
    TEST_STRING64("", 0, 0ULL);

    TEST_STRING64("", 1ULL, 5048724184180415669ULL);
    // Make sure seed value is handled unsigned.
    TEST_STRING64("", 0xffffffffULL, 7706185961851046380ULL);
    // Make sure we handle embedded nulls.
    TEST_STRING64("\0\0\0\0"_sd, 0ULL, 14961230494313510588ULL);

    // One full chunk.
    TEST_STRING64("aaaa", 0x9747b28cULL, 13033599803469372400ULL);
    // Three characters.
    TEST_STRING64("aaa", 0x9747b28cULL, 10278871841506805355ULL);
    // Two characters.
    TEST_STRING64("aa", 0x9747b28cULL, 1343929393636293407ULL);
    // One character.
    TEST_STRING64("a", 0x9747b28cULL, 6694838689256856093ULL);

    // Endian order within the chunks
    TEST_STRING64("abcd", 0x9747b28cULL, 5310993687375067025ULL);
    TEST_STRING64("abc", 0x9747b28cULL, 3982135406228655836ULL);
    TEST_STRING64("ab", 0x9747b28cULL, 9526501539032868875ULL);
    TEST_STRING64("a", 0x9747b28cULL, 6694838689256856093ULL);

    TEST_STRING64("Hello, world!", 0x9747b28cULL, 17132966038248896814ULL);

    // Make sure to handle UTF-8 high characters. A bcrypt implementation messed this up. Here we
    // use U+03C0: Greek Small Letter Pi.
    TEST_STRING64("ππππππππ", 0x9747b28cULL, 10874605236735318559ULL);

    // String of 256 characters. Make sure you don't store string lengths in a char, and overflow at
    // 255 bytes (as OpenBSD's canonical BCrypt implementation did).
    TEST_STRING64(std::string(256, 'a'), 0x9747b28cULL, 557766291455132100ULL);
}

}  // namespace
}  // namespace mongo
