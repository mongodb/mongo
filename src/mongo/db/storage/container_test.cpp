// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/storage/container.h"

#include "mongo/db/storage/record_store.h"
#include "mongo/db/storage/record_store_test_harness.h"
#include "mongo/unittest/unittest.h"

#include <functional>
#include <string>

namespace mongo {
namespace {

template <typename Key>
void expectKeyEq(Key actual, Key expected) {
    if constexpr (std::is_same_v<Key, int64_t>) {
        EXPECT_EQ(actual, expected);
    } else if constexpr (std::is_same_v<Key, std::span<const char>>) {
        EXPECT_EQ(std::string(actual.data(), actual.size()),
                  std::string(expected.data(), expected.size()));
    } else {
        FAIL("Unexpected key type");
    }
}

template <typename Container, typename Key>
void runContainerTest(KeyFormat keyFormat, Key key1, Key key2) {
    auto harnessHelper = newRecordStoreHarnessHelper();
    auto rs = harnessHelper->newRecordStore("test.container",
                                            RecordStore::Options{.keyFormat = keyFormat});
    auto& container = std::get<std::reference_wrapper<Container>>(rs->getContainer()).get();

    auto opCtx = harnessHelper->newOperationContext();
    auto& ru = *shard_role_details::getRecoveryUnit(opCtx.get());

    std::string value1{"v1"};
    std::string value2{"v2"};

    {
        StorageWriteTransaction txn(ru);
        ASSERT_OK(container.insert(ru, key1, value1, container::ExistingKeyPolicy::reject));
        txn.commit();

        auto cursor = container.getCursor(ru);
        auto found1 = cursor->find(key1);
        ASSERT_TRUE(found1);
        EXPECT_EQ(std::string(found1->data(), found1->size()), value1);
        auto found2 = cursor->find(key2);
        EXPECT_FALSE(found2);
    }
    {
        StorageWriteTransaction txn(ru);
        ASSERT_OK(container.insert(ru, key2, value2, container::ExistingKeyPolicy::reject));
        txn.commit();

        auto cursor = container.getCursor(ru);
        auto found1 = cursor->find(key1);
        ASSERT_TRUE(found1);
        EXPECT_EQ(std::string(found1->data(), found1->size()), value1);
        auto found2 = cursor->find(key2);
        ASSERT_TRUE(found2);
        EXPECT_EQ(std::string(found2->data(), found2->size()), value2);
    }
    {
        auto cursor = container.getCursor(ru);
        auto next = cursor->next();
        ASSERT_TRUE(next);
        expectKeyEq(next->first, key1);
        EXPECT_EQ(std::string(next->second.data(), next->second.size()), value1);
        next = cursor->next();
        ASSERT_TRUE(next);
        expectKeyEq(next->first, key2);
        EXPECT_EQ(std::string(next->second.data(), next->second.size()), value2);
        next = cursor->next();
        EXPECT_FALSE(next);
        next = cursor->next();
        EXPECT_FALSE(next);
    }
    {
        auto cursor = container.getCursor(ru);
        auto found = cursor->find(key1);
        ASSERT_TRUE(found);
        EXPECT_EQ(std::string(found->data(), found->size()), value1);
        auto next = cursor->next();
        ASSERT_TRUE(next);
        expectKeyEq(next->first, key2);
        EXPECT_EQ(std::string(next->second.data(), next->second.size()), value2);
        next = cursor->next();
        EXPECT_FALSE(next);
        next = cursor->next();
        EXPECT_FALSE(next);
    }
    {
        StorageWriteTransaction txn(ru);
        ASSERT_NOT_OK(container.insert(ru, key1, value2, container::ExistingKeyPolicy::reject));
        txn.commit();

        auto cursor = container.getCursor(ru);
        auto found1 = cursor->find(key1);
        ASSERT_TRUE(found1);
        EXPECT_EQ(std::string(found1->data(), found1->size()), value1);
    }
    {
        StorageWriteTransaction txn(ru);
        ASSERT_OK(container.insert(ru, key1, value2, container::ExistingKeyPolicy::overwrite));
        txn.commit();

        auto cursor = container.getCursor(ru);
        auto found1 = cursor->find(key1);
        ASSERT_TRUE(found1);
        EXPECT_EQ(std::string(found1->data(), found1->size()), value2);
    }
    {
        StorageWriteTransaction txn(ru);
        ASSERT_OK(container.remove(ru, key1));
        txn.commit();

        auto cursor = container.getCursor(ru);
        auto found1 = cursor->find(key1);
        EXPECT_FALSE(found1);
        auto found2 = cursor->find(key2);
        ASSERT_TRUE(found2);
        EXPECT_EQ(std::string(found2->data(), found2->size()), value2);
    }
}

TEST(ContainerTest, IntegerKeyedContainer) {
    runContainerTest<IntegerKeyedContainer, int64_t>(KeyFormat::Long, 1, 2);
}

TEST(ContainerTest, StringKeyedContainer) {
    runContainerTest<StringKeyedContainer, std::span<const char>>(KeyFormat::String, "k1", "k2");
}

}  // namespace
}  // namespace mongo
