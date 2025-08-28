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

#include "mongo/db/op_observer/op_observer_registry.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/string_data.h"
#include "mongo/db/op_observer/op_observer_noop.h"
#include "mongo/db/repl/optime.h"
#include "mongo/db/service_context_test_fixture.h"
#include "mongo/db/tenant_id.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"

#include <functional>

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {
namespace {
int testObservers = 0;
struct TestObserver : public OpObserverNoop {
    TestObserver() {
        testObservers++;
    }
    ~TestObserver() override {
        testObservers--;
    }
    int drops = 0;
    repl::OpTime opTime;

    void onDropDatabase(OperationContext* opCtx,
                        const DatabaseName& dbName,
                        bool markFromMigrate) override {
        drops++;
    }
    repl::OpTime onDropCollection(OperationContext* opCtx,
                                  const NamespaceString& collectionName,
                                  const UUID& uuid,
                                  std::uint64_t numRecords,
                                  bool markFromMigrate,
                                  bool isTimeseries) override {
        drops++;
        OpObserver::Times::get(opCtx).reservedOpTimes.push_back(opTime);
        return {};
    }
    void onRenameCollection(OperationContext* opCtx,
                            const NamespaceString& fromCollection,
                            const NamespaceString& toCollection,
                            const UUID& uuid,
                            const boost::optional<UUID>& dropTargetUUID,
                            std::uint64_t numRecords,
                            bool stayTemp,
                            bool markFromMigrate,
                            bool isTimeseries) override {
        preRenameCollection(opCtx,
                            fromCollection,
                            toCollection,
                            uuid,
                            dropTargetUUID,
                            numRecords,
                            stayTemp,
                            markFromMigrate,
                            isTimeseries);
        postRenameCollection(opCtx, fromCollection, toCollection, uuid, dropTargetUUID, stayTemp);
    }

    repl::OpTime preRenameCollection(OperationContext* opCtx,
                                     const NamespaceString& fromCollection,
                                     const NamespaceString& toCollection,
                                     const UUID& uuid,
                                     const boost::optional<UUID>& dropTargetUUID,
                                     std::uint64_t numRecords,
                                     bool stayTemp,
                                     bool markFromMigrate,
                                     bool isTimeseries) override {
        OpObserver::Times::get(opCtx).reservedOpTimes.push_back(opTime);
        return {};
    }
    void postRenameCollection(OperationContext* opCtx,
                              const NamespaceString& fromCollection,
                              const NamespaceString& toCollection,
                              const UUID& uuid,
                              const boost::optional<UUID>& dropTargetUUID,
                              bool stayTemp) override {}
};

struct ThrowingObserver : public TestObserver {
    void onDropDatabase(OperationContext* opCtx,
                        const DatabaseName& dbName,
                        bool markFromMigrate) override {
        drops++;
        uasserted(ErrorCodes::InternalError, "throwing observer");
    }
};

struct OpObserverRegistryTest : public ServiceContextTest {
    NamespaceString testNss = NamespaceString::createNamespaceString_forTest("test", "coll");
    std::unique_ptr<TestObserver> unique1 = std::make_unique<TestObserver>();
    std::unique_ptr<TestObserver> unique2 = std::make_unique<TestObserver>();
    TestObserver* observer1 = unique1.get();
    TestObserver* observer2 = unique2.get();
    OpObserverRegistry registry;

    ServiceContext::UniqueOperationContext opCtxHolder{makeOperationContext()};
    OperationContext* opCtx{opCtxHolder.get()};

    /**
     * The 'op' function calls an observer method on the registry that returns an OpTime.
     * The method checks that the registry correctly returns only the first observer's `OpTime`.
     */
    void checkConsistentOpTime(std::function<repl::OpTime()> op) {
        const repl::OpTime myTime(Timestamp(1, 1), 1);
        ASSERT(op() == repl::OpTime());
        observer1->opTime = myTime;
        ASSERT(op() == myTime);
        observer2->opTime = repl::OpTime(Timestamp(1, 1), 2);
        ASSERT(op() == myTime);
        observer1->opTime = {};
        ASSERT(op() == repl::OpTime{});
    }

    /**
     * The 'op' function calls an observer method on the registry that returns an OpTime.
     * The method checks that the registry invariants if the observers return multiple times.
     */
    void checkInconsistentOpTime(std::function<repl::OpTime()> op) {
        observer1->opTime = repl::OpTime(Timestamp(1, 1), 1);
        observer2->opTime = repl::OpTime(Timestamp(2, 2), 2);
        op();  // This will invariant because of inconsistent timestamps: for death test.
    }
};

TEST_F(OpObserverRegistryTest, NoObservers) {
    // Check that it's OK to call observer methods with no observers registered.
    registry.onDropDatabase(opCtx,
                            DatabaseName::createDatabaseName_forTest(boost::none, "test"),
                            false /*markFromMigrate*/);
}

TEST_F(OpObserverRegistryTest, TwoObservers) {
    ASSERT_EQUALS(testObservers, 2);
    registry.addObserver(std::move(unique1));
    registry.addObserver(std::move(unique2));
    registry.onDropDatabase(opCtx,
                            DatabaseName::createDatabaseName_forTest(boost::none, "test"),
                            false /*markFromMigrate*/);
    ASSERT_EQUALS(observer1->drops, 1);
    ASSERT_EQUALS(observer2->drops, 1);
}

TEST_F(OpObserverRegistryTest, ThrowingObserver1) {
    unique1 = std::make_unique<ThrowingObserver>();
    observer1 = unique1.get();
    registry.addObserver(std::move(unique1));
    registry.addObserver(std::move(unique2));
    ASSERT_THROWS(
        registry.onDropDatabase(opCtx,
                                DatabaseName::createDatabaseName_forTest(boost::none, "test"),
                                false /*markFromMigrate*/),
        AssertionException);
    ASSERT_EQUALS(observer1->drops, 1);
    ASSERT_EQUALS(observer2->drops, 0);
}

TEST_F(OpObserverRegistryTest, ThrowingObserver2) {
    unique2 = std::make_unique<ThrowingObserver>();
    observer2 = unique1.get();
    registry.addObserver(std::move(unique1));
    registry.addObserver(std::move(unique2));
    ASSERT_THROWS(
        registry.onDropDatabase(opCtx,
                                DatabaseName::createDatabaseName_forTest(boost::none, "test"),
                                false /*markFromMigrate*/),
        AssertionException);
    ASSERT_EQUALS(observer1->drops, 1);
    ASSERT_EQUALS(observer2->drops, 1);
}

TEST_F(OpObserverRegistryTest, OnDropCollectionObserverResultReturnsRightTime) {
    registry.addObserver(std::move(unique1));
    registry.addObserver(std::make_unique<OpObserverNoop>());
    auto op = [&]() -> repl::OpTime {
        return registry.onDropCollection(opCtx,
                                         testNss,
                                         UUID::gen(),
                                         0U,
                                         /*markFromMigrate=*/false,
                                         /*isTimeseries=*/false);
    };
    checkConsistentOpTime(op);
}

TEST_F(OpObserverRegistryTest, PreRenameCollectionObserverResultReturnsRightTime) {
    registry.addObserver(std::move(unique1));
    registry.addObserver(std::make_unique<OpObserverNoop>());
    auto op = [&]() -> repl::OpTime {
        UUID uuid = UUID::gen();
        auto opTime = registry.preRenameCollection(opCtx,
                                                   testNss,
                                                   testNss,
                                                   uuid,
                                                   {},
                                                   0U,
                                                   /*stayTemp=*/false,
                                                   /*markFromMigrate=*/false,
                                                   /*isTimeseries=*/false);
        registry.postRenameCollection(opCtx, testNss, testNss, uuid, {}, false);
        return opTime;
    };
    checkConsistentOpTime(op);
}

DEATH_TEST_F(OpObserverRegistryTest, OnDropCollectionReturnsInconsistentTime, "invariant") {
    registry.addObserver(std::move(unique1));
    registry.addObserver(std::move(unique2));
    auto op = [&]() -> repl::OpTime {
        return registry.onDropCollection(opCtx,
                                         testNss,
                                         UUID::gen(),
                                         0U,
                                         /*markFromMigrate=*/false,
                                         /*isTimeseries=*/false);
    };
    checkInconsistentOpTime(op);
}

DEATH_TEST_F(OpObserverRegistryTest, PreRenameCollectionReturnsInconsistentTime, "invariant") {
    registry.addObserver(std::move(unique1));
    registry.addObserver(std::move(unique2));
    auto op = [&]() -> repl::OpTime {
        UUID uuid = UUID::gen();
        auto opTime = registry.preRenameCollection(opCtx,
                                                   testNss,
                                                   testNss,
                                                   uuid,
                                                   {},
                                                   0U,
                                                   /*stayTemp=*/false,
                                                   /*markFromMigrate=*/false,
                                                   /*isTimeseries=*/false);
        registry.postRenameCollection(opCtx, testNss, testNss, uuid, {}, false);
        return opTime;
    };
    checkInconsistentOpTime(op);
}

}  // namespace
}  // namespace mongo
