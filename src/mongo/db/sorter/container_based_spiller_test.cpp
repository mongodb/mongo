/**
 *    Copyright (C) 2026-present MongoDB, Inc.
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

#include "mongo/db/sorter/container_based_spiller.h"

#include "mongo/db/namespace_string.h"
#include "mongo/db/repl/replication_coordinator_mock.h"
#include "mongo/db/service_context_d_test_fixture.h"
#include "mongo/db/shard_role/shard_catalog/collection_mock.h"
#include "mongo/db/shard_role/transaction_resources.h"
#include "mongo/db/sorter/sorter_test_utils.h"
#include "mongo/db/storage/container.h"
#include "mongo/db/storage/ident.h"
#include "mongo/db/storage/recovery_unit_noop.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"

#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace mongo::sorter {
namespace {

class ViewableIntegerKeyedContainer final : public IntegerKeyedContainer {
public:
    using Entry = std::pair<int64_t, std::string>;

    class Cursor : public IntegerKeyedContainer::Cursor {
    public:
        explicit Cursor(const std::vector<ViewableIntegerKeyedContainer::Entry>& entries)
            : _entries(entries) {}

        boost::optional<std::span<const char>> find(int64_t key) override {
            for (auto&& [entryKey, entryValue] : _entries) {
                if (entryKey == key) {
                    return {{entryValue.data(), entryValue.size()}};
                }
            }
            return boost::none;
        }

    private:
        const std::vector<ViewableIntegerKeyedContainer::Entry>& _entries;
    };

    ViewableIntegerKeyedContainer() = default;

    explicit ViewableIntegerKeyedContainer(std::shared_ptr<Ident> ident)
        : _ident(std::move(ident)) {}

    std::shared_ptr<Ident> ident() const override {
        return _ident;
    }

    void setIdent(std::shared_ptr<Ident> ident) override {
        _ident = std::move(ident);
    }

    Status insert(RecoveryUnit&, int64_t key, std::span<const char> value) override {
        _entries.emplace_back(key, std::string(value.begin(), value.end()));
        return Status::OK();
    }

    Status remove(RecoveryUnit&, int64_t key) override {
        _entries.erase(std::find_if(_entries.begin(), _entries.end(), [key](const Entry& entry) {
            return entry.first == key;
        }));
        return Status::OK();
    }

    std::unique_ptr<IntegerKeyedContainer::Cursor> getCursor(RecoveryUnit&) const override {
        return std::make_unique<Cursor>(_entries);
    }

    std::shared_ptr<IntegerKeyedContainer::Cursor> getSharedCursor(RecoveryUnit&) const override {
        return std::make_shared<Cursor>(_entries);
    }

    const std::vector<Entry>& entries() const {
        return _entries;
    }

private:
    std::shared_ptr<Ident> _ident;
    std::vector<Entry> _entries;
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

    ASSERT_OK(container.insert(
        ru, containerKey1, {containerValue1.buf(), static_cast<size_t>(containerValue1.len())}));
    ASSERT_OK(container.insert(
        ru, containerKey2, {containerValue2.buf(), static_cast<size_t>(containerValue2.len())}));
    ASSERT_OK(container.insert(
        ru, containerKey3, {containerValue3.buf(), static_cast<size_t>(containerValue3.len())}));
    ASSERT_OK(container.insert(
        ru, containerKey4, {containerValue4.buf(), static_cast<size_t>(containerValue4.len())}));
    ASSERT_OK(container.insert(
        ru, containerKey5, {containerValue5.buf(), static_cast<size_t>(containerValue5.len())}));

    ContainerIterator<IntWrapper, IntWrapper> iterator{container.getSharedCursor(ru),
                                                       containerKey1,
                                                       containerKey5 + 1,
                                                       Iterator<IntWrapper, IntWrapper>::Settings{},
                                                       /*_checksumCalculator=*/3272515249,
                                                       SorterChecksumVersion::v2};

    ASSERT_TRUE(iterator.more());
    auto next = iterator.next();
    EXPECT_EQ(next.first, key1);
    EXPECT_EQ(next.second, value1);

    ASSERT_TRUE(iterator.more());
    next = iterator.next();
    EXPECT_EQ(next.first, key2);
    EXPECT_EQ(next.second, value2);

    ASSERT_TRUE(iterator.more());
    EXPECT_EQ(iterator.nextWithDeferredValue(), key3);
    EXPECT_EQ(iterator.getDeferredValue(), value3);

    ASSERT_TRUE(iterator.more());
    EXPECT_EQ(iterator.nextWithDeferredValue(), key4);
    EXPECT_EQ(iterator.getDeferredValue(), value4);

    ASSERT_TRUE(iterator.more());
    next = iterator.next();
    EXPECT_EQ(next.first, key5);
    EXPECT_EQ(next.second, value5);

    EXPECT_FALSE(iterator.more());
}

TEST(ContainerIteratorTest, SharedCursor) {
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

    ASSERT_OK(container.insert(
        ru, containerKey1, {containerValue1.buf(), static_cast<size_t>(containerValue1.len())}));
    ASSERT_OK(container.insert(
        ru, containerKey2, {containerValue2.buf(), static_cast<size_t>(containerValue2.len())}));
    ASSERT_OK(container.insert(
        ru, containerKey3, {containerValue3.buf(), static_cast<size_t>(containerValue3.len())}));
    ASSERT_OK(container.insert(
        ru, containerKey4, {containerValue4.buf(), static_cast<size_t>(containerValue4.len())}));

    auto cursor = container.getSharedCursor(ru);
    ContainerIterator<IntWrapper, IntWrapper> iterator1{
        cursor,
        containerKey1,
        containerKey2 + 1,
        Iterator<IntWrapper, IntWrapper>::Settings{},
        /*_checksumCalculator=*/71873048,
        SorterChecksumVersion::v2};
    ContainerIterator<IntWrapper, IntWrapper> iterator2{
        cursor,
        containerKey3,
        containerKey4 + 1,
        Iterator<IntWrapper, IntWrapper>::Settings{},
        /*_checksumCalculator=*/2815298670,
        SorterChecksumVersion::v2};

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

    ContainerIterator<IntWrapper, IntWrapper> iterator{container.getSharedCursor(ru),
                                                       containerKey1,
                                                       containerKey1 + 1,
                                                       Iterator<IntWrapper, IntWrapper>::Settings{},
                                                       /*_checksumCalculator=*/0,
                                                       SorterChecksumVersion::v2};

    ASSERT_TRUE(iterator.more());
    EXPECT_THROW(iterator.next(), DBException);
    EXPECT_THROW(iterator.nextWithDeferredValue(), DBException);

    ASSERT_OK(container.insert(
        ru, containerKey1, {containerValue1.buf(), static_cast<size_t>(containerValue1.len())}));
    EXPECT_EQ(iterator.nextWithDeferredValue(), key1);

    ASSERT_OK(container.remove(ru, containerKey1));
    EXPECT_THROW(iterator.getDeferredValue(), DBException);
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

    ASSERT_OK(container.insert(
        ru, containerKey1, {containerValue1.buf(), static_cast<size_t>(containerValue1.len())}));

    ContainerIterator<IntWrapper, IntWrapper> iterator{container.getSharedCursor(ru),
                                                       containerKey1,
                                                       containerKey1 + 1,
                                                       Iterator<IntWrapper, IntWrapper>::Settings{},
                                                       /*_checksumCalculator=*/0,
                                                       SorterChecksumVersion::v2};

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

    ASSERT_OK(container.insert(
        ru, containerKey1, {containerValue1.buf(), static_cast<size_t>(containerValue1.len())}));

    auto cursor = container.getSharedCursor(ru);
    ContainerIterator<IntWrapper, IntWrapper> iterator{cursor,
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

    ASSERT_OK(container.insert(
        ru, containerKey1, {containerValue1.buf(), static_cast<size_t>(containerValue1.len())}));

    auto cursor = container.getSharedCursor(ru);
    ContainerIterator<IntWrapper, IntWrapper> iterator{cursor,
                                                       containerKey1,
                                                       containerKey1 + 1,
                                                       Iterator<IntWrapper, IntWrapper>::Settings{},
                                                       /*_checksumCalculator=*/0,
                                                       SorterChecksumVersion::v2};

    ASSERT_TRUE(iterator.more());
    iterator.next();
}

class ContainerIteratorTest : public testing::TestWithParam<SorterChecksumVersion> {};

INSTANTIATE_TEST_SUITE_P(ContainerIteratorTestSuite,
                         ContainerIteratorTest,
                         testing::Values(SorterChecksumVersion::v1, SorterChecksumVersion::v2));

TEST_P(ContainerIteratorTest, EmptyIteratorHasZeroChecksum) {
    RecoveryUnitNoop ru;
    ViewableIntegerKeyedContainer container;

    auto cursor = container.getSharedCursor(ru);
    ContainerIterator<IntWrapper, IntWrapper> iterator{cursor,
                                                       /*start=*/0,
                                                       /*end=*/1,
                                                       Iterator<IntWrapper, IntWrapper>::Settings{},
                                                       /*_checksumCalculator=*/0,
                                                       ContainerIteratorTest::GetParam()};
}

class SortedContainerWriterTest : public ServiceContextMongoDTest {
public:
    SorterTracker sorterTracker;

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
    auto coll = std::make_shared<CollectionMock>(
        NamespaceString::createNamespaceString_forTest("test", "coll"));
    CollectionPtr collPtr(coll.get());

    SortOptions opts;
    const int64_t startingKey = 5;
    auto& ru = *shard_role_details::getRecoveryUnit(opCtx.get());
    SorterContainerStats stats(&this->sorterTracker);
    SortedContainerWriter<IntWrapper, IntWrapper> writer(
        *opCtx,
        ru,
        collPtr,
        container,
        stats,
        opts,
        startingKey,
        SortedContainerWriter<IntWrapper, IntWrapper>::Settings{});

    const IntWrapper k1{1};
    const IntWrapper v1{2};
    const IntWrapper k2{3};
    const IntWrapper v2{4};
    writer.addAlreadySorted(k1, v1);
    writer.addAlreadySorted(k2, v2);

    ASSERT_EQ(container.entries().size(), 2U);
    ASSERT_EQ(container.entries()[0].first, startingKey);
    ASSERT_EQ(container.entries()[1].first, startingKey + 1);

    BufBuilder expected;
    expected.reset();
    k1.serializeForSorter(expected);
    v1.serializeForSorter(expected);
    ASSERT_EQ(container.entries()[0].second, std::string(expected.buf(), expected.len()));

    expected.reset();
    k2.serializeForSorter(expected);
    v2.serializeForSorter(expected);
    ASSERT_EQ(container.entries()[1].second, std::string(expected.buf(), expected.len()));

    exhaustIterators<IntWrapper, IntWrapper>(writer);
}

TEST_F(SortedContainerWriterTest, ContainerWriterStoresEmptyValueForZeroLengthSerialization) {
    auto opCtx = makeOperationContext();
    auto* replCoord = repl::ReplicationCoordinator::get(opCtx.get());
    auto* replCoordMock = dynamic_cast<repl::ReplicationCoordinatorMock*>(replCoord);
    ASSERT(replCoordMock);
    replCoordMock->alwaysAllowWrites(true);

    ViewableIntegerKeyedContainer container;
    container.setIdent(std::make_shared<Ident>("sorted_container_writer_empty_test"));
    auto coll = std::make_shared<CollectionMock>(
        NamespaceString::createNamespaceString_forTest("test", "coll"));
    CollectionPtr collPtr(coll.get());

    SortOptions opts;
    const int64_t startingKey = 29;
    auto& ru = *shard_role_details::getRecoveryUnit(opCtx.get());
    SorterContainerStats stats(&this->sorterTracker);
    SortedContainerWriter<NullValue, NullValue> writer(
        *opCtx,
        ru,
        collPtr,
        container,
        stats,
        opts,
        startingKey,
        SortedContainerWriter<NullValue, NullValue>::Settings{});
    writer.addAlreadySorted(NullValue{}, NullValue{});

    ASSERT_EQ(container.entries().size(), 1U);
    ASSERT_EQ(container.entries()[0].first, startingKey);
    ASSERT_TRUE(container.entries()[0].second.empty());

    exhaustIterators<NullValue, NullValue>(writer);
}

TEST_F(SortedContainerWriterTest, ContainerWriterAllowsNullValueWithNonNullKey) {
    auto opCtx = makeOperationContext();
    auto* replCoord = repl::ReplicationCoordinator::get(opCtx.get());
    auto* replCoordMock = dynamic_cast<repl::ReplicationCoordinatorMock*>(replCoord);
    ASSERT(replCoordMock);
    replCoordMock->alwaysAllowWrites(true);

    ViewableIntegerKeyedContainer container;
    container.setIdent(std::make_shared<Ident>("sorted_container_writer_null_value_test"));
    auto coll = std::make_shared<CollectionMock>(
        NamespaceString::createNamespaceString_forTest("test", "coll"));
    CollectionPtr collPtr(coll.get());

    SortOptions opts;
    const int64_t startingKey = 2002;
    auto& ru = *shard_role_details::getRecoveryUnit(opCtx.get());
    SorterContainerStats stats(&this->sorterTracker);
    SortedContainerWriter<IntWrapper, NullValue> writer(
        *opCtx,
        ru,
        collPtr,
        container,
        stats,
        opts,
        startingKey,
        SortedContainerWriter<IntWrapper, NullValue>::Settings{});

    const IntWrapper key{123};
    writer.addAlreadySorted(key, NullValue{});

    ASSERT_EQ(container.entries().size(), 1U);
    ASSERT_EQ(container.entries()[0].first, startingKey);

    BufBuilder expected;
    key.serializeForSorter(expected);
    NullValue{}.serializeForSorter(expected);
    ASSERT_EQ(container.entries()[0].second, std::string(expected.buf(), expected.len()));

    exhaustIterators<IntWrapper, NullValue>(writer);
}

class ContainerBasedSpillerTest : public ServiceContextMongoDTest {};

TEST_F(ContainerBasedSpillerTest, Spill) {
    auto opCtx = makeOperationContext();

    auto replCoord = dynamic_cast<repl::ReplicationCoordinatorMock*>(
        repl::ReplicationCoordinator::get(opCtx.get()));
    ASSERT(replCoord);
    replCoord->alwaysAllowWrites(true);

    auto ns = NamespaceString::createNamespaceString_forTest("test", "container_based_spiller");
    CollectionMock coll{ns};
    CollectionPtr collPtr{&coll};
    ViewableIntegerKeyedContainer container{std::make_shared<Ident>("ident")};
    SorterContainerStats stats{nullptr};

    ContainerBasedSpiller<IntWrapper, NullValue> spiller{
        *opCtx,
        *shard_role_details::getRecoveryUnit(opCtx.get()),
        collPtr,
        container,
        stats,
        ns.dbName(),
        SorterChecksumVersion::v2};

    std::vector<std::pair<IntWrapper, NullValue>> data{{50, {}}, {100, {}}, {75, {}}, {125, {}}};
    std::span span{data};

    auto it1 = spiller.spill(
        SortOptions{}, SorterSpiller<IntWrapper, NullValue>::Settings{}, span.subspan(0, 2), 0);
    auto it2 = spiller.spill(
        SortOptions{}, SorterSpiller<IntWrapper, NullValue>::Settings{}, span.subspan(2, 2), 0);

    ASSERT_TRUE(it1->more());
    EXPECT_EQ(it1->next().first, 50);
    ASSERT_TRUE(it1->more());
    EXPECT_EQ(it1->next().first, 100);

    ASSERT_TRUE(it2->more());
    EXPECT_EQ(it2->next().first, 75);
    ASSERT_TRUE(it2->more());
    EXPECT_EQ(it2->next().first, 125);
}

}  // namespace
}  // namespace mongo::sorter
