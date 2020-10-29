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

#include "mongo/db/curop.h"
#include "mongo/db/error_labels.h"
#include "mongo/db/logical_session_id.h"
#include "mongo/db/pipeline/aggregate_command_gen.h"
#include "mongo/db/pipeline/aggregation_request_helper.h"
#include "mongo/db/pipeline/lite_parsed_pipeline.h"
#include "mongo/db/service_context_test_fixture.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

TEST(IsTransientTransactionErrorTest, WriteConflictIsTransient) {
    ASSERT_TRUE(isTransientTransactionError(
        ErrorCodes::WriteConflict, false /* hasWriteConcernError */, false /* isCommitOrAbort */));
}

TEST(IsTransientTransactionErrorTest, LockTimeoutIsTransient) {
    ASSERT_TRUE(isTransientTransactionError(
        ErrorCodes::LockTimeout, false /* hasWriteConcernError */, false /* isCommitOrAbort */));
}

TEST(IsTransientTransactionErrorTest, PreparedTransactionInProgressIsTransient) {
    ASSERT_TRUE(isTransientTransactionError(ErrorCodes::PreparedTransactionInProgress,
                                            false /* hasWriteConcernError */,
                                            false /* isCommitOrAbort */));
}

TEST(IsTransientTransactionErrorTest, TenantMigrationCommittedIsTransient) {
    ASSERT_TRUE(isTransientTransactionError(ErrorCodes::TenantMigrationCommitted,
                                            false /* hasWriteConcernError */,
                                            false /* isCommitOrAbort */));
}

TEST(IsTransientTransactionErrorTest, TenantMigrationAbortedIsTransient) {
    ASSERT_TRUE(isTransientTransactionError(ErrorCodes::TenantMigrationAborted,
                                            false /* hasWriteConcernError */,
                                            false /* isCommitOrAbort */));
}

TEST(IsTransientTransactionErrorTest, ShardInvalidatedForTargetingIsTransient) {
    ASSERT_TRUE(isTransientTransactionError(ErrorCodes::ShardInvalidatedForTargeting,
                                            false /* hasWriteConcernError */,
                                            false /* isCommitOrAbort */));
}

TEST(IsTransientTransactionErrorTest, StaleDbVersionIsTransient) {
    ASSERT_TRUE(isTransientTransactionError(
        ErrorCodes::StaleDbVersion, false /* hasWriteConcernError */, false /* isCommitOrAbort */));
}

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
    ASSERT_FALSE(isTransientTransactionError(ErrorCodes::NotWritablePrimary,
                                             false /* hasWriteConcernError */,
                                             true /* isCommitOrAbort */));
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

class ErrorLabelBuilderTest : public ServiceContextTest {
public:
    ErrorLabelBuilderTest() : _opCtx(makeOperationContext()) {}

    void setCommand(BSONObj cmdObj) const {
        CurOp::get(opCtx())->setGenericOpRequestDetails(
            opCtx(), _testNss, nullptr, cmdObj, NetworkOp::dbMsg);
    }

    void setGetMore(BSONObj originatingCommand) const {
        setCommand(BSON("getMore" << 1000000ll << "collection" << _testNss.coll()));
        stdx::lock_guard<Client> lk(*opCtx()->getClient());
        CurOp::get(opCtx())->setOriginatingCommand_inlock(originatingCommand);
    }

    const NamespaceString& nss() const {
        return _testNss;
    }

    OperationContext* opCtx() const {
        return _opCtx.get();
    }

private:
    const NamespaceString _testNss{"test", "testing"};
    ServiceContext::UniqueOperationContext _opCtx;
};

TEST_F(ErrorLabelBuilderTest, NonErrorCodesHaveNoLabel) {
    OperationSessionInfoFromClient sessionInfo;
    std::string commandName = "insert";
    ErrorLabelBuilder builder(opCtx(), sessionInfo, commandName, boost::none, boost::none, false);
    ASSERT_FALSE(builder.isTransientTransactionError());
    ASSERT_FALSE(builder.isRetryableWriteError());
    ASSERT_FALSE(builder.isResumableChangeStreamError());
}

TEST_F(ErrorLabelBuilderTest, NonTransactionsHaveNoTransientTransactionErrorLabel) {
    OperationSessionInfoFromClient sessionInfo;
    std::string commandName = "insert";
    ErrorLabelBuilder builder(
        opCtx(), sessionInfo, commandName, ErrorCodes::WriteConflict, boost::none, false);
    ASSERT_FALSE(builder.isTransientTransactionError());
}

TEST_F(ErrorLabelBuilderTest, RetryableWritesHaveNoTransientTransactionErrorLabel) {
    OperationSessionInfoFromClient sessionInfo;
    sessionInfo.setTxnNumber(1);
    std::string commandName = "insert";
    ErrorLabelBuilder builder(
        opCtx(), sessionInfo, commandName, ErrorCodes::WriteConflict, boost::none, false);
    ASSERT_FALSE(builder.isTransientTransactionError());
}

TEST_F(ErrorLabelBuilderTest, NonTransientTransactionErrorsHaveNoTransientTransactionErrorLabel) {
    OperationSessionInfoFromClient sessionInfo;
    sessionInfo.setTxnNumber(1);
    sessionInfo.setAutocommit(false);
    std::string commandName = "commitTransaction";
    ErrorLabelBuilder builder(
        opCtx(), sessionInfo, commandName, ErrorCodes::NotWritablePrimary, boost::none, false);
    ASSERT_FALSE(builder.isTransientTransactionError());
}

TEST_F(ErrorLabelBuilderTest, TransientTransactionErrorsHaveTransientTransactionErrorLabel) {
    OperationSessionInfoFromClient sessionInfo;
    sessionInfo.setTxnNumber(1);
    sessionInfo.setAutocommit(false);
    std::string commandName = "commitTransaction";
    ErrorLabelBuilder builder(
        opCtx(), sessionInfo, commandName, ErrorCodes::NoSuchTransaction, boost::none, false);
    ASSERT_TRUE(builder.isTransientTransactionError());
}

TEST_F(ErrorLabelBuilderTest, NonRetryableWritesHaveNoRetryableWriteErrorLabel) {
    OperationSessionInfoFromClient sessionInfo;
    std::string commandName = "insert";
    ErrorLabelBuilder builder(
        opCtx(), sessionInfo, commandName, ErrorCodes::NotWritablePrimary, boost::none, false);

    // Test regular writes.
    ASSERT_FALSE(builder.isRetryableWriteError());

    // Test transaction writes.
    sessionInfo.setTxnNumber(1);
    sessionInfo.setAutocommit(false);
    ASSERT_FALSE(builder.isRetryableWriteError());
}

TEST_F(ErrorLabelBuilderTest, NonRetryableWriteErrorsHaveNoRetryableWriteErrorLabel) {
    OperationSessionInfoFromClient sessionInfo;
    sessionInfo.setTxnNumber(1);
    std::string commandName = "update";
    ErrorLabelBuilder builder(
        opCtx(), sessionInfo, commandName, ErrorCodes::WriteConflict, boost::none, false);
    ASSERT_FALSE(builder.isRetryableWriteError());
}

TEST_F(ErrorLabelBuilderTest, RetryableWriteErrorsHaveRetryableWriteErrorLabel) {
    OperationSessionInfoFromClient sessionInfo;
    sessionInfo.setTxnNumber(1);
    std::string commandName = "update";
    ErrorLabelBuilder builder(
        opCtx(), sessionInfo, commandName, ErrorCodes::NotWritablePrimary, boost::none, false);
    ASSERT_TRUE(builder.isRetryableWriteError());
}

TEST_F(ErrorLabelBuilderTest,
       RetryableWriteErrorsHaveNoRetryableWriteErrorLabelForInternalClients) {
    OperationSessionInfoFromClient sessionInfo;
    sessionInfo.setTxnNumber(1);
    std::string commandName = "update";
    ErrorLabelBuilder builder(
        opCtx(), sessionInfo, commandName, ErrorCodes::NotWritablePrimary, boost::none, true);
    ASSERT_FALSE(builder.isRetryableWriteError());
}

TEST_F(ErrorLabelBuilderTest,
       NonRetryableWriteErrorsInWriteConcernErrorsHaveNoRetryableWriteErrorLabel) {
    OperationSessionInfoFromClient sessionInfo;
    sessionInfo.setTxnNumber(1);
    std::string commandName = "update";
    ErrorLabelBuilder builder(opCtx(),
                              sessionInfo,
                              commandName,
                              ErrorCodes::WriteConcernFailed,
                              ErrorCodes::WriteConcernFailed,
                              false);
    ASSERT_FALSE(builder.isRetryableWriteError());
}

TEST_F(ErrorLabelBuilderTest,
       RetryableWriteErrorsInWriteConcernErrorsHaveRetryableWriteErrorLabel) {
    OperationSessionInfoFromClient sessionInfo;
    sessionInfo.setTxnNumber(1);
    std::string commandName = "update";
    ErrorLabelBuilder builder(opCtx(),
                              sessionInfo,
                              commandName,
                              ErrorCodes::WriteConcernFailed,
                              ErrorCodes::PrimarySteppedDown,
                              false);
    ASSERT_TRUE(builder.isRetryableWriteError());
}

TEST_F(ErrorLabelBuilderTest, RetryableWriteErrorsOnCommitAbortHaveRetryableWriteErrorLabel) {
    OperationSessionInfoFromClient sessionInfo;
    sessionInfo.setTxnNumber(1);
    sessionInfo.setAutocommit(false);
    std::string commandName;

    commandName = "commitTransaction";
    ErrorLabelBuilder commitBuilder(
        opCtx(), sessionInfo, commandName, ErrorCodes::NotWritablePrimary, boost::none, false);
    ASSERT_TRUE(commitBuilder.isRetryableWriteError());

    commandName = "coordinateCommitTransaction";
    ErrorLabelBuilder coordinateCommitBuilder(
        opCtx(), sessionInfo, commandName, ErrorCodes::NotWritablePrimary, boost::none, false);
    ASSERT_TRUE(coordinateCommitBuilder.isRetryableWriteError());

    commandName = "abortTransaction";
    ErrorLabelBuilder abortBuilder(
        opCtx(), sessionInfo, commandName, ErrorCodes::NotWritablePrimary, boost::none, false);
    ASSERT_TRUE(abortBuilder.isRetryableWriteError());
}

TEST_F(ErrorLabelBuilderTest, NonResumableChangeStreamError) {
    OperationSessionInfoFromClient sessionInfo;
    std::string commandName;
    ErrorLabelBuilder builder(
        opCtx(), sessionInfo, commandName, ErrorCodes::ChangeStreamHistoryLost, boost::none, false);
    ASSERT_TRUE(builder.isNonResumableChangeStreamError());
}

TEST_F(ErrorLabelBuilderTest, ResumableChangeStreamErrorAppliesToChangeStreamAggregations) {
    OperationSessionInfoFromClient sessionInfo;
    // Build the aggregation command and confirm that it parses correctly, so we know that the error
    // is the only factor that determines the success or failure of isResumableChangeStreamError().
    auto cmdObj = BSON("aggregate" << nss().coll() << "pipeline"
                                   << BSON_ARRAY(BSON("$changeStream" << BSONObj())) << "cursor"
                                   << BSONObj() << "$db" << nss().db());
    auto aggRequest = uassertStatusOK(aggregation_request_helper::parseFromBSON(nss(), cmdObj));
    ASSERT_TRUE(LiteParsedPipeline(aggRequest).hasChangeStream());

    // The label applies to a $changeStream "aggregate" command.
    std::string commandName = "aggregate";
    setCommand(cmdObj);
    ErrorLabelBuilder resumableAggBuilder(
        opCtx(), sessionInfo, commandName, ErrorCodes::NetworkTimeout, boost::none, false);
    ASSERT_TRUE(resumableAggBuilder.isResumableChangeStreamError());
    // The label applies to a "getMore" command on a $changeStream cursor.
    commandName = "getMore";
    setGetMore(cmdObj);
    ErrorLabelBuilder resumableGetMoreBuilder(
        opCtx(), sessionInfo, commandName, ErrorCodes::NetworkTimeout, boost::none, false);
    ASSERT_TRUE(resumableGetMoreBuilder.isResumableChangeStreamError());
}

TEST_F(ErrorLabelBuilderTest, ResumableChangeStreamErrorDoesNotApplyToNonResumableErrors) {
    OperationSessionInfoFromClient sessionInfo;
    // Build the aggregation command and confirm that it parses correctly, so we know that the error
    // is the only factor that determines the success or failure of isResumableChangeStreamError().
    auto cmdObj = BSON("aggregate" << nss().coll() << "pipeline"
                                   << BSON_ARRAY(BSON("$changeStream" << BSONObj())) << "cursor"
                                   << BSONObj() << "$db" << nss().db());
    auto aggRequest = uassertStatusOK(aggregation_request_helper::parseFromBSON(nss(), cmdObj));
    ASSERT_TRUE(LiteParsedPipeline(aggRequest).hasChangeStream());

    // The label does not apply to a ChangeStreamFatalError error on a $changeStream aggregation.
    std::string commandName = "aggregate";
    setCommand(cmdObj);
    ErrorLabelBuilder resumableAggBuilder(
        opCtx(), sessionInfo, commandName, ErrorCodes::ChangeStreamFatalError, boost::none, false);
    ASSERT_FALSE(resumableAggBuilder.isResumableChangeStreamError());
    // The label does not apply to a ChangeStreamFatalError error on a $changeStream getMore.
    commandName = "getMore";
    setGetMore(cmdObj);
    ErrorLabelBuilder resumableGetMoreBuilder(
        opCtx(), sessionInfo, commandName, ErrorCodes::ChangeStreamFatalError, boost::none, false);
    ASSERT_FALSE(resumableGetMoreBuilder.isResumableChangeStreamError());
}

TEST_F(ErrorLabelBuilderTest, ResumableChangeStreamErrorDoesNotApplyToNonChangeStreamAggregations) {
    OperationSessionInfoFromClient sessionInfo;
    // Build the aggregation command and confirm that it parses correctly, so we know that the error
    // is the only factor that determines the success or failure of isResumableChangeStreamError().
    auto cmdObj =
        BSON("aggregate" << nss().coll() << "pipeline" << BSON_ARRAY(BSON("$match" << BSONObj()))
                         << "cursor" << BSONObj() << "$db" << nss().db());
    auto aggRequest = uassertStatusOK(aggregation_request_helper::parseFromBSON(nss(), cmdObj));
    ASSERT_FALSE(LiteParsedPipeline(aggRequest).hasChangeStream());

    // The label does not apply to a non-$changeStream "aggregate" command.
    std::string commandName = "aggregate";
    setCommand(cmdObj);
    ErrorLabelBuilder nonResumableAggBuilder(
        opCtx(), sessionInfo, commandName, ErrorCodes::NetworkTimeout, boost::none, false);
    ASSERT_FALSE(nonResumableAggBuilder.isResumableChangeStreamError());
    // The label does not apply to a "getMore" command on a non-$changeStream cursor.
    commandName = "getMore";
    setGetMore(cmdObj);
    ErrorLabelBuilder nonResumableGetMoreBuilder(
        opCtx(), sessionInfo, commandName, ErrorCodes::NetworkTimeout, boost::none, false);
    ASSERT_FALSE(nonResumableGetMoreBuilder.isResumableChangeStreamError());
}

TEST_F(ErrorLabelBuilderTest, ResumableChangeStreamErrorDoesNotApplyToNonAggregations) {
    OperationSessionInfoFromClient sessionInfo;
    auto cmdObj = BSON("find" << nss().coll() << "filter" << BSONObj());
    // The label does not apply to a "find" command.
    std::string commandName = "find";
    setCommand(cmdObj);
    ErrorLabelBuilder nonResumableFindBuilder(
        opCtx(), sessionInfo, commandName, ErrorCodes::NetworkTimeout, boost::none, false);
    ASSERT_FALSE(nonResumableFindBuilder.isResumableChangeStreamError());
    // The label does not apply to a "getMore" command on a "find" cursor.
    commandName = "getMore";
    setGetMore(cmdObj);
    ErrorLabelBuilder nonResumableGetMoreBuilder(
        opCtx(), sessionInfo, commandName, ErrorCodes::NetworkTimeout, boost::none, false);
    ASSERT_FALSE(nonResumableGetMoreBuilder.isResumableChangeStreamError());
}

}  // namespace
}  // namespace mongo
