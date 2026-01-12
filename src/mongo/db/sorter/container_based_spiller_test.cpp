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

    Status remove(RecoveryUnit&, int64_t) override {
        return Status::OK();
    }

    std::unique_ptr<IntegerKeyedContainer::Cursor> getCursor(RecoveryUnit&) const override {
        return nullptr;
    }

    std::shared_ptr<IntegerKeyedContainer::Cursor> getSharedCursor(RecoveryUnit&) const override {
        return nullptr;
    }

    const std::vector<Entry>& entries() const {
        return _entries;
    }

private:
    std::shared_ptr<Ident> _ident;
    std::vector<Entry> _entries;
};

class SortedContainerWriterTest : public ServiceContextMongoDTest {
public:
    SorterTracker sorterTracker;
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
        *opCtx, ru, collPtr, container, stats, opts, startingKey);

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
        *opCtx, ru, collPtr, container, stats, opts, startingKey);
    writer.addAlreadySorted(NullValue{}, NullValue{});

    ASSERT_EQ(container.entries().size(), 1U);
    ASSERT_EQ(container.entries()[0].first, startingKey);
    ASSERT_TRUE(container.entries()[0].second.empty());
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
        *opCtx, ru, collPtr, container, stats, opts, startingKey);

    const IntWrapper key{123};
    writer.addAlreadySorted(key, NullValue{});

    ASSERT_EQ(container.entries().size(), 1U);
    ASSERT_EQ(container.entries()[0].first, startingKey);

    BufBuilder expected;
    key.serializeForSorter(expected);
    NullValue{}.serializeForSorter(expected);
    ASSERT_EQ(container.entries()[0].second, std::string(expected.buf(), expected.len()));
}

}  // namespace
}  // namespace mongo::sorter
