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

#include "mongo/base/error_codes.h"
#include "mongo/config.h"  // IWYU pragma: keep
#include "mongo/db/service_context_d_test_fixture.h"
#include "mongo/db/sorter/sorter_template_defs.h"
#include "mongo/db/sorter/sorter_test_utils.h"
#include "mongo/db/sorter/typed_sorter_test_utils.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/fail_point.h"

#include <algorithm>
#include <ctime>
#include <fstream>  // IWYU pragma: keep
#include <memory>
#include <string>
#include <thread>  // IWYU pragma: keep
#include <type_traits>
#include <utility>
#include <vector>

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

unittest::TempDir makeSpillDir() {
    auto name = fmt::format("{}_{}", unittest::getSuiteName(), unittest::getTestName());
    std::replace(name.begin(), name.end(), '/', '_');
    std::replace(name.begin(), name.end(), '\\', '_');
    return unittest::TempDir{name};
}

using InMemIterTest = unittest::Test;
using test::ContainerTraits;
using test::FileTraits;
using test::makeFileSpiller;

TEST_F(InMemIterTest, Empty) {
    EmptyIterator empty;
    sorter::InMemIterator<IntWrapper, IntWrapper, IWComparator> inMem;
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
        const IntWrapper& peek() override {
            MONGO_UNREACHABLE;
        }
        SorterRange getRange() const override {
            MONGO_UNREACHABLE;
        }
        bool spillable() const override {
            return false;
        }
        [[nodiscard]] std::unique_ptr<Iterator<IntWrapper, IntWrapper>> spill(
            const SortOptions& opts,
            const typename Sorter<IntWrapper, IntWrapper>::Settings& settings) override {
            MONGO_UNREACHABLE;
        }
        size_t _pos;
    } unsortedIter;

    ASSERT_ITERATORS_EQUIVALENT(makeInMemIterator(unsorted),
                                static_cast<IWIterator*>(&unsortedIter));
}

TEST_F(InMemIterTest, SpillDoesNotChangeResultAndUpdateStatistics) {
    static const int data[] = {6, 3, 7, 4, 0, 9, 5, 7, 1, 8};

    unittest::TempDir spillDir("InMemIterTests");
    SorterTracker sorterTracker;
    SorterFileStats sorterFileStats(&sorterTracker);
    const SortOptions opts = SortOptions().Tracker(&sorterTracker);
    std::shared_ptr<FileBasedSpiller<IntWrapper, IntWrapper, IWComparator>> spiller =
        std::make_shared<FileBasedSpiller<IntWrapper, IntWrapper, IWComparator>>(
            spillDir.path(),
            &sorterFileStats,
            /*dbName=*/boost::none,
            sorter::kLatestChecksumVersion,
            testSpillingMinAvailableDiskSpaceBytes);

    auto expectedIterator = makeInMemIterator(data, spiller);
    auto iteratorToSpill = makeInMemIterator(data, spiller);
    ASSERT_ITERATORS_EQUIVALENT_FOR_N_STEPS(expectedIterator, iteratorToSpill, 3);

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

class ContainerInMemIterTest : public ServiceContextMongoDTest {
    // TODO (SERVER-116165): Remove.
    RAIIServerParameterControllerForTest _ffContainerWrites{"featureFlagContainerWrites", true};
};

TEST_F(ContainerInMemIterTest, SpillDoesNotChangeResultAndUpdateStatistics) {
    static const int data[] = {6, 3, 7, 4, 0, 9, 5, 7, 1, 8};

    unittest::TempDir spillDir("InMemIterTests");
    SorterTracker sorterTracker;
    const SortOptions opts = SortOptions().Tracker(&sorterTracker);
    ContainerTraits<> containerTraits(makeOperationContext());
    auto spiller = containerTraits.makeSpiller(opts, spillDir.path());

    auto expectedIterator = makeInMemIterator(data, spiller);
    auto iteratorToSpill = makeInMemIterator(data, spiller);
    ASSERT_ITERATORS_EQUIVALENT_FOR_N_STEPS(expectedIterator, iteratorToSpill, 3);

    ASSERT_TRUE(iteratorToSpill->spillable());
    auto spilledIterator = iteratorToSpill->spill(opts, IWSorter::Settings{});
    ASSERT_FALSE(spilledIterator->spillable());
    ASSERT_ITERATORS_EQUIVALENT(expectedIterator, spilledIterator);

    ASSERT_EQ(sorterTracker.spilledRanges.loadRelaxed(), 1);
    ASSERT_EQ(sorterTracker.spilledKeyValuePairs.loadRelaxed(), 7);
}

/**
 * This suite includes test cases for resumable index builds where the Sorter is reconstructed from
 * state persisted to disk during a previous clean shutdown.
 */
class MakeFromExistingRangesFixture : public ServiceContextMongoDTest {
public:
    static std::vector<SorterRange> makeSampleRanges();
    static std::unique_ptr<FileBasedSpiller<IntWrapper, IntWrapper, IWComparator>> makeSpiller(
        const SortOptions& opts,
        const boost::filesystem::path& path,
        SorterFileStats* fileStats,
        SorterChecksumVersion = sorter::kLatestChecksumVersion,
        std::string storageIdentifier = "");
};

// static
std::vector<SorterRange> MakeFromExistingRangesFixture::makeSampleRanges() {
    std::vector<SorterRange> ranges;
    // Sample data extracted from resumable_index_build_bulk_load_phase.js test run.
    ranges.push_back({0, 24, 18294710});
    return ranges;
}

// static
std::unique_ptr<FileBasedSpiller<IntWrapper, IntWrapper, IWComparator>>
MakeFromExistingRangesFixture::makeSpiller(const SortOptions& opts,
                                           const boost::filesystem::path& spillDir,
                                           SorterFileStats* fileStats,
                                           const SorterChecksumVersion checksumVersion,
                                           std::string storageIdentifier) {
    return makeFileSpiller(
        opts, spillDir, fileStats, checksumVersion, std::move(storageIdentifier));
}

template <typename Traits>
class MakeFromExistingRangesTypedTestBase : public MakeFromExistingRangesFixture {
public:
    static_assert(test::StorageTraits<Traits>);
    static constexpr bool kHasFileStats = Traits::kHasFileStats;

protected:
    void SetUp() override {
        MakeFromExistingRangesFixture::SetUp();
        if constexpr (std::is_constructible_v<Traits, ServiceContext::UniqueOperationContext>) {
            _storage.emplace(this->makeOperationContext());
        } else {
            _storage.emplace();
        }
    }

    Traits& storage() {
        return *_storage;
    }

private:
    boost::optional<Traits> _storage;
};

template <typename Traits>
class FileBasedMakeFromExistingRangesTest : public MakeFromExistingRangesTypedTestBase<Traits> {};

template <typename Traits>
class MakeFromExistingRangesTest : public MakeFromExistingRangesTypedTestBase<Traits> {
public:
    // TODO (SERVER-116165): Remove.
    RAIIServerParameterControllerForTest ffContainerWrites{"featureFlagContainerWrites", true};
};

using MakeFromExistingRangesTypes = ::testing::Types<FileTraits<>, ContainerTraits<>>;
TYPED_TEST_SUITE(MakeFromExistingRangesTest, MakeFromExistingRangesTypes);

using SorterMakeFromExistingRangesFileBasedTypes = ::testing::Types<FileTraits<>>;
TYPED_TEST_SUITE(FileBasedMakeFromExistingRangesTest, SorterMakeFromExistingRangesFileBasedTypes);

// Note that these tests use a spiller but do not exercise any of its behavior.
namespace {
using MakeFromExistingRangesDeathTest = MakeFromExistingRangesFixture;
DEATH_TEST_F(
    MakeFromExistingRangesDeathTest,
    NonZeroLimit,
    "Creating a Sorter from existing ranges is only available with the NoLimitSorter (limit 0)") {
    unittest::TempDir spillDir = makeSpillDir();
    auto opts = SortOptions().Limit(1ULL);
    IWSorter::template makeFromExistingRanges<IWComparator>(
        "",
        {},
        opts,
        IWComparator(ASC),
        makeSpiller(opts, spillDir.path(), /*fileStats=*/nullptr),
        /*settings=*/{});
}

DEATH_TEST_F(MakeFromExistingRangesDeathTest,
             EmptyStorageIdentifier,
             "!storageIdentifier.empty()") {
    unittest::TempDir spillDir = makeSpillDir();
    std::string storageIdentifier;
    auto opts = SortOptions();
    IWSorter::template makeFromExistingRanges<IWComparator>(
        storageIdentifier,
        {},
        opts,
        IWComparator(ASC),
        makeSpiller(opts,
                    spillDir.path(),
                    /*fileStats=*/nullptr,
                    sorter::kLatestChecksumVersion,
                    storageIdentifier),
        /*settings=*/{});
}

DEATH_TEST_F(MakeFromExistingRangesDeathTest, NullSpiller, "this->_spiller != nullptr") {
    unittest::TempDir spillDir = makeSpillDir();
    SorterTracker sorterTracker;

    auto opts =
        SortOptions().Limit(0).MaxMemoryUsageBytes(sizeof(IWSorter::Data)).Tracker(&sorterTracker);

    IWPair pairInsertedBeforeShutdown(1, 100);

    // This test uses two sorters. The first sorter is used to persist data to disk in a shutdown
    // scenario. On startup, we will fail to restore the original state due to having a nullptr
    // Spiller.
    IWSorter::PersistedState state;
    {
        auto sorterBeforeShutdown = IWSorter::template make<IWComparator>(
            opts,
            IWComparator(ASC),
            makeSpiller(opts, spillDir.path(), /*fileStats=*/nullptr),
            /*settings=*/{});
        sorterBeforeShutdown->add(pairInsertedBeforeShutdown.first,
                                  pairInsertedBeforeShutdown.second);
        state = sorterBeforeShutdown->persistDataForShutdown();
        ASSERT_FALSE(state.storageIdentifier.empty());
        ASSERT_EQUALS(1U, state.ranges.size()) << state.ranges.size();
        ASSERT_EQ(1, sorterBeforeShutdown->stats().numSorted());
    }

    // On restart, reconstruct sorter from persisted state.
    // We should fail because we are using a nullptr as the Spiller.
    IWSorter::template makeFromExistingRanges<IWComparator>(
        state.storageIdentifier,
        state.ranges,
        opts,
        IWComparator(ASC),
        std::shared_ptr<FileBasedSpiller<IntWrapper, IntWrapper, IWComparator>>(nullptr),
        /*settings=*/{});
}
}  // namespace

TYPED_TEST(FileBasedMakeFromExistingRangesTest, MissingFileOnResume) {
    auto storageIdentifier = "unused_sorter_storage";
    unittest::TempDir spillDir = makeSpillDir();
    SorterTracker sorterTracker;
    auto opts = SortOptions().Tracker(&sorterTracker);
    ASSERT_THROWS(
        IWSorter::template makeFromExistingRanges<IWComparator>(
            storageIdentifier,
            {},
            opts,
            IWComparator(ASC),
            this->storage().makeSpillerForResume(
                opts, spillDir.path(), sorter::kLatestChecksumVersion, storageIdentifier, {}),
            /*settings=*/{}),
        std::exception);
}

TYPED_TEST(FileBasedMakeFromExistingRangesTest, MissingStorage) {
    auto storageIdentifier = "unused_sorter_storage";
    auto spillDir = "unused_storage_location";
    SorterTracker sorterTracker;
    auto opts = SortOptions().Tracker(&sorterTracker);
    auto ranges = MakeFromExistingRangesFixture::makeSampleRanges();
    ASSERT_THROWS_WITH_CHECK(
        IWSorter::template makeFromExistingRanges<IWComparator>(
            storageIdentifier,
            ranges,
            opts,
            IWComparator(ASC),
            this->storage().makeSpillerForResume(
                opts, spillDir, sorter::kLatestChecksumVersion, storageIdentifier, ranges),
            /*settings=*/{}),
        std::exception,
        [&](const auto& ex) {
            ASSERT_STRING_CONTAINS(ex.what(), spillDir);
            ASSERT_STRING_CONTAINS(ex.what(), storageIdentifier);
        });
}

TYPED_TEST(FileBasedMakeFromExistingRangesTest, EmptyStorage) {
    unittest::TempDir spillDir = makeSpillDir();
    auto storageIdentifier = this->storage().makeEmptyStorage(spillDir.path());
    SorterTracker sorterTracker;
    auto opts = SortOptions().Tracker(&sorterTracker);
    auto ranges = MakeFromExistingRangesFixture::makeSampleRanges();
    // Throws unexpected empty storage.
    ASSERT_THROWS_CODE(
        IWSorter::template makeFromExistingRanges<IWComparator>(
            storageIdentifier,
            ranges,
            opts,
            IWComparator(ASC),
            this->storage().makeSpillerForResume(
                opts, spillDir.path(), sorter::kLatestChecksumVersion, storageIdentifier, ranges),
            /*settings=*/{}),
        DBException,
        16815);
}

TYPED_TEST(FileBasedMakeFromExistingRangesTest, CorruptedStorage) {
    unittest::TempDir spillDir = makeSpillDir();
    SorterTracker sorterTracker;
    auto opts = SortOptions().Tracker(&sorterTracker);
    auto storageIdentifier = this->storage().makeCorruptedStorage(spillDir.path());
    auto ranges = MakeFromExistingRangesFixture::makeSampleRanges();
    auto sorter = IWSorter::template makeFromExistingRanges<IWComparator>(
        storageIdentifier,
        ranges,
        opts,
        IWComparator(ASC),
        this->storage().makeSpillerForResume(
            opts, spillDir.path(), sorter::kLatestChecksumVersion, storageIdentifier, ranges),
        /*settings=*/{});

    // The number of spills is set when NoLimitSorter is constructed from existing ranges.
    ASSERT_EQ(ranges.size(), sorter->stats().spilledRanges());
    ASSERT_EQ(0, sorter->stats().numSorted());

    // Error reading storage.
    ASSERT_THROWS_CODE(sorter->done(), DBException, TypeParam::kCorruptedStorageErrorCode);
}

TYPED_TEST(MakeFromExistingRangesTest, RoundTrip) {
    unittest::TempDir spillDir = makeSpillDir();
    SorterTracker sorterTracker;

    auto opts =
        SortOptions()
            .Tracker(&sorterTracker)
            .MaxMemoryUsageBytes(sizeof(IWSorter::Data) + this->storage().iteratorSizeBytes());

    IWPair pairInsertedBeforeShutdown(1, 100);

    // This test uses two sorters. The first sorter is used to persist data to disk in a shutdown
    // scenario. On startup, we will restore the original state of the sorter using the persisted
    // data.
    IWSorter::PersistedState state;
    {
        auto sorterBeforeShutdown =
            IWSorter::make(opts,
                           IWComparator(ASC),
                           this->storage().makeSpiller(opts, spillDir.path()),
                           /*settings=*/{});
        sorterBeforeShutdown->add(pairInsertedBeforeShutdown.first,
                                  pairInsertedBeforeShutdown.second);
        state = sorterBeforeShutdown->persistDataForShutdown();
        ASSERT_FALSE(state.storageIdentifier.empty());
        ASSERT_EQUALS(1U, state.ranges.size()) << state.ranges.size();
        ASSERT_EQ(1, sorterBeforeShutdown->stats().numSorted());
    }

    // On restart, reconstruct sorter from persisted state.
    auto sorter = IWSorter::template makeFromExistingRanges<IWComparator>(
        state.storageIdentifier,
        state.ranges,
        opts,
        IWComparator(ASC),
        this->storage().makeSpillerForResume(opts,
                                             spillDir.path(),
                                             sorter::kLatestChecksumVersion,
                                             state.storageIdentifier,
                                             state.ranges),
        /*settings=*/{});

    // The number of spills is set when NoLimitSorter is constructed from existing ranges.
    ASSERT_EQ(state.ranges.size(), sorter->stats().spilledRanges());

    // Ensure that the restored sorter can accept additional data.
    IWPair pairInsertedAfterStartup(2, 200);
    sorter->add(pairInsertedAfterStartup.first, pairInsertedAfterStartup.second);

    // Technically this sorter has not sorted anything.
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

TYPED_TEST(MakeFromExistingRangesTest, GetPersistedState) {
    auto spillDir = makeSpillDir();
    auto opts = SortOptions{};
    auto sorter = IWSorter::make(
        opts, IWComparator(ASC), this->storage().makeSpiller(opts, spillDir.path()), {});

    IWPair data{1, 100};
    sorter->add(data.first, data.second);

    // Before spilling, Sorter::getPersistedState returns no sorted ranges.
    auto state = sorter->getPersistedState();
    EXPECT_FALSE(state.storageIdentifier.empty());
    EXPECT_EQ(state.ranges.size(), 0);

    // Sorter::persistDataForShutdown forces a spill and returns the sorted range.
    state = sorter->persistDataForShutdown();
    EXPECT_FALSE(state.storageIdentifier.empty());
    EXPECT_EQ(state.ranges.size(), 1);

    // After spilling, Sorter::getPersistedState returns the same as Sorter::persistDataForShutdown.
    state = sorter->getPersistedState();
    EXPECT_FALSE(state.storageIdentifier.empty());
    EXPECT_EQ(state.ranges.size(), 1);
}

TYPED_TEST(MakeFromExistingRangesTest, NextWithDeferredValues) {
    unittest::TempDir spillDir = makeSpillDir();
    auto opts = SortOptions().Tracker(nullptr);

    IWPair pair1(1, 100);
    IWPair pair2(2, 200);

    std::unique_ptr<SortedStorageWriter<IntWrapper, IntWrapper>> writer =
        this->storage().makeWriter(opts, spillDir.path());
    writer->addAlreadySorted(pair1.first, pair1.second);
    writer->addAlreadySorted(pair2.first, pair2.second);
    auto iter = writer->done();

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

TYPED_TEST(MakeFromExistingRangesTest, MergeSpillsTracksMergedSpillBatches) {
    unittest::TempDir spillDir = makeSpillDir();
    auto opts = SortOptions();

    auto spiller = this->storage().makeSpiller(opts, spillDir.path());
    using IteratorPtr = std::shared_ptr<sorter::Iterator<IntWrapper, IntWrapper>>;

    constexpr size_t kInitialRanges = 7;
    constexpr size_t kNumTargetedSpills = 2;
    constexpr size_t kMaxSpillsPerMerge = 3;
    // [1, 2, 3, 4, 5, 6, 7] initial
    // [123, 456, 7]         3 merges
    // [1234567]             1 merge
    constexpr size_t kExpectedMergedSpills = 4;

    std::vector<IteratorPtr> ranges;
    ranges.reserve(kInitialRanges);
    for (size_t i = 0; i < kInitialRanges; ++i) {
        std::vector<IWPair> oneRecord{{static_cast<int>(i), -static_cast<int>(i)}};
        ranges.push_back(spiller->spill(opts, IWSorter::Settings{}, oneRecord));
    }

    SorterTracker tracker;
    SorterStats sorterStats{&tracker};
    spiller->mergeSpills(opts,
                         IWSorter::Settings{},
                         sorterStats,
                         ranges,
                         IWComparator(ASC),
                         kNumTargetedSpills,
                         kMaxSpillsPerMerge);

    ASSERT_LTE(ranges.size(), kNumTargetedSpills);
    ASSERT_EQ(sorterStats.mergedSpills(), kExpectedMergedSpills);
    ASSERT_EQ(tracker.mergedSpills.loadRelaxed(), kExpectedMergedSpills);
}

TYPED_TEST(MakeFromExistingRangesTest, MergeSpillsRejectsDisjointRanges) {
    unittest::TempDir spillDir = makeSpillDir();
    auto opts = SortOptions();

    auto spiller = this->storage().makeSpiller(opts, spillDir.path());
    using IteratorPtr = std::shared_ptr<sorter::Iterator<IntWrapper, IntWrapper>>;
    auto spillSingleKey = [&](int key) -> IteratorPtr {
        std::vector<IWPair> oneRecord{{key, -key}};
        return spiller->spill(opts, IWSorter::Settings{}, oneRecord);
    };

    auto firstRange = spillSingleKey(50);   // [0, 1)
    auto secondRange = spillSingleKey(75);  // [1, 2)
    auto thirdRange = spillSingleKey(100);  // [2, 3)

    // Reorder to create a gap: [0, 1), [2, 3), [1, 2).
    std::vector<IteratorPtr> disjointRanges{firstRange, thirdRange, secondRange};
    SorterStats sorterStats{nullptr};

    ASSERT_THROWS_CODE(
        spiller->mergeSpills(
            opts, IWSorter::Settings{}, sorterStats, disjointRanges, IWComparator(ASC), 2, 2),
        DBException,
        12017001);
}

TYPED_TEST(MakeFromExistingRangesTest, MergeSpillsRejectsDecreasingOffsets) {
    unittest::TempDir spillDir = makeSpillDir();
    auto opts = SortOptions();

    auto spiller = this->storage().makeSpiller(opts, spillDir.path());
    using IteratorPtr = std::shared_ptr<sorter::Iterator<IntWrapper, IntWrapper>>;
    auto makeRange = [](int64_t start, int64_t end) -> IteratorPtr {
        return std::make_shared<RangeOnlyIterator>(SorterRange{start, end, 0});
    };

    std::vector<IteratorPtr> invalidRanges{
        makeRange(0, 1),
        makeRange(2, 1),  // end < start
    };
    SorterStats sorterStats{nullptr};

    ASSERT_THROWS_CODE(
        spiller->mergeSpills(
            opts, IWSorter::Settings{}, sorterStats, invalidRanges, IWComparator(ASC), 1, 2),
        DBException,
        12017000);
}

TYPED_TEST(FileBasedMakeFromExistingRangesTest, ChecksumVersion) {
    unittest::TempDir spillDir = makeSpillDir();
    auto opts = SortOptions().Tracker(nullptr);

    // By default checksum version should be v2
    {
        auto sorter = IWSorter::make(opts,
                                     IWComparator(ASC),
                                     this->storage().makeSpiller(opts, spillDir.path()),
                                     /*settings=*/{});
        sorter->add(1, -1);
        auto state = sorter->persistDataForShutdown();
        ASSERT_EQUALS(state.ranges[0].getChecksumVersion(), sorter::kLatestChecksumVersion);
        ASSERT_EQUALS(state.ranges[0].getChecksum(), 1921809301);
    }

    // Setting checksum version to v1 results in using v1 but getChecksumVersion() returning none
    // because v1 did not persist a version.
    {
        auto sorter = IWSorter::make(
            opts,
            IWComparator(ASC),
            this->storage().makeSpiller(opts, spillDir.path(), SorterChecksumVersion::v1),
            /*settings=*/{});
        sorter->add(1, -1);
        auto state = sorter->persistDataForShutdown();
        ASSERT_EQUALS(state.ranges[0].getChecksumVersion(), boost::none);
        ASSERT_EQUALS(state.ranges[0].getChecksum(), 4121002018);
    }
}

TYPED_TEST(MakeFromExistingRangesTest, ValidChecksumValidation) {
    unittest::TempDir spillDir = makeSpillDir();
    auto state = this->storage().makeSpillState(spillDir.path());
    auto it = IWSorter::template makeFromExistingRanges<IWComparator>(
                  state.storageIdentifier,
                  state.ranges,
                  state.opts,
                  state.comp,
                  this->storage().makeSpillerForResume(state.opts,
                                                       spillDir.path(),
                                                       sorter::kLatestChecksumVersion,
                                                       state.storageIdentifier,
                                                       state.ranges),
                  /*settings=*/{})
                  ->done();
    ASSERT_ITERATORS_EQUIVALENT(it, std::make_unique<IntIterator>(0, 10));
}

TYPED_TEST(FileBasedMakeFromExistingRangesTest, IncompleteReadDoesNotReportChecksumError) {
    unittest::TempDir spillDir = makeSpillDir();
    auto state = this->storage().makeSpillState(spillDir.path());
    this->storage().corruptSpillState(state);
    auto it = IWSorter::template makeFromExistingRanges<IWComparator>(
                  state.storageIdentifier,
                  state.ranges,
                  state.opts,
                  state.comp,
                  this->storage().makeSpillerForResume(state.opts,
                                                       spillDir.path(),
                                                       sorter::kLatestChecksumVersion,
                                                       state.storageIdentifier,
                                                       state.ranges),
                  /*settings=*/{})
                  ->done();
    // Read the first (and only) block of data, but don't deserialize any of it
    ASSERT(it->more());
    // it's destructor doesn't check the checksum since we didn't use everything
}

namespace {
using FileBasedMakeFromExistingRangesDeathTest = MakeFromExistingRangesFixture;

class ContainerBasedMakeFromExistingRangesDeathTest : public MakeFromExistingRangesFixture {
    RAIIServerParameterControllerForTest _ffContainerWrites{"featureFlagContainerWrites", true};

protected:
    void SetUp() override {
        MakeFromExistingRangesFixture::SetUp();
        _traits.emplace(makeOperationContext());
    }

    ContainerTraits<>& storage() {
        return *_traits;
    }

private:
    boost::optional<ContainerTraits<>> _traits;
};

DEATH_TEST_F(ContainerBasedMakeFromExistingRangesDeathTest,
             CompleteReadReportsChecksumError,
             "Possible corruption of data.") {
    unittest::TempDir spillDir = makeSpillDir();
    auto state = storage().makeSpillState(spillDir.path());
    storage().corruptSpillState(state);
    auto it = IWSorter::template makeFromExistingRanges<IWComparator>(
                  state.storageIdentifier,
                  state.ranges,
                  state.opts,
                  state.comp,
                  storage().makeSpillerForResume(state.opts,
                                                 spillDir.path(),
                                                 sorter::kLatestChecksumVersion,
                                                 state.storageIdentifier,
                                                 state.ranges),
                  /*settings=*/{})
                  ->done();
    ASSERT_ITERATORS_EQUIVALENT(it, std::make_unique<IntIterator>(0, 10));
}

DEATH_TEST_F(FileBasedMakeFromExistingRangesDeathTest,
             CompleteReadReportsChecksumError,
             "Data read from disk does not match what was written to disk.") {
    unittest::TempDir spillDir = makeSpillDir();
    auto state = FileTraits<>::makeSpillState(spillDir.path());
    FileTraits<>::corruptSpillState(state);
    auto it = IWSorter::template makeFromExistingRanges<IWComparator>(
                  state.storageIdentifier,
                  state.ranges,
                  state.opts,
                  state.comp,
                  FileTraits<>::makeSpillerForResume(state.opts,
                                                     spillDir.path(),
                                                     sorter::kLatestChecksumVersion,
                                                     state.storageIdentifier,
                                                     state.ranges),
                  /*settings=*/{})
                  ->done();
    ASSERT_ITERATORS_EQUIVALENT(it, std::make_unique<IntIterator>(0, 10));
    // it's destructor ends up checking the checksum and aborts due to it being wrong
}

DEATH_TEST_F(FileBasedMakeFromExistingRangesDeathTest,
             CompleteReadReportsChecksumErrorFromIncorrectChecksumVersion,
             "Data read from disk does not match what was written to disk.") {
    unittest::TempDir spillDir = makeSpillDir();
    auto state = FileTraits<>::makeSpillState(spillDir.path());
    state.ranges[0].setChecksumVersion(boost::none);
    auto it = IWSorter::template makeFromExistingRanges<IWComparator>(
                  state.storageIdentifier,
                  state.ranges,
                  state.opts,
                  state.comp,
                  makeSpiller(state.opts,
                              spillDir.path(),
                              /*fileStats=*/nullptr,
                              sorter::kLatestChecksumVersion,
                              state.storageIdentifier),
                  /*settings=*/{})
                  ->done();
    ASSERT_ITERATORS_EQUIVALENT(it, std::make_unique<IntIterator>(0, 10));
    // it's destructor ends up checking the checksum and aborts due to it being wrong (because we
    // used the wrong checksum algorithm)
}
}  // namespace

using Key = IntWrapper;
struct Doc {
    Key time;

    bool operator==(const Doc& other) const {
        return time == other.time;
    }

    void serializeForSorter(BufBuilder& buf) const {
        time.serializeForSorter(buf);
    }

    struct SorterDeserializeSettings {};
    static Doc deserializeForSorter(BufReader& buf, const SorterDeserializeSettings&) {
        return {IntWrapper::deserializeForSorter(buf, {})};
    }

    int memUsageForSorter() const {
        return sizeof(Doc);
    }

    Doc getOwned() const {
        return *this;
    }

    void makeOwned() {}
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

class BoundedSorterTestBase : public ServiceContextMongoDTest {
protected:
    void SetUp() override {
        ServiceContextMongoDTest::SetUp();
        sorter = makeAsc({});
    }

public:
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

    std::unique_ptr<S> makeAsc(
        SortOptions options,
        std::shared_ptr<sorter::Spiller<Key, Doc, ComparatorAsc>> spiller = nullptr,
        bool checkInput = true) {
        return std::make_unique<SAsc>(
            options, ComparatorAsc{}, BoundMakerAsc{}, spiller, checkInput);
    }
    std::unique_ptr<S> makeAscNoBound(
        SortOptions options,
        std::shared_ptr<sorter::Spiller<Key, Doc, ComparatorAsc>> spiller = nullptr,
        bool checkInput = true) {
        return std::make_unique<SAscNoBound>(
            options, ComparatorAsc{}, NoBoundAsc{}, spiller, checkInput);
    }
    std::unique_ptr<S> makeDesc(
        SortOptions options,
        std::shared_ptr<sorter::Spiller<Key, Doc, ComparatorDesc>> spiller = nullptr,
        bool checkInput = true) {
        return std::make_unique<SDesc>(
            options, ComparatorDesc{}, BoundMakerDesc{}, spiller, checkInput);
    }

    SorterTracker sorterTracker;
    std::unique_ptr<S> sorter;
};

template <typename Traits>
class BoundedSorterTest : public BoundedSorterTestBase {
public:
    Traits& storage() {
        return *_storage;
    }

protected:
    void SetUp() override {
        BoundedSorterTestBase::SetUp();
        if constexpr (std::is_constructible_v<Traits, ServiceContext::UniqueOperationContext>) {
            _storage.emplace(this->makeOperationContext());
        } else {
            _storage.emplace();
        }
    }

    void TearDown() override {
        sorter.reset();
        _storage.reset();
        BoundedSorterTestBase::TearDown();
    }

private:
    // TODO (SERVER-109578): Remove.
    RAIIServerParameterControllerForTest _ffContainerWrites{"featureFlagContainerWrites", true};
    boost::optional<Traits> _storage;
};

using BoundedSorterTraits =
    ::testing::Types<FileTraits<Key, Doc, ComparatorAsc>, ContainerTraits<Key, Doc, ComparatorAsc>>;
TYPED_TEST_SUITE(BoundedSorterTest, BoundedSorterTraits);

using FileBasedBoundedSorterTest = BoundedSorterTestBase;

TYPED_TEST(BoundedSorterTest, Empty) {
    ASSERT(this->sorter->getState() == S::State::kWait);

    this->sorter->done();
    ASSERT(this->sorter->getState() == S::State::kDone);
}
TYPED_TEST(BoundedSorterTest, Sorted) {
    auto output = this->sort({
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
    this->assertSorted(output);
}

TYPED_TEST(BoundedSorterTest, SortedExceptOne) {
    auto output = this->sort({
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
    this->assertSorted(output);
}

TYPED_TEST(BoundedSorterTest, AlmostSorted) {
    auto output = this->sort({
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
    this->assertSorted(output);
}

TYPED_TEST(BoundedSorterTest, WrongInput) {
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
    this->sorter = this->makeAsc({}, /*spiller=*/nullptr, /*checkInput*/ false);
    auto output = this->sort(input);
    ASSERT_EQ(output.size(), 7);

    ASSERT_EQ(output[0].time, 3);
    ASSERT_EQ(output[1].time, 4);
    ASSERT_EQ(output[2].time, 1);  // Out of order.
    ASSERT_EQ(output[3].time, 5);
    ASSERT_EQ(output[4].time, 10);
    ASSERT_EQ(output[5].time, 15);
    ASSERT_EQ(output[6].time, 16);

    // Test that by default, bad input like this would be detected.
    this->sorter = this->makeAsc({});
    ASSERT(this->sorter->checkInput());
    ASSERT_THROWS_CODE(this->sort(input), DBException, 6369910);
}

TYPED_TEST(BoundedSorterTest, MemoryLimitsNoExtSortAllowed) {
    auto options = SortOptions().MaxMemoryUsageBytes(16);
    this->sorter = this->makeAsc(options);

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
        this->sort(input), DBException, ErrorCodes::QueryExceededMemoryLimitNoDiskUseAllowed);
}

TYPED_TEST(BoundedSorterTest, SpillSorted) {
    unittest::TempDir spillDir = makeSpillDir();
    auto options = SortOptions().MaxMemoryUsageBytes(16).Tracker(&this->sorterTracker);
    this->sorter = this->makeAsc(options, this->storage().makeSpiller(options, spillDir.path()));

    auto output = this->sort({
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
    this->assertSorted(output);

    ASSERT_EQ(this->sorter->stats().spilledRanges(), 3);
}

TYPED_TEST(BoundedSorterTest, SpillSortedExceptOne) {
    unittest::TempDir spillDir = makeSpillDir();
    auto options = SortOptions().MaxMemoryUsageBytes(16);
    this->sorter = this->makeAsc(options, this->storage().makeSpiller(options, spillDir.path()));

    auto output = this->sort({
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
    this->assertSorted(output);

    ASSERT_EQ(this->sorter->stats().spilledRanges(), 3);
}

TYPED_TEST(BoundedSorterTest, SpillAlmostSorted) {
    unittest::TempDir spillDir = makeSpillDir();
    auto options = SortOptions().MaxMemoryUsageBytes(16).Tracker(&this->sorterTracker);
    this->sorter = this->makeAsc(options, this->storage().makeSpiller(options, spillDir.path()));

    auto output = this->sort({
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
    this->assertSorted(output);

    ASSERT_EQ(this->sorter->stats().spilledRanges(), 2);
}

TYPED_TEST(BoundedSorterTest, SpillWrongInput) {
    unittest::TempDir spillDir = makeSpillDir();
    auto options = SortOptions().MaxMemoryUsageBytes(16);

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
    this->sorter = this->makeAsc(
        options, this->storage().makeSpiller(options, spillDir.path()), /*checkInput=*/false);
    auto output = this->sort(input);
    ASSERT_EQ(output.size(), 7);

    ASSERT_EQ(output[0].time, 3);
    ASSERT_EQ(output[1].time, 4);
    ASSERT_EQ(output[2].time, 1);  // Out of order.
    ASSERT_EQ(output[3].time, 5);
    ASSERT_EQ(output[4].time, 10);
    ASSERT_EQ(output[5].time, 15);
    ASSERT_EQ(output[6].time, 16);

    ASSERT_EQ(this->sorter->stats().spilledRanges(), 2);


    // Test that by default, bad input like this would be detected.
    this->sorter = this->makeAsc(options, this->storage().makeSpiller(options, spillDir.path()));
    ASSERT(this->sorter->checkInput());
    ASSERT_THROWS_CODE(this->sort(input), DBException, 6369910);
}

TYPED_TEST(BoundedSorterTest, LimitNoSpill) {
    unittest::TempDir spillDir = makeSpillDir();
    auto options = SortOptions().MaxMemoryUsageBytes(40).Tracker(&this->sorterTracker).Limit(2);
    this->sorter = this->makeAsc(options, this->storage().makeSpiller(options, spillDir.path()));

    auto output = this->sort(
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
    this->assertSorted(output);
    // Also check that the correct values made it into the top K.
    ASSERT_EQ(output[0].time, 0);
    ASSERT_EQ(output[1].time, 3);

    ASSERT_EQ(this->sorter->stats().spilledRanges(), 0);
}

TYPED_TEST(BoundedSorterTest, LimitSpill) {
    unittest::TempDir spillDir = makeSpillDir();
    auto options = SortOptions().MaxMemoryUsageBytes(40).Tracker(&this->sorterTracker).Limit(3);
    this->sorter = this->makeAsc(options, this->storage().makeSpiller(options, spillDir.path()));

    auto output = this->sort(
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
    this->assertSorted(output);
    // Also check that the correct values made it into the top K.
    ASSERT_EQ(output[0].time, 0);
    ASSERT_EQ(output[1].time, 3);
    ASSERT_EQ(output[2].time, 10);

    ASSERT_EQ(this->sorter->stats().spilledRanges(), 1);
}

TYPED_TEST(BoundedSorterTest, ForceSpill) {
    unittest::TempDir spillDir = makeSpillDir();
    auto options =
        SortOptions().MaxMemoryUsageBytes(100 * 1024 * 1024).Tracker(&this->sorterTracker);
    this->sorter = this->makeAsc(options, this->storage().makeSpiller(options, spillDir.path()));

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
        this->sorter->add(input[i].time, std::move(input[i]));
        while (this->sorter->getState() == S::State::kReady) {
            output.push_back(this->sorter->next().second);
        }
        if (i % 3 == 2) {
            this->sorter->forceSpill();
        }
    }
    this->sorter->done();

    while (this->sorter->getState() == S::State::kReady) {
        output.push_back(this->sorter->next().second);
    }
    ASSERT(this->sorter->getState() == S::State::kDone);

    ASSERT_EQ(output.size(), input.size());
    this->assertSorted(output);

    ASSERT_EQ(this->sorter->stats().spilledRanges(), 5);
    ASSERT_EQ(this->sorter->stats().spilledKeyValuePairs(), 13);
}

TYPED_TEST(BoundedSorterTest, DescSorted) {
    this->sorter = this->makeDesc({});
    auto output = this->sort({
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
    this->assertSorted(output, /* ascending */ false);
}

TYPED_TEST(BoundedSorterTest, DescSortedExceptOne) {
    this->sorter = this->makeDesc({});

    auto output = this->sort({

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
    this->assertSorted(output, /* ascending */ false);
}

TYPED_TEST(BoundedSorterTest, DescAlmostSorted) {
    this->sorter = this->makeDesc({});

    auto output = this->sort({
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
    this->assertSorted(output, /* ascending */ false);
}

TYPED_TEST(BoundedSorterTest, DescWrongInput) {
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
    this->sorter = this->makeDesc({}, /*spiller=*/nullptr, /*checkInput=*/false);
    auto output = this->sort(input);
    ASSERT_EQ(output.size(), 7);

    ASSERT_EQ(output[0].time, 16);
    ASSERT_EQ(output[1].time, 14);
    ASSERT_EQ(output[2].time, 15);  // Out of order.
    ASSERT_EQ(output[3].time, 10);
    ASSERT_EQ(output[4].time, 5);
    ASSERT_EQ(output[5].time, 3);
    ASSERT_EQ(output[6].time, 1);

    // Test that by default, bad input like this would be detected.
    this->sorter = this->makeDesc({});
    ASSERT(this->sorter->checkInput());
    ASSERT_THROWS_CODE(this->sort(input), DBException, 6369910);
}

TYPED_TEST(BoundedSorterTest, CompoundAsc) {
    {
        auto output = this->sort({
            {1001},
            {1005},
            {1004},
            {1007},
        });
        this->assertSorted(output);
    }

    {
        // After restart(), the sorter accepts new input.
        // The new values are compared to each other, but not compared to any of the old values,
        // so it's fine for the new values to be smaller even though the sort is ascending.
        this->sorter->restart();
        auto output = this->sort({
            {1},
            {5},
            {4},
            {7},
        });
        this->assertSorted(output);
    }

    {
        // restart() can be called any number of times.
        this->sorter->restart();
        auto output = this->sort({
            {11},
            {15},
            {14},
            {17},
        });
        this->assertSorted(output);
    }
}

TYPED_TEST(BoundedSorterTest, CompoundDesc) {
    this->sorter = this->makeDesc({});
    {
        auto output = this->sort({
            {1007},
            {1004},
            {1005},
            {1001},
        });
        this->assertSorted(output, /* ascending */ false);
    }

    {
        // After restart(), the sorter accepts new input.
        // The new values are compared to each other, but not compared to any of the old values,
        // so it's fine for the new values to be smaller even though the sort is ascending.
        this->sorter->restart();
        auto output = this->sort({
            {7},
            {4},
            {5},
            {1},
        });
        this->assertSorted(output, /* ascending */ false);
    }

    {
        // restart() can be called any number of times.
        this->sorter->restart();
        auto output = this->sort({
            {17},
            {14},
            {15},
            {11},
        });
        this->assertSorted(output, /* ascending */ false);
    }
}

TYPED_TEST(BoundedSorterTest, CompoundLimit) {
    // A limit applies to the entire sorter, not to each partition of a compound sort.

    // Example where the limit lands in the first partition.
    this->sorter = this->makeAsc(SortOptions().Limit(2));
    {
        auto output = this->sort(
            {
                {1001},
                {1005},
                {1004},
                {1007},
            },
            2);
        this->assertSorted(output);
        // Also check that the correct values made it into the top K.
        ASSERT_EQ(output[0].time, 1001);
        ASSERT_EQ(output[1].time, 1004);

        this->sorter->restart();
        output = this->sort(
            {
                {1},
                {5},
                {4},
                {7},
            },
            0);

        this->sorter->restart();
        output = this->sort(
            {
                {11},
                {15},
                {14},
                {17},
            },
            0);
    }

    // Example where the limit lands in the second partition.
    this->sorter = this->makeAsc(SortOptions().Limit(6));
    {
        auto output = this->sort({
            {1001},
            {1005},
            {1004},
            {1007},
        });
        this->assertSorted(output);

        this->sorter->restart();
        output = this->sort(
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

        this->sorter->restart();
        output = this->sort(
            {
                {11},
                {15},
                {14},
                {17},
            },
            0);
    }
}

TYPED_TEST(BoundedSorterTest, CompoundSpill) {
    unittest::TempDir spillDir = makeSpillDir();
    auto options = SortOptions().Tracker(&this->sorterTracker).MaxMemoryUsageBytes(40);
    this->sorter = this->makeAsc(options, this->storage().makeSpiller(options, spillDir.path()));

    // When each partition is small enough, we don't spill.
    ASSERT_EQ(this->sorter->stats().spilledRanges(), 0);
    auto output = this->sort({
        {1001},
        {1007},
    });
    this->assertSorted(output);
    ASSERT_EQ(this->sorter->stats().spilledRanges(), 0);

    // If any individual partition is large enough, we do spill.
    this->sorter->restart();
    ASSERT_EQ(this->sorter->stats().spilledRanges(), 0);
    output = this->sort({
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
    this->assertSorted(output);
    ASSERT_EQ(this->sorter->stats().spilledRanges(), 1);

    // If later partitions are small again, they don't spill.
    this->sorter->restart();
    ASSERT_EQ(this->sorter->stats().spilledRanges(), 1);
    output = this->sort({
        {11},
        {17},
    });
    this->assertSorted(output);
    ASSERT_EQ(this->sorter->stats().spilledRanges(), 1);
}

TYPED_TEST(BoundedSorterTest, LargeSpill) {
    static const Key kKey = 1;
    static constexpr uint64_t kMemoryLimit = 4 * sorter::kSortedFileBufferSize;
    static const int kPerEntryMemUsage = kKey.memUsageForSorter() + Doc{kKey}.memUsageForSorter();
    static size_t kDocCountToCauseSpilling = (kMemoryLimit / kPerEntryMemUsage) + 1;

    unittest::TempDir spillDir = makeSpillDir();
    auto options = SortOptions().MaxMemoryUsageBytes(kMemoryLimit);
    this->sorter =
        this->makeAscNoBound(options, this->storage().makeSpiller(options, spillDir.path()));

    std::vector<Doc> input;
    input.reserve(kDocCountToCauseSpilling);
    for (size_t i = 0; i < kDocCountToCauseSpilling; ++i) {
        input.emplace_back(Doc{kKey});
    }

    this->assertSorted(this->sort(input));
    ASSERT_GTE(this->sorter->stats().spilledRanges(), 1);
}
template <typename Traits>
class SpillerMergeDiskSpaceTest : public MakeFromExistingRangesTypedTestBase<Traits> {
    // TODO (SERVER-116165): Remove.
    RAIIServerParameterControllerForTest ffContainerWrites{"featureFlagContainerWrites", true};
};

TYPED_TEST_SUITE(SpillerMergeDiskSpaceTest, MakeFromExistingRangesTypes);

TYPED_TEST(SpillerMergeDiskSpaceTest, MergeSpillsRespectsDiskSpaceCheck) {
    using Traits = TypeParam;
    // Simulate available disk space strictly below the test threshold so that any
    // call to ensureSufficientDiskSpaceForSpilling() will fail with OutOfDiskSpace.
    FailPointEnableBlock fp(
        "simulateAvailableDiskSpace",
        BSON("bytes" << static_cast<long long>(testSpillingMinAvailableDiskSpaceBytes - 1)));

    unittest::TempDir spillDir = makeSpillDir();
    SortOptions opts;

    auto& storage = this->storage();
    auto spiller = storage.makeSpiller(opts, spillDir.path(), sorter::kLatestChecksumVersion);
    ASSERT(spiller);

    using IteratorPtr = std::shared_ptr<sorter::Iterator<IntWrapper, IntWrapper>>;

    std::vector<IWPair> data{{1, 10}, {2, 20}, {3, 30}, {4, 40}};
    std::span<IWPair> span{data};

    std::vector<IteratorPtr> ranges;
    ranges.push_back(spiller->spill(opts, IWSorter::Settings{}, span.subspan(0, 2)));
    ranges.push_back(spiller->spill(opts, IWSorter::Settings{}, span.subspan(2, 2)));

    SorterStats sorterStats{/*sorterTracker=*/nullptr};

    // mergeSpills() must call ensureSufficientDiskSpaceForSpilling(...) and propagate
    // OutOfDiskSpace when the simulated available space is below the threshold.
    ASSERT_THROWS_CODE(spiller->mergeSpills(opts,
                                            IWSorter::Settings{},
                                            sorterStats,
                                            ranges,
                                            IWComparator(ASC),
                                            /*numTargetedSpills=*/1,
                                            /*maxSpillsPerMerge=*/2),
                       DBException,
                       ErrorCodes::OutOfDiskSpace);
}
}  // namespace
}  // namespace sorter
}  // namespace mongo

template class ::mongo::Sorter<::mongo::sorter::Key, ::mongo::sorter::Doc>;
template class ::mongo::BoundedSorter<::mongo::sorter::Key,
                                      ::mongo::sorter::Doc,
                                      ::mongo::sorter::IWComparator,
                                      ::mongo::sorter::BoundMakerAsc>;
template class ::mongo::BoundedSorter<::mongo::sorter::Key,
                                      ::mongo::sorter::Doc,
                                      ::mongo::sorter::IWComparator,
                                      ::mongo::sorter::BoundMakerDesc>;
