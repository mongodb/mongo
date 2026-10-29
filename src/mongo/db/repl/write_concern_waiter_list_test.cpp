// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/repl/write_concern_waiter_list.h"

#include "mongo/base/counter.h"
#include "mongo/base/error_codes.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/repl/optime.h"
#include "mongo/db/write_concern_options.h"
#include "mongo/unittest/unittest.h"

#include <vector>

namespace mongo {
namespace repl {
namespace {

// Builds a distinct, hashable numeric write concern. Different `w` values hash/compare distinct.
WriteConcernOptions makeWriteConcern(int w) {
    return WriteConcernOptions(
        w, WriteConcernOptions::SyncMode::NONE, WriteConcernOptions::kNoTimeout);
}

OpTime makeOpTime(int seconds) {
    return OpTime(Timestamp(seconds, 0), /*term=*/1);
}

// add(), remove() and setValueIf() are all synchronous, so no draining is needed between an
// operation and asserting its effect.
class WriteConcernWaiterListTest : public unittest::Test {
public:
    void setUp() override {
        _wcWaiterList = std::make_unique<WriteConcernWaiterList>(_waiterCount);
    }

    void tearDown() override {
        if (_wcWaiterList) {
            // Fail any waiters a test left behind so their promises are not broken on destruction.
            _wcWaiterList->setErrorAll({ErrorCodes::ShutdownInProgress, "teardown"});
            _wcWaiterList.reset();
        }
    }

    long long waiterCount() const {
        return _waiterCount.get();
    }

    Counter64 _waiterCount;
    std::unique_ptr<WriteConcernWaiterList> _wcWaiterList;
};

TEST_F(WriteConcernWaiterListTest, NewListIsEmpty) {
    ASSERT_EQ(0, waiterCount());
    ASSERT(_wcWaiterList->getWriteConcerns()->empty());
}

TEST_F(WriteConcernWaiterListTest, AddIsSynchronous) {
    auto wc = makeWriteConcern(2);
    auto [future, waiter] = _wcWaiterList->add(makeOpTime(10), wc);
    ASSERT_EQ(1u, _wcWaiterList->getWriteConcerns()->size());
    ASSERT_EQ(1, waiterCount());
    ASSERT_FALSE(future.isReady());
}

TEST_F(WriteConcernWaiterListTest, FulfillmentWithOpTimeWakesPrefix) {
    auto wc = makeWriteConcern(2);
    auto [f5, w5] = _wcWaiterList->add(makeOpTime(5), wc);
    auto [f10, w10] = _wcWaiterList->add(makeOpTime(10), wc);
    auto [f15, w15] = _wcWaiterList->add(makeOpTime(15), wc);

    // Threshold at opTime 10 wakes the waiters at 5 and 10, but not 15.
    WriteConcernFulfillmentMap map;
    map[wc] = makeOpTime(10);
    _wcWaiterList->setValueIf(map);

    ASSERT_OK(f5.getNoThrow());
    ASSERT_OK(f10.getNoThrow());
    ASSERT_FALSE(f15.isReady());
    ASSERT_EQ(1, waiterCount());
    ASSERT_EQ(1u, _wcWaiterList->getWriteConcerns()->size());
}

TEST_F(WriteConcernWaiterListTest, FulfillmentWithBoolTrueWakesAllAbsentWakesNone) {
    auto wc = makeWriteConcern(3);
    auto [f1, w1] = _wcWaiterList->add(makeOpTime(5), wc);
    auto [f2, w2] = _wcWaiterList->add(makeOpTime(50), wc);

    // A write concern absent from the map wakes none of its waiters (false is never stored).
    _wcWaiterList->setValueIf(WriteConcernFulfillmentMap{});
    ASSERT_FALSE(f1.isReady());
    ASSERT_FALSE(f2.isReady());
    ASSERT_EQ(2, waiterCount());

    // true wakes all regardless of opTime.
    WriteConcernFulfillmentMap allMap;
    allMap[wc] = true;
    _wcWaiterList->setValueIf(allMap);
    ASSERT_OK(f1.getNoThrow());
    ASSERT_OK(f2.getNoThrow());
    ASSERT_EQ(0, waiterCount());
}

TEST_F(WriteConcernWaiterListTest, FulfillmentWithStatusFailsAll) {
    auto wc = makeWriteConcern(2);
    auto [f1, w1] = _wcWaiterList->add(makeOpTime(5), wc);
    auto [f2, w2] = _wcWaiterList->add(makeOpTime(6), wc);

    WriteConcernFulfillmentMap map;
    map[wc] = Status{ErrorCodes::PrimarySteppedDown, "stepped down"};
    _wcWaiterList->setValueIf(map);

    ASSERT_EQ(ErrorCodes::PrimarySteppedDown, f1.getNoThrow().code());
    ASSERT_EQ(ErrorCodes::PrimarySteppedDown, f2.getNoThrow().code());
    ASSERT_EQ(0, waiterCount());
}

TEST_F(WriteConcernWaiterListTest, TwoFulfillmentsAppliedInEitherOrderWakeCorrectly) {
    // Two fulfillment maps applied in either order wake the right waiters (monotonic thresholds).
    auto wc = makeWriteConcern(2);
    auto [f5, w5] = _wcWaiterList->add(makeOpTime(5), wc);
    auto [f15, w15] = _wcWaiterList->add(makeOpTime(15), wc);

    WriteConcernFulfillmentMap m1;
    m1[wc] = makeOpTime(5);
    WriteConcernFulfillmentMap m2;
    m2[wc] = makeOpTime(15);

    // Applying the later threshold first wakes both waiters.
    _wcWaiterList->setValueIf(m2);
    ASSERT_OK(f5.getNoThrow());
    ASSERT_OK(f15.getNoThrow());
    ASSERT_EQ(0, waiterCount());

    // The earlier threshold is then a no-op -- there is nothing left for it to wake.
    _wcWaiterList->setValueIf(m1);
    ASSERT_EQ(0, waiterCount());
}

TEST_F(WriteConcernWaiterListTest, FulfillmentWakesLargeWaiterList) {
    auto wc = makeWriteConcern(2);
    constexpr int kN = 300;
    std::vector<SharedSemiFuture<void>> futures;
    for (int i = 1; i <= kN; ++i) {
        auto [f, w] = _wcWaiterList->add(makeOpTime(i), wc);
        futures.push_back(std::move(f));
    }
    WriteConcernFulfillmentMap map;
    map[wc] = makeOpTime(kN);  // threshold wakes all kN waiters
    _wcWaiterList->setValueIf(map);
    for (auto& f : futures) {
        ASSERT_OK(f.getNoThrow());
    }
    ASSERT_EQ(0, waiterCount());
}

TEST_F(WriteConcernWaiterListTest, FulfillmentOnlyAffectsMatchingWriteConcern) {
    auto wcA = makeWriteConcern(2);
    auto wcB = makeWriteConcern(3);
    auto [fa, wa] = _wcWaiterList->add(makeOpTime(5), wcA);
    auto [fb, wb] = _wcWaiterList->add(makeOpTime(5), wcB);

    WriteConcernFulfillmentMap map;
    map[wcA] = true;  // only wake wcA
    _wcWaiterList->setValueIf(map);

    ASSERT_OK(fa.getNoThrow());
    ASSERT_FALSE(fb.isReady());
    ASSERT_EQ(1, waiterCount());
}

TEST_F(WriteConcernWaiterListTest, NumWaitersCountsOnlyTheGivenWriteConcern) {
    auto wc2 = makeWriteConcern(2);
    auto wc3 = makeWriteConcern(3);

    // A WriteConcern the list has never seen has no waiters, and asking must not create a bucket
    // for it.
    ASSERT_EQ(0u, _wcWaiterList->numWaiters(wc2));
    ASSERT(_wcWaiterList->getWriteConcerns()->empty());

    auto [f5, w5] = _wcWaiterList->add(makeOpTime(5), wc2);
    auto [f10, w10] = _wcWaiterList->add(makeOpTime(10), wc2);
    auto [f15, w15] = _wcWaiterList->add(makeOpTime(15), wc3);

    ASSERT_EQ(2u, _wcWaiterList->numWaiters(wc2));
    ASSERT_EQ(1u, _wcWaiterList->numWaiters(wc3));

    // Once the last waiter of a WriteConcern is woken its bucket stays in the list, so
    // getWriteConcerns() still names it -- but it now has no waiters, which is what lets a caller
    // skip resolving it.
    WriteConcernFulfillmentMap map;
    map[wc2] = makeOpTime(10);
    _wcWaiterList->setValueIf(map);
    ASSERT_OK(f5.getNoThrow());
    ASSERT_OK(f10.getNoThrow());

    ASSERT_EQ(0u, _wcWaiterList->numWaiters(wc2));
    ASSERT_EQ(2u, _wcWaiterList->getWriteConcerns()->size());
    ASSERT_EQ(1u, _wcWaiterList->numWaiters(wc3));
}

TEST_F(WriteConcernWaiterListTest, RemoveReturnsWhetherFound) {
    auto wc = makeWriteConcern(2);
    auto [future, waiter] = _wcWaiterList->add(makeOpTime(10), wc);

    ASSERT_TRUE(_wcWaiterList->remove(makeOpTime(10), waiter));
    ASSERT_EQ(0, waiterCount());
    // The waiter's own promise is left untouched by remove.
    ASSERT_FALSE(future.isReady());

    // Removing again returns false.
    ASSERT_FALSE(_wcWaiterList->remove(makeOpTime(10), waiter));
}

TEST_F(WriteConcernWaiterListTest, RemovedWaiterIsNotWokenByLaterFulfillment) {
    auto wc = makeWriteConcern(2);
    auto [future, waiter] = _wcWaiterList->add(makeOpTime(10), wc);
    ASSERT_TRUE(_wcWaiterList->remove(makeOpTime(10), waiter));

    WriteConcernFulfillmentMap map;
    map[wc] = true;
    _wcWaiterList->setValueIf(map);

    ASSERT_FALSE(future.isReady());
}

TEST_F(WriteConcernWaiterListTest, SetErrorAllFailsEveryWaiter) {
    auto wcA = makeWriteConcern(2);
    auto wcB = makeWriteConcern(3);
    auto [fa, wa] = _wcWaiterList->add(makeOpTime(5), wcA);
    auto [fb, wb] = _wcWaiterList->add(makeOpTime(6), wcB);

    _wcWaiterList->setErrorAll({ErrorCodes::ShutdownInProgress, "shutting down"});

    ASSERT_EQ(ErrorCodes::ShutdownInProgress, fa.getNoThrow().code());
    ASSERT_EQ(ErrorCodes::ShutdownInProgress, fb.getNoThrow().code());
    ASSERT_EQ(0, waiterCount());
}

TEST_F(WriteConcernWaiterListTest, PromiseFulfilledExactlyOnce) {
    auto wc = makeWriteConcern(2);
    auto [future, waiter] = _wcWaiterList->add(makeOpTime(10), wc);

    // Fulfill via setValueIf, then setErrorAll (targeting the already-removed waiter) is a no-op.
    WriteConcernFulfillmentMap map;
    map[wc] = true;
    _wcWaiterList->setValueIf(map);
    _wcWaiterList->setErrorAll({ErrorCodes::PrimarySteppedDown, "later"});

    ASSERT_OK(future.getNoThrow());
    ASSERT_EQ(0, waiterCount());
}

}  // namespace
}  // namespace repl
}  // namespace mongo
