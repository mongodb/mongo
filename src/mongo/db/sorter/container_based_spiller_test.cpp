// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/sorter/container_based_spiller.h"

#include "mongo/db/namespace_string.h"
#include "mongo/db/repl/replication_coordinator_mock.h"
#include "mongo/db/service_context_d_test_fixture.h"
#include "mongo/db/shard_role/transaction_resources.h"
#include "mongo/db/sorter/container_test_utils.h"
#include "mongo/db/sorter/sorter_checksum_calculator.h"
#include "mongo/db/sorter/sorter_test_utils.h"
#include "mongo/db/storage/container.h"
#include "mongo/db/storage/ident.h"
#include "mongo/db/storage/key_format.h"
#include "mongo/db/storage/record_store.h"
#include "mongo/db/storage/record_store_write_conflict_fail_points.h"
#include "mongo/db/storage/recovery_unit_noop.h"
#include "mongo/db/storage/storage_engine.h"
#include "mongo/db/storage/storage_options.h"
#include "mongo/db/storage/write_unit_of_work.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/fail_point.h"

#include <limits>
#include <memory>
#include <numeric>
#include <ostream>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <boost/filesystem/path.hpp>

namespace mongo::sorter {
namespace {
using namespace std::literals::string_view_literals;

using Storage = ContainerBasedStorage<IntWrapper, IntWrapper>;

class SpillerCallbackMock : public SpillCallbacks {
public:
    MOCK_METHOD(void, preSpill, (), (override));
    MOCK_METHOD(void, onSpill, (), (override));
    MOCK_METHOD(void, onSpillBatch, (), (override));
    MOCK_METHOD(void, postSpill, (), (override));
    MOCK_METHOD(void, onChunkWritten, (int64_t bytesWritten), (override));
};

TEST(ContainerIteratorTest, Iterate) {
    RecoveryUnitNoop ru;
    ViewableIntegerKeyedContainer container;

    int64_t containerKey1 = 1;
    int64_t containerKey2 = 2;
    int64_t containerKey3 = 3;
    int64_t containerKey4 = 4;
    int64_t containerKey5 = 5;

    IntWrapper key1{101};
    IntWrapper key2{102};
    IntWrapper key3{103};
    IntWrapper key4{104};
    IntWrapper key5{105};
    IntWrapper value1{100};
    IntWrapper value2{90};
    IntWrapper value3{80};
    IntWrapper value4{70};
    IntWrapper value5{60};

    BufBuilder containerValue1;
    key1.serializeForSorter(containerValue1);
    value1.serializeForSorter(containerValue1);

    BufBuilder containerValue2;
    key2.serializeForSorter(containerValue2);
    value2.serializeForSorter(containerValue2);

    BufBuilder containerValue3;
    key3.serializeForSorter(containerValue3);
    value3.serializeForSorter(containerValue3);

    BufBuilder containerValue4;
    key4.serializeForSorter(containerValue4);
    value4.serializeForSorter(containerValue4);

    BufBuilder containerValue5;
    key5.serializeForSorter(containerValue5);
    value5.serializeForSorter(containerValue5);

    ASSERT_OK(container.insert(ru,
                               containerKey1,
                               {containerValue1.buf(), static_cast<size_t>(containerValue1.len())},
                               container::ExistingKeyPolicy::reject));
    ASSERT_OK(container.insert(ru,
                               containerKey2,
                               {containerValue2.buf(), static_cast<size_t>(containerValue2.len())},
                               container::ExistingKeyPolicy::reject));
    ASSERT_OK(container.insert(ru,
                               containerKey3,
                               {containerValue3.buf(), static_cast<size_t>(containerValue3.len())},
                               container::ExistingKeyPolicy::reject));
    ASSERT_OK(container.insert(ru,
                               containerKey4,
                               {containerValue4.buf(), static_cast<size_t>(containerValue4.len())},
                               container::ExistingKeyPolicy::reject));
    ASSERT_OK(container.insert(ru,
                               containerKey5,
                               {containerValue5.buf(), static_cast<size_t>(containerValue5.len())},
                               container::ExistingKeyPolicy::reject));

    ContainerIterator<IntWrapper, IntWrapper> iterator{container.getCursor(ru),
                                                       containerKey1,
                                                       containerKey5 + 1,
                                                       Iterator<IntWrapper, IntWrapper>::Settings{},
                                                       /*_checksumCalculator=*/3272515249,
                                                       sorter::kLatestChecksumVersion};

    // Before iteration starts, getRange() reports no current position.
    EXPECT_FALSE(iterator.getRange().getCurrent().has_value());
    EXPECT_FALSE(iterator.getRange().getCurrentChecksum().has_value());

    size_t prevChecksum = 0;
    boost::optional<int64_t> currentChecksum;

    auto checkChecksum = [&](const ContainerIterator<IntWrapper, IntWrapper>& it) {
        currentChecksum = it.getRange().getCurrentChecksum();
        ASSERT(currentChecksum.has_value());
        EXPECT_NE(*currentChecksum, prevChecksum);
        prevChecksum = *currentChecksum;
    };

    ASSERT_TRUE(iterator.more());
    auto next = iterator.next();
    EXPECT_EQ(next.first, key1);
    EXPECT_EQ(next.second, value1);
    ASSERT_TRUE(iterator.getRange().getCurrent().has_value());
    EXPECT_EQ(*iterator.getRange().getCurrent(), containerKey2);
    checkChecksum(iterator);

    ASSERT_TRUE(iterator.more());
    next = iterator.next();
    EXPECT_EQ(next.first, key2);
    EXPECT_EQ(next.second, value2);
    ASSERT_TRUE(iterator.getRange().getCurrent().has_value());
    EXPECT_EQ(*iterator.getRange().getCurrent(), containerKey3);
    checkChecksum(iterator);

    ASSERT_TRUE(iterator.more());
    EXPECT_EQ(iterator.nextWithDeferredValue(), key3);
    EXPECT_EQ(iterator.getDeferredValue(), value3);
    ASSERT_TRUE(iterator.getRange().getCurrent().has_value());
    EXPECT_EQ(*iterator.getRange().getCurrent(), containerKey4);
    checkChecksum(iterator);

    ASSERT_TRUE(iterator.more());
    EXPECT_EQ(iterator.nextWithDeferredValue(), key4);
    EXPECT_EQ(iterator.getDeferredValue(), value4);
    ASSERT_TRUE(iterator.getRange().getCurrent().has_value());
    EXPECT_EQ(*iterator.getRange().getCurrent(), containerKey5);
    checkChecksum(iterator);

    ASSERT_TRUE(iterator.more());
    next = iterator.next();
    EXPECT_EQ(next.first, key5);
    EXPECT_EQ(next.second, value5);
    ASSERT_TRUE(iterator.getRange().getCurrent().has_value());
    EXPECT_EQ(*iterator.getRange().getCurrent(), iterator.getRange().getEnd());
    checkChecksum(iterator);

    EXPECT_FALSE(iterator.more());

    // After exhaustion, getRange() reports the end of the range.
    ASSERT_TRUE(iterator.getRange().getCurrent().has_value());
    EXPECT_EQ(*iterator.getRange().getCurrent(), iterator.getRange().getEnd());
    EXPECT_EQ(*iterator.getRange().getCurrentChecksum(), prevChecksum);

    // Construct an iterator starting at key 4.
    ContainerIterator<IntWrapper, IntWrapper> positionedIterator{
        container.getCursor(ru),
        containerKey1,
        containerKey4,
        containerKey5 + 1,
        Iterator<IntWrapper, IntWrapper>::Settings{},
        /*checksum=*/3272515249,
        /*currentChecksum=*/1727402339,
        sorter::kLatestChecksumVersion};

    prevChecksum = 1727402339;
    currentChecksum = boost::none;

    ASSERT_TRUE(positionedIterator.more());
    EXPECT_EQ(positionedIterator.nextWithDeferredValue(), key4);
    EXPECT_EQ(positionedIterator.getDeferredValue(), value4);
    ASSERT_TRUE(positionedIterator.getRange().getCurrent().has_value());
    EXPECT_EQ(*positionedIterator.getRange().getCurrent(), containerKey5);
    checkChecksum(positionedIterator);

    ASSERT_TRUE(positionedIterator.more());
    EXPECT_EQ(positionedIterator.nextWithDeferredValue(), key5);
    EXPECT_EQ(positionedIterator.getDeferredValue(), value5);
    ASSERT_TRUE(positionedIterator.getRange().getCurrent().has_value());
    EXPECT_EQ(*positionedIterator.getRange().getCurrent(), positionedIterator.getRange().getEnd());
    checkChecksum(positionedIterator);
}

TEST(ContainerIteratorTest, MultipleCursors) {
    RecoveryUnitNoop ru;
    ViewableIntegerKeyedContainer container;

    int64_t containerKey1 = 1;
    int64_t containerKey2 = 2;
    int64_t containerKey3 = 3;
    int64_t containerKey4 = 4;

    IntWrapper key1{100};
    IntWrapper key2{200};
    IntWrapper key3{150};
    IntWrapper key4{250};
    IntWrapper value1{100};
    IntWrapper value2{90};
    IntWrapper value3{80};
    IntWrapper value4{70};

    BufBuilder containerValue1;
    key1.serializeForSorter(containerValue1);
    value1.serializeForSorter(containerValue1);

    BufBuilder containerValue2;
    key2.serializeForSorter(containerValue2);
    value2.serializeForSorter(containerValue2);

    BufBuilder containerValue3;
    key3.serializeForSorter(containerValue3);
    value3.serializeForSorter(containerValue3);

    BufBuilder containerValue4;
    key4.serializeForSorter(containerValue4);
    value4.serializeForSorter(containerValue4);

    ASSERT_OK(container.insert(ru,
                               containerKey1,
                               {containerValue1.buf(), static_cast<size_t>(containerValue1.len())},
                               container::ExistingKeyPolicy::reject));
    ASSERT_OK(container.insert(ru,
                               containerKey2,
                               {containerValue2.buf(), static_cast<size_t>(containerValue2.len())},
                               container::ExistingKeyPolicy::reject));
    ASSERT_OK(container.insert(ru,
                               containerKey3,
                               {containerValue3.buf(), static_cast<size_t>(containerValue3.len())},
                               container::ExistingKeyPolicy::reject));
    ASSERT_OK(container.insert(ru,
                               containerKey4,
                               {containerValue4.buf(), static_cast<size_t>(containerValue4.len())},
                               container::ExistingKeyPolicy::reject));

    ContainerIterator<IntWrapper, IntWrapper> iterator1{
        container.getCursor(ru),
        containerKey1,
        containerKey2 + 1,
        Iterator<IntWrapper, IntWrapper>::Settings{},
        /*_checksumCalculator=*/71873048,
        sorter::kLatestChecksumVersion};
    ContainerIterator<IntWrapper, IntWrapper> iterator2{
        container.getCursor(ru),
        containerKey3,
        containerKey4 + 1,
        Iterator<IntWrapper, IntWrapper>::Settings{},
        /*_checksumCalculator=*/2815298670,
        sorter::kLatestChecksumVersion};

    ASSERT_TRUE(iterator1.more());
    ASSERT_TRUE(iterator2.more());
    EXPECT_EQ(iterator1.nextWithDeferredValue(), key1);
    EXPECT_EQ(iterator2.nextWithDeferredValue(), key3);
    EXPECT_EQ(iterator1.getDeferredValue(), value1);
    EXPECT_EQ(iterator2.getDeferredValue(), value3);

    ASSERT_TRUE(iterator1.more());
    ASSERT_TRUE(iterator2.more());
    EXPECT_EQ(iterator1.nextWithDeferredValue(), key2);
    auto next = iterator2.next();
    EXPECT_EQ(next.first, key4);
    EXPECT_EQ(next.second, value4);
    EXPECT_EQ(iterator1.getDeferredValue(), value2);

    EXPECT_FALSE(iterator1.more());
    EXPECT_FALSE(iterator2.more());
}

TEST(ContainerIteratorTest, ContainerMissingKey) {
    RecoveryUnitNoop ru;
    ViewableIntegerKeyedContainer container;

    int64_t containerKey1 = 1;

    IntWrapper key1{101};
    IntWrapper value1{100};

    BufBuilder containerValue1;
    key1.serializeForSorter(containerValue1);
    value1.serializeForSorter(containerValue1);

    ContainerIterator<IntWrapper, IntWrapper> iterator{container.getCursor(ru),
                                                       containerKey1,
                                                       containerKey1 + 1,
                                                       Iterator<IntWrapper, IntWrapper>::Settings{},
                                                       /*_checksumCalculator=*/4104690164,
                                                       sorter::kLatestChecksumVersion};

    ASSERT_TRUE(iterator.more());
    EXPECT_THROW(iterator.next(), DBException);
    EXPECT_THROW(iterator.nextWithDeferredValue(), DBException);

    ASSERT_OK(container.insert(ru,
                               containerKey1,
                               {containerValue1.buf(), static_cast<size_t>(containerValue1.len())},
                               container::ExistingKeyPolicy::reject));
    EXPECT_EQ(iterator.nextWithDeferredValue(), key1);

    ASSERT_OK(container.remove(ru, containerKey1));
    EXPECT_EQ(iterator.getDeferredValue(), value1);
}

TEST(ContainerIteratorTest, InvalidDeferredValueUsage) {
    RecoveryUnitNoop ru;
    ViewableIntegerKeyedContainer container;

    int64_t containerKey1 = 1;

    IntWrapper key1{101};
    IntWrapper value1{100};

    BufBuilder containerValue1;
    key1.serializeForSorter(containerValue1);
    value1.serializeForSorter(containerValue1);

    ASSERT_OK(container.insert(ru,
                               containerKey1,
                               {containerValue1.buf(), static_cast<size_t>(containerValue1.len())},
                               container::ExistingKeyPolicy::reject));

    ContainerIterator<IntWrapper, IntWrapper> iterator{container.getCursor(ru),
                                                       containerKey1,
                                                       containerKey1 + 1,
                                                       Iterator<IntWrapper, IntWrapper>::Settings{},
                                                       /*_checksumCalculator=*/4104690164,
                                                       sorter::kLatestChecksumVersion};

    ASSERT_TRUE(iterator.more());
    EXPECT_THROW(iterator.getDeferredValue(), DBException);
    EXPECT_EQ(iterator.nextWithDeferredValue(), key1);
    EXPECT_THROW(iterator.nextWithDeferredValue(), DBException);
    EXPECT_EQ(iterator.getDeferredValue(), value1);
    EXPECT_THROW(iterator.getDeferredValue(), DBException);
}

DEATH_TEST(ContainerIteratorChecksumDeathTest, IncorrectChecksumV1Fails, "11605900") {
    RecoveryUnitNoop ru;
    ViewableIntegerKeyedContainer container;

    int64_t containerKey1 = 1;
    IntWrapper key1{100};
    IntWrapper value1{100};

    BufBuilder containerValue1;
    key1.serializeForSorter(containerValue1);
    value1.serializeForSorter(containerValue1);

    ASSERT_OK(container.insert(ru,
                               containerKey1,
                               {containerValue1.buf(), static_cast<size_t>(containerValue1.len())},
                               container::ExistingKeyPolicy::reject));

    ContainerIterator<IntWrapper, IntWrapper> iterator{container.getCursor(ru),
                                                       containerKey1,
                                                       containerKey1 + 1,
                                                       Iterator<IntWrapper, IntWrapper>::Settings{},
                                                       /*_checksumCalculator=*/0,
                                                       SorterChecksumVersion::v1};

    ASSERT_TRUE(iterator.more());
    iterator.next();
}


DEATH_TEST(ContainerIteratorChecksumDeathTest, IncorrectChecksumV2Fails, "11605900") {
    RecoveryUnitNoop ru;
    ViewableIntegerKeyedContainer container;

    int64_t containerKey1 = 1;
    IntWrapper key1{100};
    IntWrapper value1{100};

    BufBuilder containerValue1;
    key1.serializeForSorter(containerValue1);
    value1.serializeForSorter(containerValue1);

    ASSERT_OK(container.insert(ru,
                               containerKey1,
                               {containerValue1.buf(), static_cast<size_t>(containerValue1.len())},
                               container::ExistingKeyPolicy::reject));

    ContainerIterator<IntWrapper, IntWrapper> iterator{container.getCursor(ru),
                                                       containerKey1,
                                                       containerKey1 + 1,
                                                       Iterator<IntWrapper, IntWrapper>::Settings{},
                                                       /*_checksumCalculator=*/0,
                                                       sorter::kLatestChecksumVersion};

    ASSERT_TRUE(iterator.more());
    iterator.next();
}

class ContainerIteratorTest : public testing::TestWithParam<SorterChecksumVersion> {
public:
    // TODO (SERVER-116165): Remove.
    unittest::ServerParameterGuard ffContainerWrites{"featureFlagContainerWrites", true};
};

INSTANTIATE_TEST_SUITE_P(ContainerIteratorTestSuite,
                         ContainerIteratorTest,
                         testing::Values(SorterChecksumVersion::v1,
                                         sorter::kLatestChecksumVersion));

TEST_P(ContainerIteratorTest, EmptyIteratorHasZeroChecksum) {
    RecoveryUnitNoop ru;
    ViewableIntegerKeyedContainer container;

    ContainerIterator<IntWrapper, IntWrapper> iterator{container.getCursor(ru),
                                                       /*start=*/0,
                                                       /*end=*/1,
                                                       Iterator<IntWrapper, IntWrapper>::Settings{},
                                                       /*_checksumCalculator=*/0,
                                                       ContainerIteratorTest::GetParam()};
}

class SortedContainerWriterTest : public ServiceContextMongoDTest {
public:
    SorterTracker sorterTracker;
    // TODO (SERVER-116165): Remove.
    unittest::ServerParameterGuard ffContainerWrites{"featureFlagContainerWrites", true};

    /**
     * Creates and exhausts iterators created from the writer to ensure that the final checksum
     * matches between the writer and the iterator.
     */
    template <typename Key, typename Value>
    void exhaustIterators(SortedContainerWriter<Key, Value>& writer) {
        // Exhausting the cursor with next() calls.
        std::shared_ptr<Iterator<Key, Value>> iterator1 = writer.done();
        while (iterator1->more()) {
            iterator1->next();
        }

        // Exhausting the cursor with nextWithDeferredValue()/getDeferredValue() calls.
        std::shared_ptr<Iterator<Key, Value>> iterator2 = writer.done();
        while (iterator2->more()) {
            iterator2->nextWithDeferredValue();
            iterator2->getDeferredValue();
        }

        // Exhausting the cursor with alternating calls with next() and
        // nextWithDeferredValue()/getDeferredValue() calls.
        std::shared_ptr<Iterator<Key, Value>> iterator3 = writer.done();
        for (auto i = 0; iterator3->more(); ++i) {
            if (i % 2 == 0) {
                iterator3->next();
            } else {
                iterator3->nextWithDeferredValue();
                iterator3->getDeferredValue();
            }
        }
    }
};

TEST_F(SortedContainerWriterTest, ContainerWriterUsesNextKeyForContainerEntries) {
    auto opCtx = makeOperationContext();
    auto* replCoord = repl::ReplicationCoordinator::get(opCtx.get());
    auto* replCoordMock = dynamic_cast<repl::ReplicationCoordinatorMock*>(replCoord);
    ASSERT(replCoordMock);
    replCoordMock->alwaysAllowWrites(true);

    ViewableIntegerKeyedContainer container;
    container.setIdent(std::make_shared<Ident>("sorted_container_writer_test"));
    SortOptions opts;
    const int64_t startingKey = 5;
    auto& ru = *shard_role_details::getRecoveryUnit(opCtx.get());
    SorterContainerStats stats(&this->sorterTracker);
    SortedContainerWriter<IntWrapper, IntWrapper> writer(
        *opCtx,
        ru,
        container,
        stats,
        opts,
        startingKey,
        sorter::kLatestChecksumVersion,
        SortedContainerWriter<IntWrapper, IntWrapper>::Settings{});

    const IntWrapper k1{1};
    const IntWrapper v1{2};
    const IntWrapper k2{3};
    const IntWrapper v2{4};
    writer.addAlreadySorted(k1, v1);
    writer.addAlreadySorted(k2, v2);

    ASSERT_EQ(container.entries().size(), 2U);
    EXPECT_EQ(container.entries()[0].first, startingKey);
    EXPECT_EQ(container.entries()[1].first, startingKey + 1);

    BufBuilder expected;
    expected.reset();
    k1.serializeForSorter(expected);
    v1.serializeForSorter(expected);
    EXPECT_EQ(container.entries()[0].second, std::string(expected.buf(), expected.len()));

    expected.reset();
    k2.serializeForSorter(expected);
    v2.serializeForSorter(expected);
    EXPECT_EQ(container.entries()[1].second, std::string(expected.buf(), expected.len()));

    exhaustIterators<IntWrapper, IntWrapper>(writer);
}

TEST_F(SortedContainerWriterTest, GetBufferSizeReflectsAverageEntrySize) {
    auto opCtx = makeOperationContext();
    auto* replCoord = repl::ReplicationCoordinator::get(opCtx.get());
    auto* replCoordMock = dynamic_cast<repl::ReplicationCoordinatorMock*>(replCoord);
    ASSERT(replCoordMock);
    replCoordMock->alwaysAllowWrites(true);

    ViewableIntegerKeyedContainer container;
    container.setIdent(std::make_shared<Ident>("buffer_size_test"));
    auto& ru = *shard_role_details::getRecoveryUnit(opCtx.get());
    SorterContainerStats stats(&this->sorterTracker);

    ContainerBasedStorage<IntWrapper, IntWrapper> storage(*opCtx,
                                                          ru,
                                                          container,
                                                          stats,
                                                          /*currKey=*/1,
                                                          /*dbName=*/boost::none,
                                                          sorter::kLatestChecksumVersion);

    // Before any spills, buffer size is 0. In production getBufferSize() is only consulted after
    // the first spill.
    EXPECT_EQ(storage.getBufferSize(), 0);

    // Write two IntWrapper+IntWrapper entries (4 + 4 = 8 bytes each serialized).
    SortOptions opts;
    auto writer =
        storage.makeWriter(opts, ContainerBasedStorage<IntWrapper, IntWrapper>::Settings{});
    writer->addAlreadySorted(IntWrapper{1}, IntWrapper{2});
    writer->addAlreadySorted(IntWrapper{3}, IntWrapper{4});

    // Average entry size = 16 / 2 = 8, plus per-cursor overhead.
    EXPECT_EQ(storage.getBufferSize(), 8 + Storage::kPerCursorOverheadBytes);

    // Spilled bytes should have propagated to the tracker. The container-based sorter does not
    // compress in the sorter layer, so `bytesSpilled` and `bytesSpilledUncompressed` match.
    EXPECT_EQ(stats.bytesSpilledUncompressed(), 16);
    EXPECT_EQ(stats.bytesSpilled(), 16);
    EXPECT_EQ(stats.numSpilledEntries(), 2);
    EXPECT_EQ(this->sorterTracker.bytesSpilledUncompressed.load(), 16);
    EXPECT_EQ(this->sorterTracker.bytesSpilled.load(), 16);
}

/**
 * Runs over element types of differing serialized width, so a reported write size has to track the
 * entries actually written rather than any fixed value.
 */
template <typename Element>
class SortedContainerWriterElementTypedTest : public SortedContainerWriterTest {};

using ContainerElementTypes = testing::Types<ContainerElementWrapper<char>,
                                             ContainerElementWrapper<int16_t>,
                                             ContainerElementWrapper<int32_t>,
                                             ContainerElementWrapper<int64_t>>;
TYPED_TEST_SUITE(SortedContainerWriterElementTypedTest, ContainerElementTypes);

TYPED_TEST(SortedContainerWriterElementTypedTest,
           AddAlreadySortedWrapperReportsBytesInsideNestedWriteUnitOfWork) {
    using Element = TypeParam;

    auto opCtx = this->makeOperationContext();
    auto* replCoordMock = dynamic_cast<repl::ReplicationCoordinatorMock*>(
        repl::ReplicationCoordinator::get(opCtx.get()));
    ASSERT(replCoordMock);
    replCoordMock->alwaysAllowWrites(true);

    ViewableIntegerKeyedContainer container;
    container.setIdent(
        std::make_shared<Ident>(str::stream() << "bytes_written_test_" << sizeof(Element)));
    SortOptions opts;
    const int64_t startingKey = 11;
    auto& ru = *shard_role_details::getRecoveryUnit(opCtx.get());
    SorterContainerStats stats(&this->sorterTracker);
    SortedContainerWriter<Element, Element> writer(
        *opCtx,
        ru,
        container,
        stats,
        opts,
        startingKey,
        sorter::kLatestChecksumVersion,
        typename SortedContainerWriter<Element, Element>::Settings{});

    const Element k1{1};
    const Element v1{2};
    const Element k2{3};
    const Element v2{4};

    BufBuilder expected;
    k1.serializeForSorter(expected);
    v1.serializeForSorter(expected);
    const int64_t entrySize = expected.len();
    ASSERT_EQ(entrySize, 2 * static_cast<int64_t>(sizeof(typename Element::ElementType)));

    // mergeSpills() calls addAlreadySortedWrapper() from inside an enclosing unit of work, so the
    // writer's own unit of work is nested and its onCommit handlers do not run until this outer one
    // commits. The reported size must be available as soon as the call returns.
    {
        WriteUnitOfWork wuow{opCtx.get()};
        EXPECT_EQ(writer.addAlreadySortedWrapper(k1, v1).bytesWritten, entrySize);
        EXPECT_EQ(writer.addAlreadySortedWrapper(k2, v2).bytesWritten, entrySize);
        wuow.commit();
    }

    EXPECT_EQ(stats.bytesSpilled(), 2 * entrySize);
    EXPECT_EQ(stats.numSpilledEntries(), 2);

    this->template exhaustIterators<Element, Element>(writer);
}

TEST_F(SortedContainerWriterTest, StatsUpdatedOnCommit) {
    auto opCtx = makeOperationContext();
    auto* replCoordMock = dynamic_cast<repl::ReplicationCoordinatorMock*>(
        repl::ReplicationCoordinator::get(opCtx.get()));
    ASSERT(replCoordMock);
    replCoordMock->alwaysAllowWrites(true);

    ViewableIntegerKeyedContainer container;
    container.setIdent(std::make_shared<Ident>("sorted_container_writer_stats_commit_test"));
    SorterContainerStats stats{&this->sorterTracker};
    SortOptions opts;
    SortedContainerWriter<IntWrapper, IntWrapper> writer{
        *opCtx,
        *shard_role_details::getRecoveryUnit(opCtx.get()),
        container,
        stats,
        opts,
        /*nextKey=*/1,
        sorter::kLatestChecksumVersion,
        SortedContainerWriter<IntWrapper, IntWrapper>::Settings{}};

    {
        WriteUnitOfWork wuow{opCtx.get()};
        writer.addAlreadySorted(IntWrapper{1}, IntWrapper{2});
        writer.addAlreadySorted(IntWrapper{3}, IntWrapper{4});

        // Writes are not yet committed: stats must still be zero.
        EXPECT_EQ(stats.bytesSpilled(), 0);
        EXPECT_EQ(stats.bytesSpilledUncompressed(), 0);
        EXPECT_EQ(stats.numSpilledEntries(), 0);

        wuow.commit();
    }

    // After commit, the stats reflect the committed writes.
    EXPECT_EQ(stats.bytesSpilled(), sizeof(IntWrapper) * 4);
    EXPECT_EQ(stats.bytesSpilledUncompressed(), sizeof(IntWrapper) * 4);
    EXPECT_EQ(stats.numSpilledEntries(), 2);
    EXPECT_EQ(sorterTracker.bytesSpilled.load(), stats.bytesSpilled());
    EXPECT_EQ(sorterTracker.bytesSpilledUncompressed.load(), stats.bytesSpilledUncompressed());
}

TEST_F(SortedContainerWriterTest, StatsNotUpdatedOnAbort) {
    auto opCtx = makeOperationContext();
    auto* replCoordMock = dynamic_cast<repl::ReplicationCoordinatorMock*>(
        repl::ReplicationCoordinator::get(opCtx.get()));
    ASSERT(replCoordMock);
    replCoordMock->alwaysAllowWrites(true);

    ViewableIntegerKeyedContainer container;
    container.setIdent(std::make_shared<Ident>("sorted_container_writer_stats_abort_test"));
    SorterContainerStats stats{&this->sorterTracker};
    SortOptions opts;
    SortedContainerWriter<IntWrapper, IntWrapper> writer{
        *opCtx,
        *shard_role_details::getRecoveryUnit(opCtx.get()),
        container,
        stats,
        opts,
        /*nextKey=*/1,
        sorter::kLatestChecksumVersion,
        SortedContainerWriter<IntWrapper, IntWrapper>::Settings{}};

    {
        WriteUnitOfWork wuow{opCtx.get()};
        writer.addAlreadySorted(IntWrapper{1}, IntWrapper{2});
        writer.addAlreadySorted(IntWrapper{3}, IntWrapper{4});

        // Writes are not yet committed: stats must still be zero.
        EXPECT_EQ(stats.bytesSpilled(), 0);
        EXPECT_EQ(stats.bytesSpilledUncompressed(), 0);
        EXPECT_EQ(stats.numSpilledEntries(), 0);
    }

    // After abort, the stats reflect none of the rolled-back writes.
    EXPECT_EQ(stats.bytesSpilled(), 0);
    EXPECT_EQ(stats.bytesSpilledUncompressed(), 0);
    EXPECT_EQ(stats.numSpilledEntries(), 0);
    EXPECT_EQ(sorterTracker.bytesSpilled.load(), 0);
    EXPECT_EQ(sorterTracker.bytesSpilledUncompressed.load(), 0);
}

TEST_F(SortedContainerWriterTest, GetBufferSizeEdgeCases) {
    auto opCtx = makeOperationContext();
    auto* replCoord = repl::ReplicationCoordinator::get(opCtx.get());
    auto* replCoordMock = dynamic_cast<repl::ReplicationCoordinatorMock*>(replCoord);
    ASSERT(replCoordMock);
    replCoordMock->alwaysAllowWrites(true);

    ViewableIntegerKeyedContainer container;
    container.setIdent(std::make_shared<Ident>("buffer_size_edge_test"));
    auto& ru = *shard_role_details::getRecoveryUnit(opCtx.get());

    // Zero-byte entries (e.g. NullValue + NullValue): overhead only.
    {
        SorterContainerStats stats(nullptr);
        Storage storage(*opCtx,
                        ru,
                        container,
                        stats,
                        /*currKey=*/1,
                        /*dbName=*/boost::none,
                        sorter::kLatestChecksumVersion);
        stats.addSpilledDataSizeUncompressed(0);
        stats.incrementNumSpilledEntries();
        EXPECT_EQ(storage.getBufferSize(), Storage::kPerCursorOverheadBytes);
    }

    // Non-divisible total: 10 bytes across 3 entries truncates to 3 (not 4), plus overhead. The
    // fixed per-cursor overhead provides the conservative bias, so truncation is safe.
    {
        SorterContainerStats stats(nullptr);
        Storage storage(*opCtx,
                        ru,
                        container,
                        stats,
                        /*currKey=*/1,
                        /*dbName=*/boost::none,
                        sorter::kLatestChecksumVersion);
        stats.addSpilledDataSizeUncompressed(4);
        stats.incrementNumSpilledEntries();
        stats.addSpilledDataSizeUncompressed(3);
        stats.incrementNumSpilledEntries();
        stats.addSpilledDataSizeUncompressed(3);
        stats.incrementNumSpilledEntries();
        EXPECT_EQ(storage.getBufferSize(), 3 + Storage::kPerCursorOverheadBytes);
    }
}

enum class ContainerWriterMode {
    Single,
    Batched,
};

std::ostream& operator<<(std::ostream& os, ContainerWriterMode mode) {
    switch (mode) {
        case ContainerWriterMode::Single:
            return os << "Single";
        case ContainerWriterMode::Batched:
            return os << "Batched";
    }
    MONGO_UNREACHABLE_TASSERT(11605901);
}

class SortedContainerWriterModeParameterizedTest
    : public SortedContainerWriterTest,
      public testing::WithParamInterface<ContainerWriterMode> {};

TEST_P(SortedContainerWriterModeParameterizedTest,
       ContainerWriterStoresEmptyValueForZeroLengthSerialization) {
    auto opCtx = makeOperationContext();
    auto* replCoord = repl::ReplicationCoordinator::get(opCtx.get());
    auto* replCoordMock = dynamic_cast<repl::ReplicationCoordinatorMock*>(replCoord);
    ASSERT(replCoordMock);
    replCoordMock->alwaysAllowWrites(true);

    ViewableIntegerKeyedContainer container;
    container.setIdent(std::make_shared<Ident>("sorted_container_writer_empty_test"));
    SortOptions opts;
    const int64_t startingKey = 29;
    auto& ru = *shard_role_details::getRecoveryUnit(opCtx.get());
    SorterContainerStats stats(&this->sorterTracker);
    SortedContainerWriter<NullValue, NullValue> writer(
        *opCtx,
        ru,
        container,
        stats,
        opts,
        startingKey,
        sorter::kLatestChecksumVersion,
        SortedContainerWriter<NullValue, NullValue>::Settings{});

    const std::vector<std::pair<NullValue, NullValue>> entries(3);
    switch (GetParam()) {
        case ContainerWriterMode::Single:
            for (auto& [k, v] : entries) {
                writer.addAlreadySorted(k, v);
            }
            break;
        case ContainerWriterMode::Batched: {
            const auto result =
                writer.addAlreadySortedBatch(entries, std::numeric_limits<int64_t>::max());
            EXPECT_EQ(result.kvPairsWritten, static_cast<int64_t>(entries.size()));
            EXPECT_EQ(result.bytesWritten, 0);
            break;
        }
    }

    ASSERT_EQ(container.entries().size(), entries.size());
    for (size_t i = 0; i < entries.size(); ++i) {
        EXPECT_EQ(container.entries()[i].first, startingKey + static_cast<int64_t>(i));
        EXPECT_TRUE(container.entries()[i].second.empty());
    }

    exhaustIterators<NullValue, NullValue>(writer);
}

TEST_P(SortedContainerWriterModeParameterizedTest, ContainerWriterAllowsNullValueWithNonNullKey) {
    auto opCtx = makeOperationContext();
    auto* replCoord = repl::ReplicationCoordinator::get(opCtx.get());
    auto* replCoordMock = dynamic_cast<repl::ReplicationCoordinatorMock*>(replCoord);
    ASSERT(replCoordMock);
    replCoordMock->alwaysAllowWrites(true);

    ViewableIntegerKeyedContainer container;
    container.setIdent(std::make_shared<Ident>("sorted_container_writer_null_value_test"));
    SortOptions opts;
    const int64_t startingKey = 2002;
    auto& ru = *shard_role_details::getRecoveryUnit(opCtx.get());
    SorterContainerStats stats(&this->sorterTracker);
    SortedContainerWriter<IntWrapper, NullValue> writer(
        *opCtx,
        ru,
        container,
        stats,
        opts,
        startingKey,
        sorter::kLatestChecksumVersion,
        SortedContainerWriter<IntWrapper, NullValue>::Settings{});

    const std::vector<std::pair<IntWrapper, NullValue>> entries{{IntWrapper{123}, NullValue{}},
                                                                {IntWrapper{124}, NullValue{}},
                                                                {IntWrapper{125}, NullValue{}}};
    switch (GetParam()) {
        case ContainerWriterMode::Single:
            for (auto& [k, v] : entries) {
                writer.addAlreadySorted(k, v);
            }
            break;
        case ContainerWriterMode::Batched: {
            auto result =
                writer.addAlreadySortedBatch(entries, std::numeric_limits<int64_t>::max());
            EXPECT_EQ(result.kvPairsWritten, static_cast<int64_t>(entries.size()));
            break;
        }
    }

    ASSERT_EQ(container.entries().size(), entries.size());
    for (size_t i = 0; i < entries.size(); ++i) {
        EXPECT_EQ(container.entries()[i].first, startingKey + static_cast<int64_t>(i));

        BufBuilder expected;
        entries[i].first.serializeForSorter(expected);
        entries[i].second.serializeForSorter(expected);
        EXPECT_EQ(container.entries()[i].second, std::string(expected.buf(), expected.len()));
    }

    exhaustIterators<IntWrapper, NullValue>(writer);
}

INSTANTIATE_TEST_SUITE_P(NullValueSorterWrites,
                         SortedContainerWriterModeParameterizedTest,
                         testing::Values(ContainerWriterMode::Single, ContainerWriterMode::Batched),
                         testing::PrintToStringParamName{});

class ContainerBasedSpillerTest : public ServiceContextMongoDTest,
                                  public testing::WithParamInterface<std::tuple<int64_t, int64_t>> {
public:
    // TODO (SERVER-116165): Remove.
    unittest::ServerParameterGuard ffContainerWrites{"featureFlagContainerWrites", true};

    int64_t batchSize() const {
        return std::get<0>(GetParam());
    }
    int64_t batchBytes() const {
        return std::get<1>(GetParam());
    }
};

INSTANTIATE_TEST_SUITE_P(
    ContainerBasedSpillerTest,
    ContainerBasedSpillerTest,
    testing::Combine(testing::Values(1, 2, 4),
                     testing::Values(1, sizeof(IntWrapper), std::numeric_limits<int64_t>::max())));

TEST_P(ContainerBasedSpillerTest, Spill) {
    auto opCtx = makeOperationContext();

    auto replCoord = dynamic_cast<repl::ReplicationCoordinatorMock*>(
        repl::ReplicationCoordinator::get(opCtx.get()));
    ASSERT(replCoord);
    replCoord->alwaysAllowWrites(true);

    const auto identStr = ident::generateNewInternalIdent("container_spill"sv);
    ViewableIntegerKeyedContainer container{std::make_shared<Ident>(identStr)};
    SorterContainerStats stats{nullptr};
    auto callbacks = std::make_unique<testing::NiceMock<SpillerCallbackMock>>();
    auto& callbacksRef = *callbacks;

    ContainerBasedSpiller<IntWrapper, NullValue, IWComparator> spiller{
        *opCtx,
        *shard_role_details::getRecoveryUnit(opCtx.get()),
        container,
        stats,
        boost::none,
        sorter::kLatestChecksumVersion,
        std::move(callbacks),
        batchSize(),
        batchBytes(),
        testSpillingMinAvailableDiskSpaceBytes};

    EXPECT_CALL(callbacksRef, preSpill).Times(2);
    EXPECT_CALL(callbacksRef, postSpill).Times(2);
    int spilled = 0;
    EXPECT_CALL(callbacksRef, onSpill).Times(2).WillRepeatedly([&]() {
        EXPECT_EQ(spiller.iterators().size(), ++spilled);
    });

    std::vector<std::pair<IntWrapper, NullValue>> data{{50, {}}, {100, {}}, {75, {}}, {125, {}}};
    std::span span{data};

    spiller.spill(SortOptions{},
                  Spiller<IntWrapper, NullValue, IWComparator>::Settings{},
                  span.subspan(0, 2));
    spiller.spill(SortOptions{},
                  Spiller<IntWrapper, NullValue, IWComparator>::Settings{},
                  span.subspan(2, 2));

    ASSERT_EQ(spiller.iterators().size(), 2);
    auto it1 = spiller.iterators()[0];
    auto it2 = spiller.iterators()[1];

    ASSERT_TRUE(it1->more());
    EXPECT_EQ(it1->next().first, 50);
    ASSERT_TRUE(it1->more());
    EXPECT_EQ(it1->next().first, 100);

    ASSERT_TRUE(it2->more());
    EXPECT_EQ(it2->next().first, 75);
    ASSERT_TRUE(it2->more());
    EXPECT_EQ(it2->next().first, 125);
}

TEST_P(ContainerBasedSpillerTest, SpillWritesAllEntriesAcrossMultipleBatches) {
    auto opCtx = makeOperationContext();
    auto& ru = *shard_role_details::getRecoveryUnit(opCtx.get());
    dynamic_cast<repl::ReplicationCoordinatorMock*>(repl::ReplicationCoordinator::get(opCtx.get()))
        ->alwaysAllowWrites(true);

    ViewableIntegerKeyedContainer container{
        std::make_shared<Ident>(ident::generateNewInternalIdent("multi_batch_spill"sv))};
    SorterContainerStats stats{nullptr};
    using Settings = Spiller<IntWrapper, IntWrapper, IWComparator>::Settings;

    ContainerBasedSpiller<IntWrapper, IntWrapper, IWComparator> spiller{
        *opCtx,
        ru,
        container,
        stats,
        boost::none,
        sorter::kLatestChecksumVersion,
        nullptr,
        batchSize(),
        batchBytes(),
        testSpillingMinAvailableDiskSpaceBytes};

    // Enough entries that every (batchSize, batchBytes) combination needs many passes through the
    // subspan loop. The count is coprime with every batchSize under test, so the final batch is
    // always a short remainder rather than a full one.
    static constexpr int kNumEntries = 101;
    std::vector<std::pair<IntWrapper, IntWrapper>> data;
    data.reserve(kNumEntries);
    for (int i = 0; i < kNumEntries; ++i) {
        data.push_back({IntWrapper{i * 10}, IntWrapper{-i * 10}});
    }

    spiller.spill(SortOptions{}, Settings{}, std::span{data});

    ASSERT_EQ(container.entries().size(), static_cast<size_t>(kNumEntries));
    EXPECT_EQ(stats.numSpilledEntries(), kNumEntries);
    const int64_t firstKey = container.entries()[0].first;
    for (size_t i = 0; i < container.entries().size(); ++i) {
        EXPECT_EQ(container.entries()[i].first, firstKey + static_cast<int64_t>(i));
    }

    ASSERT_EQ(spiller.iterators().size(), 1U);
    auto it = spiller.iterators()[0];
    for (int i = 0; i < kNumEntries; ++i) {
        ASSERT_TRUE(it->more());
        auto [key, val] = it->next();
        EXPECT_EQ(key, i * 10);
        EXPECT_EQ(val, -i * 10);
    }
    EXPECT_FALSE(it->more());
}

TEST_P(ContainerBasedSpillerTest, MergeSpills) {
    auto opCtx = makeOperationContext();

    auto replCoord = dynamic_cast<repl::ReplicationCoordinatorMock*>(
        repl::ReplicationCoordinator::get(opCtx.get()));
    ASSERT(replCoord);
    replCoord->alwaysAllowWrites(true);

    const auto identStr = ident::generateNewInternalIdent("container_spill"sv);
    ViewableIntegerKeyedContainer container{std::make_shared<Ident>(identStr)};
    SorterContainerStats containerStats{nullptr};

    ContainerBasedSpiller<IntWrapper, NullValue, IWComparator> spiller{
        *opCtx,
        *shard_role_details::getRecoveryUnit(opCtx.get()),
        container,
        containerStats,
        boost::none,
        sorter::kLatestChecksumVersion,
        [&] {
            auto callbacks = std::make_unique<testing::NiceMock<SpillerCallbackMock>>();
            // 3 spills plus 2 merge passes
            EXPECT_CALL(*callbacks, onSpill).Times(5);
            return callbacks;
        }(),
        batchSize(),
        batchBytes(),
        testSpillingMinAvailableDiskSpaceBytes};

    std::vector<std::pair<IntWrapper, NullValue>> data{
        {50, {}}, {100, {}}, {75, {}}, {125, {}}, {25, {}}};
    std::span<std::pair<IntWrapper, NullValue>> span{data};

    spiller.spill(SortOptions{},
                  Spiller<IntWrapper, NullValue, IWComparator>::Settings{},
                  span.subspan(0, 2));
    spiller.spill(SortOptions{},
                  Spiller<IntWrapper, NullValue, IWComparator>::Settings{},
                  span.subspan(2, 2));
    spiller.spill(SortOptions{},
                  Spiller<IntWrapper, NullValue, IWComparator>::Settings{},
                  span.subspan(4, 1));

    SorterStats sorterStats{nullptr};
    spiller.mergeSpills(SortOptions{},
                        Spiller<IntWrapper, NullValue, IWComparator>::Settings{},
                        sorterStats,
                        IWComparator(ASC),
                        2,
                        2);

    auto& iterators = spiller.iterators();
    EXPECT_EQ(iterators.size(), 2);
    EXPECT_EQ(container.entries().size(), data.size());

    ASSERT_TRUE(iterators[0]->more());
    EXPECT_EQ(iterators[0]->next().first, 50);
    ASSERT_TRUE(iterators[0]->more());
    EXPECT_EQ(iterators[0]->next().first, 75);
    ASSERT_TRUE(iterators[0]->more());
    EXPECT_EQ(iterators[0]->next().first, 100);
    ASSERT_TRUE(iterators[0]->more());
    EXPECT_EQ(iterators[0]->next().first, 125);
    EXPECT_FALSE(iterators[0]->more());

    ASSERT_TRUE(iterators[1]->more());
    EXPECT_EQ(iterators[1]->next().first, 25);
    EXPECT_FALSE(iterators[1]->more());
}

TEST_P(ContainerBasedSpillerTest, MergeSpillsMultiplePasses) {
    auto opCtx = makeOperationContext();

    auto replCoord = dynamic_cast<repl::ReplicationCoordinatorMock*>(
        repl::ReplicationCoordinator::get(opCtx.get()));
    ASSERT(replCoord);
    replCoord->alwaysAllowWrites(true);

    const auto identStr = ident::generateNewInternalIdent("container_spill"sv);
    ViewableIntegerKeyedContainer container{std::make_shared<Ident>(identStr)};
    SorterContainerStats containerStats{nullptr};

    std::pair<IntWrapper, NullValue> data[] = {{50, {}},
                                               {100, {}},
                                               {75, {}},
                                               {125, {}},
                                               {120, {}},
                                               {115, {}},
                                               {110, {}},
                                               {150, {}},
                                               {175, {}},
                                               {105, {}}};
    std::span span{data};

    ContainerBasedSpiller<IntWrapper, NullValue, IWComparator> spiller{
        *opCtx,
        *shard_role_details::getRecoveryUnit(opCtx.get()),
        container,
        containerStats,
        boost::none,
        sorter::kLatestChecksumVersion,
        [&] {
            auto callbacks = std::make_unique<testing::NiceMock<SpillerCallbackMock>>();
            // One spill per element plus 8 merge passes
            EXPECT_CALL(*callbacks, onSpill).Times(span.size() + 8);
            return callbacks;
        }(),
        batchSize(),
        batchBytes(),
        testSpillingMinAvailableDiskSpaceBytes};


    for (size_t i = 0; i < span.size(); ++i) {
        spiller.spill(SortOptions{},
                      Spiller<IntWrapper, NullValue, IWComparator>::Settings{},
                      span.subspan(i, 1));
    }

    SorterStats sorterStats{nullptr};
    spiller.mergeSpills(SortOptions{},
                        Spiller<IntWrapper, NullValue, IWComparator>::Settings{},
                        sorterStats,
                        IWComparator(ASC),
                        3,
                        2);

    auto& iterators = spiller.iterators();
    EXPECT_EQ(iterators.size(), 3);
    EXPECT_EQ(container.entries().size(), span.size());

    ASSERT_TRUE(iterators[0]->more());
    EXPECT_EQ(iterators[0]->next().first, 50);
    ASSERT_TRUE(iterators[0]->more());
    EXPECT_EQ(iterators[0]->next().first, 75);
    ASSERT_TRUE(iterators[0]->more());
    EXPECT_EQ(iterators[0]->next().first, 100);
    ASSERT_TRUE(iterators[0]->more());
    EXPECT_EQ(iterators[0]->next().first, 125);
    EXPECT_FALSE(iterators[0]->more());

    ASSERT_TRUE(iterators[1]->more());
    EXPECT_EQ(iterators[1]->next().first, 110);
    ASSERT_TRUE(iterators[1]->more());
    EXPECT_EQ(iterators[1]->next().first, 115);
    ASSERT_TRUE(iterators[1]->more());
    EXPECT_EQ(iterators[1]->next().first, 120);
    ASSERT_TRUE(iterators[1]->more());
    EXPECT_EQ(iterators[1]->next().first, 150);
    EXPECT_FALSE(iterators[1]->more());

    ASSERT_TRUE(iterators[2]->more());
    EXPECT_EQ(iterators[2]->next().first, 105);
    ASSERT_TRUE(iterators[2]->more());
    EXPECT_EQ(iterators[2]->next().first, 175);
    EXPECT_FALSE(iterators[2]->more());
}

TEST_P(ContainerBasedSpillerTest, MergeSpillsOnSpillSeesCompleteIteratorView) {
    auto opCtx = makeOperationContext();

    auto replCoord = dynamic_cast<repl::ReplicationCoordinatorMock*>(
        repl::ReplicationCoordinator::get(opCtx.get()));
    ASSERT(replCoord);
    replCoord->alwaysAllowWrites(true);

    const auto identStr = ident::generateNewInternalIdent("container_spill"sv);
    ViewableIntegerKeyedContainer container{std::make_shared<Ident>(identStr)};
    SorterContainerStats containerStats{nullptr};
    std::vector<std::vector<std::pair<int64_t, int64_t>>> snapshots;

    auto callbacks = std::make_unique<SpillerCallbackMock>();
    auto& callbacksRef = *callbacks;

    ContainerBasedSpiller<IntWrapper, NullValue, IWComparator> spiller{
        *opCtx,
        *shard_role_details::getRecoveryUnit(opCtx.get()),
        container,
        containerStats,
        boost::none,
        sorter::kLatestChecksumVersion,
        std::move(callbacks),
        batchSize(),
        batchBytes(),
        testSpillingMinAvailableDiskSpaceBytes};

    EXPECT_CALL(callbacksRef, onSpill).WillRepeatedly([&] {
        std::vector<std::pair<int64_t, int64_t>> snapshot;
        for (const auto& it : spiller.iterators()) {
            auto range = it->getRange();
            snapshot.emplace_back(range.getStart(), range.getEnd());
        }
        snapshots.push_back(std::move(snapshot));
    });

    // Five single-element spills so each starting iterator covers exactly one container key.
    std::vector<std::pair<IntWrapper, NullValue>> data{
        {50, {}}, {100, {}}, {75, {}}, {125, {}}, {25, {}}};
    std::span<std::pair<IntWrapper, NullValue>> span{data};
    for (size_t i = 0; i < data.size(); ++i) {
        spiller.spill(SortOptions{},
                      Spiller<IntWrapper, NullValue, IWComparator>::Settings{},
                      span.subspan(i, 1));
    }

    // We only care about snapshots taken during the merge pass.
    snapshots.clear();

    SorterStats sorterStats{nullptr};
    spiller.mergeSpills(SortOptions{},
                        Spiller<IntWrapper, NullValue, IWComparator>::Settings{},
                        sorterStats,
                        IWComparator(ASC),
                        /*numTargetedSpills=*/2,
                        /*maxSpillsPerMerge=*/2);

    // Five inner-loop iterations: three in outer pass 1 (chunks [I1,I2], [I3,I4], [I5]) and two in
    // outer pass 2 (chunks [X,Y], [Z]). The snapshot recorded at each onSpill must reflect the
    // post-merge view: the just-written merged range replaces the merged-from entries of this
    // chunk, even though the merged-from data is still on disk.
    ASSERT_EQ(snapshots.size(), 5);
    EXPECT_EQ(snapshots[0],
              (std::vector<std::pair<int64_t, int64_t>>{{3, 4}, {4, 5}, {5, 6}, {6, 8}}));
    EXPECT_EQ(snapshots[1], (std::vector<std::pair<int64_t, int64_t>>{{5, 6}, {6, 8}, {8, 10}}));
    EXPECT_EQ(snapshots[2], (std::vector<std::pair<int64_t, int64_t>>{{6, 8}, {8, 10}, {10, 11}}));
    EXPECT_EQ(snapshots[3], (std::vector<std::pair<int64_t, int64_t>>{{10, 11}, {11, 15}}));
    EXPECT_EQ(snapshots[4], (std::vector<std::pair<int64_t, int64_t>>{{11, 15}, {15, 16}}));

    auto& iterators = spiller.iterators();
    ASSERT_EQ(iterators.size(), 2);
    EXPECT_EQ(container.entries().size(), data.size());

    ASSERT_TRUE(iterators[0]->more());
    EXPECT_EQ(iterators[0]->next().first, 50);
    ASSERT_TRUE(iterators[0]->more());
    EXPECT_EQ(iterators[0]->next().first, 75);
    ASSERT_TRUE(iterators[0]->more());
    EXPECT_EQ(iterators[0]->next().first, 100);
    ASSERT_TRUE(iterators[0]->more());
    EXPECT_EQ(iterators[0]->next().first, 125);
    EXPECT_FALSE(iterators[0]->more());

    ASSERT_TRUE(iterators[1]->more());
    EXPECT_EQ(iterators[1]->next().first, 25);
    EXPECT_FALSE(iterators[1]->more());
}

TEST_P(ContainerBasedSpillerTest, SpillDirPathFromIdent) {
    auto opCtx = makeOperationContext();

    auto ns = NamespaceString::createNamespaceString_forTest("test", "container_based_spiller");
    SorterContainerStats stats{nullptr};
    auto& ru = *shard_role_details::getRecoveryUnit(opCtx.get());

    const boost::filesystem::path basePath{storageGlobalParams.dbpath};
    const auto dbComponent = ident::createDBNamePathComponent(ns.dbName());
    const auto sorterStem = "sorter";

    struct TestCase {
        bool directoryPerDB;
        bool directoryForIndexes;
        boost::filesystem::path expectedPath;
    };
    const std::vector<TestCase> testCases = {
        {false, false, basePath},
        {false, true, basePath},
        {true, false, basePath / dbComponent},
        {true, true, basePath / dbComponent},
    };

    for (const auto& testCase : testCases) {
        auto indexIdent = ident::generateNewIndexIdent(
            ns.dbName(), testCase.directoryPerDB, testCase.directoryForIndexes);
        auto internalIdent = ident::generateNewInternalIndexBuildIdent(sorterStem, indexIdent);
        ViewableIntegerKeyedContainer container{std::make_shared<Ident>(internalIdent)};
        ContainerBasedSpiller<IntWrapper, NullValue, IWComparator> spiller{
            *opCtx,
            ru,
            container,
            stats,
            ns.dbName(),
            sorter::kLatestChecksumVersion,
            nullptr,
            batchSize(),
            batchBytes(),
            testSpillingMinAvailableDiskSpaceBytes};

        auto spillPath = spiller.getSpillDir();
        EXPECT_EQ(spillPath, testCase.expectedPath);
    }
}

TEST_P(ContainerBasedSpillerTest, SpillerWithStartingKeyPreservesExistingEntries) {
    auto opCtx = makeOperationContext();
    auto& ru = *shard_role_details::getRecoveryUnit(opCtx.get());
    dynamic_cast<repl::ReplicationCoordinatorMock*>(repl::ReplicationCoordinator::get(opCtx.get()))
        ->alwaysAllowWrites(true);

    ViewableIntegerKeyedContainer container{
        std::make_shared<Ident>(ident::generateNewInternalIdent("starting_key_test"sv))};
    SorterContainerStats stats{nullptr};
    using Settings = Spiller<IntWrapper, IntWrapper, IWComparator>::Settings;

    auto makeSpiller = [&](int64_t startingKey) {
        return ContainerBasedSpiller<IntWrapper, IntWrapper, IWComparator>{
            *opCtx,
            ru,
            container,
            startingKey,
            stats,
            boost::none,
            sorter::kLatestChecksumVersion,
            nullptr,
            batchSize(),
            batchBytes(),
            testSpillingMinAvailableDiskSpaceBytes};
    };

    std::vector<std::pair<IntWrapper, IntWrapper>> batch1{{10, -10}, {20, -20}, {30, -30}};
    std::vector<std::pair<IntWrapper, IntWrapper>> batch2{{40, -40}, {50, -50}};

    auto spiller1 = makeSpiller(1);
    spiller1.spill(SortOptions{}, Settings{}, std::span{batch1});
    EXPECT_EQ(container.entries().size(), 3U);

    auto spiller2 = makeSpiller(4);
    spiller2.spill(SortOptions{}, Settings{}, std::span{batch2});
    EXPECT_EQ(container.entries().size(), 5U);
}

// Verifies getSortedIterator() on ContainerBasedStorage: reads back exactly the entries
// written by makeWriter and passes the final checksum check on exhaustion.
TEST_F(SortedContainerWriterTest, GetSortedIteratorReadsRange) {
    auto opCtx = makeOperationContext();
    auto* replCoord = repl::ReplicationCoordinator::get(opCtx.get());
    auto* replCoordMock = dynamic_cast<repl::ReplicationCoordinatorMock*>(replCoord);
    ASSERT(replCoordMock);
    replCoordMock->alwaysAllowWrites(true);

    ViewableIntegerKeyedContainer container;
    container.setIdent(std::make_shared<Ident>("get_sorted_iterator_test"));
    auto& ru = *shard_role_details::getRecoveryUnit(opCtx.get());
    SorterContainerStats stats(&this->sorterTracker);

    using KV = std::pair<IntWrapper, IntWrapper>;
    using Settings = Iterator<IntWrapper, IntWrapper>::Settings;

    ContainerBasedStorage<IntWrapper, IntWrapper> storage(*opCtx,
                                                          ru,
                                                          container,
                                                          stats,
                                                          /*currKey=*/1,
                                                          boost::none,
                                                          sorter::kLatestChecksumVersion);

    std::vector<KV> entries = {{10, 1}, {20, 2}, {30, 3}, {40, 4}, {50, 5}};
    SortOptions opts;
    auto writer = storage.makeWriter(opts, Settings{});
    for (auto& [k, v] : entries) {
        writer->addAlreadySorted(k, v);
    }
    auto range = writer->done()->getRange();

    auto iter = storage.getSortedIterator(range, Settings{});
    for (auto& [k, v] : entries) {
        ASSERT_TRUE(iter->more());
        auto [gotK, gotV] = iter->next();
        EXPECT_EQ(gotK, k);
        EXPECT_EQ(gotV, v);
    }
    EXPECT_FALSE(iter->more());
}

TEST_F(SortedContainerWriterTest, ReconstructPartiallyConsumedIterator) {
    auto opCtx = makeOperationContext();
    auto* replCoord = repl::ReplicationCoordinator::get(opCtx.get());
    auto* replCoordMock = dynamic_cast<repl::ReplicationCoordinatorMock*>(replCoord);
    ASSERT(replCoordMock);
    replCoordMock->alwaysAllowWrites(true);

    ViewableIntegerKeyedContainer container;
    container.setIdent(std::make_shared<Ident>("get_sorted_iterator_resume_partial"));
    auto& ru = *shard_role_details::getRecoveryUnit(opCtx.get());
    SorterContainerStats stats(&this->sorterTracker);

    using KV = std::pair<IntWrapper, IntWrapper>;
    using Settings = Iterator<IntWrapper, IntWrapper>::Settings;

    ContainerBasedStorage<IntWrapper, IntWrapper> storage(*opCtx,
                                                          ru,
                                                          container,
                                                          stats,
                                                          /*currKey=*/1,
                                                          boost::none,
                                                          sorter::kLatestChecksumVersion);

    std::vector<KV> entries = {{10, 1}, {20, 2}, {30, 3}, {40, 4}, {50, 5}};
    SortOptions opts;
    auto writer = storage.makeWriter(opts, Settings{});
    for (auto& [k, v] : entries) {
        writer->addAlreadySorted(k, v);
    }
    auto firstIter = writer->done();

    // Drain the first three entries from the original iterator, then get its range.
    for (size_t i = 0; i < 3; ++i) {
        ASSERT_TRUE(firstIter->more());
        auto [gotK, gotV] = firstIter->next();
        EXPECT_EQ(gotK, entries[i].first);
        EXPECT_EQ(gotV, entries[i].second);
    }
    auto range = firstIter->getRange();
    ASSERT_TRUE(range.getCurrent().has_value());
    ASSERT_TRUE(range.getCurrentChecksum().has_value());

    // Reconstruct an iterator from the persisted range and read the remaining entries.
    auto reconstructed = storage.getSortedIterator(range, Settings{});
    auto reconstructedRange = reconstructed->getRange();
    ASSERT_TRUE(reconstructedRange.getCurrent().has_value());
    EXPECT_EQ(*reconstructedRange.getCurrent(), *range.getCurrent());
    ASSERT_TRUE(reconstructedRange.getCurrentChecksum().has_value());
    EXPECT_EQ(*reconstructedRange.getCurrentChecksum(), *range.getCurrentChecksum());
    for (size_t i = 3; i < entries.size(); ++i) {
        ASSERT_TRUE(reconstructed->more());
        auto [gotK, gotV] = reconstructed->next();
        EXPECT_EQ(gotK, entries[i].first);
        EXPECT_EQ(gotV, entries[i].second);
    }
    EXPECT_FALSE(reconstructed->more());
}

TEST_F(SortedContainerWriterTest, MergeIteratorReconstructsAllKeysAcrossRanges) {
    auto opCtx = makeOperationContext();
    auto* replCoord = repl::ReplicationCoordinator::get(opCtx.get());
    auto* replCoordMock = dynamic_cast<repl::ReplicationCoordinatorMock*>(replCoord);
    ASSERT(replCoordMock);
    replCoordMock->alwaysAllowWrites(true);

    ViewableIntegerKeyedContainer container;
    container.setIdent(std::make_shared<Ident>("merge_iterator_resume"));
    auto& ru = *shard_role_details::getRecoveryUnit(opCtx.get());
    SorterContainerStats stats(&this->sorterTracker);

    using KV = std::pair<IntWrapper, IntWrapper>;
    using Settings = Iterator<IntWrapper, IntWrapper>::Settings;

    ContainerBasedStorage<IntWrapper, IntWrapper> storage(*opCtx,
                                                          ru,
                                                          container,
                                                          stats,
                                                          /*currKey=*/1,
                                                          boost::none,
                                                          sorter::kLatestChecksumVersion);

    // Three sorted ranges chosen so the merge interleaves all three (no range is fully drained
    // before another starts) at the time we persist mid-merge.
    SortOptions opts;
    std::vector<std::vector<KV>> ranges = {
        {{10, 100}, {40, 400}, {70, 700}, {90, 900}},
        {{20, 200}, {50, 500}, {80, 800}},
        {{30, 300}, {60, 600}},
    };
    std::vector<KV> allKeysSorted;
    for (auto&& r : ranges) {
        for (auto& kv : r) {
            allKeysSorted.push_back(kv);
        }
    }
    std::sort(allKeysSorted.begin(), allKeysSorted.end(), [](const KV& a, const KV& b) {
        return IWComparator(ASC)(a.first, b.first) < 0;
    });

    // Build the iterators for each range and remember them so we can call getRange() on the
    // underlying iterators after the merge has consumed some keys.
    std::vector<std::shared_ptr<Iterator<IntWrapper, IntWrapper>>> iters;
    int64_t nextContainerKey = 1;
    for (auto&& rangeData : ranges) {
        storage.updateCurrKey(nextContainerKey);
        auto writer = storage.makeWriter(opts, Settings{});
        for (auto& [k, v] : rangeData) {
            writer->addAlreadySorted(k, v);
        }
        nextContainerKey += static_cast<int64_t>(rangeData.size());
        iters.push_back(std::shared_ptr<Iterator<IntWrapper, IntWrapper>>(writer->done()));
    }

    auto firstMerge =
        sorter::merge<IntWrapper, IntWrapper>(std::span{iters}, opts, IWComparator(ASC));

    // Drain the first four keys -- 10, 20, 30, 40. All three ranges have contributed at least one
    // key by this point, but none are exhausted.
    constexpr size_t kKeysDrained = 4;
    for (size_t i = 0; i < kKeysDrained; ++i) {
        ASSERT_TRUE(firstMerge->more());
        auto [gotK, gotV] = firstMerge->next();
        EXPECT_EQ(gotK, allKeysSorted[i].first);
        EXPECT_EQ(gotV, allKeysSorted[i].second);
    }

    std::vector<SorterRange> persistedRanges;
    persistedRanges.reserve(iters.size());
    for (auto& it : iters) {
        persistedRanges.push_back(it->getRange());
    }

    // Reconstruct fresh iterators from the persisted ranges and merge them again. Every key
    // not yet emitted should appear in order, with no duplicates and no losses.
    std::vector<std::shared_ptr<Iterator<IntWrapper, IntWrapper>>> resumedIters;
    for (auto& range : persistedRanges) {
        resumedIters.push_back(storage.getSortedIterator(range, Settings{}));
    }
    auto resumedMerge =
        sorter::merge<IntWrapper, IntWrapper>(std::span{resumedIters}, opts, IWComparator(ASC));

    for (size_t i = kKeysDrained; i < allKeysSorted.size(); ++i) {
        ASSERT_TRUE(resumedMerge->more()) << "resumed merge ran out at index " << i << "; expected "
                                          << allKeysSorted[i].first.toString() << " next";
        auto [gotK, gotV] = resumedMerge->next();
        EXPECT_EQ(gotK, allKeysSorted[i].first);
        EXPECT_EQ(gotV, allKeysSorted[i].second);
    }
    EXPECT_FALSE(resumedMerge->more());
}

TEST_F(SortedContainerWriterTest, ReconstructPartiallyExhaustedIterator) {
    auto opCtx = makeOperationContext();
    auto* replCoord = repl::ReplicationCoordinator::get(opCtx.get());
    auto* replCoordMock = dynamic_cast<repl::ReplicationCoordinatorMock*>(replCoord);
    ASSERT(replCoordMock);
    replCoordMock->alwaysAllowWrites(true);

    ViewableIntegerKeyedContainer container;
    container.setIdent(std::make_shared<Ident>("get_sorted_iterator_resume_exhausted"));
    auto& ru = *shard_role_details::getRecoveryUnit(opCtx.get());
    SorterContainerStats stats(&this->sorterTracker);

    using KV = std::pair<IntWrapper, IntWrapper>;
    using Settings = Iterator<IntWrapper, IntWrapper>::Settings;

    ContainerBasedStorage<IntWrapper, IntWrapper> storage(*opCtx,
                                                          ru,
                                                          container,
                                                          stats,
                                                          /*currKey=*/1,
                                                          boost::none,
                                                          sorter::kLatestChecksumVersion);

    std::vector<KV> entries = {{10, 1}, {20, 2}, {30, 3}};
    SortOptions opts;
    auto writer = storage.makeWriter(opts, Settings{});
    for (auto& [k, v] : entries) {
        writer->addAlreadySorted(k, v);
    }
    auto firstIter = writer->done();

    for (auto& [k, v] : entries) {
        ASSERT_TRUE(firstIter->more());
        auto [gotK, gotV] = firstIter->next();
        EXPECT_EQ(gotK, k);
        EXPECT_EQ(gotV, v);
    }
    EXPECT_FALSE(firstIter->more());

    auto range = firstIter->getRange();
    ASSERT_TRUE(range.getCurrent().has_value());
    EXPECT_EQ(*range.getCurrent(), range.getEnd());

    auto reconstructed = storage.getSortedIterator(range, Settings{});
    auto reconstructedRange = reconstructed->getRange();
    ASSERT_TRUE(reconstructedRange.getCurrent().has_value());
    EXPECT_EQ(*reconstructedRange.getCurrent(), *range.getCurrent());
    ASSERT_TRUE(reconstructedRange.getCurrentChecksum().has_value());
    EXPECT_EQ(*reconstructedRange.getCurrentChecksum(), *range.getCurrentChecksum());
    EXPECT_FALSE(reconstructed->more());
}

/**
 * Provides a storage-backed IntegerKeyedContainer.
 */
class StorageBackedContainerTest : public ServiceContextMongoDTest {
public:
    // TODO (SERVER-116165): Remove.
    unittest::ServerParameterGuard ffContainerWrites{"featureFlagContainerWrites", true};

protected:
    void setUp() override {
        ServiceContextMongoDTest::setUp();
        _opCtx = makeOperationContext();
        auto* replCoord = dynamic_cast<repl::ReplicationCoordinatorMock*>(
            repl::ReplicationCoordinator::get(_opCtx.get()));
        ASSERT(replCoord);
        replCoord->alwaysAllowWrites(true);

        auto* storageEngine = getServiceContext()->getStorageEngine();
        {
            WriteUnitOfWork wuow(_opCtx.get());
            _tempRS = storageEngine->makeInternalRecordStore(
                _opCtx.get(), storageEngine->generateNewInternalIdent(), KeyFormat::Long);
            wuow.commit();
        }
        _container =
            &std::get<std::reference_wrapper<IntegerKeyedContainer>>(_tempRS->getContainer()).get();
    }

    OperationContext* opCtx() {
        return _opCtx.get();
    }
    IntegerKeyedContainer& container() {
        return *_container;
    }
    RecoveryUnit& ru() {
        return *shard_role_details::getRecoveryUnit(_opCtx.get());
    }

private:
    ServiceContext::UniqueOperationContext _opCtx;
    std::unique_ptr<RecordStore> _tempRS;
    IntegerKeyedContainer* _container = nullptr;
};

TEST_F(StorageBackedContainerTest, RecoverCursorAfterAbandoningSnapshot) {
    const std::vector<std::pair<IntWrapper, IntWrapper>> data = {
        {1, 1},
        {2, 2},
        {3, 3},
        {4, 4},
        {5, 5},
    };

    SorterChecksumCalculator checksumCalc{sorter::kLatestChecksumVersion};
    {
        StorageWriteTransaction txn(ru());
        int64_t containerKey = 1;
        for (auto& [k, v] : data) {
            BufBuilder buf;
            k.serializeForSorter(buf);
            v.serializeForSorter(buf);
            ASSERT_OK(container().insert(ru(),
                                         containerKey++,
                                         {buf.buf(), static_cast<size_t>(buf.len())},
                                         container::ExistingKeyPolicy::reject));
            checksumCalc.addData(buf.buf(), buf.len());
        }
        txn.commit();
    }

    ContainerIterator<IntWrapper, IntWrapper> iter(container().getCursor(ru()),
                                                   /*start=*/1,
                                                   /*end=*/static_cast<int64_t>(data.size()) + 1,
                                                   Iterator<IntWrapper, IntWrapper>::Settings{},
                                                   checksumCalc.checksum(),
                                                   sorter::kLatestChecksumVersion);

    ASSERT_TRUE(iter.more());
    EXPECT_EQ(iter.next(), (std::pair<IntWrapper, IntWrapper>{1, 1}));
    ASSERT_TRUE(iter.more());
    EXPECT_EQ(iter.next(), (std::pair<IntWrapper, IntWrapper>{2, 2}));

    // Testing that we can recover from this.
    ru().abandonSnapshot();

    ASSERT_TRUE(iter.more());
    EXPECT_EQ(iter.next(), (std::pair<IntWrapper, IntWrapper>{3, 3}));
    ASSERT_TRUE(iter.more());
    EXPECT_EQ(iter.next(), (std::pair<IntWrapper, IntWrapper>{4, 4}));
    ASSERT_TRUE(iter.more());
    EXPECT_EQ(iter.next(), (std::pair<IntWrapper, IntWrapper>{5, 5}));
    EXPECT_FALSE(iter.more());
}

class ContainerBasedSpillerWriteConflictTest : public StorageBackedContainerTest {
protected:
    SorterContainerStats stats{nullptr};
};

// Calls mergeSpills() with a deterministic WCE on the first merged write. Exercises SERVER-126155
// and SERVER-124271.
TEST_F(ContainerBasedSpillerWriteConflictTest, MergeSpillsSurvivesCursorResetUnderWCE) {
    ContainerBasedSpiller<IntWrapper, IntWrapper, IWComparator> spiller{
        *opCtx(),
        ru(),
        container(),
        stats,
        boost::none,
        sorter::kLatestChecksumVersion,
        nullptr,
        /*batchSize=*/1,
        /*batchBytes=*/std::numeric_limits<int64_t>::max(),
        testSpillingMinAvailableDiskSpaceBytes};

    std::vector<std::pair<IntWrapper, IntWrapper>> data{
        {10, 100},
        {40, 400},  // range 0
        {20, 200},
        {50, 500},  // range 1
        {30, 300},
        {60, 600},  // range 2
    };
    std::span<std::pair<IntWrapper, IntWrapper>> span{data};
    using SpillerSettings = Spiller<IntWrapper, IntWrapper, IWComparator>::Settings;
    spiller.spill(SortOptions{}, SpillerSettings{}, span.subspan(0, 2));
    spiller.spill(SortOptions{}, SpillerSettings{}, span.subspan(2, 2));
    spiller.spill(SortOptions{}, SpillerSettings{}, span.subspan(4, 2));
    ASSERT_EQ(spiller.iterators().size(), 3);

    // Fires WCE on the first merged write.
    auto writeConflict = enableWriteConflictForWrites(
        FailPoint::ModeOptions{.mode = FailPoint::Mode::nTimes, .val = 1});

    SorterStats sorterStats{nullptr};
    spiller.mergeSpills(SortOptions{},
                        SpillerSettings{},
                        sorterStats,
                        IWComparator(ASC),
                        /*numTargetedSpills=*/1,
                        /*maxSpillsPerMerge=*/3);

    ASSERT_EQ(spiller.iterators().size(), 1);
    const std::vector<std::pair<int, int>> expected{
        {10, 100}, {20, 200}, {30, 300}, {40, 400}, {50, 500}, {60, 600}};
    auto it = spiller.iterators()[0];
    for (const auto& [k, v] : expected) {
        ASSERT_TRUE(it->more());
        auto next = it->next();
        EXPECT_EQ(static_cast<int>(next.first), k);
        EXPECT_EQ(static_cast<int>(next.second), v);
    }
    EXPECT_FALSE(it->more());
}

// Verifies that the writeConflictRetry block in mergeSpills_remove correctly retries when the
// storage engine throws a WCE on the first deletion.
TEST_F(ContainerBasedSpillerWriteConflictTest, MergeSpillsRemoveSurvivesWCE) {
    using Spiller = ContainerBasedSpiller<IntWrapper, IntWrapper, IWComparator>;

    auto callbacks = std::make_unique<SpillerCallbackMock>();
    auto& callbacksRef = *callbacks;
    Spiller spiller{*opCtx(),
                    ru(),
                    container(),
                    stats,
                    boost::none,
                    sorter::kLatestChecksumVersion,
                    std::move(callbacks),
                    /*batchSize=*/1,
                    /*batchBytes=*/std::numeric_limits<int64_t>::max(),
                    testSpillingMinAvailableDiskSpaceBytes};

    std::pair<IntWrapper, IntWrapper> data[] = {
        {10, 100},
        {40, 400},  // range 0
        {20, 200},
        {50, 500},  // range 1
        {30, 300},
        {60, 600},  // range 2
    };
    std::span<std::pair<IntWrapper, IntWrapper>> span{data};
    using SpillerSettings = Spiller::Settings;
    spiller.spill(SortOptions{}, SpillerSettings{}, span.subspan(0, 2));
    spiller.spill(SortOptions{}, SpillerSettings{}, span.subspan(2, 2));
    spiller.spill(SortOptions{}, SpillerSettings{}, span.subspan(4, 2));
    ASSERT_EQ(spiller.iterators().size(), 3);

    SorterStats sorterStats{nullptr};

    std::unique_ptr<FailPointEnableBlock> writeConflict;
    FailPoint::EntryCountT wceCountBefore = 0;
    EXPECT_CALL(callbacksRef, onSpill).WillOnce([&] {
        writeConflict = enableWriteConflictForWrites(
            FailPoint::ModeOptions{.mode = FailPoint::Mode::nTimes, .val = 1});
        wceCountBefore = writeConflict->initialTimesEntered();
    });

    spiller.mergeSpills(SortOptions{},
                        SpillerSettings{},
                        sorterStats,
                        IWComparator(ASC),
                        /*numTargetedSpills=*/1,
                        /*maxSpillsPerMerge=*/3);

    ASSERT_EQ((*writeConflict)->waitForTimesEntered(wceCountBefore + 1), wceCountBefore + 1)
        << "Expected exactly one WCE to fire inside mergeSpills_remove";

    ASSERT_EQ(spiller.iterators().size(), 1);
    const std::pair<int, int> expected[] = {
        {10, 100}, {20, 200}, {30, 300}, {40, 400}, {50, 500}, {60, 600}};
    auto it = spiller.iterators()[0];
    for (const auto& [k, v] : expected) {
        ASSERT_TRUE(it->more());
        auto next = it->next();
        EXPECT_EQ(static_cast<int>(next.first), k);
        EXPECT_EQ(static_cast<int>(next.second), v);
    }
    EXPECT_FALSE(it->more());
}

class ContainerBasedSpillerCallbackTest : public ServiceContextMongoDTest {
public:
    // TODO (SERVER-116165): Remove.
    unittest::ServerParameterGuard ffContainerWrites{"featureFlagContainerWrites", true};

protected:
    using Settings = sorter::Spiller<IntWrapper, NullValue, IWComparator>::Settings;
    static constexpr int64_t kBatchSize = 4;
    static constexpr int64_t kBatchBytes = std::numeric_limits<int64_t>::max();

    void setUp() override {
        ServiceContextMongoDTest::setUp();
        _opCtx = makeOperationContext();
        auto replCoord = dynamic_cast<repl::ReplicationCoordinatorMock*>(
            repl::ReplicationCoordinator::get(_opCtx.get()));
        ASSERT(replCoord);
        replCoord->alwaysAllowWrites(true);
        _container.setIdent(
            std::make_shared<Ident>(ident::generateNewInternalIdent("container_spill"sv)));
    }

    auto makeContainerBasedSpiller(std::unique_ptr<SpillCallbacks> callbacks = nullptr) {
        return ContainerBasedSpiller<IntWrapper, NullValue, IWComparator>{
            *_opCtx,
            *shard_role_details::getRecoveryUnit(_opCtx.get()),
            _container,
            _containerStats,
            boost::none,
            sorter::kLatestChecksumVersion,
            std::move(callbacks),
            kBatchSize,
            kBatchBytes,
            testSpillingMinAvailableDiskSpaceBytes};
    }

    ServiceContext::UniqueOperationContext _opCtx;
    ViewableIntegerKeyedContainer _container;
    SorterContainerStats _containerStats{nullptr};
};

TEST_F(ContainerBasedSpillerCallbackTest, SpillCallbacksFireAroundSpillerEntryPoints) {
    int preCount = 0;
    int onCount = 0;
    int postCount = 0;
    auto callbacks = std::make_unique<testing::NiceMock<SpillerCallbackMock>>();
    EXPECT_CALL(*callbacks, preSpill).WillRepeatedly([&] { ++preCount; });
    EXPECT_CALL(*callbacks, onSpill).WillRepeatedly([&] { ++onCount; });
    EXPECT_CALL(*callbacks, postSpill).WillRepeatedly([&] { ++postCount; });
    auto spiller = makeContainerBasedSpiller(std::move(callbacks));

    std::vector<std::pair<IntWrapper, NullValue>> data{
        {10, {}}, {20, {}}, {30, {}}, {40, {}}, {50, {}}, {60, {}}};
    std::span span{data};

    // Each spill() call invokes pre/on/post exactly once around its internal
    // writeConflictRetry.
    spiller.spill(SortOptions{}, Settings{}, span.subspan(0, 2));
    EXPECT_EQ(preCount, 1);
    EXPECT_EQ(onCount, 1);
    EXPECT_EQ(postCount, 1);

    spiller.spill(SortOptions{}, Settings{}, span.subspan(2, 2));
    EXPECT_EQ(preCount, 2);
    EXPECT_EQ(onCount, 2);
    EXPECT_EQ(postCount, 2);

    // mergeSpills() also wraps its writeConflictRetry calls in a single pre/post pair, with onSpill
    // firing after writing the merged ranges but before deleting the merged-from ranges.
    SorterStats sorterStats{nullptr};
    spiller.mergeSpills(SortOptions{}, Settings{}, sorterStats, IWComparator(ASC), 1, 2);
    EXPECT_EQ(preCount, 3);
    EXPECT_EQ(onCount, 3);
    EXPECT_EQ(postCount, 3);
}

TEST_F(ContainerBasedSpillerCallbackTest, OnChunkWrittenFiresPerChunkWithBytesWritten) {
    std::vector<int64_t> chunks;
    int preCount = 0;
    // kBatchSize is 4, so a 10-entry spill is written as chunks of 4, 4 and 2.
    auto callbacks = std::make_unique<testing::NiceMock<SpillerCallbackMock>>();
    EXPECT_CALL(*callbacks, preSpill).WillRepeatedly([&] { ++preCount; });
    EXPECT_CALL(*callbacks, onChunkWritten).WillRepeatedly([&](int64_t bytes) {
        chunks.push_back(bytes);
    });
    auto spiller = makeContainerBasedSpiller(std::move(callbacks));

    std::vector<std::pair<IntWrapper, NullValue>> data;
    for (int i = 0; i < 10; ++i) {
        data.emplace_back(IntWrapper{i}, NullValue{});
    }
    spiller.spill(SortOptions{}, Settings{}, std::span{data});

    // Several chunks per spill, i.e. the callback is a finer-grained hook than preSpill/postSpill.
    ASSERT_EQ(preCount, 1);
    ASSERT_EQ(chunks.size(), 3);
    for (auto bytes : chunks) {
        ASSERT_GT(bytes, 0);
    }
    // The reported bytes account for exactly what was spilled, so a caller charging a rate limiter
    // per chunk bills the same total as the container stats report.
    ASSERT_EQ(std::accumulate(chunks.begin(), chunks.end(), int64_t{0}),
              _containerStats.bytesSpilled());
}

TEST_F(ContainerBasedSpillerCallbackTest, OnChunkWrittenFiresWhileMergingSpills) {
    std::vector<int64_t> chunks;
    auto callbacks = std::make_unique<testing::NiceMock<SpillerCallbackMock>>();
    EXPECT_CALL(*callbacks, onChunkWritten).WillRepeatedly([&](int64_t bytes) {
        chunks.push_back(bytes);
    });
    auto spiller = makeContainerBasedSpiller(std::move(callbacks));

    std::vector<std::pair<IntWrapper, NullValue>> data{
        {10, {}}, {20, {}}, {30, {}}, {40, {}}, {50, {}}, {60, {}}};
    std::span span{data};
    spiller.spill(SortOptions{}, Settings{}, span.subspan(0, 3));
    spiller.spill(SortOptions{}, Settings{}, span.subspan(3, 3));
    const auto chunksFromSpills = chunks.size();
    ASSERT_GT(chunksFromSpills, 0);

    // Merges rewrite the merged ranges into the container, so they are paced too.
    SorterStats sorterStats{nullptr};
    spiller.mergeSpills(SortOptions{}, Settings{}, sorterStats, IWComparator(ASC), 1, 2);
    ASSERT_GT(chunks.size(), chunksFromSpills);
    for (auto bytes : chunks) {
        ASSERT_GT(bytes, 0);
    }
}

TEST_F(ContainerBasedSpillerCallbackTest, NoCallbacksLeavesSpillsAsNoOps) {
    // No callbacks passed: spill() must route through the no-op branch for each.
    auto spiller = makeContainerBasedSpiller();

    std::vector<std::pair<IntWrapper, NullValue>> data{{10, {}}, {20, {}}};
    spiller.spill(SortOptions{}, Settings{}, std::span{data});
}

TEST_F(ContainerBasedSpillerCallbackTest, PostSpillFiresEvenWhenOnSpillThrows) {
    // onSpill throws after the writeConflictRetry succeeds, proving the postSpill guard runs
    // even when the exception propagates out of spill().
    auto callbacks = std::make_unique<SpillerCallbackMock>();
    EXPECT_CALL(*callbacks, preSpill).Times(1);
    EXPECT_CALL(*callbacks, onSpill).WillOnce([] {
        uasserted(ErrorCodes::InternalError, "simulated post-spill failure");
    });
    EXPECT_CALL(*callbacks, postSpill).Times(1);
    auto spiller = makeContainerBasedSpiller(std::move(callbacks));

    std::pair<IntWrapper, NullValue> data[] = {{10, {}}, {20, {}}};
    ASSERT_THROWS_CODE(spiller.spill({}, {}, data), DBException, ErrorCodes::InternalError);
}

TEST_F(ContainerBasedSpillerCallbackTest, OnSpillBatchFiresAtBatchBoundaries) {
    auto callbacks = std::make_unique<SpillerCallbackMock>();
    auto& callbacksRef = *callbacks;
    auto spiller = makeContainerBasedSpiller(std::move(callbacks));

    // kBatchSize is 4, so spilling batches the 10 pairs as 4, 4, 2. onSpillBatch fires at each
    // interior batch boundary, so twice, and never mid-batch or after the trailing batch.
    EXPECT_CALL(callbacksRef, onSpillBatch).Times(2).WillRepeatedly([&] {
        // Interior batch boundaries don't produce an iterator
        EXPECT_TRUE(spiller.iterators().empty());
    });

    std::pair<IntWrapper, NullValue> data[] = {{10, {}},
                                               {20, {}},
                                               {30, {}},
                                               {40, {}},
                                               {50, {}},
                                               {60, {}},
                                               {70, {}},
                                               {80, {}},
                                               {90, {}},
                                               {100, {}}};
    spiller.spill(SortOptions{}, Settings{}, data);
    EXPECT_EQ(1, spiller.iterators().size());
}

TEST_F(ContainerBasedSpillerCallbackTest, OnSpillBatchDoesNotFireForSingleBatch) {
    // Fewer pairs than kBatchSize means there is no batch boundary, so onSpillBatch never fires.
    auto callbacks = std::make_unique<SpillerCallbackMock>();
    EXPECT_CALL(*callbacks, onSpillBatch).Times(0);
    auto spiller = makeContainerBasedSpiller(std::move(callbacks));

    std::vector<std::pair<IntWrapper, NullValue>> data{{10, {}}, {20, {}}};
    spiller.spill(SortOptions{}, Settings{}, std::span{data});
}

TEST_F(ContainerBasedSpillerCallbackTest, PostSpillThrowingWhileUnwindingDoesNotMaskOriginalError) {
    // A postSpill that restores a plan executor throws when the operation was interrupted, which is
    // exactly what happens when a throttled spill is killed mid-wait. The original error must still
    // be what escapes, and a second exception must not escape the scope guard.
    auto callbacks = std::make_unique<testing::NiceMock<SpillerCallbackMock>>();
    EXPECT_CALL(*callbacks, onSpill).WillRepeatedly([] {
        uasserted(ErrorCodes::InternalError, "simulated post-spill failure");
    });
    EXPECT_CALL(*callbacks, postSpill).WillRepeatedly([] {
        uasserted(ErrorCodes::Interrupted, "simulated interrupted cursor restore");
    });
    auto spiller = makeContainerBasedSpiller(std::move(callbacks));

    std::vector<std::pair<IntWrapper, NullValue>> data{{10, {}}, {20, {}}};
    ASSERT_THROWS_CODE(spiller.spill(SortOptions{}, Settings{}, std::span{data}),
                       DBException,
                       ErrorCodes::InternalError);
}

TEST_F(ContainerBasedSpillerCallbackTest, PostSpillFailureOnSuccessPathPropagates) {
    // With no exception in flight, a postSpill failure is a real error and must reach the caller.
    auto callbacks = std::make_unique<testing::NiceMock<SpillerCallbackMock>>();
    EXPECT_CALL(*callbacks, postSpill).WillRepeatedly([] {
        uasserted(ErrorCodes::Interrupted, "simulated cursor restore failure");
    });
    auto spiller = makeContainerBasedSpiller(std::move(callbacks));

    std::vector<std::pair<IntWrapper, NullValue>> data{{10, {}}, {20, {}}};
    ASSERT_THROWS_CODE(spiller.spill(SortOptions{}, Settings{}, std::span{data}),
                       DBException,
                       ErrorCodes::Interrupted);
}

TEST_F(ContainerBasedSpillerCallbackTest, SpillCallbacksFireAroundSpillWithHeap) {
    int preCount = 0;
    int postCount = 0;
    // spillWithHeap wraps with preSpill/postSpill but does not fire onSpill (matching existing
    // semantics; onSpill is reserved for spill() and mergeSpills()).
    auto callbacks = std::make_unique<SpillerCallbackMock>();
    EXPECT_CALL(*callbacks, preSpill).WillRepeatedly([&] { ++preCount; });
    EXPECT_CALL(*callbacks, onSpill).Times(0);
    EXPECT_CALL(*callbacks, postSpill).WillRepeatedly([&] { ++postCount; });
    auto spiller = makeContainerBasedSpiller(std::move(callbacks));

    IWComparator comp{ASC};
    std::priority_queue<std::pair<IntWrapper, NullValue>,
                        std::vector<std::pair<IntWrapper, NullValue>>,
                        Greater<IntWrapper, NullValue, IWComparator>>
        heap{Greater<IntWrapper, NullValue, IWComparator>{&comp}};
    heap.emplace(IntWrapper{10}, NullValue{});
    heap.emplace(IntWrapper{20}, NullValue{});

    auto iter = spiller.spillWithHeap(SortOptions{}, Settings{}, heap);
    ASSERT(iter);
    EXPECT_EQ(preCount, 1);
    EXPECT_EQ(postCount, 1);
}

}  // namespace
}  // namespace mongo::sorter
