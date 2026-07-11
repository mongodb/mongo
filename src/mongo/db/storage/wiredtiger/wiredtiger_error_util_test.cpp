// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/storage/wiredtiger/wiredtiger_error_util.h"

#include "mongo/db/storage/exceptions.h"
#include "mongo/logv2/log.h"
#include "mongo/unittest/unittest.h"

#include <string_view>

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

    std::string_view prefix = "";
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
