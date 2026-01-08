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

#include "mongo/db/storage/container.h"

#include "mongo/db/storage/record_store.h"
#include "mongo/db/storage/record_store_test_harness.h"
#include "mongo/unittest/unittest.h"

#include <functional>
#include <string>

namespace mongo {
namespace {

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
        ASSERT_OK(container.insert(ru, key1, value1));
        txn.commit();

        auto cursor = container.getCursor(ru);
        auto found1 = cursor->find(key1);
        ASSERT_TRUE(found1);
        ASSERT_EQ(std::string(found1->data(), found1->size()), value1);
        auto found2 = cursor->find(key2);
        ASSERT_FALSE(found2);
    }
    {
        StorageWriteTransaction txn(ru);
        ASSERT_OK(container.insert(ru, key2, value2));
        txn.commit();

        auto cursor = container.getCursor(ru);
        auto found1 = cursor->find(key1);
        ASSERT_TRUE(found1);
        ASSERT_EQ(std::string(found1->data(), found1->size()), value1);
        auto found2 = cursor->find(key2);
        ASSERT_TRUE(found2);
        ASSERT_EQ(std::string(found2->data(), found2->size()), value2);
    }
    {
        StorageWriteTransaction txn(ru);
        ASSERT_OK(container.remove(ru, key1));
        txn.commit();

        auto cursor = container.getCursor(ru);
        auto found1 = cursor->find(key1);
        ASSERT_FALSE(found1);
        auto found2 = cursor->find(key2);
        ASSERT_TRUE(found2);
        ASSERT_EQ(std::string(found2->data(), found2->size()), value2);
    }
}

TEST(ContainerTest, IntegerKeyedContainer) {
    runContainerTest<IntegerKeyedContainer, int64_t>(KeyFormat::Long, 1, 2);
}

TEST(ContainerTest, StringKeyedContainer) {
    runContainerTest<StringKeyedContainer, std::string>(KeyFormat::String, "k1", "k2");
}

}  // namespace
}  // namespace mongo
