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

#include "mongo/platform/random.h"
#include "mongo/unittest/unittest.h"

#include <string_view>

namespace mongo {
namespace {
using namespace std::literals::string_view_literals;

constexpr std::string_view kData = "abacabadabacaba"sv;

class SorterChecksumCalculatorTest : public testing::TestWithParam<SorterChecksumVersion> {};

INSTANTIATE_TEST_SUITE_P(AllChecksumVersions,
                         SorterChecksumCalculatorTest,
                         testing::Values(SorterChecksumVersion::v1, SorterChecksumVersion::v2));

TEST(SorterChecksumCalculatorTest, CollisionCheck) {
    static constexpr size_t kTestCount = 1000000;

    for (auto version : {SorterChecksumVersion::v1, SorterChecksumVersion::v2}) {
        SorterChecksumCalculator calculator(version);
        absl::flat_hash_set<size_t> seenValues;
        seenValues.insert(calculator.checksum());
        for (size_t i = 0; i < kTestCount; ++i) {
            calculator.addData(kData.data(), kData.size());
            size_t checksum = calculator.checksum();
            EXPECT_FALSE(seenValues.contains(checksum)) << "version: " << idl::serialize(version);
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
            EXPECT_NE(expectedChecksum, calculator.checksum())
                << "version: " << idl::serialize(version);
        }
    }
}

TEST_P(SorterChecksumCalculatorTest, Seed) {
    size_t seed = 1;
    SorterChecksumCalculator calculator{GetParam(), seed};
    EXPECT_EQ(calculator.checksum(), seed);
}

TEST_P(SorterChecksumCalculatorTest, AddUncommittedDataDoesNotAffectCommittedChecksum) {
    SorterChecksumCalculator calculator{GetParam()};
    size_t initial = calculator.checksum();
    calculator.addUncommittedData(kData.data(), kData.size());
    EXPECT_EQ(calculator.checksum(), initial);
}

TEST_P(SorterChecksumCalculatorTest, CommitMatchesAddData) {
    SorterChecksumCalculator viaAddData{GetParam()};
    viaAddData.addData(kData.data(), kData.size());

    SorterChecksumCalculator viaUncommitted{GetParam()};
    viaUncommitted.addUncommittedData(kData.data(), kData.size());
    viaUncommitted.commit();

    EXPECT_EQ(viaUncommitted.checksum(), viaAddData.checksum());
}

TEST_P(SorterChecksumCalculatorTest, AbortDiscardsPendingBytes) {
    SorterChecksumCalculator calculator{GetParam()};
    size_t initial = calculator.checksum();
    calculator.addUncommittedData(kData.data(), kData.size());
    calculator.abort();
    EXPECT_EQ(calculator.checksum(), initial);
}

TEST_P(SorterChecksumCalculatorTest, AbortThenReaddThenCommitMatchesSingleAdd) {
    SorterChecksumCalculator expected{GetParam()};
    expected.addData(kData.data(), kData.size());

    SorterChecksumCalculator retried{GetParam()};
    retried.addUncommittedData(kData.data(), kData.size());
    retried.abort();
    retried.addUncommittedData(kData.data(), kData.size());
    retried.commit();

    EXPECT_EQ(retried.checksum(), expected.checksum());
}

TEST_P(SorterChecksumCalculatorTest, MultipleUncommittedAddsAccumulate) {
    constexpr std::string_view kPart1 = "abacaba"sv;
    constexpr std::string_view kPart2 = "dabacaba"sv;

    SorterChecksumCalculator expected{GetParam()};
    expected.addData(kPart1.data(), kPart1.size());
    expected.addData(kPart2.data(), kPart2.size());

    SorterChecksumCalculator twoPart{GetParam()};
    twoPart.addUncommittedData(kPart1.data(), kPart1.size());
    twoPart.addUncommittedData(kPart2.data(), kPart2.size());
    twoPart.commit();

    EXPECT_EQ(twoPart.checksum(), expected.checksum());
}

TEST_P(SorterChecksumCalculatorTest, CommitAfterPriorCommitContinuesFromCommittedState) {
    SorterChecksumCalculator expected{GetParam()};
    expected.addData(kData.data(), kData.size());
    expected.addData(kData.data(), kData.size());

    SorterChecksumCalculator stepwise{GetParam()};
    stepwise.addUncommittedData(kData.data(), kData.size());
    stepwise.commit();
    stepwise.addUncommittedData(kData.data(), kData.size());
    stepwise.commit();

    EXPECT_EQ(stepwise.checksum(), expected.checksum());
}

TEST_P(SorterChecksumCalculatorTest, CommitWithNothingPendingIsNoop) {
    SorterChecksumCalculator calculator{GetParam()};
    calculator.addData(kData.data(), kData.size());
    size_t committed = calculator.checksum();
    calculator.commit();
    EXPECT_EQ(calculator.checksum(), committed);
}

TEST_P(SorterChecksumCalculatorTest, AbortWithNothingPendingIsNoop) {
    SorterChecksumCalculator calculator{GetParam()};
    calculator.addData(kData.data(), kData.size());
    size_t committed = calculator.checksum();
    calculator.abort();
    EXPECT_EQ(calculator.checksum(), committed);
}

TEST_P(SorterChecksumCalculatorTest, SeedFlowsIntoUncommittedState) {
    constexpr size_t kSeed = 0xdeadbeef;
    SorterChecksumCalculator expected{GetParam(), kSeed};
    expected.addData(kData.data(), kData.size());

    SorterChecksumCalculator calculator{GetParam(), kSeed};
    calculator.addUncommittedData(kData.data(), kData.size());
    calculator.commit();

    EXPECT_EQ(calculator.checksum(), expected.checksum());
}

TEST_P(SorterChecksumCalculatorTest, CommitAndAbortBeforeAnyDataAreNoops) {
    constexpr size_t kSeed = 0xdeadbeef;
    SorterChecksumCalculator calculator{GetParam(), kSeed};
    calculator.commit();
    EXPECT_EQ(calculator.checksum(), kSeed);
    calculator.abort();
    EXPECT_EQ(calculator.checksum(), kSeed);
}

TEST_P(SorterChecksumCalculatorTest, RepeatedCommitWithPendingIsNoop) {
    SorterChecksumCalculator twice{GetParam()};
    twice.addUncommittedData(kData.data(), kData.size());
    twice.commit();
    twice.commit();

    SorterChecksumCalculator once{GetParam()};
    once.addUncommittedData(kData.data(), kData.size());
    once.commit();

    EXPECT_EQ(twice.checksum(), once.checksum());
}

TEST_P(SorterChecksumCalculatorTest, RepeatedAbortWithPendingIsNoop) {
    SorterChecksumCalculator calculator{GetParam()};
    size_t initial = calculator.checksum();
    calculator.addUncommittedData(kData.data(), kData.size());
    calculator.abort();
    calculator.abort();
    EXPECT_EQ(calculator.checksum(), initial);
}

TEST_P(SorterChecksumCalculatorTest, AddDataAfterCommitContinuesFromCommittedState) {
    SorterChecksumCalculator expected{GetParam()};
    expected.addData(kData.data(), kData.size());
    expected.addData(kData.data(), kData.size());

    SorterChecksumCalculator calculator{GetParam()};
    calculator.addUncommittedData(kData.data(), kData.size());
    calculator.commit();
    calculator.addData(kData.data(), kData.size());

    EXPECT_EQ(calculator.checksum(), expected.checksum());
}

TEST_P(SorterChecksumCalculatorTest, AddDataAfterAbortBehavesAsSingleAdd) {
    SorterChecksumCalculator expected{GetParam()};
    expected.addData(kData.data(), kData.size());

    SorterChecksumCalculator calculator{GetParam()};
    calculator.addUncommittedData(kData.data(), kData.size());
    calculator.abort();
    calculator.addData(kData.data(), kData.size());

    EXPECT_EQ(calculator.checksum(), expected.checksum());
}

TEST_P(SorterChecksumCalculatorTest, ZeroLengthAddUncommittedDataThenAbort) {
    SorterChecksumCalculator calculator{GetParam()};
    size_t initial = calculator.checksum();
    calculator.addUncommittedData(kData.data(), 0);
    calculator.abort();
    EXPECT_EQ(calculator.checksum(), initial);
}

TEST_P(SorterChecksumCalculatorTest, AddDataWithPendingUncommittedDataDiscardsPending) {
    // addData() with uncommitted bytes pending behaves identically to abort() then addData().
    SorterChecksumCalculator viaImplicit{GetParam()};
    viaImplicit.addUncommittedData(kData.data(), kData.size());
    viaImplicit.addData(kData.data(), kData.size());

    SorterChecksumCalculator viaExplicit{GetParam()};
    viaExplicit.addUncommittedData(kData.data(), kData.size());
    viaExplicit.abort();
    viaExplicit.addData(kData.data(), kData.size());

    EXPECT_EQ(viaImplicit.checksum(), viaExplicit.checksum());
}

}  // namespace
}  // namespace mongo
