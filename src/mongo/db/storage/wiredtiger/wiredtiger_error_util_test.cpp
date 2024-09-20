/**
 *    Copyright (C) 2024-present MongoDB, Inc.
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

#include "mongo/db/storage/wiredtiger/wiredtiger_error_util.h"
#include "mongo/unittest/assert.h"
#include "mongo/unittest/framework.h"

namespace mongo {
namespace {
class WiredTigerUtilHelperTest : public unittest::Test {
public:
    struct TestCase {
        bool txnTooLarge;
        bool tempUnavailable;
        bool cacheSufficiency;
        const char* reason;
    };

    void throwsExpectedException(TestCase testCase, ErrorCodes::Error err) {
        try {
            throwAppropriateException(testCase.txnTooLarge,
                                      testCase.tempUnavailable,
                                      testCase.cacheSufficiency,
                                      testCase.reason,
                                      prefix,
                                      retCode);
        } catch (DBException& ex) {
            ASSERT_EQ(ex.code(), err)
                << "expected " << ErrorCodes::errorString(err)
                << " error for txnTooLargeEnabled: " << testCase.txnTooLarge
                << ", temporarilyUnavailableEnabled: " << testCase.tempUnavailable
                << ", cacheIsInsufficientForTransaction: " << testCase.cacheSufficiency
                << ", and rollback reason: " << testCase.reason;
        }
    }

    bool txnTooLargeEnabled{true};
    bool temporarilyUnavailableEnabled{true};
    bool disabled{false};

    bool insufficient{true};
    bool sufficient{false};

    const char* reasonEviction = WT_TXN_ROLLBACK_REASON_OLDEST_FOR_EVICTION;
    const char* reasonOverflow = WT_TXN_ROLLBACK_REASON_CACHE_OVERFLOW;
    const char* noReason = "";

    StringData prefix = "";
    int retCode = 0;
};

TEST_F(WiredTigerUtilHelperTest, transactionExceededCacheThreshold) {
    ASSERT_TRUE(txnExceededCacheThreshold(1, 2, 0.4));
    ASSERT_FALSE(txnExceededCacheThreshold(1, 2, 0.6));
    ASSERT_FALSE(txnExceededCacheThreshold(1, 2, 0.5));
}

TEST_F(WiredTigerUtilHelperTest, rollbackReasonWasCachePressure) {
    auto randomReason = "random";

    ASSERT_FALSE(rollbackReasonWasCachePressure(noReason));
    ASSERT_FALSE(rollbackReasonWasCachePressure(randomReason));

    ASSERT_TRUE(rollbackReasonWasCachePressure(reasonEviction));
    ASSERT_TRUE(rollbackReasonWasCachePressure(reasonOverflow));
}

TEST_F(WiredTigerUtilHelperTest, throwTransactionTooLargeForCacheException) {
    // Throws TransactionTooLargeForCacheException only when TransactionTooLargeForCache is enabled,
    // cache is insufficient for transaction, and reason for rollback was cache pressure. Also
    // throws TransactionTooLargeForCacheException even if temporarilyUnavailableEnabled is enabled.
    std::vector<TestCase> transactionTooLargeTestCases = {
        {txnTooLargeEnabled, disabled, insufficient, reasonEviction},
        {txnTooLargeEnabled, disabled, insufficient, reasonOverflow},
        {txnTooLargeEnabled, temporarilyUnavailableEnabled, insufficient, reasonEviction},
        {txnTooLargeEnabled, temporarilyUnavailableEnabled, insufficient, reasonOverflow},
    };

    for (auto testCase : transactionTooLargeTestCases) {
        throwsExpectedException(testCase, ErrorCodes::TransactionTooLargeForCache);
    }
}

TEST_F(WiredTigerUtilHelperTest, throwTemporarilyUnavailableException) {
    std::vector<TestCase> temporarilyUnavailableTestCases = {
        // If both or one of txnTooLargeEnabled and cacheIsInsufficientForTransaction are false,
        // throws TemporarilyUnavailableException if it is enabled and rollback reason was cache
        // pressure.
        {disabled, temporarilyUnavailableEnabled, sufficient, reasonEviction},
        {disabled, temporarilyUnavailableEnabled, sufficient, reasonOverflow},
        {disabled, temporarilyUnavailableEnabled, insufficient, reasonEviction},
        {disabled, temporarilyUnavailableEnabled, insufficient, reasonOverflow},
        {txnTooLargeEnabled, temporarilyUnavailableEnabled, sufficient, reasonEviction},
        {txnTooLargeEnabled, temporarilyUnavailableEnabled, sufficient, reasonOverflow},
    };

    for (auto testCase : temporarilyUnavailableTestCases) {
        throwsExpectedException(testCase, ErrorCodes::TemporarilyUnavailable);
    }
}

TEST_F(WiredTigerUtilHelperTest, throwAppropriateException) {
    std::vector<TestCase> writeConflictExceptionTestCases = {
        // Throws WCE if reason for rollback was not cache pressure.
        {txnTooLargeEnabled, disabled, insufficient, noReason},
        {txnTooLargeEnabled, disabled, sufficient, noReason},
        {txnTooLargeEnabled, temporarilyUnavailableEnabled, insufficient, noReason},
        {txnTooLargeEnabled, temporarilyUnavailableEnabled, sufficient, noReason},
        {disabled, temporarilyUnavailableEnabled, insufficient, noReason},
        {disabled, temporarilyUnavailableEnabled, sufficient, noReason},
        {disabled, disabled, insufficient, noReason},
        {disabled, disabled, sufficient, noReason},

        // Throws WCE if cache is sufficient for transaction no matter the reason for rollback if
        // temporarilyUnavailable is disabled.
        {txnTooLargeEnabled, disabled, sufficient, reasonEviction},
        {txnTooLargeEnabled, disabled, sufficient, reasonOverflow},

        // Throws WCE whether cache is sufficient or insufficient for transaction no matter the
        // reason for rollback if both TransactionTooLargeForCache and temporarilyUnavailable are
        // disabled.
        {disabled, disabled, sufficient, reasonEviction},
        {disabled, disabled, sufficient, reasonOverflow},
        {disabled, disabled, insufficient, reasonOverflow},
        {disabled, disabled, insufficient, reasonEviction},
    };

    for (auto testCase : writeConflictExceptionTestCases) {
        throwsExpectedException(testCase, ErrorCodes::WriteConflict);
    }
}
}  // namespace
}  // namespace mongo
