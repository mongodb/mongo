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

#include "mongo/db/op_observer_registry.h"
#include "mongo/db/op_observer_noop.h"
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
        return opTime;
    }
    repl::OpTime onRenameCollection(OperationContext* opCtx,
                                    const NamespaceString& fromCollection,
                                    const NamespaceString& toCollection,
                                    OptionalCollectionUUID uuid,
                                    bool dropTarget,
                                    OptionalCollectionUUID dropTargetUUID,
                                    bool stayTemp) {
        return opTime;
    }
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
     * The method checks that the registry correctly merges the results of the registered observers.
     */
    void checkConsistentOpTime(stdx::function<repl::OpTime()> op) {
        const repl::OpTime myTime(Timestamp(1, 1), 1);
        ASSERT(op() == repl::OpTime());
        observer1->opTime = myTime;
        ASSERT(op() == myTime);
        observer2->opTime = myTime;
        ASSERT(op() == myTime);
        observer1->opTime = {};
        ASSERT(op() == myTime);
    }

    /**
     * The 'op' function calls an observer method on the registry that returns an OpTime.
     * The method checks that the registry invariants if the observers return conflicting times.
     */
    void checkInconsistentOpTime(stdx::function<repl::OpTime()> op) {
        observer1->opTime = repl::OpTime(Timestamp(1, 1), 1);
        observer2->opTime = repl::OpTime(Timestamp(2, 2), 2);
        op();  // This will invariant because of inconsistent timestamps: for death test.
    }
};

TEST_F(OpObserverRegistryTest, NoObservers) {
    // Check that it's OK to call observer methods with no observers registered.
    registry.onDropDatabase(nullptr, "test");
}

TEST_F(OpObserverRegistryTest, TwoObservers) {
    ASSERT_EQUALS(testObservers, 2);
    registry.addObserver(std::move(unique1));
    registry.addObserver(std::move(unique2));
    registry.onDropDatabase(nullptr, "test");
    ASSERT_EQUALS(observer1->drops, 1);
    ASSERT_EQUALS(observer2->drops, 1);
}

TEST_F(OpObserverRegistryTest, ThrowingObserver1) {
    unique1 = stdx::make_unique<ThrowingObserver>();
    observer1 = unique1.get();
    registry.addObserver(std::move(unique1));
    registry.addObserver(std::move(unique2));
    ASSERT_THROWS(registry.onDropDatabase(nullptr, "test"), AssertionException);
    ASSERT_EQUALS(observer1->drops, 1);
    ASSERT_EQUALS(observer2->drops, 0);
}

TEST_F(OpObserverRegistryTest, ThrowingObserver2) {
    unique2 = stdx::make_unique<ThrowingObserver>();
    observer2 = unique1.get();
    registry.addObserver(std::move(unique1));
    registry.addObserver(std::move(unique2));
    ASSERT_THROWS(registry.onDropDatabase(nullptr, "test"), AssertionException);
    ASSERT_EQUALS(observer1->drops, 1);
    ASSERT_EQUALS(observer2->drops, 1);
}

TEST_F(OpObserverRegistryTest, OnDropCollectionObserverResultReturnsRightTime) {
    registry.addObserver(std::move(unique1));
    registry.addObserver(std::move(unique2));
    auto op = [&]() -> repl::OpTime { return registry.onDropCollection(nullptr, testNss, {}); };
    checkConsistentOpTime(op);
}

TEST_F(OpObserverRegistryTest, OnRenameCollectionObserverResultReturnsRightTime) {
    registry.addObserver(std::move(unique1));
    registry.addObserver(std::move(unique2));
    auto op = [&]() -> repl::OpTime {
        return registry.onRenameCollection(nullptr, testNss, testNss, {}, false, {}, false);
    };
    checkConsistentOpTime(op);
}

DEATH_TEST_F(OpObserverRegistryTest, OnDropCollectionReturnsInconsistentTime, "invariant") {
    registry.addObserver(std::move(unique1));
    registry.addObserver(std::move(unique2));
    auto op = [&]() -> repl::OpTime { return registry.onDropCollection(nullptr, testNss, {}); };
    checkInconsistentOpTime(op);
}

DEATH_TEST_F(OpObserverRegistryTest, OnRenameCollectionReturnsInconsistentTime, "invariant") {
    registry.addObserver(std::move(unique1));
    registry.addObserver(std::move(unique2));
    auto op = [&]() -> repl::OpTime {
        return registry.onRenameCollection(nullptr, testNss, testNss, {}, false, {}, false);
    };
    checkInconsistentOpTime(op);
}

}  // namespace
}  // namespace mongo
