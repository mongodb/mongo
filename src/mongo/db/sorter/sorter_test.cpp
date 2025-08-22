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
#include "mongo/base/static_assert.h"
#include "mongo/config.h"  // IWYU pragma: keep
#include "mongo/db/sorter/sorter_template_defs.h"
#include "mongo/db/sorter/sorter_test_utils.h"
#include "mongo/stdx/thread.h"  // IWYU pragma: keep
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"

#include <ctime>
#include <fstream>  // IWYU pragma: keep
#include <memory>
#include <span>

#include <boost/filesystem/operations.hpp>
#include <boost/filesystem/path.hpp>
#include <boost/iterator/iterator_facade.hpp>
#include <fmt/format.h>


namespace mongo {
namespace sorter {
namespace {

//
// Tests for Sorter framework internals
//

unittest::TempDir makeTempDir() {
    return unittest::TempDir{
        fmt::format("{}_{}", unittest::getSuiteName(), unittest::getTestName())};
}

using InMemIterTest = unittest::Test;

TEST_F(InMemIterTest, Empty) {
    EmptyIterator empty;
    sorter::InMemIterator<IntWrapper, IntWrapper> inMem;
    ASSERT_ITERATORS_EQUIVALENT(&inMem, &empty);
}

TEST_F(InMemIterTest, Sorted) {
    static const int zeroUpTo20[] = {0,  1,  2,  3,  4,  5,  6,  7,  8,  9,
                                     10, 11, 12, 13, 14, 15, 16, 17, 18, 19};
    ASSERT_ITERATORS_EQUIVALENT(makeInMemIterator(zeroUpTo20),
                                std::make_shared<IntIterator>(0, 20));
}

TEST_F(InMemIterTest, DoesNoReorderGivenInput) {
    static const int unsorted[] = {6, 3, 7, 4, 0, 9, 5, 7, 1, 8};
    class UnsortedIter : public IWIterator {
    public:
        UnsortedIter() : _pos(0) {}
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

TEST_F(InMemIterTest, SpillDoesNotChangeResultAndUpdateStatistics) {
    static const int data[] = {6, 3, 7, 4, 0, 9, 5, 7, 1, 8};

    auto expectedIterator = makeInMemIterator(data);
    auto iteratorToSpill = makeInMemIterator(data);
    ASSERT_ITERATORS_EQUIVALENT_FOR_N_STEPS(expectedIterator, iteratorToSpill, 3);

    unittest::TempDir tempDir("InMemIterTests");
    SorterTracker sorterTracker;
    SorterFileStats sorterFileStats(&sorterTracker);
    const SortOptions opts =
        SortOptions().TempDir(tempDir.path()).FileStats(&sorterFileStats).Tracker(&sorterTracker);

    ASSERT_TRUE(iteratorToSpill->spillable());
    auto spilledIterator = iteratorToSpill->spill(opts, IWSorter::Settings{});
    ASSERT_FALSE(spilledIterator->spillable());
    ASSERT_ITERATORS_EQUIVALENT(expectedIterator, spilledIterator);

    ASSERT_EQ(sorterTracker.spilledRanges.loadRelaxed(), 1);
    ASSERT_EQ(sorterTracker.spilledKeyValuePairs.loadRelaxed(), 7);
    ASSERT_EQ(sorterTracker.bytesSpilledUncompressed.loadRelaxed(), 56);
    ASSERT_LT(sorterTracker.bytesSpilled.loadRelaxed(), 100);
    ASSERT_GT(sorterTracker.bytesSpilled.loadRelaxed(), 0);

    ASSERT_EQ(sorterFileStats.bytesSpilledUncompressed(), 56);
    ASSERT_LT(sorterFileStats.bytesSpilled(), 100);
    ASSERT_GT(sorterFileStats.bytesSpilled(), 0);
}

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

DEATH_TEST_F(SorterMakeFromExistingRangesTest, EmptyFileName, "!fileName.empty()") {
    std::string fileName;
    auto opts = SortOptions().TempDir("unused_temp_dir");
    IWSorter::makeFromExistingRanges(fileName, {}, opts, IWComparator(ASC));
}

TEST_F(SorterMakeFromExistingRangesTest, SkipFileCheckingOnEmptyRanges) {
    auto fileName = "unused_sorter_file";
    SorterTracker sorterTracker;
    auto opts = SortOptions().TempDir("unused_temp_dir").Tracker(&sorterTracker);
    auto sorter = IWSorter::makeFromExistingRanges(fileName, {}, opts, IWComparator(ASC));

    ASSERT_EQ(0, sorter->stats().spilledRanges());

    auto iter = sorter->done();
    ASSERT_EQ(0, sorter->stats().numSorted());

    ASSERT_FALSE(iter->more());
}

TEST_F(SorterMakeFromExistingRangesTest, MissingFile) {
    auto fileName = "unused_sorter_file";
    auto tempDir = "unused_temp_dir";
    auto opts = SortOptions().TempDir(tempDir);
    ASSERT_THROWS_WITH_CHECK(
        IWSorter::makeFromExistingRanges(fileName, makeSampleRanges(), opts, IWComparator(ASC)),
        std::exception,
        [&](const auto& ex) {
            ASSERT_STRING_CONTAINS(ex.what(), tempDir);
            ASSERT_STRING_CONTAINS(ex.what(), fileName);
        });
}

TEST_F(SorterMakeFromExistingRangesTest, EmptyFile) {
    unittest::TempDir tempDir = makeTempDir();
    auto tempFilePath = boost::filesystem::path(tempDir.path()) / "empty_sorter_file";
    ASSERT(std::ofstream(tempFilePath.string()))
        << "failed to create empty temporary file: " << tempFilePath.string();
    auto fileName = tempFilePath.filename().string();
    auto opts = SortOptions().TempDir(tempDir.path());
    // 16815 - unexpected empty file.
    ASSERT_THROWS_CODE(
        IWSorter::makeFromExistingRanges(fileName, makeSampleRanges(), opts, IWComparator(ASC)),
        DBException,
        16815);
}

TEST_F(SorterMakeFromExistingRangesTest, CorruptedFile) {
    unittest::TempDir tempDir = makeTempDir();
    auto tempFilePath = boost::filesystem::path(tempDir.path()) / "corrupted_sorter_file";
    {
        std::ofstream ofs(tempFilePath.string());
        ASSERT(ofs) << "failed to create temporary file: " << tempFilePath.string();
        ofs << "invalid sorter data";
    }
    auto fileName = tempFilePath.filename().string();
    SorterTracker sorterTracker;
    auto opts = SortOptions().TempDir(tempDir.path()).Tracker(&sorterTracker);
    auto sorter =
        IWSorter::makeFromExistingRanges(fileName, makeSampleRanges(), opts, IWComparator(ASC));

    // The number of spills is set when NoLimitSorter is constructed from existing ranges.
    ASSERT_EQ(makeSampleRanges().size(), sorter->stats().spilledRanges());
    ASSERT_EQ(0, sorter->stats().numSorted());

    // 16817 - error reading file.
    ASSERT_THROWS_CODE(sorter->done(), DBException, 16817);
}

TEST_F(SorterMakeFromExistingRangesTest, RoundTrip) {
    unittest::TempDir tempDir = makeTempDir();
    SorterTracker sorterTracker;

    auto opts = SortOptions()
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

TEST_F(SorterMakeFromExistingRangesTest, NextWithDeferredValues) {
    unittest::TempDir tempDir = makeTempDir();
    auto opts = SortOptions().TempDir(tempDir.path());

    IWPair pair1(1, 100);
    IWPair pair2(2, 200);
    auto spillFile = std::make_shared<Sorter<IntWrapper, IntWrapper>::File>(
        sorter::nextFileName(*(opts.tempDir)), opts.sorterFileStats);
    SortedFileWriter<IntWrapper, IntWrapper> writer(opts, std::move(spillFile));
    writer.addAlreadySorted(pair1.first, pair1.second);
    writer.addAlreadySorted(pair2.first, pair2.second);
    auto iter = writer.done();

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
}

TEST_F(SorterMakeFromExistingRangesTest, ChecksumVersion) {
    unittest::TempDir tempDir = makeTempDir();
    auto opts = SortOptions().TempDir(tempDir.path());

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
    ret.opts = SortOptions().TempDir(tempDir.path());

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
    unittest::TempDir tempDir = makeTempDir();
    auto state = makeSpillFile(tempDir);
    auto it = IWSorter::makeFromExistingRanges(state.fileName, state.ranges, state.opts, state.comp)
                  ->done();
    ASSERT_ITERATORS_EQUIVALENT(it, std::make_unique<IntIterator>(0, 10));
}

TEST_F(SorterMakeFromExistingRangesTest, IncompleteReadDoesNotReportChecksumError) {
    unittest::TempDir tempDir = makeTempDir();
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
    unittest::TempDir tempDir = makeTempDir();
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
    unittest::TempDir tempDir = makeTempDir();
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
    struct NoBoundAsc {
        Key operator()(Key k, const Doc&) const {
            return -1'000'000'000;
        }
        Document serialize(const SerializationOptions& opts = {}) const {
            MONGO_UNREACHABLE;
        }
    };

    using S = BoundedSorterInterface<Key, Doc>;
    using SAsc = BoundedSorter<Key, Doc, ComparatorAsc, BoundMakerAsc>;
    using SAscNoBound = BoundedSorter<Key, Doc, ComparatorAsc, NoBoundAsc>;
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
    std::unique_ptr<S> makeAscNoBound(SortOptions options, bool checkInput = true) {
        return std::make_unique<SAscNoBound>(options, ComparatorAsc{}, NoBoundAsc{}, checkInput);
    }
    std::unique_ptr<S> makeDesc(SortOptions options, bool checkInput = true) {
        return std::make_unique<SDesc>(options, ComparatorDesc{}, BoundMakerDesc{}, checkInput);
    }

    SorterTracker sorterTracker;
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
    unittest::TempDir tempDir = makeTempDir();
    auto options =
        SortOptions().TempDir(tempDir.path()).MaxMemoryUsageBytes(16).Tracker(&sorterTracker);
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
    unittest::TempDir tempDir = makeTempDir();
    auto options = SortOptions().TempDir(tempDir.path()).MaxMemoryUsageBytes(16);
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
    unittest::TempDir tempDir = makeTempDir();
    auto options =
        SortOptions().TempDir(tempDir.path()).MaxMemoryUsageBytes(16).Tracker(&sorterTracker);
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
    unittest::TempDir tempDir = makeTempDir();
    auto options = SortOptions().TempDir(tempDir.path()).MaxMemoryUsageBytes(16);

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
    unittest::TempDir tempDir = makeTempDir();
    auto options = SortOptions()
                       .TempDir(tempDir.path())
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
    unittest::TempDir tempDir = makeTempDir();
    auto options = SortOptions()
                       .TempDir(tempDir.path())
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

TEST_F(BoundedSorterTest, ForceSpill) {
    SorterFileStats fileStats(&sorterTracker);
    unittest::TempDir tempDir = makeTempDir();
    auto options = SortOptions()
                       .TempDir(tempDir.path())
                       .MaxMemoryUsageBytes(100 * 1024 * 1024)
                       .Tracker(&sorterTracker)
                       .FileStats(&fileStats);

    sorter = makeAsc(options);
    // Sorter stores pointers to sorterTracker and fileStats, it has to be destroyed before them.
    ScopeGuard sorterReset{[&]() {
        sorter.reset();
    }};

    std::vector<Doc> input = {
        {7},
        {6},
        {-2},
        {-3},
        {9},  // will return -1
        {7},
        {6},
        {-1},
        {0},
        {11},  // will return 0
        {5},
        {4},
        {1},
        {12},  // will return 1
        {3},
        {2},
    };

    std::vector<Doc> output;
    for (size_t i = 0; i < input.size(); ++i) {
        sorter->add(input[i].time, std::move(input[i]));
        while (sorter->getState() == S::State::kReady) {
            output.push_back(sorter->next().second);
        }
        if (i % 3 == 2) {
            sorter->forceSpill();
        }
    }
    sorter->done();

    while (sorter->getState() == S::State::kReady) {
        output.push_back(sorter->next().second);
    }
    ASSERT(sorter->getState() == S::State::kDone);

    ASSERT_EQ(output.size(), input.size());
    assertSorted(output);

    ASSERT_EQ(sorter->stats().spilledRanges(), 5);
    ASSERT_EQ(sorter->stats().spilledKeyValuePairs(), 13);
    ASSERT_EQ(fileStats.bytesSpilledUncompressed(), 104);
    ASSERT_GT(fileStats.bytesSpilled(), 0);
    ASSERT_LT(fileStats.bytesSpilled(), 1000);
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
    unittest::TempDir tempDir = makeTempDir();
    auto options =
        SortOptions().TempDir(tempDir.path()).Tracker(&sorterTracker).MaxMemoryUsageBytes(40);
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

TEST_F(BoundedSorterTest, LargeSpill) {
    static const Key kKey = 1;
    static constexpr uint64_t kMemoryLimit = 1024 * 1024;
    static constexpr size_t kDocCount = kMemoryLimit;

    unittest::TempDir tempDir = makeTempDir();
    auto options = SortOptions().TempDir(tempDir.path()).MaxMemoryUsageBytes(kMemoryLimit);
    sorter = makeAscNoBound(options);

    std::vector<Doc> input;
    input.reserve(kDocCount);
    for (size_t i = 0; i < kDocCount; ++i) {
        input.emplace_back(Doc{kKey});
    }

    assertSorted(sort(input));
    ASSERT_GTE(sorter->stats().spilledRanges(), 1);
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
