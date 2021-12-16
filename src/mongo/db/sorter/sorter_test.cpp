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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest

#include <boost/filesystem.hpp>
#include <fstream>
#include <memory>

#include "mongo/base/data_type_endian.h"
#include "mongo/base/static_assert.h"
#include "mongo/config.h"
#include "mongo/db/sorter/factory.h"
#include "mongo/db/sorter/in_mem_iterator.h"
#include "mongo/db/sorter/single_elem_iterator.h"
#include "mongo/db/sorter/sorted_file_writer.h"
#include "mongo/db/sorter/sorter.h"
#include "mongo/logv2/log.h"
#include "mongo/platform/random.h"
#include "mongo/stdx/thread.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/temp_dir.h"
#include "mongo/unittest/unittest.h"

namespace mongo::sorter {
namespace {

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

private:
    int _i;
};

typedef std::pair<IntWrapper, IntWrapper> IWPair;
typedef SortedDataIterator<IntWrapper, IntWrapper> IWIterator;
typedef Sorter<IntWrapper, IntWrapper> IWSorter;

enum Direction { ASC = 1, DESC = -1 };
class IWComparator {
public:
    IWComparator(Direction dir = ASC) : _dir(dir) {}
    int operator()(const IWPair& lhs, const IWPair& rhs) const {
        if (lhs.first == rhs.first)
            return 0;
        if (lhs.first < rhs.first)
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
    void openSource() {}
    void closeSource() {}
    bool more() const {
        if (_increment == 0)
            return true;
        if (_increment > 0)
            return _current < _stop;
        return _current > _stop;
    }
    IWPair next() {
        IWPair out(_current, -_current);
        _current += _increment;
        return out;
    }

private:
    int _current;
    int _increment;
    int _stop;
};

class EmptyIterator : public IWIterator {
public:
    void openSource() {}
    void closeSource() {}
    bool more() const {
        return false;
    }
    Data next() {
        verify(false);
    }
};

class LimitIterator : public IWIterator {
public:
    LimitIterator(long long limit, std::unique_ptr<IWIterator> source)
        : _remaining(limit), _source(std::move(source)) {
        verify(limit > 0);
    }

    void openSource() {}
    void closeSource() {}

    bool more() const {
        return _remaining && _source->more();
    }
    Data next() {
        verify(more());
        _remaining--;
        return _source->next();
    }

private:
    long long _remaining;
    std::unique_ptr<IWIterator> _source;
};

template <typename It1, typename It2>
void _assertIteratorsEquivalent(It1 it1, It2 it2, int line) {
    int iteration;
    try {
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
    } catch (...) {
        LOGV2(22047, "Failure", "line"_attr = line, "iteration"_attr = iteration);
        throw;
    }
}
#define ASSERT_ITERATORS_EQUIVALENT(it1, it2) _assertIteratorsEquivalent(it1, it2, __LINE__)

std::vector<IWPair> makeDataForInMemIterator(const std::vector<int>& ints) {
    std::vector<IWPair> data;
    for (auto i : ints) {
        data.emplace_back(i, -i);
    }
    return data;
}

std::unique_ptr<IWIterator> makeInMemIterator(std::vector<IWPair>& data) {
    return std::make_unique<InMemIterator<IntWrapper, IntWrapper>>(data,
                                                                   IWIterator::ReturnPolicy::kMove);
}

std::unique_ptr<IWIterator> mergeIterators(
    const std::vector<std::unique_ptr<IWIterator>>& iterators,
    Direction Dir = ASC,
    const Options& opts = Options()) {
    invariant(!opts.tempDir);
    return std::make_unique<MergeIterator<IntWrapper, IntWrapper>>(
        iterators, opts.limit, IWComparator(Dir));
}

//
// Tests for Sorter framework internals
//

class SingleElemIterTests {
    void run() {
        {
            EmptyIterator empty;
            SingleElemIterator<IntWrapper, IntWrapper> singleElem{IWIterator::ReturnPolicy::kMove};
            ASSERT_ITERATORS_EQUIVALENT(&singleElem, &empty);
        }
    }
};

class InMemIterTests {
public:
    void run() {
        {
            auto data = makeDataForInMemIterator(
                {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19});
            ASSERT_ITERATORS_EQUIVALENT(makeInMemIterator(data),
                                        std::make_unique<IntIterator>(0, 20));
        }
        {
            // Make sure InMemIterator doesn't do any reordering on it's own.
            static std::vector<int> unsorted{6, 3, 7, 4, 0, 9, 5, 7, 1, 8};
            class UnsortedIter : public IWIterator {
            public:
                UnsortedIter() : _pos(0) {}
                void openSource() {}
                void closeSource() {}
                bool more() const {
                    return _pos < unsorted.size();
                }
                IWPair next() {
                    IWPair ret(unsorted[_pos], -unsorted[_pos]);
                    _pos++;
                    return ret;
                }
                size_t _pos;
            } unsortedIter;

            auto data = makeDataForInMemIterator(unsorted);
            ASSERT_ITERATORS_EQUIVALENT(makeInMemIterator(data),
                                        static_cast<IWIterator*>(&unsortedIter));
        }
    }
};

class SortedFileWriterAndFileIteratorTests {
public:
    void run() {
        unittest::TempDir tempDir("sortedFileWriterTests");
        Options opts;
        opts.tempDir = tempDir.path();

        {  // small
            auto file = std::make_unique<File>(*opts.tempDir + "/" + nextFileName("sorter-test"));
            SortedFileWriter<IntWrapper, IntWrapper> sorter(file.get());
            sorter.addAlreadySorted(0, 0);
            sorter.addAlreadySorted(1, -1);
            sorter.addAlreadySorted(2, -2);
            sorter.addAlreadySorted(3, -3);
            sorter.addAlreadySorted(4, -4);
            ASSERT_ITERATORS_EQUIVALENT(sorter.done(), std::make_unique<IntIterator>(0, 5));
        }
        {  // big
            auto file = std::make_unique<File>(*opts.tempDir + "/" + nextFileName("sorter-test"));
            SortedFileWriter<IntWrapper, IntWrapper> sorter(file.get());
            for (int i = 0; i < 10 * 1000 * 1000; i++)
                sorter.addAlreadySorted(i, -i);

            ASSERT_ITERATORS_EQUIVALENT(sorter.done(),
                                        std::make_unique<IntIterator>(0, 10 * 1000 * 1000));
        }

        ASSERT(boost::filesystem::is_empty(tempDir.path()));
    }
};


class MergeIteratorTests {
public:
    void run() {
        {  // test empty (no inputs)
            ASSERT_ITERATORS_EQUIVALENT(mergeIterators({}, ASC), std::make_unique<EmptyIterator>());
        }
        {  // test empty (only empty inputs)
            std::vector<std::unique_ptr<IWIterator>> iterators;
            iterators.push_back(std::make_unique<EmptyIterator>());
            iterators.push_back(std::make_unique<EmptyIterator>());
            iterators.push_back(std::make_unique<EmptyIterator>());

            ASSERT_ITERATORS_EQUIVALENT(mergeIterators(iterators, ASC),
                                        std::make_unique<EmptyIterator>());
        }

        {  // test ASC
            std::vector<std::unique_ptr<IWIterator>> iterators;
            iterators.push_back(std::make_unique<IntIterator>(1, 20, 2));  // 1, 3, ... 19
            iterators.push_back(std::make_unique<IntIterator>(0, 20, 2));  // 0, 2, ... 18

            ASSERT_ITERATORS_EQUIVALENT(mergeIterators(iterators, ASC),
                                        std::make_unique<IntIterator>(0, 20, 1));
        }

        {  // test DESC with an empty source
            std::vector<std::unique_ptr<IWIterator>> iterators;
            iterators.push_back(std::make_unique<IntIterator>(30, 0, -3));  // 30, 27, ... 3
            iterators.push_back(std::make_unique<IntIterator>(29, 0, -3));  // 29, 26, ... 2
            iterators.push_back(std::make_unique<IntIterator>(28, 0, -3));  // 28, 25, ... 1
            iterators.push_back(std::make_unique<EmptyIterator>());         // 28, 25, ... 1

            ASSERT_ITERATORS_EQUIVALENT(mergeIterators(iterators, DESC),
                                        std::make_unique<IntIterator>(30, 0, -1));
        }
        {  // test Limit
            std::vector<std::unique_ptr<IWIterator>> iterators;
            iterators.push_back(std::make_unique<IntIterator>(1, 20, 2));  // 1, 3, ... 19
            iterators.push_back(std::make_unique<IntIterator>(0, 20, 2));  // 0, 2, ... 18

            Options opts;
            opts.limit = 10;

            ASSERT_ITERATORS_EQUIVALENT(
                mergeIterators(iterators, ASC, opts),
                std::make_unique<LimitIterator>(10, std::make_unique<IntIterator>(0, 20, 1)));
        }
    }
};

namespace SorterTests {
class Basic {
public:
    virtual ~Basic() {}

    void run() {
        unittest::TempDir tempDir("sorterTests");
        Options opts;
        opts.tempDir = tempDir.path();

        {  // test empty (no limit)
            ASSERT_ITERATORS_EQUIVALENT(makeSorter(opts)->done(),
                                        std::make_unique<EmptyIterator>());
        }
        {  // test empty (limit 1)
            opts.limit = 1;
            ASSERT_ITERATORS_EQUIVALENT(makeSorter(opts)->done(),
                                        std::make_unique<EmptyIterator>());
        }
        {  // test empty (limit 10)
            opts.limit = 10;
            ASSERT_ITERATORS_EQUIVALENT(makeSorter(opts)->done(),
                                        std::make_unique<EmptyIterator>());
        }

        opts.limit = 0;
        const auto runTests = [this, &opts](bool assertRanges) {
            {  // test all data ASC
                std::unique_ptr<IWSorter> sorter = makeSorter(opts, IWComparator(ASC));
                addData(sorter.get());
                ASSERT_ITERATORS_EQUIVALENT(sorter->done(), correct());
                ASSERT_EQ(numAdded(), sorter->numSorted());
                if (assertRanges) {
                    assertRangeInfo(sorter, opts);
                }
            }
            {  // test all data DESC
                std::unique_ptr<IWSorter> sorter = makeSorter(opts, IWComparator(DESC));
                addData(sorter.get());
                ASSERT_ITERATORS_EQUIVALENT(sorter->done(), correctReverse());
                ASSERT_EQ(numAdded(), sorter->numSorted());
                if (assertRanges) {
                    assertRangeInfo(sorter, opts);
                }
            }

            // The debug builds are too slow to run these tests.
            // Among other things, MSVC++ makes all heap functions O(N) not O(logN).
#if !defined(MONGO_CONFIG_DEBUG_BUILD)
            {  // merge all data ASC
                std::unique_ptr<IWSorter> sorters[] = {makeSorter(opts, IWComparator(ASC)),
                                                       makeSorter(opts, IWComparator(ASC))};

                addData(sorters[0].get());
                addData(sorters[1].get());

                std::vector<std::unique_ptr<IWIterator>> iters1;
                iters1.push_back(sorters[0]->done());
                iters1.push_back(sorters[1]->done());

                std::vector<std::unique_ptr<IWIterator>> iters2;
                iters2.push_back(correct());
                iters2.push_back(correct());

                ASSERT_ITERATORS_EQUIVALENT(mergeIterators(iters1, ASC),
                                            mergeIterators(iters2, ASC));

                if (assertRanges) {
                    assertRangeInfo(sorters[0], opts);
                    assertRangeInfo(sorters[1], opts);
                }
            }
            {  // merge all data DESC and use multiple threads to insert
                std::unique_ptr<IWSorter> sorters[] = {makeSorter(opts, IWComparator(DESC)),
                                                       makeSorter(opts, IWComparator(DESC))};

                stdx::thread inBackground(&Basic::addData, this, sorters[0].get());
                addData(sorters[1].get());
                inBackground.join();

                std::vector<std::unique_ptr<IWIterator>> iters1;

                iters1.push_back(sorters[0]->done());
                iters1.push_back(sorters[1]->done());

                std::vector<std::unique_ptr<IWIterator>> iters2;
                iters2.push_back(correctReverse());
                iters2.push_back(correctReverse());

                ASSERT_ITERATORS_EQUIVALENT(mergeIterators(iters1, DESC),
                                            mergeIterators(iters2, DESC));

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
    virtual std::unique_ptr<IWIterator> correct() {
        return std::make_unique<IntIterator>(0, 5);  // 0, 1, ... 4
    }

    // like correct but with opposite sort direction
    virtual std::unique_ptr<IWIterator> correctReverse() {
        return std::make_unique<IntIterator>(4, -1, -1);  // 4, 3, ... 0
    }

    virtual size_t correctNumRanges() const {
        return 0;
    }

    // It is safe to ignore / overwrite any part of options
    virtual Options adjustSortOptions(Options opts) {
        return opts;
    }

private:
    // Make a new sorter with desired opts and comp. Opts may be ignored but not comp
    std::unique_ptr<IWSorter> makeSorter(Options opts, IWComparator comp = IWComparator(ASC)) {
        return sorter::make<IntWrapper, IntWrapper>("sorter-test", adjustSortOptions(opts), comp);
    }

    void assertRangeInfo(const std::unique_ptr<IWSorter>& sorter, const Options& opts) {
        auto numRanges = correctNumRanges();
        if (numRanges == 0)
            return;

        auto state = sorter->persistDataForShutdown();
        if (opts.tempDir) {
            ASSERT_NE(state.fileName, "");
        }
        ASSERT_EQ(state.ranges.size(), numRanges);
    }
};

class Limit : public Basic {
    Options adjustSortOptions(Options opts) override {
        opts.limit = 5;
        return opts;
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
    std::unique_ptr<IWIterator> correct() override {
        return std::make_unique<IntIterator>(-1, 4);
    }
    std::unique_ptr<IWIterator> correctReverse() override {
        return std::make_unique<IntIterator>(4, -1, -1);
    }
};

template <uint64_t Limit>
class LimitExtreme : public Basic {
    Options adjustSortOptions(Options opts) override {
        opts.limit = Limit;
        return opts;
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
    std::unique_ptr<IWIterator> correct() override {
        static auto data = makeDataForInMemIterator({-1, -1, -1, 0, 1, 1, 1, 2, 2, 3});
        return makeInMemIterator(data);
    }
    std::unique_ptr<IWIterator> correctReverse() override {
        static auto data = makeDataForInMemIterator({3, 2, 2, 1, 1, 1, 0, -1, -1, -1});
        return makeInMemIterator(data);
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

    Options adjustSortOptions(Options opts) override {
        // Make sure we use a reasonable number of files when we spill
        MONGO_STATIC_ASSERT((NUM_ITEMS * sizeof(IWPair)) / MEM_LIMIT > 50);
        MONGO_STATIC_ASSERT((NUM_ITEMS * sizeof(IWPair)) / MEM_LIMIT < 500);

        opts.maxMemoryUsageBytes = MEM_LIMIT;

        return opts;
    }

    void addData(IWSorter* sorter) override {
        for (int i = 0; i < NUM_ITEMS; i++)
            sorter->add(_array[i], -_array[i]);
    }

    size_t numAdded() const override {
        return NUM_ITEMS;
    }

    std::unique_ptr<IWIterator> correct() override {
        return std::make_unique<IntIterator>(0, NUM_ITEMS);
    }
    std::unique_ptr<IWIterator> correctReverse() override {
        return std::make_unique<IntIterator>(NUM_ITEMS - 1, -1, -1);
    }

    size_t correctNumRanges() const override {
        // We add 1 to the calculation since the call to persistDataForShutdown() spills the
        // remaining in-memory Sorter data to disk, adding one extra range.
        return NUM_ITEMS * sizeof(IWPair) / MEM_LIMIT + 1;
    }

    enum Constants {
        NUM_ITEMS = 500 * 1000,
        MEM_LIMIT = 64 * 1024,
    };
    std::unique_ptr<int[]> _array;
    PseudoRandom _random;
};


template <long long Limit, bool Random = true>
class LotsOfDataWithLimit : public LotsOfDataLittleMemory<Random> {
    typedef LotsOfDataLittleMemory<Random> Parent;
    Options adjustSortOptions(Options opts) {
        // Make sure our tests will spill or not as desired
        MONGO_STATIC_ASSERT(MEM_LIMIT / 2 > (100 * sizeof(IWPair)));
        MONGO_STATIC_ASSERT(MEM_LIMIT < (5000 * sizeof(IWPair)));
        MONGO_STATIC_ASSERT(MEM_LIMIT * 2 > (5000 * sizeof(IWPair)));

        // Make sure we use a reasonable number of files when we spill
        MONGO_STATIC_ASSERT((Parent::NUM_ITEMS * sizeof(IWPair)) / MEM_LIMIT > 100);
        MONGO_STATIC_ASSERT((Parent::NUM_ITEMS * sizeof(IWPair)) / MEM_LIMIT < 500);

        opts.maxMemoryUsageBytes = MEM_LIMIT;
        opts.limit = Limit;

        return opts;
    }
    std::unique_ptr<IWIterator> correct() override {
        return std::make_unique<LimitIterator>(Limit, Parent::correct());
    }
    std::unique_ptr<IWIterator> correctReverse() override {
        return std::make_unique<LimitIterator>(Limit, Parent::correctReverse());
    }
    size_t correctNumRanges() const override {
        // For the TopKSorter, the number of ranges depends on the specific composition of the data
        // being sorted.
        return 0;
    }
    enum { MEM_LIMIT = 32 * 1024 };
};
}  // namespace SorterTests

class SorterSuite : public mongo::unittest::OldStyleSuiteSpecification {
public:
    SorterSuite() : mongo::unittest::OldStyleSuiteSpecification("sorter") {}

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
    }
};

mongo::unittest::OldStyleSuiteInitializer<SorterSuite> extSortTests;

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
    Options opts;
    opts.limit = 1;
    opts.tempDir = "unused_temp_dir";
    sorter::makeFromExistingRanges<IntWrapper, IntWrapper>("", {}, opts, IWComparator(ASC));
}

DEATH_TEST_F(SorterMakeFromExistingRangesTest, ExtSortNotAllowed, "options.tempDir") {
    Options opts;
    ASSERT_FALSE(opts.tempDir);
    sorter::makeFromExistingRanges<IntWrapper, IntWrapper>("", {}, opts, IWComparator(ASC));
}

DEATH_TEST_F(SorterMakeFromExistingRangesTest, EmptyFileName, "!fileName.empty()") {
    std::string fileName;
    Options opts;
    opts.tempDir = "unused_temp_dir";
    sorter::makeFromExistingRanges<IntWrapper, IntWrapper>(fileName, {}, opts, IWComparator(ASC));
}

TEST_F(SorterMakeFromExistingRangesTest, SkipFileCheckingOnEmptyRanges) {
    auto fileName = "unused_sorter_file";
    Options opts;
    opts.tempDir = "unused_temp_dir";
    auto sorter = sorter::makeFromExistingRanges<IntWrapper, IntWrapper>(
        fileName, {}, opts, IWComparator(ASC));

    ASSERT_EQ(0, sorter->numSpills());

    auto iter = std::unique_ptr<IWIterator>(sorter->done());
    ASSERT_EQ(0, sorter->numSorted());

    ASSERT_FALSE(iter->more());
}

TEST_F(SorterMakeFromExistingRangesTest, MissingFile) {
    auto fileName = "unused_sorter_file";
    auto tempDir = "unused_temp_dir";
    Options opts;
    opts.tempDir = tempDir;
    auto makeSorter = [&] {
        sorter::makeFromExistingRanges<IntWrapper, IntWrapper>(
            fileName, makeSampleRanges(), opts, IWComparator(ASC));
    };
    ASSERT_THROWS_WITH_CHECK(makeSorter(), std::exception, [&](const auto& ex) {
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
    Options opts;
    opts.tempDir = tempDir.path();
    auto makeSorter = [&] {
        sorter::makeFromExistingRanges<IntWrapper, IntWrapper>(
            fileName, makeSampleRanges(), opts, IWComparator(ASC));
    };
    // 16815 - unexpected empty file.
    ASSERT_THROWS_CODE(makeSorter(), DBException, 16815);
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
    Options opts;
    opts.tempDir = tempDir.path();
    auto sorter = sorter::makeFromExistingRanges<IntWrapper, IntWrapper>(
        fileName, makeSampleRanges(), opts, IWComparator(ASC));

    // The number of spills is set when NoLimitSorter is constructed from existing ranges.
    ASSERT_EQ(makeSampleRanges().size(), sorter->numSpills());
    ASSERT_EQ(0, sorter->numSorted());

    // 16817 - error reading file.
    ASSERT_THROWS_CODE(sorter->done(), DBException, 16817);
}

TEST_F(SorterMakeFromExistingRangesTest, RoundTrip) {
    unittest::TempDir tempDir(_agent.getSuiteName() + "_" + _agent.getTestName());

    Options opts;
    opts.tempDir = tempDir.path();
    opts.maxMemoryUsageBytes = sizeof(IWSorter::Data);

    IWPair pairInsertedBeforeShutdown(1, 100);

    // This test uses two sorters. The first sorter is used to persist data to disk in a shutdown
    // scenario. On startup, we will restore the original state of the sorter using the persisted
    // data.
    IWSorter::PersistedState state;
    {
        auto sorterBeforeShutdown =
            sorter::make<IntWrapper, IntWrapper>("sorter-test", opts, IWComparator(ASC));
        sorterBeforeShutdown->add(pairInsertedBeforeShutdown.first,
                                  pairInsertedBeforeShutdown.second);
        state = sorterBeforeShutdown->persistDataForShutdown();
        ASSERT_FALSE(state.fileName.empty());
        ASSERT_EQUALS(1U, state.ranges.size()) << state.ranges.size();
        ASSERT_EQ(1, sorterBeforeShutdown->numSorted());
    }

    // On restart, reconstruct sorter from persisted state.
    auto sorter = sorter::makeFromExistingRanges<IntWrapper, IntWrapper>(
        state.fileName, state.ranges, opts, IWComparator(ASC));

    // The number of spills is set when NoLimitSorter is constructed from existing ranges.
    ASSERT_EQ(state.ranges.size(), sorter->numSpills());

    // Ensure that the restored sorter can accept additional data.
    IWPair pairInsertedAfterStartup(2, 200);
    sorter->add(pairInsertedAfterStartup.first, pairInsertedAfterStartup.second);

    // Only the pair added after reconstructing the Sorter is counted.
    ASSERT_EQ(1, sorter->numSorted());

    // Read data from sorter.
    {
        auto iter = std::unique_ptr<IWIterator>(sorter->done());

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
    }
}

}  // namespace
}  // namespace mongo::sorter
