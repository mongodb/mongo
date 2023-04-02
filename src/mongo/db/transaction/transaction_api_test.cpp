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

#include <queue>

#include "mongo/config.h"
#include "mongo/db/commands.h"
#include "mongo/db/commands/txn_cmds_gen.h"
#include "mongo/db/error_labels.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/s/operation_sharding_state.h"
#include "mongo/db/service_context.h"
#include "mongo/db/service_context_test_fixture.h"
#include "mongo/db/session/logical_session_id_helpers.h"
#include "mongo/db/transaction/internal_transaction_metrics.h"
#include "mongo/db/transaction/transaction_api.h"
#include "mongo/executor/network_interface_factory.h"
#include "mongo/executor/thread_pool_task_executor.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/s/is_mongos.h"
#include "mongo/stdx/future.h"
#include "mongo/stdx/thread.h"
#include "mongo/unittest/barrier.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/clock_source_mock.h"
#include "mongo/util/concurrency/thread_pool.h"
#include "mongo/util/executor_test_util.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/scopeguard.h"

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

class MockResourceYielder : public ResourceYielder {
public:
    void yield(OperationContext*) {
        _timesYielded++;

        if (_skipNTimes > 0) {
            _skipNTimes--;
            return;
        }

        auto error = _yieldError.load();
        if (error != ErrorCodes::OK) {
            uasserted(error, "Simulated yield error");
        }
    }

    void unyield(OperationContext*) {
        _timesUnyielded++;

        if (_skipNTimes > 0) {
            _skipNTimes--;
            return;
        }

        auto error = _unyieldError.load();
        if (error != ErrorCodes::OK) {
            uasserted(error, "Simulated unyield error");
        }
    }

    void skipNTimes(int skip) {
        _skipNTimes = skip;
    }

    void throwInYield(ErrorCodes::Error error = ErrorCodes::Interrupted) {
        _yieldError.store(error);
    }

    void throwInUnyield(ErrorCodes::Error error = ErrorCodes::Interrupted) {
        _unyieldError.store(error);
    }

    auto timesYielded() {
        return _timesYielded;
    }

    auto timesUnyielded() {
        return _timesUnyielded;
    }

private:
    int _skipNTimes{0};
    int _timesYielded{0};
    int _timesUnyielded{0};
    AtomicWord<ErrorCodes::Error> _yieldError{ErrorCodes::OK};
    AtomicWord<ErrorCodes::Error> _unyieldError{ErrorCodes::OK};
};

namespace txn_api::details {

class MockTransactionClient : public SEPTransactionClient {
public:
    using SEPTransactionClient::SEPTransactionClient;

    virtual void initialize(std::unique_ptr<TxnHooks> hooks) override {
        _hooks = std::move(hooks);
    }

    virtual SemiFuture<BSONObj> runCommand(const DatabaseName& dbName, BSONObj cmd) const override {
        stdx::unique_lock<Latch> ul(_mutex);
        [&]() {
            StringData cmdName = cmd.firstElementFieldNameStringData();
            if (cmdName != AbortTransaction::kCommandName) {
                // Only hang abort commands.
                return;
            }

            if (_hangNextAbortCommand) {
                // Tests that expect to hang an abort must use the barrier to synchronize since the
                // abort runs on a different thread.
                _hitHungAbort.countDownAndWait();
            }
            _hangNextAbortCommandCV.wait(ul, [&] { return !_hangNextAbortCommand; });
        }();

        auto cmdBob = BSONObjBuilder(std::move(cmd));
        invariant(_hooks);
        _hooks->runRequestHook(&cmdBob);
        _sentRequests.emplace_back(cmdBob.obj());

        auto nextResponse = [&] {
            if (_responses.empty()) {
                // Some tests reuse responses so return the last returned response when empty.
                return _lastResponse;
            }
            auto response = _responses.front();
            _responses.pop();
            _lastResponse = response;
            return response;
        }();

        if (!nextResponse.isOK()) {
            return SemiFuture<BSONObj>::makeReady(nextResponse);
        }

        auto nextResponseRes = nextResponse.getValue();
        _hooks->runReplyHook(nextResponseRes);
        return SemiFuture<BSONObj>::makeReady(nextResponseRes);
    }

    virtual BSONObj runCommandSync(const DatabaseName& dbName, BSONObj cmd) const override {
        MONGO_UNREACHABLE;
    }

    virtual SemiFuture<BatchedCommandResponse> runCRUDOp(
        const BatchedCommandRequest& cmd, std::vector<StmtId> stmtIds) const override {
        MONGO_UNREACHABLE;
    }

    virtual BatchedCommandResponse runCRUDOpSync(const BatchedCommandRequest& cmd,
                                                 std::vector<StmtId> stmtIds) const override {
        MONGO_UNREACHABLE;
    }

    virtual bool supportsClientTransactionContext() const override {
        return true;
    }

    virtual bool runsClusterOperations() const override {
        return false;
    }

    BSONObj getLastSentRequest() {
        stdx::lock_guard<Latch> lg(_mutex);
        if (_sentRequests.empty()) {
            return BSONObj();
        }
        return _sentRequests.back();
    }

    const std::vector<BSONObj>& getSentRequests() {
        stdx::lock_guard<Latch> lg(_mutex);
        return _sentRequests;
    }

    void setNextCommandResponse(StatusWith<BSONObj> res) {
        stdx::lock_guard<Latch> lg(_mutex);
        _responses.push(res);
    }

    void setHangNextAbortCommand(bool enable) {
        stdx::lock_guard<Latch> lg(_mutex);
        _hangNextAbortCommand = enable;

        // Wake up any waiting threads.
        _hangNextAbortCommandCV.notify_all();
    }

    void waitForHungAbortWaiter() {
        _hitHungAbort.countDownAndWait();
    }

private:
    std::unique_ptr<TxnHooks> _hooks;
    mutable StatusWith<BSONObj> _lastResponse{BSONObj()};
    mutable std::queue<StatusWith<BSONObj>> _responses;
    mutable std::vector<BSONObj> _sentRequests;
    bool _runningLocalTransaction{false};

    mutable Mutex _mutex = MONGO_MAKE_LATCH("MockTransactionClient");
    mutable stdx::condition_variable _hangNextAbortCommandCV;
    bool _hangNextAbortCommand{false};
    mutable unittest::Barrier _hitHungAbort{2};
};

}  // namespace txn_api::details

namespace {

LogicalSessionId getLsid(BSONObj obj) {
    auto osi = OperationSessionInfo::parse(IDLParserContext{"assertSessionIdMetadata"}, obj);
    ASSERT(osi.getSessionId());
    return *osi.getSessionId();
}

// TODO SERVER-64017: Make these consistent with the unified terms for internal sessions.
enum class LsidAssertion {
    kStandalone,
    kNonRetryableChild,
    kRetryableChild,
};
void assertSessionIdMetadata(BSONObj obj,
                             LsidAssertion lsidAssert,
                             boost::optional<LogicalSessionId> parentLsid = boost::none,
                             boost::optional<TxnNumber> parentTxnNumber = boost::none) {
    auto lsid = getLsid(obj);

    switch (lsidAssert) {
        case LsidAssertion::kStandalone:
            ASSERT(!getParentSessionId(lsid)) << obj;
            break;
        case LsidAssertion::kNonRetryableChild:
            invariant(parentLsid);

            ASSERT(getParentSessionId(lsid)) << obj;
            ASSERT_EQ(*getParentSessionId(lsid), *parentLsid) << obj;
            ASSERT(isInternalSessionForNonRetryableWrite(lsid)) << obj;
            break;
        case LsidAssertion::kRetryableChild:
            invariant(parentTxnNumber);

            ASSERT(getParentSessionId(lsid)) << obj;
            ASSERT_EQ(*getParentSessionId(lsid), *parentLsid) << obj;
            ASSERT(isInternalSessionForRetryableWrite(lsid)) << obj;
            ASSERT(lsid.getTxnNumber()) << obj;
            ASSERT_EQ(*lsid.getTxnNumber(), *parentTxnNumber) << obj;
            break;
    }
}

// Assumes all fields are set in the given APIParameters.
void assertAPIParameters(BSONObj obj, boost::optional<APIParameters> expectedParams) {
    if (expectedParams) {
        ASSERT_EQ(obj[APIParametersFromClient::kApiVersionFieldName].String(),
                  expectedParams->getAPIVersion().value())
            << obj;
        ASSERT_EQ(obj[APIParametersFromClient::kApiStrictFieldName].Bool(),
                  expectedParams->getAPIStrict().value())
            << obj;
        ASSERT_EQ(obj[APIParametersFromClient::kApiDeprecationErrorsFieldName].Bool(),
                  expectedParams->getAPIDeprecationErrors().value())
            << obj;
    } else {
        ASSERT(obj[APIParametersFromClient::kApiVersionFieldName].eoo()) << obj;
        ASSERT(obj[APIParametersFromClient::kApiStrictFieldName].eoo()) << obj;
        ASSERT(obj[APIParametersFromClient::kApiDeprecationErrorsFieldName].eoo()) << obj;
    }
}

void assertTxnMetadata(BSONObj obj,
                       TxnNumber txnNumber,
                       boost::optional<bool> startTransaction,
                       boost::optional<BSONObj> readConcern = boost::none,
                       boost::optional<BSONObj> writeConcern = boost::none,
                       boost::optional<int> maxTimeMS = boost::none) {
    ASSERT_EQ(obj["lsid"].type(), BSONType::Object) << obj;
    ASSERT_EQ(obj["autocommit"].Bool(), false) << obj;
    ASSERT_EQ(obj["txnNumber"].Long(), txnNumber) << obj;

    if (startTransaction) {
        ASSERT_EQ(obj["startTransaction"].Bool(), *startTransaction) << obj;
    } else {
        ASSERT(obj["startTransaction"].eoo()) << obj;
    }

    if (readConcern) {
        ASSERT_BSONOBJ_EQ(obj["readConcern"].Obj(), *readConcern);
    } else if (startTransaction) {
        // If we didn't expect an explicit read concern, the startTransaction request should still
        // send the implicit default read concern.
        ASSERT_BSONOBJ_EQ(obj["readConcern"].Obj(), repl::ReadConcernArgs::kImplicitDefault);
    } else {
        ASSERT(obj["readConcern"].eoo()) << obj;
    }

    if (writeConcern) {
        ASSERT_BSONOBJ_EQ(obj["writeConcern"].Obj(), *writeConcern);
    } else {
        ASSERT(obj["writeConcern"].eoo()) << obj;
    }

    if (maxTimeMS) {
        ASSERT_EQ(obj["maxTimeMS"].numberInt(), *maxTimeMS) << obj;
    } else {
        ASSERT(obj["maxTimeMS"].eoo()) << obj;
    }
}

class TxnAPITest : public ServiceContextTest {
protected:
    void setUp() final {
        ServiceContextTest::setUp();

        _opCtx = makeOperationContext();

        // Use a thread pool with one max thread to verify the API can run truly asynchronously on
        // its executor.
        ThreadPool::Options options;
        options.poolName = "TxnAPITest";
        options.minThreads = 1;
        options.maxThreads = 1;

        _executor = std::make_shared<executor::ThreadPoolTaskExecutor>(
            std::make_unique<ThreadPool>(std::move(options)),
            executor::makeNetworkInterface("TxnAPITestNetwork"));

        _executor->startup();
        _inlineExecutor = std::make_shared<executor::InlineExecutor>();

        _sleepInlineExecutor = _inlineExecutor->getSleepableExecutor(_executor);

        auto mockClient = std::make_unique<txn_api::details::MockTransactionClient>(
            opCtx(), _inlineExecutor, _sleepInlineExecutor, nullptr);
        _mockClient = mockClient.get();
        _txnWithRetries =
            std::make_unique<txn_api::SyncTransactionWithRetries>(opCtx(),
                                                                  _sleepInlineExecutor,
                                                                  nullptr /* resourceYielder */,
                                                                  _inlineExecutor,
                                                                  std::move(mockClient));

        // The bulk of the API tests are for the non-local transaction cases, so set isMongos=true
        // by default.
        setMongos(true);
    }

    void tearDown() override {
        setMongos(false);

        _executor->shutdown();
        _executor->join();
        _executor.reset();
    }

    void waitForAllEarlierTasksToComplete() {
        while (_executor->hasTasks()) {
            continue;
        }
    }

    OperationContext* opCtx() {
        return _opCtx.get();
    }

    txn_api::details::MockTransactionClient* mockClient() {
        return _mockClient;
    }

    MockResourceYielder* resourceYielder() {
        return _resourceYielder;
    }

    txn_api::SyncTransactionWithRetries& txnWithRetries() {
        return *_txnWithRetries;
    }

    void resetTxnWithRetries(
        std::unique_ptr<MockResourceYielder> resourceYielder = nullptr,
        std::shared_ptr<executor::InlineExecutor::SleepableExecutor> executor = nullptr) {
        auto executorToUse = _sleepInlineExecutor;


        auto mockClient = std::make_unique<txn_api::details::MockTransactionClient>(
            opCtx(), _inlineExecutor, executorToUse, nullptr);
        _mockClient = mockClient.get();
        if (resourceYielder) {
            _resourceYielder = resourceYielder.get();
        }

        // Guarantee any tasks spawned by the API have finished and the thread pool threads are
        // synchronized with the main test thread so any shared pointers held by them will be reset,
        // which should guarantee sessions are pooled deterministically.
        waitForAllEarlierTasksToComplete();

        // Reset _txnWithRetries so it returns and reacquires the same session from the session
        // pool. This ensures that we can predictably monitor txnNumber's value.
        _txnWithRetries = nullptr;
        _txnWithRetries =
            std::make_unique<txn_api::SyncTransactionWithRetries>(opCtx(),
                                                                  executorToUse,
                                                                  std::move(resourceYielder),
                                                                  _inlineExecutor,
                                                                  std::move(mockClient));
    }

    void resetTxnWithRetriesWithClient(std::unique_ptr<txn_api::TransactionClient> txnClient) {
        waitForAllEarlierTasksToComplete();
        _txnWithRetries = nullptr;
        _txnWithRetries = std::make_unique<txn_api::SyncTransactionWithRetries>(
            opCtx(), _sleepInlineExecutor, nullptr, _inlineExecutor, std::move(txnClient));
    }

    void expectSentAbort(TxnNumber txnNumber, BSONObj writeConcern) {
        auto lastRequest = mockClient()->getLastSentRequest();
        assertTxnMetadata(lastRequest,
                          txnNumber,
                          boost::none /* startTransaction */,
                          boost::none /* readConcern */,
                          writeConcern);
        ASSERT_EQ(lastRequest.firstElementFieldNameStringData(), "abortTransaction"_sd);
    }

private:
    ServiceContext::UniqueOperationContext _opCtx;
    std::shared_ptr<executor::ThreadPoolTaskExecutor> _executor;

    std::shared_ptr<executor::InlineExecutor> _inlineExecutor;
    std::shared_ptr<executor::InlineExecutor::SleepableExecutor> _sleepInlineExecutor;
    txn_api::details::MockTransactionClient* _mockClient{nullptr};
    MockResourceYielder* _resourceYielder{nullptr};
    std::unique_ptr<txn_api::SyncTransactionWithRetries> _txnWithRetries;
};

class MockClusterOperationTransactionClient : public txn_api::TransactionClient {
public:
    virtual void initialize(std::unique_ptr<txn_api::details::TxnHooks> hooks) {}

    virtual BSONObj runCommandSync(const DatabaseName& dbName, BSONObj cmd) const override {
        MONGO_UNREACHABLE;
    }

    virtual SemiFuture<BSONObj> runCommand(const DatabaseName& dbName, BSONObj cmd) const override {
        MONGO_UNREACHABLE;
    }

    virtual SemiFuture<BatchedCommandResponse> runCRUDOp(
        const BatchedCommandRequest& cmd, std::vector<StmtId> stmtIds) const override {
        MONGO_UNREACHABLE;
    }

    virtual BatchedCommandResponse runCRUDOpSync(const BatchedCommandRequest& cmd,
                                                 std::vector<StmtId> stmtIds) const override {
        MONGO_UNREACHABLE;
    }

    virtual SemiFuture<std::vector<BSONObj>> exhaustiveFind(
        const FindCommandRequest& cmd) const override {
        MONGO_UNREACHABLE;
    }

    virtual std::vector<BSONObj> exhaustiveFindSync(const FindCommandRequest& cmd) const override {
        MONGO_UNREACHABLE;
    }

    virtual bool supportsClientTransactionContext() const override {
        return true;
    }

    virtual bool runsClusterOperations() const override {
        return true;
    }
};

TEST_F(TxnAPITest, OwnSession_AttachesTxnMetadata) {
    auto swResult = txnWithRetries().runNoThrow(
        opCtx(), [&](const txn_api::TransactionClient& txnClient, ExecutorPtr txnExec) {
            mockClient()->setNextCommandResponse(kOKInsertResponse);
            auto insertRes = txnClient
                                 .runCommand(DatabaseName(boost::none, "user"_sd),
                                             BSON("insert"
                                                  << "foo"
                                                  << "documents" << BSON_ARRAY(BSON("x" << 1))))
                                 .get();
            ASSERT_EQ(insertRes["n"].Int(), 1);  // Verify the mocked response was returned.
            assertTxnMetadata(
                mockClient()->getLastSentRequest(), 0 /* txnNumber */, true /* startTransaction */);
            assertSessionIdMetadata(mockClient()->getLastSentRequest(), LsidAssertion::kStandalone);
            assertAPIParameters(mockClient()->getLastSentRequest(), boost::none);

            mockClient()->setNextCommandResponse(kOKInsertResponse);
            insertRes = txnClient
                            .runCommand(DatabaseName(boost::none, "user"_sd),
                                        BSON("insert"
                                             << "foo"
                                             << "documents" << BSON_ARRAY(BSON("x" << 1))))
                            .get();
            ASSERT_EQ(insertRes["n"].Int(), 1);  // Verify the mocked response was returned.
            assertTxnMetadata(mockClient()->getLastSentRequest(),
                              0 /* txnNumber */,
                              boost::none /* startTransaction */);
            assertSessionIdMetadata(mockClient()->getLastSentRequest(), LsidAssertion::kStandalone);
            assertAPIParameters(mockClient()->getLastSentRequest(), boost::none);

            // The commit response.
            mockClient()->setNextCommandResponse(kOKCommandResponse);
            return SemiFuture<void>::makeReady();
        });
    ASSERT(swResult.getStatus().isOK());
    ASSERT(swResult.getValue().getEffectiveStatus().isOK());

    auto lastRequest = mockClient()->getLastSentRequest();
    assertTxnMetadata(lastRequest,
                      0 /* txnNumber */,
                      boost::none /* startTransaction */,
                      boost::none /* readConcern */,
                      WriteConcernOptions().toBSON() /* writeConcern */);
    assertSessionIdMetadata(mockClient()->getLastSentRequest(), LsidAssertion::kStandalone);
    assertAPIParameters(mockClient()->getLastSentRequest(), boost::none);
    ASSERT_EQ(lastRequest.firstElementFieldNameStringData(), "commitTransaction"_sd);
}

TEST_F(TxnAPITest, AttachesAPIVersion) {
    APIParameters params = APIParameters();
    params.setAPIVersion("2");
    params.setAPIStrict(true);
    params.setAPIDeprecationErrors(true);
    APIParameters::get(opCtx()) = params;
    resetTxnWithRetries();

    int attempt = -1;
    auto swResult = txnWithRetries().runNoThrow(
        opCtx(), [&](const txn_api::TransactionClient& txnClient, ExecutorPtr txnExec) {
            attempt += 1;

            mockClient()->setNextCommandResponse(kOKInsertResponse);
            auto insertRes = txnClient
                                 .runCommand(DatabaseName(boost::none, "user"_sd),
                                             BSON("insert"
                                                  << "foo"
                                                  << "documents" << BSON_ARRAY(BSON("x" << 1))))
                                 .get();
            ASSERT_EQ(insertRes["n"].Int(), 1);  // Verify the mocked response was returned.
            assertTxnMetadata(mockClient()->getLastSentRequest(),
                              attempt + 1 /* txnNumber */,
                              true /* startTransaction */);
            assertSessionIdMetadata(mockClient()->getLastSentRequest(), LsidAssertion::kStandalone);
            assertAPIParameters(mockClient()->getLastSentRequest(), params);

            mockClient()->setNextCommandResponse(kOKInsertResponse);
            insertRes = txnClient
                            .runCommand(DatabaseName(boost::none, "user"_sd),
                                        BSON("insert"
                                             << "foo"
                                             << "documents" << BSON_ARRAY(BSON("x" << 1))))
                            .get();
            ASSERT_EQ(insertRes["n"].Int(), 1);  // Verify the mocked response was returned.
            assertTxnMetadata(mockClient()->getLastSentRequest(),
                              attempt + 1 /* txnNumber */,
                              boost::none /* startTransaction */);
            assertSessionIdMetadata(mockClient()->getLastSentRequest(), LsidAssertion::kStandalone);
            assertAPIParameters(mockClient()->getLastSentRequest(), params);

            // Throw a transient error to verify we attach API params on retries as well.
            uassert(ErrorCodes::HostUnreachable, "Mock network error", attempt != 0);

            // The commit response.
            mockClient()->setNextCommandResponse(kOKCommandResponse);
            return SemiFuture<void>::makeReady();
        });
    ASSERT(swResult.getStatus().isOK());
    ASSERT(swResult.getValue().getEffectiveStatus().isOK());

    auto lastRequest = mockClient()->getLastSentRequest();
    assertTxnMetadata(lastRequest,
                      attempt + 1 /* txnNumber */,
                      boost::none /* startTransaction */,
                      boost::none /* readConcern */,
                      WriteConcernOptions().toBSON() /* writeConcern */);
    assertSessionIdMetadata(mockClient()->getLastSentRequest(), LsidAssertion::kStandalone);
    ASSERT_EQ(lastRequest.firstElementFieldNameStringData(), "commitTransaction"_sd);
    assertAPIParameters(mockClient()->getLastSentRequest(), params);
}

TEST_F(TxnAPITest, OwnSession_AttachesWriteConcernOnCommit) {
    const std::vector<WriteConcernOptions> writeConcernOptions = {
        WriteConcernOptions{1, WriteConcernOptions::SyncMode::JOURNAL, Milliseconds{100}},
        WriteConcernOptions{
            "majority", WriteConcernOptions::SyncMode::FSYNC, WriteConcernOptions::kNoTimeout},
        WriteConcernOptions{
            2, WriteConcernOptions::SyncMode::NONE, WriteConcernOptions::kNoWaiting}};

    auto txnNumber{0};
    for (const auto& writeConcern : writeConcernOptions) {

        opCtx()->setWriteConcern(writeConcern);

        // resetTxnWithRetries() function releases and reacquires the txn, so lastTxnNumber is
        // incremented.
        resetTxnWithRetries();
        ++txnNumber;

        int attempt = -1;
        auto swResult = txnWithRetries().runNoThrow(
            opCtx(), [&](const txn_api::TransactionClient& txnClient, ExecutorPtr txnExec) {
                attempt += 1;
                // No write concern on requests prior to commit/abort.
                mockClient()->setNextCommandResponse(kOKInsertResponse);
                auto insertRes = txnClient
                                     .runCommand(DatabaseName(boost::none, "user"_sd),
                                                 BSON("insert"
                                                      << "foo"
                                                      << "documents" << BSON_ARRAY(BSON("x" << 1))))
                                     .get();
                ASSERT_EQ(insertRes["n"].Int(), 1);  // Verify the mocked response was returned.

                // Each attempt releases and reacquires the session from the session pool,
                // incrementing the txnNumber.
                assertTxnMetadata(mockClient()->getLastSentRequest(),
                                  attempt + txnNumber /* txnNumber */,
                                  true /* startTransaction */);
                assertSessionIdMetadata(mockClient()->getLastSentRequest(),
                                        LsidAssertion::kStandalone);


                mockClient()->setNextCommandResponse(kOKInsertResponse);
                insertRes = txnClient
                                .runCommand(DatabaseName(boost::none, "user"_sd),
                                            BSON("insert"
                                                 << "foo"
                                                 << "documents" << BSON_ARRAY(BSON("x" << 1))))
                                .get();
                ASSERT_EQ(insertRes["n"].Int(), 1);  // Verify the mocked response was returned.

                // Each attempt returns and reacquires the session from the session pool,
                // incrementing the txnNumber.
                assertTxnMetadata(mockClient()->getLastSentRequest(),
                                  attempt + txnNumber /* txnNumber */,
                                  boost::none /* startTransaction */);
                assertSessionIdMetadata(mockClient()->getLastSentRequest(),
                                        LsidAssertion::kStandalone);

                // Throw a transient error to verify the retries behavior.
                uassert(ErrorCodes::HostUnreachable, "Mock network error", attempt != 0);

                // The commit response.
                mockClient()->setNextCommandResponse(kOKCommandResponse);
                return SemiFuture<void>::makeReady();
            });
        ASSERT(swResult.getStatus().isOK());
        ASSERT(swResult.getValue().getEffectiveStatus().isOK());

        auto lastRequest = mockClient()->getLastSentRequest();

        // Each attempt returns and reacquires the session from the session pool, incrementing the
        // txnNumber.
        assertTxnMetadata(lastRequest,
                          attempt + txnNumber /* txnNumber */,
                          boost::none /* startTransaction */,
                          boost::none /* readConcern */,
                          writeConcern.toBSON());
        assertSessionIdMetadata(mockClient()->getLastSentRequest(), LsidAssertion::kStandalone);

        ASSERT_EQ(lastRequest.firstElementFieldNameStringData(), "commitTransaction"_sd);
        txnNumber += attempt;
    }
}

TEST_F(TxnAPITest, OwnSession_AttachesWriteConcernOnAbort) {
    const std::vector<WriteConcernOptions> writeConcernOptions = {
        WriteConcernOptions{1, WriteConcernOptions::SyncMode::JOURNAL, Milliseconds{100}},
        WriteConcernOptions{
            "majority", WriteConcernOptions::SyncMode::FSYNC, WriteConcernOptions::kNoTimeout},
        WriteConcernOptions{
            2, WriteConcernOptions::SyncMode::NONE, WriteConcernOptions::kNoWaiting}};

    auto txnNumber{0};
    for (const auto& writeConcern : writeConcernOptions) {
        opCtx()->setWriteConcern(writeConcern);

        // resetTxnWithRetries() function releases and reacquires the txn, so lastTxnNumber is
        // incremented.
        resetTxnWithRetries();
        ++txnNumber;

        auto swResult = txnWithRetries().runNoThrow(
            opCtx(), [&](const txn_api::TransactionClient& txnClient, ExecutorPtr txnExec) {
                mockClient()->setNextCommandResponse(kOKInsertResponse);
                auto insertRes = txnClient
                                     .runCommand(DatabaseName(boost::none, "user"_sd),
                                                 BSON("insert"
                                                      << "foo"
                                                      << "documents" << BSON_ARRAY(BSON("x" << 1))))
                                     .get();
                ASSERT_EQ(insertRes["n"].Int(), 1);  // Verify the mocked response was returned.

                mockClient()->setNextCommandResponse(
                    kOKCommandResponse);  // Best effort abort response.

                uasserted(ErrorCodes::InternalError, "Mock error");
                return SemiFuture<void>::makeReady();
            });
        ASSERT_EQ(swResult.getStatus(), ErrorCodes::InternalError);

        waitForAllEarlierTasksToComplete();  // Wait for the final best effort abort to complete.
        expectSentAbort(txnNumber /* txnNumber */, writeConcern.toBSON());
    }
}

TEST_F(TxnAPITest, OwnSession_AttachesReadConcernOnStartTransaction) {
    const std::vector<repl::ReadConcernLevel> readConcernLevels = {
        repl::ReadConcernLevel::kLocalReadConcern,
        repl::ReadConcernLevel::kMajorityReadConcern,
        repl::ReadConcernLevel::kSnapshotReadConcern};

    auto txnNumber{0};
    for (const auto& readConcernLevel : readConcernLevels) {
        auto readConcern = repl::ReadConcernArgs(readConcernLevel);
        repl::ReadConcernArgs::get(opCtx()) = readConcern;

        // resetTxnWithRetries() function releases and reacquires the txn, so lastTxnNumber is
        // incremented.
        resetTxnWithRetries();
        ++txnNumber;

        int attempt = -1;
        auto swResult = txnWithRetries().runNoThrow(
            opCtx(), [&](const txn_api::TransactionClient& txnClient, ExecutorPtr txnExec) {
                attempt += 1;
                mockClient()->setNextCommandResponse(kOKInsertResponse);
                auto insertRes = txnClient
                                     .runCommand(DatabaseName(boost::none, "user"_sd),
                                                 BSON("insert"
                                                      << "foo"
                                                      << "documents" << BSON_ARRAY(BSON("x" << 1))))
                                     .get();
                ASSERT_EQ(insertRes["n"].Int(), 1);  // Verify the mocked response was returned.

                // Each attempt returns and reacquires the session from the session pool,
                // incrementing the txnNumber.
                assertTxnMetadata(mockClient()->getLastSentRequest(),
                                  attempt + txnNumber /* txnNumber */,
                                  true /* startTransaction */,
                                  readConcern.toBSONInner());
                assertSessionIdMetadata(mockClient()->getLastSentRequest(),
                                        LsidAssertion::kStandalone);

                // Subsequent requests shouldn't have a read concern.
                mockClient()->setNextCommandResponse(kOKInsertResponse);
                insertRes = txnClient
                                .runCommand(DatabaseName(boost::none, "user"_sd),
                                            BSON("insert"
                                                 << "foo"
                                                 << "documents" << BSON_ARRAY(BSON("x" << 1))))
                                .get();
                ASSERT_EQ(insertRes["n"].Int(), 1);  // Verify the mocked response was returned.

                // Each attempt returns and reacquires the session from the session pool,
                // incrementing the txnNumber.
                assertTxnMetadata(mockClient()->getLastSentRequest(),
                                  attempt + txnNumber /* txnNumber */,
                                  boost::none /* startTransaction */);
                assertSessionIdMetadata(mockClient()->getLastSentRequest(),
                                        LsidAssertion::kStandalone);

                // Throw a transient error to verify the retry will still use the read concern.
                uassert(ErrorCodes::HostUnreachable, "Mock network error", attempt != 0);

                // The commit response.
                mockClient()->setNextCommandResponse(kOKCommandResponse);
                return SemiFuture<void>::makeReady();
            });
        ASSERT(swResult.getStatus().isOK());
        ASSERT(swResult.getValue().getEffectiveStatus().isOK());

        auto lastRequest = mockClient()->getLastSentRequest();

        // Each attempt returns and reacquires the session from the session pool, incrementing the
        // txnNumber.
        assertTxnMetadata(lastRequest,
                          attempt + txnNumber /* txnNumber */,
                          boost::none /* startTransaction */,
                          boost::none /* readConcern */,
                          WriteConcernOptions().toBSON() /* writeConcern */);
        assertSessionIdMetadata(mockClient()->getLastSentRequest(), LsidAssertion::kStandalone);
        ASSERT_EQ(lastRequest.firstElementFieldNameStringData(), "commitTransaction"_sd);
        txnNumber += attempt;
    }
}

TEST_F(TxnAPITest, OwnSession_AbortsOnError) {
    auto swResult = txnWithRetries().runNoThrow(
        opCtx(), [&](const txn_api::TransactionClient& txnClient, ExecutorPtr txnExec) {
            mockClient()->setNextCommandResponse(kOKInsertResponse);
            auto insertRes = txnClient
                                 .runCommand(DatabaseName(boost::none, "user"_sd),
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

    waitForAllEarlierTasksToComplete();  // Wait for the final best effort abort to complete.
    expectSentAbort(0 /* txnNumber */, WriteConcernOptions().toBSON());
}

TEST_F(TxnAPITest, OwnSession_SkipsAbortIfNoCommandsWereRun) {
    auto swResult = txnWithRetries().runNoThrow(
        opCtx(), [&](const txn_api::TransactionClient& txnClient, ExecutorPtr txnExec) {
            // The abort response, the client should not receive this.
            mockClient()->setNextCommandResponse(kResWithBadValueError);

            uasserted(ErrorCodes::InternalError, "Mock error");
            return SemiFuture<void>::makeReady();
        });
    ASSERT_EQ(swResult.getStatus(), ErrorCodes::InternalError);

    // Would wait for the best effort abort if it was scheduled.
    waitForAllEarlierTasksToComplete();
    auto lastRequest = mockClient()->getLastSentRequest();
    ASSERT(lastRequest.isEmpty());
}

TEST_F(TxnAPITest, OwnSession_SkipsCommitIfNoCommandsWereRun) {
    auto swResult = txnWithRetries().runNoThrow(
        opCtx(), [&](const txn_api::TransactionClient& txnClient, ExecutorPtr txnExec) {
            // The commit response, the client should not receive this.
            mockClient()->setNextCommandResponse(kResWithBadValueError);

            return SemiFuture<void>::makeReady();
        });
    ASSERT(swResult.getStatus().isOK());
    ASSERT(swResult.getValue().getEffectiveStatus().isOK());

    // Would wait for the best effort abort if it was scheduled.
    waitForAllEarlierTasksToComplete();
    auto lastRequest = mockClient()->getLastSentRequest();
    ASSERT(lastRequest.isEmpty());
}

TEST_F(TxnAPITest, OwnSession_RetriesOnTransientError) {
    int attempt = -1;
    auto swResult = txnWithRetries().runNoThrow(
        opCtx(), [&](const txn_api::TransactionClient& txnClient, ExecutorPtr txnExec) {
            attempt += 1;
            if (attempt > 0) {
                // Verify an abort was sent in between retries.
                expectSentAbort(attempt - 1 /* txnNumber */, WriteConcernOptions().toBSON());
            }

            mockClient()->setNextCommandResponse(attempt == 0 ? kNoSuchTransactionResponse
                                                              : kOKInsertResponse);
            auto insertRes = txnClient
                                 .runCommand(DatabaseName(boost::none, "user"_sd),
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
                              true /* startTransaction */);
            assertSessionIdMetadata(mockClient()->getLastSentRequest(), LsidAssertion::kStandalone);

            return SemiFuture<void>::makeReady();
        });
    ASSERT(swResult.getStatus().isOK());
    ASSERT(swResult.getValue().getEffectiveStatus().isOK());

    ASSERT_EQ(1, InternalTransactionMetrics::get(opCtx())->getStarted());
    ASSERT_EQ(1, InternalTransactionMetrics::get(opCtx())->getRetriedTransactions());
    ASSERT_EQ(0, InternalTransactionMetrics::get(opCtx())->getRetriedCommits());
    ASSERT_EQ(1, InternalTransactionMetrics::get(opCtx())->getSucceeded());

    auto lastRequest = mockClient()->getLastSentRequest();
    assertTxnMetadata(lastRequest,
                      attempt /* txnNumber */,
                      boost::none /* startTransaction */,
                      boost::none /* readConcern */,
                      WriteConcernOptions().toBSON() /* writeConcern */);
    ASSERT_EQ(lastRequest.firstElementFieldNameStringData(), "commitTransaction"_sd);
}

TEST_F(TxnAPITest, OwnSession_RetriesOnTransientClientError) {
    int attempt = -1;
    auto swResult = txnWithRetries().runNoThrow(
        opCtx(), [&](const txn_api::TransactionClient& txnClient, ExecutorPtr txnExec) {
            attempt += 1;
            if (attempt > 0) {
                // Verify an abort was sent in between retries.
                expectSentAbort(attempt - 1 /* txnNumber */, WriteConcernOptions().toBSON());
            }

            mockClient()->setNextCommandResponse(kOKInsertResponse);
            auto insertRes = txnClient
                                 .runCommand(DatabaseName(boost::none, "user"_sd),
                                             BSON("insert"
                                                  << "foo"
                                                  << "documents" << BSON_ARRAY(BSON("x" << 1))))
                                 .get();
            ASSERT_OK(getStatusFromWriteCommandReply(insertRes));

            ASSERT_EQ(insertRes["n"].Int(), 1);  // Verify the mocked response was returned.
            assertTxnMetadata(mockClient()->getLastSentRequest(),
                              attempt /* txnNumber */,
                              true /* startTransaction */);
            assertSessionIdMetadata(mockClient()->getLastSentRequest(), LsidAssertion::kStandalone);

            // The commit or implicit abort response.
            mockClient()->setNextCommandResponse(kOKCommandResponse);

            uassert(ErrorCodes::HostUnreachable, "Mock network error", attempt != 0);
            return SemiFuture<void>::makeReady();
        });
    ASSERT(swResult.getStatus().isOK());
    ASSERT(swResult.getValue().getEffectiveStatus().isOK());

    ASSERT_EQ(1, InternalTransactionMetrics::get(opCtx())->getStarted());
    ASSERT_EQ(1, InternalTransactionMetrics::get(opCtx())->getRetriedTransactions());
    ASSERT_EQ(0, InternalTransactionMetrics::get(opCtx())->getRetriedCommits());
    ASSERT_EQ(1, InternalTransactionMetrics::get(opCtx())->getSucceeded());

    auto lastRequest = mockClient()->getLastSentRequest();
    assertTxnMetadata(lastRequest,
                      attempt /* txnNumber */,
                      boost::none /* startTransaction */,
                      boost::none /* readConcern */,
                      WriteConcernOptions().toBSON() /* writeConcern */);
    assertSessionIdMetadata(mockClient()->getLastSentRequest(), LsidAssertion::kStandalone);
    ASSERT_EQ(lastRequest.firstElementFieldNameStringData(), "commitTransaction"_sd);
}

TEST_F(TxnAPITest, OwnSession_CommitError) {
    auto swResult = txnWithRetries().runNoThrow(
        opCtx(), [&](const txn_api::TransactionClient& txnClient, ExecutorPtr txnExec) {
            mockClient()->setNextCommandResponse(kOKInsertResponse);
            auto insertRes = txnClient
                                 .runCommand(DatabaseName(boost::none, "user"_sd),
                                             BSON("insert"
                                                  << "foo"
                                                  << "documents" << BSON_ARRAY(BSON("x" << 1))))
                                 .get();
            ASSERT_OK(getStatusFromWriteCommandReply(insertRes));

            ASSERT_EQ(insertRes["n"].Int(), 1);  // Verify the mocked response was returned.
            assertTxnMetadata(
                mockClient()->getLastSentRequest(), 0 /* txnNumber */, true /* startTransaction */);
            assertSessionIdMetadata(mockClient()->getLastSentRequest(), LsidAssertion::kStandalone);

            // The commit response.
            mockClient()->setNextCommandResponse(
                BSON("ok" << 0 << "code" << ErrorCodes::InternalError));

            return SemiFuture<void>::makeReady();
        });
    ASSERT(swResult.getStatus().isOK());
    ASSERT_EQ(swResult.getValue().cmdStatus, ErrorCodes::InternalError);
    ASSERT(swResult.getValue().wcError.toStatus().isOK());
    ASSERT_EQ(swResult.getValue().getEffectiveStatus(), ErrorCodes::InternalError);

    ASSERT_EQ(1, InternalTransactionMetrics::get(opCtx())->getStarted());
    ASSERT_EQ(0, InternalTransactionMetrics::get(opCtx())->getRetriedTransactions());
    ASSERT_EQ(0, InternalTransactionMetrics::get(opCtx())->getRetriedCommits());
    ASSERT_EQ(0, InternalTransactionMetrics::get(opCtx())->getSucceeded());

    auto lastRequest = mockClient()->getLastSentRequest();
    assertTxnMetadata(lastRequest,
                      0 /* txnNumber */,
                      boost::none /* startTransaction */,
                      boost::none /* readConcern */,
                      WriteConcernOptions().toBSON() /* writeConcern */);
    ASSERT_EQ(lastRequest.firstElementFieldNameStringData(), "commitTransaction"_sd);
}

TEST_F(TxnAPITest, OwnSession_TransientCommitError) {
    int attempt = -1;
    auto swResult = txnWithRetries().runNoThrow(
        opCtx(), [&](const txn_api::TransactionClient& txnClient, ExecutorPtr txnExec) {
            attempt += 1;
            mockClient()->setNextCommandResponse(kOKInsertResponse);
            auto insertRes = txnClient
                                 .runCommand(DatabaseName(boost::none, "user"_sd),
                                             BSON("insert"
                                                  << "foo"
                                                  << "documents" << BSON_ARRAY(BSON("x" << 1))))
                                 .get();
            ASSERT_OK(getStatusFromWriteCommandReply(insertRes));

            ASSERT_EQ(insertRes["n"].Int(), 1);  // Verify the mocked response was returned.
            assertTxnMetadata(mockClient()->getLastSentRequest(),
                              attempt /* txnNumber */,
                              true /* startTransaction */);
            assertSessionIdMetadata(mockClient()->getLastSentRequest(), LsidAssertion::kStandalone);

            // Set commit response. Initial commit response is kNoSuchTransaction so the transaction
            // retries.
            if (attempt == 0) {
                mockClient()->setNextCommandResponse(kNoSuchTransactionResponse);
            } else {
                mockClient()->setNextCommandResponse(kOKCommandResponse);
            }
            return SemiFuture<void>::makeReady();
        });
    ASSERT(swResult.getStatus().isOK());
    ASSERT(swResult.getValue().getEffectiveStatus().isOK());

    ASSERT_EQ(1, InternalTransactionMetrics::get(opCtx())->getStarted());
    ASSERT_EQ(1, InternalTransactionMetrics::get(opCtx())->getRetriedTransactions());
    ASSERT_EQ(0, InternalTransactionMetrics::get(opCtx())->getRetriedCommits());
    ASSERT_EQ(1, InternalTransactionMetrics::get(opCtx())->getSucceeded());

    auto lastRequest = mockClient()->getLastSentRequest();
    assertTxnMetadata(lastRequest,
                      attempt /* txnNumber */,
                      boost::none /* startTransaction */,
                      boost::none /* readConcern */,
                      WriteConcernOptions().toBSON() /* writeConcern */);
    assertSessionIdMetadata(mockClient()->getLastSentRequest(), LsidAssertion::kStandalone);
    ASSERT_EQ(lastRequest.firstElementFieldNameStringData(), "commitTransaction"_sd);
}

TEST_F(TxnAPITest, OwnSession_RetryableCommitError) {
    auto swResult = txnWithRetries().runNoThrow(
        opCtx(), [&](const txn_api::TransactionClient& txnClient, ExecutorPtr txnExec) {
            mockClient()->setNextCommandResponse(kOKInsertResponse);
            auto insertRes = txnClient
                                 .runCommand(DatabaseName(boost::none, "user"_sd),
                                             BSON("insert"
                                                  << "foo"
                                                  << "documents" << BSON_ARRAY(BSON("x" << 1))))
                                 .get();
            ASSERT_OK(getStatusFromWriteCommandReply(insertRes));

            ASSERT_EQ(insertRes["n"].Int(), 1);  // Verify the mocked response was returned.
            assertTxnMetadata(
                mockClient()->getLastSentRequest(), 0 /* txnNumber */, true /* startTransaction */);
            assertSessionIdMetadata(mockClient()->getLastSentRequest(), LsidAssertion::kStandalone);

            // The commit response.
            mockClient()->setNextCommandResponse(
                BSON("ok" << 0 << "code" << ErrorCodes::InterruptedDueToReplStateChange));
            mockClient()->setNextCommandResponse(kOKCommandResponse);
            return SemiFuture<void>::makeReady();
        });
    ASSERT(swResult.getStatus().isOK());
    ASSERT(swResult.getValue().getEffectiveStatus().isOK());

    ASSERT_EQ(1, InternalTransactionMetrics::get(opCtx())->getStarted());
    ASSERT_EQ(0, InternalTransactionMetrics::get(opCtx())->getRetriedTransactions());
    ASSERT_EQ(1, InternalTransactionMetrics::get(opCtx())->getRetriedCommits());
    ASSERT_EQ(1, InternalTransactionMetrics::get(opCtx())->getSucceeded());


    auto lastRequest = mockClient()->getLastSentRequest();
    assertTxnMetadata(lastRequest,
                      0 /* txnNumber */,
                      boost::none /* startTransaction */,
                      boost::none /* readConcern */,
                      CommandHelpers::kMajorityWriteConcern.toBSON());
    assertSessionIdMetadata(mockClient()->getLastSentRequest(), LsidAssertion::kStandalone);
    ASSERT_EQ(lastRequest.firstElementFieldNameStringData(), "commitTransaction"_sd);
}

TEST_F(TxnAPITest, OwnSession_NonRetryableCommitWCError) {
    auto swResult = txnWithRetries().runNoThrow(
        opCtx(), [&](const txn_api::TransactionClient& txnClient, ExecutorPtr txnExec) {
            mockClient()->setNextCommandResponse(kOKInsertResponse);
            auto insertRes = txnClient
                                 .runCommand(DatabaseName(boost::none, "user"_sd),
                                             BSON("insert"
                                                  << "foo"
                                                  << "documents" << BSON_ARRAY(BSON("x" << 1))))
                                 .get();
            ASSERT_EQ(insertRes["n"].Int(), 1);  // Verify the mocked response was returned.
            assertTxnMetadata(
                mockClient()->getLastSentRequest(), 0 /* txnNumber */, true /* startTransaction */);
            assertSessionIdMetadata(mockClient()->getLastSentRequest(), LsidAssertion::kStandalone);

            // The commit response.
            mockClient()->setNextCommandResponse(kResWithWriteConcernError);
            mockClient()->setNextCommandResponse(
                kOKCommandResponse);  // Best effort abort response.
            return SemiFuture<void>::makeReady();
        });
    ASSERT(swResult.getStatus().isOK());
    ASSERT(swResult.getValue().cmdStatus.isOK());
    ASSERT_EQ(swResult.getValue().wcError.toStatus(), ErrorCodes::WriteConcernFailed);
    ASSERT_EQ(swResult.getValue().getEffectiveStatus(), ErrorCodes::WriteConcernFailed);

    ASSERT_EQ(1, InternalTransactionMetrics::get(opCtx())->getStarted());
    ASSERT_EQ(0, InternalTransactionMetrics::get(opCtx())->getRetriedTransactions());
    ASSERT_EQ(0, InternalTransactionMetrics::get(opCtx())->getRetriedCommits());
    ASSERT_EQ(0, InternalTransactionMetrics::get(opCtx())->getSucceeded());

    auto lastRequest = mockClient()->getLastSentRequest();
    assertTxnMetadata(lastRequest,
                      0 /* txnNumber */,
                      boost::none /* startTransaction */,
                      boost::none /* readConcern */,
                      WriteConcernOptions().toBSON() /* writeConcern */);
    ASSERT_EQ(lastRequest.firstElementFieldNameStringData(), "commitTransaction"_sd);
}

TEST_F(TxnAPITest, OwnSession_RetryableCommitWCError) {
    auto swResult = txnWithRetries().runNoThrow(
        opCtx(), [&](const txn_api::TransactionClient& txnClient, ExecutorPtr txnExec) {
            mockClient()->setNextCommandResponse(kOKInsertResponse);
            auto insertRes = txnClient
                                 .runCommand(DatabaseName(boost::none, "user"_sd),
                                             BSON("insert"
                                                  << "foo"
                                                  << "documents" << BSON_ARRAY(BSON("x" << 1))))
                                 .get();
            ASSERT_OK(getStatusFromWriteCommandReply(insertRes));

            ASSERT_EQ(insertRes["n"].Int(), 1);  // Verify the mocked response was returned.
            assertTxnMetadata(
                mockClient()->getLastSentRequest(), 0 /* txnNumber */, true /* startTransaction */);
            assertSessionIdMetadata(mockClient()->getLastSentRequest(), LsidAssertion::kStandalone);

            // The commit responses.
            mockClient()->setNextCommandResponse(kResWithRetryableWriteConcernError);
            mockClient()->setNextCommandResponse(kOKCommandResponse);
            return SemiFuture<void>::makeReady();
        });
    ASSERT(swResult.getStatus().isOK());
    ASSERT(swResult.getValue().getEffectiveStatus().isOK());

    ASSERT_EQ(1, InternalTransactionMetrics::get(opCtx())->getStarted());
    ASSERT_EQ(0, InternalTransactionMetrics::get(opCtx())->getRetriedTransactions());
    ASSERT_EQ(1, InternalTransactionMetrics::get(opCtx())->getRetriedCommits());
    ASSERT_EQ(1, InternalTransactionMetrics::get(opCtx())->getSucceeded());

    auto lastRequest = mockClient()->getLastSentRequest();
    assertTxnMetadata(lastRequest,
                      0 /* txnNumber */,
                      boost::none /* startTransaction */,
                      boost::none /* readConcern */,
                      CommandHelpers::kMajorityWriteConcern.toBSON());
    assertSessionIdMetadata(mockClient()->getLastSentRequest(), LsidAssertion::kStandalone);
    ASSERT_EQ(lastRequest.firstElementFieldNameStringData(), "commitTransaction"_sd);
}

TEST_F(TxnAPITest, RunNoErrors) {
    txnWithRetries().run(opCtx(),
                         [&](const txn_api::TransactionClient& txnClient, ExecutorPtr txnExec) {
                             return SemiFuture<void>::makeReady();
                         });
}

TEST_F(TxnAPITest, RunThrowsOnBodyError) {
    ASSERT_THROWS_CODE(
        txnWithRetries().run(opCtx(),
                             [&](const txn_api::TransactionClient& txnClient, ExecutorPtr txnExec) {
                                 uasserted(ErrorCodes::InternalError, "Mock error");
                                 return SemiFuture<void>::makeReady();
                             }),
        DBException,
        ErrorCodes::InternalError);
}

TEST_F(TxnAPITest, RunThrowsOnCommitCmdError) {
    ASSERT_THROWS_CODE(
        txnWithRetries().run(
            opCtx(),
            [&](const txn_api::TransactionClient& txnClient, ExecutorPtr txnExec) {
                mockClient()->setNextCommandResponse(kOKInsertResponse);
                auto insertRes = txnClient
                                     .runCommand(DatabaseName(boost::none, "user"_sd),
                                                 BSON("insert"
                                                      << "foo"
                                                      << "documents" << BSON_ARRAY(BSON("x" << 1))))
                                     .get();

                // The commit response.
                mockClient()->setNextCommandResponse(
                    BSON("ok" << 0 << "code" << ErrorCodes::InternalError));
                mockClient()->setNextCommandResponse(
                    kOKCommandResponse);  // Best effort abort response.
                return SemiFuture<void>::makeReady();
            }),
        DBException,
        ErrorCodes::InternalError);
}

TEST_F(TxnAPITest, RunThrowsOnCommitWCError) {
    ASSERT_THROWS_CODE(
        txnWithRetries().run(
            opCtx(),
            [&](const txn_api::TransactionClient& txnClient, ExecutorPtr txnExec) {
                mockClient()->setNextCommandResponse(kOKInsertResponse);
                auto insertRes = txnClient
                                     .runCommand(DatabaseName(boost::none, "user"_sd),
                                                 BSON("insert"
                                                      << "foo"
                                                      << "documents" << BSON_ARRAY(BSON("x" << 1))))
                                     .get();

                // The commit response.
                mockClient()->setNextCommandResponse(kResWithWriteConcernError);
                mockClient()->setNextCommandResponse(
                    kOKCommandResponse);  // Best effort abort response.
                return SemiFuture<void>::makeReady();
            }),
        DBException,
        ErrorCodes::WriteConcernFailed);
}

TEST_F(TxnAPITest, UnyieldsAfterBodyError) {
    resetTxnWithRetries(std::make_unique<MockResourceYielder>());

    auto swResult = txnWithRetries().runNoThrow(
        opCtx(), [&](const txn_api::TransactionClient& txnClient, ExecutorPtr txnExec) {
            auto insertRes = txnClient
                                 .runCommand(DatabaseName(boost::none, "user"_sd),
                                             BSON("insert"
                                                  << "foo"
                                                  << "documents" << BSON_ARRAY(BSON("x" << 1))))
                                 .get();
            uasserted(ErrorCodes::InternalError, "Simulated body error");
            return SemiFuture<void>::makeReady();
        });
    ASSERT_EQ(swResult.getStatus(), ErrorCodes::InternalError);
    // Yield before starting and corresponding unyield.
    ASSERT_EQ(resourceYielder()->timesYielded(), 1);
    ASSERT_EQ(resourceYielder()->timesUnyielded(), 1);
}

TEST_F(TxnAPITest, HandlesExceptionWhileYielding) {
    resetTxnWithRetries(std::make_unique<MockResourceYielder>());
    resourceYielder()->throwInYield();

    auto swResult = txnWithRetries().runNoThrow(
        opCtx(), [&](const txn_api::TransactionClient& txnClient, ExecutorPtr txnExec) {
            auto insertRes = txnClient
                                 .runCommand(DatabaseName(boost::none, "user"_sd),
                                             BSON("insert"
                                                  << "foo"
                                                  << "documents" << BSON_ARRAY(BSON("x" << 1))))
                                 .get();
            return SemiFuture<void>::makeReady();
        });
    ASSERT_EQ(swResult.getStatus(), ErrorCodes::Interrupted);
    // Yield before starting but no unyield because yield failed.
    ASSERT_EQ(resourceYielder()->timesYielded(), 1);
    ASSERT_EQ(resourceYielder()->timesUnyielded(), 0);
}

TEST_F(TxnAPITest, HandlesExceptionWhileUnyielding) {
    resetTxnWithRetries(std::make_unique<MockResourceYielder>());
    resourceYielder()->throwInUnyield();

    auto swResult = txnWithRetries().runNoThrow(
        opCtx(), [&](const txn_api::TransactionClient& txnClient, ExecutorPtr txnExec) {
            auto insertRes = txnClient
                                 .runCommand(DatabaseName(boost::none, "user"_sd),
                                             BSON("insert"
                                                  << "foo"
                                                  << "documents" << BSON_ARRAY(BSON("x" << 1))))
                                 .get();
            return SemiFuture<void>::makeReady();
        });
    ASSERT_EQ(swResult.getStatus(), ErrorCodes::Interrupted);
    // Yield before starting and corresponding unyield.
    ASSERT_EQ(resourceYielder()->timesYielded(), 1);
    ASSERT_EQ(resourceYielder()->timesUnyielded(), 1);
}

// TODO SERVER-75553 - with inline executors, this test runs in the wrong order
#if 0
TEST_F(TxnAPITest, UnyieldsAfterCancellation) {
    resetTxnWithRetries(std::make_unique<MockResourceYielder>());

    unittest::Barrier txnApiStarted(2);
    unittest::Barrier opCtxKilled(2);

    auto killerThread = stdx::thread([&txnApiStarted, &opCtxKilled, opCtx = opCtx()] {
        txnApiStarted.countDownAndWait();
        opCtx->markKilled();
        opCtxKilled.countDownAndWait();
    });

    auto swResult = txnWithRetries().runNoThrow(
        opCtx(), [&](const txn_api::TransactionClient& txnClient, ExecutorPtr txnExec) {
            mockClient()->setNextCommandResponse(kOKInsertResponse);
            auto insertRes = txnClient
                                 .runCommand(DatabaseName(boost::none, "user"_sd),
                                             BSON("insert"
                                                  << "foo"
                                                  << "documents" << BSON_ARRAY(BSON("x" << 1))))
                                 .get();

            resourceYielder()->throwInUnyield(ErrorCodes::InternalError);

            txnApiStarted.countDownAndWait();
            opCtxKilled.countDownAndWait();

            return SemiFuture<void>::makeReady();
        });

    // The transaction should fail with an Interrupted error from killing the opCtx using the
    // API instead of the ResourceYielder error from within the API callback.
    ASSERT_EQ(swResult.getStatus(), ErrorCodes::Interrupted);
    // Yield before starting and corresponding unyield.
    ASSERT_EQ(resourceYielder()->timesYielded(), 1);
    ASSERT_EQ(resourceYielder()->timesUnyielded(), 1);

    killerThread.join();
}
#endif

TEST_F(TxnAPITest, ClientSession_UsesNonRetryableInternalSession) {
    opCtx()->setLogicalSessionId(makeLogicalSessionIdForTest());
    resetTxnWithRetries();

    int attempt = -1;
    boost::optional<LogicalSessionId> firstAttemptLsid;
    auto swResult = txnWithRetries().runNoThrow(
        opCtx(), [&](const txn_api::TransactionClient& txnClient, ExecutorPtr txnExec) {
            attempt += 1;

            mockClient()->setNextCommandResponse(kOKInsertResponse);
            auto insertRes = txnClient
                                 .runCommand(DatabaseName(boost::none, "user"_sd),
                                             BSON("insert"
                                                  << "foo"
                                                  << "documents" << BSON_ARRAY(BSON("x" << 1))))
                                 .get();
            ASSERT_EQ(insertRes["n"].Int(), 1);  // Verify the mocked response was returned.
            assertTxnMetadata(mockClient()->getLastSentRequest(),
                              attempt /* txnNumber */,
                              true /* startTransaction */);
            assertSessionIdMetadata(mockClient()->getLastSentRequest(),
                                    LsidAssertion::kNonRetryableChild,
                                    opCtx()->getLogicalSessionId());

            if (attempt == 0) {
                firstAttemptLsid = getLsid(mockClient()->getLastSentRequest());
                // Trigger transient error retry to verify the same session is used by the retry.
                mockClient()->setNextCommandResponse(kNoSuchTransactionResponse);
            } else {
                ASSERT_EQ(*firstAttemptLsid, getLsid(mockClient()->getLastSentRequest()));
                mockClient()->setNextCommandResponse(kOKCommandResponse);  // Commit response.
            }
            return SemiFuture<void>::makeReady();
        });
    ASSERT(swResult.getStatus().isOK());
    ASSERT(swResult.getValue().getEffectiveStatus().isOK());

    auto lastRequest = mockClient()->getLastSentRequest();
    assertTxnMetadata(lastRequest,
                      attempt /* txnNumber */,
                      boost::none /* startTransaction */,
                      boost::none /* readConcern */,
                      WriteConcernOptions().toBSON() /* writeConcern */);
    assertSessionIdMetadata(mockClient()->getLastSentRequest(),
                            LsidAssertion::kNonRetryableChild,
                            opCtx()->getLogicalSessionId());
    ASSERT_EQ(lastRequest.firstElementFieldNameStringData(), "commitTransaction"_sd);
}

TEST_F(TxnAPITest, ClientRetryableWrite_UsesRetryableInternalSession) {
    opCtx()->setLogicalSessionId(makeLogicalSessionIdForTest());
    opCtx()->setTxnNumber(5);
    resetTxnWithRetries();

    int attempt = -1;
    boost::optional<LogicalSessionId> firstAttemptLsid;
    auto swResult = txnWithRetries().runNoThrow(
        opCtx(), [&](const txn_api::TransactionClient& txnClient, ExecutorPtr txnExec) {
            attempt += 1;

            mockClient()->setNextCommandResponse(kOKInsertResponse);
            auto insertRes = txnClient
                                 .runCommand(DatabaseName(boost::none, "user"_sd),
                                             BSON("insert"
                                                  << "foo"
                                                  << "documents"
                                                  << BSON_ARRAY(BSON("x" << 1))
                                                  // Retryable transactions must include stmtIds for
                                                  // retryable write commands.
                                                  << "stmtIds" << BSON_ARRAY(1)))
                                 .get();
            ASSERT_EQ(insertRes["n"].Int(), 1);  // Verify the mocked response was returned.
            assertTxnMetadata(mockClient()->getLastSentRequest(),
                              attempt /* txnNumber */,
                              true /* startTransaction */);
            assertSessionIdMetadata(mockClient()->getLastSentRequest(),
                                    LsidAssertion::kRetryableChild,
                                    opCtx()->getLogicalSessionId(),
                                    opCtx()->getTxnNumber());

            // Verify a non-retryable write command does not need to include stmtIds.
            mockClient()->setNextCommandResponse(kOKCommandResponse);
            auto findRes = txnClient
                               .runCommand(DatabaseName(boost::none, "user"_sd),
                                           BSON("find"
                                                << "foo"))
                               .get();
            ASSERT(findRes["ok"]);  // Verify the mocked response was returned.

            // Verify the alternate format for stmtIds is allowed.
            mockClient()->setNextCommandResponse(kOKInsertResponse);
            insertRes =
                txnClient
                    .runCommand(DatabaseName(boost::none, "user"_sd),
                                BSON("insert"
                                     << "foo"
                                     << "documents" << BSON_ARRAY(BSON("x" << 1)) << "stmtId" << 1))
                    .get();
            ASSERT_EQ(insertRes["n"].Int(), 1);  // Verify the mocked response was returned.

            if (attempt == 0) {
                firstAttemptLsid = getLsid(mockClient()->getLastSentRequest());
                // Trigger transient error retry to verify the same session is used by the retry.
                mockClient()->setNextCommandResponse(kNoSuchTransactionResponse);
            } else {
                ASSERT_EQ(*firstAttemptLsid, getLsid(mockClient()->getLastSentRequest()));
                mockClient()->setNextCommandResponse(kOKCommandResponse);  // Commit response.
            }
            return SemiFuture<void>::makeReady();
        });
    ASSERT(swResult.getStatus().isOK());
    ASSERT(swResult.getValue().getEffectiveStatus().isOK());

    auto lastRequest = mockClient()->getLastSentRequest();
    assertTxnMetadata(lastRequest,
                      attempt /* txnNumber */,
                      boost::none /* startTransaction */,
                      boost::none /* readConcern */,
                      WriteConcernOptions().toBSON() /* writeConcern */);
    assertSessionIdMetadata(mockClient()->getLastSentRequest(),
                            LsidAssertion::kRetryableChild,
                            opCtx()->getLogicalSessionId(),
                            opCtx()->getTxnNumber());
    ASSERT_EQ(lastRequest.firstElementFieldNameStringData(), "commitTransaction"_sd);
}

#ifdef MONGO_CONFIG_DEBUG_BUILD
DEATH_TEST_F(TxnAPITest,
             ClientRetryableWrite_RetryableWriteWithoutStmtIdCrashesOnDebug,
             "In a retryable write transaction every retryable write command should") {
    opCtx()->setLogicalSessionId(makeLogicalSessionIdForTest());
    opCtx()->setTxnNumber(5);
    resetTxnWithRetries();

    auto swResult = txnWithRetries().runNoThrow(
        opCtx(), [&](const txn_api::TransactionClient& txnClient, ExecutorPtr txnExec) {
            mockClient()->setNextCommandResponse(kOKInsertResponse);
            auto insertRes = txnClient
                                 .runCommand(DatabaseName(boost::none, "user"_sd),
                                             BSON("insert"
                                                  << "foo"
                                                  << "documents" << BSON_ARRAY(BSON("x" << 1))))
                                 .get();

            return SemiFuture<void>::makeReady();
        });
    ASSERT_EQ(swResult.getStatus(), ErrorCodes::duplicateCodeForTest(6410500));
}
#endif

TEST_F(TxnAPITest, ClientTransaction_UsesClientTransactionOptionsAndDoesNotCommitOnSuccess) {
    opCtx()->setLogicalSessionId(makeLogicalSessionIdForTest());
    opCtx()->setTxnNumber(5);
    opCtx()->setInMultiDocumentTransaction();
    resetTxnWithRetries();

    auto swResult = txnWithRetries().runNoThrow(
        opCtx(), [&](const txn_api::TransactionClient& txnClient, ExecutorPtr txnExec) {
            mockClient()->setNextCommandResponse(kOKInsertResponse);
            auto insertRes = txnClient
                                 .runCommand(DatabaseName(boost::none, "user"_sd),
                                             BSON("insert"
                                                  << "foo"
                                                  << "documents" << BSON_ARRAY(BSON("x" << 1))))
                                 .get();
            ASSERT_EQ(insertRes["n"].Int(), 1);  // Verify the mocked response was returned.
            assertTxnMetadata(mockClient()->getLastSentRequest(),
                              *opCtx()->getTxnNumber(),
                              boost::none /* startTransaction */);
            assertSessionIdMetadata(mockClient()->getLastSentRequest(), LsidAssertion::kStandalone);
            ASSERT_EQ(*opCtx()->getLogicalSessionId(), getLsid(mockClient()->getLastSentRequest()));

            return SemiFuture<void>::makeReady();
        });
    ASSERT(swResult.getStatus().isOK());
    ASSERT(swResult.getValue().getEffectiveStatus().isOK());

    // No commit should have been sent.
    auto lastRequest = mockClient()->getLastSentRequest();
    ASSERT_EQ(lastRequest.firstElementFieldNameStringData(), "insert"_sd);
}

TEST_F(TxnAPITest, ClientTransaction_DoesNotAppendStartTransactionFields) {
    opCtx()->setLogicalSessionId(makeLogicalSessionIdForTest());
    opCtx()->setTxnNumber(5);
    opCtx()->setInMultiDocumentTransaction();

    auto readConcern = repl::ReadConcernArgs(repl::ReadConcernLevel::kLocalReadConcern);
    repl::ReadConcernArgs::get(opCtx()) = readConcern;

    auto writeConcernOptions =
        WriteConcernOptions{1, WriteConcernOptions::SyncMode::JOURNAL, Milliseconds{100}};
    opCtx()->setWriteConcern(writeConcernOptions);

    resetTxnWithRetries();

    auto swResult = txnWithRetries().runNoThrow(
        opCtx(), [&](const txn_api::TransactionClient& txnClient, ExecutorPtr txnExec) {
            mockClient()->setNextCommandResponse(kOKInsertResponse);
            auto insertRes = txnClient
                                 .runCommand(DatabaseName(boost::none, "user"_sd),
                                             BSON("insert"
                                                  << "foo"
                                                  << "documents" << BSON_ARRAY(BSON("x" << 1))))
                                 .get();
            ASSERT_EQ(insertRes["n"].Int(), 1);  // Verify the mocked response was returned.
            assertTxnMetadata(mockClient()->getLastSentRequest(),
                              *opCtx()->getTxnNumber(),
                              boost::none /* startTransaction */,
                              boost::none /* readConcern */,
                              boost::none /* writeConcern */);
            assertSessionIdMetadata(mockClient()->getLastSentRequest(), LsidAssertion::kStandalone);
            ASSERT_EQ(*opCtx()->getLogicalSessionId(), getLsid(mockClient()->getLastSentRequest()));

            return SemiFuture<void>::makeReady();
        });
    ASSERT(swResult.getStatus().isOK());
    ASSERT(swResult.getValue().getEffectiveStatus().isOK());

    // No commit should have been sent.
    auto lastRequest = mockClient()->getLastSentRequest();
    ASSERT_EQ(lastRequest.firstElementFieldNameStringData(), "insert"_sd);
}

TEST_F(TxnAPITest, ClientTransaction_DoesNotBestEffortAbortOnFailure) {
    opCtx()->setLogicalSessionId(makeLogicalSessionIdForTest());
    opCtx()->setTxnNumber(5);
    opCtx()->setInMultiDocumentTransaction();
    resetTxnWithRetries();

    auto swResult = txnWithRetries().runNoThrow(
        opCtx(), [&](const txn_api::TransactionClient& txnClient, ExecutorPtr txnExec) {
            mockClient()->setNextCommandResponse(kOKInsertResponse);
            auto insertRes = txnClient
                                 .runCommand(DatabaseName(boost::none, "user"_sd),
                                             BSON("insert"
                                                  << "foo"
                                                  << "documents" << BSON_ARRAY(BSON("x" << 1))))
                                 .get();
            ASSERT_EQ(insertRes["n"].Int(), 1);  // Verify the mocked response was returned.
            assertTxnMetadata(mockClient()->getLastSentRequest(),
                              *opCtx()->getTxnNumber(),
                              boost::none /* startTransaction */);
            assertSessionIdMetadata(mockClient()->getLastSentRequest(), LsidAssertion::kStandalone);
            ASSERT_EQ(*opCtx()->getLogicalSessionId(), getLsid(mockClient()->getLastSentRequest()));

            // Trigger mock error retry to verify the API does not best effort abort.
            uasserted(ErrorCodes::InternalError, "Mock error");

            return SemiFuture<void>::makeReady();
        });
    // The error should have been propagated.
    ASSERT_EQ(swResult.getStatus(), ErrorCodes::InternalError);

    // No best effort abort should have been sent.
    auto lastRequest = mockClient()->getLastSentRequest();
    ASSERT_EQ(lastRequest.firstElementFieldNameStringData(), "insert"_sd);
}

TEST_F(TxnAPITest, ClientTransaction_DoesNotRetryOnTransientErrors) {
    opCtx()->setLogicalSessionId(makeLogicalSessionIdForTest());
    opCtx()->setTxnNumber(5);
    opCtx()->setInMultiDocumentTransaction();
    resetTxnWithRetries();

    auto swResult = txnWithRetries().runNoThrow(
        opCtx(), [&](const txn_api::TransactionClient& txnClient, ExecutorPtr txnExec) {
            mockClient()->setNextCommandResponse(kOKInsertResponse);
            auto insertRes = txnClient
                                 .runCommand(DatabaseName(boost::none, "user"_sd),
                                             BSON("insert"
                                                  << "foo"
                                                  << "documents" << BSON_ARRAY(BSON("x" << 1))))
                                 .get();
            ASSERT_EQ(insertRes["n"].Int(), 1);  // Verify the mocked response was returned.
            assertTxnMetadata(mockClient()->getLastSentRequest(),
                              *opCtx()->getTxnNumber(),
                              boost::none /* startTransaction */);
            assertSessionIdMetadata(mockClient()->getLastSentRequest(), LsidAssertion::kStandalone);
            ASSERT_EQ(*opCtx()->getLogicalSessionId(), getLsid(mockClient()->getLastSentRequest()));

            // Trigger transient error retry to verify the API does not retry.
            uasserted(ErrorCodes::HostUnreachable, "Mock network error");

            return SemiFuture<void>::makeReady();
        });
    // The transient error should have been propagated.
    ASSERT_EQ(swResult.getStatus(), ErrorCodes::HostUnreachable);

    // No best effort abort should have been sent.
    auto lastRequest = mockClient()->getLastSentRequest();
    ASSERT_EQ(lastRequest.firstElementFieldNameStringData(), "insert"_sd);
}

TEST_F(TxnAPITest, HandleErrorRetryCommitOnNetworkError) {
    auto swResult = txnWithRetries().runNoThrow(
        opCtx(), [&](const txn_api::TransactionClient& txnClient, ExecutorPtr txnExec) {
            mockClient()->setNextCommandResponse(kOKInsertResponse);
            auto insertRes = txnClient
                                 .runCommand(DatabaseName(boost::none, "user"_sd),
                                             BSON("insert"
                                                  << "foo"
                                                  << "documents" << BSON_ARRAY(BSON("x" << 1))))
                                 .get();

            ASSERT_EQ(insertRes["n"].Int(), 1);  // Verify the mocked response was returned.
            assertTxnMetadata(
                mockClient()->getLastSentRequest(), 0 /* txnNumber */, true /* startTransaction */);
            assertSessionIdMetadata(mockClient()->getLastSentRequest(), LsidAssertion::kStandalone);

            // The commit response.
            mockClient()->setNextCommandResponse(
                Status(ErrorCodes::HostUnreachable, "Host Unreachable"));
            mockClient()->setNextCommandResponse(kOKCommandResponse);

            return SemiFuture<void>::makeReady();
        });
    ASSERT(swResult.getStatus().isOK());
    ASSERT(swResult.getValue().getEffectiveStatus().isOK());

    auto lastRequest = mockClient()->getLastSentRequest();
    assertTxnMetadata(lastRequest,
                      0 /* txnNumber */,
                      boost::none /* startTransaction */,
                      boost::none /* readConcern */,
                      CommandHelpers::kMajorityWriteConcern.toBSON());
    assertSessionIdMetadata(mockClient()->getLastSentRequest(), LsidAssertion::kStandalone);
    ASSERT_EQ(lastRequest.firstElementFieldNameStringData(), "commitTransaction"_sd);
}

TEST_F(TxnAPITest, RetryCommitMultipleTimesIncludesMajorityWriteConcern) {
    auto writeConcernOptions =
        WriteConcernOptions{1, WriteConcernOptions::SyncMode::JOURNAL, Milliseconds{100}};
    opCtx()->setWriteConcern(writeConcernOptions);
    resetTxnWithRetries();

    //
    // Mock command responses.
    //

    // Insert response.
    mockClient()->setNextCommandResponse(kOKInsertResponse);
    // The commit responses. First three attempts fail transiently and the fourth succeeds.
    mockClient()->setNextCommandResponse(Status(ErrorCodes::HostUnreachable, "Host Unreachable"));
    mockClient()->setNextCommandResponse(
        BSON("ok" << 0 << "code" << ErrorCodes::InterruptedDueToReplStateChange));
    mockClient()->setNextCommandResponse(kResWithRetryableWriteConcernError);
    mockClient()->setNextCommandResponse(kOKCommandResponse);

    auto swResult = txnWithRetries().runNoThrow(
        opCtx(), [&](const txn_api::TransactionClient& txnClient, ExecutorPtr txnExec) {
            auto insertRes = txnClient
                                 .runCommand(DatabaseName(boost::none, "user"_sd),
                                             BSON("insert"
                                                  << "foo"
                                                  << "documents" << BSON_ARRAY(BSON("x" << 1))))
                                 .get();
            ASSERT_EQ(insertRes["n"].Int(), 1);  // Verify the mocked response was returned.

            return SemiFuture<void>::makeReady();
        });
    ASSERT(swResult.getStatus().isOK());
    ASSERT(swResult.getValue().getEffectiveStatus().isOK());

    //
    // Verify the commit requests used the correct write concerns.
    //

    auto sentRequests = mockClient()->getSentRequests();
    ASSERT_EQ(sentRequests.size(), 5);

    // Skip i = 0, which is the intial attempt's insert.
    for (size_t i = 1; i < sentRequests.size(); ++i) {
        assertTxnMetadata(sentRequests[i],
                          1 /* txnNumber */,
                          boost::none /* startTransaction */,
                          boost::none /* readConcern */,
                          // First commit attempt uses the client's WC, retries use majority.
                          i == 1 ? opCtx()->getWriteConcern().toBSON()
                                 : CommandHelpers::kMajorityWriteConcern.toBSON());
        assertSessionIdMetadata(mockClient()->getLastSentRequest(), LsidAssertion::kStandalone);
        ASSERT_EQ(sentRequests[i].firstElementFieldNameStringData(), "commitTransaction"_sd);
    }
}

TEST_F(TxnAPITest, CommitAfterTransientErrorAfterRetryCommitUsesOriginalWriteConcern) {
    auto writeConcernOptions =
        WriteConcernOptions{1, WriteConcernOptions::SyncMode::JOURNAL, Milliseconds{100}};
    opCtx()->setWriteConcern(writeConcernOptions);
    resetTxnWithRetries();

    //
    // Mock command responses.
    //

    // First attempt.
    mockClient()->setNextCommandResponse(kOKInsertResponse);
    // The commit responses. First attempt fails transiently and the second should trigger a
    // transient transaction retry.
    mockClient()->setNextCommandResponse(Status(ErrorCodes::HostUnreachable, "Host Unreachable"));
    mockClient()->setNextCommandResponse(kNoSuchTransactionResponse);

    // Second attempt after transient transaction error.
    mockClient()->setNextCommandResponse(kOKInsertResponse);
    mockClient()->setNextCommandResponse(Status(ErrorCodes::HostUnreachable, "Host Unreachable"));
    mockClient()->setNextCommandResponse(kOKCommandResponse);

    auto swResult = txnWithRetries().runNoThrow(
        opCtx(), [&](const txn_api::TransactionClient& txnClient, ExecutorPtr txnExec) {
            auto insertRes = txnClient
                                 .runCommand(DatabaseName(boost::none, "user"_sd),
                                             BSON("insert"
                                                  << "foo"
                                                  << "documents" << BSON_ARRAY(BSON("x" << 1))))
                                 .get();
            ASSERT_EQ(insertRes["n"].Int(), 1);  // Verify the mocked response was returned.

            return SemiFuture<void>::makeReady();
        });
    ASSERT(swResult.getStatus().isOK());
    ASSERT(swResult.getValue().getEffectiveStatus().isOK());

    //
    // Verify the commit requests used the correct write concerns.
    //

    auto sentRequests = mockClient()->getSentRequests();
    ASSERT_EQ(sentRequests.size(), 6);

    // Skip i = 0, which is the intial attempt's insert.
    for (size_t i = 1; i <= 2; ++i) {
        assertTxnMetadata(sentRequests[i],
                          1 /* txnNumber */,
                          boost::none /* startTransaction */,
                          boost::none /* readConcern */,
                          // First commit attempt uses the client's WC, retry uses majority.
                          i == 1 ? opCtx()->getWriteConcern().toBSON()
                                 : CommandHelpers::kMajorityWriteConcern.toBSON());
        assertSessionIdMetadata(mockClient()->getLastSentRequest(), LsidAssertion::kStandalone);
        ASSERT_EQ(sentRequests[i].firstElementFieldNameStringData(), "commitTransaction"_sd);
    }
    // Skip i = 3, which is the second attempt's insert.
    for (size_t i = 4; i <= 5; ++i) {
        assertTxnMetadata(sentRequests[i],
                          2 /* txnNumber */,
                          boost::none /* startTransaction */,
                          boost::none /* readConcern */,
                          // First commit attempt uses the client's WC, retry uses majority.
                          i == 4 ? opCtx()->getWriteConcern().toBSON()
                                 : CommandHelpers::kMajorityWriteConcern.toBSON());
        assertSessionIdMetadata(mockClient()->getLastSentRequest(), LsidAssertion::kStandalone);
        ASSERT_EQ(sentRequests[i].firstElementFieldNameStringData(), "commitTransaction"_sd);
    }
}

StatusWith<std::vector<BSONObj>> exhaustiveFindSyncNoThrow(
    txn_api::details::MockTransactionClient* client, const FindCommandRequest& cmd) {

    try {
        return client->exhaustiveFindSync(cmd);
    } catch (...) {
        return exceptionToStatus();
    }
}


TEST_F(TxnAPITest, TestExhaustiveFindWithSingleBatch) {
    FindCommandRequest findCommand(NamespaceString::createNamespaceString_forTest("foo.bar"));
    findCommand.setBatchSize(1);
    findCommand.setSingleBatch(true);

    const long long cursorId = 0;
    BSONObj firstBatchDoc = BSON("_id" << 0);
    auto findResponse = BSON("cursor" << BSON("id" << cursorId << "ns"
                                                   << "foo.bar"
                                                   << "firstBatch" << BSON_ARRAY(firstBatchDoc))
                                      << "ok" << 1);

    mockClient()->setNextCommandResponse(findResponse);
    auto exhaustiveFindRes = mockClient()->exhaustiveFindSync(findCommand);

    ASSERT_EQ(exhaustiveFindRes.size(), 1);
    ASSERT_BSONOBJ_EQ(exhaustiveFindRes[0], firstBatchDoc);

    // Check that we only ran the find command and no follow up getMores.
    auto lastRequest = mockClient()->getLastSentRequest();
    ASSERT_EQ(lastRequest["find"].String(), "bar");
    ASSERT_EQ(lastRequest["batchSize"].Long(), 1);
    ASSERT_EQ(lastRequest["singleBatch"].Bool(), true);
}

TEST_F(TxnAPITest, TestExhaustiveFindWithMultipleBatches) {
    FindCommandRequest findCommand(NamespaceString::createNamespaceString_forTest("foo.bar"));
    findCommand.setBatchSize(1);
    findCommand.setSingleBatch(false);

    const long long cursorId = 1;
    const long long cursorIdNext = 0;
    BSONObj firstBatchDoc = BSON("_id" << 0);
    BSONObj nextBatchDoc = BSON("_id" << 1);
    auto findResponse = BSON("cursor" << BSON("id" << cursorId << "ns"
                                                   << "foo.bar"
                                                   << "firstBatch" << BSON_ARRAY(firstBatchDoc))
                                      << "ok" << 1);
    auto getMoreResponse = BSON("cursor" << BSON("id" << cursorIdNext << "ns"
                                                      << "foo.bar"
                                                      << "nextBatch" << BSON_ARRAY(nextBatchDoc))
                                         << "ok" << 1);

    mockClient()->setNextCommandResponse(findResponse);
    mockClient()->setNextCommandResponse(getMoreResponse);
    auto exhaustiveFindRes = mockClient()->exhaustiveFindSync(findCommand);

    ASSERT_EQ(exhaustiveFindRes.size(), 2);
    ASSERT_BSONOBJ_EQ(exhaustiveFindRes[0], firstBatchDoc);
    ASSERT_BSONOBJ_EQ(exhaustiveFindRes[1], nextBatchDoc);

    // Check that getMore was run.
    auto lastRequest = mockClient()->getLastSentRequest();
    ASSERT_EQ(lastRequest["getMore"].Long(), 1);
    ASSERT_EQ(lastRequest["batchSize"].Long(), 1);
    ASSERT_EQ(lastRequest["collection"].String(), "bar");
}

TEST_F(TxnAPITest, TestExhaustiveFindErrorOnFind) {
    FindCommandRequest findCommand(NamespaceString::createNamespaceString_forTest("foo.bar"));
    findCommand.setBatchSize(1);
    findCommand.setSingleBatch(true);

    const long long cursorId = 0;
    BSONObj firstBatchDoc = BSON("_id" << 0);
    auto findResponse = BSON("cursor" << BSON("id" << cursorId << "ns"
                                                   << "foo.bar"
                                                   << "firstBatch" << BSON_ARRAY(firstBatchDoc))
                                      << "ok" << 1);

    auto badFindResponse = BSON("ok" << 0 << "code" << ErrorCodes::HostUnreachable);
    mockClient()->setNextCommandResponse(badFindResponse);
    auto exhaustiveFindRes = exhaustiveFindSyncNoThrow(mockClient(), findCommand);

    ASSERT_EQ(exhaustiveFindRes.getStatus(), ErrorCodes::HostUnreachable);

    // Check that we only ran the find command and no follow up getMores.
    auto lastRequest = mockClient()->getLastSentRequest();
    ASSERT_EQ(lastRequest["find"].String(), "bar");
    ASSERT_EQ(lastRequest["batchSize"].Long(), 1);
    ASSERT_EQ(lastRequest["singleBatch"].Bool(), true);
}

TEST_F(TxnAPITest, TestExhaustiveFindErrorOnGetMore) {
    FindCommandRequest findCommand(NamespaceString::createNamespaceString_forTest("foo.bar"));
    findCommand.setBatchSize(2);
    findCommand.setSingleBatch(false);

    const long long cursorId = 1;
    BSONObj firstBatchDoc = BSON("_id" << 0);
    BSONObj nextBatchDoc = BSON("_id" << 1);
    auto findResponse = BSON("cursor" << BSON("id" << cursorId << "ns"
                                                   << "foo.bar"
                                                   << "firstBatch" << BSON_ARRAY(firstBatchDoc))
                                      << "ok" << 1);
    auto badGetMoreResponse = BSON("ok" << 0 << "code" << ErrorCodes::HostUnreachable);

    mockClient()->setNextCommandResponse(findResponse);
    mockClient()->setNextCommandResponse(badGetMoreResponse);
    auto exhaustiveFindRes = exhaustiveFindSyncNoThrow(mockClient(), findCommand);

    ASSERT_EQ(exhaustiveFindRes.getStatus(), ErrorCodes::HostUnreachable);

    // Check that getMore was run.
    auto lastRequest = mockClient()->getLastSentRequest();
    ASSERT_EQ(lastRequest["getMore"].Long(), 1);
    ASSERT_EQ(lastRequest["batchSize"].Long(), 2);
    ASSERT_EQ(lastRequest["collection"].String(), "bar");
}

TEST_F(TxnAPITest, OwnSession_StartTransactionRetryLimitOnTransientErrors) {
    FailPointEnableBlock fp("overrideTransactionApiMaxRetriesToThree");

    int retryCount = 0;
    auto swResult = txnWithRetries().runNoThrow(
        opCtx(), [&](const txn_api::TransactionClient& txnClient, ExecutorPtr txnExec) {
            retryCount++;

            // Command response used for insert below and eventually abortTransaction.
            mockClient()->setNextCommandResponse(kOKCommandResponse);
            auto insertRes = txnClient
                                 .runCommand(DatabaseName(boost::none, "user"_sd),
                                             BSON("insert"
                                                  << "foo"
                                                  << "documents" << BSON_ARRAY(BSON("x" << 1))))
                                 .get();
            uasserted(ErrorCodes::HostUnreachable, "Host unreachable error");
            return SemiFuture<void>::makeReady();
        });
    // The transient error should have been propagated.
    ASSERT_EQ(swResult.getStatus(), ErrorCodes::HostUnreachable);

    // We get 4 due to the initial try and then 3 follow up retries because
    // overrideTransactionApiMaxRetriesToThree sets the max retry attempts to 3.
    ASSERT_EQ(retryCount, 4);

    waitForAllEarlierTasksToComplete();  // Wait for the final best effort abort to complete.
    expectSentAbort(3 /* txnNumber */, WriteConcernOptions().toBSON());
    assertSessionIdMetadata(mockClient()->getLastSentRequest(), LsidAssertion::kStandalone);
}

TEST_F(TxnAPITest, OwnSession_CommitTransactionRetryLimitOnTransientErrors) {
    FailPointEnableBlock fp("overrideTransactionApiMaxRetriesToThree");

    // If we are able to successfully finish this test, then we know that we have limited our
    // retries.
    auto swResult = txnWithRetries().runNoThrow(
        opCtx(), [&](const txn_api::TransactionClient& txnClient, ExecutorPtr txnExec) {
            mockClient()->setNextCommandResponse(kOKInsertResponse);
            auto insertRes = txnClient
                                 .runCommand(DatabaseName(boost::none, "user"_sd),
                                             BSON("insert"
                                                  << "foo"
                                                  << "documents" << BSON_ARRAY(BSON("x" << 1))))
                                 .get();
            ASSERT_EQ(insertRes["n"].Int(), 1);  // Verify the mocked response was returned.
            assertTxnMetadata(
                mockClient()->getLastSentRequest(), 0 /* txnNumber */, true /* startTransaction */);
            assertSessionIdMetadata(mockClient()->getLastSentRequest(), LsidAssertion::kStandalone);

            // The commit response.
            mockClient()->setNextCommandResponse(
                BSON("ok" << 0 << "code" << ErrorCodes::PrimarySteppedDown));
            return SemiFuture<void>::makeReady();
        });

    // The transient error should have been propagated.
    ASSERT_EQ(swResult.getValue().getEffectiveStatus(), ErrorCodes::PrimarySteppedDown);
    auto lastRequest = mockClient()->getLastSentRequest();

    // Retrying commitTransaction uses majority write concern.
    assertTxnMetadata(lastRequest,
                      0 /* txnNumber */,
                      boost::none /* startTransaction */,
                      boost::none /* readConcern */,
                      CommandHelpers::kMajorityWriteConcern.toBSON() /* writeConcern */);
    assertSessionIdMetadata(mockClient()->getLastSentRequest(), LsidAssertion::kStandalone);
    ASSERT_EQ(lastRequest.firstElementFieldNameStringData(), "commitTransaction"_sd);
}

TEST_F(TxnAPITest, MaxTimeMSIsSetIfOperationContextHasDeadlineAndIgnoresDefaultRetryLimit) {
    FailPointEnableBlock fp("overrideTransactionApiMaxRetriesToThree");

    const std::shared_ptr<ClockSourceMock> mockClock = std::make_shared<ClockSourceMock>();
    mockClock->reset(getServiceContext()->getFastClockSource()->now());
    getServiceContext()->setFastClockSource(std::make_unique<SharedClockSourceAdapter>(mockClock));
    int maxTimeMS = 1000 * 60 * 60 * 24;        // 1 day.
    int advanceTimeByMS = 1000 * 60 * 60 * 23;  // 23 hours.
    opCtx()->setDeadlineByDate(mockClock->now() + Milliseconds(maxTimeMS),
                               ErrorCodes::MaxTimeMSExpired);

    // txnNumber will be incremented upon the release of a session.
    resetTxnWithRetries();

    int attempt = -1;
    auto swResult = txnWithRetries().runNoThrow(
        opCtx(), [&](const txn_api::TransactionClient& txnClient, ExecutorPtr txnExec) {
            attempt += 1;

            mockClient()->setNextCommandResponse(kOKInsertResponse);
            auto insertRes = txnClient
                                 .runCommand(DatabaseName(boost::none, "user"_sd),
                                             BSON("insert"
                                                  << "foo"
                                                  << "documents" << BSON_ARRAY(BSON("x" << 1))))
                                 .get();
            ASSERT_EQ(insertRes["n"].Int(), 1);  // Verify the mocked response was returned.
            assertTxnMetadata(mockClient()->getLastSentRequest(),
                              attempt + 1 /* txnNumber */,
                              true /* startTransaction */,
                              boost::none /* readConcern */,
                              boost::none /* writeConcern */,
                              maxTimeMS /* maxTimeMS */);
            assertSessionIdMetadata(mockClient()->getLastSentRequest(), LsidAssertion::kStandalone);

            // Throw a transient error more times than the overriden retry limit to verify the limit
            // is disabled when a deadline is set.
            uassert(ErrorCodes::HostUnreachable, "Host unreachable error", attempt > 3);

            mockClock->advance(Milliseconds(advanceTimeByMS));

            // The commit response.
            mockClient()->setNextCommandResponse(kOKCommandResponse);
            return SemiFuture<void>::makeReady();
        });
    ASSERT(swResult.getStatus().isOK());
    ASSERT(swResult.getValue().getEffectiveStatus().isOK());

    auto lastRequest = mockClient()->getLastSentRequest();
    assertTxnMetadata(lastRequest,
                      attempt + 1 /* txnNumber */,
                      boost::none /* startTransaction */,
                      boost::none /* readConcern */,
                      WriteConcernOptions().toBSON() /* writeConcern */,
                      maxTimeMS - advanceTimeByMS /* maxTimeMS */);
    assertSessionIdMetadata(mockClient()->getLastSentRequest(), LsidAssertion::kStandalone);
    ASSERT_EQ(lastRequest.firstElementFieldNameStringData(), "commitTransaction"_sd);
}

TEST_F(TxnAPITest, CannotBeUsedWithinShardedOperationsIfClientDoesNotSupportIt) {
    OperationShardingState::setShardRole(opCtx(),
                                         NamespaceString::createNamespaceString_forTest("foo.bar"),
                                         ShardVersion(),
                                         boost::none);

    ASSERT_THROWS_CODE(
        resetTxnWithRetries(), DBException, ErrorCodes::duplicateCodeForTest(6638800));
}

TEST_F(TxnAPITest, CanBeUsedWithinShardedOperationsIfClientSupportsIt) {
    OperationShardingState::setShardRole(opCtx(),
                                         NamespaceString::createNamespaceString_forTest("foo.bar"),
                                         ShardVersion(),
                                         boost::none);

    // Should not throw.
    resetTxnWithRetriesWithClient(std::make_unique<MockClusterOperationTransactionClient>());
}

TEST_F(TxnAPITest, DoNotAllowCrossShardTransactionsOnShardWhenInClientTransaction) {
    setMongos(false);
    ON_BLOCK_EXIT([&] { setMongos(true); });

    opCtx()->setLogicalSessionId(makeLogicalSessionIdForTest());
    opCtx()->setTxnNumber(5);
    opCtx()->setInMultiDocumentTransaction();
    ASSERT_THROWS_CODE(
        resetTxnWithRetriesWithClient(std::make_unique<MockClusterOperationTransactionClient>()),
        DBException,
        6648101);
}

TEST_F(TxnAPITest, DoNotAllowCrossShardTransactionsOnShardWhenInRetryableWrite) {
    setMongos(false);
    ON_BLOCK_EXIT([&] { setMongos(true); });

    opCtx()->setLogicalSessionId(makeLogicalSessionIdForTest());
    opCtx()->setTxnNumber(5);
    ASSERT_THROWS_CODE(
        resetTxnWithRetriesWithClient(std::make_unique<MockClusterOperationTransactionClient>()),
        DBException,
        6648100);
}

TEST_F(TxnAPITest, AllowCrossShardTransactionsOnMongosWhenInRetryableWrite) {
    setMongos(true);
    ON_BLOCK_EXIT([&] { setMongos(false); });

    opCtx()->setLogicalSessionId(makeLogicalSessionIdForTest());
    opCtx()->setTxnNumber(5);
    resetTxnWithRetriesWithClient(std::make_unique<MockClusterOperationTransactionClient>());
}

TEST_F(TxnAPITest, AllowCrossShardTransactionsOnMongosWhenInClientTransaction) {
    setMongos(true);
    ON_BLOCK_EXIT([&] { setMongos(false); });

    opCtx()->setLogicalSessionId(makeLogicalSessionIdForTest());
    opCtx()->setTxnNumber(5);
    opCtx()->setInMultiDocumentTransaction();
    resetTxnWithRetriesWithClient(std::make_unique<MockClusterOperationTransactionClient>());
}

TEST_F(TxnAPITest, FailoverAndShutdownErrorsAreFatalForLocalTransactionBodyError) {
    setMongos(false);
    ON_BLOCK_EXIT([&] { setMongos(true); });
    auto runTest = [&](bool expectSuccess, Status status) {
        resetTxnWithRetries();

        int attempt = -1;
        auto swResult = txnWithRetries().runNoThrow(
            opCtx(), [&](const txn_api::TransactionClient& txnClient, ExecutorPtr txnExec) {
                attempt += 1;

                mockClient()->setNextCommandResponse(kOKInsertResponse);
                auto insertRes = txnClient
                                     .runCommand(DatabaseName(boost::none, "user"_sd),
                                                 BSON("insert"
                                                      << "foo"
                                                      << "documents" << BSON_ARRAY(BSON("x" << 1))))
                                     .get();
                ASSERT_OK(getStatusFromWriteCommandReply(insertRes));

                // Only throw once to verify the API gives up right away.
                if (attempt == 0) {
                    uassertStatusOK(status);
                }
                // The commit response.
                mockClient()->setNextCommandResponse(kOKCommandResponse);
                return SemiFuture<void>::makeReady();
            });
        if (!expectSuccess) {
            ASSERT_EQ(swResult.getStatus(), status);

            // The API should have returned without trying to abort.
            auto lastRequest = mockClient()->getLastSentRequest();
            ASSERT_EQ(lastRequest.firstElementFieldNameStringData(), "insert"_sd);
        } else {
            ASSERT(swResult.getStatus().isOK());
            ASSERT(swResult.getValue().getEffectiveStatus().isOK());
            auto lastRequest = mockClient()->getLastSentRequest();
            ASSERT_EQ(lastRequest.firstElementFieldNameStringData(), "commitTransaction"_sd);
        }
    };

    runTest(false, Status(ErrorCodes::InterruptedDueToReplStateChange, "mock repl change error"));
    runTest(false, Status(ErrorCodes::InterruptedAtShutdown, "mock shutdown error"));

    // Verify the fatal for local logic doesn't apply to all transient or retriable errors.
    runTest(true, Status(ErrorCodes::HostUnreachable, "mock transient error"));
}

TEST_F(TxnAPITest, FailoverAndShutdownErrorsAreFatalForLocalTransactionCommandError) {
    setMongos(false);
    ON_BLOCK_EXIT([&] { setMongos(true); });
    auto runTest = [&](bool expectSuccess, Status status) {
        resetTxnWithRetries();

        int attempt = -1;
        auto swResult = txnWithRetries().runNoThrow(
            opCtx(), [&](const txn_api::TransactionClient& txnClient, ExecutorPtr txnExec) {
                attempt += 1;

                mockClient()->setNextCommandResponse(kOKInsertResponse);
                auto insertRes = txnClient
                                     .runCommand(DatabaseName(boost::none, "user"_sd),
                                                 BSON("insert"
                                                      << "foo"
                                                      << "documents" << BSON_ARRAY(BSON("x" << 1))))
                                     .get();
                ASSERT_OK(getStatusFromWriteCommandReply(insertRes));

                // The commit response.
                mockClient()->setNextCommandResponse(BSON("ok" << 0 << "code" << status.code()));
                mockClient()->setNextCommandResponse(kOKCommandResponse);
                return SemiFuture<void>::makeReady();
            });
        if (!expectSuccess) {
            ASSERT(swResult.getStatus().isOK());
            ASSERT_EQ(swResult.getValue().cmdStatus, status);
            ASSERT(swResult.getValue().wcError.toStatus().isOK());

            // The API should have returned without trying to abort.
            auto lastRequest = mockClient()->getLastSentRequest();
            ASSERT_EQ(lastRequest.firstElementFieldNameStringData(), "commitTransaction"_sd);
        } else {
            ASSERT(swResult.getStatus().isOK());
            ASSERT(swResult.getValue().getEffectiveStatus().isOK());
            auto lastRequest = mockClient()->getLastSentRequest();
            ASSERT_EQ(lastRequest.firstElementFieldNameStringData(), "commitTransaction"_sd);
        }
    };

    runTest(false, Status(ErrorCodes::InterruptedDueToReplStateChange, "mock repl change error"));
    runTest(false, Status(ErrorCodes::InterruptedAtShutdown, "mock shutdown error"));

    // Verify the fatal for local logic doesn't apply to all transient or retriable errors.
    runTest(true, Status(ErrorCodes::HostUnreachable, "mock retriable error"));
}

TEST_F(TxnAPITest, FailoverAndShutdownErrorsAreFatalForLocalTransactionWCError) {
    setMongos(false);
    ON_BLOCK_EXIT([&] { setMongos(true); });
    auto runTest = [&](bool expectSuccess, Status status) {
        resetTxnWithRetries();

        int attempt = -1;
        auto swResult = txnWithRetries().runNoThrow(
            opCtx(), [&](const txn_api::TransactionClient& txnClient, ExecutorPtr txnExec) {
                attempt += 1;

                mockClient()->setNextCommandResponse(kOKInsertResponse);
                auto insertRes = txnClient
                                     .runCommand(DatabaseName(boost::none, "user"_sd),
                                                 BSON("insert"
                                                      << "foo"
                                                      << "documents" << BSON_ARRAY(BSON("x" << 1))))
                                     .get();
                ASSERT_OK(getStatusFromWriteCommandReply(insertRes));

                // The commit response.
                auto wcError = BSON("code" << status.code() << "errmsg"
                                           << "mock");
                auto resWithWCError = BSON("ok" << 1 << "writeConcernError" << wcError);
                mockClient()->setNextCommandResponse(resWithWCError);
                mockClient()->setNextCommandResponse(kOKCommandResponse);
                return SemiFuture<void>::makeReady();
            });
        if (!expectSuccess) {
            ASSERT(swResult.getStatus().isOK());
            ASSERT(swResult.getValue().cmdStatus.isOK());
            ASSERT_EQ(swResult.getValue().wcError.toStatus(), status);

            // The API should have returned without trying to abort.
            auto lastRequest = mockClient()->getLastSentRequest();
            ASSERT_EQ(lastRequest.firstElementFieldNameStringData(), "commitTransaction"_sd);
        } else {
            ASSERT(swResult.getStatus().isOK());
            ASSERT(swResult.getValue().getEffectiveStatus().isOK());
            auto lastRequest = mockClient()->getLastSentRequest();
            ASSERT_EQ(lastRequest.firstElementFieldNameStringData(), "commitTransaction"_sd);
        }
    };

    runTest(false, Status(ErrorCodes::InterruptedDueToReplStateChange, "mock repl change error"));
    runTest(false, Status(ErrorCodes::InterruptedAtShutdown, "mock shutdown error"));

    // Verify the fatal for local logic doesn't apply to all transient or retriable errors.
    runTest(true, Status(ErrorCodes::HostUnreachable, "mock retriable error"));
}

// TODO SERVER-75553 - test needs to be aborted and assumes multiple-threads
#if 0
TEST_F(TxnAPITest, DoesNotWaitForBestEffortAbortIfCancelled) {
    // Start the transaction with an insert.
    mockClient()->setNextCommandResponse(kOKInsertResponse);

    // Hang the best effort abort that will be triggered after giving up on the transaction.
    mockClient()->setHangNextAbortCommand(true);
    mockClient()->setNextCommandResponse(kOKCommandResponse);

    auto swResult = txnWithRetries().runNoThrow(
        opCtx(), [&](const txn_api::TransactionClient& txnClient, ExecutorPtr txnExec) {
            txnClient
                .runCommand(DatabaseName(boost::none, "user"_sd),
                            BSON("insert"
                                 << "foo"
                                 << "documents" << BSON_ARRAY(BSON("x" << 1))))
                .get();

            // Interrupting the caller's opCtx prevents waiting for the best effort abort.
            opCtx()->markKilled();
            uasserted(ErrorCodes::InternalError, "Mock non-transient error");
            return SemiFuture<void>::makeReady();
        });
    // The API isn't guaranteed to return the thrown error or an interruption error, but either way
    // it should return a non-ok status.
    ASSERT_FALSE(swResult.getStatus().isOK());

    // The abort should get hung and not have been processed yet.
    mockClient()->waitForHungAbortWaiter();
    auto lastRequest = mockClient()->getLastSentRequest();
    ASSERT_NE(lastRequest.firstElementFieldNameStringData(), "abortTransaction"_sd);

    // Unblock the abort and verify it eventually runs.
    mockClient()->setHangNextAbortCommand(false);
    waitForAllEarlierTasksToComplete();
    expectSentAbort(0 /* txnNumber */, WriteConcernOptions().toBSON());
}

TEST_F(TxnAPITest, WaitsForBestEffortAbortOnNonTransientErrorIfNotCancelled) {
    // Use an executor with higher max threads so a best effort abort could actually run
    // concurrently within the API. Otherwise the API would always wait for the best effort abort
    // even if the wait wasn't explicit.
    ThreadPool::Options options;
    options.poolName = "TxnAPITest-WaitsForBestEffortAbortOnNonTransientErrorIfNotCancelled";
    options.minThreads = 1;
    options.maxThreads = 8;
    auto executor = std::make_shared<executor::ThreadPoolTaskExecutor>(
        std::make_unique<ThreadPool>(std::move(options)),
        executor::makeNetworkInterface("TxnAPITestNetwork"));
    executor->startup();
    auto exec1 = executor::InlineExecutor().getSleepableExecutor(executor);
    resetTxnWithRetries(nullptr /* resourceYielder */, exec1);

    // Start the transaction with an insert.
    mockClient()->setNextCommandResponse(kOKInsertResponse);

    // Hang the best effort abort that will be triggered before retrying after we throw an error.
    mockClient()->setHangNextAbortCommand(true);
    mockClient()->setNextCommandResponse(kOKCommandResponse);

    auto future = stdx::async(stdx::launch::async, [&] {
        auto swResult = txnWithRetries().runNoThrow(
            opCtx(), [&](const txn_api::TransactionClient& txnClient, ExecutorPtr txnExec) {
                txnClient
                    .runCommand(DatabaseName(boost::none, "user"_sd),
                                BSON("insert"
                                     << "foo"
                                     << "documents" << BSON_ARRAY(BSON("x" << 1))))
                    .get();

                // Throw a non-transient error once to trigger a best effort abort with no retry.
                uasserted(ErrorCodes::BadValue, "Mock non-transient error");

                return SemiFuture<void>::makeReady();
            });
        ASSERT_EQ(swResult.getStatus(), ErrorCodes::BadValue);
    });

    // The future should time out since it's blocked waiting for the best effort abort to finish.
    ASSERT(stdx::future_status::ready != future.wait_for(Milliseconds(10).toSystemDuration()));

    // The abort should get hung and not have been processed yet.
    mockClient()->waitForHungAbortWaiter();
    auto lastRequest = mockClient()->getLastSentRequest();
    ASSERT_NE(lastRequest.firstElementFieldNameStringData(), "abortTransaction"_sd);

    // Allow the abort to finish and it should unblock the API.
    mockClient()->setHangNextAbortCommand(false);
    future.get();

    // After the abort finishes, the API should not have retried.
    expectSentAbort(1 /* txnNumber */, WriteConcernOptions().toBSON());

    // Wait for tasks so destructors run on the main test thread.
    executor->shutdown();
    executor->join();
}

TEST_F(TxnAPITest, WaitsForBestEffortAbortOnTransientError) {
    // Use an executor with higher max threads so a best effort abort could actually run
    // concurrently within the API. Otherwise the API would always wait for the best effort abort
    // even if the wait wasn't explicit.
    ThreadPool::Options options;
    options.poolName = "TxnAPITest-WaitsForBestEffortAbortOnTransientError";
    options.minThreads = 1;
    options.maxThreads = 8;
    auto executor = std::make_shared<executor::ThreadPoolTaskExecutor>(
        std::make_unique<ThreadPool>(std::move(options)),
        executor::makeNetworkInterface("TxnAPITestNetwork"));
    executor->startup();
    auto exec1 = executor::InlineExecutor().getSleepableExecutor(executor);

    resetTxnWithRetries(nullptr /* resourceYielder */, exec1);

    // Start the transaction with an insert.
    mockClient()->setNextCommandResponse(kOKInsertResponse);

    // Hang the best effort abort that will be triggered before retrying after we throw an error.
    mockClient()->setHangNextAbortCommand(true);
    mockClient()->setNextCommandResponse(kOKCommandResponse);

    // Second attempt's insert and successful commit response.
    mockClient()->setNextCommandResponse(kOKInsertResponse);
    mockClient()->setNextCommandResponse(kOKCommandResponse);

    auto future = stdx::async(stdx::launch::async, [&] {
        int attempt = 0;
        auto swResult = txnWithRetries().runNoThrow(
            opCtx(), [&](const txn_api::TransactionClient& txnClient, ExecutorPtr txnExec) {
                txnClient
                    .runCommand(DatabaseName(boost::none, "user"_sd),
                                BSON("insert"
                                     << "foo"
                                     << "documents" << BSON_ARRAY(BSON("x" << 1))))
                    .get();

                // Throw a transient error once to trigger a best effort abort then retry.
                uassert(ErrorCodes::HostUnreachable, "Mock network error", attempt++ > 0);

                return SemiFuture<void>::makeReady();
            });
        ASSERT(swResult.getStatus().isOK());
        ASSERT(swResult.getValue().getEffectiveStatus().isOK());
    });

    // The future should time out since it's blocked waiting for the best effort abort to finish.
    ASSERT(stdx::future_status::ready != future.wait_for(Milliseconds(10).toSystemDuration()));

    // The abort should get hung and not have been processed yet.
    mockClient()->waitForHungAbortWaiter();
    auto lastRequest = mockClient()->getLastSentRequest();
    ASSERT_NE(lastRequest.firstElementFieldNameStringData(), "abortTransaction"_sd);

    // Allow the abort to finish and it should unblock the API.
    mockClient()->setHangNextAbortCommand(false);
    future.get();

    // After the abort finishes, the API should retry and successfully commit.
    lastRequest = mockClient()->getLastSentRequest();
    assertTxnMetadata(lastRequest,
                      2 /* txnNumber */,
                      boost::none /* startTransaction */,
                      boost::none /* readConcern */,
                      WriteConcernOptions().toBSON() /* writeConcern */);
    ASSERT_EQ(lastRequest.firstElementFieldNameStringData(), "commitTransaction"_sd);

    // Wait for tasks so destructors run on the main test thread.
    executor->shutdown();
    executor->join();
}
#endif

}  // namespace
}  // namespace mongo
