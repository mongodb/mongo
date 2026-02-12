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

#include "mongo/config.h"  // IWYU pragma: keep
#include "mongo/db/service_context_d_test_fixture.h"
#include "mongo/db/sorter/sorter_template_defs.h"
#include "mongo/db/sorter/sorter_test_utils.h"
#include "mongo/db/sorter/typed_sorter_test_utils.h"
#include "mongo/stdx/thread.h"  // IWYU pragma: keep
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"

#include <algorithm>
#include <ctime>
#include <fstream>  // IWYU pragma: keep
#include <memory>
#include <string>
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

unittest::TempDir makeTempDir() {
    auto name = fmt::format("{}_{}", unittest::getSuiteName(), unittest::getTestName());
    std::replace(name.begin(), name.end(), '/', '_');
    std::replace(name.begin(), name.end(), '\\', '_');
    return unittest::TempDir{name};
}

using InMemIterTest = unittest::Test;
using test::ContainerTraits;
using test::FileTraits;
using test::makeFileSorterSpiller;

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

    unittest::TempDir tempDir("InMemIterTests");
    SorterTracker sorterTracker;
    SorterFileStats sorterFileStats(&sorterTracker);
    const SortOptions opts = SortOptions().TempDir(tempDir.path()).Tracker(&sorterTracker);
    std::shared_ptr<FileBasedSorterSpiller<IntWrapper, IntWrapper>> spiller =
        std::make_shared<FileBasedSorterSpiller<IntWrapper, IntWrapper>>(tempDir.path(),
                                                                         &sorterFileStats);

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

/**
 * This suite includes test cases for resumable index builds where the Sorter is reconstructed from
 * state persisted to disk during a previous clean shutdown.
 */
class MakeFromExistingRangesFixture : public ServiceContextMongoDTest {
public:
    static std::vector<SorterRange> makeSampleRanges();
    static std::unique_ptr<FileBasedSorterSpiller<IntWrapper, IntWrapper>> makeSorterSpiller(
        const SortOptions& opts, SorterFileStats* fileStats, std::string storageIdentifier = "");
};

// static
std::vector<SorterRange> MakeFromExistingRangesFixture::makeSampleRanges() {
    std::vector<SorterRange> ranges;
    // Sample data extracted from resumable_index_build_bulk_load_phase.js test run.
    ranges.push_back({0, 24, 18294710});
    return ranges;
}

// static
std::unique_ptr<FileBasedSorterSpiller<IntWrapper, IntWrapper>>
MakeFromExistingRangesFixture::makeSorterSpiller(const SortOptions& opts,
                                                 SorterFileStats* fileStats,
                                                 std::string storageIdentifier) {
    return makeFileSorterSpiller(opts, fileStats, std::move(storageIdentifier));
}

template <typename Traits>
class MakeFromExistingRangesTypedTestBase : public MakeFromExistingRangesFixture {
public:
    static_assert(test::StorageTraits<Traits>);
    static constexpr bool kHasFileStats = Traits::kHasFileStats;
    static constexpr int kEmptyStorageErrorCode = Traits::kEmptyStorageErrorCode;
    static constexpr int kCorruptedStorageErrorCode = Traits::kCorruptedStorageErrorCode;

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
class MakeFromExistingRangesTest : public MakeFromExistingRangesTypedTestBase<Traits> {};

using MakeFromExistingRangesTypes = ::testing::Types<FileTraits, ContainerTraits>;
TYPED_TEST_SUITE(MakeFromExistingRangesTest, MakeFromExistingRangesTypes);

using SorterMakeFromExistingRangesFileBasedTypes = ::testing::Types<FileTraits>;
TYPED_TEST_SUITE(FileBasedMakeFromExistingRangesTest, SorterMakeFromExistingRangesFileBasedTypes);

// TODO SERVER-117323: Parameterize or otherwise gain equivalent coverage for the container-based
// sorter that these tests provide for the file-based sorter.
using MakeFromExistingRangesDeathTest = MakeFromExistingRangesFixture;
DEATH_TEST_F(
    MakeFromExistingRangesDeathTest,
    NonZeroLimit,
    "Creating a Sorter from existing ranges is only available with the NoLimitSorter (limit 0)") {
    auto opts = SortOptions().Limit(1ULL).TempDir("unused_storage_location");
    IWSorter::makeFromExistingRanges(
        "", {}, opts, IWComparator(ASC), makeSorterSpiller(opts, /*fileStats=*/nullptr));
}

DEATH_TEST_F(MakeFromExistingRangesDeathTest,
             EmptyStorageIdentifier,
             "!storageIdentifier.empty()") {
    std::string storageIdentifier;
    auto opts = SortOptions().TempDir("unused_storage_location");
    IWSorter::makeFromExistingRanges(
        storageIdentifier,
        {},
        opts,
        IWComparator(ASC),
        makeSorterSpiller(opts, /*fileStats=*/nullptr, storageIdentifier));
}

DEATH_TEST_F(MakeFromExistingRangesDeathTest, NullSorterSpiller, "this->_spiller != nullptr") {
    unittest::TempDir storageLocation = makeTempDir();
    SorterTracker sorterTracker;

    auto opts = SortOptions()
                    .Limit(0)
                    .TempDir(storageLocation.path())
                    .MaxMemoryUsageBytes(sizeof(IWSorter::Data))
                    .Tracker(&sorterTracker);

    IWPair pairInsertedBeforeShutdown(1, 100);

    // This test uses two sorters. The first sorter is used to persist data to disk in a shutdown
    // scenario. On startup, we will fail to restore the original state due to having a nullptr
    // SorterSpiller.
    IWSorter::PersistedState state;
    {
        auto sorterBeforeShutdown =
            IWSorter::make(opts, IWComparator(ASC), makeSorterSpiller(opts, /*fileStats=*/nullptr));
        sorterBeforeShutdown->add(pairInsertedBeforeShutdown.first,
                                  pairInsertedBeforeShutdown.second);
        state = sorterBeforeShutdown->persistDataForShutdown();
        ASSERT_FALSE(state.storageIdentifier.empty());
        ASSERT_EQUALS(1U, state.ranges.size()) << state.ranges.size();
        ASSERT_EQ(1, sorterBeforeShutdown->stats().numSorted());
    }

    // On restart, reconstruct sorter from persisted state.
    // We should fail because we are using a nullptr as the SorterSpiller.
    IWSorter::makeFromExistingRanges(
        state.storageIdentifier,
        state.ranges,
        opts,
        IWComparator(ASC),
        std::shared_ptr<FileBasedSorterSpiller<IntWrapper, IntWrapper>>(nullptr));
}

TYPED_TEST(FileBasedMakeFromExistingRangesTest, SkipFileCheckingOnEmptyRanges) {
    auto storageIdentifier = "unused_sorter_storage";
    SorterTracker sorterTracker;
    auto opts = SortOptions().TempDir("unused_storage_location").Tracker(&sorterTracker);
    auto sorter = IWSorter::makeFromExistingRanges(
        storageIdentifier,
        {},
        opts,
        IWComparator(ASC),
        this->storage().makeSpillerForResume(opts, storageIdentifier));

    ASSERT_EQ(0, sorter->stats().spilledRanges());

    auto iter = sorter->done();
    ASSERT_EQ(0, sorter->stats().numSorted());

    ASSERT_FALSE(iter->more());
}

TYPED_TEST(FileBasedMakeFromExistingRangesTest, MissingStorage) {
    auto storageIdentifier = "unused_sorter_storage";
    auto storageLocation = "unused_storage_location";
    SorterTracker sorterTracker;
    auto opts = SortOptions().TempDir(storageLocation).Tracker(&sorterTracker);
    ASSERT_THROWS_WITH_CHECK(IWSorter::makeFromExistingRanges(
                                 storageIdentifier,
                                 MakeFromExistingRangesFixture::makeSampleRanges(),
                                 opts,
                                 IWComparator(ASC),
                                 this->storage().makeSpillerForResume(opts, storageIdentifier)),
                             std::exception,
                             [&](const auto& ex) {
                                 ASSERT_STRING_CONTAINS(ex.what(), storageLocation);
                                 ASSERT_STRING_CONTAINS(ex.what(), storageIdentifier);
                             });
}

TYPED_TEST(FileBasedMakeFromExistingRangesTest, EmptyStorage) {
    unittest::TempDir storageLocation = makeTempDir();
    auto storageIdentifier = this->storage().makeEmptyStorage(storageLocation.path());
    SorterTracker sorterTracker;
    auto opts = SortOptions().TempDir(storageLocation.path()).Tracker(&sorterTracker);
    // Throws unexpected empty storage.
    ASSERT_THROWS_CODE(IWSorter::makeFromExistingRanges(
                           storageIdentifier,
                           MakeFromExistingRangesFixture::makeSampleRanges(),
                           opts,
                           IWComparator(ASC),
                           this->storage().makeSpillerForResume(opts, storageIdentifier)),
                       DBException,
                       TestFixture::kEmptyStorageErrorCode);
}

TYPED_TEST(FileBasedMakeFromExistingRangesTest, CorruptedStorage) {
    unittest::TempDir storageLocation = makeTempDir();
    SorterTracker sorterTracker;
    auto opts = SortOptions().TempDir(storageLocation.path()).Tracker(&sorterTracker);
    auto storageIdentifier = this->storage().makeCorruptedStorage(storageLocation.path());
    auto sorter = IWSorter::makeFromExistingRanges(
        storageIdentifier,
        MakeFromExistingRangesFixture::makeSampleRanges(),
        opts,
        IWComparator(ASC),
        this->storage().makeSpillerForResume(opts, storageIdentifier));

    // The number of spills is set when NoLimitSorter is constructed from existing ranges.
    ASSERT_EQ(MakeFromExistingRangesFixture::makeSampleRanges().size(),
              sorter->stats().spilledRanges());
    ASSERT_EQ(0, sorter->stats().numSorted());

    // Error reading storage.
    ASSERT_THROWS_CODE(sorter->done(), DBException, TestFixture::kCorruptedStorageErrorCode);
}

TYPED_TEST(FileBasedMakeFromExistingRangesTest, RoundTrip) {
    unittest::TempDir storageLocation = makeTempDir();
    SorterTracker sorterTracker;

    auto opts =
        SortOptions()
            .TempDir(storageLocation.path())
            .Tracker(&sorterTracker)
            .MaxMemoryUsageBytes(sizeof(IWSorter::Data) + this->storage().iteratorSizeBytes());

    IWPair pairInsertedBeforeShutdown(1, 100);

    // This test uses two sorters. The first sorter is used to persist data to disk in a shutdown
    // scenario. On startup, we will restore the original state of the sorter using the persisted
    // data.
    IWSorter::PersistedState state;
    {
        auto sorterBeforeShutdown =
            IWSorter::make(opts, IWComparator(ASC), this->storage().makeSpiller(opts));
        sorterBeforeShutdown->add(pairInsertedBeforeShutdown.first,
                                  pairInsertedBeforeShutdown.second);
        state = sorterBeforeShutdown->persistDataForShutdown();
        ASSERT_FALSE(state.storageIdentifier.empty());
        ASSERT_EQUALS(1U, state.ranges.size()) << state.ranges.size();
        ASSERT_EQ(1, sorterBeforeShutdown->stats().numSorted());
    }

    // On restart, reconstruct sorter from persisted state.
    auto sorter = IWSorter::makeFromExistingRanges(
        state.storageIdentifier,
        state.ranges,
        opts,
        IWComparator(ASC),
        this->storage().makeSpillerForResume(opts, state.storageIdentifier));

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

TYPED_TEST(MakeFromExistingRangesTest, NextWithDeferredValues) {
    unittest::TempDir storageLocation = makeTempDir();
    auto opts = SortOptions().TempDir(storageLocation.path()).Tracker(nullptr);

    IWPair pair1(1, 100);
    IWPair pair2(2, 200);

    std::unique_ptr<SortedStorageWriter<IntWrapper, IntWrapper>> writer =
        this->storage().makeWriter(opts);
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

TYPED_TEST(FileBasedMakeFromExistingRangesTest, ChecksumVersion) {
    unittest::TempDir storageLocation = makeTempDir();
    auto opts = SortOptions().TempDir(storageLocation.path()).Tracker(nullptr);

    // By default checksum version should be v2
    {
        auto sorter = IWSorter::make(opts, IWComparator(ASC), this->storage().makeSpiller(opts));
        sorter->add(1, -1);
        auto state = sorter->persistDataForShutdown();
        ASSERT_EQUALS(state.ranges[0].getChecksumVersion(), SorterChecksumVersion::v2);
        ASSERT_EQUALS(state.ranges[0].getChecksum(), 1921809301);
    }

    // Setting checksum version to v1 results in using v1 but getChecksumVersion() returning none
    // because v1 did not persist a version.
    {
        opts.ChecksumVersion(SorterChecksumVersion::v1);
        auto sorter = IWSorter::make(opts, IWComparator(ASC), this->storage().makeSpiller(opts));
        sorter->add(1, -1);
        auto state = sorter->persistDataForShutdown();
        ASSERT_EQUALS(state.ranges[0].getChecksumVersion(), boost::none);
        ASSERT_EQUALS(state.ranges[0].getChecksum(), 4121002018);
    }
}

TYPED_TEST(FileBasedMakeFromExistingRangesTest, ValidChecksumValidation) {
    unittest::TempDir storageLocation = makeTempDir();
    auto state = this->storage().makeSpillState(storageLocation.path());
    auto it = IWSorter::makeFromExistingRanges(
                  state.storageIdentifier,
                  state.ranges,
                  state.opts,
                  state.comp,
                  this->storage().makeSpillerForResume(state.opts, state.storageIdentifier))
                  ->done();
    ASSERT_ITERATORS_EQUIVALENT(it, std::make_unique<IntIterator>(0, 10));
}

TYPED_TEST(FileBasedMakeFromExistingRangesTest, IncompleteReadDoesNotReportChecksumError) {
    unittest::TempDir storageLocation = makeTempDir();
    auto state = this->storage().makeSpillState(storageLocation.path());
    this->storage().corruptSpillState(state);
    auto it = IWSorter::makeFromExistingRanges(
                  state.storageIdentifier,
                  state.ranges,
                  state.opts,
                  state.comp,
                  this->storage().makeSpillerForResume(state.opts, state.storageIdentifier))
                  ->done();
    // Read the first (and only) block of data, but don't deserialize any of it
    ASSERT(it->more());
    // it's destructor doesn't check the checksum since we didn't use everything
}

DEATH_TEST_F(MakeFromExistingRangesDeathTest,
             CompleteReadReportsChecksumError,
             "Data read from disk does not match what was written to disk.") {
    unittest::TempDir storageLocation = makeTempDir();
    auto state = FileTraits::makeSpillState(storageLocation.path());
    FileTraits::corruptSpillState(state);
    auto it = IWSorter::makeFromExistingRanges(
                  state.storageIdentifier,
                  state.ranges,
                  state.opts,
                  state.comp,
                  FileTraits::makeSpillerForResume(state.opts, state.storageIdentifier))
                  ->done();
    ASSERT_ITERATORS_EQUIVALENT(it, std::make_unique<IntIterator>(0, 10));
    // it's destructor ends up checking the checksum and aborts due to it being wrong
}

DEATH_TEST_F(MakeFromExistingRangesDeathTest,
             CompleteReadReportsChecksumErrorFromIncorrectChecksumVersion,
             "Data read from disk does not match what was written to disk.") {
    unittest::TempDir storageLocation = makeTempDir();
    auto state = FileTraits::makeSpillState(storageLocation.path());
    state.ranges[0].setChecksumVersion(boost::none);
    auto it = IWSorter::makeFromExistingRanges(
                  state.storageIdentifier,
                  state.ranges,
                  state.opts,
                  state.comp,
                  makeSorterSpiller(state.opts, /*fileStats=*/nullptr, state.storageIdentifier))
                  ->done();
    ASSERT_ITERATORS_EQUIVALENT(it, std::make_unique<IntIterator>(0, 10));
    // it's destructor ends up checking the checksum and aborts due to it being wrong (because we
    // used the wrong checksum algorithm)
}

// TODO SERVER-117316: Create a typed bounded sorter suite.
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
    using SAsc = BoundedSorter<Key, Doc, BoundMakerAsc>;
    using SAscNoBound = BoundedSorter<Key, Doc, NoBoundAsc>;
    using SDesc = BoundedSorter<Key, Doc, BoundMakerDesc>;

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

    std::unique_ptr<S> makeAsc(SortOptions options,
                               std::shared_ptr<FileBasedSorterSpiller<Key, Doc>> spiller = nullptr,
                               bool checkInput = true) {
        return std::make_unique<SAsc>(
            options, ComparatorAsc{}, BoundMakerAsc{}, spiller, checkInput);
    }
    std::unique_ptr<S> makeAscNoBound(
        SortOptions options,
        std::shared_ptr<FileBasedSorterSpiller<Key, Doc>> spiller = nullptr,
        bool checkInput = true) {
        return std::make_unique<SAscNoBound>(
            options, ComparatorAsc{}, NoBoundAsc{}, spiller, checkInput);
    }
    std::unique_ptr<S> makeDesc(SortOptions options,
                                std::shared_ptr<FileBasedSorterSpiller<Key, Doc>> spiller = nullptr,
                                bool checkInput = true) {
        return std::make_unique<SDesc>(
            options, ComparatorDesc{}, BoundMakerDesc{}, spiller, checkInput);
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
    sorter = makeAsc({}, /*spiller=*/nullptr, /*checkInput*/ false);
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
    std::shared_ptr<FileBasedSorterSpiller<Key, Doc>> spiller =
        std::make_shared<FileBasedSorterSpiller<Key, Doc>>(tempDir.path(), nullptr);
    sorter = makeAsc(options, std::move(spiller));

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
    std::shared_ptr<FileBasedSorterSpiller<Key, Doc>> spiller =
        std::make_shared<FileBasedSorterSpiller<Key, Doc>>(tempDir.path(), nullptr);
    sorter = makeAsc(options, std::move(spiller));

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
    std::shared_ptr<FileBasedSorterSpiller<Key, Doc>> spiller =
        std::make_shared<FileBasedSorterSpiller<Key, Doc>>(tempDir.path(), nullptr);
    sorter = makeAsc(options, std::move(spiller));

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

    std::shared_ptr<FileBasedSorterSpiller<Key, Doc>> spiller1 =
        std::make_shared<FileBasedSorterSpiller<Key, Doc>>(tempDir.path(), nullptr);
    // Disable input order checking so we can see what happens.
    sorter = makeAsc(options, std::move(spiller1), /*checkInput=*/false);
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


    std::shared_ptr<FileBasedSorterSpiller<Key, Doc>> spiller2 =
        std::make_shared<FileBasedSorterSpiller<Key, Doc>>(tempDir.path(), nullptr);
    // Test that by default, bad input like this would be detected.
    sorter = makeAsc(options, std::move(spiller2));
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
    std::shared_ptr<FileBasedSorterSpiller<Key, Doc>> spiller =
        std::make_shared<FileBasedSorterSpiller<Key, Doc>>(tempDir.path(), nullptr);
    sorter = makeAsc(options, std::move(spiller));

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
                       .Tracker(&sorterTracker);

    std::shared_ptr<FileBasedSorterSpiller<Key, Doc>> spiller =
        std::make_shared<FileBasedSorterSpiller<Key, Doc>>(tempDir.path(), &fileStats);
    sorter = makeAsc(options, std::move(spiller));
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
    sorter = makeDesc({}, /*spiller=*/nullptr, /*checkInput=*/false);
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
    std::shared_ptr<FileBasedSorterSpiller<Key, Doc>> spiller =
        std::make_shared<FileBasedSorterSpiller<Key, Doc>>(tempDir.path(), nullptr);
    sorter = makeAsc(options, std::move(spiller));

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
    static constexpr uint64_t kMemoryLimit = 4 * sorter::kSortedFileBufferSize;
    static const int kPerEntryMemUsage = kKey.memUsageForSorter() + Doc{kKey}.memUsageForSorter();
    static size_t kDocCountToCauseSpilling = (kMemoryLimit / kPerEntryMemUsage) + 1;

    unittest::TempDir tempDir = makeTempDir();
    auto options = SortOptions().TempDir(tempDir.path()).MaxMemoryUsageBytes(kMemoryLimit);
    std::shared_ptr<FileBasedSorterSpiller<Key, Doc>> spiller =
        std::make_shared<FileBasedSorterSpiller<Key, Doc>>(tempDir.path(), nullptr);
    sorter = makeAscNoBound(options, std::move(spiller));

    std::vector<Doc> input;
    input.reserve(kDocCountToCauseSpilling);
    for (size_t i = 0; i < kDocCountToCauseSpilling; ++i) {
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
                                      ::mongo::sorter::BoundedSorterTest::BoundMakerAsc>;
template class ::mongo::BoundedSorter<::mongo::sorter::BoundedSorterTest::Key,
                                      ::mongo::sorter::BoundedSorterTest::Doc,
                                      ::mongo::sorter::BoundedSorterTest::BoundMakerDesc>;
