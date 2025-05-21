/**
 *    Copyright (C) 2023-present MongoDB, Inc.
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

#include "mongo/db/sorter/sorter_checksum_calculator.h"

#include "mongo/base/string_data.h"
#include "mongo/platform/random.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

constexpr StringData kData = "abacabadabacaba"_sd;

TEST(SorterChecksumCalculatorTest, CollisionCheck) {
    static constexpr size_t kTestCount = 1000000;

    for (auto version : {SorterChecksumVersion::v1, SorterChecksumVersion::v2}) {
        SorterChecksumCalculator calculator(version);
        absl::flat_hash_set<size_t> seenValues;
        seenValues.insert(calculator.checksum());
        for (size_t i = 0; i < kTestCount; ++i) {
            calculator.addData(kData.data(), kData.size());
            size_t checksum = calculator.checksum();
            ASSERT_FALSE(seenValues.contains(checksum))
                << "version: " << SorterChecksumVersion_serializer(version);
            seenValues.insert(checksum);
        }
    }
}

TEST(SorterChecksumCalculatorTest, RandomBitFlips) {
    for (auto version : {SorterChecksumVersion::v1, SorterChecksumVersion::v2}) {
        SorterChecksumCalculator fullCalculator(version);
        fullCalculator.addData(kData.data(), kData.size());
        size_t expectedChecksum = fullCalculator.checksum();

        PseudoRandom random{static_cast<uint64_t>(expectedChecksum)};

        for (size_t count = 1; count <= 8; ++count) {
            std::string data(kData.begin(), kData.end());
            size_t totalBits = data.size() * CHAR_BIT;

            absl::flat_hash_set<size_t> flippedBits;
            for (size_t i = 0; i < count; ++i) {
                size_t bit;
                do {
                    bit = random.nextInt64(totalBits);
                } while (flippedBits.contains(bit));
                flippedBits.insert(bit);
                data[bit / CHAR_BIT] ^= (1 << (bit % CHAR_BIT));
            }

            SorterChecksumCalculator calculator(version);
            calculator.addData(data.data(), data.size());
            ASSERT_NE(expectedChecksum, calculator.checksum())
                << "version: " << SorterChecksumVersion_serializer(version);
        }
    }
}

}  // namespace
}  // namespace mongo
