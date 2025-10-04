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

#include "mongo/db/storage/exceptions.h"
#include "mongo/logv2/log.h"
#include "mongo/unittest/unittest.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest
namespace mongo {
namespace {
class WiredTigerUtilHelperTest : public unittest::Test {
public:
    struct TestCase {
        bool txnTooLarge;
        bool cacheInsufficient;
        int sub_level_err;
    };

    void throwsWriteConflictException(TestCase testCase, ErrorCodes::Error err) {
        ASSERT_THROWS_CODE(throwAppropriateException(
                               testCase.txnTooLarge, session, cacheThreshold, prefix, retCode),
                           StorageUnavailableException,
                           err)
            << "Expected " << ErrorCodes::errorString(err)
            << " error for txnTooLargeEnabled: " << testCase.txnTooLarge
            << ", and sub_level_err: " << subLevelErrorToString(testCase.sub_level_err);
    }

    void throwsCachePressureException(TestCase testCase, ErrorCodes::Error err) {
        ASSERT_THROWS_CODE(
            throwCachePressureExceptionIfAppropriate(
                testCase.txnTooLarge, testCase.cacheInsufficient, reason, prefix, retCode),
            StorageUnavailableException,
            err)
            << "Expected " << ErrorCodes::errorString(err)
            << " error for txnTooLargeEnabled: " << testCase.txnTooLarge
            << ", cacheIsInsufficientForTransaction: " << testCase.cacheInsufficient
            << ", and sub_level_err: " << subLevelErrorToString(testCase.sub_level_err);
    }

    std::string subLevelErrorToString(int sub_level_err) {
        switch (sub_level_err) {
            case WT_WRITE_CONFLICT:
                return "WT_WRITE_CONFLICT";
            case WT_OLDEST_FOR_EVICTION:
                return "WT_OLDEST_FOR_EVICTION";
            case WT_CACHE_OVERFLOW:
                return "WT_CACHE_OVERFLOW";
            case WT_NONE:
                return "WT_NONE";
            default:
                break;
        }
        LOGV2_DEBUG(
            9979801, 1, "Unexpected sub-level error code", "sub_level_err"_attr = sub_level_err);
        MONGO_UNREACHABLE;
    }

    StringData prefix = "";
    const char* reason = "";
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
    ASSERT_FALSE(rollbackReasonWasCachePressure(WT_BACKGROUND_COMPACT_ALREADY_RUNNING));
    ASSERT_FALSE(rollbackReasonWasCachePressure(WT_NONE));
    ASSERT_TRUE(rollbackReasonWasCachePressure(WT_OLDEST_FOR_EVICTION));
    ASSERT_TRUE(rollbackReasonWasCachePressure(WT_CACHE_OVERFLOW));
}

TEST_F(WiredTigerUtilHelperTest, throwTransactionTooLargeForCacheException) {
    // Throws TransactionTooLargeForCacheException only when TransactionTooLargeForCache is enabled,
    // cache is insufficient for transaction, and reason for rollback was cache pressure. Also
    // throws TransactionTooLargeForCacheException even if temporarilyUnavailableEnabled is enabled.
    std::vector<TestCase> transactionTooLargeTestCases = {
        {.txnTooLarge = true, .cacheInsufficient = true, .sub_level_err = WT_OLDEST_FOR_EVICTION},
        {.txnTooLarge = true, .cacheInsufficient = true, .sub_level_err = WT_CACHE_OVERFLOW},
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
        {.txnTooLarge = false, .cacheInsufficient = false, .sub_level_err = WT_OLDEST_FOR_EVICTION},
        {.txnTooLarge = false, .cacheInsufficient = false, .sub_level_err = WT_CACHE_OVERFLOW},
        {.txnTooLarge = false, .cacheInsufficient = true, .sub_level_err = WT_OLDEST_FOR_EVICTION},
        {.txnTooLarge = false, .cacheInsufficient = true, .sub_level_err = WT_CACHE_OVERFLOW},
        {.txnTooLarge = true, .cacheInsufficient = false, .sub_level_err = WT_OLDEST_FOR_EVICTION},
        {.txnTooLarge = true, .cacheInsufficient = false, .sub_level_err = WT_CACHE_OVERFLOW},
    };

    for (auto testCase : temporarilyUnavailableTestCases) {
        throwsCachePressureException(testCase, ErrorCodes::TemporarilyUnavailable);
    }
}

TEST_F(WiredTigerUtilHelperTest, throwWriteConflictException) {
    std::vector<TestCase> writeConflictExceptionTestCases = {
        // Throws WCE if reason for rollback was not cache pressure.
        {.txnTooLarge = false, .cacheInsufficient = false /*ignored*/, .sub_level_err = WT_NONE},

        // Throws WCE for transaction no matter the reason for rollback if both
        // TransactionTooLargeForCache is disabled.
        {.txnTooLarge = false,
         .cacheInsufficient = false /*ignored*/,
         .sub_level_err = WT_OLDEST_FOR_EVICTION},
        {.txnTooLarge = false,
         .cacheInsufficient = false /*ignored*/,
         .sub_level_err = WT_CACHE_OVERFLOW}};

    for (auto testCase : writeConflictExceptionTestCases) {
        throwsWriteConflictException(testCase, ErrorCodes::WriteConflict);
    }
}

}  // namespace
}  // namespace mongo
