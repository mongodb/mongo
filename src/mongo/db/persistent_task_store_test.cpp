/**
 *    Copyright (C) 2019-present MongoDB, Inc.
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

#include "mongo/platform/basic.h"

#include "mongo/db/catalog/catalog_test_fixture.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/persistent_task_store.h"
#include "mongo/db/s/collection_sharding_runtime.h"

namespace mongo {
namespace {

const NamespaceString kNss{"test.foo"};

struct TestTask {
    std::string key;
    int min;
    int max;

    TestTask() : min(0), max(std::numeric_limits<int>::max()) {}
    TestTask(std::string key, int min = 0, int max = std::numeric_limits<int>::max())
        : key(std::move(key)), min(min), max(max) {}
    TestTask(BSONObj bson)
        : key(bson.getField("key").String()),
          min(bson.getField("min").Int()),
          max(bson.getField("max").Int()) {}

    static TestTask parse(IDLParserContext, BSONObj bson) {
        return TestTask{bson};
    }

    void serialize(BSONObjBuilder& builder) const {
        builder.append("key", key);
        builder.append("min", min);
        builder.append("max", max);
    }

    BSONObj toBSON() const {
        BSONObjBuilder builder;
        serialize(builder);
        return builder.obj();
    }
};

class PersistentTaskStoreTest : public CatalogTestFixture {
    void setUp() override {
        CatalogTestFixture::setUp();
        auto opCtx = operationContext();

        AutoGetDb autoDb(opCtx, kNss.dbName(), MODE_IX);
        Lock::CollectionLock collLock(opCtx, kNss, MODE_IX);
    }
};

TEST_F(PersistentTaskStoreTest, TestAdd) {
    auto opCtx = operationContext();

    PersistentTaskStore<TestTask> store(kNss);

    store.add(opCtx, TestTask{"one", 0, 10});
    store.add(opCtx, TestTask{"two", 10, 20});
    store.add(opCtx, TestTask{"three", 40, 50});

    ASSERT_EQ(store.count(opCtx), 3);
}

TEST_F(PersistentTaskStoreTest, TestForEach) {
    auto opCtx = operationContext();

    PersistentTaskStore<TestTask> store(kNss);

    store.add(opCtx, TestTask{"one", 0, 10});
    store.add(opCtx, TestTask{"two", 10, 20});
    store.add(opCtx, TestTask{"three", 40, 50});

    ASSERT_EQ(store.count(opCtx), 3);

    // No match.
    int count = 0;
    store.forEach(opCtx,
                  BSON("key"
                       << "four"),
                  [&count](const TestTask& t) {
                      ++count;
                      return true;
                  });
    ASSERT_EQ(count, 0);

    // Multiple matches.
    count = 0;
    store.forEach(opCtx, BSON("min" << GTE << 10), [&count](const TestTask& t) {
        ++count;
        return true;
    });
    ASSERT_EQ(count, 2);

    // Multiple matches, only take one.
    count = 0;
    store.forEach(opCtx, BSON("min" << GTE << 10), [&count](const TestTask& t) {
        ++count;
        return count < 1;
    });
    ASSERT_EQ(count, 1);

    // Single match.
    count = 0;
    store.forEach(opCtx,
                  BSON("key"
                       << "one"),
                  [&count](const TestTask& t) {
                      ++count;
                      return true;
                  });
    ASSERT_EQ(count, 1);
}

TEST_F(PersistentTaskStoreTest, TestRemove) {
    auto opCtx = operationContext();

    PersistentTaskStore<TestTask> store(kNss);

    store.add(opCtx, TestTask{"one", 0, 10});
    store.add(opCtx, TestTask{"two", 10, 20});
    store.add(opCtx, TestTask{"three", 40, 50});

    ASSERT_EQ(store.count(opCtx), 3);

    store.remove(opCtx,
                 BSON("key"
                      << "one"));

    ASSERT_EQ(store.count(opCtx), 2);
}

TEST_F(PersistentTaskStoreTest, TestRemoveMultiple) {
    auto opCtx = operationContext();

    PersistentTaskStore<TestTask> store(kNss);

    store.add(opCtx, TestTask{"one", 0, 10});
    store.add(opCtx, TestTask{"two", 10, 20});
    store.add(opCtx, TestTask{"three", 40, 50});

    ASSERT_EQ(store.count(opCtx), 3);

    // Remove multipe overlapping ranges.
    store.remove(opCtx, BSON("min" << GTE << 10));

    ASSERT_EQ(store.count(opCtx), 1);
}

TEST_F(PersistentTaskStoreTest, TestUpdate) {
    auto opCtx = operationContext();

    PersistentTaskStore<TestTask> store(kNss);
    int originalMin = 0;
    int expectedUpdatedMin = 1;
    store.add(opCtx, TestTask{"one", originalMin, 10});
    store.add(opCtx, TestTask{"two", 10, 20});
    store.add(opCtx, TestTask{"three", 40, 50});

    ASSERT_EQ(store.count(opCtx), 3);

    store.update(opCtx,
                 BSON("key"
                      << "one"),
                 BSON("$inc" << BSON("min" << 1)));

    store.forEach(opCtx,
                  BSON("key"
                       << "one"),
                  [&](const TestTask& task) {
                      ASSERT_EQ(task.min, expectedUpdatedMin);
                      return false;
                  });
}

TEST_F(PersistentTaskStoreTest, TestUpdateOnlyUpdatesOneMatchingDocument) {
    auto opCtx = operationContext();

    PersistentTaskStore<TestTask> store(kNss);
    int originalMin = 0;
    int expectedUpdatedMin = 1;
    std::string keyToMatch = "one";
    store.add(opCtx, TestTask{keyToMatch, originalMin, 10});
    store.add(opCtx, TestTask{keyToMatch, originalMin, 20});
    store.add(opCtx, TestTask{"three", 40, 50});

    // Update query will match two documents but should only update one of them.
    store.update(opCtx, BSON("key" << keyToMatch), BSON("$inc" << BSON("min" << 1)));

    ASSERT_EQ(store.count(opCtx, BSON("key" << keyToMatch << "min" << expectedUpdatedMin)), 1);
}

TEST_F(PersistentTaskStoreTest, TestUpsert) {
    auto opCtx = operationContext();

    PersistentTaskStore<TestTask> store(kNss);

    std::string keyToMatch = "foo";
    auto query = BSON("key" << keyToMatch);

    TestTask task(keyToMatch, 0, 0);
    BSONObj taskBson = task.toBSON();

    ASSERT_EQ(store.count(opCtx, query), 0);

    // Test that an attempt to upsert from the update command throws an error.
    ASSERT_THROWS_CODE(
        store.update(opCtx, query, taskBson, WriteConcerns::kMajorityWriteConcernShardingTimeout),
        DBException,
        ErrorCodes::NoMatchingDocument);

    // Test that the document is created when upserted.
    store.upsert(opCtx, query, taskBson, WriteConcerns::kMajorityWriteConcernShardingTimeout);

    ASSERT_EQ(store.count(opCtx, query), 1);

    // Verify that the task document is actually correct
    store.forEach(opCtx, query, [&](const TestTask& t) {
        ASSERT_EQ(t.toBSON().toString(), task.toBSON().toString());
        return true;
    });

    // Verify that updates happen as expected with upsert and update
    store.upsert(opCtx,
                 query,
                 BSON("$inc" << BSON("min" << 1)),
                 WriteConcerns::kMajorityWriteConcernShardingTimeout);
    store.forEach(opCtx, query, [&](const TestTask& t) {
        ASSERT_EQ(t.min, 1);
        return true;
    });

    store.update(opCtx,
                 query,
                 BSON("$inc" << BSON("min" << 1)),
                 WriteConcerns::kMajorityWriteConcernShardingTimeout);
    store.forEach(opCtx, query, [&](const TestTask& t) {
        ASSERT_EQ(t.min, 2);
        return true;
    });
}

TEST_F(PersistentTaskStoreTest, TestWritesPersistAcrossInstances) {
    auto opCtx = operationContext();

    {
        PersistentTaskStore<TestTask> store(kNss);

        store.add(opCtx, TestTask{"one", 0, 10});
        store.add(opCtx, TestTask{"two", 10, 20});
        store.add(opCtx, TestTask{"three", 40, 50});

        ASSERT_EQ(store.count(opCtx), 3);
    }

    {
        PersistentTaskStore<TestTask> store(kNss);
        ASSERT_EQ(store.count(opCtx), 3);

        auto count = store.count(opCtx, BSON("min" << GTE << 10));
        ASSERT_EQ(count, 2);

        store.remove(opCtx,
                     BSON("key"
                          << "two"));
        ASSERT_EQ(store.count(opCtx), 2);

        count = store.count(opCtx, BSON("min" << GTE << 10));
        ASSERT_EQ(count, 1);
    }

    {
        PersistentTaskStore<TestTask> store(kNss);
        ASSERT_EQ(store.count(opCtx), 2);

        auto count = store.count(opCtx, BSON("min" << GTE << 10));
        ASSERT_EQ(count, 1);
    }
}

TEST_F(PersistentTaskStoreTest, TestCountWithQuery) {
    auto opCtx = operationContext();

    PersistentTaskStore<TestTask> store(kNss);

    store.add(opCtx, TestTask{"one", 0, 10});
    store.add(opCtx, TestTask{"two", 10, 20});
    store.add(opCtx, TestTask{"two", 40, 50});

    ASSERT_EQ(store.count(opCtx,
                          BSON("key"
                               << "two")),
              2);

    // Remove multipe overlapping ranges.
    store.remove(opCtx, BSON("min" << 10));

    ASSERT_EQ(store.count(opCtx,
                          BSON("key"
                               << "two")),
              1);
}

}  // namespace
}  // namespace mongo
