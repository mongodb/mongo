// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/storage/container.h"

#include "mongo/db/storage/record_store.h"
#include "mongo/db/storage/record_store_test_harness.h"
#include "mongo/unittest/unittest.h"

#include <functional>
#include <string>

#include <boost/range/combine.hpp>

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


template <typename Container, typename Key>
void runContainerTestWithBatchedInserts(KeyFormat keyFormat,
                                        std::span<const Key> keysBatch1,
                                        std::span<const Key> keysBatch2) {
    auto harnessHelper = newRecordStoreHarnessHelper();
    auto rs = harnessHelper->newRecordStore("test.container",
                                            RecordStore::Options{.keyFormat = keyFormat});
    auto& container = std::get<std::reference_wrapper<Container>>(rs->getContainer()).get();

    auto opCtx = harnessHelper->newOperationContext();
    auto& ru = *shard_role_details::getRecoveryUnit(opCtx.get());

    // Tests that pass must insert batches of 2 keys only.
    const std::vector<std::string> valuesBatch1{"v1", "v2"};
    const std::vector<std::span<const char>> valuesBatch1Views(valuesBatch1.begin(),
                                                               valuesBatch1.end());

    {
        StorageWriteTransaction txn(ru);
        ASSERT_OK(container.insert(
            ru, keysBatch1, valuesBatch1Views, container::ExistingKeyPolicy::reject))
            << "Failed to insert into empty container";
        txn.commit();

        auto cursor = container.getCursor(ru);
        for (auto&& [key, value] : boost::combine(keysBatch1, valuesBatch1)) {
            const auto found = cursor->find(key);
            ASSERT_TRUE(found) << "Failed to find key that should have been inserted";
            EXPECT_EQ(std::string(found->data(), found->size()), value)
                << "Read back key does not match written value";
        }
    }
    {
        StorageWriteTransaction txn(ru);
        ASSERT_NOT_OK(container.insert(
            ru, keysBatch1, valuesBatch1Views, container::ExistingKeyPolicy::reject))
            << "Expected rejection of existing keys";
        txn.abort();
    }


    const std::vector<std::string> valuesBatch1Overwrite{"vv1", "vv2"};
    const std::vector<std::span<const char>> valuesBatch1OverwriteViews(
        valuesBatch1Overwrite.begin(), valuesBatch1Overwrite.end());
    {
        StorageWriteTransaction txn(ru);
        ASSERT_OK(container.insert(
            ru, keysBatch1, valuesBatch1OverwriteViews, container::ExistingKeyPolicy::overwrite))
            << "Expected overwrite of existing keys";
        txn.abort();
    }

    const std::vector<std::string> valuesBatch2{"w2", "w3"};
    const std::vector<std::span<const char>> valuesBatch2Views(valuesBatch2.begin(),
                                                               valuesBatch2.end());
    {
        StorageWriteTransaction txn(ru);
        ASSERT_NOT_OK(container.insert(
            ru, keysBatch2, valuesBatch2Views, container::ExistingKeyPolicy::reject))
            << "Expected rejection of a batch that partially overlaps existing keys";
        txn.abort();

        auto cursor = container.getCursor(ru);
        const auto overlapping = cursor->find(keysBatch2.front());
        ASSERT_TRUE(overlapping) << "Failed to find key that should have been inserted";
        EXPECT_EQ(std::string(overlapping->data(), overlapping->size()), valuesBatch1.back())
            << "Rejected batch must leave the existing value untouched";
        ASSERT_FALSE(cursor->find(keysBatch2.back()))
            << "Rejected batch must not insert its new key";
    }
    {
        StorageWriteTransaction txn(ru);
        ASSERT_OK(container.insert(
            ru, keysBatch2, valuesBatch2Views, container::ExistingKeyPolicy::overwrite))
            << "Expected overwrite of a batch that partially overlaps existing keys";
        txn.commit();

        auto cursor = container.getCursor(ru);
        for (auto&& [key, value] : boost::combine(keysBatch2, valuesBatch2)) {
            const auto found = cursor->find(key);
            ASSERT_TRUE(found) << "Failed to find key that should have been inserted";
            EXPECT_EQ(std::string(found->data(), found->size()), value)
                << "Read back key does not match written value";
        }
        const auto untouched = cursor->find(keysBatch1.front());
        ASSERT_TRUE(untouched) << "Failed to find key that should have been inserted";
        EXPECT_EQ(std::string(untouched->data(), untouched->size()), valuesBatch1.front())
            << "Overwriting batch must leave keys outside the batch untouched";
    }
}

TEST(ContainerTest, IntegerKeyedContainerWithBatchedInserts) {
    const std::vector<int64_t> batch1Keys{1, 2};
    const std::vector<int64_t> batch2Keys{2, 3};
    runContainerTestWithBatchedInserts<IntegerKeyedContainer, int64_t>(
        KeyFormat::Long, batch1Keys, batch2Keys);
}

TEST(ContainerTest, StringKeyedContainerWithBatchedInserts) {
    const std::vector<std::span<const char>> batch1Keys{"k1", "k2"};
    const std::vector<std::span<const char>> batch2Keys{"k2", "k3"};
    runContainerTestWithBatchedInserts<StringKeyedContainer, std::span<const char>>(
        KeyFormat::String, batch1Keys, batch2Keys);
}

TEST(ContainerTest, RangeBasedContainerWritesMustHaveEqualSpansIntegerKeyed) {
    const std::vector<int64_t> keys{1, 2, 3};
    ASSERT_THROWS((runContainerTestWithBatchedInserts<IntegerKeyedContainer, int64_t>(
                      KeyFormat::Long, keys, keys)),
                  DBException);
}

TEST(ContainerTest, RangeBasedContainerWritesMustHaveEqualSpansStringKeyed) {
    const std::vector<std::span<const char>> keys{"k1", "k2", "k3"};
    ASSERT_THROWS((runContainerTestWithBatchedInserts<StringKeyedContainer, std::span<const char>>(
                      KeyFormat::String, keys, keys)),
                  DBException);
}

}  // namespace
}  // namespace mongo
