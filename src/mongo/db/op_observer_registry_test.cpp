/**
 *    Copyright (C) 2017 10gen Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include "mongo/platform/basic.h"

#include "mongo/db/op_observer_registry.h"

#include "mongo/db/op_observer_noop.h"
#include "mongo/db/operation_context_noop.h"
#include "mongo/db/repl/optime.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {
int testObservers = 0;
struct TestObserver : public OpObserverNoop {
    TestObserver() {
        testObservers++;
    }
    virtual ~TestObserver() {
        testObservers--;
    }
    int drops = 0;
    repl::OpTime opTime;

    void onDropDatabase(OperationContext* opCtx, const std::string& dbName) {
        drops++;
    }
    repl::OpTime onDropCollection(OperationContext* opCtx,
                                  const NamespaceString& collectionName,
                                  OptionalCollectionUUID uuid) {
        drops++;
        OpObserver::Times::get(opCtx).reservedOpTimes.push_back(opTime);
        return {};
    }
    void onRenameCollection(OperationContext* opCtx,
                            const NamespaceString& fromCollection,
                            const NamespaceString& toCollection,
                            OptionalCollectionUUID uuid,
                            OptionalCollectionUUID dropTargetUUID,
                            bool stayTemp) {
        preRenameCollection(opCtx, fromCollection, toCollection, uuid, dropTargetUUID, stayTemp);
        postRenameCollection(opCtx, fromCollection, toCollection, uuid, dropTargetUUID, stayTemp);
    }
    repl::OpTime preRenameCollection(OperationContext* opCtx,
                                     const NamespaceString& fromCollection,
                                     const NamespaceString& toCollection,
                                     OptionalCollectionUUID uuid,
                                     OptionalCollectionUUID dropTargetUUID,
                                     bool stayTemp) {
        OpObserver::Times::get(opCtx).reservedOpTimes.push_back(opTime);
        return {};
    }
    void postRenameCollection(OperationContext* opCtx,
                              const NamespaceString& fromCollection,
                              const NamespaceString& toCollection,
                              OptionalCollectionUUID uuid,
                              OptionalCollectionUUID dropTargetUUID,
                              bool stayTemp) {}
};

struct ThrowingObserver : public TestObserver {
    void onDropDatabase(OperationContext* opCtx, const std::string& dbName) {
        drops++;
        uasserted(ErrorCodes::InternalError, "throwing observer");
    }
};

struct OpObserverRegistryTest : public unittest::Test {
    NamespaceString testNss = {"test", "coll"};
    std::unique_ptr<TestObserver> unique1 = stdx::make_unique<TestObserver>();
    std::unique_ptr<TestObserver> unique2 = stdx::make_unique<TestObserver>();
    TestObserver* observer1 = unique1.get();
    TestObserver* observer2 = unique2.get();
    OpObserverRegistry registry;
    /**
     * The 'op' function calls an observer method on the registry that returns an OpTime.
     * The method checks that the registry correctly returns only the first observer's `OpTime`.
     */
    void checkConsistentOpTime(stdx::function<repl::OpTime()> op) {
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
    void checkInconsistentOpTime(stdx::function<repl::OpTime()> op) {
        observer1->opTime = repl::OpTime(Timestamp(1, 1), 1);
        observer2->opTime = repl::OpTime(Timestamp(2, 2), 2);
        op();  // This will invariant because of inconsistent timestamps: for death test.
    }
};

TEST_F(OpObserverRegistryTest, NoObservers) {
    OperationContextNoop opCtx;
    // Check that it's OK to call observer methods with no observers registered.
    registry.onDropDatabase(&opCtx, "test");
}

TEST_F(OpObserverRegistryTest, TwoObservers) {
    OperationContextNoop opCtx;
    ASSERT_EQUALS(testObservers, 2);
    registry.addObserver(std::move(unique1));
    registry.addObserver(std::move(unique2));
    registry.onDropDatabase(&opCtx, "test");
    ASSERT_EQUALS(observer1->drops, 1);
    ASSERT_EQUALS(observer2->drops, 1);
}

TEST_F(OpObserverRegistryTest, ThrowingObserver1) {
    OperationContextNoop opCtx;
    unique1 = stdx::make_unique<ThrowingObserver>();
    observer1 = unique1.get();
    registry.addObserver(std::move(unique1));
    registry.addObserver(std::move(unique2));
    ASSERT_THROWS(registry.onDropDatabase(&opCtx, "test"), AssertionException);
    ASSERT_EQUALS(observer1->drops, 1);
    ASSERT_EQUALS(observer2->drops, 0);
}

TEST_F(OpObserverRegistryTest, ThrowingObserver2) {
    OperationContextNoop opCtx;
    unique2 = stdx::make_unique<ThrowingObserver>();
    observer2 = unique1.get();
    registry.addObserver(std::move(unique1));
    registry.addObserver(std::move(unique2));
    ASSERT_THROWS(registry.onDropDatabase(&opCtx, "test"), AssertionException);
    ASSERT_EQUALS(observer1->drops, 1);
    ASSERT_EQUALS(observer2->drops, 1);
}

TEST_F(OpObserverRegistryTest, OnDropCollectionObserverResultReturnsRightTime) {
    OperationContextNoop opCtx;
    registry.addObserver(std::move(unique1));
    registry.addObserver(std::make_unique<OpObserverNoop>());
    auto op = [&]() -> repl::OpTime { return registry.onDropCollection(&opCtx, testNss, {}); };
    checkConsistentOpTime(op);
}

TEST_F(OpObserverRegistryTest, PreRenameCollectionObserverResultReturnsRightTime) {
    OperationContextNoop opCtx;
    registry.addObserver(std::move(unique1));
    registry.addObserver(std::make_unique<OpObserverNoop>());
    auto op = [&]() -> repl::OpTime {
        auto opTime = registry.preRenameCollection(&opCtx, testNss, testNss, {}, {}, false);
        registry.postRenameCollection(&opCtx, testNss, testNss, {}, {}, false);
        return opTime;
    };
    checkConsistentOpTime(op);
}

DEATH_TEST_F(OpObserverRegistryTest, OnDropCollectionReturnsInconsistentTime, "invariant") {
    OperationContextNoop opCtx;
    registry.addObserver(std::move(unique1));
    registry.addObserver(std::move(unique2));
    auto op = [&]() -> repl::OpTime { return registry.onDropCollection(&opCtx, testNss, {}); };
    checkInconsistentOpTime(op);
}

DEATH_TEST_F(OpObserverRegistryTest, PreRenameCollectionReturnsInconsistentTime, "invariant") {
    OperationContextNoop opCtx;
    registry.addObserver(std::move(unique1));
    registry.addObserver(std::move(unique2));
    auto op = [&]() -> repl::OpTime {
        auto opTime = registry.preRenameCollection(&opCtx, testNss, testNss, {}, {}, false);
        registry.postRenameCollection(&opCtx, testNss, testNss, {}, {}, false);
        return opTime;
    };
    checkInconsistentOpTime(op);
}

}  // namespace
}  // namespace mongo
