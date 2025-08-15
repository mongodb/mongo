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

#include "mongo/db/sorter/sorter_template_defs.h"
#include "mongo/db/sorter/sorter_test_utils.h"
#include "mongo/platform/random.h"

#include <climits>

#include <boost/filesystem/directory.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest

namespace mongo::sorter {

class SortedFileWriterAndFileIteratorTests {
public:
    void run() {
        unittest::TempDir tempDir("sortedFileWriterTests");
        SorterTracker sorterTracker;
        SorterFileStats sorterFileStats(&sorterTracker);
        const SortOptions opts = SortOptions().TempDir(tempDir.path()).FileStats(&sorterFileStats);

        int currentFileSize = 0;

        // small
        currentFileSize = _appendToFile(&opts, currentFileSize, 5);

        ASSERT_EQ(sorterFileStats.opened.load(), 1);
        ASSERT_EQ(sorterFileStats.closed.load(), 1);
        ASSERT_LTE(sorterTracker.bytesSpilled.load(), currentFileSize);

        // big
        currentFileSize = _appendToFile(&opts, currentFileSize, 10 * 1000 * 1000);

        ASSERT_EQ(sorterFileStats.opened.load(), 2);
        ASSERT_EQ(sorterFileStats.closed.load(), 2);
        ASSERT_LTE(sorterTracker.bytesSpilled.load(), currentFileSize);
        ASSERT_LTE(sorterFileStats.bytesSpilled(), currentFileSize);

        ASSERT(boost::filesystem::is_empty(tempDir.path()));
    }

private:
    int _appendToFile(const SortOptions* opts, int currentFileSize, int range) {
        auto makeFile = [&] {
            return std::make_shared<Sorter<IntWrapper, IntWrapper>::File>(
                sorter::nextFileName(*(opts->tempDir)), opts->sorterFileStats);
        };

        int currentBufSize = 0;
        SortedFileWriter<IntWrapper, IntWrapper> sorter(*opts, makeFile());
        for (int i = 0; i < range; ++i) {
            sorter.addAlreadySorted(i, -i);
            currentBufSize += sizeof(i) + sizeof(-i);

            if (currentBufSize > static_cast<int>(sorter::kSortedFileBufferSize)) {
                // File size only increases if buffer size exceeds limit and spills. Each spill
                // includes the buffer and the size of the spill.
                currentFileSize += currentBufSize + sizeof(uint32_t);
                currentBufSize = 0;
            }
        }
        ASSERT_ITERATORS_EQUIVALENT(sorter.done(), std::make_unique<IntIterator>(0, range));
        // Anything left in-memory is spilled to disk when sorter.done().
        currentFileSize += currentBufSize + sizeof(uint32_t);
        return currentFileSize;
    }
};

class MergeIteratorTests {
public:
    void run() {
        unittest::TempDir tempDir("mergeIteratorTests");
        {  // test empty (no inputs)
            std::vector<std::shared_ptr<IWIterator>> vec;
            std::shared_ptr<IWIterator> mergeIter(
                IWIterator::merge(vec, SortOptions(), IWComparator()));
            ASSERT_ITERATORS_EQUIVALENT(mergeIter, std::make_shared<EmptyIterator>());
        }
        {  // test empty (only empty inputs)
            std::shared_ptr<IWIterator> iterators[] = {std::make_shared<EmptyIterator>(),
                                                       std::make_shared<EmptyIterator>(),
                                                       std::make_shared<EmptyIterator>()};

            ASSERT_ITERATORS_EQUIVALENT(mergeIterators(iterators, tempDir, ASC),
                                        std::make_shared<EmptyIterator>());
        }

        {  // test ASC
            std::shared_ptr<IWIterator> iterators[] = {
                std::make_shared<IntIterator>(1, 20, 2)  // 1, 3, ... 19
                ,
                std::make_shared<IntIterator>(0, 20, 2)  // 0, 2, ... 18
            };

            ASSERT_ITERATORS_EQUIVALENT(mergeIterators(iterators, tempDir, ASC),
                                        std::make_shared<IntIterator>(0, 20, 1));
        }

        {  // test DESC with an empty source
            std::shared_ptr<IWIterator> iterators[] = {
                std::make_shared<IntIterator>(30, 0, -3),  // 30, 27, ... 3
                std::make_shared<IntIterator>(29, 0, -3),  // 29, 26, ... 2
                std::make_shared<IntIterator>(28, 0, -3),  // 28, 25, ... 1
                std::make_shared<EmptyIterator>()};

            ASSERT_ITERATORS_EQUIVALENT(mergeIterators(iterators, tempDir, DESC),
                                        std::make_shared<IntIterator>(30, 0, -1));
        }
        {  // test Limit
            std::shared_ptr<IWIterator> iterators[] = {
                std::make_shared<IntIterator>(1, 20, 2),   // 1, 3, ... 19
                std::make_shared<IntIterator>(0, 20, 2)};  // 0, 2, ... 18

            ASSERT_ITERATORS_EQUIVALENT(
                mergeIterators(iterators, tempDir, ASC, SortOptions().Limit(10)),
                std::make_shared<LimitIterator>(10, std::make_shared<IntIterator>(0, 20, 1)));
        }

        {  // test ASC with additional merging
            auto itFull = std::make_shared<IntIterator>(0, 20, 1);

            auto itA = std::make_shared<IntIterator>(0, 5, 1);    // 0, 1, ... 4
            auto itB = std::make_shared<IntIterator>(5, 10, 1);   // 5, 6, ... 9
            auto itC = std::make_shared<IntIterator>(10, 15, 1);  // 10, 11, ... 14
            auto itD = std::make_shared<IntIterator>(15, 20, 1);  // 15, 16, ... 19

            std::shared_ptr<IWIterator> iteratorsAD[] = {itD, itA};
            auto mergedAD = mergeIterators(iteratorsAD, tempDir, ASC);
            ASSERT_ITERATORS_EQUIVALENT_FOR_N_STEPS(mergedAD, itFull, 5);

            std::shared_ptr<IWIterator> iteratorsABD[] = {mergedAD, itB};
            auto mergedABD = mergeIterators(iteratorsABD, tempDir, ASC);
            ASSERT_ITERATORS_EQUIVALENT_FOR_N_STEPS(mergedABD, itFull, 5);

            std::shared_ptr<IWIterator> iteratorsABCD[] = {itC, mergedABD};
            auto mergedABCD = mergeIterators(iteratorsABCD, tempDir, ASC);
            ASSERT_ITERATORS_EQUIVALENT_FOR_N_STEPS(mergedABCD, itFull, 5);
        }
    }
};

namespace SorterTests {
class Basic {
public:
    virtual ~Basic() {}

    void run() {
        unittest::TempDir tempDir("sorterTests");
        SorterTracker sorterTracker;
        const SortOptions opts = SortOptions().TempDir(tempDir.path()).Tracker(&sorterTracker);

        {  // test empty (no limit)
            ASSERT_ITERATORS_EQUIVALENT(makeSorter(opts)->done(),
                                        std::make_unique<EmptyIterator>());
        }
        {  // test empty (limit 1)
            ASSERT_ITERATORS_EQUIVALENT(makeSorter(SortOptions(opts).Limit(1))->done(),
                                        std::make_unique<EmptyIterator>());
        }
        {  // test empty (limit 10)
            ASSERT_ITERATORS_EQUIVALENT(makeSorter(SortOptions(opts).Limit(10))->done(),
                                        std::make_unique<EmptyIterator>());
        }

        const auto runTests = [this, &opts, &tempDir](bool assertRanges) {
            {  // test all data ASC
                std::shared_ptr<IWSorter> sorter = makeSorter(opts, IWComparator(ASC));
                addData(sorter.get());
                ASSERT_ITERATORS_EQUIVALENT(sorter->done(), correct());
                ASSERT_EQ(numAdded(), sorter->stats().numSorted());
                if (assertRanges) {
                    assertRangeInfo(sorter, opts);
                }
            }
            {  // test all data DESC
                std::shared_ptr<IWSorter> sorter = makeSorter(opts, IWComparator(DESC));
                addData(sorter.get());
                ASSERT_ITERATORS_EQUIVALENT(sorter->done(), correctReverse());
                ASSERT_EQ(numAdded(), sorter->stats().numSorted());
                if (assertRanges) {
                    assertRangeInfo(sorter, opts);
                }
            }

// The debug builds are too slow to run these tests.
// Among other things, MSVC++ makes all heap functions O(N) not O(logN).
#if !defined(MONGO_CONFIG_DEBUG_BUILD)
            {  // merge all data ASC
                std::shared_ptr<IWSorter> sorters[] = {makeSorter(opts, IWComparator(ASC)),
                                                       makeSorter(opts, IWComparator(ASC))};

                addData(sorters[0].get());
                addData(sorters[1].get());

                std::unique_ptr<IWIterator> iters1[] = {sorters[0]->done(), sorters[1]->done()};
                std::shared_ptr<IWIterator> iters2[] = {correct(), correct()};
                ASSERT_ITERATORS_EQUIVALENT(mergeIterators(iters1, tempDir, ASC),
                                            mergeIterators(iters2, tempDir, ASC));

                if (assertRanges) {
                    assertRangeInfo(sorters[0], opts);
                    assertRangeInfo(sorters[1], opts);
                }
            }
            {  // merge all data DESC and use multiple threads to insert
                std::shared_ptr<IWSorter> sorters[] = {makeSorter(opts, IWComparator(DESC)),
                                                       makeSorter(opts, IWComparator(DESC))};

                stdx::thread inBackground(&Basic::addData, this, sorters[0].get());
                addData(sorters[1].get());
                inBackground.join();

                std::unique_ptr<IWIterator> iters1[] = {sorters[0]->done(), sorters[1]->done()};
                std::shared_ptr<IWIterator> iters2[] = {correctReverse(), correctReverse()};
                ASSERT_ITERATORS_EQUIVALENT(mergeIterators(iters1, tempDir, DESC),
                                            mergeIterators(iters2, tempDir, DESC));

                if (assertRanges) {
                    assertRangeInfo(sorters[0], opts);
                    assertRangeInfo(sorters[1], opts);
                }
            }
#endif
        };

        // Run the tests without checking the Sorter ranges. This means that
        // Sorter::persistDataForShutdown() will not be called, so we can verify that the Sorter
        // properly cleans up its files upon destruction.
        runTests(false);
        ASSERT(boost::filesystem::is_empty(tempDir.path()));

        // Run the tests checking the Sorter ranges. This allows us to verify that
        // Sorter::persistDataForShutdown() correctly persists the Sorter data.
        runTests(true);
        if (correctNumRanges() == 0) {
            ASSERT(boost::filesystem::is_empty(tempDir.path()));
        } else {
            ASSERT(!boost::filesystem::is_empty(tempDir.path()));
            auto path = boost::filesystem::path(tempDir.path());
            auto directoryIterator = boost::filesystem::directory_iterator(path);
            auto numFiles = std::count_if(
                directoryIterator, boost::filesystem::directory_iterator(), [](const auto& elem) {
                    return boost::filesystem::is_regular_file(elem);
                });
#if defined(MONGO_CONFIG_DEBUG_BUILD)
            // Two sorters have executed
            ASSERT_EQ(numFiles, 2);
#else
            // Six sorters have executed
            ASSERT_EQ(numFiles, 6);
#endif
        }
    }

    // add data to the sorter
    virtual void addData(IWSorter* sorter) {
        sorter->add(2, -2);
        sorter->add(1, -1);
        sorter->add(0, 0);
        sorter->add(4, -4);
        sorter->add(3, -3);
    }

    virtual size_t numAdded() const {
        return 5;
    }

    // returns an iterator with the correct results
    virtual std::shared_ptr<IWIterator> correct() {
        return std::make_shared<IntIterator>(0, 5);  // 0, 1, ... 4
    }

    // like correct but with opposite sort direction
    virtual std::shared_ptr<IWIterator> correctReverse() {
        return std::make_shared<IntIterator>(4, -1, -1);  // 4, 3, ... 0
    }

    virtual size_t correctNumRanges() const {
        return 0;
    }

    virtual size_t correctSpilledRanges() const {
        return 0;
    }

    // It is safe to ignore / overwrite any part of options
    virtual SortOptions adjustSortOptions(SortOptions opts) {
        return opts;
    }

private:
    // Make a new sorter with desired opts and comp. Opts may be ignored but not comp
    std::shared_ptr<IWSorter> makeSorter(SortOptions opts, IWComparator comp = IWComparator(ASC)) {
        return std::shared_ptr<IWSorter>(IWSorter::make(adjustSortOptions(opts), comp));
    }

    void assertRangeInfo(const std::shared_ptr<IWSorter>& sorter, const SortOptions& opts) {
        auto numRanges = correctNumRanges();
        if (numRanges == 0)
            return;

        auto numSpilledRangesOccurred = correctSpilledRanges();
        auto state = sorter->persistDataForShutdown();
        if (opts.tempDir) {
            ASSERT_NE(state.fileName, "");
        }
        ASSERT_EQ(state.ranges.size(), numRanges);
        ASSERT_EQ(sorter->stats().spilledRanges(), numSpilledRangesOccurred);
    }
};

class PauseAndResume : public Basic {
    void addData(IWSorter* sorter) override {
        sorter->add(0, 0);
        sorter->add(3, -3);
        sorter->add(4, -4);
        auto iter = sorter->pause();
        int unsorted[] = {0, 3, 4};
        for (int i = 0; i < 3; i++) {
            ASSERT_EQ(unsorted[i], iter->next().first);
        }
        ASSERT_FALSE(iter->more());
        iter.reset();
        sorter->resume();
        sorter->add(2, -2);
        sorter->add(1, -1);
        iter = sorter->pause();
        int unsorted1[] = {0, 3, 4, 2, 1};
        for (int i = 0; i < 5; i++) {
            ASSERT_EQ(unsorted1[i], iter->next().first);
        }
        ASSERT_FALSE(iter->more());
        iter.reset();
        sorter->resume();
    }

    size_t numAdded() const override {
        return 5;
    }

    // returns an iterator with the correct results
    std::shared_ptr<IWIterator> correct() override {
        return std::make_shared<IntIterator>(0, 5);  // 0, 1, ... 4
    }

    // like correct but with opposite sort direction
    std::shared_ptr<IWIterator> correctReverse() override {
        return std::make_shared<IntIterator>(4, -1, -1);  // 4, 3, ... 0
    }
};

class Limit : public Basic {
    SortOptions adjustSortOptions(SortOptions opts) override {
        return opts.Limit(5);
    }
    void addData(IWSorter* sorter) override {
        sorter->add(0, 0);
        sorter->add(3, -3);
        sorter->add(4, -4);
        sorter->add(2, -2);
        sorter->add(1, -1);
        sorter->add(-1, 1);
    }
    size_t numAdded() const override {
        return 6;
    }
    std::shared_ptr<IWIterator> correct() override {
        return std::make_shared<IntIterator>(-1, 4);
    }
    std::shared_ptr<IWIterator> correctReverse() override {
        return std::make_shared<IntIterator>(4, -1, -1);
    }
};

template <uint64_t Limit>
class LimitExtreme : public Basic {
    SortOptions adjustSortOptions(SortOptions opts) override {
        return opts.Limit(Limit);
    }
};

class PauseAndResumeLimit : public Limit {
    SortOptions adjustSortOptions(SortOptions opts) override {
        return opts.Limit(5);
    }
    void addData(IWSorter* sorter) override {
        sorter->add(3, -3);
        sorter->add(0, 0);
        sorter->add(4, -4);
        auto iter = sorter->pause();
        // pause returns data still in the original order because we haven't reached the limit
        int unsorted[] = {3, 0, 4};
        for (int i = 0; i < 3; i++) {
            ASSERT_EQ(unsorted[i], iter->next().first);
        }
        ASSERT_FALSE(iter->more());
        iter.reset();
        sorter->resume();
        sorter->add(2, -2);
        sorter->add(1, -1);
        sorter->add(-1, 1);
        iter = sorter->pause();
        // pause will return top 5 elements in some order (they are from a heap but not yet sorted)
        std::vector<int> vec;
        for (int i = 0; i < 5; i++) {
            vec.push_back(iter->next().first);
        }
        sort(vec.begin(), vec.end());
        ASSERT_TRUE(vec.back() == 3 || vec.back() == 4);  // either 4 or 3 depending on asc or desc
        ASSERT_EQ(vec.size(), 5);
        ASSERT_FALSE(iter->more());  // check to make sure we only got limit number of entries
        iter.reset();
        sorter->resume();
    }

    size_t numAdded() const override {
        return 6;
    }

    // returns an iterator with the correct results
    std::shared_ptr<IWIterator> correct() override {
        return std::make_shared<IntIterator>(-1, 4);
    }

    // like correct but with opposite sort direction
    std::shared_ptr<IWIterator> correctReverse() override {
        return std::make_shared<IntIterator>(4, -1, -1);
    }
};

class PauseAndResumeLimitOne : public Limit {
    SortOptions adjustSortOptions(SortOptions opts) override {
        return opts.Limit(1);
    }
    void addData(IWSorter* sorter) override {
        sorter->add(3, -3);
        sorter->add(0, 0);
        sorter->add(4, -4);
        auto iter = sorter->pause();
        auto val = iter->next().first;
        ASSERT_TRUE(val == 0 || val == 4);
        ASSERT_FALSE(iter->more());
        sorter->resume();
        sorter->add(2, -2);
        sorter->add(1, -1);
        sorter->add(-1, 1);
        iter = sorter->pause();
        val = iter->next().first;
        ASSERT_TRUE(val == -1 || val == 4);  // either 4 or -1 depending on asc or desc
        ASSERT_FALSE(iter->more());  // check to make sure we only got limit number of entries
        iter.reset();
        sorter->resume();
    }

    size_t numAdded() const override {
        return 6;
    }

    // returns an iterator with the correct results
    std::shared_ptr<IWIterator> correct() override {
        return std::make_shared<IntIterator>(-1, 0);
    }

    // like correct but with opposite sort direction
    std::shared_ptr<IWIterator> correctReverse() override {
        return std::make_shared<IntIterator>(4, 3, -1);
    }
};


class Dupes : public Basic {
    void addData(IWSorter* sorter) override {
        sorter->add(1, -1);
        sorter->add(-1, 1);
        sorter->add(1, -1);
        sorter->add(-1, 1);
        sorter->add(1, -1);
        sorter->add(0, 0);
        sorter->add(2, -2);
        sorter->add(-1, 1);
        sorter->add(2, -2);
        sorter->add(3, -3);
    }
    size_t numAdded() const override {
        return 10;
    }
    std::shared_ptr<IWIterator> correct() override {
        const int array[] = {-1, -1, -1, 0, 1, 1, 1, 2, 2, 3};
        return makeInMemIterator(array);
    }
    std::shared_ptr<IWIterator> correctReverse() override {
        const int array[] = {3, 2, 2, 1, 1, 1, 0, -1, -1, -1};
        return makeInMemIterator(array);
    }
};

template <bool Random = true>
class LotsOfDataLittleMemory : public Basic {
public:
    LotsOfDataLittleMemory() : _array(new int[NUM_ITEMS]), _random(int64_t(time(nullptr))) {
        for (int i = 0; i < NUM_ITEMS; i++)
            _array[i] = i;

        if (Random)
            std::shuffle(_array.get(), _array.get() + NUM_ITEMS, _random.urbg());
    }

    SortOptions adjustSortOptions(SortOptions opts) override {
        // Make sure we use a reasonable number of files when we spill
        MONGO_STATIC_ASSERT((NUM_ITEMS * sizeof(IWPair)) / DATA_MEM_LIMIT > 50);
        // Each iterator is kFileIteratorSize bytes and we do not want to exceed the maximum number
        // of iterators that we can store in the reserved memory (MEM_LIMIT - DATA_MEM_LIMIT).
        MONGO_STATIC_ASSERT(
            (NUM_ITEMS * sizeof(IWPair)) / DATA_MEM_LIMIT <
            std::max(static_cast<std::size_t>(
                         (MEM_LIMIT - DATA_MEM_LIMIT) /
                         MergeableSorter<IntWrapper, IntWrapper, IWComparator>::kFileIteratorSize),
                     static_cast<std::size_t>(1)));

        return opts.MaxMemoryUsageBytes(MEM_LIMIT);
    }

    void addData(IWSorter* sorter) override {
        for (int i = 0; i < NUM_ITEMS; i++)
            sorter->add(_array[i], -_array[i]);
    }

    size_t numAdded() const override {
        return NUM_ITEMS;
    }

    std::shared_ptr<IWIterator> correct() override {
        return std::make_shared<IntIterator>(0, NUM_ITEMS);
    }
    std::shared_ptr<IWIterator> correctReverse() override {
        return std::make_shared<IntIterator>(NUM_ITEMS - 1, -1, -1);
    }

    size_t correctNumRanges() const override {
        return std::max(static_cast<std::size_t>(DATA_MEM_LIMIT / sorter::kSortedFileBufferSize),
                        static_cast<std::size_t>(2));
    }

    std::pair<size_t, size_t> correctMergedSpills(size_t spillsToMergeNum,
                                                  size_t targetSpillsNum,
                                                  size_t parallelSpillsNum) const {
        size_t newSpillsDone = 0l;
        while (spillsToMergeNum > targetSpillsNum) {
            auto newSpills = (spillsToMergeNum + parallelSpillsNum - 1) / parallelSpillsNum;
            newSpillsDone += newSpills;
            spillsToMergeNum = newSpills;
        }

        return {spillsToMergeNum, newSpillsDone};
    }

    size_t correctSpilledRanges() const override {
        // It spills when the data in memory is more than the maximum allowed memory.
        std::size_t recordsPerRange = DATA_MEM_LIMIT / sizeof(IWPair) + 1;
        std::size_t spillsToMerge = (NUM_ITEMS + recordsPerRange - 1) / recordsPerRange;

        // As the spills may get merged we'll account for the intermediate spills that happen.
        std::size_t spillsDone = spillsToMerge;
        std::size_t targetRanges = correctNumRanges();
        auto spillsRes = correctMergedSpills(spillsToMerge, targetRanges, targetRanges);
        spillsDone += spillsRes.second;
        return spillsDone;
    }

    enum Constants {
        NUM_ITEMS = 800 * 1000,
        MEM_LIMIT = 128 * 1024,  // The total memory limit.
        // The memory limit after subtracting the memory reserved for the file iterators.
        DATA_MEM_LIMIT = MEM_LIMIT - static_cast<int>(MEM_LIMIT / 10),
    };
    std::unique_ptr<int[]> _array;
    PseudoRandom _random;
};

template <long long Limit, bool Random = true>
class LotsOfDataWithLimit : public LotsOfDataLittleMemory<Random> {
    typedef LotsOfDataLittleMemory<Random> Parent;
    SortOptions adjustSortOptions(SortOptions opts) override {
        // Make sure our tests will spill or not as desired
        MONGO_STATIC_ASSERT(DATA_MEM_LIMIT / 2 > (100 * sizeof(IWPair)));
        MONGO_STATIC_ASSERT(DATA_MEM_LIMIT < (5000 * sizeof(IWPair)));
        MONGO_STATIC_ASSERT(DATA_MEM_LIMIT * 2 > (5000 * sizeof(IWPair)));

        // Make sure we use a reasonable number of files when we spill
        MONGO_STATIC_ASSERT((Parent::NUM_ITEMS * sizeof(IWPair)) / DATA_MEM_LIMIT > 100);
        MONGO_STATIC_ASSERT((Parent::NUM_ITEMS * sizeof(IWPair)) / DATA_MEM_LIMIT < 500);

        return opts.MaxMemoryUsageBytes(MEM_LIMIT).Limit(Limit);
    }
    std::shared_ptr<IWIterator> correct() override {
        return std::make_shared<LimitIterator>(Limit, Parent::correct());
    }
    std::shared_ptr<IWIterator> correctReverse() override {
        return std::make_shared<LimitIterator>(Limit, Parent::correctReverse());
    }
    size_t correctNumRanges() const override {
        // For the TopKSorter, the number of ranges depends on the specific composition of the data
        // being sorted.
        return 0;
    }

    enum {
        MEM_LIMIT = 32 * 1024,  // The total memory limit.
        // The memory limit after subtracting the memory reserved for the file iterators.
        DATA_MEM_LIMIT = MEM_LIMIT - static_cast<int>(MEM_LIMIT / 10),
    };
};

template <bool Random = true>
class LotsOfSpillsLittleMemory : public LotsOfDataLittleMemory<Random> {
    typedef LotsOfDataLittleMemory<Random> Parent;
    SortOptions adjustSortOptions(SortOptions opts) override {
        // Make sure we create a lot of spills
        MONGO_STATIC_ASSERT(
            (Parent::NUM_ITEMS * sizeof(IWPair)) / DATA_MEM_LIMIT >
            std::max(static_cast<std::size_t>(
                         (MEM_LIMIT - DATA_MEM_LIMIT) /
                         MergeableSorter<IntWrapper, IntWrapper, IWComparator>::kFileIteratorSize),
                     static_cast<std::size_t>(1)));

        return opts.MaxMemoryUsageBytes(MEM_LIMIT);
    }

    size_t correctSpilledRanges() const override {
        std::size_t maximumNumberOfIterators =
            std::max(static_cast<std::size_t>(
                         (MEM_LIMIT - DATA_MEM_LIMIT) /
                         MergeableSorter<IntWrapper, IntWrapper, IWComparator>::kFileIteratorSize),
                     static_cast<std::size_t>(1));
        // It spills when the data in memory is more than the maximum allowed memory.
        std::size_t recordsPerRange = DATA_MEM_LIMIT / sizeof(IWPair) + 1;
        std::size_t documentsToAdd = Parent::NUM_ITEMS;
        std::size_t spillsInParallel = Parent::correctNumRanges();
        std::size_t spillsDone = 0;
        std::size_t spillsToMerge = 0;
        while (documentsToAdd > recordsPerRange) {
            documentsToAdd -= recordsPerRange;
            ++spillsToMerge;
            ++spillsDone;
            // merge iterators when the maximum iterators limit has been reached.
            if (spillsToMerge >= maximumNumberOfIterators) {
                auto spillsRes = Parent::correctMergedSpills(
                    spillsToMerge, maximumNumberOfIterators / 2, spillsInParallel);
                spillsToMerge = spillsRes.first;
                spillsDone += spillsRes.second;
            }
        }
        spillsToMerge += (documentsToAdd > 0 ? 1 : 0);
        spillsDone += (documentsToAdd > 0 ? 1 : 0);

        // Merge spills to satisfy the maximum number of chunks to fit in memory constraint.
        auto spillsRes =
            Parent::correctMergedSpills(spillsToMerge, spillsInParallel, spillsInParallel);
        spillsDone += spillsRes.second;

        return spillsDone;
    }

    enum {
        MEM_LIMIT = 16 * 1024,  // The total memory limit.
        // The memory limit after subtracting the memory reserved for the file iterators.
        DATA_MEM_LIMIT = MEM_LIMIT - static_cast<int>(MEM_LIMIT / 10),
    };
};

class ManualSpills : public Basic {
public:
    // Using constant seed for tests to be determenistic
    ManualSpills() : _random(1) {
        for (size_t i = 0; i < kElementCount; i++) {
            _array[i] = i;
        }
        std::shuffle(_array.begin(), _array.end(), _random.urbg());
    }

    void addData(IWSorter* sorter) override {
        for (size_t i = 0; i < kElementCount; ++i) {
            sorter->add(_array[i], -_array[i]);
            if (i % kSpillEveryN == kSpillEveryN - 1) {
                sorter->spill();
            }
        }
    }

    size_t numAdded() const override {
        return kElementCount;
    }

    std::shared_ptr<IWIterator> correct() override {
        return std::make_shared<IntIterator>(0, kElementCount);
    }
    std::shared_ptr<IWIterator> correctReverse() override {
        return std::make_shared<IntIterator>(kElementCount - 1, -1, -1);
    }

    size_t correctNumRanges() const override {
        return kElementCount / kSpillEveryN;
    }

    size_t correctSpilledRanges() const override {
        return correctNumRanges();
    }

protected:
    static constexpr size_t kElementCount = 100;
    static constexpr size_t kSpillEveryN = 10;

    std::array<int, kElementCount> _array;
    PseudoRandom _random;
};

class ManualSpillsWithLimit : public ManualSpills {
    SortOptions adjustSortOptions(SortOptions opts) override {
        return opts.Limit(kElementCount / 2);
    }

    std::shared_ptr<IWIterator> correct() override {
        return std::make_shared<IntIterator>(0, kLimit);
    }

    std::shared_ptr<IWIterator> correctReverse() override {
        return std::make_shared<IntIterator>(kElementCount - 1, kElementCount - kLimit - 1, -1);
    }

protected:
    static constexpr size_t kLimit = kElementCount / 2;
};

}  // namespace SorterTests

class SorterSuite : public unittest::OldStyleSuiteSpecification {
public:
    SorterSuite() : unittest::OldStyleSuiteSpecification("sorter") {}

    template <typename T>
    static constexpr uint64_t kMaxAsU64 = std::numeric_limits<T>::max();

    void setupTests() override {
        add<SortedFileWriterAndFileIteratorTests>();
        add<MergeIteratorTests>();
        add<SorterTests::Basic>();
        add<SorterTests::Limit>();
        add<SorterTests::Dupes>();
        add<SorterTests::LotsOfDataLittleMemory</*random=*/false>>();
        add<SorterTests::LotsOfDataLittleMemory</*random=*/true>>();
        add<SorterTests::LotsOfSpillsLittleMemory</*random=*/false>>();
        add<SorterTests::LotsOfSpillsLittleMemory</*random=*/true>>();
        add<SorterTests::LotsOfDataWithLimit<1, /*random=*/false>>();     // limit=1 is special case
        add<SorterTests::LotsOfDataWithLimit<1, /*random=*/true>>();      // limit=1 is special case
        add<SorterTests::LotsOfDataWithLimit<100, /*random=*/false>>();   // fits in mem
        add<SorterTests::LotsOfDataWithLimit<100, /*random=*/true>>();    // fits in mem
        add<SorterTests::LotsOfDataWithLimit<5000, /*random=*/false>>();  // spills
        add<SorterTests::LotsOfDataWithLimit<5000, /*random=*/true>>();   // spills
        add<SorterTests::LimitExtreme<kMaxAsU64<uint32_t>>>();
        add<SorterTests::LimitExtreme<kMaxAsU64<uint32_t> - 1>>();
        add<SorterTests::LimitExtreme<kMaxAsU64<uint32_t> + 1>>();
        add<SorterTests::LimitExtreme<kMaxAsU64<uint32_t> / 8 + 1>>();
        add<SorterTests::LimitExtreme<kMaxAsU64<int32_t>>>();
        add<SorterTests::LimitExtreme<kMaxAsU64<int32_t> - 1>>();
        add<SorterTests::LimitExtreme<kMaxAsU64<int32_t> + 1>>();
        add<SorterTests::LimitExtreme<kMaxAsU64<int32_t> / 8 + 1>>();
        add<SorterTests::LimitExtreme<kMaxAsU64<uint64_t>>>();
        add<SorterTests::LimitExtreme<kMaxAsU64<uint64_t> - 1>>();
        add<SorterTests::LimitExtreme<0>>();  // kMaxAsU64<uint64_t> + 1
        add<SorterTests::LimitExtreme<kMaxAsU64<uint64_t> / 8 + 1>>();
        add<SorterTests::LimitExtreme<kMaxAsU64<int64_t>>>();
        add<SorterTests::LimitExtreme<kMaxAsU64<int64_t> - 1>>();
        add<SorterTests::LimitExtreme<kMaxAsU64<int64_t> + 1>>();
        add<SorterTests::LimitExtreme<kMaxAsU64<int64_t> / 8 + 1>>();
        add<SorterTests::PauseAndResume>();
        add<SorterTests::PauseAndResumeLimit>();
        add<SorterTests::PauseAndResumeLimitOne>();
        add<SorterTests::ManualSpills>();
        add<SorterTests::ManualSpillsWithLimit>();
    }
};

unittest::OldStyleSuiteInitializer<SorterSuite> extSortTests;
}  // namespace mongo::sorter
