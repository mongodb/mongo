// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/compiler/stats/space_saving.h"

#include "mongo/db/exec/sbe/values/value.h"
#include "mongo/db/query/compiler/stats/value_utils.h"
#include "mongo/unittest/unittest.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <random>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace mongo::stats {
namespace {

// The sketch threads raw pointers through its pool vectors, so moving it would leave the
// moved-from sketch pointing into pools the destination owns. Moving must stay deleted.
static_assert(!std::is_move_constructible_v<SpaceSavingSketch>);
static_assert(!std::is_move_assignable_v<SpaceSavingSketch>);

SBEValue intValue(int32_t v) {
    return SBEValue{makeInt32Value(v)};
}

SBEValue stringValue(const char* s) {
    return SBEValue{sbe::value::makeNewString(s)};
}

SBEValue decimalValue(const char* s) {
    return SBEValue{sbe::value::makeCopyDecimal(Decimal128(s))};
}

// True frequency of each distinct value, computed independently of the algorithm under test.
std::vector<std::pair<SBEValue, size_t>> trueFrequencies(std::vector<SBEValue> input) {
    sortValueVector(input);
    std::vector<std::pair<SBEValue, size_t>> result;
    for (const auto& value : input) {
        if (result.empty() || !(result.back().first == value)) {
            result.emplace_back(SBEValue{value}, 1);
        } else {
            ++result.back().second;
        }
    }
    return result;
}

size_t trueFreqOf(const std::vector<std::pair<SBEValue, size_t>>& freqs, const SBEValue& value) {
    for (const auto& [v, freq] : freqs) {
        if (v == value) {
            return freq;
        }
    }
    return 0;
}

TEST(SpaceSavingTest, TopKSmallKnownInput) {
    // 11 distinct values with k = kMinNumCounters forces one eviction. Values 1 and 2 are
    // monitored with counts 2 and 3; when the 11th distinct value arrives, the minimum-count
    // bucket's tail (value 3) is reused with estimate 2 (evicted count 1 plus 1) and error 1.
    std::vector<SBEValue> input = {intValue(1),
                                   intValue(1),
                                   intValue(2),
                                   intValue(2),
                                   intValue(2),
                                   intValue(3),
                                   intValue(4),
                                   intValue(5),
                                   intValue(6),
                                   intValue(7),
                                   intValue(8),
                                   intValue(9),
                                   intValue(10),
                                   intValue(11)};
    const auto result = MostCommonValues::make(input, kMinNumCounters);
    ASSERT_EQ(result._entries.size(), kMinNumCounters);

    ASSERT_EQ(result._entries[0].value, intValue(2));
    ASSERT_EQ(result._entries[0].estFreq, 3);
    ASSERT_EQ(result._entries[0].err, 0);
    ASSERT_EQ(result._entries[1].value, intValue(1));
    ASSERT_EQ(result._entries[1].estFreq, 2);
    ASSERT_EQ(result._entries[1].err, 0);
    ASSERT_EQ(result._entries[2].value, intValue(11));
    ASSERT_EQ(result._entries[2].estFreq, 2);
    ASSERT_EQ(result._entries[2].err, 1);
    for (size_t i = 3; i < result._entries.size(); ++i) {
        ASSERT_EQ(result._entries[i].estFreq, 1);
        ASSERT_EQ(result._entries[i].err, 0);
    }
}

TEST(SpaceSavingTest, KGreaterThanDistinctCountReturnsAllValuesExactly) {
    // With more counters than distinct values no counter is evicted, so all values are
    // returned with exact counts and no error.
    std::vector<SBEValue> input = {
        intValue(1), intValue(1), intValue(2), intValue(3), intValue(3), intValue(3), intValue(3)};
    const auto freqs = trueFrequencies(input);
    const auto result = MostCommonValues::make(input, kMinNumCounters);
    ASSERT_EQ(result._entries.size(), freqs.size());
    for (const auto& entry : result._entries) {
        ASSERT_EQ(entry.estFreq, trueFreqOf(freqs, entry.value));
        ASSERT_EQ(entry.err, 0);
    }
}

TEST(SpaceSavingTest, PaperExampleEvictionErrorTerm) {
    // Stream: values 1..kMinNumCounters each 3 times, then an 11th value once, with
    // k = kMinNumCounters. All counters reach count 3; when the 11th value arrives, the
    // minimum-count bucket's tail is the least recently updated counter (value 1), which is
    // reused with estimate 4 and error 3.
    std::vector<SBEValue> input;
    for (int32_t i = 1; i <= static_cast<int32_t>(kMinNumCounters); ++i) {
        input.insert(input.end(), 3, intValue(i));
    }
    input.push_back(intValue(kMinNumCounters + 1));
    const auto result = MostCommonValues::make(input, kMinNumCounters);
    ASSERT_EQ(result._entries.size(), kMinNumCounters);

    ASSERT_EQ(result._entries[0].value, intValue(kMinNumCounters + 1));
    ASSERT_EQ(result._entries[0].estFreq, 4);
    ASSERT_EQ(result._entries[0].err, 3);
    size_t cumulativeFreq = result._entries[0].estFreq;
    for (size_t i = 1; i < result._entries.size(); ++i) {
        ASSERT_EQ(result._entries[i].estFreq, 3);
        ASSERT_EQ(result._entries[i].err, 0);
        cumulativeFreq += result._entries[i].estFreq;
    }

    // Each input adds exactly one to the total estimate mass (an eviction carries the evicted count
    // into the reused counter), so the estimates sum to the input size.
    ASSERT_EQ(result._inputSize, 31);
    ASSERT_EQ(cumulativeFreq, result._inputSize);
}

TEST(SpaceSavingTest, DeterministicOutput) {
    std::vector<SBEValue> input = {intValue(1),
                                   intValue(2),
                                   intValue(1),
                                   intValue(3),
                                   intValue(2),
                                   intValue(1),
                                   stringValue("a"),
                                   stringValue("a")};
    const auto first = MostCommonValues::make(input, 3);
    const auto second = MostCommonValues::make(input, 3);
    ASSERT_EQ(first.toString(), second.toString());
    ASSERT_EQ(first._entries.size(), second._entries.size());
    for (size_t i = 0; i < first._entries.size(); ++i) {
        ASSERT_EQ(first._entries[i].estFreq, second._entries[i].estFreq);
        ASSERT_EQ(first._entries[i].err, second._entries[i].err);
        ASSERT_TRUE(first._entries[i].value == second._entries[i].value);
    }
}

TEST(SpaceSavingTest, MixedTypes) {
    std::vector<SBEValue> input = {intValue(1),
                                   stringValue("a"),
                                   intValue(1),
                                   stringValue("a"),
                                   SBEValue{sbe::value::TypeTags::Boolean, true},
                                   makeNullValue()};
    const auto result = MostCommonValues::make(input, 6);
    ASSERT_EQ(result._entries.size(), 4);

    // No eviction: exact counts. The two count-2 entries tie and are ordered by
    // BSON value order, where numbers sort before strings.
    ASSERT_EQ(result._entries[0].value, intValue(1));
    ASSERT_EQ(result._entries[0].estFreq, 2);
    ASSERT_EQ(result._entries[0].err, 0);
    ASSERT_EQ(result._entries[1].value, stringValue("a"));
    ASSERT_EQ(result._entries[1].estFreq, 2);
    ASSERT_EQ(result._entries[1].err, 0);
    for (size_t i = 2; i < result._entries.size(); ++i) {
        ASSERT_EQ(result._entries[i].estFreq, 1);
        ASSERT_EQ(result._entries[i].err, 0);
    }
}

TEST(SpaceSavingTest, SmallKIsClampedToMinimum) {
    // A 'k' below kMinNumCounters is clamped up to it, so 30 distinct values fill exactly
    // kMinNumCounters counters.
    std::vector<SBEValue> input;
    for (int32_t i = 0; i < 30; ++i) {
        input.push_back(intValue(i));
    }
    SpaceSavingSketch sketch(1);
    for (const auto& value : input) {
        sketch.increment(value);
    }
    ASSERT_EQ(sketch.numCounters(), kMinNumCounters);
}

TEST(SpaceSavingTest, EmptyInput) {
    const auto result = MostCommonValues::make({}, 3);
    ASSERT_EQ(result._entries.size(), 0);
}

TEST(SpaceSavingTest, ZipfianStreamErrorTermInvariant) {
    // 20 distinct values with skewed frequencies (value i occurs 20 - i times), shuffled so
    // that evictions occur. Whatever the eviction pattern, every returned entry must satisfy
    // the paper's guarantee: true freq in [estFreq - err, estFreq].
    std::vector<SBEValue> input;
    for (int32_t i = 0; i < 20; ++i) {
        input.insert(input.end(), 20 - i, intValue(i));
    }
    std::mt19937_64 gen(42);
    std::shuffle(input.begin(), input.end(), gen);

    const size_t k = kMinNumCounters;
    const auto freqs = trueFrequencies(input);
    const auto result = MostCommonValues::make(input, k);
    ASSERT_EQ(result._entries.size(), k);

    for (const auto& entry : result._entries) {
        const size_t trueFreq = trueFreqOf(freqs, entry.value);
        ASSERT_GTE(entry.estFreq, trueFreq);
        ASSERT_LTE(entry.estFreq - entry.err, trueFreq);
    }

    // Entries are sorted by estimated frequency descending.
    for (size_t i = 1; i < result._entries.size(); ++i) {
        ASSERT_GTE(result._entries[i - 1].estFreq, result._entries[i].estFreq);
    }
}

TEST(SpaceSavingTest, EvictionWithHeapAllocatedTypes) {
    // Long strings (StringBig), decimals, and ObjectIds have heap-allocated storage, so
    // eviction must release the evicted value, deep-copy the newcomer, and re-key the hash
    // table. Nine strings are monitored with exact counts; the decimal's counter is evicted
    // by the ObjectId (estimate 2, error 1); and when the decimal re-appears it must evict
    // the least recently observed string's counter rather than
    // on the counter the ObjectId reused. The re-apearing decimal tests the correctness of
    // the hash table erase for heap-allocated types.
    std::vector<SBEValue> input;
    for (int32_t i = 0; i < 9; ++i) {
        // 64 characters exceed the small-string limit, so these are stored as StringBig.
        input.insert(input.end(), 2, stringValue(std::string(64, 'a' + i).c_str()));
    }
    const auto decimal = decimalValue("1.5");
    input.push_back(decimal);
    sbe::value::ObjectIdType bytes;
    bytes.fill(1);
    input.push_back(SBEValue{sbe::value::makeCopyObjectId(bytes)});
    input.push_back(decimal);

    const auto result = MostCommonValues::make(input, kMinNumCounters);
    ASSERT_EQ(result._entries.size(), kMinNumCounters);

    // The re-appeared decimal took over the evicted 'a'-string's counter.
    ASSERT_EQ(result._entries[0].value, decimalValue("1.5"));
    ASSERT_EQ(result._entries[0].estFreq, 3);
    ASSERT_EQ(result._entries[0].err, 2);
    // The ObjectId reused the decimal's first counter. It is placed after the strings with
    // the same counter value, following the BSON canonical type order.
    ASSERT_EQ(result._entries[9].value, SBEValue{sbe::value::makeCopyObjectId(bytes)});
    ASSERT_EQ(result._entries[9].estFreq, 2);
    ASSERT_EQ(result._entries[9].err, 1);
    // The surviving strings kept exact counts; the 'a'-string was evicted.
    for (int32_t i = 1; i < 9; ++i) {
        ASSERT_EQ(result._entries[i].value, stringValue(std::string(64, 'a' + i).c_str()));
        ASSERT_EQ(result._entries[i].estFreq, 2);
        ASSERT_EQ(result._entries[i].err, 0);
    }

    // The deep copies in the reused counters print correctly.
    const auto str = result.toString();
    ASSERT_NE(str.find(std::string(64, 'b')), std::string::npos);
    ASSERT_NE(str.find("1.5"), std::string::npos);
    ASSERT_NE(str.find("ObjectId("), std::string::npos);
}

TEST(SpaceSavingTest, ToStringVariousTypes) {
    // toString() must handle every type the sketch accepts.
    const std::vector<SBEValue> input = {
        SBEValue{sbe::value::TypeTags::Date, sbe::value::bitcastFrom<int64_t>(1000)},
        SBEValue{sbe::value::TypeTags::MinKey, 0},
        SBEValue{sbe::value::TypeTags::NumberDouble, sbe::value::bitcastFrom<double>(2.5)}};
    const auto result = MostCommonValues::make(input, kMinNumCounters);
    ASSERT_EQ(result._entries.size(), 3);
    const auto str = result.toString();
    ASSERT_NE(str.find("1000"), std::string::npos);
    ASSERT_NE(str.find("minKey"), std::string::npos);
    ASSERT_NE(str.find("2.5"), std::string::npos);
}

TEST(SpaceSavingTest, CumulativeFrequency) {
    // Estimates sum to 19, guaranteed lower bounds (10 - 2 = 8, 9 - 5 = 4) sum to 12.
    const auto result =
        MostCommonValues{{MCVEntry{intValue(1), 10, 2}, MCVEntry{intValue(2), 9, 5}}};
    ASSERT_EQ(result.getCumulativeLowBoundFrequency(), 12);

    // The percentage is derived in toString(): 12/200.
    auto labeled = result;
    labeled._inputSize = 200;
    const auto str = labeled.toString();
    ASSERT_NE(str.find("cumulativeGuaranteedFrequency: 12 (6.0%)"), std::string::npos);
}

// Tests for the help function getGuaranteedTopK().
TEST(SpaceSavingTest, GuaranteedTopKExactCounts) {
    // All error terms are 0, so all estimates are exact and
    // the whole prefix is guaranteed.
    const auto result = MostCommonValues{
        {MCVEntry{intValue(1), 10, 0}, MCVEntry{intValue(2), 8, 0}, MCVEntry{intValue(3), 5, 0}}};
    ASSERT_EQ(result.getGuaranteedTopK(), 3);
}

TEST(SpaceSavingTest, GuaranteedTopKEmptyWhenNoEntryQualifies) {
    // The paper's k = 3 eviction example: the newcomer  has a lower
    // bound of 1 (estimate 4 - error 3), which does not exceed the other estimates, so nothing is
    // guaranteed.
    const auto result = MostCommonValues{
        {MCVEntry{intValue(4), 4, 3}, MCVEntry{intValue(2), 3, 0}, MCVEntry{intValue(3), 3, 0}}};
    ASSERT_EQ(result.getGuaranteedTopK(), 0);
}

TEST(SpaceSavingTest, GuaranteedTopKResumesAfterWeakEntry) {
    // p = 2 is invalid (lower bound of entry 1 is 7, not > 7), but p = 3 is valid: both
    // lower bounds (7 and 7) strictly exceed the estimate 6 of entry 3. Entries 1 and 2
    // are counted.
    const auto result = MostCommonValues{
        {MCVEntry{intValue(1), 10, 3}, MCVEntry{intValue(2), 7, 0}, MCVEntry{intValue(3), 6, 0}}};
    ASSERT_EQ(result.getGuaranteedTopK(), 2);
}
}  // namespace
}  // namespace mongo::stats
