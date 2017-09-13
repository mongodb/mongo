/**
 *    Copyright (C) 2016 MongoDB Inc.
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
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include <cstring>
#include <iterator>
#include <numeric>

#include "mongo/db/fts/unicode/byte_vector.h"
#include "mongo/unittest/unittest.h"

#ifdef MONGO_HAVE_FAST_BYTE_VECTOR
namespace mongo {
namespace unicode {

TEST(ByteVector, LoadStoreUnaligned) {
    uint8_t inputBuf[ByteVector::size * 2];
    uint8_t outputBuf[ByteVector::size * 2];
    std::iota(std::begin(inputBuf), std::end(inputBuf), 0);

    // Try loads and stores at all possible (mis)alignments.
    for (size_t offset = 0; offset < ByteVector::size; offset++) {
        std::memset(outputBuf, 0, sizeof(outputBuf));
        ByteVector::load(inputBuf + offset).store(outputBuf + offset);

        for (size_t i = 0; i < ByteVector::size; i++) {
            ASSERT_EQ(outputBuf[offset + i], inputBuf[offset + i]);
        }
    }
}

TEST(ByteVector, Splat) {
    uint8_t outputBuf[ByteVector::size] = {};
    ByteVector(0x12).store(outputBuf);

    for (size_t i = 0; i < ByteVector::size; i++) {
        ASSERT_EQ(outputBuf[i], 0x12);
    }
}

TEST(ByteVector, MaskAny) {
    uint8_t inputBuf[ByteVector::size];
    std::memset(inputBuf, 0xFF, sizeof(inputBuf));
    for (size_t offset = 0; offset <= ByteVector::size; offset++) {
        auto mask = ByteVector::load(inputBuf).maskAny();
        ASSERT_EQ(ByteVector::countInitialZeros(mask), offset);
        if (offset < ByteVector::size) {
            inputBuf[offset] = 0;  // Add an initial 0 for the next loop.
        }
    }
}

TEST(ByteVector, MaskHigh) {
    uint8_t inputBuf[ByteVector::size];
    std::memset(inputBuf, 0x80, sizeof(inputBuf));
    for (size_t offset = 0; offset <= ByteVector::size; offset++) {
        auto mask = ByteVector::load(inputBuf).maskHigh();
        ASSERT_EQ(ByteVector::countInitialZeros(mask), offset);
        if (offset < ByteVector::size) {
            inputBuf[offset] = 0x7f;  // Add an initial 0 bit for the next loop.
        }
    }
}

TEST(ByteVector, CompareEQ) {
    uint8_t inputBuf[ByteVector::size];
    uint8_t outputBuf[ByteVector::size] = {};
    std::iota(std::begin(inputBuf), std::end(inputBuf), 0);

    ByteVector::load(inputBuf).compareEQ(3).store(outputBuf);

    for (size_t i = 0; i < ByteVector::size; i++) {
        ASSERT_EQ(outputBuf[i], inputBuf[i] == 3 ? 0xFF : 0x00);
    }
}

TEST(ByteVector, CompareGT) {
    uint8_t inputBuf[ByteVector::size];
    uint8_t outputBuf[ByteVector::size] = {};
    std::iota(std::begin(inputBuf), std::end(inputBuf), 0);

    ByteVector::load(inputBuf).compareGT(3).store(outputBuf);

    for (size_t i = 0; i < ByteVector::size; i++) {
        ASSERT_EQ(outputBuf[i], inputBuf[i] > 3 ? 0xFF : 0x00);
    }
}

TEST(ByteVector, CompareLT) {
    uint8_t inputBuf[ByteVector::size];
    uint8_t outputBuf[ByteVector::size] = {};
    std::iota(std::begin(inputBuf), std::end(inputBuf), 0);

    ByteVector::load(inputBuf).compareLT(3).store(outputBuf);

    for (size_t i = 0; i < ByteVector::size; i++) {
        ASSERT_EQ(outputBuf[i], inputBuf[i] < 3 ? 0xFF : 0x00);
    }
}

TEST(ByteVector, BitOr) {
    uint8_t inputBuf[ByteVector::size];
    uint8_t outputBuf[ByteVector::size] = {};
    std::iota(std::begin(inputBuf), std::end(inputBuf), 0);

    (ByteVector::load(inputBuf) | ByteVector(0x1)).store(outputBuf);

    for (size_t i = 0; i < ByteVector::size; i++) {
        ASSERT_EQ(outputBuf[i], inputBuf[i] | 1);
    }
}

TEST(ByteVector, BitOrAssign) {
    uint8_t inputBuf[ByteVector::size];
    uint8_t outputBuf[ByteVector::size] = {};
    std::iota(std::begin(inputBuf), std::end(inputBuf), 0);

    auto vec = ByteVector::load(inputBuf);
    vec |= ByteVector(2);
    vec.store(outputBuf);

    for (size_t i = 0; i < ByteVector::size; i++) {
        ASSERT_EQ(outputBuf[i], inputBuf[i] | 2);
    }
}

TEST(ByteVector, BitAnd) {
    uint8_t inputBuf[ByteVector::size];
    uint8_t outputBuf[ByteVector::size] = {};
    std::iota(std::begin(inputBuf), std::end(inputBuf), 0);

    (ByteVector::load(inputBuf) & ByteVector(2)).store(outputBuf);

    for (size_t i = 0; i < ByteVector::size; i++) {
        ASSERT_EQ(outputBuf[i], inputBuf[i] & 2);
    }
}

TEST(ByteVector, BitAndAssign) {
    uint8_t inputBuf[ByteVector::size];
    uint8_t outputBuf[ByteVector::size] = {};
    std::iota(std::begin(inputBuf), std::end(inputBuf), 0);

    auto vec = ByteVector::load(inputBuf);
    vec &= ByteVector(2);
    vec.store(outputBuf);

    for (size_t i = 0; i < ByteVector::size; i++) {
        ASSERT_EQ(outputBuf[i], inputBuf[i] & 2);
    }
}

}  // namespace unicode
}  // namespace mongo
#else
// Our unittest framework gets angry if there are no tests. If we don't have ByteVector, give it a
// dummy test to make it happy.
TEST(ByteVector, ByteVectorNotSupportedOnThisPlatform) {}
#endif
