/**
 *    Copyright (C) 2013 10gen Inc.
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
 *    must comply with the GNU Affero General Public License in all respects
 *    for all of the code used other than as permitted herein. If you modify
 *    file(s) with this exception, you may extend this exception to your
 *    version of the file(s), but you are not obligated to do so. If you do not
 *    wish to do so, delete this exception statement from your version. If you
 *    delete this exception statement from all source files in the program,
 *    then also delete it in the license file.
 */

#include "mongo/platform/basic.h"

#include "mongo/db/sorter/sorter.h"

#include <boost/filesystem.hpp>

#include "mongo/base/data_type_endian.h"
#include "mongo/base/init.h"
#include "mongo/config.h"
#include "mongo/db/service_context.h"
#include "mongo/db/service_context_noop.h"
#include "mongo/stdx/memory.h"
#include "mongo/stdx/thread.h"
#include "mongo/unittest/temp_dir.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/mongoutils/str.h"

// Need access to internal classes
#include "mongo/db/sorter/sorter.cpp"

namespace mongo {
using namespace mongo::sorter;
using std::make_shared;
using std::pair;

// Stub to avoid including the server_options library
// TODO: This should go away once we can do these checks at compile time
bool isMongos() {
    return false;
}

// Stub to avoid including the server environment library.
MONGO_INITIALIZER(SetGlobalEnvironment)(InitializerContext* context) {
    setGlobalServiceContext(stdx::make_unique<ServiceContextNoop>());
    return Status::OK();
}

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

private:
    int _i;
};

typedef pair<IntWrapper, IntWrapper> IWPair;
typedef SortIteratorInterface<IntWrapper, IntWrapper> IWIterator;
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
    bool more() {
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
    bool more() {
        return false;
    }
    Data next() {
        verify(false);
    }
};

class LimitIterator : public IWIterator {
public:
    LimitIterator(long long limit, std::shared_ptr<IWIterator> source)
        : _remaining(limit), _source(source) {
        verify(limit > 0);
    }

    bool more() {
        return _remaining && _source->more();
    }
    Data next() {
        verify(more());
        _remaining--;
        return _source->next();
    }

private:
    long long _remaining;
    std::shared_ptr<IWIterator> _source;
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
        mongo::unittest::log() << "Failure from line " << line << " on iteration " << iteration
                               << std::endl;
        throw;
    }
}
#define ASSERT_ITERATORS_EQUIVALENT(it1, it2) _assertIteratorsEquivalent(it1, it2, __LINE__)

template <int N>
std::shared_ptr<IWIterator> makeInMemIterator(const int (&array)[N]) {
    std::vector<IWPair> vec;
    for (int i = 0; i < N; i++)
        vec.push_back(IWPair(array[i], -array[i]));
    return std::make_shared<sorter::InMemIterator<IntWrapper, IntWrapper>>(vec);
}

template <typename IteratorPtr, int N>
std::shared_ptr<IWIterator> mergeIterators(IteratorPtr (&array)[N],
                                           Direction Dir = ASC,
                                           const SortOptions& opts = SortOptions()) {
    std::vector<std::shared_ptr<IWIterator>> vec;
    for (int i = 0; i < N; i++)
        vec.push_back(std::shared_ptr<IWIterator>(array[i]));
    return std::shared_ptr<IWIterator>(IWIterator::merge(vec, opts, IWComparator(Dir)));
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
                                        make_shared<IntIterator>(0, 20));
        }
        {
            // make sure InMemIterator doesn't do any reordering on it's own
            static const int unsorted[] = {6, 3, 7, 4, 0, 9, 5, 7, 1, 8};
            class UnsortedIter : public IWIterator {
            public:
                UnsortedIter() : _pos(0) {}
                bool more() {
                    return _pos < sizeof(unsorted) / sizeof(unsorted[0]);
                }
                IWPair next() {
                    IWPair ret(unsorted[_pos], -unsorted[_pos]);
                    _pos++;
                    return ret;
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
        const SortOptions opts = SortOptions().TempDir(tempDir.path());
        {  // small
            SortedFileWriter<IntWrapper, IntWrapper> sorter(opts);
            sorter.addAlreadySorted(0, 0);
            sorter.addAlreadySorted(1, -1);
            sorter.addAlreadySorted(2, -2);
            sorter.addAlreadySorted(3, -3);
            sorter.addAlreadySorted(4, -4);
            ASSERT_ITERATORS_EQUIVALENT(std::shared_ptr<IWIterator>(sorter.done()),
                                        make_shared<IntIterator>(0, 5));
        }
        {  // big
            SortedFileWriter<IntWrapper, IntWrapper> sorter(opts);
            for (int i = 0; i < 10 * 1000 * 1000; i++)
                sorter.addAlreadySorted(i, -i);

            ASSERT_ITERATORS_EQUIVALENT(std::shared_ptr<IWIterator>(sorter.done()),
                                        make_shared<IntIterator>(0, 10 * 1000 * 1000));
        }

        ASSERT(boost::filesystem::is_empty(tempDir.path()));
    }
};


class MergeIteratorTests {
public:
    void run() {
        {  // test empty (no inputs)
            std::vector<std::shared_ptr<IWIterator>> vec;
            std::shared_ptr<IWIterator> mergeIter(
                IWIterator::merge(vec, SortOptions(), IWComparator()));
            ASSERT_ITERATORS_EQUIVALENT(mergeIter, make_shared<EmptyIterator>());
        }
        {  // test empty (only empty inputs)
            std::shared_ptr<IWIterator> iterators[] = {make_shared<EmptyIterator>(),
                                                       make_shared<EmptyIterator>(),
                                                       make_shared<EmptyIterator>()};

            ASSERT_ITERATORS_EQUIVALENT(mergeIterators(iterators, ASC),
                                        make_shared<EmptyIterator>());
        }

        {  // test ASC
            std::shared_ptr<IWIterator> iterators[] = {
                make_shared<IntIterator>(1, 20, 2)  // 1, 3, ... 19
                ,
                make_shared<IntIterator>(0, 20, 2)  // 0, 2, ... 18
            };

            ASSERT_ITERATORS_EQUIVALENT(mergeIterators(iterators, ASC),
                                        make_shared<IntIterator>(0, 20, 1));
        }

        {  // test DESC with an empty source
            std::shared_ptr<IWIterator> iterators[] = {
                make_shared<IntIterator>(30, 0, -3)  // 30, 27, ... 3
                ,
                make_shared<IntIterator>(29, 0, -3)  // 29, 26, ... 2
                ,
                make_shared<IntIterator>(28, 0, -3)  // 28, 25, ... 1
                ,
                make_shared<EmptyIterator>()};

            ASSERT_ITERATORS_EQUIVALENT(mergeIterators(iterators, DESC),
                                        make_shared<IntIterator>(30, 0, -1));
        }
        {  // test Limit
            std::shared_ptr<IWIterator> iterators[] = {
                make_shared<IntIterator>(1, 20, 2)  // 1, 3, ... 19
                ,
                make_shared<IntIterator>(0, 20, 2)  // 0, 2, ... 18
            };

            ASSERT_ITERATORS_EQUIVALENT(
                mergeIterators(iterators, ASC, SortOptions().Limit(10)),
                make_shared<LimitIterator>(10, make_shared<IntIterator>(0, 20, 1)));
        }
    }
};

namespace SorterTests {
class Basic {
public:
    virtual ~Basic() {}

    void run() {
        unittest::TempDir tempDir("sorterTests");
        const SortOptions opts = SortOptions().TempDir(tempDir.path());

        {  // test empty (no limit)
            ASSERT_ITERATORS_EQUIVALENT(done(makeSorter(opts)), make_shared<EmptyIterator>());
        }
        {  // test empty (limit 1)
            ASSERT_ITERATORS_EQUIVALENT(done(makeSorter(SortOptions(opts).Limit(1))),
                                        make_shared<EmptyIterator>());
        }
        {  // test empty (limit 10)
            ASSERT_ITERATORS_EQUIVALENT(done(makeSorter(SortOptions(opts).Limit(10))),
                                        make_shared<EmptyIterator>());
        }

        {  // test all data ASC
            std::shared_ptr<IWSorter> sorter = makeSorter(opts, IWComparator(ASC));
            addData(sorter);
            ASSERT_ITERATORS_EQUIVALENT(done(sorter), correct());
        }
        {  // test all data DESC
            std::shared_ptr<IWSorter> sorter = makeSorter(opts, IWComparator(DESC));
            addData(sorter);
            ASSERT_ITERATORS_EQUIVALENT(done(sorter), correctReverse());
        }

// The debug builds are too slow to run these tests.
// Among other things, MSVC++ makes all heap functions O(N) not O(logN).
#if !defined(MONGO_CONFIG_DEBUG_BUILD)
        {  // merge all data ASC
            std::shared_ptr<IWSorter> sorters[] = {makeSorter(opts, IWComparator(ASC)),
                                                   makeSorter(opts, IWComparator(ASC))};

            addData(sorters[0]);
            addData(sorters[1]);

            std::shared_ptr<IWIterator> iters1[] = {done(sorters[0]), done(sorters[1])};
            std::shared_ptr<IWIterator> iters2[] = {correct(), correct()};
            ASSERT_ITERATORS_EQUIVALENT(mergeIterators(iters1, ASC), mergeIterators(iters2, ASC));
        }
        {  // merge all data DESC and use multiple threads to insert
            std::shared_ptr<IWSorter> sorters[] = {makeSorter(opts, IWComparator(DESC)),
                                                   makeSorter(opts, IWComparator(DESC))};

            stdx::thread inBackground(&Basic::addData, this, sorters[0]);
            addData(sorters[1]);
            inBackground.join();

            std::shared_ptr<IWIterator> iters1[] = {done(sorters[0]), done(sorters[1])};
            std::shared_ptr<IWIterator> iters2[] = {correctReverse(), correctReverse()};
            ASSERT_ITERATORS_EQUIVALENT(mergeIterators(iters1, DESC), mergeIterators(iters2, DESC));
        }
#endif
        ASSERT(boost::filesystem::is_empty(tempDir.path()));
    }

    // add data to the sorter
    virtual void addData(unowned_ptr<IWSorter> sorter) {
        sorter->add(2, -2);
        sorter->add(1, -1);
        sorter->add(0, 0);
        sorter->add(4, -4);
        sorter->add(3, -3);
    }

    // returns an iterator with the correct results
    virtual std::shared_ptr<IWIterator> correct() {
        return make_shared<IntIterator>(0, 5);  // 0, 1, ... 4
    }

    // like correct but with opposite sort direction
    virtual std::shared_ptr<IWIterator> correctReverse() {
        return make_shared<IntIterator>(4, -1, -1);  // 4, 3, ... 0
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

    std::shared_ptr<IWIterator> done(unowned_ptr<IWSorter> sorter) {
        return std::shared_ptr<IWIterator>(sorter->done());
    }
};

class Limit : public Basic {
    virtual SortOptions adjustSortOptions(SortOptions opts) {
        return opts.Limit(5);
    }
    void addData(unowned_ptr<IWSorter> sorter) {
        sorter->add(0, 0);
        sorter->add(3, -3);
        sorter->add(4, -4);
        sorter->add(2, -2);
        sorter->add(1, -1);
        sorter->add(-1, 1);
    }
    virtual std::shared_ptr<IWIterator> correct() {
        return make_shared<IntIterator>(-1, 4);
    }
    virtual std::shared_ptr<IWIterator> correctReverse() {
        return make_shared<IntIterator>(4, -1, -1);
    }
};

class Dupes : public Basic {
    void addData(unowned_ptr<IWSorter> sorter) {
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
    virtual std::shared_ptr<IWIterator> correct() {
        const int array[] = {-1, -1, -1, 0, 1, 1, 1, 2, 2, 3};
        return makeInMemIterator(array);
    }
    virtual std::shared_ptr<IWIterator> correctReverse() {
        const int array[] = {3, 2, 2, 1, 1, 1, 0, -1, -1, -1};
        return makeInMemIterator(array);
    }
};

template <bool Random = true>
class LotsOfDataLittleMemory : public Basic {
public:
    LotsOfDataLittleMemory() : _array(new int[NUM_ITEMS]) {
        for (int i = 0; i < NUM_ITEMS; i++)
            _array[i] = i;

        if (Random)
            std::random_shuffle(_array.get(), _array.get() + NUM_ITEMS);
    }

    SortOptions adjustSortOptions(SortOptions opts) {
        // Make sure we use a reasonable number of files when we spill
        static_assert((NUM_ITEMS * sizeof(IWPair)) / MEM_LIMIT > 50,
                      "(NUM_ITEMS * sizeof(IWPair)) / MEM_LIMIT > 50");
        static_assert((NUM_ITEMS * sizeof(IWPair)) / MEM_LIMIT < 500,
                      "(NUM_ITEMS * sizeof(IWPair)) / MEM_LIMIT < 500");

        return opts.MaxMemoryUsageBytes(MEM_LIMIT).ExtSortAllowed();
    }

    void addData(unowned_ptr<IWSorter> sorter) {
        for (int i = 0; i < NUM_ITEMS; i++)
            sorter->add(_array[i], -_array[i]);

        if (typeid(*this) == typeid(LotsOfDataLittleMemory)) {
            // don't do this check in subclasses since they may set a limit
            ASSERT_GREATER_THAN_OR_EQUALS(static_cast<size_t>(sorter->numFiles()),
                                          (NUM_ITEMS * sizeof(IWPair)) / MEM_LIMIT);
        }
    }

    virtual std::shared_ptr<IWIterator> correct() {
        return make_shared<IntIterator>(0, NUM_ITEMS);
    }
    virtual std::shared_ptr<IWIterator> correctReverse() {
        return make_shared<IntIterator>(NUM_ITEMS - 1, -1, -1);
    }

    enum Constants {
        NUM_ITEMS = 500 * 1000,
        MEM_LIMIT = 64 * 1024,
    };
    std::unique_ptr<int[]> _array;
};


template <long long Limit, bool Random = true>
class LotsOfDataWithLimit : public LotsOfDataLittleMemory<Random> {
    typedef LotsOfDataLittleMemory<Random> Parent;
    SortOptions adjustSortOptions(SortOptions opts) {
        // Make sure our tests will spill or not as desired
        static_assert(MEM_LIMIT / 2 > (100 * sizeof(IWPair)),
                      "MEM_LIMIT / 2 > (100 * sizeof(IWPair))");
        static_assert(MEM_LIMIT < (5000 * sizeof(IWPair)), "MEM_LIMIT < (5000 * sizeof(IWPair))");
        static_assert(MEM_LIMIT * 2 > (5000 * sizeof(IWPair)),
                      "MEM_LIMIT * 2 > (5000 * sizeof(IWPair))");

        // Make sure we use a reasonable number of files when we spill
        static_assert((Parent::NUM_ITEMS * sizeof(IWPair)) / MEM_LIMIT > 100,
                      "(Parent::NUM_ITEMS * sizeof(IWPair)) / MEM_LIMIT > 100");
        static_assert((Parent::NUM_ITEMS * sizeof(IWPair)) / MEM_LIMIT < 500,
                      "(Parent::NUM_ITEMS * sizeof(IWPair)) / MEM_LIMIT < 500");

        return opts.MaxMemoryUsageBytes(MEM_LIMIT).ExtSortAllowed().Limit(Limit);
    }
    virtual std::shared_ptr<IWIterator> correct() {
        return make_shared<LimitIterator>(Limit, Parent::correct());
    }
    virtual std::shared_ptr<IWIterator> correctReverse() {
        return make_shared<LimitIterator>(Limit, Parent::correctReverse());
    }
    enum { MEM_LIMIT = 32 * 1024 };
};
}

class SorterSuite : public mongo::unittest::Suite {
public:
    SorterSuite() : Suite("sorter") {}

    void setupTests() {
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
    }
};

mongo::unittest::SuiteInstance<SorterSuite> extSortTests;
}
