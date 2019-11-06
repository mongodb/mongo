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

#include "mongo/db/error_labels.h"
#include "mongo/db/logical_session_id.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

TEST(IsTransientTransactionErrorTest, NetworkErrorsAreTransientBeforeCommit) {
    ASSERT_TRUE(isTransientTransactionError(ErrorCodes::HostUnreachable,
                                            false /* hasWriteConcernError */,
                                            false /* isCommitOrAbort */));
}

TEST(IsTransientTransactionErrorTest, NetworkErrorsAreNotTransientOnCommit) {
    ASSERT_FALSE(isTransientTransactionError(
        ErrorCodes::HostUnreachable, false /* hasWriteConcernError */, true /* isCommitOrAbort */));
}

TEST(IsTransientTransactionErrorTest, RetryableWriteErrorsAreNotTransientOnAbort) {
    ASSERT_FALSE(isTransientTransactionError(
        ErrorCodes::NotMaster, false /* hasWriteConcernError */, true /* isCommitOrAbort */));
}

TEST(IsTransientTransactionErrorTest,
     NoSuchTransactionWithWriteConcernErrorsAreNotTransientOnCommit) {
    ASSERT_FALSE(isTransientTransactionError(ErrorCodes::NoSuchTransaction,
                                             true /* hasWriteConcernError */,
                                             true /* isCommitOrAbort */));
}

TEST(IsTransientTransactionErrorTest,
     NoSuchTransactionWithoutWriteConcernErrorsAreTransientOnCommit) {
    ASSERT_TRUE(isTransientTransactionError(ErrorCodes::NoSuchTransaction,
                                            false /* hasWriteConcernError */,
                                            true /* isCommitOrAbort */));
}

TEST(ErrorLabelBuilderTest, NonErrorCodesHaveNoLabel) {
    OperationSessionInfoFromClient sessionInfo;
    std::string commandName = "insert";
    ErrorLabelBuilder builder(sessionInfo, commandName, boost::none, boost::none, false);
    ASSERT_FALSE(builder.isTransientTransactionError());
    ASSERT_FALSE(builder.isRetryableWriteError());
    ASSERT_FALSE(builder.isNonResumableChangeStreamError());
}

TEST(ErrorLabelBuilderTest, NonTransactionsHaveNoTransientTransactionErrorLabel) {
    OperationSessionInfoFromClient sessionInfo;
    std::string commandName = "insert";
    ErrorLabelBuilder builder(
        sessionInfo, commandName, ErrorCodes::WriteConflict, boost::none, false);
    ASSERT_FALSE(builder.isTransientTransactionError());
}

TEST(ErrorLabelBuilderTest, RetryableWritesHaveNoTransientTransactionErrorLabel) {
    OperationSessionInfoFromClient sessionInfo;
    sessionInfo.setTxnNumber(1);
    std::string commandName = "insert";
    ErrorLabelBuilder builder(
        sessionInfo, commandName, ErrorCodes::WriteConflict, boost::none, false);
    ASSERT_FALSE(builder.isTransientTransactionError());
}

TEST(ErrorLabelBuilderTest, NonTransientTransactionErrorsHaveNoTransientTransactionErrorLabel) {
    OperationSessionInfoFromClient sessionInfo;
    sessionInfo.setTxnNumber(1);
    sessionInfo.setAutocommit(false);
    std::string commandName = "commitTransaction";
    ErrorLabelBuilder builder(sessionInfo, commandName, ErrorCodes::NotMaster, boost::none, false);
    ASSERT_FALSE(builder.isTransientTransactionError());
}

TEST(ErrorLabelBuilderTest, TransientTransactionErrorsHaveTransientTransactionErrorLabel) {
    OperationSessionInfoFromClient sessionInfo;
    sessionInfo.setTxnNumber(1);
    sessionInfo.setAutocommit(false);
    std::string commandName = "commitTransaction";
    ErrorLabelBuilder builder(
        sessionInfo, commandName, ErrorCodes::NoSuchTransaction, boost::none, false);
    ASSERT_TRUE(builder.isTransientTransactionError());
}

TEST(ErrorLabelBuilderTest, NonRetryableWritesHaveNoRetryableWriteErrorLabel) {
    OperationSessionInfoFromClient sessionInfo;
    std::string commandName = "insert";
    ErrorLabelBuilder builder(sessionInfo, commandName, ErrorCodes::NotMaster, boost::none, false);

    // Test regular writes.
    ASSERT_FALSE(builder.isRetryableWriteError());

    // Test transaction writes.
    sessionInfo.setTxnNumber(1);
    sessionInfo.setAutocommit(false);
    ASSERT_FALSE(builder.isRetryableWriteError());
}

TEST(ErrorLabelBuilderTest, NonRetryableWriteErrorsHaveNoRetryableWriteErrorLabel) {
    OperationSessionInfoFromClient sessionInfo;
    sessionInfo.setTxnNumber(1);
    std::string commandName = "update";
    ErrorLabelBuilder builder(
        sessionInfo, commandName, ErrorCodes::WriteConflict, boost::none, false);
    ASSERT_FALSE(builder.isRetryableWriteError());
}

TEST(ErrorLabelBuilderTest, RetryableWriteErrorsHaveRetryableWriteErrorLabel) {
    OperationSessionInfoFromClient sessionInfo;
    sessionInfo.setTxnNumber(1);
    std::string commandName = "update";
    ErrorLabelBuilder builder(sessionInfo, commandName, ErrorCodes::NotMaster, boost::none, false);
    ASSERT_TRUE(builder.isRetryableWriteError());
}

TEST(ErrorLabelBuilderTest, RetryableWriteErrorsHaveNoRetryableWriteErrorLabelForInternalClients) {
    OperationSessionInfoFromClient sessionInfo;
    sessionInfo.setTxnNumber(1);
    std::string commandName = "update";
    ErrorLabelBuilder builder(sessionInfo, commandName, ErrorCodes::NotMaster, boost::none, true);
    ASSERT_FALSE(builder.isRetryableWriteError());
}

TEST(ErrorLabelBuilderTest,
     NonRetryableWriteErrorsInWriteConcernErrorsHaveNoRetryableWriteErrorLabel) {
    OperationSessionInfoFromClient sessionInfo;
    sessionInfo.setTxnNumber(1);
    std::string commandName = "update";
    ErrorLabelBuilder builder(sessionInfo,
                              commandName,
                              ErrorCodes::WriteConcernFailed,
                              ErrorCodes::WriteConcernFailed,
                              false);
    ASSERT_FALSE(builder.isRetryableWriteError());
}

TEST(ErrorLabelBuilderTest, RetryableWriteErrorsInWriteConcernErrorsHaveRetryableWriteErrorLabel) {
    OperationSessionInfoFromClient sessionInfo;
    sessionInfo.setTxnNumber(1);
    std::string commandName = "update";
    ErrorLabelBuilder builder(sessionInfo,
                              commandName,
                              ErrorCodes::WriteConcernFailed,
                              ErrorCodes::PrimarySteppedDown,
                              false);
    ASSERT_TRUE(builder.isRetryableWriteError());
}

TEST(ErrorLabelBuilderTest, RetryableWriteErrorsOnCommitAbortHaveRetryableWriteErrorLabel) {
    OperationSessionInfoFromClient sessionInfo;
    sessionInfo.setTxnNumber(1);
    sessionInfo.setAutocommit(false);
    std::string commandName;

    commandName = "commitTransaction";
    ErrorLabelBuilder commitBuilder(
        sessionInfo, commandName, ErrorCodes::NotMaster, boost::none, false);
    ASSERT_TRUE(commitBuilder.isRetryableWriteError());

    commandName = "coordinateCommitTransaction";
    ErrorLabelBuilder coordinateCommitBuilder(
        sessionInfo, commandName, ErrorCodes::NotMaster, boost::none, false);
    ASSERT_TRUE(coordinateCommitBuilder.isRetryableWriteError());

    commandName = "abortTransaction";
    ErrorLabelBuilder abortBuilder(
        sessionInfo, commandName, ErrorCodes::NotMaster, boost::none, false);
    ASSERT_TRUE(abortBuilder.isRetryableWriteError());
}

TEST(ErrorLabelBuilderTest, NonResumableChangeStreamError) {
    OperationSessionInfoFromClient sessionInfo;
    std::string commandName;
    ErrorLabelBuilder builder(
        sessionInfo, commandName, ErrorCodes::ChangeStreamHistoryLost, boost::none, false);
    ASSERT_TRUE(builder.isNonResumableChangeStreamError());
}

}  // namespace
}  // namespace mongo
