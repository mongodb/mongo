/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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

#include "mongo/base/string_data.h"
#include "mongo/config.h"  // IWYU pragma: keep
#include "mongo/db/service_context_d_test_fixture.h"
#include "mongo/db/sorter/file_based_spiller.h"
#include "mongo/db/sorter/sorter_template_defs.h"
#include "mongo/db/sorter/sorter_test_utils.h"
#include "mongo/db/sorter/typed_sorter_test_utils.h"
#include "mongo/logv2/log.h"
#include "mongo/platform/random.h"
#include "mongo/stdx/thread.h"
#include "mongo/unittest/unittest.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <numeric>
#include <queue>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

#include <boost/filesystem/operations.hpp>
#include <fmt/format.h>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest

namespace mongo::sorter {
namespace {

namespace SorterTests {

using test::ContainerTraits;
using test::FileTraits;

template <typename Traits>
constexpr bool shouldSkipContainerBasedTestInDebugBuild() {
#if defined(MONGO_CONFIG_DEBUG_BUILD)
    return std::is_same_v<Traits, ContainerTraits>;
#else
    return false;
#endif
}

class SortedFileWriterAndFileIteratorTests : public unittest::Test {
protected:
    int appendToFile(const SortOptions& opts,
                     const boost::filesystem::path& spillDir,
                     SorterFileStats* sorterFileStats,
                     int currentFileSize,
                     int range) const {
        auto makeFile = [&] {
            return std::make_shared<SorterFile>(sorter::nextFileName(spillDir), sorterFileStats);
        };

        int currentBufSize = 0;
        // TODO(SERVER-114080): Ensure testing of non-file-based sorter storage is comprehensive.
        FileBasedSorterStorage<IntWrapper, IntWrapper> sorterStorage(
            makeFile(),
            /*dbName=*/boost::none,
            sorter::kLatestChecksumVersion);
        std::unique_ptr<SortedStorageWriter<IntWrapper, IntWrapper>> sorter =
            sorterStorage.makeWriter(opts, /*settings=*/{});
        for (int i = 0; i < range; ++i) {
            sorter->addAlreadySorted(i, -i);
            currentBufSize += sizeof(i) + sizeof(-i);

            if (currentBufSize > static_cast<int>(sorter::kSortedFileBufferSize)) {
                // File size only increases if buffer size exceeds limit and spills. Each spill
                // includes the buffer and the size of the spill.
                currentFileSize += currentBufSize + sizeof(uint32_t);
                currentBufSize = 0;
            }
        }
        ASSERT_ITERATORS_EQUIVALENT(sorter->done(), std::make_unique<IntIterator>(0, range));
        // Anything left in-memory is spilled to disk when we call done().
        currentFileSize += currentBufSize + sizeof(uint32_t);
        return currentFileSize;
    }
};

using KeyList = std::vector<int>;
using Histogram = std::unordered_map<int, std::size_t>;

enum class ShuffleMode { kNoShuffle, kShuffle };

constexpr std::array<Direction, 2> kDirections = {ASC, DESC};
constexpr std::size_t kSmallNumberOfKeys = 100;
constexpr std::size_t kLargeNumberOfKeys = 100 * 1000;
constexpr std::size_t kAggressiveSpillMemLimit = 16 * 1024;
constexpr std::size_t kManualSpillEveryN = 10;

constexpr std::size_t dataMemLimitFromTotal(std::size_t totalMemLimit) {
    return totalMemLimit - totalMemLimit / 10;
}

std::string makeSpillDirName() {
    auto name = fmt::format("{}_{}", unittest::getSuiteName(), unittest::getTestName());
    std::replace(name.begin(), name.end(), '/', '_');
    std::replace(name.begin(), name.end(), '\\', '_');
    return name;
}

uint64_t generateShuffleSeed(StringData context = {}) {
    const auto seed = SecureRandom{}.nextUInt64();
    LOGV2(11974200,
          "Sorter long-running test shuffle seed",
          "seed"_attr = seed,
          "context"_attr = context);
    return seed;
}

std::vector<int> makeInputData(std::size_t length,
                               ShuffleMode shuffleMode = ShuffleMode::kShuffle,
                               StringData context = {}) {
    std::vector<int> keys(length);
    std::iota(keys.begin(), keys.end(), 0);
    if (shuffleMode == ShuffleMode::kShuffle) {
        std::shuffle(keys.begin(), keys.end(), PseudoRandom{generateShuffleSeed(context)}.urbg());
    }
    return keys;
}

Histogram buildHistogram(const KeyList& keys) {
    Histogram histogram;
    for (int key : keys) {
        ++histogram[key];
    }
    return histogram;
}

std::pair<std::size_t, std::size_t> computeMergedSpills(std::size_t spillsToMergeNum,
                                                        std::size_t targetSpillsNum,
                                                        std::size_t parallelSpillsNum) {
    std::size_t newSpillsDone = 0;
    while (spillsToMergeNum > targetSpillsNum) {
        auto newSpills = (spillsToMergeNum + parallelSpillsNum - 1) / parallelSpillsNum;
        newSpillsDone += newSpills;
        spillsToMergeNum = newSpills;
    }
    return {spillsToMergeNum, newSpillsDone};
}

struct RangeCoverageExpectation {
    std::size_t numRanges;
    std::size_t spilledRanges;
};

RangeCoverageExpectation expectedRangeCoverageForAggressiveSpilling() {
    const auto dataMemLimit = dataMemLimitFromTotal(kAggressiveSpillMemLimit);
    const auto expectedNumRanges =
        std::max<std::size_t>(dataMemLimit / sorter::kSortedFileBufferSize, 2);
    const auto maximumNumberOfIterators = std::max<std::size_t>(
        (kAggressiveSpillMemLimit - dataMemLimit) / sizeof(FileIterator<IntWrapper, IntWrapper>),
        1);

    const auto recordsPerRange = dataMemLimit / sizeof(IWPair) + 1;
    std::size_t documentsToAdd = kLargeNumberOfKeys;
    std::size_t spillsToMerge = 0;
    std::size_t spillsDone = 0;
    while (documentsToAdd > recordsPerRange) {
        documentsToAdd -= recordsPerRange;
        ++spillsToMerge;
        ++spillsDone;

        if (spillsToMerge >= maximumNumberOfIterators) {
            auto spillsRes =
                computeMergedSpills(spillsToMerge, maximumNumberOfIterators / 2, expectedNumRanges);
            spillsToMerge = spillsRes.first;
            spillsDone += spillsRes.second;
        }
    }
    if (documentsToAdd > 0) {
        ++spillsToMerge;
        ++spillsDone;
    }

    auto spillsRes = computeMergedSpills(spillsToMerge, expectedNumRanges, expectedNumRanges);
    spillsDone += spillsRes.second;
    return {expectedNumRanges, spillsDone};
}

RangeCoverageExpectation expectedRangeCoverageForManualSpills() {
    const auto rangeCount = (kSmallNumberOfKeys + kManualSpillEveryN - 1) / kManualSpillEveryN;
    return {rangeCount, rangeCount};
}

struct ExpectedSorterOutput {
    Histogram frequencies;
    std::size_t count;
};

ExpectedSorterOutput expectedOutputForLimit(const KeyList& input,
                                            const SortOptions& opts,
                                            Direction direction) {
    const auto expectedCount =
        opts.limit == 0 ? input.size() : std::min<std::size_t>(opts.limit, input.size());
    if (expectedCount == 0) {
        return {Histogram{}, 0};
    }

    if (opts.limit == 0 || opts.limit >= input.size()) {
        return {buildHistogram(input), expectedCount};
    }

    std::vector<int> selected;
    selected.reserve(expectedCount);
    if (direction == ASC) {
        std::priority_queue<int> smallestK;
        for (int key : input) {
            if (smallestK.size() < expectedCount) {
                smallestK.push(key);
            } else if (key < smallestK.top()) {
                smallestK.pop();
                smallestK.push(key);
            }
        }
        while (!smallestK.empty()) {
            selected.push_back(smallestK.top());
            smallestK.pop();
        }
    } else {
        std::priority_queue<int, std::vector<int>, std::greater<int>> largestK;
        for (int key : input) {
            if (largestK.size() < expectedCount) {
                largestK.push(key);
            } else if (key > largestK.top()) {
                largestK.pop();
                largestK.push(key);
            }
        }
        while (!largestK.empty()) {
            selected.push_back(largestK.top());
            largestK.pop();
        }
    }
    return {buildHistogram(selected), expectedCount};
}

template <typename IteratorHandle>
void assertOutputMatches(IteratorHandle dataToValidate,
                         ExpectedSorterOutput expected,
                         Direction direction) {
    boost::optional<int> prev;
    std::size_t seen = 0;
    while (dataToValidate->more()) {
        auto pair = dataToValidate->next();
        const auto key = static_cast<int>(pair.first);
        const auto value = static_cast<int>(pair.second);

        ASSERT_EQ(value, -key);
        if (prev) {
            if (direction == ASC) {
                ASSERT_LTE(*prev, key);
            } else {
                ASSERT_GTE(*prev, key);
            }
        }
        prev = key;

        auto it = expected.frequencies.find(key);
        ASSERT(it != expected.frequencies.end());
        ASSERT_GT(it->second, 0U);
        if (--it->second == 0) {
            expected.frequencies.erase(it);
        }
        ++seen;
    }

    ASSERT_EQ(seen, expected.count);
    ASSERT(expected.frequencies.empty());
}

void validateSortOutput(const std::shared_ptr<IWSorter>& sorter,
                        const SortOptions& opts,
                        const KeyList& input,
                        Direction direction) {
    auto expected = expectedOutputForLimit(input, opts, direction);
    assertOutputMatches(sorter->done(), std::move(expected), direction);
    ASSERT_EQ(sorter->stats().numSorted(), input.size());
}

void assertPersistedRangeInfo(const std::shared_ptr<IWSorter>& sorter,
                              const SortOptions& opts,
                              const RangeCoverageExpectation& expected) {
    auto state = sorter->persistDataForShutdown();
    ASSERT_EQ(state.ranges.size(), expected.numRanges);
    ASSERT_EQ(sorter->stats().spilledRanges(), expected.spilledRanges);
}

template <typename Traits>
class SorterTypedTest : public ServiceContextMongoDTest {
public:
    static_assert(test::StorageTraits<Traits>);
    // TODO (SERVER-116165): Remove.
    RAIIServerParameterControllerForTest ffContainerWrites{"featureFlagContainerWrites", true};

protected:
    void SetUp() override {
        ServiceContextMongoDTest::SetUp();
        if constexpr (std::is_constructible_v<Traits, ServiceContext::UniqueOperationContext>) {
            _storage.emplace(this->makeOperationContext());
        } else {
            _storage.emplace();
        }
        resetFixtureSpillDir();
    }

    void resetFixtureSpillDir() {
        _spillDir = std::make_unique<unittest::TempDir>(makeSpillDirName());
        _opts = SortOptions();
    }

    const unittest::TempDir& spillDir() const {
        return *_spillDir;
    }

    const SortOptions& opts() const {
        return _opts;
    }

    Traits& storage() {
        return *_storage;
    }

    virtual void addInputToSorter(IWSorter* sorter, const KeyList& input) const {
        for (int key : input) {
            sorter->add(key, -key);
        }
    }

    // TODO(SERVER-121481): Enable creating a sorter without a spiller and ensure there is test
    // coverage of sorters without spillers.
    std::shared_ptr<IWSorter> makeSorter(const SortOptions& sortOpts,
                                         const boost::filesystem::path& spillDir,
                                         Direction direction) {
        return std::shared_ptr<IWSorter>(IWSorter::make(sortOpts,
                                                        IWComparator(direction),
                                                        storage().makeSpiller(sortOpts, spillDir),
                                                        /*settings=*/{}));
    }

    std::shared_ptr<IWSorter> runSort(const SortOptions& sortOpts,
                                      const KeyList& input,
                                      Direction direction) {
        auto sorter = makeSorter(sortOpts, spillDir().path(), direction);
        addInputToSorter(sorter.get(), input);
        return sorter;
    }

    std::array<std::shared_ptr<IWSorter>, 2> runMergedSortAndValidate(
        const SortOptions& sortOpts,
        const KeyList& input,
        Direction direction,
        bool insertInParallel = false) {
        std::array<std::shared_ptr<IWSorter>, 2> sorters = {
            makeSorter(sortOpts, spillDir().path(), direction),
            makeSorter(sortOpts, spillDir().path(), direction)};

        if (insertInParallel) {
            stdx::thread inBackground(
                [this, sorter = sorters[0], &input] { addInputToSorter(sorter.get(), input); });
            addInputToSorter(sorters[1].get(), input);
            inBackground.join();
        } else {
            addInputToSorter(sorters[0].get(), input);
            addInputToSorter(sorters[1].get(), input);
        }

        std::unique_ptr<IWIterator> iters[] = {sorters[0]->done(), sorters[1]->done()};
        KeyList doubledInput = input;
        doubledInput.insert(doubledInput.end(), input.begin(), input.end());

        auto mergedSortOpts = sortOpts;
        if (sortOpts.limit != 0) {
            const auto boundedPerSorter = std::min<std::size_t>(sortOpts.limit, input.size());
            mergedSortOpts = SortOptions(sortOpts).Limit(boundedPerSorter * 2);
        }

        auto mergedExpected = expectedOutputForLimit(doubledInput, mergedSortOpts, direction);
        assertOutputMatches(
            mergeIterators(iters, spillDir(), direction), std::move(mergedExpected), direction);
        return sorters;
    }

    void assertSortAndMerge(
        const SortOptions& sortOpts,
        const KeyList& input,
        const boost::optional<RangeCoverageExpectation>& expectedRangeCoverage = boost::none) {
        for (Direction direction : kDirections) {
            auto sorter = runSort(sortOpts, input, direction);
            validateSortOutput(sorter, sortOpts, input, direction);
            if (expectedRangeCoverage) {
                assertPersistedRangeInfo(sorter, sortOpts, *expectedRangeCoverage);
            }
        }

#if !defined(MONGO_CONFIG_DEBUG_BUILD)
        for (Direction direction : kDirections) {
            auto mergedSorters = runMergedSortAndValidate(
                sortOpts,
                input,
                direction,
                /*insertInParallel=*/Traits::kHasFileStats && direction == DESC);
            if (expectedRangeCoverage) {
                assertPersistedRangeInfo(mergedSorters[0], sortOpts, *expectedRangeCoverage);
                assertPersistedRangeInfo(mergedSorters[1], sortOpts, *expectedRangeCoverage);
            }
        }
        ASSERT_EQ(expectedRangeCoverage.has_value(),
                  !boost::filesystem::is_empty(spillDir().path()));
#else
        ASSERT_EQ(expectedRangeCoverage.has_value(),
                  !boost::filesystem::is_empty(spillDir().path()));
#endif
    }

private:
    std::unique_ptr<unittest::TempDir> _spillDir;
    SortOptions _opts;
    boost::optional<Traits> _storage;
};

using SorterTypedTestTypes = ::testing::Types<FileTraits, ContainerTraits>;
TYPED_TEST_SUITE(SorterTypedTest, SorterTypedTestTypes);

using SorterFileTest = SorterTypedTest<FileTraits>;

template <typename TypeParam>
class SorterTypedTestManualSpills : public SorterTypedTest<TypeParam> {
protected:
    void addInputToSorter(IWSorter* sorter, const KeyList& values) const override {
        for (std::size_t i = 0; i < values.size(); ++i) {
            sorter->add(values[i], -values[i]);
            if (i % kManualSpillEveryN == kManualSpillEveryN - 1) {
                sorter->spill();
            }
        }
    }
};
TYPED_TEST_SUITE(SorterTypedTestManualSpills, SorterTypedTestTypes);

class SorterTestPauseAndResumeBase : public SorterFileTest {
protected:
    void assertSortAndMergeWithPauseValidation(const SortOptions& sortOpts,
                                               const boost::filesystem::path& spillDir,
                                               const KeyList& input) {
        for (Direction direction : kDirections) {
            auto sorter = runSort(sortOpts, input, direction);
            validateSortOutput(sorter, sortOpts, input, direction);
        }

#if !defined(MONGO_CONFIG_DEBUG_BUILD)
        for (Direction direction : kDirections) {
            runMergedSortAndValidate(
                sortOpts, input, direction, /*insertInParallel=*/direction == DESC);
        }
#endif

        ASSERT(boost::filesystem::is_empty(spillDir));
    }
};

class SorterTestPauseAndResume : public SorterTestPauseAndResumeBase {
protected:
    void addInputToSorter(IWSorter* sorter, const KeyList& input) const override {
        const auto splitIndex = input.size() / 2;
        const KeyList firstHalf(input.begin(), input.begin() + splitIndex);
        const KeyList secondHalf(input.begin() + splitIndex, input.end());

        for (int key : firstHalf) {
            sorter->add(key, -key);
        }
        auto iter = sorter->pause();
        for (int key : firstHalf) {
            ASSERT_EQ(key, iter->next().first);
        }
        ASSERT_FALSE(iter->more());
        iter.reset();
        sorter->resume();

        for (int key : secondHalf) {
            sorter->add(key, -key);
        }
        iter = sorter->pause();
        for (int key : input) {
            ASSERT_EQ(key, iter->next().first);
        }
        ASSERT_FALSE(iter->more());
        iter.reset();
        sorter->resume();
    }
};

class SorterTestPauseAndResumeLimit : public SorterTestPauseAndResumeBase {
protected:
    void addInputToSorter(IWSorter* sorter, const KeyList& input) const override {
        ASSERT_EQ(input.size(), 6U);
        sorter->add(input[0], -input[0]);
        sorter->add(input[1], -input[1]);
        sorter->add(input[2], -input[2]);
        auto iter = sorter->pause();
        for (int i = 0; i < 3; ++i) {
            ASSERT_EQ(input[i], iter->next().first);
        }
        ASSERT_FALSE(iter->more());
        iter.reset();
        sorter->resume();

        sorter->add(input[3], -input[3]);
        sorter->add(input[4], -input[4]);
        sorter->add(input[5], -input[5]);
        iter = sorter->pause();
        std::vector<int> keys;
        keys.reserve(5);
        for (int i = 0; i < 5; ++i) {
            keys.push_back(iter->next().first);
        }
        std::sort(keys.begin(), keys.end());
        ASSERT_TRUE(keys.back() == input[0] || keys.back() == input[2]);
        ASSERT_EQ(keys.size(), 5U);
        ASSERT_FALSE(iter->more());
        iter.reset();
        sorter->resume();
    }
};

class SorterTestPauseAndResumeLimitOne : public SorterTestPauseAndResumeBase {
protected:
    void addInputToSorter(IWSorter* sorter, const KeyList& input) const override {
        ASSERT_EQ(input.size(), 6U);
        sorter->add(input[0], -input[0]);
        sorter->add(input[1], -input[1]);
        sorter->add(input[2], -input[2]);
        auto iter = sorter->pause();
        auto val = iter->next().first;
        ASSERT_TRUE(val == input[1] || val == input[2]);
        ASSERT_FALSE(iter->more());
        sorter->resume();
        sorter->add(input[3], -input[3]);
        sorter->add(input[4], -input[4]);
        sorter->add(input[5], -input[5]);
        iter = sorter->pause();
        val = iter->next().first;
        ASSERT_TRUE(val == input[5] || val == input[2]);
        ASSERT_FALSE(iter->more());
        iter.reset();
        sorter->resume();
    }
};

TEST_F(SortedFileWriterAndFileIteratorTests, SortedFileWriterAndFileIterator) {
    unittest::TempDir spillDir("sortedFileWriterTests");
    SorterTracker sorterTracker;
    SorterFileStats sorterFileStats(&sorterTracker);
    const SortOptions opts = SortOptions();

    int currentFileSize = 0;

    currentFileSize = appendToFile(opts, spillDir.path(), &sorterFileStats, currentFileSize, 5);

    ASSERT_EQ(sorterFileStats.opened.load(), 1);
    ASSERT_EQ(sorterFileStats.closed.load(), 1);
    ASSERT_LTE(sorterTracker.bytesSpilled.load(), currentFileSize);

    currentFileSize =
        appendToFile(opts, spillDir.path(), &sorterFileStats, currentFileSize, 10 * 1000 * 1000);

    ASSERT_EQ(sorterFileStats.opened.load(), 2);
    ASSERT_EQ(sorterFileStats.closed.load(), 2);
    ASSERT_LTE(sorterTracker.bytesSpilled.load(), currentFileSize);
    ASSERT_LTE(sorterFileStats.bytesSpilled(), currentFileSize);

    ASSERT(boost::filesystem::is_empty(spillDir.path()));
}

TEST(MergeIteratorTests, MergeIterator) {
    unittest::TempDir spillDir("mergeIteratorTests");
    {  // test empty (no inputs)
        std::vector<std::shared_ptr<IWIterator>> vec;
        std::shared_ptr<IWIterator> mergeIter(
            sorter::merge<IntWrapper, IntWrapper>(vec, SortOptions(), IWComparator()));
        ASSERT_ITERATORS_EQUIVALENT(mergeIter, std::make_shared<EmptyIterator>());
    }
    {  // test empty (only empty inputs)
        std::shared_ptr<IWIterator> iterators[] = {std::make_shared<EmptyIterator>(),
                                                   std::make_shared<EmptyIterator>(),
                                                   std::make_shared<EmptyIterator>()};

        ASSERT_ITERATORS_EQUIVALENT(mergeIterators(iterators, spillDir, ASC),
                                    std::make_shared<EmptyIterator>());
    }

    {  // test ASC
        std::shared_ptr<IWIterator> iterators[] = {
            std::make_shared<IntIterator>(1, 20, 2),  // 1, 3, ... 19
            std::make_shared<IntIterator>(0, 20, 2)   // 0, 2, ... 18
        };

        ASSERT_ITERATORS_EQUIVALENT(mergeIterators(iterators, spillDir, ASC),
                                    std::make_shared<IntIterator>(0, 20, 1));
    }

    {  // test DESC with an empty source
        std::shared_ptr<IWIterator> iterators[] = {std::make_shared<IntIterator>(30, 0, -3),
                                                   std::make_shared<IntIterator>(29, 0, -3),
                                                   std::make_shared<IntIterator>(28, 0, -3),
                                                   std::make_shared<EmptyIterator>()};

        ASSERT_ITERATORS_EQUIVALENT(mergeIterators(iterators, spillDir, DESC),
                                    std::make_shared<IntIterator>(30, 0, -1));
    }
    {  // test Limit
        std::shared_ptr<IWIterator> iterators[] = {std::make_shared<IntIterator>(1, 20, 2),
                                                   std::make_shared<IntIterator>(0, 20, 2)};

        ASSERT_ITERATORS_EQUIVALENT(
            mergeIterators(iterators, spillDir, ASC, SortOptions().Limit(10)),
            std::make_shared<LimitIterator>(10, std::make_shared<IntIterator>(0, 20, 1)));
    }

    {  // test ASC with additional merging
        auto itFull = std::make_shared<IntIterator>(0, 20, 1);

        auto itA = std::make_shared<IntIterator>(0, 5, 1);
        auto itB = std::make_shared<IntIterator>(5, 10, 1);
        auto itC = std::make_shared<IntIterator>(10, 15, 1);
        auto itD = std::make_shared<IntIterator>(15, 20, 1);

        std::shared_ptr<IWIterator> iteratorsAD[] = {itD, itA};
        auto mergedAD = mergeIterators(iteratorsAD, spillDir, ASC);
        ASSERT_ITERATORS_EQUIVALENT_FOR_N_STEPS(mergedAD, itFull, 5);

        std::shared_ptr<IWIterator> iteratorsABD[] = {mergedAD, itB};
        auto mergedABD = mergeIterators(iteratorsABD, spillDir, ASC);
        ASSERT_ITERATORS_EQUIVALENT_FOR_N_STEPS(mergedABD, itFull, 5);

        std::shared_ptr<IWIterator> iteratorsABCD[] = {itC, mergedABD};
        auto mergedABCD = mergeIterators(iteratorsABCD, spillDir, ASC);
        ASSERT_ITERATORS_EQUIVALENT_FOR_N_STEPS(mergedABCD, itFull, 5);
    }
}

TYPED_TEST(SorterTypedTest, Empty) {
    auto sorter = this->runSort(this->opts(), /*input=*/{}, ASC);
    validateSortOutput(sorter, this->opts(), /*input=*/{}, ASC);

    auto limitedSorter1 = this->runSort(SortOptions(this->opts()).Limit(1), /*input=*/{}, ASC);
    validateSortOutput(limitedSorter1, SortOptions(this->opts()).Limit(1), /*input=*/{}, ASC);

    auto limitedSorter10 = this->runSort(SortOptions(this->opts()).Limit(10), /*input=*/{}, ASC);
    validateSortOutput(limitedSorter10, SortOptions(this->opts()).Limit(10), /*input=*/{}, ASC);
}

TYPED_TEST(SorterTypedTest, Basic) {
    const KeyList input = makeInputData(kSmallNumberOfKeys);
    this->assertSortAndMerge(this->opts(), input);
}

TYPED_TEST(SorterTypedTest, Limit) {
    const KeyList input = makeInputData(kSmallNumberOfKeys + 1);
    const auto sortOpts = SortOptions(this->opts()).Limit(kSmallNumberOfKeys);
    this->assertSortAndMerge(sortOpts, input);
}

TYPED_TEST(SorterTypedTest, DuplicateValues) {
    KeyList input = makeInputData(kSmallNumberOfKeys, ShuffleMode::kShuffle);
    KeyList copy = input;
    input.reserve(input.size() + copy.size());
    // Concatenate input with itself.
    input.insert(input.end(), copy.begin(), copy.end());
    this->assertSortAndMerge(this->opts(), input);
}

template <typename T>
inline constexpr auto kMaxAsU64 = static_cast<unsigned long long>(std::numeric_limits<T>::max());

TYPED_TEST(SorterTypedTest, LimitExtremes) {
    const KeyList input = makeInputData(kSmallNumberOfKeys);

    constexpr auto limits = std::array{
        kMaxAsU64<uint32_t>,
        kMaxAsU64<uint32_t> - 1,
        kMaxAsU64<uint32_t> + 1,
        kMaxAsU64<uint32_t> / 8 + 1,
        kMaxAsU64<int32_t>,
        kMaxAsU64<int32_t> - 1,
        kMaxAsU64<int32_t> + 1,
        kMaxAsU64<int32_t> / 8 + 1,
        kMaxAsU64<uint64_t>,
        kMaxAsU64<uint64_t> - 1,
        0ull,
        kMaxAsU64<uint64_t> / 8 + 1,
        kMaxAsU64<int64_t>,
        kMaxAsU64<int64_t> - 1,
        kMaxAsU64<int64_t> + 1,
        kMaxAsU64<int64_t> / 8 + 1,
    };

    for (auto limit : limits) {
        this->assertSortAndMerge(SortOptions(this->opts()).Limit(limit), input);
    }
}

TYPED_TEST(SorterTypedTest, AggressiveSpilling) {
    if constexpr (shouldSkipContainerBasedTestInDebugBuild<TypeParam>()) {
        GTEST_SKIP() << "Skipping container based instantiation due to being a debug build";
    }

    for (auto shuffleMode : {ShuffleMode::kNoShuffle, ShuffleMode::kShuffle}) {
        this->resetFixtureSpillDir();
        const auto context = fmt::format(
            "dataSize={},memoryLimit={},limit={}", kLargeNumberOfKeys, kAggressiveSpillMemLimit, 0);
        const KeyList input = makeInputData(kLargeNumberOfKeys, shuffleMode, context);
        const auto sortOpts =
            SortOptions(this->opts()).MaxMemoryUsageBytes(kAggressiveSpillMemLimit);
        if constexpr (TypeParam::kHasFileStats) {
            this->assertSortAndMerge(sortOpts, input, expectedRangeCoverageForAggressiveSpilling());
        } else {
            this->assertSortAndMerge(sortOpts, input);
        }
    }
}

TYPED_TEST(SorterTypedTest, LotsOfDataWithLimit) {
    if constexpr (shouldSkipContainerBasedTestInDebugBuild<TypeParam>()) {
        GTEST_SKIP() << "Skipping container based instantiation due to being a debug build";
    }

    constexpr auto limits = std::array{1ull, 100ull, 5000ull};

    for (auto limit : limits) {
        for (auto shuffleMode : {ShuffleMode::kNoShuffle, ShuffleMode::kShuffle}) {
            this->resetFixtureSpillDir();
            const auto context = fmt::format("dataSize={},memoryLimit={},limit={}",
                                             kLargeNumberOfKeys,
                                             kAggressiveSpillMemLimit,
                                             limit);
            const KeyList input = makeInputData(kLargeNumberOfKeys, shuffleMode, context);
            const auto sortOpts = SortOptions(this->opts())
                                      .MaxMemoryUsageBytes(kAggressiveSpillMemLimit)
                                      .Limit(limit);
            this->assertSortAndMerge(sortOpts, input);
        }
    }
}

TYPED_TEST(SorterTypedTestManualSpills, ManualSpills) {
    KeyList input = makeInputData(kSmallNumberOfKeys, ShuffleMode::kShuffle);
    if constexpr (TypeParam::kHasFileStats) {
        this->assertSortAndMerge(this->opts(), input, expectedRangeCoverageForManualSpills());
    } else {
        this->assertSortAndMerge(this->opts(), input);
    }
}

TYPED_TEST(SorterTypedTestManualSpills, ManualSpillsWithLimit) {
    constexpr auto limit = kSmallNumberOfKeys / 2;
    KeyList input = makeInputData(kSmallNumberOfKeys, ShuffleMode::kShuffle);
    const auto sortOpts = SortOptions(this->opts()).Limit(limit);
    this->assertSortAndMerge(sortOpts, input);
}

TEST_F(SorterTestPauseAndResume, PauseAndResume) {
    const KeyList input = {0, 3, 4, 2, 1};
    assertSortAndMergeWithPauseValidation(opts(), spillDir().path(), input);
}

TEST_F(SorterTestPauseAndResumeLimit, PauseAndResumeLimit) {
    const KeyList input = {3, 0, 4, 2, 1, -1};
    const auto sortOpts = SortOptions(opts()).Limit(5);
    assertSortAndMergeWithPauseValidation(sortOpts, spillDir().path(), input);
}

TEST_F(SorterTestPauseAndResumeLimitOne, PauseAndResumeLimitOne) {
    const KeyList input = {3, 0, 4, 2, 1, -1};
    const auto sortOpts = SortOptions(opts()).Limit(1);
    assertSortAndMergeWithPauseValidation(sortOpts, spillDir().path(), input);
}

}  // namespace SorterTests
}  // namespace
}  // namespace mongo::sorter
