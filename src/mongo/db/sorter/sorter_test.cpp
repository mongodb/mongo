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

#include <boost/filesystem/directory.hpp>
#include <boost/filesystem/operations.hpp>
#include <boost/filesystem/path.hpp>
#include <boost/iterator/iterator_facade.hpp>
#include <climits>
#include <ctime>
#include <fmt/format.h>
#include <fstream>  // IWYU pragma: keep
#include <memory>
#include <span>

#include "mongo/base/data_type_endian.h"
#include "mongo/base/static_assert.h"
#include "mongo/config.h"  // IWYU pragma: keep
#include "mongo/db/sorter/sorter.h"
#include "mongo/logv2/log.h"
#include "mongo/platform/random.h"
#include "mongo/stdx/thread.h"  // IWYU pragma: keep
#include "mongo/unittest/assert.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/framework.h"
#include "mongo/unittest/temp_dir.h"


// Need access to internal classes
#include "mongo/db/sorter/sorter.cpp"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest


namespace mongo {
namespace sorter {
namespace {

//
// Sorter framework testing utilities
//

class IntWrapper {
public:
    IntWrapper(int i = 0) : _i(i) {}
    operator const int&() const {
        return _i;
    }

    /// members for Sorter
    struct SorterDeserializeSettings {};  // unused
    void serializeForSorter(BufBuilder& buf) const {
        buf.appendNum(_i);
    }
    static IntWrapper deserializeForSorter(BufReader& buf, const SorterDeserializeSettings&) {
        return buf.read<LittleEndian<int>>().value;
    }
    int memUsageForSorter() const {
        return sizeof(IntWrapper);
    }
    IntWrapper getOwned() const {
        return *this;
    }
    void makeOwned() {}

    std::string toString() const {
        return std::to_string(_i);
    }

private:
    int _i;
};

typedef std::pair<IntWrapper, IntWrapper> IWPair;
typedef SortIteratorInterface<IntWrapper, IntWrapper> IWIterator;
typedef Sorter<IntWrapper, IntWrapper> IWSorter;

enum Direction { ASC = 1, DESC = -1 };
class IWComparator {
public:
    IWComparator(Direction dir = ASC) : _dir(dir) {}
    int operator()(const IntWrapper& lhs, const IntWrapper& rhs) const {
        if (lhs == rhs)
            return 0;
        if (lhs < rhs)
            return -1 * _dir;
        return 1 * _dir;
    }

private:
    Direction _dir;
};

class IntIterator : public IWIterator {
public:
    IntIterator(int start = 0, int stop = INT_MAX, int increment = 1)
        : _current(start), _increment(increment), _stop(stop) {}
    void openSource() override {}
    void closeSource() override {}
    bool more() override {
        if (_increment == 0)
            return true;
        if (_increment > 0)
            return _current < _stop;
        return _current > _stop;
    }
    IWPair next() override {
        IWPair out(_current, -_current);
        _current += _increment;
        return out;
    }
    IntWrapper nextWithDeferredValue() override {
        MONGO_UNREACHABLE;
    }
    IntWrapper getDeferredValue() override {
        MONGO_UNREACHABLE;
    }
    const IntWrapper& current() override {
        MONGO_UNREACHABLE;
    }

private:
    int _current;
    int _increment;
    int _stop;
};

class EmptyIterator : public IWIterator {
public:
    void openSource() override {}
    void closeSource() override {}
    bool more() override {
        return false;
    }
    Data next() override {
        MONGO_UNREACHABLE;
    }
    IntWrapper nextWithDeferredValue() override {
        MONGO_UNREACHABLE;
    }
    IntWrapper getDeferredValue() override {
        MONGO_UNREACHABLE;
    }
    const IntWrapper& current() override {
        MONGO_UNREACHABLE;
    }
};

class LimitIterator : public IWIterator {
public:
    LimitIterator(long long limit, std::shared_ptr<IWIterator> source)
        : _remaining(limit), _source(source) {
        invariant(limit > 0);
    }

    void openSource() override {}
    void closeSource() override {}

    bool more() override {
        return _remaining && _source->more();
    }
    Data next() override {
        invariant(more());
        _remaining--;
        return _source->next();
    }
    IntWrapper nextWithDeferredValue() override {
        MONGO_UNREACHABLE;
    }
    IntWrapper getDeferredValue() override {
        MONGO_UNREACHABLE;
    }
    const IntWrapper& current() override {
        MONGO_UNREACHABLE;
    }

private:
    long long _remaining;
    std::shared_ptr<IWIterator> _source;
};

template <typename It1, typename It2>
void _assertIteratorsEquivalent(It1&& it1, It2&& it2, int line) {
    int iteration;
    try {
        it1->openSource();
        it2->openSource();
        for (iteration = 0; true; iteration++) {
            ASSERT_EQUALS(it1->more(), it2->more());
            ASSERT_EQUALS(it1->more(), it2->more());  // make sure more() is safe to call twice
            if (!it1->more())
                return;

            IWPair pair1 = it1->next();
            IWPair pair2 = it2->next();
            ASSERT_EQUALS(pair1.first, pair2.first);
            ASSERT_EQUALS(pair1.second, pair2.second);
        }
        it1->closeSource();
        it2->closeSource();
    } catch (...) {
        LOGV2(22047,
              "Failure from line {line} on iteration {iteration}",
              "line"_attr = line,
              "iteration"_attr = iteration);
        it1->closeSource();
        it2->closeSource();
        throw;
    }
}
#define ASSERT_ITERATORS_EQUIVALENT(it1, it2) _assertIteratorsEquivalent(it1, it2, __LINE__)

template <typename It1, typename It2>
void _assertIteratorsEquivalentForNSteps(It1& it1, It2& it2, int maxSteps, int line) {
    int iteration;
    try {
        it1->openSource();
        it2->openSource();
        for (iteration = 0; iteration < maxSteps; iteration++) {
            ASSERT_EQUALS(it1->more(), it2->more());
            ASSERT_EQUALS(it1->more(), it2->more());  // make sure more() is safe to call twice
            if (!it1->more())
                return;

            IWPair pair1 = it1->next();
            IWPair pair2 = it2->next();
            ASSERT_EQUALS(pair1.first, pair2.first);
            ASSERT_EQUALS(pair1.second, pair2.second);
        }
        it1->closeSource();
        it2->closeSource();
    } catch (...) {
        LOGV2(6409300,
              "Failure from line {line} on iteration {iteration}",
              "line"_attr = line,
              "iteration"_attr = iteration);
        it1->closeSource();
        it2->closeSource();
        throw;
    }
}
#define ASSERT_ITERATORS_EQUIVALENT_FOR_N_STEPS(it1, it2, n) \
    _assertIteratorsEquivalentForNSteps(it1, it2, n, __LINE__)

template <int N>
std::shared_ptr<IWIterator> makeInMemIterator(const int (&array)[N]) {
    std::vector<IWPair> vec;
    for (int i = 0; i < N; i++)
        vec.push_back(IWPair(array[i], -array[i]));
    return std::make_shared<sorter::InMemIterator<IntWrapper, IntWrapper>>(vec);
}

/**
 * Spills the contents of inputIter to a file and returns a FileIterator for reading the data back.
 * This is needed because the MergeIterator currently requires that it is merging from sorted spill
 * file segments (as opposed to any other kind of iterator).
 */
template <typename IteratorPtr>
std::shared_ptr<IWIterator> spillToFile(IteratorPtr inputIter, const unittest::TempDir& tempDir) {
    inputIter->openSource();
    if (!inputIter->more()) {
        inputIter->closeSource();
        return std::make_shared<EmptyIterator>();
    }
    SorterFileStats sorterFileStats(nullptr /* sorterTracker */);
    const SortOptions opts = SortOptions().TempDir(tempDir.path());
    auto spillFile = std::make_shared<Sorter<IntWrapper, IntWrapper>::File>(
        sorter::nextFileName(opts.tempDir), opts.sorterFileStats);
    SortedFileWriter<IntWrapper, IntWrapper> writer(opts, spillFile);
    while (inputIter->more()) {
        auto pair = inputIter->next();
        writer.addAlreadySorted(pair.first, pair.second);
    }
    auto outputIter = writer.done();
    inputIter->closeSource();
    return outputIter;
}

template <typename IteratorPtr, int N>
std::shared_ptr<IWIterator> mergeIterators(IteratorPtr (&array)[N],
                                           const unittest::TempDir& tempDir,
                                           Direction Dir = ASC,
                                           const SortOptions& opts = SortOptions()) {
    invariant(!opts.extSortAllowed);
    std::vector<std::shared_ptr<IWIterator>> vec;
    for (auto& it : array) {
        // Spill iterator outputs to a file and obtain a new iterator for it.
        vec.push_back(spillToFile(std::move(it), tempDir));
    }
    return IWIterator::merge(vec, opts, IWComparator(Dir));
}

//
// Tests for Sorter framework internals
//

class InMemIterTests {
public:
    void run() {
        {
            EmptyIterator empty;
            sorter::InMemIterator<IntWrapper, IntWrapper> inMem;
            ASSERT_ITERATORS_EQUIVALENT(&inMem, &empty);
        }
        {
            static const int zeroUpTo20[] = {0,  1,  2,  3,  4,  5,  6,  7,  8,  9,
                                             10, 11, 12, 13, 14, 15, 16, 17, 18, 19};
            ASSERT_ITERATORS_EQUIVALENT(makeInMemIterator(zeroUpTo20),
                                        std::make_shared<IntIterator>(0, 20));
        }
        {
            // make sure InMemIterator doesn't do any reordering on it's own
            static const int unsorted[] = {6, 3, 7, 4, 0, 9, 5, 7, 1, 8};
            class UnsortedIter : public IWIterator {
            public:
                UnsortedIter() : _pos(0) {}
                void openSource() override {}
                void closeSource() override {}
                bool more() override {
                    return _pos < sizeof(unsorted) / sizeof(unsorted[0]);
                }
                IWPair next() override {
                    IWPair ret(unsorted[_pos], -unsorted[_pos]);
                    _pos++;
                    return ret;
                }
                IntWrapper nextWithDeferredValue() override {
                    MONGO_UNREACHABLE;
                }
                IntWrapper getDeferredValue() override {
                    MONGO_UNREACHABLE;
                }
                const IntWrapper& current() override {
                    MONGO_UNREACHABLE;
                }
                size_t _pos;
            } unsortedIter;

            ASSERT_ITERATORS_EQUIVALENT(makeInMemIterator(unsorted),
                                        static_cast<IWIterator*>(&unsortedIter));
        }
    }
};

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
                sorter::nextFileName(opts->tempDir), opts->sorterFileStats);
        };

        int currentBufSize = 0;
        SortedFileWriter<IntWrapper, IntWrapper> sorter(*opts, makeFile());
        for (int i = 0; i < range; ++i) {
            sorter.addAlreadySorted(i, -i);
            currentBufSize += sizeof(i) + sizeof(-i);

            if (currentBufSize > static_cast<int>(kSortedFileBufferSize)) {
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
        const SortOptions opts =
            SortOptions().TempDir(tempDir.path()).ExtSortAllowed().Tracker(&sorterTracker);

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
        if (opts.extSortAllowed) {
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

        return opts.MaxMemoryUsageBytes(MEM_LIMIT).ExtSortAllowed();
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
        return std::max(static_cast<std::size_t>(DATA_MEM_LIMIT / kSortedFileBufferSize),
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

        return opts.MaxMemoryUsageBytes(MEM_LIMIT).ExtSortAllowed().Limit(Limit);
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

        return opts.MaxMemoryUsageBytes(MEM_LIMIT).ExtSortAllowed();
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
}  // namespace SorterTests

class SorterSuite : public unittest::OldStyleSuiteSpecification {
public:
    SorterSuite() : unittest::OldStyleSuiteSpecification("sorter") {}

    template <typename T>
    static constexpr uint64_t kMaxAsU64 = std::numeric_limits<T>::max();

    void setupTests() override {
        add<InMemIterTests>();
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
    }
};

unittest::OldStyleSuiteInitializer<SorterSuite> extSortTests;

/**
 * This suite includes test cases for resumable index builds where the Sorter is reconstructed from
 * state persisted to disk during a previous clean shutdown.
 */
class SorterMakeFromExistingRangesTest : public unittest::Test {
public:
    static std::vector<SorterRange> makeSampleRanges();
};

// static
std::vector<SorterRange> SorterMakeFromExistingRangesTest::makeSampleRanges() {
    std::vector<SorterRange> ranges;
    // Sample data extracted from resumable_index_build_bulk_load_phase.js test run.
    ranges.push_back({0, 24, 18294710});
    return ranges;
}

DEATH_TEST_F(
    SorterMakeFromExistingRangesTest,
    NonZeroLimit,
    "Creating a Sorter from existing ranges is only available with the NoLimitSorter (limit 0)") {
    auto opts = SortOptions().Limit(1ULL);
    IWSorter::makeFromExistingRanges("", {}, opts, IWComparator(ASC));
}

DEATH_TEST_F(SorterMakeFromExistingRangesTest, ExtSortNotAllowed, "opts.extSortAllowed") {
    auto opts = SortOptions();
    ASSERT_FALSE(opts.extSortAllowed);
    IWSorter::makeFromExistingRanges("", {}, opts, IWComparator(ASC));
}

DEATH_TEST_F(SorterMakeFromExistingRangesTest, EmptyTempDir, "!opts.tempDir.empty()") {
    auto opts = SortOptions().ExtSortAllowed();
    ASSERT_EQUALS("", opts.tempDir);
    IWSorter::makeFromExistingRanges("", {}, opts, IWComparator(ASC));
}

DEATH_TEST_F(SorterMakeFromExistingRangesTest, EmptyFileName, "!fileName.empty()") {
    std::string fileName;
    auto opts = SortOptions().ExtSortAllowed().TempDir("unused_temp_dir");
    IWSorter::makeFromExistingRanges(fileName, {}, opts, IWComparator(ASC));
}

TEST_F(SorterMakeFromExistingRangesTest, SkipFileCheckingOnEmptyRanges) {
    auto fileName = "unused_sorter_file";
    SorterTracker sorterTracker;
    auto opts = SortOptions().ExtSortAllowed().TempDir("unused_temp_dir").Tracker(&sorterTracker);
    auto sorter = IWSorter::makeFromExistingRanges(fileName, {}, opts, IWComparator(ASC));

    ASSERT_EQ(0, sorter->stats().spilledRanges());

    auto iter = sorter->done();
    ASSERT_EQ(0, sorter->stats().numSorted());

    iter->openSource();
    ASSERT_FALSE(iter->more());
    iter->closeSource();
}

TEST_F(SorterMakeFromExistingRangesTest, MissingFile) {
    auto fileName = "unused_sorter_file";
    auto tempDir = "unused_temp_dir";
    auto opts = SortOptions().ExtSortAllowed().TempDir(tempDir);
    ASSERT_THROWS_WITH_CHECK(
        IWSorter::makeFromExistingRanges(fileName, makeSampleRanges(), opts, IWComparator(ASC)),
        std::exception,
        [&](const auto& ex) {
            ASSERT_STRING_CONTAINS(ex.what(), tempDir);
            ASSERT_STRING_CONTAINS(ex.what(), fileName);
        });
}

TEST_F(SorterMakeFromExistingRangesTest, EmptyFile) {
    unittest::TempDir tempDir(_agent.getSuiteName() + "_" + _agent.getTestName());
    auto tempFilePath = boost::filesystem::path(tempDir.path()) / "empty_sorter_file";
    ASSERT(std::ofstream(tempFilePath.string()))
        << "failed to create empty temporary file: " << tempFilePath.string();
    auto fileName = tempFilePath.filename().string();
    auto opts = SortOptions().ExtSortAllowed().TempDir(tempDir.path());
    // 16815 - unexpected empty file.
    ASSERT_THROWS_CODE(
        IWSorter::makeFromExistingRanges(fileName, makeSampleRanges(), opts, IWComparator(ASC)),
        DBException,
        16815);
}

TEST_F(SorterMakeFromExistingRangesTest, CorruptedFile) {
    unittest::TempDir tempDir(_agent.getSuiteName() + "_" + _agent.getTestName());
    auto tempFilePath = boost::filesystem::path(tempDir.path()) / "corrupted_sorter_file";
    {
        std::ofstream ofs(tempFilePath.string());
        ASSERT(ofs) << "failed to create temporary file: " << tempFilePath.string();
        ofs << "invalid sorter data";
    }
    auto fileName = tempFilePath.filename().string();
    SorterTracker sorterTracker;
    auto opts = SortOptions().ExtSortAllowed().TempDir(tempDir.path()).Tracker(&sorterTracker);
    auto sorter =
        IWSorter::makeFromExistingRanges(fileName, makeSampleRanges(), opts, IWComparator(ASC));

    // The number of spills is set when NoLimitSorter is constructed from existing ranges.
    ASSERT_EQ(makeSampleRanges().size(), sorter->stats().spilledRanges());
    ASSERT_EQ(0, sorter->stats().numSorted());

    // 16817 - error reading file.
    ASSERT_THROWS_CODE(sorter->done(), DBException, 16817);
}

TEST_F(SorterMakeFromExistingRangesTest, RoundTrip) {
    unittest::TempDir tempDir(_agent.getSuiteName() + "_" + _agent.getTestName());
    SorterTracker sorterTracker;

    auto opts = SortOptions()
                    .ExtSortAllowed()
                    .TempDir(tempDir.path())
                    .MaxMemoryUsageBytes(
                        sizeof(IWSorter::Data) +
                        MergeableSorter<IntWrapper, IntWrapper, IWComparator>::kFileIteratorSize)
                    .Tracker(&sorterTracker);

    IWPair pairInsertedBeforeShutdown(1, 100);

    // This test uses two sorters. The first sorter is used to persist data to disk in a shutdown
    // scenario. On startup, we will restore the original state of the sorter using the persisted
    // data.
    IWSorter::PersistedState state;
    {
        auto sorterBeforeShutdown = IWSorter::make(opts, IWComparator(ASC));
        sorterBeforeShutdown->add(pairInsertedBeforeShutdown.first,
                                  pairInsertedBeforeShutdown.second);
        state = sorterBeforeShutdown->persistDataForShutdown();
        ASSERT_FALSE(state.fileName.empty());
        ASSERT_EQUALS(1U, state.ranges.size()) << state.ranges.size();
        ASSERT_EQ(1, sorterBeforeShutdown->stats().numSorted());
    }

    // On restart, reconstruct sorter from persisted state.
    auto sorter =
        IWSorter::makeFromExistingRanges(state.fileName, state.ranges, opts, IWComparator(ASC));

    // The number of spills is set when NoLimitSorter is constructed from existing ranges.
    ASSERT_EQ(state.ranges.size(), sorter->stats().spilledRanges());

    // Ensure that the restored sorter can accept additional data.
    IWPair pairInsertedAfterStartup(2, 200);
    sorter->add(pairInsertedAfterStartup.first, pairInsertedAfterStartup.second);

    // Technically this sorter has not sorted anything. It is just merging files.
    ASSERT_EQ(0, sorter->stats().numSorted());

    // Read data from sorter.
    {
        auto iter = sorter->done();
        iter->openSource();

        ASSERT(iter->more());
        auto pair1 = iter->next();
        ASSERT_EQUALS(pairInsertedBeforeShutdown.first, pair1.first)
            << pair1.first << "/" << pair1.second;
        ASSERT_EQUALS(pairInsertedBeforeShutdown.second, pair1.second)
            << pair1.first << "/" << pair1.second;

        ASSERT(iter->more());
        auto pair2 = iter->next();
        ASSERT_EQUALS(pairInsertedAfterStartup.first, pair2.first)
            << pair2.first << "/" << pair2.second;
        ASSERT_EQUALS(pairInsertedAfterStartup.second, pair2.second)
            << pair2.first << "/" << pair2.second;

        ASSERT_FALSE(iter->more());
        iter->closeSource();
    }
}

TEST_F(SorterMakeFromExistingRangesTest, NextWithDeferredValues) {
    unittest::TempDir tempDir(_agent.getSuiteName() + "_" + _agent.getTestName());
    auto opts = SortOptions().ExtSortAllowed().TempDir(tempDir.path());

    IWPair pair1(1, 100);
    IWPair pair2(2, 200);
    auto spillFile = std::make_shared<Sorter<IntWrapper, IntWrapper>::File>(
        sorter::nextFileName(opts.tempDir), opts.sorterFileStats);
    SortedFileWriter<IntWrapper, IntWrapper> writer(opts, std::move(spillFile));
    writer.addAlreadySorted(pair1.first, pair1.second);
    writer.addAlreadySorted(pair2.first, pair2.second);
    auto iter = writer.done();
    iter->openSource();

    ASSERT(iter->more());
    IntWrapper key1 = iter->nextWithDeferredValue();
    IntWrapper value1 = iter->getDeferredValue();
    ASSERT_EQUALS(pair1.first, key1);
    ASSERT_EQUALS(pair1.second, value1);

    ASSERT(iter->more());
    IntWrapper key2 = iter->nextWithDeferredValue();
    IntWrapper value2 = iter->getDeferredValue();
    ASSERT_EQUALS(pair2.first, key2);
    ASSERT_EQUALS(pair2.second, value2);

    ASSERT_FALSE(iter->more());
    iter->closeSource();
}

TEST_F(SorterMakeFromExistingRangesTest, ChecksumVersion) {
    unittest::TempDir tempDir(_agent.getSuiteName() + "_" + _agent.getTestName());
    auto opts = SortOptions().ExtSortAllowed().TempDir(tempDir.path());

    // By default checksum version should be v2
    {
        auto sorter = IWSorter::make(opts, IWComparator(ASC));
        sorter->add(1, -1);
        auto state = sorter->persistDataForShutdown();
        ASSERT_EQUALS(state.ranges[0].getChecksumVersion(), SorterChecksumVersion::v2);
        ASSERT_EQUALS(state.ranges[0].getChecksum(), 1921809301);
    }

    // Setting checksum version to v1 results in using v1 but getChecksumVersion() returning none
    // because v1 did not persist a version.
    {
        opts.ChecksumVersion(SorterChecksumVersion::v1);
        auto sorter = IWSorter::make(opts, IWComparator(ASC));
        sorter->add(1, -1);
        auto state = sorter->persistDataForShutdown();
        ASSERT_EQUALS(state.ranges[0].getChecksumVersion(), boost::none);
        ASSERT_EQUALS(state.ranges[0].getChecksum(), 4121002018);
    }
}

struct SpillFileState {
    std::string fileName;
    std::vector<SorterRange> ranges;
    SortOptions opts;
    IWComparator comp{ASC};
};

SpillFileState makeSpillFile(unittest::TempDir& tempDir) {
    SpillFileState ret;
    ret.opts = SortOptions().ExtSortAllowed().TempDir(tempDir.path());

    auto sorter = IWSorter::make(ret.opts, ret.comp);
    for (int i = 0; i < 10; ++i)
        sorter->add(i, -i);
    auto state = sorter->persistDataForShutdown();
    ret.fileName = std::move(state.fileName);
    ret.ranges = std::move(state.ranges);
    return ret;
}

void corruptChecksum(SpillFileState& state) {
    auto& range = state.ranges[0];
    range.setChecksum(range.getChecksum() ^ 1);
}

TEST_F(SorterMakeFromExistingRangesTest, ValidChecksumValidation) {
    unittest::TempDir tempDir(_agent.getSuiteName() + "_" + _agent.getTestName());
    auto state = makeSpillFile(tempDir);
    auto it = IWSorter::makeFromExistingRanges(state.fileName, state.ranges, state.opts, state.comp)
                  ->done();
    ASSERT_ITERATORS_EQUIVALENT(it, std::make_unique<IntIterator>(0, 10));
}

TEST_F(SorterMakeFromExistingRangesTest, IncompleteReadDoesNotReportChecksumError) {
    unittest::TempDir tempDir(_agent.getSuiteName() + "_" + _agent.getTestName());
    auto state = makeSpillFile(tempDir);
    corruptChecksum(state);
    auto it = IWSorter::makeFromExistingRanges(state.fileName, state.ranges, state.opts, state.comp)
                  ->done();
    // Read the first (and only) block of data, but don't deserialize any of it
    ASSERT(it->more());
    // it's destructor doesn't check the checksum since we didn't use everything
}

DEATH_TEST_F(SorterMakeFromExistingRangesTest,
             CompleteReadReportsChecksumError,
             "Data read from disk does not match what was written to disk.") {
    unittest::TempDir tempDir(_agent.getSuiteName() + "_" + _agent.getTestName());
    auto state = makeSpillFile(tempDir);
    corruptChecksum(state);
    auto it = IWSorter::makeFromExistingRanges(state.fileName, state.ranges, state.opts, state.comp)
                  ->done();
    ASSERT_ITERATORS_EQUIVALENT(it, std::make_unique<IntIterator>(0, 10));
    // it's destructor ends up checking the checksum and aborts due to it being wrong
}

DEATH_TEST_F(SorterMakeFromExistingRangesTest,
             CompleteReadReportsChecksumErrorFromIncorrectChecksumVersion,
             "Data read from disk does not match what was written to disk.") {
    unittest::TempDir tempDir(_agent.getSuiteName() + "_" + _agent.getTestName());
    auto state = makeSpillFile(tempDir);
    state.ranges[0].setChecksumVersion(boost::none);
    auto it = IWSorter::makeFromExistingRanges(state.fileName, state.ranges, state.opts, state.comp)
                  ->done();
    ASSERT_ITERATORS_EQUIVALENT(it, std::make_unique<IntIterator>(0, 10));
    // it's destructor ends up checking the checksum and aborts due to it being wrong (because we
    // used the wrong checksum algorithm)
}

class BoundedSorterTest : public unittest::Test {
public:
    using Key = IntWrapper;
    struct Doc {
        Key time;

        bool operator==(const Doc& other) const {
            return time == other.time;
        }

        void serializeForSorter(BufBuilder& buf) const {
            time.serializeForSorter(buf);
        }

        struct SorterDeserializeSettings {};  // unused
        static Doc deserializeForSorter(BufReader& buf, const SorterDeserializeSettings&) {
            return {IntWrapper::deserializeForSorter(buf, {})};
        }

        int memUsageForSorter() const {
            return sizeof(Doc);
        }
    };
    struct ComparatorAsc {
        int operator()(Key x, Key y) const {
            return x - y;
        }
    };
    struct ComparatorDesc {
        int operator()(Key x, Key y) const {
            return y - x;
        }
    };
    struct BoundMakerAsc {
        Key operator()(Key k, const Doc&) const {
            return k - 10;
        }
        Document serialize(const SerializationOptions& opts = {}) const {
            MONGO_UNREACHABLE;
        }
    };
    struct BoundMakerDesc {
        Key operator()(Key k, const Doc&) const {
            return k + 10;
        }
        Document serialize(const SerializationOptions& opts = {}) const {
            MONGO_UNREACHABLE;
        }
    };

    using S = BoundedSorterInterface<Key, Doc>;
    using SAsc = BoundedSorter<Key, Doc, ComparatorAsc, BoundMakerAsc>;
    using SDesc = BoundedSorter<Key, Doc, ComparatorDesc, BoundMakerDesc>;

    /**
     * Feed the input into the sorter one-by-one, taking any output as soon as it's available.
     */
    std::vector<Doc> sort(std::vector<Doc> input, int expectedSize = -1) {
        std::vector<Doc> output;
        auto push = [&](Doc doc) {
            output.push_back(doc);
        };

        for (auto&& doc : input) {
            sorter->add(doc.time, doc);
            while (sorter->getState() == S::State::kReady)
                push(sorter->next().second);
        }
        sorter->done();

        while (sorter->getState() == S::State::kReady)
            push(sorter->next().second);
        ASSERT(sorter->getState() == S::State::kDone);

        ASSERT_EQ(output.size(), expectedSize == -1 ? input.size() : expectedSize);
        return output;
    }

    static void assertSorted(const std::vector<Doc>& docs, bool ascending = true) {
        for (size_t i = 1; i < docs.size(); ++i) {
            Doc prev = docs[i - 1];
            Doc curr = docs[i];
            if (ascending) {
                ASSERT_LTE(prev.time, curr.time);
            } else {
                ASSERT_GTE(prev.time, curr.time);
            }
        }
    }

    std::unique_ptr<S> makeAsc(SortOptions options, bool checkInput = true) {
        return std::make_unique<SAsc>(options, ComparatorAsc{}, BoundMakerAsc{}, checkInput);
    }
    std::unique_ptr<S> makeDesc(SortOptions options, bool checkInput = true) {
        return std::make_unique<SDesc>(options, ComparatorDesc{}, BoundMakerDesc{}, checkInput);
    }

    std::unique_ptr<S> sorter = makeAsc({});
};
TEST_F(BoundedSorterTest, Empty) {
    ASSERT(sorter->getState() == S::State::kWait);

    sorter->done();
    ASSERT(sorter->getState() == S::State::kDone);
}
TEST_F(BoundedSorterTest, Sorted) {
    auto output = sort({
        {0},
        {3},
        {10},
        {11},
        {12},
        {13},
        {14},
        {15},
        {16},
    });
    assertSorted(output);
}

TEST_F(BoundedSorterTest, SortedExceptOne) {
    auto output = sort({
        {0},
        {3},
        {10},
        // Swap 11 and 12.
        {12},
        {11},
        {13},
        {14},
        {15},
        {16},
    });
    assertSorted(output);
}

TEST_F(BoundedSorterTest, AlmostSorted) {
    auto output = sort({
        // 0 and 11 cannot swap.
        {0},
        {11},
        {13},
        {10},
        {12},
        // 3 and 14 cannot swap.
        {3},
        {14},
        {15},
        {16},
    });
    assertSorted(output);
}

TEST_F(BoundedSorterTest, WrongInput) {
    std::vector<Doc> input = {
        {3},
        {4},
        {5},
        {10},
        {15},
        // This 1 is too far out of order: it's more than 10 away from 15.
        // So it will appear too late in the output.
        // We will still be hanging on to anything in the range [5, inf).
        // So we will have already returned 3, 4.
        {1},
        {16},
    };

    // Disable input order checking so we can see what happens.
    sorter = makeAsc({}, /* checkInput */ false);
    auto output = sort(input);
    ASSERT_EQ(output.size(), 7);

    ASSERT_EQ(output[0].time, 3);
    ASSERT_EQ(output[1].time, 4);
    ASSERT_EQ(output[2].time, 1);  // Out of order.
    ASSERT_EQ(output[3].time, 5);
    ASSERT_EQ(output[4].time, 10);
    ASSERT_EQ(output[5].time, 15);
    ASSERT_EQ(output[6].time, 16);

    // Test that by default, bad input like this would be detected.
    sorter = makeAsc({});
    ASSERT(sorter->checkInput());
    ASSERT_THROWS_CODE(sort(input), DBException, 6369910);
}

TEST_F(BoundedSorterTest, MemoryLimitsNoExtSortAllowed) {
    auto options = SortOptions().MaxMemoryUsageBytes(16);
    sorter = makeAsc(options);

    std::vector<Doc> input = {
        {0},
        {3},
        {10},
        {11},
        {12},
        {13},
        {14},
        {15},
        {16},
    };

    ASSERT_THROWS_CODE(
        sort(input), DBException, ErrorCodes::QueryExceededMemoryLimitNoDiskUseAllowed);
}

TEST_F(BoundedSorterTest, SpillSorted) {
    SorterTracker sorterTracker;
    auto options = SortOptions()
                       .ExtSortAllowed()
                       .TempDir("unused_temp_dir")
                       .MaxMemoryUsageBytes(16)
                       .Tracker(&sorterTracker);
    sorter = makeAsc(options);

    auto output = sort({
        {0},
        {3},
        {10},
        {11},
        {12},
        {13},
        {14},
        {15},
        {16},
    });
    assertSorted(output);

    ASSERT_EQ(sorter->stats().spilledRanges(), 3);
}

TEST_F(BoundedSorterTest, SpillSortedExceptOne) {
    auto options =
        SortOptions().ExtSortAllowed().TempDir("unused_temp_dir").MaxMemoryUsageBytes(16);
    sorter = makeAsc(options);

    auto output = sort({
        {0},
        {3},
        {10},
        // Swap 11 and 12.
        {12},
        {11},
        {13},
        {14},
        {15},
        {16},
    });
    assertSorted(output);

    ASSERT_EQ(sorter->stats().spilledRanges(), 3);
}

TEST_F(BoundedSorterTest, SpillAlmostSorted) {
    SorterTracker sorterTracker;
    auto options = SortOptions()
                       .ExtSortAllowed()
                       .TempDir("unused_temp_dir")
                       .MaxMemoryUsageBytes(16)
                       .Tracker(&sorterTracker);
    sorter = makeAsc(options);

    auto output = sort({
        // 0 and 11 cannot swap.
        {0},
        {11},
        {13},
        {10},
        {12},
        // 3 and 14 cannot swap.
        {3},
        {14},
        {15},
        {16},
    });
    assertSorted(output);

    ASSERT_EQ(sorter->stats().spilledRanges(), 2);
}

TEST_F(BoundedSorterTest, SpillWrongInput) {
    auto options =
        SortOptions().ExtSortAllowed().TempDir("unused_temp_dir").MaxMemoryUsageBytes(16);

    std::vector<Doc> input = {
        {3},
        {4},
        {5},
        {10},
        {15},
        // This 1 is too far out of order: it's more than 10 away from 15.
        // So it will appear too late in the output.
        // We will still be hanging on to anything in the range [5, inf).
        // So we will have already returned 3, 4.
        {1},
        {16},
    };

    // Disable input order checking so we can see what happens.
    sorter = makeAsc(options, /* checkInput */ false);
    auto output = sort(input);
    ASSERT_EQ(output.size(), 7);

    ASSERT_EQ(output[0].time, 3);
    ASSERT_EQ(output[1].time, 4);
    ASSERT_EQ(output[2].time, 1);  // Out of order.
    ASSERT_EQ(output[3].time, 5);
    ASSERT_EQ(output[4].time, 10);
    ASSERT_EQ(output[5].time, 15);
    ASSERT_EQ(output[6].time, 16);

    ASSERT_EQ(sorter->stats().spilledRanges(), 2);

    // Test that by default, bad input like this would be detected.
    sorter = makeAsc(options);
    ASSERT(sorter->checkInput());
    ASSERT_THROWS_CODE(sort(input), DBException, 6369910);
}

TEST_F(BoundedSorterTest, LimitNoSpill) {
    SorterTracker sorterTracker;
    auto options = SortOptions()
                       .ExtSortAllowed()
                       .TempDir("unused_temp_dir")
                       .MaxMemoryUsageBytes(40)
                       .Tracker(&sorterTracker)
                       .Limit(2);
    sorter = makeAsc(options);

    auto output = sort(
        {
            // 0 and 11 cannot swap.
            {0},
            {11},
            {13},
            {10},
            {12},
            // 3 and 14 cannot swap.
            {3},
            {14},
            {15},
            {16},
        },
        2);
    assertSorted(output);
    // Also check that the correct values made it into the top K.
    ASSERT_EQ(output[0].time, 0);
    ASSERT_EQ(output[1].time, 3);

    ASSERT_EQ(sorter->stats().spilledRanges(), 0);
}

TEST_F(BoundedSorterTest, LimitSpill) {
    SorterTracker sorterTracker;
    auto options = SortOptions()
                       .ExtSortAllowed()
                       .TempDir("unused_temp_dir")
                       .MaxMemoryUsageBytes(40)
                       .Tracker(&sorterTracker)
                       .Limit(3);
    sorter = makeAsc(options);

    auto output = sort(
        {
            // 0 and 11 cannot swap.
            {0},
            {11},
            {13},
            {10},
            {12},
            // 3 and 14 cannot swap.
            {3},
            {14},
            {15},
            {16},
        },
        3);
    assertSorted(output);
    // Also check that the correct values made it into the top K.
    ASSERT_EQ(output[0].time, 0);
    ASSERT_EQ(output[1].time, 3);
    ASSERT_EQ(output[2].time, 10);

    ASSERT_EQ(sorter->stats().spilledRanges(), 1);
}

TEST_F(BoundedSorterTest, DescSorted) {
    sorter = makeDesc({});
    auto output = sort({
        {16},
        {15},
        {14},
        {13},
        {12},
        {11},
        {10},
        {3},
        {0},
    });
    assertSorted(output, /* ascending */ false);
}

TEST_F(BoundedSorterTest, DescSortedExceptOne) {
    sorter = makeDesc({});

    auto output = sort({

        {16},
        {15},
        {14},
        {13},
        // Swap 11 and 12.
        {11},
        {12},
        {10},
        {3},
        {0},
    });
    assertSorted(output, /* ascending */ false);
}

TEST_F(BoundedSorterTest, DescAlmostSorted) {
    sorter = makeDesc({});

    auto output = sort({
        {16},
        {15},
        // 3 and 14 cannot swap.
        {14},
        {3},
        {12},
        {10},
        {13},
        // 0 and 11 cannot swap.
        {11},
        {0},
    });
    assertSorted(output, /* ascending */ false);
}

TEST_F(BoundedSorterTest, DescWrongInput) {
    std::vector<Doc> input = {
        {16},
        {14},
        {10},
        {5},
        {3},
        // This 15 is too far out of order: it's more than 10 away from 3.
        // So it will appear too late in the output.
        // We will still be hanging on to anything in the range [-inf, 13).
        // So we will have already returned 16, 14.
        {15},
        {1},
    };

    // Disable input order checking so we can see what happens.
    sorter = makeDesc({}, /* checkInput */ false);
    auto output = sort(input);
    ASSERT_EQ(output.size(), 7);

    ASSERT_EQ(output[0].time, 16);
    ASSERT_EQ(output[1].time, 14);
    ASSERT_EQ(output[2].time, 15);  // Out of order.
    ASSERT_EQ(output[3].time, 10);
    ASSERT_EQ(output[4].time, 5);
    ASSERT_EQ(output[5].time, 3);
    ASSERT_EQ(output[6].time, 1);

    // Test that by default, bad input like this would be detected.
    sorter = makeDesc({});
    ASSERT(sorter->checkInput());
    ASSERT_THROWS_CODE(sort(input), DBException, 6369910);
}

TEST_F(BoundedSorterTest, CompoundAsc) {
    {
        auto output = sort({
            {1001},
            {1005},
            {1004},
            {1007},
        });
        assertSorted(output);
    }

    {
        // After restart(), the sorter accepts new input.
        // The new values are compared to each other, but not compared to any of the old values,
        // so it's fine for the new values to be smaller even though the sort is ascending.
        sorter->restart();
        auto output = sort({
            {1},
            {5},
            {4},
            {7},
        });
        assertSorted(output);
    }

    {
        // restart() can be called any number of times.
        sorter->restart();
        auto output = sort({
            {11},
            {15},
            {14},
            {17},
        });
        assertSorted(output);
    }
}

TEST_F(BoundedSorterTest, CompoundDesc) {
    sorter = makeDesc({});
    {
        auto output = sort({
            {1007},
            {1004},
            {1005},
            {1001},
        });
        assertSorted(output, /* ascending */ false);
    }

    {
        // After restart(), the sorter accepts new input.
        // The new values are compared to each other, but not compared to any of the old values,
        // so it's fine for the new values to be smaller even though the sort is ascending.
        sorter->restart();
        auto output = sort({
            {7},
            {4},
            {5},
            {1},
        });
        assertSorted(output, /* ascending */ false);
    }

    {
        // restart() can be called any number of times.
        sorter->restart();
        auto output = sort({
            {17},
            {14},
            {15},
            {11},
        });
        assertSorted(output, /* ascending */ false);
    }
}

TEST_F(BoundedSorterTest, CompoundLimit) {
    // A limit applies to the entire sorter, not to each partition of a compound sort.

    // Example where the limit lands in the first partition.
    sorter = makeAsc(SortOptions().Limit(2));
    {
        auto output = sort(
            {
                {1001},
                {1005},
                {1004},
                {1007},
            },
            2);
        assertSorted(output);
        // Also check that the correct values made it into the top K.
        ASSERT_EQ(output[0].time, 1001);
        ASSERT_EQ(output[1].time, 1004);

        sorter->restart();
        output = sort(
            {
                {1},
                {5},
                {4},
                {7},
            },
            0);

        sorter->restart();
        output = sort(
            {
                {11},
                {15},
                {14},
                {17},
            },
            0);
    }

    // Example where the limit lands in the second partition.
    sorter = makeAsc(SortOptions().Limit(6));
    {
        auto output = sort({
            {1001},
            {1005},
            {1004},
            {1007},
        });
        assertSorted(output);

        sorter->restart();
        output = sort(
            {
                {1},
                {5},
                {4},
                {7},
            },
            2);
        // Also check that the correct values made it into the top K.
        ASSERT_EQ(output[0].time, 1);
        ASSERT_EQ(output[1].time, 4);

        sorter->restart();
        output = sort(
            {
                {11},
                {15},
                {14},
                {17},
            },
            0);
    }
}

TEST_F(BoundedSorterTest, CompoundSpill) {
    SorterTracker sorterTracker;
    auto options = SortOptions()
                       .ExtSortAllowed()
                       .TempDir("unused_temp_dir")
                       .Tracker(&sorterTracker)
                       .MaxMemoryUsageBytes(40);
    sorter = makeAsc(options);

    // When each partition is small enough, we don't spill.
    ASSERT_EQ(sorter->stats().spilledRanges(), 0);
    auto output = sort({
        {1001},
        {1007},
    });
    assertSorted(output);
    ASSERT_EQ(sorter->stats().spilledRanges(), 0);

    // If any individual partition is large enough, we do spill.
    sorter->restart();
    ASSERT_EQ(sorter->stats().spilledRanges(), 0);
    output = sort({
        {1},
        {5},
        {5},
        {5},
        {5},
        {5},
        {5},
        {5},
        {5},
        {4},
        {7},
    });
    assertSorted(output);
    ASSERT_EQ(sorter->stats().spilledRanges(), 1);

    // If later partitions are small again, they don't spill.
    sorter->restart();
    ASSERT_EQ(sorter->stats().spilledRanges(), 1);
    output = sort({
        {11},
        {17},
    });
    assertSorted(output);
    ASSERT_EQ(sorter->stats().spilledRanges(), 1);
}

}  // namespace
}  // namespace sorter
}  // namespace mongo

template class ::mongo::Sorter<::mongo::sorter::BoundedSorterTest::Key,
                               ::mongo::sorter::BoundedSorterTest::Doc>;
template class ::mongo::BoundedSorter<::mongo::sorter::BoundedSorterTest::Key,
                                      ::mongo::sorter::BoundedSorterTest::Doc,
                                      ::mongo::sorter::BoundedSorterTest::ComparatorAsc,
                                      ::mongo::sorter::BoundedSorterTest::BoundMakerAsc>;
template class ::mongo::BoundedSorter<::mongo::sorter::BoundedSorterTest::Key,
                                      ::mongo::sorter::BoundedSorterTest::Doc,
                                      ::mongo::sorter::BoundedSorterTest::ComparatorDesc,
                                      ::mongo::sorter::BoundedSorterTest::BoundMakerDesc>;
