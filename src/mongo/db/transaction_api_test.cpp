/**
 *    Copyright (C) 2021-present MongoDB, Inc.
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
#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"
#include "mongo/db/service_context_test_fixture.h"
#include "mongo/db/transaction_api.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/stdx/future.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/executor_test_util.h"
#include "mongo/util/fail_point.h"

namespace mongo {

const BSONObj kOKCommandResponse = BSON("ok" << 1);

const BSONObj kOKInsertResponse = BSON("ok" << 1 << "n" << 1);

const BSONObj kResWithBadValueError = BSON("ok" << 0 << "code" << ErrorCodes::BadValue);

const BSONObj kNoSuchTransactionResponse =
    BSON("ok" << 0 << "code" << ErrorCodes::NoSuchTransaction << kErrorLabelsFieldName
              << BSON_ARRAY(ErrorLabel::kTransientTransaction));

const BSONObj kWriteConcernError = BSON("code" << ErrorCodes::WriteConcernFailed << "errmsg"
                                               << "mock");
const BSONObj kResWithWriteConcernError =
    BSON("ok" << 1 << "writeConcernError" << kWriteConcernError);

const BSONObj kRetryableWriteConcernError =
    BSON("code" << ErrorCodes::PrimarySteppedDown << "errmsg"
                << "mock");
const BSONObj kResWithRetryableWriteConcernError =
    BSON("ok" << 1 << "writeConcernError" << kRetryableWriteConcernError);

namespace txn_api::details {

class MockTransactionClient : public TransactionClient {
public:
    virtual void injectHooks(std::unique_ptr<TxnMetadataHooks> hooks) override {
        _hooks = std::move(hooks);
    }

    virtual SemiFuture<BSONObj> runCommand(StringData dbName, BSONObj cmd) const override {
        auto cmdBob = BSONObjBuilder(cmd);
        invariant(_hooks);
        _hooks->runRequestHook(&cmdBob);
        _lastSentRequest = cmdBob.obj();

        auto nextResponse = _nextResponse;
        if (!_secondResponse.isEmpty()) {
            _nextResponse = _secondResponse;
            _secondResponse = BSONObj();
        }
        _hooks->runReplyHook(nextResponse);
        return SemiFuture<BSONObj>::makeReady(nextResponse);
    }

    virtual SemiFuture<BatchedCommandResponse> runCRUDOp(
        const BatchedCommandRequest& cmd, std::vector<StmtId> stmtIds) const override {
        MONGO_UNREACHABLE;
    }

    virtual SemiFuture<std::vector<BSONObj>> exhaustiveFind(
        const FindCommandRequest& cmd) const override {
        MONGO_UNREACHABLE;
    }

    BSONObj getLastSentRequest() {
        return _lastSentRequest;
    }

    void setNextCommandResponse(BSONObj res) {
        _nextResponse = res;
    }

    void setSecondCommandResponse(BSONObj res) {
        _secondResponse = res;
    }

private:
    std::unique_ptr<TxnMetadataHooks> _hooks;
    mutable BSONObj _nextResponse;
    mutable BSONObj _secondResponse;
    mutable BSONObj _lastSentRequest;
};

}  // namespace txn_api::details

namespace {

void assertTxnMetadata(BSONObj obj,
                       TxnNumber txnNumber,
                       boost::optional<TxnRetryCounter> txnRetryCounter,
                       boost::optional<bool> startTransaction,
                       boost::optional<BSONObj> readConcern = boost::none,
                       boost::optional<BSONObj> writeConcern = boost::none) {
    ASSERT_EQ(obj["lsid"].type(), BSONType::Object);
    ASSERT_EQ(obj["autocommit"].Bool(), false);
    ASSERT_EQ(obj["txnNumber"].Long(), txnNumber);

    if (txnRetryCounter) {
        ASSERT_EQ(obj["txnRetryCounter"].Int(), *txnRetryCounter);
    } else {
        ASSERT(obj["txnRetryCounter"].eoo());
    }

    if (startTransaction) {
        ASSERT_EQ(obj["startTransaction"].Bool(), *startTransaction);
    } else {
        ASSERT(obj["startTransaction"].eoo());
    }

    if (readConcern) {
        ASSERT_BSONOBJ_EQ(obj["readConcern"].Obj(), *readConcern);
    } else if (startTransaction) {
        // If we didn't expect an explicit read concern, the startTransaction request should still
        // send the implicit default read concern.
        ASSERT_BSONOBJ_EQ(obj["readConcern"].Obj(), repl::ReadConcernArgs::kImplicitDefault);
    } else {
        ASSERT(obj["readConcern"].eoo());
    }

    if (writeConcern) {
        ASSERT_BSONOBJ_EQ(obj["writeConcern"].Obj(), *writeConcern);
    } else {
        ASSERT(obj["writeConcern"].eoo());
    }
}

class TxnAPITest : public ServiceContextTest {
protected:
    void setUp() final {
        ServiceContextTest::setUp();

        _opCtx = makeOperationContext();

        auto mockClient = std::make_unique<txn_api::details::MockTransactionClient>();
        _mockClient = mockClient.get();
        _txnWithRetries = std::make_shared<txn_api::TransactionWithRetries>(
            opCtx(), InlineQueuedCountingExecutor::make(), std::move(mockClient));
    }

    OperationContext* opCtx() {
        return _opCtx.get();
    }

    txn_api::details::MockTransactionClient* mockClient() {
        return _mockClient;
    }

    txn_api::TransactionWithRetries& txnWithRetries() {
        return *_txnWithRetries;
    }

    void resetTxnWithRetries() {
        auto mockClient = std::make_unique<txn_api::details::MockTransactionClient>();
        _mockClient = mockClient.get();
        _txnWithRetries = std::make_shared<txn_api::TransactionWithRetries>(
            opCtx(), InlineQueuedCountingExecutor::make(), std::move(mockClient));
    }

    void expectSentAbort(TxnNumber txnNumber,
                         TxnRetryCounter txnRetryCounter,
                         BSONObj writeConcern) {
        auto lastRequest = mockClient()->getLastSentRequest();
        assertTxnMetadata(lastRequest,
                          txnNumber,
                          txnRetryCounter,
                          boost::none /* startTransaction */,
                          boost::none /* readConcern */,
                          writeConcern);
        ASSERT_EQ(lastRequest.firstElementFieldNameStringData(), "abortTransaction"_sd);
    }

private:
    ServiceContext::UniqueOperationContext _opCtx;
    txn_api::details::MockTransactionClient* _mockClient;
    std::shared_ptr<txn_api::TransactionWithRetries> _txnWithRetries;
};

TEST_F(TxnAPITest, OwnSession_AttachesTxnMetadata) {
    auto swResult = txnWithRetries().runSyncNoThrow(
        opCtx(), [&](const txn_api::TransactionClient& txnClient, ExecutorPtr txnExec) {
            mockClient()->setNextCommandResponse(kOKInsertResponse);
            auto insertRes = txnClient
                                 .runCommand("user"_sd,
                                             BSON("insert"
                                                  << "foo"
                                                  << "documents" << BSON_ARRAY(BSON("x" << 1))))
                                 .get();
            ASSERT_EQ(insertRes["n"].Int(), 1);  // Verify the mocked response was returned.
            assertTxnMetadata(mockClient()->getLastSentRequest(),
                              0 /* txnNumber */,
                              0 /* txnRetryCounter */,
                              true /* startTransaction */);

            mockClient()->setNextCommandResponse(kOKInsertResponse);
            insertRes = txnClient
                            .runCommand("user"_sd,
                                        BSON("insert"
                                             << "foo"
                                             << "documents" << BSON_ARRAY(BSON("x" << 1))))
                            .get();
            ASSERT_EQ(insertRes["n"].Int(), 1);  // Verify the mocked response was returned.
            assertTxnMetadata(mockClient()->getLastSentRequest(),
                              0 /* txnNumber */,
                              0 /* txnRetryCounter */,
                              boost::none /* startTransaction */);

            // The commit response.
            mockClient()->setNextCommandResponse(kOKCommandResponse);
            return SemiFuture<void>::makeReady();
        });
    ASSERT(swResult.getStatus().isOK());
    ASSERT(swResult.getValue().getEffectiveStatus().isOK());

    auto lastRequest = mockClient()->getLastSentRequest();
    assertTxnMetadata(lastRequest,
                      0 /* txnNumber */,
                      0 /* txnRetryCounter */,
                      boost::none /* startTransaction */,
                      boost::none /* readConcern */,
                      WriteConcernOptions().toBSON() /* writeConcern */);
    ASSERT_EQ(lastRequest.firstElementFieldNameStringData(), "commitTransaction"_sd);
}

TEST_F(TxnAPITest, OwnSession_AttachesWriteConcernOnCommit) {
    const std::vector<WriteConcernOptions> writeConcernOptions = {
        WriteConcernOptions(1, WriteConcernOptions::SyncMode::JOURNAL, 100),
        WriteConcernOptions("majority", WriteConcernOptions::SyncMode::FSYNC, 0),
        WriteConcernOptions(2, WriteConcernOptions::SyncMode::NONE, -1)};

    for (const auto& writeConcern : writeConcernOptions) {
        opCtx()->setWriteConcern(writeConcern);

        resetTxnWithRetries();

        int attempt = -1;
        auto swResult = txnWithRetries().runSyncNoThrow(
            opCtx(), [&](const txn_api::TransactionClient& txnClient, ExecutorPtr txnExec) {
                attempt += 1;

                // No write concern on requests prior to commit/abort.
                mockClient()->setNextCommandResponse(kOKInsertResponse);
                auto insertRes = txnClient
                                     .runCommand("user"_sd,
                                                 BSON("insert"
                                                      << "foo"
                                                      << "documents" << BSON_ARRAY(BSON("x" << 1))))
                                     .get();
                ASSERT_EQ(insertRes["n"].Int(), 1);  // Verify the mocked response was returned.
                assertTxnMetadata(mockClient()->getLastSentRequest(),
                                  attempt /* txnNumber */,
                                  0 /* txnRetryCounter */,
                                  true /* startTransaction */);

                mockClient()->setNextCommandResponse(kOKInsertResponse);
                insertRes = txnClient
                                .runCommand("user"_sd,
                                            BSON("insert"
                                                 << "foo"
                                                 << "documents" << BSON_ARRAY(BSON("x" << 1))))
                                .get();
                ASSERT_EQ(insertRes["n"].Int(), 1);  // Verify the mocked response was returned.
                assertTxnMetadata(mockClient()->getLastSentRequest(),
                                  attempt /* txnNumber */,
                                  0 /* txnRetryCounter */,
                                  boost::none /* startTransaction */);

                // Throw a transient error to verify the retries behavior.
                uassert(ErrorCodes::HostUnreachable, "Mock network error", attempt != 0);

                // The commit response.
                mockClient()->setNextCommandResponse(kOKCommandResponse);
                return SemiFuture<void>::makeReady();
            });
        ASSERT(swResult.getStatus().isOK());
        ASSERT(swResult.getValue().getEffectiveStatus().isOK());

        auto lastRequest = mockClient()->getLastSentRequest();
        assertTxnMetadata(lastRequest,
                          attempt /* txnNumber */,
                          0 /* txnRetryCounter */,
                          boost::none /* startTransaction */,
                          boost::none /* readConcern */,
                          writeConcern.toBSON());
        ASSERT_EQ(lastRequest.firstElementFieldNameStringData(), "commitTransaction"_sd);
    }
}

TEST_F(TxnAPITest, OwnSession_AttachesWriteConcernOnAbort) {
    const std::vector<WriteConcernOptions> writeConcernOptions = {
        WriteConcernOptions(1, WriteConcernOptions::SyncMode::JOURNAL, 100),
        WriteConcernOptions("majority", WriteConcernOptions::SyncMode::FSYNC, 0),
        WriteConcernOptions(2, WriteConcernOptions::SyncMode::NONE, -1)};

    for (const auto& writeConcern : writeConcernOptions) {
        opCtx()->setWriteConcern(writeConcern);

        resetTxnWithRetries();
        auto swResult = txnWithRetries().runSyncNoThrow(
            opCtx(), [&](const txn_api::TransactionClient& txnClient, ExecutorPtr txnExec) {
                mockClient()->setNextCommandResponse(kOKInsertResponse);
                auto insertRes = txnClient
                                     .runCommand("user"_sd,
                                                 BSON("insert"
                                                      << "foo"
                                                      << "documents" << BSON_ARRAY(BSON("x" << 1))))
                                     .get();
                ASSERT_EQ(insertRes["n"].Int(), 1);  // Verify the mocked response was returned.

                mockClient()->setSecondCommandResponse(
                    kOKCommandResponse);  // Best effort abort response.

                uasserted(ErrorCodes::InternalError, "Mock error");
                return SemiFuture<void>::makeReady();
            });
        ASSERT_EQ(swResult.getStatus(), ErrorCodes::InternalError);

        expectSentAbort(0 /* txnNumber */, 0 /* txnRetryCounter */, writeConcern.toBSON());
    }
}

TEST_F(TxnAPITest, OwnSession_AttachesReadConcernOnStartTransaction) {
    const std::vector<repl::ReadConcernLevel> readConcernLevels = {
        repl::ReadConcernLevel::kLocalReadConcern,
        repl::ReadConcernLevel::kMajorityReadConcern,
        repl::ReadConcernLevel::kSnapshotReadConcern};

    for (const auto& readConcernLevel : readConcernLevels) {
        auto readConcern = repl::ReadConcernArgs(readConcernLevel);
        repl::ReadConcernArgs::get(opCtx()) = readConcern;

        resetTxnWithRetries();

        int attempt = -1;
        auto swResult = txnWithRetries().runSyncNoThrow(
            opCtx(), [&](const txn_api::TransactionClient& txnClient, ExecutorPtr txnExec) {
                attempt += 1;
                mockClient()->setNextCommandResponse(kOKInsertResponse);
                auto insertRes = txnClient
                                     .runCommand("user"_sd,
                                                 BSON("insert"
                                                      << "foo"
                                                      << "documents" << BSON_ARRAY(BSON("x" << 1))))
                                     .get();
                ASSERT_EQ(insertRes["n"].Int(), 1);  // Verify the mocked response was returned.
                assertTxnMetadata(mockClient()->getLastSentRequest(),
                                  attempt /* txnNumber */,
                                  0 /* txnRetryCounter */,
                                  true /* startTransaction */,
                                  readConcern.toBSONInner());

                // Subsequent requests shouldn't have a read concern.
                mockClient()->setNextCommandResponse(kOKInsertResponse);
                insertRes = txnClient
                                .runCommand("user"_sd,
                                            BSON("insert"
                                                 << "foo"
                                                 << "documents" << BSON_ARRAY(BSON("x" << 1))))
                                .get();
                ASSERT_EQ(insertRes["n"].Int(), 1);  // Verify the mocked response was returned.
                assertTxnMetadata(mockClient()->getLastSentRequest(),
                                  attempt /* txnNumber */,
                                  0 /* txnRetryCounter */,
                                  boost::none /* startTransaction */);

                // Throw a transient error to verify the retry will still use the read concern.
                uassert(ErrorCodes::HostUnreachable, "Mock network error", attempt != 0);

                // The commit response.
                mockClient()->setNextCommandResponse(kOKCommandResponse);
                return SemiFuture<void>::makeReady();
            });
        ASSERT(swResult.getStatus().isOK());
        ASSERT(swResult.getValue().getEffectiveStatus().isOK());

        auto lastRequest = mockClient()->getLastSentRequest();
        assertTxnMetadata(lastRequest,
                          attempt /* txnNumber */,
                          0 /* txnRetryCounter */,
                          boost::none /* startTransaction */,
                          boost::none /* readConcern */,
                          WriteConcernOptions().toBSON() /* writeConcern */);
        ASSERT_EQ(lastRequest.firstElementFieldNameStringData(), "commitTransaction"_sd);
    }
}

TEST_F(TxnAPITest, OwnSession_AbortsOnError) {
    auto swResult = txnWithRetries().runSyncNoThrow(
        opCtx(), [&](const txn_api::TransactionClient& txnClient, ExecutorPtr txnExec) {
            mockClient()->setNextCommandResponse(kOKInsertResponse);
            auto insertRes = txnClient
                                 .runCommand("user"_sd,
                                             BSON("insert"
                                                  << "foo"
                                                  << "documents" << BSON_ARRAY(BSON("x" << 1))))
                                 .get();
            ASSERT_EQ(insertRes["n"].Int(), 1);  // Verify the mocked response was returned.

            // The best effort abort response, the client should ignore this.
            mockClient()->setNextCommandResponse(kResWithBadValueError);

            uasserted(ErrorCodes::InternalError, "Mock error");
            return SemiFuture<void>::makeReady();
        });
    ASSERT_EQ(swResult.getStatus(), ErrorCodes::InternalError);

    expectSentAbort(0 /* txnNumber */, 0 /* txnRetryCounter */, WriteConcernOptions().toBSON());
}

TEST_F(TxnAPITest, OwnSession_SkipsCommitIfNoCommandsWereRun) {
    auto swResult = txnWithRetries().runSyncNoThrow(
        opCtx(), [&](const txn_api::TransactionClient& txnClient, ExecutorPtr txnExec) {
            // The commit response, the client should not receive this.
            mockClient()->setNextCommandResponse(kResWithBadValueError);

            uasserted(ErrorCodes::InternalError, "Mock error");
            return SemiFuture<void>::makeReady();
        });
    ASSERT_EQ(swResult.getStatus(), ErrorCodes::InternalError);

    auto lastRequest = mockClient()->getLastSentRequest();
    ASSERT(lastRequest.isEmpty());
}

TEST_F(TxnAPITest, OwnSession_RetriesOnTransientError) {
    int attempt = -1;
    auto swResult = txnWithRetries().runSyncNoThrow(
        opCtx(), [&](const txn_api::TransactionClient& txnClient, ExecutorPtr txnExec) {
            attempt += 1;
            if (attempt > 0) {
                // Verify an abort was sent in between retries.
                expectSentAbort(attempt - 1 /* txnNumber */,
                                0 /* txnRetryCounter */,
                                WriteConcernOptions().toBSON());
            }

            mockClient()->setNextCommandResponse(attempt == 0 ? kNoSuchTransactionResponse
                                                              : kOKInsertResponse);
            auto insertRes = txnClient
                                 .runCommand("user"_sd,
                                             BSON("insert"
                                                  << "foo"
                                                  << "documents" << BSON_ARRAY(BSON("x" << 1))))
                                 .get();

            // The commit or implicit abort response.
            mockClient()->setNextCommandResponse(kOKCommandResponse);

            if (attempt == 0) {
                uassertStatusOK(getStatusFromWriteCommandReply(insertRes));
            } else {
                ASSERT_OK(getStatusFromWriteCommandReply(insertRes));
            }

            ASSERT_EQ(insertRes["n"].Int(), 1);  // Verify the mocked response was returned.
            assertTxnMetadata(mockClient()->getLastSentRequest(),
                              attempt /* txnNumber */,
                              0 /* txnRetryCounter */,
                              true /* startTransaction */);

            return SemiFuture<void>::makeReady();
        });
    ASSERT(swResult.getStatus().isOK());
    ASSERT(swResult.getValue().getEffectiveStatus().isOK());

    auto lastRequest = mockClient()->getLastSentRequest();
    assertTxnMetadata(lastRequest,
                      attempt /* txnNumber */,
                      0 /* txnRetryCounter */,
                      boost::none /* startTransaction */,
                      boost::none /* readConcern */,
                      WriteConcernOptions().toBSON() /* writeConcern */);
    ASSERT_EQ(lastRequest.firstElementFieldNameStringData(), "commitTransaction"_sd);
}

TEST_F(TxnAPITest, OwnSession_RetriesOnTransientClientError) {
    int attempt = -1;
    auto swResult = txnWithRetries().runSyncNoThrow(
        opCtx(), [&](const txn_api::TransactionClient& txnClient, ExecutorPtr txnExec) {
            attempt += 1;
            if (attempt > 0) {
                // Verify an abort was sent in between retries.
                expectSentAbort(attempt - 1 /* txnNumber */,
                                0 /* txnRetryCounter */,
                                WriteConcernOptions().toBSON());
            }

            mockClient()->setNextCommandResponse(kOKInsertResponse);
            auto insertRes = txnClient
                                 .runCommand("user"_sd,
                                             BSON("insert"
                                                  << "foo"
                                                  << "documents" << BSON_ARRAY(BSON("x" << 1))))
                                 .get();
            ASSERT_OK(getStatusFromWriteCommandReply(insertRes));

            ASSERT_EQ(insertRes["n"].Int(), 1);  // Verify the mocked response was returned.
            assertTxnMetadata(mockClient()->getLastSentRequest(),
                              attempt /* txnNumber */,
                              0 /* txnRetryCounter */,
                              true /* startTransaction */);

            // The commit or implicit abort response.
            mockClient()->setNextCommandResponse(kOKCommandResponse);

            uassert(ErrorCodes::HostUnreachable, "Mock network error", attempt != 0);
            return SemiFuture<void>::makeReady();
        });
    ASSERT(swResult.getStatus().isOK());
    ASSERT(swResult.getValue().getEffectiveStatus().isOK());

    auto lastRequest = mockClient()->getLastSentRequest();
    assertTxnMetadata(lastRequest,
                      attempt /* txnNumber */,
                      0 /* txnRetryCounter */,
                      boost::none /* startTransaction */,
                      boost::none /* readConcern */,
                      WriteConcernOptions().toBSON() /* writeConcern */);
    ASSERT_EQ(lastRequest.firstElementFieldNameStringData(), "commitTransaction"_sd);
}

TEST_F(TxnAPITest, OwnSession_CommitError) {
    auto swResult = txnWithRetries().runSyncNoThrow(
        opCtx(), [&](const txn_api::TransactionClient& txnClient, ExecutorPtr txnExec) {
            mockClient()->setNextCommandResponse(kOKInsertResponse);
            auto insertRes = txnClient
                                 .runCommand("user"_sd,
                                             BSON("insert"
                                                  << "foo"
                                                  << "documents" << BSON_ARRAY(BSON("x" << 1))))
                                 .get();
            ASSERT_OK(getStatusFromWriteCommandReply(insertRes));

            ASSERT_EQ(insertRes["n"].Int(), 1);  // Verify the mocked response was returned.
            assertTxnMetadata(mockClient()->getLastSentRequest(),
                              0 /* txnNumber */,
                              0 /* txnRetryCounter */,
                              true /* startTransaction */);

            // The commit response.
            mockClient()->setNextCommandResponse(
                BSON("ok" << 0 << "code" << ErrorCodes::InternalError));

            // The best effort abort response, the client should ignore this.
            mockClient()->setSecondCommandResponse(kResWithBadValueError);
            return SemiFuture<void>::makeReady();
        });
    ASSERT(swResult.getStatus().isOK());
    ASSERT_EQ(swResult.getValue().cmdStatus, ErrorCodes::InternalError);
    ASSERT(swResult.getValue().wcError.toStatus().isOK());
    ASSERT_EQ(swResult.getValue().getEffectiveStatus(), ErrorCodes::InternalError);

    expectSentAbort(0 /* txnNumber */, 0 /* txnRetryCounter */, WriteConcernOptions().toBSON());
}

TEST_F(TxnAPITest, OwnSession_TransientCommitError) {
    int attempt = -1;
    auto swResult = txnWithRetries().runSyncNoThrow(
        opCtx(), [&](const txn_api::TransactionClient& txnClient, ExecutorPtr txnExec) {
            attempt += 1;
            if (attempt > 0) {
                // Verify an abort was sent in between retries.
                expectSentAbort(attempt - 1 /* txnNumber */,
                                0 /* txnRetryCounter */,
                                WriteConcernOptions().toBSON());
            }

            mockClient()->setNextCommandResponse(kOKInsertResponse);
            auto insertRes = txnClient
                                 .runCommand("user"_sd,
                                             BSON("insert"
                                                  << "foo"
                                                  << "documents" << BSON_ARRAY(BSON("x" << 1))))
                                 .get();
            ASSERT_OK(getStatusFromWriteCommandReply(insertRes));

            ASSERT_EQ(insertRes["n"].Int(), 1);  // Verify the mocked response was returned.
            assertTxnMetadata(mockClient()->getLastSentRequest(),
                              attempt /* txnNumber */,
                              0 /* txnRetryCounter */,
                              true /* startTransaction */);

            // Set commit and best effort abort response, if necessary.
            if (attempt == 0) {
                mockClient()->setNextCommandResponse(kNoSuchTransactionResponse);
                mockClient()->setSecondCommandResponse(kOKCommandResponse);
            } else {
                mockClient()->setNextCommandResponse(kOKCommandResponse);
            }
            return SemiFuture<void>::makeReady();
        });
    ASSERT(swResult.getStatus().isOK());
    ASSERT(swResult.getValue().getEffectiveStatus().isOK());

    auto lastRequest = mockClient()->getLastSentRequest();
    assertTxnMetadata(lastRequest,
                      attempt /* txnNumber */,
                      0 /* txnRetryCounter */,
                      boost::none /* startTransaction */,
                      boost::none /* readConcern */,
                      WriteConcernOptions().toBSON() /* writeConcern */);
    ASSERT_EQ(lastRequest.firstElementFieldNameStringData(), "commitTransaction"_sd);
}

TEST_F(TxnAPITest, OwnSession_RetryableCommitError) {
    auto swResult = txnWithRetries().runSyncNoThrow(
        opCtx(), [&](const txn_api::TransactionClient& txnClient, ExecutorPtr txnExec) {
            mockClient()->setNextCommandResponse(kOKInsertResponse);
            auto insertRes = txnClient
                                 .runCommand("user"_sd,
                                             BSON("insert"
                                                  << "foo"
                                                  << "documents" << BSON_ARRAY(BSON("x" << 1))))
                                 .get();
            ASSERT_OK(getStatusFromWriteCommandReply(insertRes));

            ASSERT_EQ(insertRes["n"].Int(), 1);  // Verify the mocked response was returned.
            assertTxnMetadata(mockClient()->getLastSentRequest(),
                              0 /* txnNumber */,
                              0 /* txnRetryCounter */,
                              true /* startTransaction */);

            // The commit response.
            mockClient()->setNextCommandResponse(
                BSON("ok" << 0 << "code" << ErrorCodes::InterruptedDueToReplStateChange));
            mockClient()->setSecondCommandResponse(kOKCommandResponse);
            return SemiFuture<void>::makeReady();
        });
    ASSERT(swResult.getStatus().isOK());
    ASSERT(swResult.getValue().getEffectiveStatus().isOK());

    auto lastRequest = mockClient()->getLastSentRequest();
    assertTxnMetadata(lastRequest,
                      0 /* txnNumber */,
                      0 /* txnRetryCounter */,
                      boost::none /* startTransaction */,
                      boost::none /* readConcern */,
                      WriteConcernOptions().toBSON() /* writeConcern */);
    ASSERT_EQ(lastRequest.firstElementFieldNameStringData(), "commitTransaction"_sd);
}

TEST_F(TxnAPITest, OwnSession_NonRetryableCommitWCError) {
    auto swResult = txnWithRetries().runSyncNoThrow(
        opCtx(), [&](const txn_api::TransactionClient& txnClient, ExecutorPtr txnExec) {
            mockClient()->setNextCommandResponse(kOKInsertResponse);
            auto insertRes = txnClient
                                 .runCommand("user"_sd,
                                             BSON("insert"
                                                  << "foo"
                                                  << "documents" << BSON_ARRAY(BSON("x" << 1))))
                                 .get();
            ASSERT_EQ(insertRes["n"].Int(), 1);  // Verify the mocked response was returned.
            assertTxnMetadata(mockClient()->getLastSentRequest(),
                              0 /* txnNumber */,
                              0 /* txnRetryCounter */,
                              true /* startTransaction */);

            // The commit response.
            mockClient()->setNextCommandResponse(kResWithWriteConcernError);
            mockClient()->setSecondCommandResponse(
                kOKCommandResponse);  // Best effort abort response.
            return SemiFuture<void>::makeReady();
        });
    ASSERT(swResult.getStatus().isOK());
    ASSERT(swResult.getValue().cmdStatus.isOK());
    ASSERT_EQ(swResult.getValue().wcError.toStatus(), ErrorCodes::WriteConcernFailed);
    ASSERT_EQ(swResult.getValue().getEffectiveStatus(), ErrorCodes::WriteConcernFailed);

    expectSentAbort(0 /* txnNumber */, 0 /* txnRetryCounter */, WriteConcernOptions().toBSON());
}

TEST_F(TxnAPITest, OwnSession_RetryableCommitWCError) {
    auto swResult = txnWithRetries().runSyncNoThrow(
        opCtx(), [&](const txn_api::TransactionClient& txnClient, ExecutorPtr txnExec) {
            mockClient()->setNextCommandResponse(kOKInsertResponse);
            auto insertRes = txnClient
                                 .runCommand("user"_sd,
                                             BSON("insert"
                                                  << "foo"
                                                  << "documents" << BSON_ARRAY(BSON("x" << 1))))
                                 .get();
            ASSERT_OK(getStatusFromWriteCommandReply(insertRes));

            ASSERT_EQ(insertRes["n"].Int(), 1);  // Verify the mocked response was returned.
            assertTxnMetadata(mockClient()->getLastSentRequest(),
                              0 /* txnNumber */,
                              0 /* txnRetryCounter */,
                              true /* startTransaction */);

            // The commit responses.
            mockClient()->setNextCommandResponse(kResWithRetryableWriteConcernError);
            mockClient()->setSecondCommandResponse(kOKCommandResponse);
            return SemiFuture<void>::makeReady();
        });
    ASSERT(swResult.getStatus().isOK());
    ASSERT(swResult.getValue().getEffectiveStatus().isOK());

    auto lastRequest = mockClient()->getLastSentRequest();
    assertTxnMetadata(lastRequest,
                      0 /* txnNumber */,
                      0 /* txnRetryCounter */,
                      boost::none /* startTransaction */,
                      boost::none /* readConcern */,
                      WriteConcernOptions().toBSON() /* writeConcern */);
    ASSERT_EQ(lastRequest.firstElementFieldNameStringData(), "commitTransaction"_sd);
}

TEST_F(TxnAPITest, RunSyncNoErrors) {
    txnWithRetries().runSync(opCtx(),
                             [&](const txn_api::TransactionClient& txnClient, ExecutorPtr txnExec) {
                                 return SemiFuture<void>::makeReady();
                             });
}

TEST_F(TxnAPITest, RunSyncThrowsOnBodyError) {
    ASSERT_THROWS_CODE(txnWithRetries().runSync(
                           opCtx(),
                           [&](const txn_api::TransactionClient& txnClient, ExecutorPtr txnExec) {
                               uasserted(ErrorCodes::InternalError, "Mock error");
                               return SemiFuture<void>::makeReady();
                           }),
                       DBException,
                       ErrorCodes::InternalError);
}

TEST_F(TxnAPITest, RunSyncThrowsOnCommitCmdError) {
    ASSERT_THROWS_CODE(txnWithRetries().runSync(
                           opCtx(),
                           [&](const txn_api::TransactionClient& txnClient, ExecutorPtr txnExec) {
                               mockClient()->setNextCommandResponse(kOKInsertResponse);
                               auto insertRes = txnClient
                                                    .runCommand("user"_sd,
                                                                BSON("insert"
                                                                     << "foo"
                                                                     << "documents"
                                                                     << BSON_ARRAY(BSON("x" << 1))))
                                                    .get();

                               // The commit response.
                               mockClient()->setNextCommandResponse(
                                   BSON("ok" << 0 << "code" << ErrorCodes::InternalError));
                               mockClient()->setSecondCommandResponse(
                                   kOKCommandResponse);  // Best effort abort response.
                               return SemiFuture<void>::makeReady();
                           }),
                       DBException,
                       ErrorCodes::InternalError);
}

TEST_F(TxnAPITest, RunSyncThrowsOnCommitWCError) {
    ASSERT_THROWS_CODE(txnWithRetries().runSync(
                           opCtx(),
                           [&](const txn_api::TransactionClient& txnClient, ExecutorPtr txnExec) {
                               mockClient()->setNextCommandResponse(kOKInsertResponse);
                               auto insertRes = txnClient
                                                    .runCommand("user"_sd,
                                                                BSON("insert"
                                                                     << "foo"
                                                                     << "documents"
                                                                     << BSON_ARRAY(BSON("x" << 1))))
                                                    .get();

                               // The commit response.
                               mockClient()->setNextCommandResponse(kResWithWriteConcernError);
                               mockClient()->setSecondCommandResponse(
                                   kOKCommandResponse);  // Best effort abort response.
                               return SemiFuture<void>::makeReady();
                           }),
                       DBException,
                       ErrorCodes::WriteConcernFailed);
}

}  // namespace
}  // namespace mongo
