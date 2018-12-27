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

#include "mongo/platform/basic.h"

#include "mongo/db/transaction_coordinator_futures_util.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

TEST(TransactionCoordinatorFuturesUtilTest, CollectReturnsInitValueWhenInputIsEmptyVector) {
    std::vector<Future<int>> futures;
    auto resultFuture = txn::collect(std::move(futures), 0, [](int& result, const int& next) {
        result = 20;
        return txn::ShouldStopIteration::kNo;
    });

    ASSERT_EQ(resultFuture.get(), 0);
}

TEST(TransactionCoordinatorFuturesUtilTest, CollectReturnsOnlyResultWhenOnlyOneFuture) {
    std::vector<Future<int>> futures;
    auto pf = makePromiseFuture<int>();
    futures.push_back(std::move(pf.future));
    auto resultFuture = txn::collect(std::move(futures), 0, [](int& result, const int& next) {
        result = next;
        return txn::ShouldStopIteration::kNo;
    });
    pf.promise.emplaceValue(3);

    ASSERT_EQ(resultFuture.get(), 3);
}

TEST(TransactionCoordinatorFuturesUtilTest, CollectReturnsCombinedResultWithSeveralInputFutures) {
    std::vector<Future<int>> futures;
    std::vector<Promise<int>> promises;
    std::vector<int> futureValues;
    for (int i = 0; i < 5; ++i) {
        auto pf = makePromiseFuture<int>();
        futures.push_back(std::move(pf.future));
        promises.push_back(std::move(pf.promise));
        futureValues.push_back(i);
    }

    // Sum all of the inputs.
    auto resultFuture = txn::collect(std::move(futures), 0, [](int& result, const int& next) {
        result += next;
        return txn::ShouldStopIteration::kNo;
    });

    for (size_t i = 0; i < promises.size(); ++i) {
        promises[i].emplaceValue(futureValues[i]);
    }

    // Result should be the sum of all the values emplaced into the promises.
    ASSERT_EQ(resultFuture.get(), std::accumulate(futureValues.begin(), futureValues.end(), 0));
}

}  // namespace
}  // namespace mongo
