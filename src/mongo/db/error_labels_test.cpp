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

#include <boost/cstdint.hpp>
#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <cstdint>
#include <mutex>

#include <boost/optional/optional.hpp>

#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/client.h"
#include "mongo/db/curop.h"
#include "mongo/db/error_labels.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/pipeline/aggregation_request_helper.h"
#include "mongo/db/pipeline/lite_parsed_pipeline.h"
#include "mongo/db/service_context.h"
#include "mongo/db/service_context_test_fixture.h"
#include "mongo/rpc/message.h"
#include "mongo/unittest/assert.h"
#include "mongo/unittest/bson_test_util.h"
#include "mongo/unittest/framework.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/fail_point.h"

namespace mongo {
namespace {

const repl::OpTime kOpTime{Timestamp(10, 10), 10};
const repl::OpTime kLaterOpTime{Timestamp(10, 11), 10};

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

TEST(IsTransientTransactionErrorTest, ShardCannotRefreshDueToLocksHeldIsTransient) {
    ASSERT_TRUE(isTransientTransactionError(ErrorCodes::ShardCannotRefreshDueToLocksHeld,
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
            _testNss, nullptr, cmdObj, NetworkOp::dbMsg);
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
    const NamespaceString _testNss =
        NamespaceString::createNamespaceString_forTest("test", "testing");
    ServiceContext::UniqueOperationContext _opCtx;
};

TEST_F(ErrorLabelBuilderTest, NonErrorCodesHaveNoLabel) {
    OperationSessionInfoFromClient sessionInfo{LogicalSessionFromClient(UUID::gen())};
    std::string commandName = "insert";
    ErrorLabelBuilder builder(opCtx(),
                              sessionInfo,
                              commandName,
                              boost::none,
                              boost::none,
                              false,
                              false /* isMongos */,
                              kOpTime,
                              kOpTime);
    ASSERT_FALSE(builder.isTransientTransactionError());
    ASSERT_FALSE(builder.isRetryableWriteError());
    ASSERT_FALSE(builder.isResumableChangeStreamError());
    ASSERT_FALSE(builder.isErrorWithNoWritesPerformed());
}

TEST_F(ErrorLabelBuilderTest, NonTransactionsHaveNoTransientTransactionErrorLabel) {
    OperationSessionInfoFromClient sessionInfo{LogicalSessionFromClient(UUID::gen())};
    std::string commandName = "insert";
    ErrorLabelBuilder builder(opCtx(),
                              sessionInfo,
                              commandName,
                              ErrorCodes::WriteConflict,
                              boost::none,
                              false /* isInternalClient */,
                              false /* isMongos */,
                              repl::OpTime{},
                              repl::OpTime{});
    ASSERT_FALSE(builder.isTransientTransactionError());
}

TEST_F(ErrorLabelBuilderTest, RetryableWritesHaveNoTransientTransactionErrorLabel) {
    OperationSessionInfoFromClient sessionInfo{LogicalSessionFromClient(UUID::gen())};
    sessionInfo.setTxnNumber(1);
    std::string commandName = "insert";
    ErrorLabelBuilder builder(opCtx(),
                              sessionInfo,
                              commandName,
                              ErrorCodes::WriteConflict,
                              boost::none,
                              false /* isInternalClient */,
                              false /* isMongos */,
                              repl::OpTime{},
                              repl::OpTime{});
    ASSERT_FALSE(builder.isTransientTransactionError());
}

TEST_F(ErrorLabelBuilderTest, NonTransientTransactionErrorsHaveNoTransientTransactionErrorLabel) {
    OperationSessionInfoFromClient sessionInfo{LogicalSessionFromClient(UUID::gen())};
    sessionInfo.setTxnNumber(1);
    sessionInfo.setAutocommit(false);
    std::string commandName = "commitTransaction";
    ErrorLabelBuilder builder(opCtx(),
                              sessionInfo,
                              commandName,
                              ErrorCodes::NotWritablePrimary,
                              boost::none,
                              false /* isInternalClient */,
                              false /* isMongos */,
                              repl::OpTime{},
                              repl::OpTime{});
    ASSERT_FALSE(builder.isTransientTransactionError());
}

TEST_F(ErrorLabelBuilderTest, TransientTransactionErrorsHaveTransientTransactionErrorLabel) {
    OperationSessionInfoFromClient sessionInfo{LogicalSessionFromClient(UUID::gen())};
    sessionInfo.setTxnNumber(1);
    sessionInfo.setAutocommit(false);
    std::string commandName = "commitTransaction";
    ErrorLabelBuilder builder(opCtx(),
                              sessionInfo,
                              commandName,
                              ErrorCodes::NoSuchTransaction,
                              boost::none,
                              false /* isInternalClient */,
                              false /* isMongos */,
                              repl::OpTime{},
                              repl::OpTime{});
    ASSERT_TRUE(builder.isTransientTransactionError());
}

TEST_F(
    ErrorLabelBuilderTest,
    TransientTransactionErrorWithRetryableWriteConcernErrorHasTransientTransactionErrorLabelOnly) {
    OperationSessionInfoFromClient sessionInfo{LogicalSessionFromClient(UUID::gen())};
    sessionInfo.setTxnNumber(1);
    sessionInfo.setAutocommit(false);
    std::string commandName = "commitTransaction";

    auto transientError = ErrorCodes::WriteConflict;
    auto retryableError = ErrorCodes::InterruptedDueToReplStateChange;
    auto actualErrorLabels = getErrorLabels(opCtx(),
                                            sessionInfo,
                                            commandName,
                                            transientError,
                                            retryableError,
                                            false /* isInternalClient */,
                                            false /* isMongos */,
                                            repl::OpTime{},
                                            repl::OpTime{});

    // Ensure only the TransientTransactionError label is attached so users know to retry the entire
    // transaction.
    BSONArrayBuilder expectedLabelArray;
    expectedLabelArray << ErrorLabel::kTransientTransaction;
    ASSERT_BSONOBJ_EQ(actualErrorLabels, BSON(kErrorLabelsFieldName << expectedLabelArray.arr()));
}

TEST_F(ErrorLabelBuilderTest, NonRetryableWritesHaveNoRetryableWriteErrorLabel) {
    OperationSessionInfoFromClient sessionInfo{LogicalSessionFromClient(UUID::gen())};
    std::string commandName = "insert";
    ErrorLabelBuilder builder(opCtx(),
                              sessionInfo,
                              commandName,
                              ErrorCodes::NotWritablePrimary,
                              boost::none,
                              false /* isInternalClient */,
                              false /* isMongos */,
                              repl::OpTime{},
                              repl::OpTime{});

    // Test regular writes.
    ASSERT_FALSE(builder.isRetryableWriteError());

    // Test transaction writes.
    sessionInfo.setTxnNumber(1);
    sessionInfo.setAutocommit(false);
    ASSERT_FALSE(builder.isRetryableWriteError());
}

TEST_F(ErrorLabelBuilderTest, NonRetryableWriteErrorsHaveNoRetryableWriteErrorLabel) {
    OperationSessionInfoFromClient sessionInfo{LogicalSessionFromClient(UUID::gen())};
    sessionInfo.setTxnNumber(1);
    std::string commandName = "update";
    ErrorLabelBuilder builder(opCtx(),
                              sessionInfo,
                              commandName,
                              ErrorCodes::WriteConflict,
                              boost::none,
                              false /* isInternalClient */,
                              false /* isMongos */,
                              repl::OpTime{},
                              repl::OpTime{});
    ASSERT_FALSE(builder.isRetryableWriteError());
}

TEST_F(ErrorLabelBuilderTest, RetryableWriteErrorsHaveRetryableWriteErrorLabel) {
    OperationSessionInfoFromClient sessionInfo{LogicalSessionFromClient(UUID::gen())};
    sessionInfo.setTxnNumber(1);
    std::string commandName = "update";
    ErrorLabelBuilder builder(opCtx(),
                              sessionInfo,
                              commandName,
                              ErrorCodes::NotWritablePrimary,
                              boost::none,
                              false /* isInternalClient */,
                              false /* isMongos */,
                              repl::OpTime{},
                              repl::OpTime{});
    ASSERT_TRUE(builder.isRetryableWriteError());
}

TEST_F(ErrorLabelBuilderTest, NonLocalShutDownErrorsOnMongosDoNotHaveRetryableWriteErrorLabel) {
    OperationSessionInfoFromClient sessionInfo{LogicalSessionFromClient(UUID::gen())};
    sessionInfo.setTxnNumber(1);
    std::string commandName = "update";
    ErrorLabelBuilder builder(opCtx(),
                              sessionInfo,
                              commandName,
                              ErrorCodes::InterruptedAtShutdown,
                              boost::none,
                              false /* isInternalClient */,
                              true /* isMongos */,
                              repl::OpTime{},
                              repl::OpTime{});
    ASSERT_FALSE(builder.isRetryableWriteError());
}

TEST_F(ErrorLabelBuilderTest,
       LocalShutDownErrorsOnMongosHaveRetryableWriteErrorLabelInterruptedAtShutdown) {
    OperationSessionInfoFromClient sessionInfo{LogicalSessionFromClient(UUID::gen())};
    sessionInfo.setTxnNumber(1);
    std::string commandName = "update";
    FailPointEnableBlock failPoint("errorLabelBuilderMockShutdown");
    ErrorLabelBuilder builder(opCtx(),
                              sessionInfo,
                              commandName,
                              ErrorCodes::InterruptedAtShutdown,
                              boost::none,
                              false /* isInternalClient */,
                              true /* isMongos */,
                              repl::OpTime{},
                              repl::OpTime{});
    ASSERT_TRUE(builder.isRetryableWriteError());
}

TEST_F(ErrorLabelBuilderTest,
       LocalShutDownErrorsOnMongosHaveRetryableWriteErrorLabelCallbackCanceled) {
    OperationSessionInfoFromClient sessionInfo{LogicalSessionFromClient(UUID::gen())};
    sessionInfo.setTxnNumber(1);
    std::string commandName = "update";
    FailPointEnableBlock failPoint("errorLabelBuilderMockShutdown");
    ErrorLabelBuilder builder(opCtx(),
                              sessionInfo,
                              commandName,
                              ErrorCodes::CallbackCanceled,
                              boost::none,
                              false /* isInternalClient */,
                              true /* isMongos */,
                              repl::OpTime{},
                              repl::OpTime{});
    ASSERT_TRUE(builder.isRetryableWriteError());
}

TEST_F(ErrorLabelBuilderTest,
       RetryableWriteErrorsHaveNoRetryableWriteErrorLabelForInternalClients) {
    OperationSessionInfoFromClient sessionInfo{LogicalSessionFromClient(UUID::gen())};
    sessionInfo.setTxnNumber(1);
    std::string commandName = "update";
    ErrorLabelBuilder builder(opCtx(),
                              sessionInfo,
                              commandName,
                              ErrorCodes::NotWritablePrimary,
                              boost::none,
                              true /* isInternalClient */,
                              false /* isMongos */,
                              repl::OpTime{},
                              repl::OpTime{});
    ASSERT_FALSE(builder.isRetryableWriteError());
}

TEST_F(ErrorLabelBuilderTest,
       NonRetryableWriteErrorsInWriteConcernErrorsHaveNoRetryableWriteErrorLabel) {
    OperationSessionInfoFromClient sessionInfo{LogicalSessionFromClient(UUID::gen())};
    sessionInfo.setTxnNumber(1);
    std::string commandName = "update";
    ErrorLabelBuilder builder(opCtx(),
                              sessionInfo,
                              commandName,
                              ErrorCodes::WriteConcernFailed,
                              ErrorCodes::WriteConcernFailed,
                              false /* isInternalClient */,
                              false /* isMongos */,
                              repl::OpTime{},
                              repl::OpTime{});
    ASSERT_FALSE(builder.isRetryableWriteError());
}

TEST_F(ErrorLabelBuilderTest,
       RetryableWriteErrorsInWriteConcernErrorsHaveRetryableWriteErrorLabel) {
    OperationSessionInfoFromClient sessionInfo{LogicalSessionFromClient(UUID::gen())};
    sessionInfo.setTxnNumber(1);
    std::string commandName = "update";
    ErrorLabelBuilder builder(opCtx(),
                              sessionInfo,
                              commandName,
                              ErrorCodes::WriteConcernFailed,
                              ErrorCodes::PrimarySteppedDown,
                              false /* isInternalClient */,
                              false /* isMongos */,
                              repl::OpTime{},
                              repl::OpTime{});
    ASSERT_TRUE(builder.isRetryableWriteError());
}

TEST_F(ErrorLabelBuilderTest, RetryableWriteErrorsOnCommitAbortHaveRetryableWriteErrorLabel) {
    OperationSessionInfoFromClient sessionInfo{LogicalSessionFromClient(UUID::gen())};
    sessionInfo.setTxnNumber(1);
    sessionInfo.setAutocommit(false);
    std::string commandName;

    commandName = "commitTransaction";
    ErrorLabelBuilder commitBuilder(opCtx(),
                                    sessionInfo,
                                    commandName,
                                    ErrorCodes::NotWritablePrimary,
                                    boost::none,
                                    false /* isInternalClient */,
                                    false /* isMongos */,
                                    repl::OpTime{},
                                    repl::OpTime{});
    ASSERT_TRUE(commitBuilder.isRetryableWriteError());
    ASSERT_FALSE(commitBuilder.isTransientTransactionError());

    commandName = "clusterCommitTransaction";
    ErrorLabelBuilder clusterCommitBuilder(opCtx(),
                                           sessionInfo,
                                           commandName,
                                           ErrorCodes::NotWritablePrimary,
                                           boost::none,
                                           false /* isInternalClient */,
                                           true /* isMongos */,
                                           repl::OpTime{},
                                           repl::OpTime{});
    ASSERT_TRUE(commitBuilder.isRetryableWriteError());
    ASSERT_FALSE(commitBuilder.isTransientTransactionError());

    commandName = "coordinateCommitTransaction";
    ErrorLabelBuilder coordinateCommitBuilder(opCtx(),
                                              sessionInfo,
                                              commandName,
                                              ErrorCodes::NotWritablePrimary,
                                              boost::none,
                                              false /* isInternalClient */,
                                              false /* isMongos */,
                                              repl::OpTime{},
                                              repl::OpTime{});
    ASSERT_TRUE(coordinateCommitBuilder.isRetryableWriteError());
    ASSERT_FALSE(commitBuilder.isTransientTransactionError());

    commandName = "abortTransaction";
    ErrorLabelBuilder abortBuilder(opCtx(),
                                   sessionInfo,
                                   commandName,
                                   ErrorCodes::NotWritablePrimary,
                                   boost::none,
                                   false /* isInternalClient */,
                                   false /* isMongos */,
                                   repl::OpTime{},
                                   repl::OpTime{});
    ASSERT_TRUE(abortBuilder.isRetryableWriteError());
    ASSERT_FALSE(commitBuilder.isTransientTransactionError());

    commandName = "clusterAbortTransaction";
    ErrorLabelBuilder clusterAbortBuilder(opCtx(),
                                          sessionInfo,
                                          commandName,
                                          ErrorCodes::NotWritablePrimary,
                                          boost::none,
                                          false /* isInternalClient */,
                                          true /* isMongos */,
                                          repl::OpTime{},
                                          repl::OpTime{});
    ASSERT_TRUE(commitBuilder.isRetryableWriteError());
    ASSERT_FALSE(commitBuilder.isTransientTransactionError());
}

TEST_F(ErrorLabelBuilderTest, NonResumableChangeStreamError) {
    OperationSessionInfoFromClient sessionInfo{LogicalSessionFromClient(UUID::gen())};
    std::string commandName;
    ErrorLabelBuilder builder(opCtx(),
                              sessionInfo,
                              commandName,
                              ErrorCodes::ChangeStreamHistoryLost,
                              boost::none,
                              false /* isInternalClient */,
                              false /* isMongos */,
                              repl::OpTime{},
                              repl::OpTime{});
    ASSERT_TRUE(builder.isNonResumableChangeStreamError());
}

TEST_F(ErrorLabelBuilderTest, ResumableChangeStreamErrorAppliesToChangeStreamAggregations) {
    OperationSessionInfoFromClient sessionInfo{LogicalSessionFromClient(UUID::gen())};
    // Build the aggregation command and confirm that it parses correctly, so we know that the error
    // is the only factor that determines the success or failure of isResumableChangeStreamError().
    auto cmdObj = BSON("aggregate" << nss().coll() << "pipeline"
                                   << BSON_ARRAY(BSON("$changeStream" << BSONObj())) << "cursor"
                                   << BSONObj() << "$db" << nss().db_forTest());
    auto aggRequest =
        uassertStatusOK(aggregation_request_helper::parseFromBSONForTests(nss(), cmdObj));
    ASSERT_TRUE(LiteParsedPipeline(aggRequest).hasChangeStream());

    // The label applies to a $changeStream "aggregate" command.
    std::string commandName = "aggregate";
    setCommand(cmdObj);
    ErrorLabelBuilder resumableAggBuilder(opCtx(),
                                          sessionInfo,
                                          commandName,
                                          ErrorCodes::NetworkTimeout,
                                          boost::none,
                                          false /* isInternalClient */,
                                          false /* isMongos */,
                                          repl::OpTime{},
                                          repl::OpTime{});
    ASSERT_TRUE(resumableAggBuilder.isResumableChangeStreamError());
    // The label applies to a "getMore" command on a $changeStream cursor.
    commandName = "getMore";
    setGetMore(cmdObj);
    ErrorLabelBuilder resumableGetMoreBuilder(opCtx(),
                                              sessionInfo,
                                              commandName,
                                              ErrorCodes::NetworkTimeout,
                                              boost::none,
                                              false /* isInternalClient */,
                                              false /* isMongos */,
                                              repl::OpTime{},
                                              repl::OpTime{});
    ASSERT_TRUE(resumableGetMoreBuilder.isResumableChangeStreamError());
}

TEST_F(ErrorLabelBuilderTest, ResumableChangeStreamErrorDoesNotApplyToNonResumableErrors) {
    OperationSessionInfoFromClient sessionInfo{LogicalSessionFromClient(UUID::gen())};
    // Build the aggregation command and confirm that it parses correctly, so we know that the error
    // is the only factor that determines the success or failure of isResumableChangeStreamError().
    auto cmdObj = BSON("aggregate" << nss().coll() << "pipeline"
                                   << BSON_ARRAY(BSON("$changeStream" << BSONObj())) << "cursor"
                                   << BSONObj() << "$db" << nss().db_forTest());
    auto aggRequest =
        uassertStatusOK(aggregation_request_helper::parseFromBSONForTests(nss(), cmdObj));
    ASSERT_TRUE(LiteParsedPipeline(aggRequest).hasChangeStream());

    // The label does not apply to a ChangeStreamFatalError error on a $changeStream aggregation.
    std::string commandName = "aggregate";
    setCommand(cmdObj);
    ErrorLabelBuilder resumableAggBuilder(opCtx(),
                                          sessionInfo,
                                          commandName,
                                          ErrorCodes::ChangeStreamFatalError,
                                          boost::none,
                                          false /* isInternalClient */,
                                          false /* isMongos */,
                                          repl::OpTime{},
                                          repl::OpTime{});
    ASSERT_FALSE(resumableAggBuilder.isResumableChangeStreamError());
    // The label does not apply to a ChangeStreamFatalError error on a $changeStream getMore.
    commandName = "getMore";
    setGetMore(cmdObj);
    ErrorLabelBuilder resumableGetMoreBuilder(opCtx(),
                                              sessionInfo,
                                              commandName,
                                              ErrorCodes::ChangeStreamFatalError,
                                              boost::none,
                                              false /* isInternalClient */,
                                              false /* isMongos */,
                                              repl::OpTime{},
                                              repl::OpTime{});
    ASSERT_FALSE(resumableGetMoreBuilder.isResumableChangeStreamError());
}

TEST_F(ErrorLabelBuilderTest, ResumableChangeStreamErrorDoesNotApplyToNonChangeStreamAggregations) {
    OperationSessionInfoFromClient sessionInfo{LogicalSessionFromClient(UUID::gen())};
    // Build the aggregation command and confirm that it parses correctly, so we know that the error
    // is the only factor that determines the success or failure of isResumableChangeStreamError().
    auto cmdObj =
        BSON("aggregate" << nss().coll() << "pipeline" << BSON_ARRAY(BSON("$match" << BSONObj()))
                         << "cursor" << BSONObj() << "$db" << nss().db_forTest());
    auto aggRequest =
        uassertStatusOK(aggregation_request_helper::parseFromBSONForTests(nss(), cmdObj));
    ASSERT_FALSE(LiteParsedPipeline(aggRequest).hasChangeStream());

    // The label does not apply to a non-$changeStream "aggregate" command.
    std::string commandName = "aggregate";
    setCommand(cmdObj);
    ErrorLabelBuilder nonResumableAggBuilder(opCtx(),
                                             sessionInfo,
                                             commandName,
                                             ErrorCodes::NetworkTimeout,
                                             boost::none,
                                             false /* isInternalClient */,
                                             false /* isMongos */,
                                             repl::OpTime{},
                                             repl::OpTime{});
    ASSERT_FALSE(nonResumableAggBuilder.isResumableChangeStreamError());
    // The label does not apply to a "getMore" command on a non-$changeStream cursor.
    commandName = "getMore";
    setGetMore(cmdObj);
    ErrorLabelBuilder nonResumableGetMoreBuilder(opCtx(),
                                                 sessionInfo,
                                                 commandName,
                                                 ErrorCodes::NetworkTimeout,
                                                 boost::none,
                                                 false /* isInternalClient */,
                                                 false /* isMongos */,
                                                 repl::OpTime{},
                                                 repl::OpTime{});
    ASSERT_FALSE(nonResumableGetMoreBuilder.isResumableChangeStreamError());
}

TEST_F(ErrorLabelBuilderTest, ResumableChangeStreamErrorDoesNotApplyToNonAggregations) {
    OperationSessionInfoFromClient sessionInfo{LogicalSessionFromClient(UUID::gen())};
    auto cmdObj = BSON("find" << nss().coll() << "filter" << BSONObj());
    // The label does not apply to a "find" command.
    std::string commandName = "find";
    setCommand(cmdObj);
    ErrorLabelBuilder nonResumableFindBuilder(opCtx(),
                                              sessionInfo,
                                              commandName,
                                              ErrorCodes::NetworkTimeout,
                                              boost::none,
                                              false /* isInternalClient */,
                                              false /* isMongos */,
                                              repl::OpTime{},
                                              repl::OpTime{});
    ASSERT_FALSE(nonResumableFindBuilder.isResumableChangeStreamError());
    // The label does not apply to a "getMore" command on a "find" cursor.
    commandName = "getMore";
    setGetMore(cmdObj);
    ErrorLabelBuilder nonResumableGetMoreBuilder(opCtx(),
                                                 sessionInfo,
                                                 commandName,
                                                 ErrorCodes::NetworkTimeout,
                                                 boost::none,
                                                 false /* isInternalClient */,
                                                 false /* isMongos */,
                                                 repl::OpTime{},
                                                 repl::OpTime{});
    ASSERT_FALSE(nonResumableGetMoreBuilder.isResumableChangeStreamError());
}

TEST_F(ErrorLabelBuilderTest, NoWritesPerformedLabelApplied) {
    OperationSessionInfoFromClient sessionInfo{LogicalSessionFromClient(UUID::gen())};
    std::string commandName = "find";
    ErrorLabelBuilder builder(opCtx(),
                              sessionInfo,
                              commandName,
                              ErrorCodes::WriteConcernFailed,
                              ErrorCodes::WriteConcernFailed,
                              false,
                              false /* isMongos */,
                              kOpTime,
                              kOpTime);
    ASSERT_TRUE(builder.isErrorWithNoWritesPerformed());
}

TEST_F(ErrorLabelBuilderTest, NoWritesPerformedLabelNotAppliedAfterWrite) {
    OperationSessionInfoFromClient sessionInfo{LogicalSessionFromClient(UUID::gen())};
    std::string commandName = "update";
    ErrorLabelBuilder builder(opCtx(),
                              sessionInfo,
                              commandName,
                              ErrorCodes::WriteConcernFailed,
                              ErrorCodes::WriteConcernFailed,
                              false,
                              false /* isMongos */,
                              kOpTime,
                              kLaterOpTime);
    ASSERT_FALSE(builder.isErrorWithNoWritesPerformed());
}

TEST_F(ErrorLabelBuilderTest, NoWritesPerformedLabelNotAppliedIfUnknown) {
    OperationSessionInfoFromClient sessionInfo{LogicalSessionFromClient(UUID::gen())};
    std::string commandName = "update";
    ErrorLabelBuilder builder(opCtx(),
                              sessionInfo,
                              commandName,
                              ErrorCodes::WriteConcernFailed,
                              ErrorCodes::WriteConcernFailed,
                              false,
                              false /* isMongos */,
                              repl::OpTime{},
                              repl::OpTime{});
    ASSERT_FALSE(builder.isErrorWithNoWritesPerformed());
}

TEST_F(ErrorLabelBuilderTest, NoWritesPerformedAndRetryableWriteAppliesBothLabels) {
    OperationSessionInfoFromClient sessionInfo{LogicalSessionFromClient(UUID::gen())};
    sessionInfo.setTxnNumber(1);
    std::string commandName = "update";
    auto actualErrorLabels = getErrorLabels(opCtx(),
                                            sessionInfo,
                                            commandName,
                                            ErrorCodes::NotWritablePrimary,
                                            boost::none,
                                            false /* isInternalClient */,
                                            false /* isMongos */,
                                            kOpTime,
                                            kOpTime);
    BSONArrayBuilder expectedLabelArray;
    expectedLabelArray << ErrorLabel::kRetryableWrite;
    expectedLabelArray << ErrorLabel::kNoWritesPerformed;
    ASSERT_BSONOBJ_EQ(actualErrorLabels, BSON(kErrorLabelsFieldName << expectedLabelArray.arr()));
}

TEST_F(ErrorLabelBuilderTest, NoWritesPerformedNotAppliedDuringOrdinaryUpdate) {
    OperationSessionInfoFromClient sessionInfo{LogicalSessionFromClient(UUID::gen())};
    std::string commandName = "update";
    auto actualErrorLabels = getErrorLabels(opCtx(),
                                            sessionInfo,
                                            commandName,
                                            ErrorCodes::NotWritablePrimary,
                                            boost::none,
                                            false /* isInternalClient */,
                                            false /* isMongos */,
                                            kOpTime,
                                            kOpTime);
    ASSERT_BSONOBJ_EQ(actualErrorLabels, BSONObj());
}

TEST_F(ErrorLabelBuilderTest, NoWritesPerformedNotAppliedDuringTransientTransactionError) {
    OperationSessionInfoFromClient sessionInfo{LogicalSessionFromClient(UUID::gen())};
    sessionInfo.setTxnNumber(1);
    sessionInfo.setAutocommit(false);
    std::string commandName = "commitTransaction";
    auto actualErrorLabels = getErrorLabels(opCtx(),
                                            sessionInfo,
                                            commandName,
                                            ErrorCodes::NoSuchTransaction,
                                            boost::none,
                                            false /* isInternalClient */,
                                            false /* isMongos */,
                                            kOpTime,
                                            kOpTime);
    BSONArrayBuilder expectedLabelArray;
    expectedLabelArray << ErrorLabel::kTransientTransaction;
    ASSERT_BSONOBJ_EQ(actualErrorLabels, BSON(kErrorLabelsFieldName << expectedLabelArray.arr()));
}

}  // namespace
}  // namespace mongo
