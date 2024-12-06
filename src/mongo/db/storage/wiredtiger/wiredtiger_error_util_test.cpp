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
        bool cacheInsufficient;
        const char* reason;
    };

    void throwsWriteConflictException(TestCase testCase, ErrorCodes::Error err) {
        ASSERT_THROWS_CODE(throwAppropriateException(testCase.txnTooLarge,
                                                     testCase.tempUnavailable,
                                                     session,
                                                     cacheThreshold,
                                                     testCase.reason,
                                                     prefix,
                                                     retCode),
                           StorageUnavailableException,
                           err)
            << "Expected " << ErrorCodes::errorString(err)
            << " error for txnTooLargeEnabled: " << testCase.txnTooLarge
            << ", temporarilyUnavailableEnabled: " << testCase.tempUnavailable
            << ", and rollback reason: " << testCase.reason;
    }

    void throwsCachePressureException(TestCase testCase, ErrorCodes::Error err) {
        ASSERT_THROWS_CODE(throwCachePressureExceptionIfAppropriate(testCase.txnTooLarge,
                                                                    testCase.tempUnavailable,
                                                                    testCase.cacheInsufficient,
                                                                    testCase.reason,
                                                                    prefix,
                                                                    retCode),
                           StorageUnavailableException,
                           err)
            << "Expected " << ErrorCodes::errorString(err)
            << " error for txnTooLargeEnabled: " << testCase.txnTooLarge
            << ", temporarilyUnavailableEnabled: " << testCase.tempUnavailable
            << ", cacheIsInsufficientForTransaction: " << testCase.cacheInsufficient
            << ", and rollback reason: " << testCase.reason;
    }

    const char* reasonEviction = WT_TXN_ROLLBACK_REASON_OLDEST_FOR_EVICTION;
    const char* reasonOverflow = WT_TXN_ROLLBACK_REASON_CACHE_OVERFLOW;
    const char* noReason = "";

    StringData prefix = "";
    int retCode = 0;
    double cacheThreshold = 0;
    WT_SESSION* session = nullptr;
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
        {.txnTooLarge = true,
         .tempUnavailable = false,
         .cacheInsufficient = true,
         .reason = reasonEviction},
        {.txnTooLarge = true,
         .tempUnavailable = false,
         .cacheInsufficient = true,
         .reason = reasonOverflow},
        {.txnTooLarge = true,
         .tempUnavailable = true,
         .cacheInsufficient = true,
         .reason = reasonEviction},
        {.txnTooLarge = true,
         .tempUnavailable = true,
         .cacheInsufficient = true,
         .reason = reasonOverflow},
    };

    for (auto testCase : transactionTooLargeTestCases) {
        throwsCachePressureException(testCase, ErrorCodes::TransactionTooLargeForCache);
    }
}

TEST_F(WiredTigerUtilHelperTest, throwTemporarilyUnavailableException) {
    std::vector<TestCase> temporarilyUnavailableTestCases = {
        // If both or one of txnTooLargeEnabled and cacheIsInsufficientForTransaction are false,
        // throws TemporarilyUnavailableException if it is enabled and rollback reason was cache
        // pressure.
        {.txnTooLarge = false,
         .tempUnavailable = true,
         .cacheInsufficient = false,
         .reason = reasonEviction},
        {.txnTooLarge = false,
         .tempUnavailable = true,
         .cacheInsufficient = false,
         .reason = reasonOverflow},
        {.txnTooLarge = false,
         .tempUnavailable = true,
         .cacheInsufficient = true,
         .reason = reasonEviction},
        {.txnTooLarge = false,
         .tempUnavailable = true,
         .cacheInsufficient = true,
         .reason = reasonOverflow},
        {.txnTooLarge = true,
         .tempUnavailable = true,
         .cacheInsufficient = false,
         .reason = reasonEviction},
        {.txnTooLarge = true,
         .tempUnavailable = true,
         .cacheInsufficient = false,
         .reason = reasonOverflow},
    };

    for (auto testCase : temporarilyUnavailableTestCases) {
        throwsCachePressureException(testCase, ErrorCodes::TemporarilyUnavailable);
    }
}

TEST_F(WiredTigerUtilHelperTest, throwWriteConflictException) {
    std::vector<TestCase> writeConflictExceptionTestCases = {
        // Throws WCE if reason for rollback was not cache pressure.
        {.txnTooLarge = true,
         .tempUnavailable = false,
         .cacheInsufficient = false /*ignored*/,
         .reason = noReason},
        {.txnTooLarge = true,
         .tempUnavailable = true,
         .cacheInsufficient = false /*ignored*/,
         .reason = noReason},
        {.txnTooLarge = false,
         .tempUnavailable = true,
         .cacheInsufficient = false /*ignored*/,
         .reason = noReason},
        {.txnTooLarge = false,
         .tempUnavailable = false,
         .cacheInsufficient = false /*ignored*/,
         .reason = noReason},

        // Throws WCE for transaction no matter the reason for rollback if both
        // TransactionTooLargeForCache and temporarilyUnavailable are disabled.
        {.txnTooLarge = false,
         .tempUnavailable = false,
         .cacheInsufficient = false /*ignored*/,
         .reason = reasonEviction},
        {.txnTooLarge = false,
         .tempUnavailable = false,
         .cacheInsufficient = false /*ignored*/,
         .reason = reasonOverflow},
        {.txnTooLarge = false,
         .tempUnavailable = false,
         .cacheInsufficient = false /*ignored*/,
         .reason = reasonOverflow},
        {.txnTooLarge = false,
         .tempUnavailable = false,
         .cacheInsufficient = false /*ignored*/,
         .reason = reasonEviction},
    };

    for (auto testCase : writeConflictExceptionTestCases) {
        throwsWriteConflictException(testCase, ErrorCodes::WriteConflict);
    }
}

TEST_F(WiredTigerUtilHelperTest, doesNotThrowCachePressureException) {
    std::vector<TestCase> notCachePressureExceptionTestCases = {
        // Does not throw cache pressure exception if cache is sufficient for transaction no matter
        // the reason for rollback if temporarilyUnavailable is disabled.
        {.txnTooLarge = true,
         .tempUnavailable = false,
         .cacheInsufficient = false,
         .reason = reasonEviction},
        {.txnTooLarge = true,
         .tempUnavailable = false,
         .cacheInsufficient = false,
         .reason = reasonOverflow},
    };

    for (auto testCase : notCachePressureExceptionTestCases) {
        ASSERT_DOES_NOT_THROW(throwCachePressureExceptionIfAppropriate(testCase.txnTooLarge,
                                                                       testCase.tempUnavailable,
                                                                       testCase.cacheInsufficient,
                                                                       testCase.reason,
                                                                       prefix,
                                                                       retCode));
    }
}
}  // namespace
}  // namespace mongo
