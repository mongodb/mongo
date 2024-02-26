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

#include "mongo/bson/timestamp.h"
#include "mongo/db/repl/read_concern_args.h"
#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
// IWYU pragma: no_include "cxxabi.h"
// IWYU pragma: no_include "ext/alloc_traits.h"
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <future>
#include <iterator>
#include <system_error>
#include <type_traits>

#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/bson/bsontypes_util.h"
#include "mongo/bson/util/builder.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/db/catalog/collection_catalog.h"
#include "mongo/db/catalog/collection_options.h"
#include "mongo/db/catalog/collection_write_path.h"
#include "mongo/db/catalog/database.h"
#include "mongo/db/catalog/local_oplog_info.h"
#include "mongo/db/catalog_raii.h"
#include "mongo/db/client.h"
#include "mongo/db/cluster_role.h"
#include "mongo/db/concurrency/lock_manager_defs.h"
#include "mongo/db/curop.h"
#include "mongo/db/database_name.h"
#include "mongo/db/dbhelpers.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/global_settings.h"
#include "mongo/db/op_observer/op_observer.h"
#include "mongo/db/op_observer/op_observer_impl.h"
#include "mongo/db/op_observer/op_observer_noop.h"
#include "mongo/db/op_observer/op_observer_registry.h"
#include "mongo/db/op_observer/operation_logger_impl.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/member_state.h"
#include "mongo/db/repl/mock_repl_coord_server_fixture.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/db/repl/oplog_entry.h"
#include "mongo/db/repl/oplog_entry_gen.h"
#include "mongo/db/repl/optime.h"
#include "mongo/db/repl/repl_settings.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/repl/replication_coordinator_mock.h"
#include "mongo/db/repl/storage_interface.h"
#include "mongo/db/repl/storage_interface_impl.h"
#include "mongo/db/server_options.h"
#include "mongo/db/service_context.h"
#include "mongo/db/service_context_d_test_fixture.h"
#include "mongo/db/session/internal_session_pool.h"
#include "mongo/db/session/logical_session_id_helpers.h"
#include "mongo/db/session/session_catalog.h"
#include "mongo/db/session/session_catalog_mongod.h"
#include "mongo/db/session/session_txn_record_gen.h"
#include "mongo/db/shard_id.h"
#include "mongo/db/shard_role.h"
#include "mongo/db/storage/durable_history_pin.h"
#include "mongo/db/storage/record_data.h"
#include "mongo/db/storage/record_store.h"
#include "mongo/db/storage/storage_stats.h"
#include "mongo/db/tenant_id.h"
#include "mongo/db/transaction/server_transactions_metrics.h"
#include "mongo/db/transaction/session_catalog_mongod_transaction_interface_impl.h"
#include "mongo/db/transaction/transaction_participant.h"
#include "mongo/db/transaction/transaction_participant_gen.h"
#include "mongo/db/transaction_resources.h"
#include "mongo/db/txn_retry_counter_too_old_info.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/idl/server_parameter_test_util.h"
#include "mongo/logv2/log.h"
#include "mongo/logv2/log_attr.h"
#include "mongo/logv2/log_component.h"
#include "mongo/logv2/log_severity.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/rpc/metadata/client_metadata.h"
#include "mongo/s/session_catalog_router.h"
#include "mongo/s/transaction_router.h"
#include "mongo/stdx/future.h"
#include "mongo/unittest/assert.h"
#include "mongo/unittest/barrier.h"
#include "mongo/unittest/bson_test_util.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/framework.h"
#include "mongo/unittest/log_test.h"
#include "mongo/util/clock_source_mock.h"
#include "mongo/util/decorable.h"
#include "mongo/util/duration.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/net/socket_utils.h"
#include "mongo/util/scopeguard.h"
#include "mongo/util/str.h"
#include "mongo/util/tick_source.h"
#include "mongo/util/tick_source_mock.h"
#include "mongo/util/uuid.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kDefault

namespace mongo {
namespace {

const NamespaceString kNss = NamespaceString::createNamespaceString_forTest("TestDB", "TestColl");

/**
 * Creates an OplogEntry with given parameters and preset defaults for this test suite.
 */
repl::OplogEntry makeOplogEntry(repl::OpTime opTime,
                                repl::OpTypeEnum opType,
                                BSONObj object,
                                OperationSessionInfo sessionInfo,
                                Date_t wallClockTime,
                                const std::vector<StmtId>& stmtIds,
                                boost::optional<repl::OpTime> prevWriteOpTimeInTransaction) {
    return repl::DurableOplogEntry(
        opTime,                        // optime
        opType,                        // opType
        kNss,                          // namespace
        boost::none,                   // uuid
        boost::none,                   // fromMigrate
        boost::none,                   // checkExistenceForDiffInsert
        0,                             // version
        object,                        // o
        boost::none,                   // o2
        sessionInfo,                   // sessionInfo
        boost::none,                   // upsert
        wallClockTime,                 // wall clock time
        stmtIds,                       // statement ids
        prevWriteOpTimeInTransaction,  // optime of previous write within same transaction
        boost::none,                   // pre-image optime
        boost::none,                   // post-image optime
        boost::none,                   // ShardId of resharding recipient
        boost::none,                   // _id
        boost::none);                  // needsRetryImage
}

class OpObserverMock : public OpObserverNoop {
public:
    /**
     * TransactionPartipant calls onTransactionPrepare() within a side transaction. The boundaries
     * of this side transaction may be defined within OpObserverImpl or TransactionParticipant. To
     * verify the transaction lifecycle outside any side transaction boundaries, we override
     * postTransactionPrepare() instead of onTransactionPrepare().
     *
     * Note that OpObserverImpl is not registered with the OpObserverRegistry in TxtParticipantTest
     * setup. Only a small handful of tests explicitly register OpObserverImpl and these tests do
     * no override the callback `postTransactionPrepareFn`.
     */
    void postTransactionPrepare(OperationContext* opCtx,
                                const std::vector<OplogSlot>& reservedSlots,
                                const TransactionOperations& transactionOperations) override;

    bool postTransactionPrepareThrowsException = false;
    bool transactionPrepared = false;
    std::function<void()> postTransactionPrepareFn = []() {
    };

    void onUnpreparedTransactionCommit(
        OperationContext* opCtx,
        const std::vector<OplogSlot>& reservedSlots,
        const TransactionOperations& transactionOperations,
        const ApplyOpsOplogSlotAndOperationAssignment& applyOpsOperationAssignment,
        OpStateAccumulator* opAccumulator = nullptr) override;
    bool onUnpreparedTransactionCommitThrowsException = false;
    bool unpreparedTransactionCommitted = false;
    std::function<void(const std::vector<repl::ReplOperation>&)> onUnpreparedTransactionCommitFn =
        [](const std::vector<repl::ReplOperation>& statements) {
        };


    void onPreparedTransactionCommit(
        OperationContext* opCtx,
        OplogSlot commitOplogEntryOpTime,
        Timestamp commitTimestamp,
        const std::vector<repl::ReplOperation>& statements) noexcept override;
    bool onPreparedTransactionCommitThrowsException = false;
    bool preparedTransactionCommitted = false;
    std::function<void(OplogSlot, Timestamp, const std::vector<repl::ReplOperation>&)>
        onPreparedTransactionCommitFn = [](OplogSlot commitOplogEntryOpTime,
                                           Timestamp commitTimestamp,
                                           const std::vector<repl::ReplOperation>& statements) {
        };

    void onTransactionAbort(OperationContext* opCtx,
                            boost::optional<OplogSlot> abortOplogEntryOpTime) override;
    bool onTransactionAbortThrowsException = false;
    bool transactionAborted = false;

    repl::OpTime onDropCollection(OperationContext* opCtx,
                                  const NamespaceString& collectionName,
                                  const UUID& uuid,
                                  std::uint64_t numRecords,
                                  CollectionDropType dropType,
                                  bool markFromMigrate) override;

    const repl::OpTime dropOpTime = {Timestamp(Seconds(100), 1U), 1LL};
};

void OpObserverMock::postTransactionPrepare(OperationContext* opCtx,
                                            const std::vector<OplogSlot>& reservedSlots,
                                            const TransactionOperations& transactionOperations) {
    ASSERT_TRUE(shard_role_details::getLocker(opCtx)->inAWriteUnitOfWork());

    uassert(ErrorCodes::OperationFailed,
            "postTransactionPrepare() failed",
            !postTransactionPrepareThrowsException);
    transactionPrepared = true;
    postTransactionPrepareFn();
}

void OpObserverMock::onUnpreparedTransactionCommit(
    OperationContext* opCtx,
    const std::vector<OplogSlot>& reservedSlots,
    const TransactionOperations& transactionOperations,
    const ApplyOpsOplogSlotAndOperationAssignment& applyOpsOperationAssignment,
    OpStateAccumulator* opAccumulator) {
    ASSERT(shard_role_details::getLocker(opCtx)->inAWriteUnitOfWork());

    uassert(ErrorCodes::OperationFailed,
            "onUnpreparedTransactionCommit() failed",
            !onUnpreparedTransactionCommitThrowsException);

    ASSERT_EQUALS(transactionOperations.numOperations() +
                      transactionOperations.getNumberOfPrePostImagesToWrite(),
                  reservedSlots.size());
    ASSERT_FALSE(applyOpsOperationAssignment.prepare);

    unpreparedTransactionCommitted = true;
    const auto& statements = transactionOperations.getOperationsForOpObserver();
    onUnpreparedTransactionCommitFn(statements);
}

void OpObserverMock::onPreparedTransactionCommit(
    OperationContext* opCtx,
    OplogSlot commitOplogEntryOpTime,
    Timestamp commitTimestamp,
    const std::vector<repl::ReplOperation>& statements) noexcept {
    ASSERT_FALSE(shard_role_details::getLocker(opCtx)->inAWriteUnitOfWork());
    // The 'commitTimestamp' must be cleared before we write the oplog entry.
    ASSERT(shard_role_details::getRecoveryUnit(opCtx)->getCommitTimestamp().isNull());

    OpObserverNoop::onPreparedTransactionCommit(
        opCtx, commitOplogEntryOpTime, commitTimestamp, statements);
    uassert(ErrorCodes::OperationFailed,
            "onPreparedTransactionCommit() failed",
            !onPreparedTransactionCommitThrowsException);
    preparedTransactionCommitted = true;
    onPreparedTransactionCommitFn(commitOplogEntryOpTime, commitTimestamp, statements);
}

void OpObserverMock::onTransactionAbort(OperationContext* opCtx,
                                        boost::optional<OplogSlot> abortOplogEntryOpTime) {
    OpObserverNoop::onTransactionAbort(opCtx, abortOplogEntryOpTime);
    uassert(ErrorCodes::OperationFailed,
            "onTransactionAbort() failed",
            !onTransactionAbortThrowsException);
    transactionAborted = true;
}

repl::OpTime OpObserverMock::onDropCollection(OperationContext* opCtx,
                                              const NamespaceString& collectionName,
                                              const UUID& uuid,
                                              std::uint64_t numRecords,
                                              const CollectionDropType dropType,
                                              bool markFromMigrate) {
    // If the oplog is not disabled for this namespace, then we need to reserve an op time for the
    // drop.
    if (!repl::ReplicationCoordinator::get(opCtx)->isOplogDisabledFor(opCtx, collectionName)) {
        OpObserver::Times::get(opCtx).reservedOpTimes.push_back(dropOpTime);
    }
    return {};
}

/**
 * When this class is in scope, makes the system behave as if we're in a DBDirectClient.
 */
class DirectClientSetter {
public:
    explicit DirectClientSetter(OperationContext* opCtx)
        : _opCtx(opCtx), _wasInDirectClient(opCtx->getClient()->isInDirectClient()) {
        opCtx->getClient()->setInDirectClient(true);
    }

    ~DirectClientSetter() {
        _opCtx->getClient()->setInDirectClient(_wasInDirectClient);
    }

private:
    const OperationContext* _opCtx;
    const bool _wasInDirectClient;
};

class TxnParticipantTest : public MockReplCoordServerFixture {
protected:
    TxnParticipantTest(Options options = {}) : MockReplCoordServerFixture(std::move(options)) {}

    void setUp() override {
        repl::ReplSettings replSettings;
        replSettings.setReplSetString("realReplicaSet");
        setGlobalReplSettings(replSettings);

        MockReplCoordServerFixture::setUp();
        const auto service = opCtx()->getServiceContext();

        repl::StorageInterface::set(service, std::make_unique<repl::StorageInterfaceImpl>());
        MongoDSessionCatalog::set(
            service,
            std::make_unique<MongoDSessionCatalog>(
                std::make_unique<MongoDSessionCatalogTransactionInterfaceImpl>()));
        auto mongoDSessionCatalog = MongoDSessionCatalog::get(opCtx());
        mongoDSessionCatalog->onStepUp(opCtx());

        OpObserverRegistry* opObserverRegistry =
            dynamic_cast<OpObserverRegistry*>(service->getOpObserver());
        auto mockObserver = std::make_unique<OpObserverMock>();
        _opObserver = mockObserver.get();
        opObserverRegistry->addObserver(std::move(mockObserver));

        {
            // Set up a collection so that TransactionParticipant::prepareTransaction() can safely
            // access it.
            AutoGetDb autoDb(opCtx(), kNss.dbName(), MODE_X);
            auto db = autoDb.ensureDbExists(opCtx());
            ASSERT_TRUE(db);

            WriteUnitOfWork wuow(opCtx());
            CollectionOptions options;
            options.uuid = _uuid;
            db->createCollection(opCtx(), kNss, options);
            wuow.commit();
        }

        opCtx()->setLogicalSessionId(_sessionId);
        opCtx()->setTxnNumber(_txnNumber);
        opCtx()->setInMultiDocumentTransaction();

        // Normally, committing a transaction is supposed to usassert if the corresponding prepare
        // has not been majority committed. We excempt our unit tests from this expectation.
        setGlobalFailPoint("skipCommitTxnCheckPrepareMajorityCommitted",
                           BSON("mode"
                                << "alwaysOn"));
    }

    void tearDown() override {
        _opObserver = nullptr;

        // Clear all sessions to free up any stashed resources.
        SessionCatalog::get(opCtx()->getServiceContext())->reset_forTest();

        MockReplCoordServerFixture::tearDown();

        setGlobalFailPoint("skipCommitTxnCheckPrepareMajorityCommitted",
                           BSON("mode"
                                << "off"));
    }

    SessionCatalog* catalog() {
        return SessionCatalog::get(opCtx()->getServiceContext());
    }

    void runFunctionFromDifferentOpCtx(std::function<void(OperationContext*)> func) {
        // Create a new client (e.g. for migration) and opCtx.
        auto newClientOwned = getServiceContext()->getService()->makeClient("newClient");
        AlternativeClientRegion acr(newClientOwned);
        auto newOpCtx = cc().makeOperationContext();
        func(newOpCtx.get());
    }

    std::unique_ptr<MongoDSessionCatalog::Session> checkOutSession(
        TransactionParticipant::TransactionActions startNewTxn =
            TransactionParticipant::TransactionActions::kStart) {
        opCtx()->setInMultiDocumentTransaction();
        auto mongoDSessionCatalog = MongoDSessionCatalog::get(opCtx());
        auto opCtxSession = mongoDSessionCatalog->checkOutSession(opCtx());
        auto txnParticipant = TransactionParticipant::get(opCtx());
        txnParticipant.beginOrContinue(opCtx(),
                                       {*opCtx()->getTxnNumber()},
                                       false /* autocommit */,
                                       startNewTxn /* startTransaction */);
        return opCtxSession;
    }

    void callUnderSplitSessionNoUnstash(
        const InternalSessionPool::Session& session,
        std::function<void(OperationContext* opCtx,
                           TransactionParticipant::Participant& txnParticipant)> fn) {
        runFunctionFromDifferentOpCtx([&](OperationContext* opCtx) {
            // Prepared writes as part of a split session must be done with an
            // `UnreplicatedWritesBlock`. This is how we mimic oplog application.
            repl::UnreplicatedWritesBlock notReplicated(opCtx);
            opCtx->setLogicalSessionId(session.getSessionId());
            opCtx->setTxnNumber(session.getTxnNumber());
            opCtx->setInMultiDocumentTransaction();

            auto mongoDSessionCatalog = MongoDSessionCatalog::get(opCtx);
            std::unique_ptr<MongoDSessionCatalog::Session> session =
                mongoDSessionCatalog->checkOutSession(opCtx);

            auto newTxnParticipant = TransactionParticipant::get(opCtx);
            fn(opCtx, newTxnParticipant);

            session->checkIn(opCtx, OperationContextSession::CheckInReason::kDone);
        });
    }

    void callUnderSplitSession(const InternalSessionPool::Session& session,
                               std::function<void(OperationContext* opCtx)> fn) {
        runFunctionFromDifferentOpCtx([&](OperationContext* opCtx) {
            // Prepared writes as part of a split session must be done with an
            // `UnreplicatedWritesBlock`. This is how we mimic oplog application.
            repl::UnreplicatedWritesBlock notReplicated(opCtx);
            opCtx->setLogicalSessionId(session.getSessionId());
            opCtx->setTxnNumber(session.getTxnNumber());
            opCtx->setInMultiDocumentTransaction();

            auto mongoDSessionCatalog = MongoDSessionCatalog::get(opCtx);
            std::unique_ptr<MongoDSessionCatalog::Session> session =
                mongoDSessionCatalog->checkOutSession(opCtx);
            auto newTxnParticipant = TransactionParticipant::get(opCtx);
            newTxnParticipant.beginOrContinueTransactionUnconditionally(opCtx,
                                                                        {*(opCtx->getTxnNumber())});
            newTxnParticipant.unstashTransactionResources(opCtx, "crud ops");

            fn(opCtx);

            newTxnParticipant.stashTransactionResources(opCtx);
            session->checkIn(opCtx, OperationContextSession::CheckInReason::kDone);
        });
    }

    void assertSessionState(BSONObj obj, DurableTxnStateEnum expectedState) {
        SessionTxnRecord txnRecord =
            SessionTxnRecord::parse(IDLParserContext("test sessn txn parser"), obj);
        ASSERT_EQ(expectedState, txnRecord.getState().get());
    }

    void assertNotInSessionState(BSONObj obj, DurableTxnStateEnum expectedState) {
        SessionTxnRecord txnRecord =
            SessionTxnRecord::parse(IDLParserContext("test sessn txn parser"), obj);
        ASSERT_NE(expectedState, txnRecord.getState().get());
    }

    const LogicalSessionId _sessionId{makeLogicalSessionIdForTest()};
    const TxnNumber _txnNumber{20};
    const UUID _uuid = UUID::gen();

    OpObserverMock* _opObserver = nullptr;
};

namespace {
void insertTxnRecord(OperationContext* opCtx, unsigned i, DurableTxnStateEnum state) {
    const auto& nss = NamespaceString::kSessionTransactionsTableNamespace;

    Timestamp ts(1, i);
    SessionTxnRecord record;
    record.setStartOpTime(repl::OpTime(ts, 0));
    record.setState(state);
    record.setSessionId(makeLogicalSessionIdForTest());
    record.setTxnNum(1);
    record.setLastWriteOpTime(repl::OpTime(ts, 0));
    record.setLastWriteDate(Date_t::now());

    AutoGetDb autoDb(opCtx, nss.dbName(), MODE_X);
    auto db = autoDb.ensureDbExists(opCtx);
    ASSERT(db);
    WriteUnitOfWork wuow(opCtx);
    auto coll = CollectionCatalog::get(opCtx)->lookupCollectionByNamespace(opCtx, nss);
    ASSERT(coll);
    OpDebug* const nullOpDebug = nullptr;
    ASSERT_OK(collection_internal::insertDocument(
        opCtx, CollectionPtr(coll), InsertStatement(record.toBSON()), nullOpDebug, false));
    wuow.commit();
}
}  // namespace

TEST_F(TxnParticipantTest, IsActiveTransactionParticipantSetCorrectly) {
    auto sessionCheckout = checkOutSession();
    ASSERT(TransactionParticipant::get(opCtx()));
    ASSERT(opCtx()->isActiveTransactionParticipant());
}

// Test that transaction lock acquisition times out in `maxTransactionLockRequestTimeoutMillis`
// milliseconds.
TEST_F(TxnParticipantTest, TransactionThrowsLockTimeoutIfLockIsUnavailable) {
    const std::string dbName = "TestDB";

    auto sessionCheckout = checkOutSession();
    auto txnParticipant = TransactionParticipant::get(opCtx());
    txnParticipant.unstashTransactionResources(opCtx(), "insert");

    {
        Lock::DBLock dbXLock(
            opCtx(), DatabaseName::createDatabaseName_forTest(boost::none, dbName), MODE_X);
    }
    txnParticipant.stashTransactionResources(opCtx());
    auto clientWithDatabaseXLock = Client::releaseCurrent();


    /**
     * Make a new Session, Client, OperationContext and transaction and then attempt to take the
     * same database exclusive lock, which should conflict because the other transaction already
     * took it.
     */

    auto service = opCtx()->getServiceContext();
    auto newClientOwned = service->getService()->makeClient("newTransactionClient");
    auto newClient = newClientOwned.get();
    Client::setCurrent(std::move(newClientOwned));

    const auto newSessionId = makeLogicalSessionIdForTest();
    const TxnNumber newTxnNum = 10;
    {
        // Limit the scope of the new opCtx to make sure that it gets destroyed before
        // new client is destroyed.
        auto newOpCtx = newClient->makeOperationContext();
        newOpCtx.get()->setLogicalSessionId(newSessionId);
        newOpCtx.get()->setTxnNumber(newTxnNum);
        newOpCtx.get()->setInMultiDocumentTransaction();

        auto mongoDSessionCatalog = MongoDSessionCatalog::get(newOpCtx.get());
        auto newOpCtxSession = mongoDSessionCatalog->checkOutSession(newOpCtx.get());
        auto newTxnParticipant = TransactionParticipant::get(newOpCtx.get());
        newTxnParticipant.beginOrContinue(newOpCtx.get(),
                                          {newTxnNum},
                                          false /* autocommit */,
                                          TransactionParticipant::TransactionActions::kStart);
        newTxnParticipant.unstashTransactionResources(newOpCtx.get(), "insert");

        Date_t t1 = Date_t::now();
        ASSERT_THROWS_CODE(
            Lock::DBLock(newOpCtx.get(),
                         DatabaseName::createDatabaseName_forTest(boost::none, dbName),
                         MODE_X),
            AssertionException,
            ErrorCodes::LockTimeout);
        Date_t t2 = Date_t::now();
        int defaultMaxTransactionLockRequestTimeoutMillis = 5;
        ASSERT_GTE(t2 - t1, Milliseconds(defaultMaxTransactionLockRequestTimeoutMillis));

        // A non-conflicting lock acquisition should work just fine.
        {
            Lock::DBLock tempLock(
                newOpCtx.get(),
                DatabaseName::createDatabaseName_forTest(boost::none, "NewTestDB"),
                MODE_X);
        }
    }
    // Restore the original client so that teardown works.
    Client::releaseCurrent();
    Client::setCurrent(std::move(clientWithDatabaseXLock));
}

TEST_F(TxnParticipantTest, StashAndUnstashResources) {
    Locker* originalLocker = shard_role_details::getLocker(opCtx());
    RecoveryUnit* originalRecoveryUnit = shard_role_details::getRecoveryUnit(opCtx());
    ASSERT(originalLocker);
    ASSERT(originalRecoveryUnit);

    auto sessionCheckout = checkOutSession();

    repl::ReadConcernArgs readConcernArgs;
    ASSERT_OK(
        readConcernArgs.initialize(BSON("find"
                                        << "test" << repl::ReadConcernArgs::kReadConcernFieldName
                                        << BSON(repl::ReadConcernArgs::kLevelFieldName
                                                << "snapshot"))));
    repl::ReadConcernArgs::get(opCtx()) = readConcernArgs;

    // Perform initial unstash which sets up a WriteUnitOfWork.
    auto txnParticipant = TransactionParticipant::get(opCtx());
    txnParticipant.unstashTransactionResources(opCtx(), "find");
    ASSERT_EQUALS(originalLocker, shard_role_details::getLocker(opCtx()));
    ASSERT_EQUALS(originalRecoveryUnit, shard_role_details::getRecoveryUnit(opCtx()));
    ASSERT(shard_role_details::getWriteUnitOfWork(opCtx()));
    ASSERT(shard_role_details::getLocker(opCtx())->isLocked());

    // Stash resources. The original Locker and RecoveryUnit now belong to the stash.
    txnParticipant.stashTransactionResources(opCtx());
    ASSERT_NOT_EQUALS(originalLocker, shard_role_details::getLocker(opCtx()));
    ASSERT_NOT_EQUALS(originalRecoveryUnit, shard_role_details::getRecoveryUnit(opCtx()));
    ASSERT(!shard_role_details::getWriteUnitOfWork(opCtx()));

    // Unset the read concern on the OperationContext. This is needed to unstash.
    repl::ReadConcernArgs::get(opCtx()) = repl::ReadConcernArgs();

    // Unstash the stashed resources. This restores the original Locker and RecoveryUnit to the
    // OperationContext.
    txnParticipant.unstashTransactionResources(opCtx(), "find");
    ASSERT_EQUALS(originalLocker, shard_role_details::getLocker(opCtx()));
    ASSERT_EQUALS(originalRecoveryUnit, shard_role_details::getRecoveryUnit(opCtx()));
    ASSERT(shard_role_details::getWriteUnitOfWork(opCtx()));

    // Commit the transaction. This allows us to release locks.
    txnParticipant.commitUnpreparedTransaction(opCtx());
}

TEST_F(TxnParticipantTest, CannotSpecifyStartTransactionOnInProgressTxn) {
    // Must specify startTransaction=true and autocommit=false to start a transaction.
    auto sessionCheckout = checkOutSession();
    auto txnParticipant = TransactionParticipant::get(opCtx());
    ASSERT_TRUE(txnParticipant.transactionIsOpen());

    // Cannot try to start a transaction that already started.
    ASSERT_THROWS_CODE(
        txnParticipant.beginOrContinue(opCtx(),
                                       {*opCtx()->getTxnNumber(), boost::none},
                                       false /* autocommit */,
                                       TransactionParticipant::TransactionActions::kStart),
        AssertionException,
        ErrorCodes::ConflictingOperationInProgress);
}

TEST_F(TxnParticipantTest, AutocommitRequiredOnEveryTxnOp) {
    auto sessionCheckout = checkOutSession();
    auto txnParticipant = TransactionParticipant::get(opCtx());

    // We must have stashed transaction resources to do a second operation on the transaction.
    txnParticipant.unstashTransactionResources(opCtx(), "insert");
    // The transaction machinery cannot store an empty locker.
    { Lock::GlobalLock lk(opCtx(), MODE_IX, Date_t::now(), Lock::InterruptBehavior::kThrow); }
    txnParticipant.stashTransactionResources(opCtx());

    auto txnNum = *opCtx()->getTxnNumber();
    // Omitting 'autocommit' after the first statement of a transaction should throw an error.
    ASSERT_THROWS_CODE(
        txnParticipant.beginOrContinue(opCtx(),
                                       {txnNum},
                                       boost::none /* autocommit */,
                                       TransactionParticipant::TransactionActions::kNone),
        AssertionException,
        ErrorCodes::IncompleteTransactionHistory);

    // Including autocommit=false should succeed.
    txnParticipant.beginOrContinue(opCtx(),
                                   {*opCtx()->getTxnNumber()},
                                   false /* autocommit */,
                                   TransactionParticipant::TransactionActions::kContinue);
}

DEATH_TEST_F(TxnParticipantTest, AutocommitCannotBeTrue3, "invariant") {
    auto sessionCheckout = checkOutSession();
    auto txnParticipant = TransactionParticipant::get(opCtx());

    // Passing 'autocommit=true' is not allowed and should crash.
    txnParticipant.beginOrContinue(opCtx(),
                                   {*opCtx()->getTxnNumber()},
                                   true /* autocommit */,
                                   TransactionParticipant::TransactionActions::kNone);
}

TEST_F(TxnParticipantTest, SameTransactionPreservesStoredStatements) {
    auto sessionCheckout = checkOutSession();
    auto txnParticipant = TransactionParticipant::get(opCtx());

    // We must have stashed transaction resources to re-open the transaction.
    txnParticipant.unstashTransactionResources(opCtx(), "insert");
    auto operation = repl::DurableOplogEntry::makeInsertOperation(
        kNss, _uuid, BSON("_id" << 0), BSON("_id" << 0));
    txnParticipant.addTransactionOperation(opCtx(), operation);
    ASSERT_BSONOBJ_EQ(operation.toBSON(),
                      txnParticipant.getTransactionOperationsForTest()[0].toBSON());
    // The transaction machinery cannot store an empty locker.
    { Lock::GlobalLock lk(opCtx(), MODE_IX, Date_t::now(), Lock::InterruptBehavior::kThrow); }
    txnParticipant.stashTransactionResources(opCtx());

    // Check the transaction operations before re-opening the transaction.
    ASSERT_BSONOBJ_EQ(operation.toBSON(),
                      txnParticipant.getTransactionOperationsForTest()[0].toBSON());

    // Re-opening the same transaction should have no effect.
    txnParticipant.beginOrContinue(opCtx(),
                                   {*opCtx()->getTxnNumber()},
                                   false /* autocommit */,
                                   TransactionParticipant::TransactionActions::kContinue);
    ASSERT_BSONOBJ_EQ(operation.toBSON(),
                      txnParticipant.getTransactionOperationsForTest()[0].toBSON());
}

TEST_F(TxnParticipantTest, AbortClearsStoredStatements) {
    auto sessionCheckout = checkOutSession();
    auto txnParticipant = TransactionParticipant::get(opCtx());
    txnParticipant.unstashTransactionResources(opCtx(), "insert");
    auto operation = repl::DurableOplogEntry::makeInsertOperation(
        kNss, _uuid, BSON("_id" << 0), BSON("_id" << 0));
    txnParticipant.addTransactionOperation(opCtx(), operation);
    ASSERT_BSONOBJ_EQ(operation.toBSON(),
                      txnParticipant.getTransactionOperationsForTest()[0].toBSON());

    // The transaction machinery cannot store an empty locker.
    { Lock::GlobalLock lk(opCtx(), MODE_IX, Date_t::now(), Lock::InterruptBehavior::kThrow); }
    txnParticipant.stashTransactionResources(opCtx());
    txnParticipant.abortTransaction(opCtx());
    ASSERT_TRUE(txnParticipant.getTransactionOperationsForTest().empty());
    ASSERT_TRUE(txnParticipant.transactionIsAborted());
}

// This test makes sure the commit machinery works even when no operations are done on the
// transaction.
TEST_F(TxnParticipantTest, EmptyUnpreparedTransactionCommit) {
    auto sessionCheckout = checkOutSession();
    auto txnParticipant = TransactionParticipant::get(opCtx());
    txnParticipant.unstashTransactionResources(opCtx(), "commitTransaction");

    // The transaction machinery cannot store an empty locker.
    { Lock::GlobalLock lk(opCtx(), MODE_IX, Date_t::now(), Lock::InterruptBehavior::kThrow); }
    txnParticipant.commitUnpreparedTransaction(opCtx());
    txnParticipant.stashTransactionResources(opCtx());

    ASSERT_TRUE(txnParticipant.transactionIsCommitted());
}

// This test makes sure the commit machinery works even when no operations are done on the
// transaction.
TEST_F(TxnParticipantTest, EmptyPreparedTransactionCommit) {
    auto sessionCheckout = checkOutSession();
    auto txnParticipant = TransactionParticipant::get(opCtx());
    txnParticipant.unstashTransactionResources(opCtx(), "commitTransaction");

    // The transaction machinery cannot store an empty locker.
    { Lock::GlobalLock lk(opCtx(), MODE_IX, Date_t::now(), Lock::InterruptBehavior::kThrow); }
    const auto [prepareTimestamp, namespaces] = txnParticipant.prepareTransaction(opCtx(), {});
    const auto commitTS = Timestamp(prepareTimestamp.getSecs(), prepareTimestamp.getInc() + 1);
    txnParticipant.commitPreparedTransaction(opCtx(), commitTS, {});
    txnParticipant.stashTransactionResources(opCtx());

    ASSERT_TRUE(txnParticipant.transactionIsCommitted());
}

TEST_F(TxnParticipantTest, PrepareSucceedsWithNestedLocks) {
    auto sessionCheckout = checkOutSession();
    auto txnParticipant = TransactionParticipant::get(opCtx());
    txnParticipant.unstashTransactionResources(opCtx(), "commitTransaction");

    {
        Lock::GlobalLock lk1(opCtx(), MODE_IX, Date_t::now(), Lock::InterruptBehavior::kThrow);
        Lock::GlobalLock lk2(opCtx(), MODE_IX, Date_t::now(), Lock::InterruptBehavior::kThrow);
    }

    const auto [prepareTimestamp, namespaces] = txnParticipant.prepareTransaction(opCtx(), {});
    const auto commitTS = Timestamp(prepareTimestamp.getSecs(), prepareTimestamp.getInc() + 1);
    txnParticipant.commitPreparedTransaction(opCtx(), commitTS, {});
    txnParticipant.stashTransactionResources(opCtx());

    ASSERT_TRUE(txnParticipant.transactionIsCommitted());
}

TEST_F(TxnParticipantTest, PrepareFailsOnTemporaryCollection) {
    NamespaceString tempCollNss =
        NamespaceString::createNamespaceString_forTest(kNss.db_forTest(), "tempCollection");
    UUID tempCollUUID = UUID::gen();

    // Create a temporary collection so that we can write to it.
    {
        AutoGetDb autoDb(opCtx(), kNss.dbName(), MODE_X);
        auto db = autoDb.ensureDbExists(opCtx());
        ASSERT_TRUE(db);

        WriteUnitOfWork wuow(opCtx());
        CollectionOptions options;
        options.uuid = tempCollUUID;
        options.temp = true;
        db->createCollection(opCtx(), tempCollNss, options);
        wuow.commit();
    }

    // Set up a transaction on the temp collection
    auto outerScopedSession = checkOutSession();
    auto txnParticipant = TransactionParticipant::get(opCtx());

    txnParticipant.unstashTransactionResources(opCtx(), "insert");

    auto operation = repl::DurableOplogEntry::makeInsertOperation(
        tempCollNss, tempCollUUID, BSON("_id" << 0), BSON("_id" << 0));
    txnParticipant.addTransactionOperation(opCtx(), operation);

    ASSERT_THROWS_CODE(txnParticipant.prepareTransaction(opCtx(), {}),
                       AssertionException,
                       ErrorCodes::OperationNotSupportedInTransaction);
}

TEST_F(TxnParticipantTest, CommitTransactionSetsCommitTimestampOnPreparedTransaction) {
    auto sessionCheckout = checkOutSession();

    auto txnParticipant = TransactionParticipant::get(opCtx());
    txnParticipant.unstashTransactionResources(opCtx(), "commitTransaction");

    // The transaction machinery cannot store an empty locker.
    { Lock::GlobalLock lk(opCtx(), MODE_IX, Date_t::now(), Lock::InterruptBehavior::kThrow); }
    const auto prepareResponse = txnParticipant.prepareTransaction(opCtx(), {});
    const auto& prepareTimestamp = prepareResponse.first;
    const auto commitTS = Timestamp(prepareTimestamp.getSecs(), prepareTimestamp.getInc() + 1);

    auto originalFn = _opObserver->onPreparedTransactionCommitFn;
    _opObserver->onPreparedTransactionCommitFn =
        [&](OplogSlot commitOplogEntryOpTime,
            Timestamp commitTimestamp,
            const std::vector<repl::ReplOperation>& statements) {
            originalFn(commitOplogEntryOpTime, commitTimestamp, statements);

            ASSERT_GT(commitTimestamp, prepareTimestamp);

            ASSERT(statements.empty());
        };

    txnParticipant.commitPreparedTransaction(opCtx(), commitTS, {});

    // The recovery unit is reset on commit.
    ASSERT(shard_role_details::getRecoveryUnit(opCtx())->getCommitTimestamp().isNull());

    txnParticipant.stashTransactionResources(opCtx());
    ASSERT_TRUE(txnParticipant.transactionIsCommitted());
    ASSERT(shard_role_details::getRecoveryUnit(opCtx())->getCommitTimestamp().isNull());
}

TEST_F(TxnParticipantTest, CommitTransactionWithCommitTimestampFailsOnUnpreparedTransaction) {
    const auto commitTimestamp = Timestamp(6, 6);

    auto sessionCheckout = checkOutSession();
    auto txnParticipant = TransactionParticipant::get(opCtx());
    txnParticipant.unstashTransactionResources(opCtx(), "commitTransaction");

    // The transaction machinery cannot store an empty locker.
    { Lock::GlobalLock lk(opCtx(), MODE_IX, Date_t::now(), Lock::InterruptBehavior::kThrow); }
    ASSERT_THROWS_CODE(txnParticipant.commitPreparedTransaction(opCtx(), commitTimestamp, {}),
                       AssertionException,
                       ErrorCodes::InvalidOptions);
}

TEST_F(TxnParticipantTest, CommitTransactionDoesNotSetCommitTimestampOnUnpreparedTransaction) {
    auto sessionCheckout = checkOutSession();

    auto originalFn = _opObserver->onUnpreparedTransactionCommitFn;
    _opObserver->onUnpreparedTransactionCommitFn =
        [&](const std::vector<repl::ReplOperation>& statements) {
            originalFn(statements);
            ASSERT(shard_role_details::getRecoveryUnit(opCtx())->getCommitTimestamp().isNull());
            ASSERT(statements.empty());
        };

    auto txnParticipant = TransactionParticipant::get(opCtx());
    txnParticipant.unstashTransactionResources(opCtx(), "commitTransaction");

    // The transaction machinery cannot store an empty locker.
    { Lock::GlobalLock lk(opCtx(), MODE_IX, Date_t::now(), Lock::InterruptBehavior::kThrow); }
    txnParticipant.commitUnpreparedTransaction(opCtx());

    ASSERT(shard_role_details::getRecoveryUnit(opCtx())->getCommitTimestamp().isNull());

    txnParticipant.stashTransactionResources(opCtx());
    ASSERT_TRUE(txnParticipant.transactionIsCommitted());
    ASSERT(shard_role_details::getRecoveryUnit(opCtx())->getCommitTimestamp().isNull());
}

TEST_F(TxnParticipantTest, CommitTransactionWithoutCommitTimestampFailsOnPreparedTransaction) {
    auto sessionCheckout = checkOutSession();

    auto txnParticipant = TransactionParticipant::get(opCtx());
    txnParticipant.unstashTransactionResources(opCtx(), "commitTransaction");

    // The transaction machinery cannot store an empty locker.
    { Lock::GlobalLock lk(opCtx(), MODE_IX, Date_t::now(), Lock::InterruptBehavior::kThrow); }
    txnParticipant.prepareTransaction(opCtx(), {});
    ASSERT_THROWS_CODE(txnParticipant.commitUnpreparedTransaction(opCtx()),
                       AssertionException,
                       ErrorCodes::InvalidOptions);
}

TEST_F(TxnParticipantTest, CommitTransactionWithNullCommitTimestampFailsOnPreparedTransaction) {
    auto sessionCheckout = checkOutSession();
    auto txnParticipant = TransactionParticipant::get(opCtx());

    txnParticipant.unstashTransactionResources(opCtx(), "commitTransaction");

    // The transaction machinery cannot store an empty locker.
    { Lock::GlobalLock lk(opCtx(), MODE_IX, Date_t::now(), Lock::InterruptBehavior::kThrow); }
    txnParticipant.prepareTransaction(opCtx(), {});
    ASSERT_THROWS_CODE(txnParticipant.commitPreparedTransaction(opCtx(), Timestamp(), {}),
                       AssertionException,
                       ErrorCodes::InvalidOptions);
}

TEST_F(TxnParticipantTest,
       CommitTransactionWithCommitTimestampLessThanPrepareTimestampFailsOnPreparedTransaction) {
    auto sessionCheckout = checkOutSession();
    auto txnParticipant = TransactionParticipant::get(opCtx());

    txnParticipant.unstashTransactionResources(opCtx(), "commitTransaction");

    // The transaction machinery cannot store an empty locker.
    { Lock::GlobalLock lk(opCtx(), MODE_IX, Date_t::now(), Lock::InterruptBehavior::kThrow); }
    const auto prepareResponse = txnParticipant.prepareTransaction(opCtx(), {});
    const auto& prepareTimestamp = prepareResponse.first;
    ASSERT_THROWS_CODE(txnParticipant.commitPreparedTransaction(
                           opCtx(), Timestamp(prepareTimestamp.getSecs() - 1, 1), {}),
                       AssertionException,
                       ErrorCodes::InvalidOptions);
}

// This test makes sure the abort machinery works even when no operations are done on the
// transaction.
TEST_F(TxnParticipantTest, EmptyTransactionAbort) {
    auto sessionCheckout = checkOutSession();
    auto txnParticipant = TransactionParticipant::get(opCtx());
    txnParticipant.unstashTransactionResources(opCtx(), "abortTransaction");

    // The transaction machinery cannot store an empty locker.
    { Lock::GlobalLock lk(opCtx(), MODE_IX, Date_t::now(), Lock::InterruptBehavior::kThrow); }
    txnParticipant.stashTransactionResources(opCtx());
    txnParticipant.abortTransaction(opCtx());
    ASSERT_TRUE(txnParticipant.transactionIsAborted());
}

// This test makes sure the abort machinery works even when no operations are done on the
// transaction.
TEST_F(TxnParticipantTest, EmptyPreparedTransactionAbort) {
    auto sessionCheckout = checkOutSession();
    auto txnParticipant = TransactionParticipant::get(opCtx());
    txnParticipant.unstashTransactionResources(opCtx(), "abortTransaction");

    // The transaction machinery cannot store an empty locker.
    { Lock::GlobalLock lk(opCtx(), MODE_IX, Date_t::now(), Lock::InterruptBehavior::kThrow); }
    txnParticipant.prepareTransaction(opCtx(), {});
    txnParticipant.abortTransaction(opCtx());
    ASSERT_TRUE(txnParticipant.transactionIsAborted());
}

TEST_F(TxnParticipantTest, KillOpBeforeCommittingPreparedTransaction) {
    auto sessionCheckout = checkOutSession();
    auto txnParticipant = TransactionParticipant::get(opCtx());

    // Prepare the transaction.
    txnParticipant.unstashTransactionResources(opCtx(), "prepareTransaction");
    const auto prepareResponse = txnParticipant.prepareTransaction(opCtx(), {});
    const auto& prepareTimestamp = prepareResponse.first;
    opCtx()->markKilled(ErrorCodes::Interrupted);
    try {
        // The commit should throw, since the operation was killed.
        txnParticipant.commitPreparedTransaction(opCtx(), prepareTimestamp, boost::none);
    } catch (const DBException& ex) {
        ASSERT_EQ(ErrorCodes::Interrupted, ex.code());
    }

    // Check the session back in.
    txnParticipant.stashTransactionResources(opCtx());
    sessionCheckout->checkIn(opCtx(), OperationContextSession::CheckInReason::kDone);

    // The transaction state should have been unaffected.
    ASSERT_TRUE(txnParticipant.transactionIsPrepared());

    auto commitPreparedFunc = [&](OperationContext* opCtx) {
        opCtx->setLogicalSessionId(_sessionId);
        opCtx->setTxnNumber(_txnNumber);
        opCtx->setInMultiDocumentTransaction();

        // Check out the session and continue the transaction.
        auto mongoDSessionCatalog = MongoDSessionCatalog::get(opCtx);
        auto opCtxSession = mongoDSessionCatalog->checkOutSession(opCtx);
        auto newTxnParticipant = TransactionParticipant::get(opCtx);
        newTxnParticipant.beginOrContinue(opCtx,
                                          {*(opCtx->getTxnNumber())},
                                          false /* autocommit */,
                                          TransactionParticipant::TransactionActions::kContinue);

        newTxnParticipant.unstashTransactionResources(opCtx, "commitTransaction");
        newTxnParticipant.commitPreparedTransaction(opCtx, prepareTimestamp, boost::none);
    };

    // Now try to commit the transaction again, with a fresh operation context.
    runFunctionFromDifferentOpCtx(commitPreparedFunc);
    ASSERT_TRUE(txnParticipant.transactionIsCommitted());
}

TEST_F(TxnParticipantTest, KillOpBeforeAbortingPreparedTransaction) {
    auto sessionCheckout = checkOutSession();
    auto txnParticipant = TransactionParticipant::get(opCtx());

    // Prepare the transaction.
    txnParticipant.unstashTransactionResources(opCtx(), "prepareTransaction");
    const auto prepareResponse = txnParticipant.prepareTransaction(opCtx(), {});
    const auto& prepareTimestamp = prepareResponse.first;
    opCtx()->markKilled(ErrorCodes::Interrupted);
    try {
        // The abort should throw, since the operation was killed.
        txnParticipant.abortTransaction(opCtx());
    } catch (const DBException& ex) {
        ASSERT_EQ(ErrorCodes::Interrupted, ex.code());
    }

    // Check the session back in.
    txnParticipant.stashTransactionResources(opCtx());
    sessionCheckout->checkIn(opCtx(), OperationContextSession::CheckInReason::kDone);

    // The transaction state should have been unaffected.
    ASSERT_TRUE(txnParticipant.transactionIsPrepared());

    auto commitPreparedFunc = [&](OperationContext* opCtx) {
        opCtx->setLogicalSessionId(_sessionId);
        opCtx->setTxnNumber(_txnNumber);
        opCtx->setInMultiDocumentTransaction();

        // Check out the session and continue the transaction.
        auto mongoDSessionCatalog = MongoDSessionCatalog::get(opCtx);
        auto opCtxSession = mongoDSessionCatalog->checkOutSession(opCtx);
        auto newTxnParticipant = TransactionParticipant::get(opCtx);
        newTxnParticipant.beginOrContinue(opCtx,
                                          {*(opCtx->getTxnNumber())},
                                          false /* autocommit */,
                                          TransactionParticipant::TransactionActions::kContinue);

        newTxnParticipant.unstashTransactionResources(opCtx, "commitTransaction");
        newTxnParticipant.commitPreparedTransaction(opCtx, prepareTimestamp, boost::none);
    };

    // Now try to commit the transaction again, with a fresh operation context.
    runFunctionFromDifferentOpCtx(commitPreparedFunc);
    ASSERT_TRUE(txnParticipant.transactionIsCommitted());
}
TEST_F(TxnParticipantTest, StashedRollbackDoesntHoldClientLock) {
    auto sessionCheckout = checkOutSession();
    auto txnParticipant = TransactionParticipant::get(opCtx());
    txnParticipant.unstashTransactionResources(opCtx(), "prepareTransaction");

    txnParticipant.prepareTransaction(opCtx(), {});

    unittest::Barrier startedRollback(2);
    unittest::Barrier finishRollback(2);

    // Rollback changes are executed in reverse order.
    shard_role_details::getRecoveryUnit(opCtx())->onRollback(
        [&](OperationContext*) { finishRollback.countDownAndWait(); });
    shard_role_details::getRecoveryUnit(opCtx())->onRollback(
        [&](OperationContext*) { startedRollback.countDownAndWait(); });

    auto future = stdx::async(stdx::launch::async, [&] {
        startedRollback.countDownAndWait();

        // Verify we can take the Client lock during the rollback of the stashed transaction.
        stdx::lock_guard<Client> lk(*opCtx()->getClient());

        finishRollback.countDownAndWait();
    });

    txnParticipant.stashTransactionResources(opCtx());
    txnParticipant.abortTransaction(opCtx());

    future.get();
}

TEST_F(TxnParticipantTest, ThrowDuringOnTransactionPrepareAbortsTransaction) {
    auto sessionCheckout = checkOutSession();
    auto txnParticipant = TransactionParticipant::get(opCtx());

    txnParticipant.unstashTransactionResources(opCtx(), "prepareTransaction");

    _opObserver->postTransactionPrepareThrowsException = true;

    ASSERT_THROWS_CODE(txnParticipant.prepareTransaction(opCtx(), {}),
                       AssertionException,
                       ErrorCodes::OperationFailed);
    ASSERT_FALSE(_opObserver->transactionPrepared);
    ASSERT(txnParticipant.transactionIsAborted());
}

TEST_F(TxnParticipantTest, UnstashFailsShouldLeaveTxnResourceStashUnchanged) {
    auto sessionCheckout = checkOutSession();
    auto txnParticipant = TransactionParticipant::get(opCtx());

    txnParticipant.unstashTransactionResources(opCtx(), "prepareTransaction");
    ASSERT_TRUE(shard_role_details::getLocker(opCtx())->isLocked());

    // Simulate the locking of an insert.
    {
        Lock::DBLock dbLock(
            opCtx(), DatabaseName::createDatabaseName_forTest(boost::none, "test"), MODE_IX);
        Lock::CollectionLock collLock(
            opCtx(), NamespaceString::createNamespaceString_forTest("test.foo"), MODE_IX);
    }

    auto [prepareTimestamp, namespaces] = txnParticipant.prepareTransaction(opCtx(), {});

    // Simulate a secondary style lock stashing such that the locks are yielded.
    {
        repl::UnreplicatedWritesBlock uwb(opCtx());
        shard_role_details::getLocker(opCtx())->unsetMaxLockTimeout();
        txnParticipant.stashTransactionResources(opCtx());
    }
    ASSERT_FALSE(txnParticipant.getTxnResourceStashLockerForTest()->isLocked());

    // Enable fail point.
    globalFailPointRegistry().find("restoreLocksFail")->setMode(FailPoint::alwaysOn);

    ASSERT_THROWS_CODE(txnParticipant.unstashTransactionResources(opCtx(), "commitTransaction"),
                       AssertionException,
                       ErrorCodes::LockTimeout);

    // Above unstash attempt fail should leave the txnResourceStash unchanged.
    ASSERT_FALSE(txnParticipant.getTxnResourceStashLockerForTest()->isLocked());

    // Disable fail point.
    globalFailPointRegistry().find("restoreLocksFail")->setMode(FailPoint::off);

    // Should be successfully able to perform lock restore.
    txnParticipant.unstashTransactionResources(opCtx(), "commitTransaction");
    ASSERT_TRUE(shard_role_details::getLocker(opCtx())->isLocked());

    // Commit the transaction to release the locks.
    txnParticipant.commitPreparedTransaction(opCtx(), prepareTimestamp, boost::none);
    ASSERT_TRUE(txnParticipant.transactionIsCommitted());
}

TEST_F(TxnParticipantTest, StepDownAfterPrepareDoesNotBlock) {
    auto sessionCheckout = checkOutSession();
    auto txnParticipant = TransactionParticipant::get(opCtx());
    txnParticipant.unstashTransactionResources(opCtx(), "prepareTransaction");

    txnParticipant.prepareTransaction(opCtx(), {});

    // Test that we can acquire the RSTL in mode X, and then immediately release it so the test can
    // complete successfully.
    auto func = [&](OperationContext* opCtx) {
        shard_role_details::getLocker(opCtx)->lock(
            opCtx, resourceIdReplicationStateTransitionLock, MODE_X);
        shard_role_details::getLocker(opCtx)->unlock(resourceIdReplicationStateTransitionLock);
    };
    runFunctionFromDifferentOpCtx(func);

    txnParticipant.abortTransaction(opCtx());
    ASSERT(_opObserver->transactionAborted);
    ASSERT(txnParticipant.transactionIsAborted());
}

TEST_F(TxnParticipantTest, StepDownAfterPrepareDoesNotBlockThenCommit) {
    auto sessionCheckout = checkOutSession();
    auto txnParticipant = TransactionParticipant::get(opCtx());
    txnParticipant.unstashTransactionResources(opCtx(), "prepareTransaction");

    const auto [prepareTimestamp, namespaces] = txnParticipant.prepareTransaction(opCtx(), {});

    // Test that we can acquire the RSTL in mode X, and then immediately release it so the test can
    // complete successfully.
    auto func = [&](OperationContext* opCtx) {
        shard_role_details::getLocker(opCtx)->lock(
            opCtx, resourceIdReplicationStateTransitionLock, MODE_X);
        shard_role_details::getLocker(opCtx)->unlock(resourceIdReplicationStateTransitionLock);
    };
    runFunctionFromDifferentOpCtx(func);

    txnParticipant.commitPreparedTransaction(opCtx(), prepareTimestamp, {});
    ASSERT(_opObserver->preparedTransactionCommitted);
    ASSERT(txnParticipant.transactionIsCommitted());
}

TEST_F(TxnParticipantTest, StepDownDuringAbortSucceeds) {
    auto sessionCheckout = checkOutSession();
    auto txnParticipant = TransactionParticipant::get(opCtx());
    txnParticipant.unstashTransactionResources(opCtx(), "abortTransaction");

    ASSERT_OK(repl::ReplicationCoordinator::get(opCtx())->setFollowerMode(
        repl::MemberState::RS_SECONDARY));
    txnParticipant.abortTransaction(opCtx());
    ASSERT(_opObserver->transactionAborted);
    ASSERT(txnParticipant.transactionIsAborted());
}


TEST_F(TxnParticipantTest, CleanOperationContextOnStepUp) {
    // Insert an in-progress transaction document.
    insertTxnRecord(opCtx(), 1, DurableTxnStateEnum::kInProgress);

    const auto service = opCtx()->getServiceContext();
    // onStepUp() relies on the storage interface to create the config.transactions table.
    repl::StorageInterface::set(service, std::make_unique<repl::StorageInterfaceImpl>());

    // The test fixture set up sets an LSID on this opCtx, which we do not want here.
    auto onStepUpFunc = [&](OperationContext* opCtx) {
        // onStepUp() must not leave aborted transactions' metadata attached to the operation
        // context.
        auto mongoDSessionCatalog = MongoDSessionCatalog::get(opCtx);
        mongoDSessionCatalog->onStepUp(opCtx);

        // onStepUp() must not leave aborted transactions' metadata attached to the operation
        // context.
        ASSERT_FALSE(opCtx->inMultiDocumentTransaction());
        ASSERT_FALSE(opCtx->isStartingMultiDocumentTransaction());
        ASSERT_FALSE(opCtx->isActiveTransactionParticipant());
        ASSERT_FALSE(opCtx->getLogicalSessionId());
        ASSERT_FALSE(opCtx->getTxnNumber());
    };

    runFunctionFromDifferentOpCtx(onStepUpFunc);
}

TEST_F(TxnParticipantTest, StepDownDuringPreparedAbortFails) {
    auto sessionCheckout = checkOutSession();
    auto txnParticipant = TransactionParticipant::get(opCtx());
    txnParticipant.unstashTransactionResources(opCtx(), "prepareTransaction");

    txnParticipant.prepareTransaction(opCtx(), {});

    ASSERT_OK(repl::ReplicationCoordinator::get(opCtx())->setFollowerMode(
        repl::MemberState::RS_SECONDARY));
    ASSERT_THROWS_CODE(txnParticipant.abortTransaction(opCtx()),
                       AssertionException,
                       ErrorCodes::NotWritablePrimary);
}

TEST_F(TxnParticipantTest, StepDownDuringPreparedCommitFails) {
    auto sessionCheckout = checkOutSession();
    auto txnParticipant = TransactionParticipant::get(opCtx());
    txnParticipant.unstashTransactionResources(opCtx(), "prepareTransaction");

    const auto [prepareTimestamp, namespaces] = txnParticipant.prepareTransaction(opCtx(), {});
    const auto commitTS = Timestamp(prepareTimestamp.getSecs(), prepareTimestamp.getInc() + 1);

    ASSERT_OK(repl::ReplicationCoordinator::get(opCtx())->setFollowerMode(
        repl::MemberState::RS_SECONDARY));
    ASSERT_THROWS_CODE(txnParticipant.commitPreparedTransaction(opCtx(), commitTS, {}),
                       AssertionException,
                       ErrorCodes::NotWritablePrimary);
}

TEST_F(TxnParticipantTest, StepDownDuringPreparedAbortReleasesRSTL) {
    auto sessionCheckout = checkOutSession();
    auto txnParticipant = TransactionParticipant::get(opCtx());

    ASSERT_EQ(shard_role_details::getLocker(opCtx())->getLockMode(
                  resourceIdReplicationStateTransitionLock),
              MODE_NONE);

    txnParticipant.unstashTransactionResources(opCtx(), "insert");
    ASSERT_EQ(shard_role_details::getLocker(opCtx())->getLockMode(
                  resourceIdReplicationStateTransitionLock),
              MODE_IX);

    // Simulate the locking of an insert.
    {
        Lock::DBLock dbLock(
            opCtx(), DatabaseName::createDatabaseName_forTest(boost::none, "test"), MODE_IX);
        Lock::CollectionLock collLock(
            opCtx(), NamespaceString::createNamespaceString_forTest("test.foo"), MODE_IX);
    }

    ASSERT_EQ(shard_role_details::getLocker(opCtx())->getLockMode(
                  resourceIdReplicationStateTransitionLock),
              MODE_IX);
    txnParticipant.stashTransactionResources(opCtx());

    ASSERT_EQ(shard_role_details::getLocker(opCtx())->getLockMode(
                  resourceIdReplicationStateTransitionLock),
              MODE_NONE);

    txnParticipant.unstashTransactionResources(opCtx(), "prepareTransaction");
    ASSERT_EQ(shard_role_details::getLocker(opCtx())->getLockMode(
                  resourceIdReplicationStateTransitionLock),
              MODE_IX);
    txnParticipant.prepareTransaction(opCtx(), {});
    ASSERT_EQ(shard_role_details::getLocker(opCtx())->getLockMode(
                  resourceIdReplicationStateTransitionLock),
              MODE_NONE);
    txnParticipant.stashTransactionResources(opCtx());

    ASSERT_EQ(shard_role_details::getLocker(opCtx())->getLockMode(
                  resourceIdReplicationStateTransitionLock),
              MODE_NONE);

    txnParticipant.unstashTransactionResources(opCtx(), "abortTransaction");
    ASSERT_EQ(shard_role_details::getLocker(opCtx())->getLockMode(
                  resourceIdReplicationStateTransitionLock),
              MODE_NONE);
    ASSERT_OK(repl::ReplicationCoordinator::get(opCtx())->setFollowerMode(
        repl::MemberState::RS_SECONDARY));
    ASSERT_THROWS_CODE(txnParticipant.abortTransaction(opCtx()),
                       AssertionException,
                       ErrorCodes::NotWritablePrimary);

    ASSERT_EQ(shard_role_details::getLocker(opCtx())->getLockMode(
                  resourceIdReplicationStateTransitionLock),
              MODE_NONE);

    // Test that we can acquire the RSTL in mode X, and then immediately release it so the test can
    // complete successfully.
    auto func = [&](OperationContext* newOpCtx) {
        shard_role_details::getLocker(newOpCtx)->lock(
            newOpCtx, resourceIdReplicationStateTransitionLock, MODE_X);
        shard_role_details::getLocker(newOpCtx)->unlock(resourceIdReplicationStateTransitionLock);
    };
    runFunctionFromDifferentOpCtx(func);
}

TEST_F(TxnParticipantTest, StepDownDuringPreparedCommitReleasesRSTL) {
    auto sessionCheckout = checkOutSession();
    auto txnParticipant = TransactionParticipant::get(opCtx());

    ASSERT_EQ(shard_role_details::getLocker(opCtx())->getLockMode(
                  resourceIdReplicationStateTransitionLock),
              MODE_NONE);

    txnParticipant.unstashTransactionResources(opCtx(), "insert");
    ASSERT_EQ(shard_role_details::getLocker(opCtx())->getLockMode(
                  resourceIdReplicationStateTransitionLock),
              MODE_IX);

    // Simulate the locking of an insert.
    {
        Lock::DBLock dbLock(
            opCtx(), DatabaseName::createDatabaseName_forTest(boost::none, "test"), MODE_IX);
        Lock::CollectionLock collLock(
            opCtx(), NamespaceString::createNamespaceString_forTest("test.foo"), MODE_IX);
    }

    ASSERT_EQ(shard_role_details::getLocker(opCtx())->getLockMode(
                  resourceIdReplicationStateTransitionLock),
              MODE_IX);
    txnParticipant.stashTransactionResources(opCtx());

    ASSERT_EQ(shard_role_details::getLocker(opCtx())->getLockMode(
                  resourceIdReplicationStateTransitionLock),
              MODE_NONE);

    txnParticipant.unstashTransactionResources(opCtx(), "prepareTransaction");
    ASSERT_EQ(shard_role_details::getLocker(opCtx())->getLockMode(
                  resourceIdReplicationStateTransitionLock),
              MODE_IX);
    const auto prepareResponse = txnParticipant.prepareTransaction(opCtx(), {});
    const auto& prepareTimestamp = prepareResponse.first;
    ASSERT_EQ(shard_role_details::getLocker(opCtx())->getLockMode(
                  resourceIdReplicationStateTransitionLock),
              MODE_NONE);
    txnParticipant.stashTransactionResources(opCtx());

    ASSERT_EQ(shard_role_details::getLocker(opCtx())->getLockMode(
                  resourceIdReplicationStateTransitionLock),
              MODE_NONE);

    txnParticipant.unstashTransactionResources(opCtx(), "commitTransaction");
    ASSERT_EQ(shard_role_details::getLocker(opCtx())->getLockMode(
                  resourceIdReplicationStateTransitionLock),
              MODE_NONE);
    ASSERT_OK(repl::ReplicationCoordinator::get(opCtx())->setFollowerMode(
        repl::MemberState::RS_SECONDARY));
    ASSERT_THROWS_CODE(
        txnParticipant.commitPreparedTransaction(opCtx(), prepareTimestamp, boost::none),
        AssertionException,
        ErrorCodes::NotWritablePrimary);

    ASSERT_EQ(shard_role_details::getLocker(opCtx())->getLockMode(
                  resourceIdReplicationStateTransitionLock),
              MODE_NONE);

    // Test that we can acquire the RSTL in mode X, and then immediately release it so the test can
    // complete successfully.
    auto func = [&](OperationContext* newOpCtx) {
        shard_role_details::getLocker(newOpCtx)->lock(
            newOpCtx, resourceIdReplicationStateTransitionLock, MODE_X);
        shard_role_details::getLocker(newOpCtx)->unlock(resourceIdReplicationStateTransitionLock);
    };
    runFunctionFromDifferentOpCtx(func);
}

TEST_F(TxnParticipantTest, ThrowDuringUnpreparedCommitLetsTheAbortAtEntryPointToCleanUp) {
    auto sessionCheckout = checkOutSession();
    auto txnParticipant = TransactionParticipant::get(opCtx());
    txnParticipant.unstashTransactionResources(opCtx(), "commitTransaction");

    _opObserver->onUnpreparedTransactionCommitThrowsException = true;

    ASSERT_THROWS_CODE(txnParticipant.commitUnpreparedTransaction(opCtx()),
                       AssertionException,
                       ErrorCodes::OperationFailed);
    ASSERT_FALSE(_opObserver->unpreparedTransactionCommitted);
    ASSERT_FALSE(txnParticipant.transactionIsAborted());
    ASSERT_FALSE(txnParticipant.transactionIsCommitted());

    // Simulate the abort at entry point.
    txnParticipant.abortTransaction(opCtx());
    ASSERT_TRUE(txnParticipant.transactionIsAborted());
}

TEST_F(TxnParticipantTest, ContinuingATransactionWithNoResourcesAborts) {
    // Check out a session, start the transaction and check it in.
    checkOutSession();

    // Check out the session again for a new operation.
    auto mongoDSessionCatalog = MongoDSessionCatalog::get(opCtx());
    auto sessionCheckout = mongoDSessionCatalog->checkOutSession(opCtx());
    auto txnParticipant = TransactionParticipant::get(opCtx());

    ASSERT_THROWS_CODE(
        txnParticipant.beginOrContinue(opCtx(),
                                       {*opCtx()->getTxnNumber()},
                                       false /* autocommit */,
                                       TransactionParticipant::TransactionActions::kContinue),
        AssertionException,
        ErrorCodes::NoSuchTransaction);
}

TEST_F(TxnParticipantTest, CannotStartNewTransactionIfNotPrimary) {
    auto mongoDSessionCatalog = MongoDSessionCatalog::get(opCtx());
    auto opCtxSession = mongoDSessionCatalog->checkOutSession(opCtx());
    auto txnParticipant = TransactionParticipant::get(opCtx());

    ASSERT_OK(repl::ReplicationCoordinator::get(opCtx())->setFollowerMode(
        repl::MemberState::RS_SECONDARY));

    // Include 'autocommit=false' for transactions.
    ASSERT_THROWS_CODE(
        txnParticipant.beginOrContinue(opCtx(),
                                       {*opCtx()->getTxnNumber()},
                                       false /* autocommit */,
                                       TransactionParticipant::TransactionActions::kStart),
        AssertionException,
        ErrorCodes::NotWritablePrimary);
}

TEST_F(TxnParticipantTest, CannotStartRetryableWriteIfNotPrimary) {
    auto mongoDSessionCatalog = MongoDSessionCatalog::get(opCtx());
    auto opCtxSession = mongoDSessionCatalog->checkOutSession(opCtx());
    auto txnParticipant = TransactionParticipant::get(opCtx());

    ASSERT_OK(repl::ReplicationCoordinator::get(opCtx())->setFollowerMode(
        repl::MemberState::RS_SECONDARY));

    // Omit the 'autocommit' field for retryable writes.
    ASSERT_THROWS_CODE(
        txnParticipant.beginOrContinue(opCtx(),
                                       {*opCtx()->getTxnNumber()},
                                       boost::none /* autocommit */,
                                       TransactionParticipant::TransactionActions::kStart),
        AssertionException,
        ErrorCodes::NotWritablePrimary);
}

TEST_F(TxnParticipantTest, CannotContinueTransactionIfNotPrimary) {
    // Will start the transaction.
    auto sessionCheckout = checkOutSession();
    auto txnParticipant = TransactionParticipant::get(opCtx());
    ASSERT_TRUE(txnParticipant.transactionIsOpen());

    ASSERT_OK(repl::ReplicationCoordinator::get(opCtx())->setFollowerMode(
        repl::MemberState::RS_SECONDARY));

    // Technically, the transaction should have been aborted on stepdown anyway, but it
    // doesn't hurt to have this kind of coverage.
    ASSERT_THROWS_CODE(
        txnParticipant.beginOrContinue(opCtx(),
                                       {*opCtx()->getTxnNumber()},
                                       false /* autocommit */,
                                       TransactionParticipant::TransactionActions::kContinue),
        AssertionException,
        ErrorCodes::NotWritablePrimary);
}

TEST_F(TxnParticipantTest, OlderTransactionFailsOnSessionWithNewerTransaction) {
    // Will start the transaction.
    auto sessionCheckout = checkOutSession();
    auto txnParticipant = TransactionParticipant::get(opCtx());
    ASSERT_TRUE(txnParticipant.transactionIsOpen());
    auto autocommit = false;
    auto startTransaction = TransactionParticipant::TransactionActions::kStart;
    const auto& sessionId = *opCtx()->getLogicalSessionId();

    StringBuilder sb;
    sb << "Cannot start transaction with { txnNumber: 19 } on session " << sessionId
       << " because a newer transaction with txnNumberAndRetryCounter { txnNumber: 20, "
          "txnRetryCounter: 0 } has already started on this session.";
    ASSERT_THROWS_WHAT(txnParticipant.beginOrContinue(
                           opCtx(), {*opCtx()->getTxnNumber() - 1}, autocommit, startTransaction),
                       AssertionException,
                       sb.str());
    ASSERT(txnParticipant.getLastWriteOpTime().isNull());
}


TEST_F(TxnParticipantTest, OldRetryableWriteFailsOnSessionWithNewerTransaction) {
    // Will start the transaction.
    auto sessionCheckout = checkOutSession();
    auto txnParticipant = TransactionParticipant::get(opCtx());
    ASSERT_TRUE(txnParticipant.transactionIsOpen());
    const auto& sessionId = *opCtx()->getLogicalSessionId();

    StringBuilder sb;
    sb << "Retryable write with txnNumber 19 is prohibited on session " << sessionId
       << " because a newer transaction with txnNumber 20 has already started on this session.";
    ASSERT_THROWS_WHAT(
        txnParticipant.beginOrContinue(opCtx(),
                                       {*opCtx()->getTxnNumber() - 1},
                                       boost::none /* autocommit */,
                                       TransactionParticipant::TransactionActions::kNone),
        AssertionException,
        sb.str());
    ASSERT(txnParticipant.getLastWriteOpTime().isNull());
}

TEST_F(TxnParticipantTest, CannotStartNewTransactionWhilePreparedTransactionInProgress) {
    auto sessionCheckout = checkOutSession();
    auto txnParticipant = TransactionParticipant::get(opCtx());

    txnParticipant.unstashTransactionResources(opCtx(), "insert");

    auto ruPrepareTimestamp = Timestamp();
    auto originalFn = _opObserver->postTransactionPrepareFn;
    _opObserver->postTransactionPrepareFn = [&]() {
        originalFn();

        ruPrepareTimestamp = shard_role_details::getRecoveryUnit(opCtx())->getPrepareTimestamp();
        ASSERT_FALSE(ruPrepareTimestamp.isNull());
    };

    // Check that prepareTimestamp gets set.
    auto [prepareTimestamp, namespaces] = txnParticipant.prepareTransaction(opCtx(), {});
    ASSERT_EQ(ruPrepareTimestamp, prepareTimestamp);

    txnParticipant.stashTransactionResources(opCtx());
    OperationContextSession::checkIn(opCtx(), OperationContextSession::CheckInReason::kDone);
    {
        ScopeGuard guard([&]() { OperationContextSession::checkOut(opCtx()); });
        // Try to start a new transaction while there is already a prepared transaction on the
        // session. This should fail with a PreparedTransactionInProgress error.
        runFunctionFromDifferentOpCtx([lsid = *opCtx()->getLogicalSessionId(),
                                       txnNumberToStart = *opCtx()->getTxnNumber() +
                                           1](OperationContext* newOpCtx) {
            newOpCtx->setLogicalSessionId(lsid);
            newOpCtx->setTxnNumber(txnNumberToStart);
            newOpCtx->setInMultiDocumentTransaction();

            auto mongoDSessionCatalog = MongoDSessionCatalog::get(newOpCtx);
            auto ocs = mongoDSessionCatalog->checkOutSession(newOpCtx);
            auto txnParticipant = TransactionParticipant::get(newOpCtx);
            ASSERT_THROWS_CODE(
                txnParticipant.beginOrContinue(newOpCtx,
                                               {txnNumberToStart},
                                               false /* autocommit */,
                                               TransactionParticipant::TransactionActions::kStart),
                AssertionException,
                ErrorCodes::PreparedTransactionInProgress);
        });
    }

    ASSERT_FALSE(txnParticipant.transactionIsAborted());
    ASSERT(_opObserver->transactionPrepared);
}

TEST_F(TxnParticipantTest, CannotInsertInPreparedTransaction) {
    auto outerScopedSession = checkOutSession();
    auto txnParticipant = TransactionParticipant::get(opCtx());

    txnParticipant.unstashTransactionResources(opCtx(), "insert");
    auto operation = repl::DurableOplogEntry::makeInsertOperation(
        kNss, _uuid, BSON("_id" << 0), BSON("_id" << 0));
    txnParticipant.addTransactionOperation(opCtx(), operation);

    txnParticipant.prepareTransaction(opCtx(), {});

    ASSERT_THROWS_CODE(txnParticipant.unstashTransactionResources(opCtx(), "insert"),
                       AssertionException,
                       ErrorCodes::PreparedTransactionInProgress);

    ASSERT_FALSE(txnParticipant.transactionIsAborted());
    ASSERT(_opObserver->transactionPrepared);
}

TEST_F(TxnParticipantTest, CannotContinueNonExistentTransaction) {
    auto mongoDSessionCatalog = MongoDSessionCatalog::get(opCtx());
    auto opCtxSession = mongoDSessionCatalog->checkOutSession(opCtx());
    auto txnParticipant = TransactionParticipant::get(opCtx());
    ASSERT_THROWS_CODE(
        txnParticipant.beginOrContinue(opCtx(),
                                       {*opCtx()->getTxnNumber()},
                                       false /* autocommit */,
                                       TransactionParticipant::TransactionActions::kContinue),
        AssertionException,
        ErrorCodes::NoSuchTransaction);
}

// Tests that a transaction aborts if it becomes too large based on the server parameter
// 'transactionLimitBytes'.
TEST_F(TxnParticipantTest, TransactionExceedsSizeParameterObjectField) {
    auto sessionCheckout = checkOutSession();
    auto txnParticipant = TransactionParticipant::get(opCtx());

    txnParticipant.unstashTransactionResources(opCtx(), "insert");
    auto oldLimit = gTransactionSizeLimitBytes.load();
    ON_BLOCK_EXIT([oldLimit] { gTransactionSizeLimitBytes.store(oldLimit); });

    // Set a limit of 2.5 MB
    gTransactionSizeLimitBytes.store(2 * 1024 * 1024 + 512 * 1024);

    // Two 1MB operations should succeed; three 1MB operations should fail.
    constexpr size_t kBigDataSize = 1 * 1024 * 1024;
    std::unique_ptr<uint8_t[]> bigData(new uint8_t[kBigDataSize]());
    auto operation = repl::DurableOplogEntry::makeInsertOperation(
        kNss,
        _uuid,
        BSON("_id" << 0 << "data" << BSONBinData(bigData.get(), kBigDataSize, BinDataGeneral)),
        BSON("_id" << 0));
    txnParticipant.addTransactionOperation(opCtx(), operation);
    txnParticipant.addTransactionOperation(opCtx(), operation);
    ASSERT_THROWS_CODE(txnParticipant.addTransactionOperation(opCtx(), operation),
                       AssertionException,
                       ErrorCodes::TransactionTooLarge);
}

TEST_F(TxnParticipantTest, TransactionExceedsSizeParameterStmtIdsField) {
    auto sessionCheckout = checkOutSession();
    auto txnParticipant = TransactionParticipant::get(opCtx());

    txnParticipant.unstashTransactionResources(opCtx(), "insert");
    auto oldLimit = gTransactionSizeLimitBytes.load();
    ON_BLOCK_EXIT([oldLimit] { gTransactionSizeLimitBytes.store(oldLimit); });

    // Set a limit of 2.5 MB
    gTransactionSizeLimitBytes.store(2 * 1024 * 1024 + 512 * 1024);

    // Two 1MB operations should succeed; three 1MB operations should fail.
    int stmtId = 0;
    auto makeOperation = [&] {
        std::vector<StmtId> stmtIds;
        stmtIds.resize(1024 * 1024 / sizeof(StmtId));
        std::generate(stmtIds.begin(), stmtIds.end(), [&stmtId] { return stmtId++; });
        auto operation = repl::DurableOplogEntry::makeInsertOperation(
            kNss, _uuid, BSON("_id" << 0), BSON("_id" << 0));
        operation.setInitializedStatementIds(stmtIds);
        return operation;
    };
    txnParticipant.addTransactionOperation(opCtx(), makeOperation());
    txnParticipant.addTransactionOperation(opCtx(), makeOperation());
    ASSERT_THROWS_CODE(txnParticipant.addTransactionOperation(opCtx(), makeOperation()),
                       AssertionException,
                       ErrorCodes::TransactionTooLarge);
}

TEST_F(TxnParticipantTest, StashInNestedSessionIsANoop) {
    auto outerScopedSession = checkOutSession();
    Locker* originalLocker = shard_role_details::getLocker(opCtx());
    RecoveryUnit* originalRecoveryUnit = shard_role_details::getRecoveryUnit(opCtx());
    ASSERT(originalLocker);
    ASSERT(originalRecoveryUnit);

    // Set the readConcern on the OperationContext.
    repl::ReadConcernArgs readConcernArgs;
    ASSERT_OK(
        readConcernArgs.initialize(BSON("find"
                                        << "test" << repl::ReadConcernArgs::kReadConcernFieldName
                                        << BSON(repl::ReadConcernArgs::kLevelFieldName
                                                << "snapshot"))));
    repl::ReadConcernArgs::get(opCtx()) = readConcernArgs;

    // Perform initial unstash, which sets up a WriteUnitOfWork.
    auto txnParticipant = TransactionParticipant::get(opCtx());
    txnParticipant.unstashTransactionResources(opCtx(), "find");
    ASSERT_EQUALS(originalLocker, shard_role_details::getLocker(opCtx()));
    ASSERT_EQUALS(originalRecoveryUnit, shard_role_details::getRecoveryUnit(opCtx()));
    ASSERT(shard_role_details::getWriteUnitOfWork(opCtx()));

    {
        // Make it look like we're in a DBDirectClient running a nested operation.
        DirectClientSetter inDirectClient(opCtx());
        txnParticipant.stashTransactionResources(opCtx());

        // The stash was a noop, so the locker, RecoveryUnit, and WriteUnitOfWork on the
        // OperationContext are unaffected.
        ASSERT_EQUALS(originalLocker, shard_role_details::getLocker(opCtx()));
        ASSERT_EQUALS(originalRecoveryUnit, shard_role_details::getRecoveryUnit(opCtx()));
        ASSERT(shard_role_details::getWriteUnitOfWork(opCtx()));
    }
}

TEST_F(TxnParticipantTest, CorrectlyStashAPIParameters) {
    auto sessionCheckout = checkOutSession();
    auto txnParticipant = TransactionParticipant::get(opCtx());

    auto defaultAPIParams = txnParticipant.getAPIParameters(opCtx());
    ASSERT_FALSE(defaultAPIParams.getAPIVersion().has_value());
    ASSERT_FALSE(defaultAPIParams.getAPIStrict().has_value());
    ASSERT_FALSE(defaultAPIParams.getAPIDeprecationErrors().has_value());

    txnParticipant.unstashTransactionResources(opCtx(), "insert");

    APIParameters updatedAPIParams = APIParameters();
    updatedAPIParams.setAPIVersion("2");
    updatedAPIParams.setAPIStrict(true);
    updatedAPIParams.setAPIDeprecationErrors(true);
    APIParameters::get(opCtx()) = updatedAPIParams;

    // Verify that API parameters on the opCtx were updated correctly.
    auto opCtxAPIParams = APIParameters::get(opCtx());
    ASSERT_EQ("2", *opCtxAPIParams.getAPIVersion());
    ASSERT_TRUE(*opCtxAPIParams.getAPIStrict());
    ASSERT_TRUE(*opCtxAPIParams.getAPIDeprecationErrors());

    txnParticipant.stashTransactionResources(opCtx());

    // Reset the API parameters on the opCtx to the default values.
    APIParameters::get(opCtx()) = defaultAPIParams;

    // Verify that 'getAPIParameters()' will return the stashed API parameters.
    APIParameters storedAPIParams = txnParticipant.getAPIParameters(opCtx());
    ASSERT_EQ("2", *storedAPIParams.getAPIVersion());
    ASSERT_TRUE(*storedAPIParams.getAPIStrict());
    ASSERT_TRUE(*storedAPIParams.getAPIDeprecationErrors());
}

TEST_F(TxnParticipantTest, PrepareReturnsAListOfAffectedNamespaces) {
    RAIIServerParameterControllerForTest controller("featureFlagEndOfTransactionChangeEvent", true);

    const std::vector<NamespaceString> kNamespaces = {
        NamespaceString::createNamespaceString_forTest("TestDB1", "TestColl1"),
        NamespaceString::createNamespaceString_forTest("TestDB1", "TestColl2"),
        NamespaceString::createNamespaceString_forTest("TestDB2", "TestColl1")};

    std::vector<UUID> uuids;
    uuids.reserve(kNamespaces.size());

    // Create collections
    for (const auto& nss : kNamespaces) {
        AutoGetDb autoDb(opCtx(), nss.dbName(), MODE_X);
        auto db = autoDb.ensureDbExists(opCtx());
        ASSERT_TRUE(db);

        WriteUnitOfWork wuow(opCtx());
        CollectionOptions options;
        auto collection = db->createCollection(opCtx(), nss, options);
        wuow.commit();
        uuids.push_back(collection->uuid());
    }

    auto sessionCheckout = checkOutSession();

    auto txnParticipant = TransactionParticipant::get(opCtx());
    ASSERT(txnParticipant.transactionIsOpen());

    txnParticipant.unstashTransactionResources(opCtx(), "insert");
    for (size_t collIndex = 0; collIndex < kNamespaces.size(); ++collIndex) {
        auto operation = repl::DurableOplogEntry::makeInsertOperation(
            kNamespaces[collIndex], uuids[collIndex], BSON("_id" << 0), BSON("_id" << 0));
        txnParticipant.addTransactionOperation(opCtx(), operation);
    }
    auto [timestamp, namespaces] = txnParticipant.prepareTransaction(opCtx(), {});
    ASSERT_EQ(namespaces, txnParticipant.affectedNamespaces());

    std::vector<NamespaceString> namespacesVec;
    std::move(namespaces.begin(), namespaces.end(), std::back_inserter(namespacesVec));
    std::sort(namespacesVec.begin(), namespacesVec.end());
    ASSERT_EQ(namespacesVec, kNamespaces);
}

/**
 * Test fixture for testing behavior that depends on a server's cluster role.
 *
 * Each test case relies on the txnNumber on the operation context, which cannot be changed, so
 * define tests for behavior shared by config and shard servers as methods here and call them in the
 * fixtures for config and shard servers defined below.
 */
class ShardedClusterParticipantTest : public TxnParticipantTest {
protected:
    void cannotSpecifyStartTransactionOnInProgressTxn() {
        auto autocommit = false;
        auto startTransaction = TransactionParticipant::TransactionActions::kStart;
        ;
        auto sessionCheckout = checkOutSession();

        auto txnParticipant = TransactionParticipant::get(opCtx());
        ASSERT(txnParticipant.transactionIsOpen());

        ASSERT_THROWS_CODE(txnParticipant.beginOrContinue(
                               opCtx(), {*opCtx()->getTxnNumber()}, autocommit, startTransaction),
                           AssertionException,
                           50911);
    }

    void canSpecifyStartTransactionOnAbortedTxn() {
        auto autocommit = false;
        auto startTransaction = TransactionParticipant::TransactionActions::kStart;

        auto sessionCheckout = checkOutSession();

        auto txnParticipant = TransactionParticipant::get(opCtx());
        ASSERT(txnParticipant.transactionIsOpen());

        txnParticipant.abortTransaction(opCtx());
        ASSERT(txnParticipant.transactionIsAborted());

        txnParticipant.beginOrContinue(
            opCtx(), {*opCtx()->getTxnNumber()}, autocommit, startTransaction);
        ASSERT(txnParticipant.transactionIsOpen());
    }

    void cannotSpecifyStartTransactionOnCommittedTxn() {
        auto autocommit = false;
        auto startTransaction = TransactionParticipant::TransactionActions::kStart;
        auto sessionCheckout = checkOutSession();

        auto txnParticipant = TransactionParticipant::get(opCtx());
        ASSERT(txnParticipant.transactionIsOpen());

        txnParticipant.unstashTransactionResources(opCtx(), "commitTransaction");
        txnParticipant.commitUnpreparedTransaction(opCtx());

        ASSERT_THROWS_CODE(txnParticipant.beginOrContinue(
                               opCtx(), {*opCtx()->getTxnNumber()}, autocommit, startTransaction),
                           AssertionException,
                           50911);
    }

    void cannotSpecifyStartTransactionOnPreparedTxn() {
        auto autocommit = false;
        auto startTransaction = TransactionParticipant::TransactionActions::kStart;
        auto sessionCheckout = checkOutSession();

        auto txnParticipant = TransactionParticipant::get(opCtx());
        ASSERT(txnParticipant.transactionIsOpen());

        txnParticipant.unstashTransactionResources(opCtx(), "insert");
        auto operation = repl::DurableOplogEntry::makeInsertOperation(
            kNss, _uuid, BSON("_id" << 0), BSON("_id" << 0));
        txnParticipant.addTransactionOperation(opCtx(), operation);
        txnParticipant.prepareTransaction(opCtx(), {});

        ASSERT_THROWS_CODE(txnParticipant.beginOrContinue(
                               opCtx(), {*opCtx()->getTxnNumber()}, autocommit, startTransaction),
                           AssertionException,
                           50911);
    }

    void cannotSpecifyStartTransactionOnAbortedPreparedTransaction() {
        auto autocommit = false;
        auto startTransaction = TransactionParticipant::TransactionActions::kStart;
        auto sessionCheckout = checkOutSession();

        auto txnParticipant = TransactionParticipant::get(opCtx());
        ASSERT(txnParticipant.transactionIsOpen());

        txnParticipant.unstashTransactionResources(opCtx(), "prepareTransaction");
        txnParticipant.prepareTransaction(opCtx(), {});
        ASSERT(txnParticipant.transactionIsPrepared());

        txnParticipant.abortTransaction(opCtx());
        ASSERT(txnParticipant.transactionIsAborted());

        startTransaction = TransactionParticipant::TransactionActions::kStart;
        ASSERT_THROWS_CODE(txnParticipant.beginOrContinue(
                               opCtx(), {*opCtx()->getTxnNumber()}, autocommit, startTransaction),
                           AssertionException,
                           50911);
    }

    void canSpecifyStartTransactionOnRetryableWriteWithNoWritesExecuted() {
        auto mongoDSessionCatalog = MongoDSessionCatalog::get(opCtx());
        auto opCtxSession = mongoDSessionCatalog->checkOutSession(opCtx());

        auto txnParticipant = TransactionParticipant::get(opCtx());
        txnParticipant.beginOrContinue(opCtx(),
                                       {*opCtx()->getTxnNumber()},
                                       boost::none /* autocommit */,
                                       TransactionParticipant::TransactionActions::kNone);
        ASSERT_FALSE(txnParticipant.transactionIsOpen());

        auto autocommit = false;
        auto startTransaction = TransactionParticipant::TransactionActions::kStart;

        txnParticipant.beginOrContinue(
            opCtx(), {*opCtx()->getTxnNumber()}, autocommit, startTransaction);

        ASSERT(txnParticipant.transactionIsOpen());
    }
};

/**
 * Test fixture for a transaction participant running on a shard server.
 */
class ShardTxnParticipantTest : public ShardedClusterParticipantTest {
protected:
    void setUp() final {
        TxnParticipantTest::setUp();
        serverGlobalParams.clusterRole = ClusterRole::ShardServer;
    }

    void tearDown() final {
        serverGlobalParams.clusterRole = ClusterRole::None;
        TxnParticipantTest::tearDown();
    }

    void runRetryableWrite(LogicalSessionId lsid, TxnNumber txnNumber) {
        runFunctionFromDifferentOpCtx([&](OperationContext* opCtx) {
            opCtx->setLogicalSessionId(lsid);
            opCtx->setTxnNumber(txnNumber);
            auto mongoDSessionCatalog = MongoDSessionCatalog::get(opCtx);
            auto opCtxSession = mongoDSessionCatalog->checkOutSession(opCtx);
            TransactionParticipant::get(opCtx).beginOrContinue(
                opCtx,
                {*opCtx->getTxnNumber()},
                boost::none,
                TransactionParticipant::TransactionActions::kNone);
        });
    }

    void runAndCommitTransaction(LogicalSessionId lsid, TxnNumber txnNumber) {
        runFunctionFromDifferentOpCtx([&](OperationContext* opCtx) {
            opCtx->setLogicalSessionId(lsid);
            opCtx->setTxnNumber(txnNumber);
            opCtx->setInMultiDocumentTransaction();
            auto mongoDSessionCatalog = MongoDSessionCatalog::get(opCtx);
            auto opCtxSession = mongoDSessionCatalog->checkOutSession(opCtx);

            auto txnParticipant = TransactionParticipant::get(opCtx);
            txnParticipant.beginOrContinue(opCtx,
                                           {*opCtx->getTxnNumber()},
                                           false,
                                           TransactionParticipant::TransactionActions::kStart);
            txnParticipant.unstashTransactionResources(opCtx, "find");
            txnParticipant.commitUnpreparedTransaction(opCtx);
        });
    }
};

TEST_F(ShardTxnParticipantTest, CannotSpecifyStartTransactionOnInProgressTxn) {
    cannotSpecifyStartTransactionOnInProgressTxn();
}

TEST_F(ShardTxnParticipantTest, CanSpecifyStartTransactionOnAbortedTxn) {
    canSpecifyStartTransactionOnAbortedTxn();
}

TEST_F(ShardTxnParticipantTest, CannotSpecifyStartTransactionOnCommittedTxn) {
    cannotSpecifyStartTransactionOnCommittedTxn();
}

TEST_F(ShardTxnParticipantTest, CannotSpecifyStartTransactionOnPreparedTxn) {
    cannotSpecifyStartTransactionOnPreparedTxn();
}

TEST_F(ShardTxnParticipantTest, canSpecifyStartTransactionOnRetryableWriteWithNoWritesExecuted) {
    canSpecifyStartTransactionOnRetryableWriteWithNoWritesExecuted();
}

TEST_F(ShardTxnParticipantTest, CannotSpecifyStartTransactionOnAbortedPreparedTransaction) {
    cannotSpecifyStartTransactionOnAbortedPreparedTransaction();
}

/**
 * Test fixture for a transaction participant running on a config server.
 */
class ConfigTxnParticipantTest : public ShardedClusterParticipantTest {
protected:
    void setUp() final {
        TxnParticipantTest::setUp();
        serverGlobalParams.clusterRole = {ClusterRole::ShardServer, ClusterRole::ConfigServer};
    }

    void tearDown() final {
        serverGlobalParams.clusterRole = ClusterRole::None;
        TxnParticipantTest::tearDown();
    }
};

TEST_F(ConfigTxnParticipantTest, CannotSpecifyStartTransactionOnInProgressTxn) {
    cannotSpecifyStartTransactionOnInProgressTxn();
}

TEST_F(ConfigTxnParticipantTest, CanSpecifyStartTransactionOnAbortedTxn) {
    canSpecifyStartTransactionOnAbortedTxn();
}

TEST_F(ConfigTxnParticipantTest, CannotSpecifyStartTransactionOnCommittedTxn) {
    cannotSpecifyStartTransactionOnCommittedTxn();
}

TEST_F(ConfigTxnParticipantTest, CannotSpecifyStartTransactionOnPreparedTxn) {
    cannotSpecifyStartTransactionOnPreparedTxn();
}

TEST_F(ConfigTxnParticipantTest, canSpecifyStartTransactionOnRetryableWriteWithNoWritesExecuted) {
    canSpecifyStartTransactionOnRetryableWriteWithNoWritesExecuted();
}

TEST_F(ConfigTxnParticipantTest, CannotSpecifyStartTransactionOnAbortedPreparedTransaction) {
    cannotSpecifyStartTransactionOnAbortedPreparedTransaction();
}

TEST_F(TxnParticipantTest, ThrowDuringUnpreparedOnTransactionAbort) {
    auto sessionCheckout = checkOutSession();
    auto txnParticipant = TransactionParticipant::get(opCtx());
    txnParticipant.unstashTransactionResources(opCtx(), "abortTransaction");

    _opObserver->onTransactionAbortThrowsException = true;

    ASSERT_THROWS_CODE(
        txnParticipant.abortTransaction(opCtx()), AssertionException, ErrorCodes::OperationFailed);
}

DEATH_TEST_F(TxnParticipantTest,
             ThrowDuringPreparedOnTransactionAbortIsFatal,
             "Caught exception during abort of transaction") {
    auto sessionCheckout = checkOutSession();
    auto txnParticipant = TransactionParticipant::get(opCtx());
    txnParticipant.unstashTransactionResources(opCtx(), "abortTransaction");
    txnParticipant.prepareTransaction(opCtx(), {});

    _opObserver->onTransactionAbortThrowsException = true;

    txnParticipant.abortTransaction(opCtx());
}

TEST_F(TxnParticipantTest, InterruptedSessionsCannotBePrepared) {
    auto sessionCheckout = checkOutSession();
    auto txnParticipant = TransactionParticipant::get(opCtx());
    txnParticipant.unstashTransactionResources(opCtx(), "prepareTransaction");

    unittest::Barrier barrier(2);

    auto future = stdx::async(stdx::launch::async, [this, &barrier] {
        ThreadClient tc(getServiceContext()->getService());
        auto sideOpCtx = tc->makeOperationContext();
        auto killToken = catalog()->killSession(_sessionId);
        barrier.countDownAndWait();

        auto scopedSession =
            catalog()->checkOutSessionForKill(sideOpCtx.get(), std::move(killToken));
    });

    barrier.countDownAndWait();

    ASSERT_THROWS_CODE(txnParticipant.prepareTransaction(opCtx(), {}),
                       AssertionException,
                       ErrorCodes::Interrupted);

    sessionCheckout.reset();
    future.get();
}

TEST_F(TxnParticipantTest, ReacquireLocksForPreparedTransactionsOnStepUp) {
    ASSERT(opCtx()->writesAreReplicated());

    // Prepare a transaction on secondary.
    {
        auto sessionCheckout = checkOutSession();
        auto txnParticipant = TransactionParticipant::get(opCtx());

        // Simulate a transaction on secondary.
        repl::UnreplicatedWritesBlock uwb(opCtx());
        ASSERT(!opCtx()->writesAreReplicated());

        txnParticipant.unstashTransactionResources(opCtx(), "prepareTransaction");
        // Simulate the locking of an insert.
        {
            Lock::DBLock dbLock(
                opCtx(), DatabaseName::createDatabaseName_forTest(boost::none, "test"), MODE_IX);
            Lock::CollectionLock collLock(
                opCtx(), NamespaceString::createNamespaceString_forTest("test.foo"), MODE_IX);
        }
        txnParticipant.prepareTransaction(opCtx(), repl::OpTime({1, 1}, 1));
        txnParticipant.stashTransactionResources(opCtx());
        // Secondary yields locks for prepared transactions.
        ASSERT_FALSE(txnParticipant.getTxnResourceStashLockerForTest()->isLocked());
    }

    // Step-up will restore the locks of prepared transactions.
    ASSERT(opCtx()->writesAreReplicated());
    const auto service = opCtx()->getServiceContext();
    // onStepUp() relies on the storage interface to create the config.transactions table.
    repl::StorageInterface::set(service, std::make_unique<repl::StorageInterfaceImpl>());
    auto mongoDSessionCatalog = MongoDSessionCatalog::get(opCtx());
    mongoDSessionCatalog->onStepUp(opCtx());
    {
        auto sessionCheckout =
            checkOutSession(TransactionParticipant::TransactionActions::kContinue);
        auto txnParticipant = TransactionParticipant::get(opCtx());
        ASSERT(txnParticipant.getTxnResourceStashLockerForTest()->isLocked());
        txnParticipant.unstashTransactionResources(opCtx(), "abortTransaction");
        txnParticipant.abortTransaction(opCtx());
    }
}

TEST_F(TxnParticipantTest, StartOrContinueTxnWithGreaterTxnNumShouldStartTxn) {
    auto sessionCheckout = checkOutSession();
    auto txnParticipant = TransactionParticipant::get(opCtx());
    ASSERT_TRUE(txnParticipant.transactionIsOpen());
    ASSERT_EQ(txnParticipant.getActiveTxnNumberAndRetryCounter().getTxnNumber(),
              *opCtx()->getTxnNumber());

    auto newTxnNum = (*opCtx()->getTxnNumber()) + 1;
    txnParticipant.beginOrContinue(opCtx(),
                                   {newTxnNum},
                                   false /* autocommit */,
                                   TransactionParticipant::TransactionActions::kStartOrContinue);
    ASSERT_TRUE(txnParticipant.transactionIsOpen());
    ASSERT_EQ(txnParticipant.getActiveTxnNumberAndRetryCounter().getTxnNumber(), newTxnNum);
}

TEST_F(TxnParticipantTest, StartOrContinueTxnWithLesserTxnNumShouldError) {
    auto sessionCheckout = checkOutSession();
    auto txnParticipant = TransactionParticipant::get(opCtx());
    ASSERT_TRUE(txnParticipant.transactionIsOpen());
    ASSERT_EQ(txnParticipant.getActiveTxnNumberAndRetryCounter().getTxnNumber(),
              *opCtx()->getTxnNumber());

    auto newTxnNum = (*opCtx()->getTxnNumber()) - 1;
    ASSERT_THROWS_CODE(txnParticipant.beginOrContinue(
                           opCtx(),
                           {newTxnNum},
                           false /* autocommit */,
                           TransactionParticipant::TransactionActions::kStartOrContinue),
                       AssertionException,
                       ErrorCodes::TransactionTooOld);
}

TEST_F(TxnParticipantTest, StartOrContinueTxnWithEqualTxnNumsShouldContinue) {
    auto sessionCheckout = checkOutSession();
    auto txnParticipant = TransactionParticipant::get(opCtx());
    ASSERT_TRUE(txnParticipant.transactionIsInProgress());
    ASSERT_EQ(txnParticipant.getActiveTxnNumberAndRetryCounter().getTxnNumber(),
              *opCtx()->getTxnNumber());
    ASSERT_EQ(txnParticipant.getActiveTxnNumberAndRetryCounter().getTxnRetryCounter(), 0);
    txnParticipant.unstashTransactionResources(opCtx(), "insert");
    txnParticipant.stashTransactionResources(opCtx());

    txnParticipant.beginOrContinue(opCtx(),
                                   {*opCtx()->getTxnNumber()},
                                   false /* autocommit */,
                                   TransactionParticipant::TransactionActions::kStartOrContinue);
    ASSERT_TRUE(txnParticipant.transactionIsInProgress());
    ASSERT_EQ(txnParticipant.getActiveTxnNumberAndRetryCounter().getTxnNumber(),
              *opCtx()->getTxnNumber());
    ASSERT_EQ(txnParticipant.getActiveTxnNumberAndRetryCounter().getTxnRetryCounter(),
              *opCtx()->getTxnRetryCounter());
}

TEST_F(ShardTxnParticipantTest,
       StartOrContinueTxnWithGreaterRetryCounterInProgressStateShouldRestart) {
    auto sessionCheckout = checkOutSession();
    auto txnParticipant = TransactionParticipant::get(opCtx());
    ASSERT(txnParticipant.transactionIsInProgress());
    ASSERT_EQ(txnParticipant.getActiveTxnNumberAndRetryCounter().getTxnRetryCounter(), 0);

    auto retryCounter = 1;
    txnParticipant.beginOrContinue(opCtx(),
                                   {*opCtx()->getTxnNumber(), retryCounter},
                                   false /* autocommit */,
                                   TransactionParticipant::TransactionActions::kStartOrContinue);
    ASSERT_TRUE(txnParticipant.transactionIsInProgress());
    ASSERT_EQ(txnParticipant.getActiveTxnNumberAndRetryCounter().getTxnNumber(),
              *opCtx()->getTxnNumber());
    ASSERT_EQ(txnParticipant.getActiveTxnNumberAndRetryCounter().getTxnRetryCounter(),
              retryCounter);
}

TEST_F(ShardTxnParticipantTest, StartOrContinueTxnWithGreaterRetryCounterPreparedStateShouldError) {
    auto sessionCheckout = checkOutSession();
    auto txnParticipant = TransactionParticipant::get(opCtx());
    ASSERT(txnParticipant.transactionIsInProgress());
    txnParticipant.unstashTransactionResources(opCtx(), "commitTransaction");
    txnParticipant.prepareTransaction(opCtx(), {});
    ASSERT(txnParticipant.transactionIsPrepared());
    ASSERT_EQ(txnParticipant.getActiveTxnNumberAndRetryCounter().getTxnRetryCounter(), 0);

    auto retryCounter = 1;
    ASSERT_THROWS_CODE(txnParticipant.beginOrContinue(
                           opCtx(),
                           {*opCtx()->getTxnNumber(), retryCounter},
                           false /* autocommit */,
                           TransactionParticipant::TransactionActions::kStartOrContinue),
                       AssertionException,
                       ErrorCodes::IllegalOperation);
}

TEST_F(ShardTxnParticipantTest, StartOrContinueTxnWithLesserRetryCounterShouldError) {
    auto sessionCheckout = checkOutSession();
    auto txnParticipant = TransactionParticipant::get(opCtx());
    ASSERT_TRUE(txnParticipant.transactionIsInProgress());

    txnParticipant.beginOrContinue(opCtx(),
                                   {*opCtx()->getTxnNumber(), 1},
                                   false /* autocommit */,
                                   TransactionParticipant::TransactionActions::kStart);
    ASSERT_EQ(txnParticipant.getActiveTxnNumberAndRetryCounter().getTxnRetryCounter(), 1);

    try {
        txnParticipant.beginOrContinue(
            opCtx(),
            {*opCtx()->getTxnNumber(), 0},
            false /* autocommit */,
            TransactionParticipant::TransactionActions::kStartOrContinue);
    } catch (const TxnRetryCounterTooOldException& ex) {
        auto info = ex.extraInfo<TxnRetryCounterTooOldInfo>();
        ASSERT_EQ(info->getTxnRetryCounter(), 1);
    }
    ASSERT_TRUE(txnParticipant.transactionIsInProgress());
    ASSERT_EQ(txnParticipant.getActiveTxnNumberAndRetryCounter().getTxnRetryCounter(), 1);
}

TEST_F(ShardTxnParticipantTest,
       StartOrContinueTxnWithEqualRetryCounterAndAbortedWithoutPrepareStateShouldRestart) {
    auto sessionCheckout = checkOutSession();
    auto txnParticipant = TransactionParticipant::get(opCtx());
    ASSERT(txnParticipant.transactionIsInProgress());
    ASSERT_EQ(txnParticipant.getActiveTxnNumberAndRetryCounter().getTxnRetryCounter(), 0);

    txnParticipant.abortTransaction(opCtx());
    ASSERT_TRUE(txnParticipant.transactionIsAbortedWithoutPrepare());

    txnParticipant.beginOrContinue(opCtx(),
                                   {*opCtx()->getTxnNumber(), 0},
                                   false /* autocommit */,
                                   TransactionParticipant::TransactionActions::kStartOrContinue);
    ASSERT_TRUE(txnParticipant.transactionIsInProgress());
    ASSERT_EQ(txnParticipant.getActiveTxnNumberAndRetryCounter().getTxnNumber(),
              *opCtx()->getTxnNumber());
    ASSERT_EQ(txnParticipant.getActiveTxnNumberAndRetryCounter().getTxnRetryCounter(), 0);
}

TEST_F(ShardTxnParticipantTest,
       StartOrContinueTxnWithEqualRetryCounterAndCommitedStateDoesNotChangeState) {
    auto sessionCheckout = checkOutSession();
    auto txnParticipant = TransactionParticipant::get(opCtx());
    ASSERT(txnParticipant.transactionIsInProgress());
    txnParticipant.unstashTransactionResources(opCtx(), "commitTransaction");
    txnParticipant.commitUnpreparedTransaction(opCtx());
    ASSERT(txnParticipant.transactionIsCommitted());
    ASSERT_EQ(txnParticipant.getActiveTxnNumberAndRetryCounter().getTxnRetryCounter(), 0);

    // The state shouldn't change at all, commitTransaction is allowed to be retried
    txnParticipant.beginOrContinue(opCtx(),
                                   {*opCtx()->getTxnNumber(), 0},
                                   false /* autocommit */,
                                   TransactionParticipant::TransactionActions::kStartOrContinue);
    ASSERT_TRUE(txnParticipant.transactionIsCommitted());
    ASSERT_EQ(txnParticipant.getActiveTxnNumberAndRetryCounter().getTxnNumber(),
              *opCtx()->getTxnNumber());
    ASSERT_EQ(txnParticipant.getActiveTxnNumberAndRetryCounter().getTxnRetryCounter(), 0);
}

TEST_F(ShardTxnParticipantTest,
       StartOrContinueTxnWithEqualRetryCounterAndInProgressStateShouldContinue) {
    auto sessionCheckout = checkOutSession();
    auto txnParticipant = TransactionParticipant::get(opCtx());
    ASSERT_TRUE(txnParticipant.transactionIsInProgress());
    ASSERT_EQ(txnParticipant.getActiveTxnNumberAndRetryCounter().getTxnRetryCounter(), 0);
    txnParticipant.unstashTransactionResources(opCtx(), "insert");
    txnParticipant.stashTransactionResources(opCtx());

    txnParticipant.beginOrContinue(opCtx(),
                                   {*opCtx()->getTxnNumber(), 0},
                                   false /* autocommit */,
                                   TransactionParticipant::TransactionActions::kStartOrContinue);
    ASSERT_TRUE(txnParticipant.transactionIsInProgress());
    ASSERT_EQ(txnParticipant.getActiveTxnNumberAndRetryCounter().getTxnNumber(),
              *opCtx()->getTxnNumber());
    ASSERT_EQ(txnParticipant.getActiveTxnNumberAndRetryCounter().getTxnRetryCounter(), 0);
}

TEST_F(ShardTxnParticipantTest,
       StartOrContinueTxnWithEqualRetryCounterAndPreparedStateShouldContinue) {
    auto sessionCheckout = checkOutSession();
    auto txnParticipant = TransactionParticipant::get(opCtx());
    ASSERT(txnParticipant.transactionIsInProgress());
    txnParticipant.unstashTransactionResources(opCtx(), "commitTransaction");
    txnParticipant.prepareTransaction(opCtx(), {});
    ASSERT(txnParticipant.transactionIsPrepared());
    ASSERT_EQ(txnParticipant.getActiveTxnNumberAndRetryCounter().getTxnRetryCounter(), 0);

    txnParticipant.beginOrContinue(opCtx(),
                                   {*opCtx()->getTxnNumber(), 0},
                                   false /* autocommit */,
                                   TransactionParticipant::TransactionActions::kStartOrContinue);
    ASSERT(txnParticipant.transactionIsPrepared());
    ASSERT_EQ(txnParticipant.getActiveTxnNumberAndRetryCounter().getTxnNumber(),
              *opCtx()->getTxnNumber());
    ASSERT_EQ(txnParticipant.getActiveTxnNumberAndRetryCounter().getTxnRetryCounter(), 0);
}

TEST_F(ShardTxnParticipantTest,
       StartOrContinueTxnWithEqualRetryCounterAndAbortedWithPrepareStateShouldContinue) {
    auto sessionCheckout = checkOutSession();
    auto txnParticipant = TransactionParticipant::get(opCtx());
    ASSERT(txnParticipant.transactionIsInProgress());
    txnParticipant.unstashTransactionResources(opCtx(), "commitTransaction");
    txnParticipant.prepareTransaction(opCtx(), {});
    ASSERT(txnParticipant.transactionIsPrepared());
    txnParticipant.abortTransaction(opCtx());
    ASSERT(txnParticipant.transactionIsAborted());
    ASSERT_EQ(txnParticipant.getActiveTxnNumberAndRetryCounter().getTxnRetryCounter(), 0);

    txnParticipant.beginOrContinue(opCtx(),
                                   {*opCtx()->getTxnNumber(), 0},
                                   false /* autocommit */,
                                   TransactionParticipant::TransactionActions::kStartOrContinue);
    ASSERT_TRUE(txnParticipant.transactionIsAborted());
    ASSERT_EQ(txnParticipant.getActiveTxnNumberAndRetryCounter().getTxnNumber(),
              *opCtx()->getTxnNumber());
    ASSERT_EQ(txnParticipant.getActiveTxnNumberAndRetryCounter().getTxnRetryCounter(), 0);
}

TEST_F(ShardTxnParticipantTest,
       StartOrContinueTxnWithUninitializedRetryCounterAndExecutedRetryableWriteStateShouldError) {
    auto mongoDSessionCatalog = MongoDSessionCatalog::get(opCtx());
    auto opCtxSession = mongoDSessionCatalog->checkOutSession(opCtx());
    auto txnParticipant = TransactionParticipant::get(opCtx());
    txnParticipant.beginOrContinue(opCtx(),
                                   {*opCtx()->getTxnNumber()},
                                   boost::none /* autocommit */,
                                   TransactionParticipant::TransactionActions::kNone);
    ASSERT(txnParticipant.transactionIsInRetryableWriteMode());
    ASSERT_EQ(txnParticipant.getActiveTxnNumberAndRetryCounter().getTxnRetryCounter(),
              kUninitializedTxnRetryCounter);

    // Execute retryable write
    Timestamp ts(1, 1);
    SessionTxnRecord sessionTxnRecord;
    sessionTxnRecord.setSessionId(*opCtx()->getLogicalSessionId());
    sessionTxnRecord.setTxnNum(*opCtx()->getTxnNumber());
    sessionTxnRecord.setLastWriteOpTime(repl::OpTime(ts, 0));
    sessionTxnRecord.setLastWriteDate(Date_t::now());
    {
        WriteUnitOfWork wuow(opCtx());
        txnParticipant.onWriteOpCompletedOnPrimary(opCtx(), {}, sessionTxnRecord);
        wuow.commit();
    }
    ASSERT(txnParticipant.transactionIsInRetryableWriteMode());

    ASSERT_THROWS_CODE(txnParticipant.beginOrContinue(
                           opCtx(),
                           {*opCtx()->getTxnNumber(), 0},
                           false /* autocommit */,
                           TransactionParticipant::TransactionActions::kStartOrContinue),
                       AssertionException,
                       ErrorCodes::NoSuchTransaction);
}

TEST_F(ShardTxnParticipantTest,
       StartOrContinueTxnWithUninitializedRetryCounterAndNoneStateShouldRestart) {
    auto mongoDSessionCatalog = MongoDSessionCatalog::get(opCtx());
    auto opCtxSession = mongoDSessionCatalog->checkOutSession(opCtx());
    auto txnParticipant = TransactionParticipant::get(opCtx());
    txnParticipant.beginOrContinue(opCtx(),
                                   {*opCtx()->getTxnNumber()},
                                   boost::none /* autocommit */,
                                   TransactionParticipant::TransactionActions::kNone);
    ASSERT(txnParticipant.transactionIsInRetryableWriteMode());
    ASSERT_EQ(txnParticipant.getActiveTxnNumberAndRetryCounter().getTxnRetryCounter(),
              kUninitializedTxnRetryCounter);

    // This mimics the scenario where a retryable write is turned into a retryable internal
    // transaction before any statements have executed
    txnParticipant.beginOrContinue(opCtx(),
                                   {*opCtx()->getTxnNumber()},
                                   false /* autocommit */,
                                   TransactionParticipant::TransactionActions::kStartOrContinue);
    ASSERT_TRUE(txnParticipant.transactionIsInProgress());
    ASSERT_EQ(txnParticipant.getActiveTxnNumberAndRetryCounter().getTxnNumber(),
              *opCtx()->getTxnNumber());
    ASSERT_EQ(txnParticipant.getActiveTxnNumberAndRetryCounter().getTxnRetryCounter(), 0);
}

TEST_F(
    ShardTxnParticipantTest,
    StartOrContinueTxnWithEqualRetryCounterAndNoneStateShouldRestartOnConflictingAbortedRetryableTxn) {
    const auto parentLsid = makeLogicalSessionIdForTest();
    const auto parentTxnNumber = *opCtx()->getTxnNumber();

    // Set up the TransactionParticipant for a retryable write, but don't execute a write yet
    {
        auto mongoDSessionCatalog = MongoDSessionCatalog::get(opCtx());
        auto ocs = mongoDSessionCatalog->checkOutSession(opCtx());
        auto txnParticipant = TransactionParticipant::get(opCtx());
        txnParticipant.beginOrContinue(opCtx(),
                                       {parentTxnNumber},
                                       boost::none /* autocommit */,
                                       TransactionParticipant::TransactionActions::kNone);
    }

    // Run a conflicting transaction that is aborted without prepare
    const auto conflictingLsid =
        makeLogicalSessionIdWithTxnNumberAndUUIDForTest(parentLsid, parentTxnNumber);
    runFunctionFromDifferentOpCtx([conflictingLsid](OperationContext* newOpCtx) {
        newOpCtx->setLogicalSessionId(conflictingLsid);
        newOpCtx->setTxnNumber(0);
        newOpCtx->setInMultiDocumentTransaction();

        auto mongoDSessionCatalog = MongoDSessionCatalog::get(newOpCtx);
        auto ocs = mongoDSessionCatalog->checkOutSession(newOpCtx);
        auto txnParticipant = TransactionParticipant::get(newOpCtx);

        txnParticipant.beginOrContinue(newOpCtx,
                                       {0},
                                       false /* autocommit */,
                                       TransactionParticipant::TransactionActions::kStart);
        ASSERT_TRUE(txnParticipant.transactionIsInProgress());

        txnParticipant.abortTransaction(newOpCtx);
        ASSERT_TRUE(txnParticipant.transactionIsAbortedWithoutPrepare());
    });

    // Retry the retryable write as an internal transaction, the TransactionParticipant will
    // "restart" the transaction because the conflicting transaction has been aborted
    {
        opCtx()->setLogicalSessionId(parentLsid);
        auto mongoDSessionCatalog = MongoDSessionCatalog::get(opCtx());
        auto ocs = mongoDSessionCatalog->checkOutSession(opCtx());
        auto txnParticipant = TransactionParticipant::get(opCtx());
        opCtx()->setInMultiDocumentTransaction();
        txnParticipant.beginOrContinue(
            opCtx(),
            {parentTxnNumber, 0},
            false /* autocommit */,
            TransactionParticipant::TransactionActions::kStartOrContinue);
        ASSERT_TRUE(txnParticipant.transactionIsInProgress());
        ASSERT_EQ(txnParticipant.getActiveTxnNumberAndRetryCounter().getTxnNumber(),
                  *opCtx()->getTxnNumber());
        ASSERT_EQ(txnParticipant.getActiveTxnNumberAndRetryCounter().getTxnRetryCounter(), 0);
    }
}

TEST_F(
    ShardTxnParticipantTest,
    StartOrContinueTxnWithEqualRetryCounterAndNoneStateShouldErrorOnConflictingInProgressRetryableTxn) {
    const auto parentLsid = makeLogicalSessionIdForTest();
    const auto parentTxnNumber = *opCtx()->getTxnNumber();

    // Set up the TransactionParticipant for a retryable write, but don't execute a write yet
    {
        auto mongoDSessionCatalog = MongoDSessionCatalog::get(opCtx());
        auto ocs = mongoDSessionCatalog->checkOutSession(opCtx());
        auto txnParticipant = TransactionParticipant::get(opCtx());
        txnParticipant.beginOrContinue(opCtx(),
                                       {parentTxnNumber},
                                       boost::none /* autocommit */,
                                       TransactionParticipant::TransactionActions::kNone);
    }

    // Start a conflicting transaction that's in the inProgress state
    const auto conflictingLsid =
        makeLogicalSessionIdWithTxnNumberAndUUIDForTest(parentLsid, parentTxnNumber);
    runFunctionFromDifferentOpCtx([conflictingLsid](OperationContext* newOpCtx) {
        newOpCtx->setLogicalSessionId(conflictingLsid);
        newOpCtx->setTxnNumber(0);
        newOpCtx->setInMultiDocumentTransaction();

        auto mongoDSessionCatalog = MongoDSessionCatalog::get(newOpCtx);
        auto ocs = mongoDSessionCatalog->checkOutSession(newOpCtx);
        auto txnParticipant = TransactionParticipant::get(newOpCtx);

        txnParticipant.beginOrContinue(newOpCtx,
                                       {0},
                                       false /* autocommit */,
                                       TransactionParticipant::TransactionActions::kStart);
        ASSERT_TRUE(txnParticipant.transactionIsInProgress());
    });

    // Attempt to convert the retryable write into a transaction. This should throw because there is
    // an open conflicting transaction
    {
        opCtx()->setLogicalSessionId(parentLsid);

        auto mongoDSessionCatalog = MongoDSessionCatalog::get(opCtx());
        auto ocs = mongoDSessionCatalog->checkOutSession(opCtx());
        auto txnParticipant = TransactionParticipant::get(opCtx());
        opCtx()->setInMultiDocumentTransaction();
        ASSERT_THROWS_CODE(txnParticipant.beginOrContinue(
                               opCtx(),
                               {parentTxnNumber},
                               false /* autocommit */,
                               TransactionParticipant::TransactionActions::kStartOrContinue),
                           AssertionException,
                           6202002);
    }

    runFunctionFromDifferentOpCtx([conflictingLsid](OperationContext* newOpCtx) {
        newOpCtx->setLogicalSessionId(conflictingLsid);
        newOpCtx->setTxnNumber(0);
        newOpCtx->setInMultiDocumentTransaction();

        auto mongoDSessionCatalog = MongoDSessionCatalog::get(newOpCtx);
        auto ocs = mongoDSessionCatalog->checkOutSession(newOpCtx);
        auto txnParticipant = TransactionParticipant::get(newOpCtx);

        ASSERT_TRUE(txnParticipant.transactionIsInProgress());
        ASSERT_EQ(txnParticipant.getActiveTxnNumberAndRetryCounter().getTxnNumber(),
                  *newOpCtx->getTxnNumber());
    });
}

TEST_F(ShardTxnParticipantTest, StartOrContinueTxnWithMatchingReadConcernShouldContinue) {
    repl::ReadConcernArgs readConcernArgs;
    ASSERT_OK(
        readConcernArgs.initialize(BSON("find"
                                        << "test" << repl::ReadConcernArgs::kReadConcernFieldName
                                        << BSON(repl::ReadConcernArgs::kAfterClusterTimeFieldName
                                                << LogicalTime(Timestamp(1, 2)).asTimestamp()
                                                << repl::ReadConcernArgs::kLevelFieldName
                                                << "majority"))));
    repl::ReadConcernArgs::get(opCtx()) = readConcernArgs;

    auto sessionCheckout = checkOutSession();
    auto txnParticipant = TransactionParticipant::get(opCtx());
    ASSERT(txnParticipant.transactionIsInProgress());
    txnParticipant.unstashTransactionResources(opCtx(), "insert");

    // Stash the resources to mimic that an op with readConern 'readConcern' has already executed
    txnParticipant.stashTransactionResources(opCtx());
    ASSERT_EQ(txnParticipant.getTxnResourceStashReadConcernArgsForTest().getLevel(),
              repl::ReadConcernArgs::get(opCtx()).getLevel());
    ASSERT_EQ(txnParticipant.getTxnResourceStashReadConcernArgsForTest().getArgsAtClusterTime(),
              repl::ReadConcernArgs::get(opCtx()).getArgsAtClusterTime());
    ASSERT_EQ(txnParticipant.getTxnResourceStashReadConcernArgsForTest().getArgsAfterClusterTime(),
              repl::ReadConcernArgs::get(opCtx()).getArgsAfterClusterTime());

    // The readConern on the opCtx has not been updated and it matches that on the TxnResourceStash,
    // so the TransactionParticipant should continue
    txnParticipant.beginOrContinue(opCtx(),
                                   {*opCtx()->getTxnNumber()},
                                   false /* autocommit */,
                                   TransactionParticipant::TransactionActions::kStartOrContinue);
    ASSERT_TRUE(txnParticipant.transactionIsInProgress());
}


TEST_F(ShardTxnParticipantTest, StartOrContinueTxnWithDifferentReadConcernShouldError) {
    repl::ReadConcernArgs originalReadConcernArgs;
    ASSERT_OK(originalReadConcernArgs.initialize(
        BSON("find"
             << "test" << repl::ReadConcernArgs::kReadConcernFieldName
             << BSON(repl::ReadConcernArgs::kAfterClusterTimeFieldName
                     << LogicalTime(Timestamp(1, 2)).asTimestamp()
                     << repl::ReadConcernArgs::kLevelFieldName << "majority"))));
    repl::ReadConcernArgs::get(opCtx()) = originalReadConcernArgs;

    auto sessionCheckout = checkOutSession();
    auto txnParticipant = TransactionParticipant::get(opCtx());
    ASSERT(txnParticipant.transactionIsInProgress());
    txnParticipant.unstashTransactionResources(opCtx(), "insert");

    // Stash the resources to mimic that an op with readConern 'readConcern' has already executed
    txnParticipant.stashTransactionResources(opCtx());

    // Set the newReadConernArgs on the opCtx before calling beginOrContinue to mimic the service
    // entry point's behavior. The readConcern args on the TxnResourceStash should still be the
    // original readConern.
    repl::ReadConcernArgs newReadConcernArgs;
    ASSERT_OK(
        newReadConcernArgs.initialize(BSON("find"
                                           << "test" << repl::ReadConcernArgs::kReadConcernFieldName
                                           << BSON(repl::ReadConcernArgs::kAtClusterTimeFieldName
                                                   << LogicalTime(Timestamp(1, 2)).asTimestamp()
                                                   << repl::ReadConcernArgs::kLevelFieldName
                                                   << "snapshot"))));
    repl::ReadConcernArgs::get(opCtx()) = newReadConcernArgs;

    ASSERT_EQ(txnParticipant.getTxnResourceStashReadConcernArgsForTest().getLevel(),
              originalReadConcernArgs.getLevel());
    ASSERT_EQ(txnParticipant.getTxnResourceStashReadConcernArgsForTest().getArgsAtClusterTime(),
              originalReadConcernArgs.getArgsAtClusterTime());
    ASSERT_EQ(txnParticipant.getTxnResourceStashReadConcernArgsForTest().getArgsAfterClusterTime(),
              originalReadConcernArgs.getArgsAfterClusterTime());

    // Assert that we fail to continue the transaction because the readConcern on the
    // TxnResourceStash does not match that on the opCtx
    ASSERT_THROWS_CODE(txnParticipant.beginOrContinue(
                           opCtx(),
                           {*opCtx()->getTxnNumber()},
                           false /* autocommit */,
                           TransactionParticipant::TransactionActions::kStartOrContinue),
                       AssertionException,
                       ErrorCodes::IllegalOperation);
}

/**
 * Test fixture for transactions metrics.
 */
class TransactionsMetricsTest : public TxnParticipantTest {
protected:
    TransactionsMetricsTest()
        : TxnParticipantTest(Options{}.useMockClock(true).useMockTickSource<Microseconds>(true)) {}

    void setUp() override {
        TxnParticipantTest::setUp();

        // Ensure that the tick source is not initialized to zero.
        mockTickSource()->reset(1);
    }

    /**
     * Returns the mock tick source.
     */
    TickSourceMock<Microseconds>* mockTickSource() {
        return dynamic_cast<TickSourceMock<Microseconds>*>(getServiceContext()->getTickSource());
    }
};

TEST_F(TransactionsMetricsTest, IncrementTotalStartedUponStartTransaction) {
    unsigned long long beforeTransactionStart =
        ServerTransactionsMetrics::get(opCtx())->getTotalStarted();

    auto sessionCheckout = checkOutSession();

    // Tests that the total transactions started counter is incremented by 1 when a new transaction
    // is started.
    ASSERT_EQ(ServerTransactionsMetrics::get(opCtx())->getTotalStarted(),
              beforeTransactionStart + 1U);
}

TEST_F(TransactionsMetricsTest, IncrementPreparedTransaction) {
    auto sessionCheckout = checkOutSession();
    auto txnParticipant = TransactionParticipant::get(opCtx());
    unsigned long long beforePrepareCount =
        ServerTransactionsMetrics::get(opCtx())->getTotalPrepared();
    txnParticipant.unstashTransactionResources(opCtx(), "prepareTransaction");
    txnParticipant.prepareTransaction(opCtx(), {});

    ASSERT_EQ(ServerTransactionsMetrics::get(opCtx())->getTotalPrepared(), beforePrepareCount + 1U);
}

TEST_F(TransactionsMetricsTest, IncrementTotalCommittedOnCommit) {
    auto sessionCheckout = checkOutSession();
    auto txnParticipant = TransactionParticipant::get(opCtx());
    txnParticipant.unstashTransactionResources(opCtx(), "commitTransaction");

    unsigned long long beforeCommitCount =
        ServerTransactionsMetrics::get(opCtx())->getTotalCommitted();

    txnParticipant.commitUnpreparedTransaction(opCtx());

    // Assert that the committed counter is incremented by 1.
    ASSERT_EQ(ServerTransactionsMetrics::get(opCtx())->getTotalCommitted(), beforeCommitCount + 1U);
}

TEST_F(TransactionsMetricsTest, IncrementTotalPreparedThenCommitted) {
    auto sessionCheckout = checkOutSession();
    auto txnParticipant = TransactionParticipant::get(opCtx());

    txnParticipant.unstashTransactionResources(opCtx(), "commitTransaction");
    const auto [prepareTimestamp, namespaces] = txnParticipant.prepareTransaction(opCtx(), {});

    unsigned long long beforePreparedThenCommittedCount =
        ServerTransactionsMetrics::get(opCtx())->getTotalPreparedThenCommitted();

    txnParticipant.commitPreparedTransaction(opCtx(), prepareTimestamp, {});

    ASSERT_TRUE(txnParticipant.transactionIsCommitted());
    ASSERT_EQ(ServerTransactionsMetrics::get(opCtx())->getTotalPreparedThenCommitted(),
              beforePreparedThenCommittedCount + 1U);
}


TEST_F(TransactionsMetricsTest, IncrementTotalAbortedUponAbort) {
    auto sessionCheckout = checkOutSession();
    auto txnParticipant = TransactionParticipant::get(opCtx());
    txnParticipant.unstashTransactionResources(opCtx(), "insert");

    unsigned long long beforeAbortCount =
        ServerTransactionsMetrics::get(opCtx())->getTotalAborted();

    txnParticipant.abortTransaction(opCtx());

    // Assert that the aborted counter is incremented by 1.
    ASSERT_EQ(ServerTransactionsMetrics::get(opCtx())->getTotalAborted(), beforeAbortCount + 1U);
}

TEST_F(TransactionsMetricsTest, IncrementTotalPreparedThenAborted) {
    unsigned long long beforePreparedThenAbortedCount =
        ServerTransactionsMetrics::get(opCtx())->getTotalPreparedThenAborted();

    auto sessionCheckout = checkOutSession();
    auto txnParticipant = TransactionParticipant::get(opCtx());
    txnParticipant.unstashTransactionResources(opCtx(), "prepareTransaction");
    txnParticipant.prepareTransaction(opCtx(), {});

    txnParticipant.abortTransaction(opCtx());
    ASSERT(txnParticipant.transactionIsAborted());
    ASSERT_EQ(ServerTransactionsMetrics::get(opCtx())->getTotalPreparedThenAborted(),
              beforePreparedThenAbortedCount + 1U);
}

TEST_F(TransactionsMetricsTest, IncrementCurrentPreparedWithCommit) {
    unsigned long long beforeCurrentPrepared =
        ServerTransactionsMetrics::get(opCtx())->getCurrentPrepared();

    auto sessionCheckout = checkOutSession();
    auto txnParticipant = TransactionParticipant::get(opCtx());

    txnParticipant.unstashTransactionResources(opCtx(), "commitTransaction");
    const auto [prepareTimestamp, namespaces] = txnParticipant.prepareTransaction(opCtx(), {});

    ASSERT_EQ(ServerTransactionsMetrics::get(opCtx())->getCurrentPrepared(),
              beforeCurrentPrepared + 1U);
    txnParticipant.commitPreparedTransaction(opCtx(), prepareTimestamp, {});
    ASSERT(txnParticipant.transactionIsCommitted());
    ASSERT_EQ(ServerTransactionsMetrics::get(opCtx())->getCurrentPrepared(), beforeCurrentPrepared);
}

TEST_F(TransactionsMetricsTest, IncrementCurrentPreparedWithAbort) {
    unsigned long long beforeCurrentPrepared =
        ServerTransactionsMetrics::get(opCtx())->getCurrentPrepared();

    auto sessionCheckout = checkOutSession();
    auto txnParticipant = TransactionParticipant::get(opCtx());
    txnParticipant.unstashTransactionResources(opCtx(), "abortTransaction");
    txnParticipant.prepareTransaction(opCtx(), {});

    ASSERT_EQ(ServerTransactionsMetrics::get(opCtx())->getCurrentPrepared(),
              beforeCurrentPrepared + 1U);
    txnParticipant.abortTransaction(opCtx());
    ASSERT(txnParticipant.transactionIsAborted());
    ASSERT_EQ(ServerTransactionsMetrics::get(opCtx())->getCurrentPrepared(), beforeCurrentPrepared);
}

TEST_F(TransactionsMetricsTest, NoPreparedMetricsChangesAfterExceptionInPrepare) {
    unsigned long long beforeCurrentPrepared =
        ServerTransactionsMetrics::get(opCtx())->getCurrentPrepared();
    unsigned long long beforeTotalPrepared =
        ServerTransactionsMetrics::get(opCtx())->getTotalPrepared();
    unsigned long long beforeTotalPreparedThenCommitted =
        ServerTransactionsMetrics::get(opCtx())->getTotalPreparedThenCommitted();
    unsigned long long beforeTotalPreparedThenAborted =
        ServerTransactionsMetrics::get(opCtx())->getTotalPreparedThenAborted();

    auto sessionCheckout = checkOutSession();
    auto txnParticipant = TransactionParticipant::get(opCtx());

    txnParticipant.unstashTransactionResources(opCtx(), "prepareTransaction");

    _opObserver->postTransactionPrepareThrowsException = true;

    ASSERT_THROWS_CODE(txnParticipant.prepareTransaction(opCtx(), {}),
                       AssertionException,
                       ErrorCodes::OperationFailed);

    ASSERT_EQ(ServerTransactionsMetrics::get(opCtx())->getCurrentPrepared(), beforeCurrentPrepared);
    ASSERT_EQ(ServerTransactionsMetrics::get(opCtx())->getTotalPrepared(), beforeTotalPrepared);
    ASSERT_EQ(ServerTransactionsMetrics::get(opCtx())->getTotalPreparedThenCommitted(),
              beforeTotalPreparedThenCommitted);
    ASSERT_EQ(ServerTransactionsMetrics::get(opCtx())->getTotalPreparedThenAborted(),
              beforeTotalPreparedThenAborted);

    if (txnParticipant.transactionIsOpen())
        txnParticipant.abortTransaction(opCtx());
    ASSERT(txnParticipant.transactionIsAborted());

    ASSERT_EQ(ServerTransactionsMetrics::get(opCtx())->getCurrentPrepared(), beforeCurrentPrepared);
    ASSERT_EQ(ServerTransactionsMetrics::get(opCtx())->getTotalPrepared(), beforeTotalPrepared);
    ASSERT_EQ(ServerTransactionsMetrics::get(opCtx())->getTotalPreparedThenCommitted(),
              beforeTotalPreparedThenCommitted);
    ASSERT_EQ(ServerTransactionsMetrics::get(opCtx())->getTotalPreparedThenAborted(),
              beforeTotalPreparedThenAborted);
}

TEST_F(TransactionsMetricsTest, TrackTotalOpenTransactionsWithAbort) {
    unsigned long long beforeTransactionStart =
        ServerTransactionsMetrics::get(opCtx())->getCurrentOpen();

    // Tests that starting a transaction increments the open transactions counter by 1.
    auto sessionCheckout = checkOutSession();
    auto txnParticipant = TransactionParticipant::get(opCtx());
    txnParticipant.unstashTransactionResources(opCtx(), "insert");
    ASSERT_EQ(ServerTransactionsMetrics::get(opCtx())->getCurrentOpen(),
              beforeTransactionStart + 1U);

    // Tests that stashing the transaction resources does not affect the open transactions counter.
    { Lock::GlobalLock lk(opCtx(), MODE_IX, Date_t::now(), Lock::InterruptBehavior::kThrow); }
    txnParticipant.stashTransactionResources(opCtx());
    ASSERT_EQ(ServerTransactionsMetrics::get(opCtx())->getCurrentOpen(),
              beforeTransactionStart + 1U);

    // Tests that aborting a transaction decrements the open transactions counter by 1.
    txnParticipant.abortTransaction(opCtx());
    ASSERT_EQ(ServerTransactionsMetrics::get(opCtx())->getCurrentOpen(), beforeTransactionStart);
}

TEST_F(TransactionsMetricsTest, TrackTotalOpenTransactionsWithCommit) {
    unsigned long long beforeTransactionStart =
        ServerTransactionsMetrics::get(opCtx())->getCurrentOpen();

    // Tests that starting a transaction increments the open transactions counter by 1.
    auto sessionCheckout = checkOutSession();
    auto txnParticipant = TransactionParticipant::get(opCtx());
    txnParticipant.unstashTransactionResources(opCtx(), "insert");
    ASSERT_EQ(ServerTransactionsMetrics::get(opCtx())->getCurrentOpen(),
              beforeTransactionStart + 1U);

    // Tests that stashing the transaction resources does not affect the open transactions counter.
    { Lock::GlobalLock lk(opCtx(), MODE_IX, Date_t::now(), Lock::InterruptBehavior::kThrow); }
    txnParticipant.stashTransactionResources(opCtx());
    ASSERT_EQ(ServerTransactionsMetrics::get(opCtx())->getCurrentOpen(),
              beforeTransactionStart + 1U);

    txnParticipant.unstashTransactionResources(opCtx(), "insert");

    // Tests that committing a transaction decrements the open transactions counter by 1.
    txnParticipant.commitUnpreparedTransaction(opCtx());
    ASSERT_EQ(ServerTransactionsMetrics::get(opCtx())->getCurrentOpen(), beforeTransactionStart);
}

TEST_F(TransactionsMetricsTest, TrackTotalActiveAndInactiveTransactionsWithCommit) {
    unsigned long long beforeActiveCounter =
        ServerTransactionsMetrics::get(opCtx())->getCurrentActive();
    unsigned long long beforeInactiveCounter =
        ServerTransactionsMetrics::get(opCtx())->getCurrentInactive();

    // Starting the transaction should put it into an inactive state.
    auto sessionCheckout = checkOutSession();
    ASSERT_EQ(ServerTransactionsMetrics::get(opCtx())->getCurrentInactive(),
              beforeInactiveCounter + 1);

    // Tests that the first unstash increments the active counter and decrements the inactive
    // counter.
    auto txnParticipant = TransactionParticipant::get(opCtx());
    txnParticipant.unstashTransactionResources(opCtx(), "insert");
    ASSERT_EQ(ServerTransactionsMetrics::get(opCtx())->getCurrentActive(),
              beforeActiveCounter + 1U);
    ASSERT_EQ(ServerTransactionsMetrics::get(opCtx())->getCurrentInactive(), beforeInactiveCounter);

    // Tests that stashing the transaction resources decrements active counter and increments
    // inactive counter.
    { Lock::GlobalLock lk(opCtx(), MODE_IX, Date_t::now(), Lock::InterruptBehavior::kThrow); }
    txnParticipant.stashTransactionResources(opCtx());
    ASSERT_EQ(ServerTransactionsMetrics::get(opCtx())->getCurrentActive(), beforeActiveCounter);
    ASSERT_EQ(ServerTransactionsMetrics::get(opCtx())->getCurrentInactive(),
              beforeInactiveCounter + 1U);

    // Tests that the second unstash increments the active counter and decrements the inactive
    // counter.
    txnParticipant.unstashTransactionResources(opCtx(), "insert");
    ASSERT_EQ(ServerTransactionsMetrics::get(opCtx())->getCurrentActive(),
              beforeActiveCounter + 1U);
    ASSERT_EQ(ServerTransactionsMetrics::get(opCtx())->getCurrentInactive(), beforeInactiveCounter);

    // Tests that committing a transaction decrements the active counter only.
    txnParticipant.commitUnpreparedTransaction(opCtx());
    ASSERT_EQ(ServerTransactionsMetrics::get(opCtx())->getCurrentActive(), beforeActiveCounter);
    ASSERT_EQ(ServerTransactionsMetrics::get(opCtx())->getCurrentInactive(), beforeInactiveCounter);
}

TEST_F(TransactionsMetricsTest, TrackTotalActiveAndInactiveTransactionsWithStashedAbort) {
    unsigned long long beforeActiveCounter =
        ServerTransactionsMetrics::get(opCtx())->getCurrentActive();
    unsigned long long beforeInactiveCounter =
        ServerTransactionsMetrics::get(opCtx())->getCurrentInactive();

    // Starting the transaction should put it into an inactive state.
    auto sessionCheckout = checkOutSession();
    ASSERT_EQ(ServerTransactionsMetrics::get(opCtx())->getCurrentInactive(),
              beforeInactiveCounter + 1);

    // Tests that the first unstash increments the active counter and decrements the inactive
    // counter.
    auto txnParticipant = TransactionParticipant::get(opCtx());
    txnParticipant.unstashTransactionResources(opCtx(), "insert");
    ASSERT_EQ(ServerTransactionsMetrics::get(opCtx())->getCurrentActive(),
              beforeActiveCounter + 1U);
    ASSERT_EQ(ServerTransactionsMetrics::get(opCtx())->getCurrentInactive(), beforeInactiveCounter);

    // Tests that stashing the transaction resources decrements active counter and increments
    // inactive counter.
    { Lock::GlobalLock lk(opCtx(), MODE_IX, Date_t::now(), Lock::InterruptBehavior::kThrow); }
    txnParticipant.stashTransactionResources(opCtx());
    ASSERT_EQ(ServerTransactionsMetrics::get(opCtx())->getCurrentActive(), beforeActiveCounter);
    ASSERT_EQ(ServerTransactionsMetrics::get(opCtx())->getCurrentInactive(),
              beforeInactiveCounter + 1U);

    // Tests that aborting a stashed transaction decrements the inactive counter only.
    txnParticipant.abortTransaction(opCtx());
    ASSERT_EQ(ServerTransactionsMetrics::get(opCtx())->getCurrentActive(), beforeActiveCounter);
    ASSERT_EQ(ServerTransactionsMetrics::get(opCtx())->getCurrentInactive(), beforeInactiveCounter);
}

TEST_F(TransactionsMetricsTest, TrackTotalActiveAndInactiveTransactionsWithUnstashedAbort) {
    unsigned long long beforeActiveCounter =
        ServerTransactionsMetrics::get(opCtx())->getCurrentActive();
    unsigned long long beforeInactiveCounter =
        ServerTransactionsMetrics::get(opCtx())->getCurrentInactive();

    // Starting the transaction should put it into an inactive state.
    auto sessionCheckout = checkOutSession();
    ASSERT_EQ(ServerTransactionsMetrics::get(opCtx())->getCurrentInactive(),
              beforeInactiveCounter + 1);

    // Tests that the first unstash increments the active counter and decrements the inactive
    // counter.
    auto txnParticipant = TransactionParticipant::get(opCtx());
    txnParticipant.unstashTransactionResources(opCtx(), "insert");
    ASSERT_EQ(ServerTransactionsMetrics::get(opCtx())->getCurrentActive(),
              beforeActiveCounter + 1U);
    ASSERT_EQ(ServerTransactionsMetrics::get(opCtx())->getCurrentInactive(), beforeInactiveCounter);

    // Tests that aborting a stashed transaction decrements the active counter only.
    txnParticipant.abortTransaction(opCtx());
    ASSERT_EQ(ServerTransactionsMetrics::get(opCtx())->getCurrentActive(), beforeActiveCounter);
    ASSERT_EQ(ServerTransactionsMetrics::get(opCtx())->getCurrentInactive(), beforeInactiveCounter);
}

TEST_F(TransactionsMetricsTest, TrackCurrentActiveAndInactivePreparedTransactionsOnCommit) {
    unsigned long long beforeActivePreparedCounter =
        ServerTransactionsMetrics::get(opCtx())->getCurrentActive();
    unsigned long long beforeInactivePreparedCounter =
        ServerTransactionsMetrics::get(opCtx())->getCurrentInactive();
    auto sessionCheckout = checkOutSession();
    unsigned long long beforePrepareCount =
        ServerTransactionsMetrics::get(opCtx())->getTotalPrepared();
    unsigned long long beforePreparedThenCommittedCount =
        ServerTransactionsMetrics::get(opCtx())->getTotalPreparedThenCommitted();

    // Tests that unstashing a transaction puts it into an active state.
    auto txnParticipant = TransactionParticipant::get(opCtx());

    txnParticipant.unstashTransactionResources(opCtx(), "prepareTransaction");
    const auto [prepareTimestamp, namespaces] = txnParticipant.prepareTransaction(opCtx(), {});

    ASSERT_EQ(ServerTransactionsMetrics::get(opCtx())->getCurrentActive(),
              beforeActivePreparedCounter + 1U);
    ASSERT_EQ(ServerTransactionsMetrics::get(opCtx())->getCurrentInactive(),
              beforeInactivePreparedCounter);
    ASSERT_EQ(ServerTransactionsMetrics::get(opCtx())->getTotalPrepared(), beforePrepareCount + 1U);

    // Tests that the first stash decrements the active counter and increments the inactive counter.
    txnParticipant.stashTransactionResources(opCtx());
    ASSERT_EQ(ServerTransactionsMetrics::get(opCtx())->getCurrentActive(),
              beforeActivePreparedCounter);
    ASSERT_EQ(ServerTransactionsMetrics::get(opCtx())->getCurrentInactive(),
              beforeInactivePreparedCounter + 1U);

    // Tests that unstashing increments the active counter and decrements the inactive counter.
    txnParticipant.unstashTransactionResources(opCtx(), "commitTransaction");
    ASSERT_EQ(ServerTransactionsMetrics::get(opCtx())->getCurrentActive(),
              beforeActivePreparedCounter + 1U);
    ASSERT_EQ(ServerTransactionsMetrics::get(opCtx())->getCurrentInactive(),
              beforeInactivePreparedCounter);

    // Tests that committing decrements the active counter only.
    txnParticipant.commitPreparedTransaction(opCtx(), prepareTimestamp, {});

    ASSERT_EQ(ServerTransactionsMetrics::get(opCtx())->getCurrentActive(),
              beforeActivePreparedCounter);
    ASSERT_EQ(ServerTransactionsMetrics::get(opCtx())->getCurrentInactive(),
              beforeInactivePreparedCounter);
    ASSERT_EQ(ServerTransactionsMetrics::get(opCtx())->getTotalPreparedThenCommitted(),
              beforePreparedThenCommittedCount + 1U);
}

TEST_F(TransactionsMetricsTest,
       TrackCurrentActiveAndInactivePreparedTransactionsWithUnstashedAbort) {
    unsigned long long beforeActivePreparedCounter =
        ServerTransactionsMetrics::get(opCtx())->getCurrentActive();
    unsigned long long beforeInactivePreparedCounter =
        ServerTransactionsMetrics::get(opCtx())->getCurrentInactive();
    auto sessionCheckout = checkOutSession();

    auto txnParticipant = TransactionParticipant::get(opCtx());

    // Tests that unstashing a transaction increments the active counter only.
    txnParticipant.unstashTransactionResources(opCtx(), "prepareTransaction");
    txnParticipant.prepareTransaction(opCtx(), {});
    ASSERT_EQ(ServerTransactionsMetrics::get(opCtx())->getCurrentActive(),
              beforeActivePreparedCounter + 1U);
    ASSERT_EQ(ServerTransactionsMetrics::get(opCtx())->getCurrentInactive(),
              beforeInactivePreparedCounter);

    // Tests that stashing a prepared transaction decrements the active counter and increments the
    // inactive counter.
    txnParticipant.stashTransactionResources(opCtx());
    ASSERT_EQ(ServerTransactionsMetrics::get(opCtx())->getCurrentActive(),
              beforeActivePreparedCounter);
    ASSERT_EQ(ServerTransactionsMetrics::get(opCtx())->getCurrentInactive(),
              beforeInactivePreparedCounter + 1U);

    // Tests that aborting a stashed prepared transaction decrements the inactive counter only.
    txnParticipant.unstashTransactionResources(opCtx(), "abortTransaction");
    ASSERT_EQ(ServerTransactionsMetrics::get(opCtx())->getCurrentActive(),
              beforeActivePreparedCounter + 1U);
    ASSERT_EQ(ServerTransactionsMetrics::get(opCtx())->getCurrentInactive(),
              beforeInactivePreparedCounter);
    txnParticipant.abortTransaction(opCtx());
    ASSERT(txnParticipant.transactionIsAborted());
    ASSERT_EQ(ServerTransactionsMetrics::get(opCtx())->getCurrentActive(),
              beforeActivePreparedCounter);
    ASSERT_EQ(ServerTransactionsMetrics::get(opCtx())->getCurrentInactive(),
              beforeInactivePreparedCounter);
}

TEST_F(TransactionsMetricsTest, TransactionErrorsBeforeUnstash) {
    unsigned long long beforeActiveCounter =
        ServerTransactionsMetrics::get(opCtx())->getCurrentActive();
    unsigned long long beforeInactiveCounter =
        ServerTransactionsMetrics::get(opCtx())->getCurrentInactive();
    unsigned long long beforeAbortedCounter =
        ServerTransactionsMetrics::get(opCtx())->getTotalAborted();

    // The first transaction statement checks out the session and begins the transaction but returns
    // before unstashTransactionResources().
    auto sessionCheckout = checkOutSession();

    // The transaction is now inactive.
    ASSERT_EQ(ServerTransactionsMetrics::get(opCtx())->getCurrentActive(), beforeActiveCounter);
    ASSERT_EQ(ServerTransactionsMetrics::get(opCtx())->getCurrentInactive(),
              beforeInactiveCounter + 1U);
    ASSERT_EQ(ServerTransactionsMetrics::get(opCtx())->getTotalAborted(), beforeAbortedCounter);

    // The second transaction statement continues the transaction. Since there are no stashed
    // transaction resources, it is not safe to continue the transaction, so the transaction is
    // aborted.
    auto txnParticipant = TransactionParticipant::get(opCtx());
    const bool autocommit = false;
    const TransactionParticipant::TransactionActions startTransaction =
        TransactionParticipant::TransactionActions::kContinue;
    ASSERT_THROWS_CODE(txnParticipant.beginOrContinue(
                           opCtx(), {*opCtx()->getTxnNumber()}, autocommit, startTransaction),
                       AssertionException,
                       ErrorCodes::NoSuchTransaction);

    // The transaction is now aborted.
    ASSERT_EQ(ServerTransactionsMetrics::get(opCtx())->getCurrentActive(), beforeActiveCounter);
    ASSERT_EQ(ServerTransactionsMetrics::get(opCtx())->getCurrentInactive(), beforeInactiveCounter);
    ASSERT_EQ(ServerTransactionsMetrics::get(opCtx())->getTotalAborted(),
              beforeAbortedCounter + 1U);
}

TEST_F(TransactionsMetricsTest, SingleTransactionStatsDurationShouldBeSetUponCommit) {
    auto tickSource = mockTickSource();

    auto sessionCheckout = checkOutSession();
    auto txnParticipant = TransactionParticipant::get(opCtx());
    txnParticipant.unstashTransactionResources(opCtx(), "commitTransaction");
    // The transaction machinery cannot store an empty locker.
    { Lock::GlobalLock lk(opCtx(), MODE_IX, Date_t::now(), Lock::InterruptBehavior::kThrow); }

    // Advance the clock.
    tickSource->advance(Microseconds(100));

    txnParticipant.commitUnpreparedTransaction(opCtx());
    ASSERT_EQ(txnParticipant.getSingleTransactionStatsForTest().getDuration(tickSource,
                                                                            tickSource->getTicks()),
              Microseconds(100));
}

TEST_F(TransactionsMetricsTest, SingleTransactionStatsPreparedDurationShouldBeSetUponCommit) {
    auto tickSource = mockTickSource();

    auto sessionCheckout = checkOutSession();
    auto txnParticipant = TransactionParticipant::get(opCtx());
    txnParticipant.unstashTransactionResources(opCtx(), "commitTransaction");
    // The transaction machinery cannot store an empty locker.
    { Lock::GlobalLock lk(opCtx(), MODE_IX, Date_t::now(), Lock::InterruptBehavior::kThrow); }

    // Advance the clock.
    tickSource->advance(Microseconds(10));

    // Prepare the transaction and extend the duration in the prepared state.
    const auto [prepareTimestamp, namespaces] = txnParticipant.prepareTransaction(opCtx(), {});

    tickSource->advance(Microseconds(100));

    txnParticipant.commitPreparedTransaction(opCtx(), prepareTimestamp, {});
    ASSERT_EQ(txnParticipant.getSingleTransactionStatsForTest().getPreparedDuration(
                  tickSource, tickSource->getTicks()),
              Microseconds(100));
}

TEST_F(TransactionsMetricsTest, SingleTransactionStatsDurationShouldBeSetUponAbort) {
    auto tickSource = mockTickSource();

    auto sessionCheckout = checkOutSession();
    auto txnParticipant = TransactionParticipant::get(opCtx());
    txnParticipant.unstashTransactionResources(opCtx(), "insert");

    // Advance the clock.
    tickSource->advance(Microseconds(100));

    txnParticipant.abortTransaction(opCtx());
    ASSERT_EQ(txnParticipant.getSingleTransactionStatsForTest().getDuration(tickSource,
                                                                            tickSource->getTicks()),
              Microseconds(100));
}

TEST_F(TransactionsMetricsTest, SingleTransactionStatsPreparedDurationShouldBeSetUponAbort) {
    auto tickSource = mockTickSource();

    auto sessionCheckout = checkOutSession();
    auto txnParticipant = TransactionParticipant::get(opCtx());
    txnParticipant.unstashTransactionResources(opCtx(), "abortTransaction");

    // Advance the clock.
    tickSource->advance(Microseconds(10));

    // Prepare the transaction and extend the duration in the prepared state.
    txnParticipant.prepareTransaction(opCtx(), {});
    tickSource->advance(Microseconds(100));

    txnParticipant.abortTransaction(opCtx());
    ASSERT_EQ(txnParticipant.getSingleTransactionStatsForTest().getPreparedDuration(
                  tickSource, tickSource->getTicks()),
              Microseconds(100));
}

TEST_F(TransactionsMetricsTest, SingleTransactionStatsDurationShouldKeepIncreasingUntilCommit) {
    auto tickSource = mockTickSource();

    auto sessionCheckout = checkOutSession();
    auto txnParticipant = TransactionParticipant::get(opCtx());
    txnParticipant.unstashTransactionResources(opCtx(), "commitTransaction");
    // The transaction machinery cannot store an empty locker.
    { Lock::GlobalLock lk(opCtx(), MODE_IX, Date_t::now(), Lock::InterruptBehavior::kThrow); }

    tickSource->advance(Microseconds(100));

    // The transaction's duration should have increased.
    ASSERT_EQ(txnParticipant.getSingleTransactionStatsForTest().getDuration(tickSource,
                                                                            tickSource->getTicks()),
              Microseconds(100));

    tickSource->advance(Microseconds(100));

    // Commit the transaction and check duration.
    txnParticipant.commitUnpreparedTransaction(opCtx());
    ASSERT_EQ(txnParticipant.getSingleTransactionStatsForTest().getDuration(tickSource,
                                                                            tickSource->getTicks()),
              Microseconds(200));

    // The transaction committed, so the duration shouldn't have increased even if more time passed.
    tickSource->advance(Microseconds(100));
    ASSERT_EQ(txnParticipant.getSingleTransactionStatsForTest().getDuration(tickSource,
                                                                            tickSource->getTicks()),
              Microseconds(200));
}

TEST_F(TransactionsMetricsTest,
       SingleTransactionStatsPreparedDurationShouldKeepIncreasingUntilCommit) {
    auto tickSource = mockTickSource();

    auto sessionCheckout = checkOutSession();
    auto txnParticipant = TransactionParticipant::get(opCtx());
    txnParticipant.unstashTransactionResources(opCtx(), "commitTransaction");
    // The transaction machinery cannot store an empty locker.
    { Lock::GlobalLock lk(opCtx(), MODE_IX, Date_t::now(), Lock::InterruptBehavior::kThrow); }

    // Prepare the transaction and extend the duration in the prepared state.
    const auto [prepareTimestamp, namespaces] = txnParticipant.prepareTransaction(opCtx(), {});
    tickSource->advance(Microseconds(100));

    // The prepared transaction's duration should have increased.
    ASSERT_EQ(txnParticipant.getSingleTransactionStatsForTest().getPreparedDuration(
                  tickSource, tickSource->getTicks()),
              Microseconds(100));

    tickSource->advance(Microseconds(100));

    // Commit the prepared transaction and check the prepared duration.
    txnParticipant.commitPreparedTransaction(opCtx(), prepareTimestamp, {});
    ASSERT_EQ(txnParticipant.getSingleTransactionStatsForTest().getPreparedDuration(
                  tickSource, tickSource->getTicks()),
              Microseconds(200));

    // The prepared transaction committed, so the prepared duration shouldn't have increased even if
    // more time passed.
    tickSource->advance(Microseconds(100));
    ASSERT_EQ(txnParticipant.getSingleTransactionStatsForTest().getPreparedDuration(
                  tickSource, tickSource->getTicks()),
              Microseconds(200));
}

TEST_F(TransactionsMetricsTest, SingleTransactionStatsDurationShouldKeepIncreasingUntilAbort) {
    auto tickSource = mockTickSource();

    auto sessionCheckout = checkOutSession();
    auto txnParticipant = TransactionParticipant::get(opCtx());
    txnParticipant.unstashTransactionResources(opCtx(), "insert");
    // The transaction machinery cannot store an empty locker.
    { Lock::GlobalLock lk(opCtx(), MODE_IX, Date_t::now(), Lock::InterruptBehavior::kThrow); }

    tickSource->advance(Microseconds(100));

    // The transaction's duration should have increased.
    ASSERT_EQ(txnParticipant.getSingleTransactionStatsForTest().getDuration(tickSource,
                                                                            tickSource->getTicks()),
              Microseconds(100));

    tickSource->advance(Microseconds(100));

    // Abort the transaction and check duration.
    txnParticipant.abortTransaction(opCtx());
    ASSERT_EQ(txnParticipant.getSingleTransactionStatsForTest().getDuration(tickSource,
                                                                            tickSource->getTicks()),
              Microseconds(200));

    // The transaction aborted, so the duration shouldn't have increased even if more time passed.
    tickSource->advance(Microseconds(100));
    ASSERT_EQ(txnParticipant.getSingleTransactionStatsForTest().getDuration(tickSource,
                                                                            tickSource->getTicks()),
              Microseconds(200));
}

TEST_F(TransactionsMetricsTest,
       SingleTransactionStatsPreparedDurationShouldKeepIncreasingUntilAbort) {
    auto tickSource = mockTickSource();

    auto sessionCheckout = checkOutSession();
    auto txnParticipant = TransactionParticipant::get(opCtx());
    txnParticipant.unstashTransactionResources(opCtx(), "abortTransaction");
    // The transaction machinery cannot store an empty locker.
    { Lock::GlobalLock lk(opCtx(), MODE_IX, Date_t::now(), Lock::InterruptBehavior::kThrow); }

    // Prepare the transaction and extend the duration in the prepared state.
    txnParticipant.prepareTransaction(opCtx(), {});
    tickSource->advance(Microseconds(100));

    // The prepared transaction's duration should have increased.
    ASSERT_EQ(txnParticipant.getSingleTransactionStatsForTest().getPreparedDuration(
                  tickSource, tickSource->getTicks()),
              Microseconds(100));

    tickSource->advance(Microseconds(100));

    // Abort the prepared transaction and check the prepared duration.
    txnParticipant.abortTransaction(opCtx());
    ASSERT_EQ(txnParticipant.getSingleTransactionStatsForTest().getPreparedDuration(
                  tickSource, tickSource->getTicks()),
              Microseconds(200));

    // The prepared transaction aborted, so the prepared duration shouldn't have increased even if
    // more time passed.
    tickSource->advance(Microseconds(100));
    ASSERT_EQ(txnParticipant.getSingleTransactionStatsForTest().getPreparedDuration(
                  tickSource, tickSource->getTicks()),
              Microseconds(200));
}

TEST_F(TransactionsMetricsTest, TimeActiveMicrosShouldBeSetUponUnstashAndStash) {
    auto tickSource = mockTickSource();

    auto sessionCheckout = checkOutSession();
    auto txnParticipant = TransactionParticipant::get(opCtx());

    // Time active should be zero.
    ASSERT_EQ(txnParticipant.getSingleTransactionStatsForTest().getTimeActiveMicros(
                  tickSource, tickSource->getTicks()),
              Microseconds{0});

    txnParticipant.unstashTransactionResources(opCtx(), "insert");
    tickSource->advance(Microseconds(100));
    // The transaction machinery cannot store an empty locker.
    { Lock::GlobalLock lk(opCtx(), MODE_IX, Date_t::now(), Lock::InterruptBehavior::kThrow); }
    txnParticipant.stashTransactionResources(opCtx());

    // Advance clock during inactive period.
    tickSource->advance(Microseconds(100));

    // Time active should have increased only during active period.
    ASSERT_EQ(txnParticipant.getSingleTransactionStatsForTest().getTimeActiveMicros(
                  tickSource, tickSource->getTicks()),
              Microseconds{100});

    txnParticipant.unstashTransactionResources(opCtx(), "insert");
    tickSource->advance(Microseconds(100));
    txnParticipant.stashTransactionResources(opCtx());

    // Advance clock during inactive period.
    tickSource->advance(Microseconds(100));

    // Time active should have increased again.
    ASSERT_EQ(txnParticipant.getSingleTransactionStatsForTest().getTimeActiveMicros(
                  tickSource, tickSource->getTicks()),
              Microseconds{200});

    // Start a new transaction.
    const auto higherTxnNum = *opCtx()->getTxnNumber() + 1;
    txnParticipant.beginOrContinue(opCtx(),
                                   {higherTxnNum},
                                   false /* autocommit */,
                                   TransactionParticipant::TransactionActions::kStart);

    // Time active should be zero for a new transaction.
    ASSERT_EQ(txnParticipant.getSingleTransactionStatsForTest().getTimeActiveMicros(
                  tickSource, tickSource->getTicks()),
              Microseconds{0});
}

TEST_F(TransactionsMetricsTest, TimeActiveMicrosShouldBeSetUponUnstashAndAbort) {
    auto tickSource = mockTickSource();

    auto sessionCheckout = checkOutSession();
    auto txnParticipant = TransactionParticipant::get(opCtx());

    // Time active should be zero.
    ASSERT_EQ(txnParticipant.getSingleTransactionStatsForTest().getTimeActiveMicros(
                  tickSource, tickSource->getTicks()),
              Microseconds{0});

    txnParticipant.unstashTransactionResources(opCtx(), "insert");
    tickSource->advance(Microseconds(100));
    txnParticipant.abortTransaction(opCtx());

    // Time active should have increased.
    ASSERT_EQ(txnParticipant.getSingleTransactionStatsForTest().getTimeActiveMicros(
                  tickSource, tickSource->getTicks()),
              Microseconds{100});

    tickSource->advance(Microseconds(100));

    // The transaction is not active after abort, so time active should not have increased.
    ASSERT_EQ(txnParticipant.getSingleTransactionStatsForTest().getTimeActiveMicros(
                  tickSource, tickSource->getTicks()),
              Microseconds{100});
}

TEST_F(TransactionsMetricsTest, TimeActiveMicrosShouldNotBeSetUponAbortOnly) {
    auto tickSource = mockTickSource();

    auto sessionCheckout = checkOutSession();
    auto txnParticipant = TransactionParticipant::get(opCtx());

    // Time active should be zero.
    ASSERT_EQ(txnParticipant.getSingleTransactionStatsForTest().getTimeActiveMicros(
                  tickSource, tickSource->getTicks()),
              Microseconds{0});

    // Advance clock during inactive period.
    tickSource->advance(Microseconds(100));

    txnParticipant.abortTransaction(opCtx());

    // Time active should still be zero.
    ASSERT_EQ(txnParticipant.getSingleTransactionStatsForTest().getTimeActiveMicros(
                  tickSource, tickSource->getTicks()),
              Microseconds{0});
}

TEST_F(TransactionsMetricsTest, TimeActiveMicrosShouldIncreaseUntilStash) {
    auto tickSource = mockTickSource();

    auto sessionCheckout = checkOutSession();
    auto txnParticipant = TransactionParticipant::get(opCtx());

    // Time active should be zero.
    ASSERT_EQ(txnParticipant.getSingleTransactionStatsForTest().getTimeActiveMicros(
                  tickSource, tickSource->getTicks()),
              Microseconds{0});
    txnParticipant.unstashTransactionResources(opCtx(), "insert");
    tickSource->advance(Microseconds(100));

    // Time active should have increased.
    ASSERT_EQ(txnParticipant.getSingleTransactionStatsForTest().getTimeActiveMicros(
                  tickSource, tickSource->getTicks()),
              Microseconds(100));

    tickSource->advance(Microseconds(100));

    // Time active should have increased again.
    ASSERT_EQ(txnParticipant.getSingleTransactionStatsForTest().getTimeActiveMicros(
                  tickSource, tickSource->getTicks()),
              Microseconds(200));

    // The transaction machinery cannot store an empty locker.
    { Lock::GlobalLock lk(opCtx(), MODE_IX, Date_t::now(), Lock::InterruptBehavior::kThrow); }
    txnParticipant.stashTransactionResources(opCtx());

    tickSource->advance(Microseconds(100));

    // The transaction is no longer active, so time active should have stopped increasing.
    ASSERT_EQ(txnParticipant.getSingleTransactionStatsForTest().getTimeActiveMicros(
                  tickSource, tickSource->getTicks()),
              Microseconds(200));
}

TEST_F(TransactionsMetricsTest, TimeActiveMicrosShouldIncreaseUntilCommit) {
    auto tickSource = mockTickSource();

    auto sessionCheckout = checkOutSession();
    auto txnParticipant = TransactionParticipant::get(opCtx());

    // Time active should be zero.
    ASSERT_EQ(txnParticipant.getSingleTransactionStatsForTest().getTimeActiveMicros(
                  tickSource, tickSource->getTicks()),
              Microseconds{0});
    txnParticipant.unstashTransactionResources(opCtx(), "commitTransaction");

    tickSource->advance(Microseconds(100));

    // Time active should have increased.
    ASSERT_EQ(txnParticipant.getSingleTransactionStatsForTest().getTimeActiveMicros(
                  tickSource, tickSource->getTicks()),
              Microseconds{100});

    tickSource->advance(Microseconds(100));

    // Time active should have increased again.
    ASSERT_EQ(txnParticipant.getSingleTransactionStatsForTest().getTimeActiveMicros(
                  tickSource, tickSource->getTicks()),
              Microseconds{200});

    txnParticipant.commitUnpreparedTransaction(opCtx());

    tickSource->advance(Microseconds(100));

    // The transaction is no longer active, so time active should have stopped increasing.
    ASSERT_EQ(txnParticipant.getSingleTransactionStatsForTest().getTimeActiveMicros(
                  tickSource, tickSource->getTicks()),
              Microseconds(200));
}

TEST_F(TransactionsMetricsTest, AdditiveMetricsObjectsShouldBeAddedTogetherUponStash) {
    auto sessionCheckout = checkOutSession();
    auto txnParticipant = TransactionParticipant::get(opCtx());

    // Initialize field values for both AdditiveMetrics objects.
    txnParticipant.getSingleTransactionStatsForTest().getOpDebug()->additiveMetrics.keysExamined =
        1;
    CurOp::get(opCtx())->debug().additiveMetrics.keysExamined = 5;
    txnParticipant.getSingleTransactionStatsForTest().getOpDebug()->additiveMetrics.docsExamined =
        2;
    CurOp::get(opCtx())->debug().additiveMetrics.docsExamined = 0;
    txnParticipant.getSingleTransactionStatsForTest().getOpDebug()->additiveMetrics.nMatched = 3;
    txnParticipant.getSingleTransactionStatsForTest().getOpDebug()->additiveMetrics.nModified = 1;
    CurOp::get(opCtx())->debug().additiveMetrics.nModified = 1;
    CurOp::get(opCtx())->debug().additiveMetrics.ninserted = 4;
    txnParticipant.getSingleTransactionStatsForTest().getOpDebug()->additiveMetrics.keysInserted =
        1;
    CurOp::get(opCtx())->debug().additiveMetrics.keysInserted = 1;
    txnParticipant.getSingleTransactionStatsForTest().getOpDebug()->additiveMetrics.keysDeleted = 0;
    CurOp::get(opCtx())->debug().additiveMetrics.keysDeleted = 0;
    txnParticipant.getSingleTransactionStatsForTest()
        .getOpDebug()
        ->additiveMetrics.prepareReadConflicts.store(5);
    CurOp::get(opCtx())->debug().additiveMetrics.prepareReadConflicts.store(4);

    auto additiveMetricsToCompare =
        txnParticipant.getSingleTransactionStatsForTest().getOpDebug()->additiveMetrics;
    additiveMetricsToCompare.add(CurOp::get(opCtx())->debug().additiveMetrics);

    txnParticipant.unstashTransactionResources(opCtx(), "insert");
    // The transaction machinery cannot store an empty locker.
    { Lock::GlobalLock lk(opCtx(), MODE_IX, Date_t::now(), Lock::InterruptBehavior::kThrow); }
    txnParticipant.stashTransactionResources(opCtx());

    ASSERT(txnParticipant.getSingleTransactionStatsForTest().getOpDebug()->additiveMetrics.equals(
        additiveMetricsToCompare));
}

TEST_F(TransactionsMetricsTest, AdditiveMetricsObjectsShouldBeAddedTogetherUponCommit) {
    auto sessionCheckout = checkOutSession();
    auto txnParticipant = TransactionParticipant::get(opCtx());

    // Initialize field values for both AdditiveMetrics objects.
    txnParticipant.getSingleTransactionStatsForTest().getOpDebug()->additiveMetrics.keysExamined =
        3;
    CurOp::get(opCtx())->debug().additiveMetrics.keysExamined = 2;
    txnParticipant.getSingleTransactionStatsForTest().getOpDebug()->additiveMetrics.docsExamined =
        0;
    CurOp::get(opCtx())->debug().additiveMetrics.docsExamined = 2;
    txnParticipant.getSingleTransactionStatsForTest().getOpDebug()->additiveMetrics.nMatched = 4;
    txnParticipant.getSingleTransactionStatsForTest().getOpDebug()->additiveMetrics.nModified = 5;
    CurOp::get(opCtx())->debug().additiveMetrics.nModified = 1;
    CurOp::get(opCtx())->debug().additiveMetrics.ninserted = 1;
    txnParticipant.getSingleTransactionStatsForTest().getOpDebug()->additiveMetrics.ndeleted = 4;
    CurOp::get(opCtx())->debug().additiveMetrics.ndeleted = 0;
    txnParticipant.getSingleTransactionStatsForTest().getOpDebug()->additiveMetrics.keysInserted =
        1;
    CurOp::get(opCtx())->debug().additiveMetrics.keysInserted = 1;
    txnParticipant.getSingleTransactionStatsForTest()
        .getOpDebug()
        ->additiveMetrics.prepareReadConflicts.store(0);
    CurOp::get(opCtx())->debug().additiveMetrics.prepareReadConflicts.store(0);
    txnParticipant.getSingleTransactionStatsForTest()
        .getOpDebug()
        ->additiveMetrics.writeConflicts.store(6);
    CurOp::get(opCtx())->debug().additiveMetrics.writeConflicts.store(3);

    auto additiveMetricsToCompare =
        txnParticipant.getSingleTransactionStatsForTest().getOpDebug()->additiveMetrics;
    additiveMetricsToCompare.add(CurOp::get(opCtx())->debug().additiveMetrics);

    txnParticipant.unstashTransactionResources(opCtx(), "insert");
    // The transaction machinery cannot store an empty locker.
    { Lock::GlobalLock lk(opCtx(), MODE_IX, Date_t::now(), Lock::InterruptBehavior::kThrow); }
    txnParticipant.commitUnpreparedTransaction(opCtx());

    ASSERT(txnParticipant.getSingleTransactionStatsForTest().getOpDebug()->additiveMetrics.equals(
        additiveMetricsToCompare));
}

TEST_F(TransactionsMetricsTest, AdditiveMetricsObjectsShouldBeAddedTogetherUponAbort) {
    auto sessionCheckout = checkOutSession();
    auto txnParticipant = TransactionParticipant::get(opCtx());

    // Initialize field values for both AdditiveMetrics objects.
    txnParticipant.getSingleTransactionStatsForTest().getOpDebug()->additiveMetrics.keysExamined =
        2;
    CurOp::get(opCtx())->debug().additiveMetrics.keysExamined = 4;
    txnParticipant.getSingleTransactionStatsForTest().getOpDebug()->additiveMetrics.docsExamined =
        1;
    CurOp::get(opCtx())->debug().additiveMetrics.docsExamined = 3;
    txnParticipant.getSingleTransactionStatsForTest().getOpDebug()->additiveMetrics.nMatched = 2;
    txnParticipant.getSingleTransactionStatsForTest().getOpDebug()->additiveMetrics.nModified = 0;
    CurOp::get(opCtx())->debug().additiveMetrics.nModified = 3;
    CurOp::get(opCtx())->debug().additiveMetrics.ndeleted = 5;
    txnParticipant.getSingleTransactionStatsForTest().getOpDebug()->additiveMetrics.keysInserted =
        1;
    CurOp::get(opCtx())->debug().additiveMetrics.keysInserted = 1;
    txnParticipant.getSingleTransactionStatsForTest().getOpDebug()->additiveMetrics.keysDeleted = 6;
    CurOp::get(opCtx())->debug().additiveMetrics.keysDeleted = 0;
    txnParticipant.getSingleTransactionStatsForTest()
        .getOpDebug()
        ->additiveMetrics.writeConflicts.store(3);
    CurOp::get(opCtx())->debug().additiveMetrics.writeConflicts.store(3);

    auto additiveMetricsToCompare =
        txnParticipant.getSingleTransactionStatsForTest().getOpDebug()->additiveMetrics;
    additiveMetricsToCompare.add(CurOp::get(opCtx())->debug().additiveMetrics);

    txnParticipant.unstashTransactionResources(opCtx(), "insert");
    // The transaction machinery cannot store an empty locker.
    { Lock::GlobalLock lk(opCtx(), MODE_IX, Date_t::now(), Lock::InterruptBehavior::kThrow); }
    txnParticipant.abortTransaction(opCtx());

    ASSERT(txnParticipant.getSingleTransactionStatsForTest().getOpDebug()->additiveMetrics.equals(
        additiveMetricsToCompare));
}

TEST_F(TransactionsMetricsTest, TimeInactiveMicrosShouldBeSetUponUnstashAndStash) {
    auto tickSource = mockTickSource();

    auto sessionCheckout = checkOutSession();
    auto txnParticipant = TransactionParticipant::get(opCtx());

    // Time inactive should have increased.
    tickSource->advance(Microseconds(100));
    ASSERT_EQ(txnParticipant.getSingleTransactionStatsForTest().getTimeInactiveMicros(
                  tickSource, tickSource->getTicks()),
              Microseconds{100});

    // Time inactive should have increased again.
    tickSource->advance(Microseconds(100));
    ASSERT_EQ(txnParticipant.getSingleTransactionStatsForTest().getTimeInactiveMicros(
                  tickSource, tickSource->getTicks()),
              Microseconds{200});

    txnParticipant.unstashTransactionResources(opCtx(), "insert");

    tickSource->advance(Microseconds(100));

    // The transaction is currently active, so time inactive should not have increased.
    ASSERT_EQ(txnParticipant.getSingleTransactionStatsForTest().getTimeInactiveMicros(
                  tickSource, tickSource->getTicks()),
              Microseconds{200});

    // The transaction machinery cannot store an empty locker.
    { Lock::GlobalLock lk(opCtx(), MODE_IX, Date_t::now(), Lock::InterruptBehavior::kThrow); }
    txnParticipant.stashTransactionResources(opCtx());

    tickSource->advance(Microseconds(100));

    // The transaction is inactive again, so time inactive should have increased.
    ASSERT_EQ(txnParticipant.getSingleTransactionStatsForTest().getTimeInactiveMicros(
                  tickSource, tickSource->getTicks()),
              Microseconds{300});
}

TEST_F(TransactionsMetricsTest, TimeInactiveMicrosShouldBeSetUponUnstashAndAbort) {
    auto tickSource = mockTickSource();

    auto sessionCheckout = checkOutSession();
    auto txnParticipant = TransactionParticipant::get(opCtx());

    // Time inactive should be greater than or equal to zero.
    ASSERT_EQ(txnParticipant.getSingleTransactionStatsForTest().getTimeInactiveMicros(
                  tickSource, tickSource->getTicks()),
              Microseconds{0});

    tickSource->advance(Microseconds(100));

    // Time inactive should have increased.
    ASSERT_EQ(txnParticipant.getSingleTransactionStatsForTest().getTimeInactiveMicros(
                  tickSource, tickSource->getTicks()),
              Microseconds{100});

    txnParticipant.unstashTransactionResources(opCtx(), "insert");
    txnParticipant.abortTransaction(opCtx());

    ASSERT_EQ(txnParticipant.getSingleTransactionStatsForTest().getTimeInactiveMicros(
                  tickSource, tickSource->getTicks()),
              Microseconds{100});

    tickSource->advance(Microseconds(100));

    // The transaction has aborted, so time inactive should not have increased.
    ASSERT_EQ(txnParticipant.getSingleTransactionStatsForTest().getTimeInactiveMicros(
                  tickSource, tickSource->getTicks()),
              Microseconds{100});
}

TEST_F(TransactionsMetricsTest, TimeInactiveMicrosShouldIncreaseUntilCommit) {
    auto tickSource = mockTickSource();

    auto sessionCheckout = checkOutSession();
    auto txnParticipant = TransactionParticipant::get(opCtx());

    // Time inactive should be greater than or equal to zero.
    ASSERT_EQ(txnParticipant.getSingleTransactionStatsForTest().getTimeInactiveMicros(
                  tickSource, tickSource->getTicks()),
              Microseconds{0});

    tickSource->advance(Microseconds(100));

    // Time inactive should have increased.
    ASSERT_EQ(txnParticipant.getSingleTransactionStatsForTest().getTimeInactiveMicros(
                  tickSource, tickSource->getTicks()),
              Microseconds{100});

    txnParticipant.unstashTransactionResources(opCtx(), "insert");
    // The transaction machinery cannot store an empty locker.
    { Lock::GlobalLock lk(opCtx(), MODE_IX, Date_t::now(), Lock::InterruptBehavior::kThrow); }
    txnParticipant.commitUnpreparedTransaction(opCtx());

    tickSource->advance(Microseconds(100));

    // The transaction has committed, so time inactive should not have increased.
    ASSERT_EQ(txnParticipant.getSingleTransactionStatsForTest().getTimeInactiveMicros(
                  tickSource, tickSource->getTicks()),
              Microseconds{100});
}

TEST_F(TransactionsMetricsTest, ReportStashedResources) {
    auto tickSource = mockTickSource();
    auto startTime = Date_t::now();
    ClockSourceMock{}.reset(startTime);

    const bool autocommit = false;

    ASSERT(shard_role_details::getLocker(opCtx()));
    ASSERT(shard_role_details::getRecoveryUnit(opCtx()));

    auto sessionCheckout = checkOutSession();

    // Create a ClientMetadata object and set it.
    BSONObjBuilder builder;
    ASSERT_OK(ClientMetadata::serializePrivate("driverName",
                                               "driverVersion",
                                               "osType",
                                               "osName",
                                               "osArchitecture",
                                               "osVersion",
                                               "appName",
                                               &builder));
    auto obj = builder.obj();
    auto clientMetadata = ClientMetadata::parse(obj["client"]);
    ClientMetadata::setAndFinalize(opCtx()->getClient(), std::move(clientMetadata.getValue()));

    repl::ReadConcernArgs readConcernArgs;
    ASSERT_OK(
        readConcernArgs.initialize(BSON("find"
                                        << "test" << repl::ReadConcernArgs::kReadConcernFieldName
                                        << BSON(repl::ReadConcernArgs::kLevelFieldName
                                                << "snapshot"))));
    repl::ReadConcernArgs::get(opCtx()) = readConcernArgs;

    // Perform initial unstash which sets up a WriteUnitOfWork.
    auto txnParticipant = TransactionParticipant::get(opCtx());
    txnParticipant.unstashTransactionResources(opCtx(), "find");
    ASSERT(shard_role_details::getWriteUnitOfWork(opCtx()));
    ASSERT(shard_role_details::getLocker(opCtx())->isLocked());

    // Prepare the transaction and extend the duration in the prepared state.
    const auto [prepareTimestamp, namespaces] = txnParticipant.prepareTransaction(opCtx(), {});
    const long preparedDuration = 10;
    tickSource->advance(Microseconds(preparedDuration));

    // Stash resources. The original Locker and RecoveryUnit now belong to the stash.
    txnParticipant.stashTransactionResources(opCtx());
    ASSERT(!shard_role_details::getWriteUnitOfWork(opCtx()));

    // Verify that the Session's report of its own stashed state aligns with our expectations.
    auto stashedState = txnParticipant.reportStashedState(opCtx());
    auto transactionDocument = stashedState.getObjectField("transaction");
    auto parametersDocument = transactionDocument.getObjectField("parameters");

    ASSERT_EQ(stashedState.getField("host").valueStringData().toString(),
              prettyHostNameAndPort(opCtx()->getClient()->getLocalPort()));
    ASSERT_EQ(stashedState.getField("desc").valueStringData().toString(), "inactive transaction");
    ASSERT_BSONOBJ_EQ(stashedState.getField("lsid").Obj(), _sessionId.toBSON());
    ASSERT_EQ(parametersDocument.getField("txnNumber").numberLong(), _txnNumber);
    ASSERT_EQ(parametersDocument.getField("autocommit").boolean(), autocommit);
    ASSERT_BSONELT_EQ(parametersDocument.getField("readConcern"),
                      readConcernArgs.toBSON().getField("readConcern"));
    ASSERT_GTE(transactionDocument.getField("readTimestamp").timestamp(), Timestamp(0, 0));
    ASSERT_EQ(
        dateFromISOString(transactionDocument.getField("startWallClockTime").valueStringData())
            .getValue(),
        startTime);
    ASSERT_EQ(
        dateFromISOString(transactionDocument.getField("expiryTime").valueStringData()).getValue(),
        startTime + Seconds(gTransactionLifetimeLimitSeconds.load()));
    ASSERT_EQ(transactionDocument.getField("timePreparedMicros").numberLong(), preparedDuration);

    ASSERT_EQ(stashedState.getField("client").valueStringData().toString(), "");
    ASSERT_EQ(stashedState.getField("connectionId").numberLong(), 0);
    ASSERT_EQ(stashedState.getField("appName").valueStringData().toString(), "appName");
    ASSERT_BSONOBJ_EQ(stashedState.getField("clientMetadata").Obj(), obj.getField("client").Obj());
    ASSERT_EQ(stashedState.getField("waitingForLock").boolean(), false);
    ASSERT_EQ(stashedState.getField("active").boolean(), false);

    // For the following time metrics, we are only verifying that the transaction sub-document is
    // being constructed correctly with proper types because we have other tests to verify that the
    // values are being tracked correctly.
    ASSERT_GTE(transactionDocument.getField("timeOpenMicros").numberLong(), 0);
    ASSERT_GTE(transactionDocument.getField("timeActiveMicros").numberLong(), 0);
    ASSERT_GTE(transactionDocument.getField("timeInactiveMicros").numberLong(), 0);

    // Unset the read concern on the OperationContext. This is needed to unstash.
    repl::ReadConcernArgs::get(opCtx()) = repl::ReadConcernArgs();

    // Unstash the stashed resources. This restores the original Locker and RecoveryUnit to the
    // OperationContext.
    txnParticipant.unstashTransactionResources(opCtx(), "commitTransaction");
    ASSERT(shard_role_details::getWriteUnitOfWork(opCtx()));

    // With the resources unstashed, verify that the Session reports an empty stashed state.
    ASSERT(txnParticipant.reportStashedState(opCtx()).isEmpty());

    // Commit the transaction. This allows us to release locks.
    txnParticipant.commitPreparedTransaction(opCtx(), prepareTimestamp, {});
}

TEST_F(TransactionsMetricsTest, ReportUnstashedResources) {
    auto tickSource = mockTickSource();
    auto startTime = Date_t::now();
    ClockSourceMock{}.reset(startTime);

    ASSERT(shard_role_details::getLocker(opCtx()));
    ASSERT(shard_role_details::getRecoveryUnit(opCtx()));

    repl::ReadConcernArgs readConcernArgs;
    ASSERT_OK(
        readConcernArgs.initialize(BSON("find"
                                        << "test" << repl::ReadConcernArgs::kReadConcernFieldName
                                        << BSON(repl::ReadConcernArgs::kLevelFieldName
                                                << "snapshot"))));
    repl::ReadConcernArgs::get(opCtx()) = readConcernArgs;

    const auto autocommit = false;
    auto sessionCheckout = checkOutSession();

    // Perform initial unstash which sets up a WriteUnitOfWork.
    auto txnParticipant = TransactionParticipant::get(opCtx());
    txnParticipant.unstashTransactionResources(opCtx(), "find");
    ASSERT(shard_role_details::getWriteUnitOfWork(opCtx()));
    ASSERT(shard_role_details::getLocker(opCtx())->isLocked());

    // Prepare transaction and extend duration in the prepared state.
    txnParticipant.prepareTransaction(opCtx(), {});
    const long prepareDuration = 10;
    tickSource->advance(Microseconds(prepareDuration));

    // Verify that the Session's report of its own unstashed state aligns with our expectations.
    BSONObjBuilder unstashedStateBuilder;
    txnParticipant.reportUnstashedState(opCtx(), &unstashedStateBuilder);
    auto unstashedState = unstashedStateBuilder.obj();
    auto transactionDocument = unstashedState.getObjectField("transaction");
    auto parametersDocument = transactionDocument.getObjectField("parameters");

    ASSERT_EQ(parametersDocument.getField("txnNumber").numberLong(), *opCtx()->getTxnNumber());
    ASSERT_EQ(parametersDocument.getField("autocommit").boolean(), autocommit);
    ASSERT_BSONELT_EQ(parametersDocument.getField("readConcern"),
                      readConcernArgs.toBSON().getField("readConcern"));
    ASSERT_GTE(transactionDocument.getField("readTimestamp").timestamp(), Timestamp(0, 0));
    ASSERT_EQ(
        dateFromISOString(transactionDocument.getField("startWallClockTime").valueStringData())
            .getValue(),
        startTime);
    ASSERT_EQ(
        dateFromISOString(transactionDocument.getField("expiryTime").valueStringData()).getValue(),
        startTime + Seconds(gTransactionLifetimeLimitSeconds.load()));
    ASSERT_EQ(transactionDocument.getField("timePreparedMicros").numberLong(), prepareDuration);

    // For the following time metrics, we are only verifying that the transaction sub-document is
    // being constructed correctly with proper types because we have other tests to verify that
    // the values are being tracked correctly.
    ASSERT_GTE(transactionDocument.getField("timeOpenMicros").numberLong(), 0);
    ASSERT_GTE(transactionDocument.getField("timeActiveMicros").numberLong(), 0);
    ASSERT_GTE(transactionDocument.getField("timeInactiveMicros").numberLong(), 0);

    // Stash resources. The original Locker and RecoveryUnit now belong to the stash.
    txnParticipant.stashTransactionResources(opCtx());
    ASSERT(!shard_role_details::getWriteUnitOfWork(opCtx()));

    // With the resources stashed, verify that the Session reports an empty unstashed state.
    BSONObjBuilder builder;
    txnParticipant.reportUnstashedState(opCtx(), &builder);
    ASSERT(builder.obj().isEmpty());
}

TEST_F(TransactionsMetricsTest, ReportUnstashedResourcesForARetryableWrite) {
    ASSERT(shard_role_details::getLocker(opCtx()));
    ASSERT(shard_role_details::getRecoveryUnit(opCtx()));

    auto clientOwned = getServiceContext()->getService()->makeClient("client");
    AlternativeClientRegion acr(clientOwned);
    auto opCtxHolder = cc().makeOperationContext();
    auto opCtx = opCtxHolder.get();
    opCtx->setLogicalSessionId(_sessionId);
    opCtx->setTxnNumber(_txnNumber);

    auto mongoDSessionCatalog = MongoDSessionCatalog::get(opCtx);
    auto opCtxSession = mongoDSessionCatalog->checkOutSession(opCtx);
    auto txnParticipant = TransactionParticipant::get(opCtx);
    txnParticipant.beginOrContinue(opCtx,
                                   {*opCtx->getTxnNumber()},
                                   boost::none /* autocommit */,
                                   TransactionParticipant::TransactionActions::kNone);
    txnParticipant.unstashTransactionResources(opCtx, "find");

    // Build a BSONObj containing the details which we expect to see reported when we invoke
    // reportUnstashedState. For a retryable write, we should only include the txnNumber.
    BSONObjBuilder reportBuilder;
    BSONObjBuilder transactionBuilder(reportBuilder.subobjStart("transaction"));
    BSONObjBuilder parametersBuilder(transactionBuilder.subobjStart("parameters"));
    parametersBuilder.append("txnNumber", *opCtx->getTxnNumber());
    parametersBuilder.done();
    transactionBuilder.done();

    // Verify that the Session's report of its own unstashed state aligns with our expectations.
    BSONObjBuilder unstashedStateBuilder;
    txnParticipant.reportUnstashedState(opCtx, &unstashedStateBuilder);
    ASSERT_BSONOBJ_EQ(unstashedStateBuilder.obj(), reportBuilder.obj());
}

TEST_F(TransactionsMetricsTest, UseAPIParametersOnOpCtxForARetryableWrite) {
    ASSERT(shard_role_details::getLocker(opCtx()));
    ASSERT(shard_role_details::getRecoveryUnit(opCtx()));

    APIParameters firstAPIParameters = APIParameters();
    firstAPIParameters.setAPIVersion("2");
    firstAPIParameters.setAPIStrict(true);
    firstAPIParameters.setAPIDeprecationErrors(true);
    APIParameters::get(opCtx()) = firstAPIParameters;

    auto mongoDSessionCatalog = MongoDSessionCatalog::get(opCtx());
    auto opCtxSession = mongoDSessionCatalog->checkOutSession(opCtx());
    auto txnParticipant = TransactionParticipant::get(opCtx());
    txnParticipant.beginOrContinue(opCtx(),
                                   {*opCtx()->getTxnNumber()},
                                   boost::none /* autocommit */,
                                   TransactionParticipant::TransactionActions::kNone);

    APIParameters secondAPIParameters = APIParameters();
    secondAPIParameters.setAPIVersion("3");
    APIParameters::get(opCtx()) = secondAPIParameters;

    // 'getAPIParameters()' should return the API parameters decorating opCtx if we are in a
    // retryable write.
    APIParameters storedAPIParameters = txnParticipant.getAPIParameters(opCtx());
    ASSERT_EQ("3", *storedAPIParameters.getAPIVersion());
    ASSERT_FALSE(storedAPIParameters.getAPIStrict().has_value());
    ASSERT_FALSE(storedAPIParameters.getAPIDeprecationErrors().has_value());

    // Stash secondAPIParameters.
    txnParticipant.stashTransactionResources(opCtx());

    APIParameters thirdAPIParameters = APIParameters();
    thirdAPIParameters.setAPIVersion("4");
    APIParameters::get(opCtx()) = thirdAPIParameters;

    // 'getAPIParameters()' should still return API parameters, even if there are stashed API
    // parameters in TxnResources.
    storedAPIParameters = txnParticipant.getAPIParameters(opCtx());
    ASSERT_EQ("4", *storedAPIParameters.getAPIVersion());
    ASSERT_FALSE(storedAPIParameters.getAPIStrict().has_value());
    ASSERT_FALSE(storedAPIParameters.getAPIDeprecationErrors().has_value());
}

namespace {

/*
 * Constructs a ClientMetadata BSONObj with the given application name.
 */
BSONObj constructClientMetadata(StringData appName) {
    BSONObjBuilder builder;
    ASSERT_OK(ClientMetadata::serializePrivate("driverName",
                                               "driverVersion",
                                               "osType",
                                               "osName",
                                               "osArchitecture",
                                               "osVersion",
                                               appName,
                                               &builder));
    return builder.obj();
}
}  // namespace

TEST_F(TransactionsMetricsTest, LastClientInfoShouldUpdateUponStash) {
    // Create a ClientMetadata object and set it.
    auto obj = constructClientMetadata("appName");
    auto clientMetadata = ClientMetadata::parse(obj["client"]);
    ClientMetadata::setAndFinalize(opCtx()->getClient(), std::move(clientMetadata.getValue()));

    auto sessionCheckout = checkOutSession();
    auto txnParticipant = TransactionParticipant::get(opCtx());
    txnParticipant.unstashTransactionResources(opCtx(), "insert");
    // The transaction machinery cannot store an empty locker.
    { Lock::GlobalLock lk(opCtx(), MODE_IX, Date_t::now(), Lock::InterruptBehavior::kThrow); }
    txnParticipant.stashTransactionResources(opCtx());

    // LastClientInfo should have been set.
    auto lastClientInfo = txnParticipant.getSingleTransactionStatsForTest().getLastClientInfo();
    ASSERT_EQ(lastClientInfo.clientHostAndPort, "");
    ASSERT_EQ(lastClientInfo.connectionId, 0);
    ASSERT_EQ(lastClientInfo.appName, "appName");
    ASSERT_BSONOBJ_EQ(lastClientInfo.clientMetadata, obj.getField("client").Obj());

    // Create another ClientMetadata object.
    auto newObj = constructClientMetadata("newAppName");
    auto newClientMetadata = ClientMetadata::parse(newObj["client"]);
    ClientMetadata::setAndFinalize(opCtx()->getClient(), std::move(newClientMetadata.getValue()));

    txnParticipant.unstashTransactionResources(opCtx(), "insert");
    txnParticipant.stashTransactionResources(opCtx());

    // LastClientInfo's clientMetadata should have been updated to the new ClientMetadata object.
    lastClientInfo = txnParticipant.getSingleTransactionStatsForTest().getLastClientInfo();
    ASSERT_EQ(lastClientInfo.appName, "newAppName");
    ASSERT_BSONOBJ_EQ(lastClientInfo.clientMetadata, newObj.getField("client").Obj());
}

TEST_F(TransactionsMetricsTest, LastClientInfoShouldUpdateUponCommit) {
    // Create a ClientMetadata object and set it.
    auto obj = constructClientMetadata("appName");
    auto clientMetadata = ClientMetadata::parse(obj["client"]);
    ClientMetadata::setAndFinalize(opCtx()->getClient(), std::move(clientMetadata.getValue()));

    auto sessionCheckout = checkOutSession();
    auto txnParticipant = TransactionParticipant::get(opCtx());
    txnParticipant.unstashTransactionResources(opCtx(), "insert");
    // The transaction machinery cannot store an empty locker.
    { Lock::GlobalLock lk(opCtx(), MODE_IX, Date_t::now(), Lock::InterruptBehavior::kThrow); }
    txnParticipant.commitUnpreparedTransaction(opCtx());

    // LastClientInfo should have been set.
    auto lastClientInfo = txnParticipant.getSingleTransactionStatsForTest().getLastClientInfo();
    ASSERT_EQ(lastClientInfo.clientHostAndPort, "");
    ASSERT_EQ(lastClientInfo.connectionId, 0);
    ASSERT_EQ(lastClientInfo.appName, "appName");
    ASSERT_BSONOBJ_EQ(lastClientInfo.clientMetadata, obj.getField("client").Obj());
}

TEST_F(TransactionsMetricsTest, LastClientInfoShouldUpdateUponAbort) {
    // Create a ClientMetadata object and set it.
    auto obj = constructClientMetadata("appName");
    auto clientMetadata = ClientMetadata::parse(obj["client"]);
    ClientMetadata::setAndFinalize(opCtx()->getClient(), std::move(clientMetadata.getValue()));

    auto sessionCheckout = checkOutSession();
    auto txnParticipant = TransactionParticipant::get(opCtx());
    txnParticipant.unstashTransactionResources(opCtx(), "insert");
    txnParticipant.abortTransaction(opCtx());

    // LastClientInfo should have been set.
    auto lastClientInfo = txnParticipant.getSingleTransactionStatsForTest().getLastClientInfo();
    ASSERT_EQ(lastClientInfo.clientHostAndPort, "");
    ASSERT_EQ(lastClientInfo.connectionId, 0);
    ASSERT_EQ(lastClientInfo.appName, "appName");
    ASSERT_BSONOBJ_EQ(lastClientInfo.clientMetadata, obj.getField("client").Obj());
}

/*
 * Sets up the additive metrics for Transactions Metrics test.
 */
void setupAdditiveMetrics(const int metricValue, OperationContext* opCtx) {
    CurOp::get(opCtx)->debug().additiveMetrics.keysExamined = metricValue;
    CurOp::get(opCtx)->debug().additiveMetrics.docsExamined = metricValue;
    CurOp::get(opCtx)->debug().additiveMetrics.nMatched = metricValue;
    CurOp::get(opCtx)->debug().additiveMetrics.nModified = metricValue;
    CurOp::get(opCtx)->debug().additiveMetrics.ninserted = metricValue;
    CurOp::get(opCtx)->debug().additiveMetrics.ndeleted = metricValue;
    CurOp::get(opCtx)->debug().additiveMetrics.keysInserted = metricValue;
    CurOp::get(opCtx)->debug().additiveMetrics.keysDeleted = metricValue;
    CurOp::get(opCtx)->debug().additiveMetrics.prepareReadConflicts.store(metricValue);
    CurOp::get(opCtx)->debug().additiveMetrics.writeConflicts.store(metricValue);
}

/*
 * Builds expected parameters info string.
 */
void buildParametersInfoString(StringBuilder* sb,
                               LogicalSessionId sessionId,
                               const TxnNumberAndRetryCounter txnNumberAndRetryCounter,
                               const APIParameters apiParameters,
                               const repl::ReadConcernArgs readConcernArgs,
                               bool autocommitVal) {
    BSONObjBuilder lsidBuilder;
    sessionId.serialize(&lsidBuilder);
    auto autocommitString = autocommitVal ? "true" : "false";
    auto apiVersionString = apiParameters.getAPIVersion().value_or("1");
    auto apiStrictString = apiParameters.getAPIStrict().value_or(false) ? "true" : "false";
    auto apiDeprecationErrorsString =
        apiParameters.getAPIDeprecationErrors().value_or(false) ? "true" : "false";
    (*sb) << "parameters:{ lsid: " << lsidBuilder.done().toString()
          << ", txnNumber: " << txnNumberAndRetryCounter.getTxnNumber()
          << ", txnRetryCounter: " << txnNumberAndRetryCounter.getTxnRetryCounter()
          << ", autocommit: " << autocommitString << ", apiVersion: \"" << apiVersionString
          << "\", apiStrict: " << apiStrictString
          << ", apiDeprecationErrors: " << apiDeprecationErrorsString
          << ", readConcern: " << readConcernArgs.toBSON().getObjectField("readConcern") << " },";
}

/*
 * Builds expected single transaction stats info string.
 */
void buildSingleTransactionStatsString(StringBuilder* sb, const int metricValue) {
    (*sb) << " keysExamined:" << metricValue << " docsExamined:" << metricValue
          << " nMatched:" << metricValue << " nModified:" << metricValue
          << " ninserted:" << metricValue << " ndeleted:" << metricValue
          << " keysInserted:" << metricValue << " keysDeleted:" << metricValue
          << " prepareReadConflicts:" << metricValue << " writeConflicts:" << metricValue;
}

/*
 * Builds the time active and time inactive info string.
 */
void buildTimeActiveInactiveString(StringBuilder* sb,
                                   TransactionParticipant::Participant txnParticipant,
                                   TickSource* tickSource,
                                   TickSource::Tick curTick) {
    // Add time active micros to string.
    (*sb) << " timeActiveMicros:"
          << durationCount<Microseconds>(
                 txnParticipant.getSingleTransactionStatsForTest().getTimeActiveMicros(tickSource,
                                                                                       curTick));

    // Add time inactive micros to string.
    (*sb) << " timeInactiveMicros:"
          << durationCount<Microseconds>(
                 txnParticipant.getSingleTransactionStatsForTest().getTimeInactiveMicros(tickSource,
                                                                                         curTick));
}

/*
 * Builds the total prepared duration info string.
 */
void buildPreparedDurationString(StringBuilder* sb,
                                 TransactionParticipant::Participant txnParticipant,
                                 TickSource* tickSource,
                                 TickSource::Tick curTick) {
    (*sb) << " totalPreparedDurationMicros:"
          << durationCount<Microseconds>(
                 txnParticipant.getSingleTransactionStatsForTest().getPreparedDuration(tickSource,
                                                                                       curTick));
}

/*
 * Builds the entire expected transaction info string and returns it.
 */
std::string buildTransactionInfoString(OperationContext* opCtx,
                                       TransactionParticipant::Participant txnParticipant,
                                       std::string terminationCause,
                                       const LogicalSessionId sessionId,
                                       const TxnNumberAndRetryCounter txnNumberAndRetryCounter,
                                       const int metricValue,
                                       const bool wasPrepared,
                                       bool autocommitVal = false,
                                       boost::optional<repl::OpTime> prepareOpTime = boost::none) {
    // Calling transactionInfoForLog to get the actual transaction info string.
    const auto lockerInfo =
        shard_role_details::getLocker(opCtx)->getLockerInfo(CurOp::get(*opCtx)->getLockStatsBase());
    // Building expected transaction info string.
    StringBuilder parametersInfo;
    // autocommit must be false for a multi statement transaction, so
    // getTransactionInfoForLogForTest should theoretically always print false. In certain unit
    // tests, we compare its output to the output generated in this function.
    //
    // Since we clear the state of a transaction on abort, if getTransactionInfoForLogForTest is
    // called after a transaction is already aborted, it will encounter boost::none for the
    // autocommit value. In that case, it will print out true.
    //
    // In cases where we call getTransactionInfoForLogForTest after aborting a transaction
    // and check if the output matches this function's output, we must explicitly set autocommitVal
    // to true.
    buildParametersInfoString(&parametersInfo,
                              sessionId,
                              txnNumberAndRetryCounter,
                              APIParameters::get(opCtx),
                              repl::ReadConcernArgs::get(opCtx),
                              autocommitVal);

    StringBuilder readTimestampInfo;
    readTimestampInfo
        << " readTimestamp:"
        << txnParticipant.getSingleTransactionStatsForTest().getReadTimestamp().toString() << ",";

    StringBuilder singleTransactionStatsInfo;
    buildSingleTransactionStatsString(&singleTransactionStatsInfo, metricValue);

    auto tickSource = opCtx->getServiceContext()->getTickSource();
    StringBuilder timeActiveAndInactiveInfo;
    buildTimeActiveInactiveString(
        &timeActiveAndInactiveInfo, txnParticipant, tickSource, tickSource->getTicks());

    BSONObjBuilder locks;
    lockerInfo.stats.report(&locks);

    // Puts all the substrings together into one expected info string. The expected info string will
    // look something like this:
    // parameters:{ lsid: { id: UUID("f825288c-100e-49a1-9fd7-b95c108049e6"), uid: BinData(0,
    // E3B0C44298FC1C149AFBF4C8996FB92427AE41E4649B934CA495991B7852B855) }, txnNumber: 1,
    // autocommit: false }, readTimestamp:Timestamp(0, 0), keysExamined:1 docsExamined:1 nMatched:1
    // nModified:1 ninserted:1 ndeleted:1 keysInserted:1 keysDeleted:1
    // prepareReadConflicts:1 writeConflicts:1 terminationCause:committed timeActiveMicros:3
    // timeInactiveMicros:2 numYields:0 locks:{ Global: { acquireCount: { r: 6, w: 4 } }, Database:
    // { acquireCount: { r: 1, w: 1, W: 2 } }, Collection: { acquireCount: { R: 1 } }, oplog: {
    // acquireCount: { W: 1 } } } wasPrepared:1 totalPreparedDurationMicros:10
    // prepareOpTime:<OpTime> 0ms
    StringBuilder expectedTransactionInfo;
    expectedTransactionInfo << parametersInfo.str() << readTimestampInfo.str()
                            << singleTransactionStatsInfo.str()
                            << " terminationCause:" << terminationCause
                            << timeActiveAndInactiveInfo.str() << " numYields:" << 0
                            << " locks:" << locks.done().toString();

    if (auto& storageStats = CurOp::get(opCtx)->debug().storageStats) {
        expectedTransactionInfo << " storage:" << storageStats->toBSON();
    }

    expectedTransactionInfo << " wasPrepared:" << wasPrepared;

    if (wasPrepared) {
        StringBuilder totalPreparedDuration;
        buildPreparedDurationString(
            &totalPreparedDuration, txnParticipant, tickSource, tickSource->getTicks());
        expectedTransactionInfo << totalPreparedDuration.str();
        expectedTransactionInfo << " prepareOpTime:"
                                << (prepareOpTime ? prepareOpTime->toString()
                                                  : txnParticipant.getPrepareOpTime().toString());
    }

    expectedTransactionInfo << ", "
                            << duration_cast<Milliseconds>(
                                   txnParticipant.getSingleTransactionStatsForTest().getDuration(
                                       tickSource, tickSource->getTicks()));
    return expectedTransactionInfo.str();
}


/*
 * Builds expected parameters info BSON.
 */
void buildParametersInfoBSON(BSONObjBuilder* builder,
                             LogicalSessionId sessionId,
                             const TxnNumber txnNum,
                             const repl::ReadConcernArgs readConcernArgs,
                             bool autocommitVal) {
    BSONObjBuilder lsidBuilder;
    sessionId.serialize(&lsidBuilder);
    auto autocommitString = autocommitVal ? "true" : "false";

    BSONObjBuilder params = builder->subobjStart("parameters");
    params.append("lsid", lsidBuilder.obj());
    params.append("txnNumber", txnNum);
    params.append("autocommit", autocommitString);
    readConcernArgs.appendInfo(&params);
}

/*
 * Builds expected single transaction stats info string.
 */
void buildSingleTransactionStatsBSON(BSONObjBuilder* builder, const int metricValue) {
    builder->append("keysExamined", metricValue);
    builder->append("docsExamined", metricValue);
    builder->append("nMatched", metricValue);
    builder->append("nModified", metricValue);
    builder->append("ninserted", metricValue);
    builder->append("ndeleted", metricValue);
    builder->append("keysInserted", metricValue);
    builder->append("keysDeleted", metricValue);
    builder->append("prepareReadConflicts", metricValue);
    builder->append("writeConflicts", metricValue);
}

/*
 * Builds the time active and time inactive info BSON.
 */
void buildTimeActiveInactiveBSON(BSONObjBuilder* builder,
                                 TransactionParticipant::Participant txnParticipant,
                                 TickSource* tickSource,
                                 TickSource::Tick curTick) {
    // Add time active micros to string.
    builder->append("timeActiveMicros",
                    durationCount<Microseconds>(
                        txnParticipant.getSingleTransactionStatsForTest().getTimeActiveMicros(
                            tickSource, curTick)));

    // Add time inactive micros to string.
    builder->append("timeInactiveMicros",
                    durationCount<Microseconds>(
                        txnParticipant.getSingleTransactionStatsForTest().getTimeInactiveMicros(
                            tickSource, curTick)));
}

/*
 * Builds the total prepared duration info BSON.
 */
void buildPreparedDurationBSON(BSONObjBuilder* builder,
                               TransactionParticipant::Participant txnParticipant,
                               TickSource* tickSource,
                               TickSource::Tick curTick) {
    builder->append("totalPreparedDurationMicros",
                    durationCount<Microseconds>(
                        txnParticipant.getSingleTransactionStatsForTest().getPreparedDuration(
                            tickSource, curTick)));
}

/*
 * Builds the entire expected transaction info BSON and returns it.
 *
 * Must be kept in sync with TransactionParticipant::Participant::_transactionInfoForLog.
 */
BSONObj buildTransactionInfoBSON(OperationContext* opCtx,
                                 TransactionParticipant::Participant txnParticipant,
                                 std::string terminationCause,
                                 const LogicalSessionId sessionId,
                                 const TxnNumber txnNum,
                                 const int metricValue,
                                 const bool wasPrepared,
                                 bool autocommitVal = false,
                                 boost::optional<repl::OpTime> prepareOpTime = boost::none) {
    // Calling transactionInfoForLog to get the actual transaction info string.
    const auto lockerInfo =
        shard_role_details::getLocker(opCtx)->getLockerInfo(CurOp::get(*opCtx)->getLockStatsBase());
    // Building expected transaction info string.
    StringBuilder parametersInfo;
    // autocommit must be false for a multi statement transaction, so
    // getTransactionInfoForLogForTest should theoretically always print false. In certain unit
    // tests, we compare its output to the output generated in this function.
    //
    // Since we clear the state of a transaction on abort, if getTransactionInfoForLogForTest is
    // called after a transaction is already aborted, it will encounter boost::none for the
    // autocommit value. In that case, it will print out true.
    //
    // In cases where we call getTransactionInfoForLogForTest after aborting a transaction
    // and check if the output matches this function's output, we must explicitly set autocommitVal
    // to true.

    BSONObjBuilder logLine;
    {
        BSONObjBuilder attrs = logLine.subobjStart("attr");

        buildParametersInfoBSON(
            &attrs, sessionId, txnNum, repl::ReadConcernArgs::get(opCtx), autocommitVal);


        attrs.append(
            "readTimestamp",
            txnParticipant.getSingleTransactionStatsForTest().getReadTimestamp().toString());

        buildSingleTransactionStatsBSON(&attrs, metricValue);

        attrs.append("terminationCause", terminationCause);
        auto tickSource = opCtx->getServiceContext()->getTickSource();
        buildTimeActiveInactiveBSON(&attrs, txnParticipant, tickSource, tickSource->getTicks());

        attrs.append("numYields", 0);

        BSONObjBuilder locks;
        lockerInfo.stats.report(&locks);
        attrs.append("locks", locks.obj());

        attrs.append("wasPrepared", wasPrepared);

        if (wasPrepared) {
            buildPreparedDurationBSON(&attrs, txnParticipant, tickSource, tickSource->getTicks());
            attrs.append("prepareOpTime",
                         (prepareOpTime ? prepareOpTime->toBSON()
                                        : txnParticipant.getPrepareOpTime().toBSON()));
        }

        attrs.append("durationMillis",
                     duration_cast<Milliseconds>(
                         txnParticipant.getSingleTransactionStatsForTest().getDuration(
                             tickSource, tickSource->getTicks()))
                         .count());
    }

    return logLine.obj();
}


TEST_F(TransactionsMetricsTest, TestTransactionInfoForLogAfterCommit) {
    // Initialize SingleTransactionStats AdditiveMetrics objects.
    const int metricValue = 1;
    setupAdditiveMetrics(metricValue, opCtx());

    auto sessionCheckout = checkOutSession();

    APIParameters apiParameters = APIParameters();
    apiParameters.setAPIVersion("2");
    apiParameters.setAPIStrict(true);
    apiParameters.setAPIDeprecationErrors(true);
    APIParameters::get(opCtx()) = apiParameters;

    repl::ReadConcernArgs readConcernArgs;
    ASSERT_OK(
        readConcernArgs.initialize(BSON("find"
                                        << "test" << repl::ReadConcernArgs::kReadConcernFieldName
                                        << BSON(repl::ReadConcernArgs::kLevelFieldName
                                                << "snapshot"))));

    repl::ReadConcernArgs::get(opCtx()) = readConcernArgs;

    auto txnParticipant = TransactionParticipant::get(opCtx());

    txnParticipant.unstashTransactionResources(opCtx(), "commitTransaction");
    txnParticipant.commitUnpreparedTransaction(opCtx());

    const auto lockerInfo = shard_role_details::getLocker(opCtx())->getLockerInfo(boost::none);
    std::string testTransactionInfo = txnParticipant.getTransactionInfoForLogForTest(
        opCtx(), &lockerInfo.stats, true, apiParameters, readConcernArgs);

    std::string expectedTransactionInfo =
        buildTransactionInfoString(opCtx(),
                                   txnParticipant,
                                   "committed",
                                   *opCtx()->getLogicalSessionId(),
                                   {*opCtx()->getTxnNumber(), *opCtx()->getTxnRetryCounter()},
                                   metricValue,
                                   false);

    ASSERT_EQ(testTransactionInfo, expectedTransactionInfo);
}

TEST_F(TransactionsMetricsTest, TestPreparedTransactionInfoForLogAfterCommit) {
    auto tickSource = mockTickSource();

    // Initialize SingleTransactionStats AdditiveMetrics objects.
    const int metricValue = 1;
    setupAdditiveMetrics(metricValue, opCtx());

    auto sessionCheckout = checkOutSession();

    APIParameters apiParameters = APIParameters();
    apiParameters.setAPIVersion("2");
    apiParameters.setAPIStrict(true);
    apiParameters.setAPIDeprecationErrors(true);
    APIParameters::get(opCtx()) = apiParameters;

    repl::ReadConcernArgs readConcernArgs;
    ASSERT_OK(
        readConcernArgs.initialize(BSON("find"
                                        << "test" << repl::ReadConcernArgs::kReadConcernFieldName
                                        << BSON(repl::ReadConcernArgs::kLevelFieldName
                                                << "snapshot"))));

    repl::ReadConcernArgs::get(opCtx()) = readConcernArgs;

    // Prepare the transaction and extend the duration in the prepared state.
    auto txnParticipant = TransactionParticipant::get(opCtx());
    txnParticipant.unstashTransactionResources(opCtx(), "commitTransaction");
    const auto [prepareTimestamp, namespaces] = txnParticipant.prepareTransaction(opCtx(), {});

    tickSource->advance(Microseconds(10));

    txnParticipant.commitPreparedTransaction(opCtx(), prepareTimestamp, {});

    const auto lockerInfo = shard_role_details::getLocker(opCtx())->getLockerInfo(boost::none);
    std::string testTransactionInfo = txnParticipant.getTransactionInfoForLogForTest(
        opCtx(), &lockerInfo.stats, true, apiParameters, readConcernArgs);

    std::string expectedTransactionInfo =
        buildTransactionInfoString(opCtx(),
                                   txnParticipant,
                                   "committed",
                                   *opCtx()->getLogicalSessionId(),
                                   {*opCtx()->getTxnNumber(), *opCtx()->getTxnRetryCounter()},
                                   metricValue,
                                   true);

    ASSERT_EQ(testTransactionInfo, expectedTransactionInfo);
}

TEST_F(TransactionsMetricsTest, TestTransactionInfoForLogAfterAbort) {
    // Initialize SingleTransactionStats AdditiveMetrics objects.
    const int metricValue = 1;
    setupAdditiveMetrics(metricValue, opCtx());

    auto sessionCheckout = checkOutSession();

    APIParameters apiParameters = APIParameters();
    apiParameters.setAPIVersion("2");
    apiParameters.setAPIStrict(true);
    apiParameters.setAPIDeprecationErrors(true);
    APIParameters::get(opCtx()) = apiParameters;

    repl::ReadConcernArgs readConcernArgs;
    ASSERT_OK(
        readConcernArgs.initialize(BSON("find"
                                        << "test" << repl::ReadConcernArgs::kReadConcernFieldName
                                        << BSON(repl::ReadConcernArgs::kLevelFieldName
                                                << "snapshot"))));
    repl::ReadConcernArgs::get(opCtx()) = readConcernArgs;

    auto txnParticipant = TransactionParticipant::get(opCtx());

    txnParticipant.unstashTransactionResources(opCtx(), "abortTransaction");
    txnParticipant.abortTransaction(opCtx());

    const auto lockerInfo = shard_role_details::getLocker(opCtx())->getLockerInfo(boost::none);

    std::string testTransactionInfo = txnParticipant.getTransactionInfoForLogForTest(
        opCtx(), &lockerInfo.stats, false, apiParameters, readConcernArgs);

    std::string expectedTransactionInfo =
        buildTransactionInfoString(opCtx(),
                                   txnParticipant,
                                   "aborted",
                                   *opCtx()->getLogicalSessionId(),
                                   {*opCtx()->getTxnNumber(), *opCtx()->getTxnRetryCounter()},
                                   metricValue,
                                   false,
                                   true);

    ASSERT_EQ(testTransactionInfo, expectedTransactionInfo);
}

TEST_F(TransactionsMetricsTest, TestPreparedTransactionInfoForLogAfterAbort) {
    auto tickSource = mockTickSource();

    // Initialize SingleTransactionStats AdditiveMetrics objects.
    const int metricValue = 1;
    setupAdditiveMetrics(metricValue, opCtx());

    auto sessionCheckout = checkOutSession();

    APIParameters apiParameters = APIParameters();
    apiParameters.setAPIVersion("2");
    apiParameters.setAPIStrict(true);
    apiParameters.setAPIDeprecationErrors(true);
    APIParameters::get(opCtx()) = apiParameters;

    repl::ReadConcernArgs readConcernArgs;
    ASSERT_OK(
        readConcernArgs.initialize(BSON("find"
                                        << "test" << repl::ReadConcernArgs::kReadConcernFieldName
                                        << BSON(repl::ReadConcernArgs::kLevelFieldName
                                                << "snapshot"))));
    repl::ReadConcernArgs::get(opCtx()) = readConcernArgs;

    // Prepare the transaction and extend the duration in the prepared state.
    auto txnParticipant = TransactionParticipant::get(opCtx());
    txnParticipant.unstashTransactionResources(opCtx(), "abortTransaction");
    txnParticipant.prepareTransaction(opCtx(), {});
    tickSource->advance(Microseconds(10));

    txnParticipant.abortTransaction(opCtx());

    const auto lockerInfo = shard_role_details::getLocker(opCtx())->getLockerInfo(boost::none);

    std::string testTransactionInfo = txnParticipant.getTransactionInfoForLogForTest(
        opCtx(), &lockerInfo.stats, false, apiParameters, readConcernArgs);

    std::string expectedTransactionInfo =
        buildTransactionInfoString(opCtx(),
                                   txnParticipant,
                                   "aborted",
                                   *opCtx()->getLogicalSessionId(),
                                   {*opCtx()->getTxnNumber(), *opCtx()->getTxnRetryCounter()},
                                   metricValue,
                                   true,
                                   true);

    ASSERT_EQ(testTransactionInfo, expectedTransactionInfo);
}

DEATH_TEST_F(TransactionsMetricsTest, TestTransactionInfoForLogWithNoLockerInfoStats, "invariant") {
    auto sessionCheckout = checkOutSession();

    APIParameters apiParameters = APIParameters();
    apiParameters.setAPIVersion("2");
    apiParameters.setAPIStrict(true);
    apiParameters.setAPIDeprecationErrors(true);
    APIParameters::get(opCtx()) = apiParameters;

    repl::ReadConcernArgs readConcernArgs;
    ASSERT_OK(
        readConcernArgs.initialize(BSON("find"
                                        << "test" << repl::ReadConcernArgs::kReadConcernFieldName
                                        << BSON(repl::ReadConcernArgs::kLevelFieldName
                                                << "snapshot"))));
    repl::ReadConcernArgs::get(opCtx()) = readConcernArgs;

    auto txnParticipant = TransactionParticipant::get(opCtx());

    const auto lockerInfo = shard_role_details::getLocker(opCtx())->getLockerInfo(boost::none);

    txnParticipant.unstashTransactionResources(opCtx(), "commitTransaction");
    txnParticipant.commitUnpreparedTransaction(opCtx());

    txnParticipant.getTransactionInfoForLogForTest(
        opCtx(), nullptr, true, apiParameters, readConcernArgs);
}

TEST_F(TransactionsMetricsTest, LogTransactionInfoAfterSlowCommit) {
    auto tickSource = mockTickSource();

    auto sessionCheckout = checkOutSession();

    APIParameters apiParameters = APIParameters();
    apiParameters.setAPIVersion("2");
    apiParameters.setAPIStrict(true);
    apiParameters.setAPIDeprecationErrors(true);
    APIParameters::get(opCtx()) = apiParameters;

    repl::ReadConcernArgs readConcernArgs;
    ASSERT_OK(
        readConcernArgs.initialize(BSON("find"
                                        << "test" << repl::ReadConcernArgs::kReadConcernFieldName
                                        << BSON(repl::ReadConcernArgs::kLevelFieldName
                                                << "snapshot"))));
    repl::ReadConcernArgs::get(opCtx()) = readConcernArgs;

    auto txnParticipant = TransactionParticipant::get(opCtx());

    // Initialize SingleTransactionStats AdditiveMetrics objects.
    const int metricValue = 1;
    setupAdditiveMetrics(metricValue, opCtx());

    txnParticipant.unstashTransactionResources(opCtx(), "commitTransaction");
    auto operation = repl::DurableOplogEntry::makeInsertOperation(
        kNss, _uuid, BSON("_id" << 0), BSON("_id" << 0));
    txnParticipant.addTransactionOperation(opCtx(), operation);

    const auto originalSlowMS = serverGlobalParams.slowMS.load();
    const auto originalSampleRate = serverGlobalParams.sampleRate.load();

    serverGlobalParams.slowMS.store(10);
    serverGlobalParams.sampleRate.store(1);

    ON_BLOCK_EXIT([originalSlowMS, originalSampleRate] {
        // serverGlobalParams may have been modified prior to this test, so we set them back to
        // their default values.
        serverGlobalParams.slowMS.store(originalSlowMS);
        serverGlobalParams.sampleRate.store(originalSampleRate);
    });

    tickSource->advance(Microseconds(11 * 1000));

    startCapturingLogMessages();
    txnParticipant.commitUnpreparedTransaction(opCtx());
    stopCapturingLogMessages();

    const auto lockerInfo = shard_role_details::getLocker(opCtx())->getLockerInfo(boost::none);

    BSONObj expected = txnParticipant.getTransactionInfoBSONForLogForTest(
        opCtx(), &lockerInfo.stats, true, apiParameters, readConcernArgs);
    ASSERT_EQUALS(1, countBSONFormatLogLinesIsSubset(expected));
}

TEST_F(TransactionsMetricsTest, LogPreparedTransactionInfoAfterSlowCommit) {
    auto tickSource = mockTickSource();

    auto sessionCheckout = checkOutSession();

    APIParameters apiParameters = APIParameters();
    apiParameters.setAPIVersion("2");
    apiParameters.setAPIStrict(true);
    apiParameters.setAPIDeprecationErrors(true);
    APIParameters::get(opCtx()) = apiParameters;

    repl::ReadConcernArgs readConcernArgs;
    ASSERT_OK(
        readConcernArgs.initialize(BSON("find"
                                        << "test" << repl::ReadConcernArgs::kReadConcernFieldName
                                        << BSON(repl::ReadConcernArgs::kLevelFieldName
                                                << "snapshot"))));
    repl::ReadConcernArgs::get(opCtx()) = readConcernArgs;

    auto txnParticipant = TransactionParticipant::get(opCtx());

    // Initialize SingleTransactionStats AdditiveMetrics objects.
    const int metricValue = 1;
    setupAdditiveMetrics(metricValue, opCtx());

    const auto originalSlowMS = serverGlobalParams.slowMS.load();
    const auto originalSampleRate = serverGlobalParams.sampleRate.load();

    serverGlobalParams.slowMS.store(10);
    serverGlobalParams.sampleRate.store(1);

    // Reset the global parameters to their original values after this test exits.
    ON_BLOCK_EXIT([originalSlowMS, originalSampleRate] {
        serverGlobalParams.slowMS.store(originalSlowMS);
        serverGlobalParams.sampleRate.store(originalSampleRate);
    });

    tickSource->advance(Microseconds(11 * 1000));

    txnParticipant.unstashTransactionResources(opCtx(), "commitTransaction");
    const auto [prepareTimestamp, namespaces] = txnParticipant.prepareTransaction(opCtx(), {});

    startCapturingLogMessages();
    txnParticipant.commitPreparedTransaction(opCtx(), prepareTimestamp, {});
    stopCapturingLogMessages();

    const auto lockerInfo = shard_role_details::getLocker(opCtx())->getLockerInfo(boost::none);

    BSONObj expected = txnParticipant.getTransactionInfoBSONForLogForTest(
        opCtx(), &lockerInfo.stats, true, apiParameters, readConcernArgs);
    ASSERT_EQUALS(1, countBSONFormatLogLinesIsSubset(expected));
}

TEST_F(TransactionsMetricsTest, LogTransactionInfoAfterSlowAbort) {
    auto tickSource = mockTickSource();

    auto sessionCheckout = checkOutSession();

    repl::ReadConcernArgs readConcernArgs;
    ASSERT_OK(
        readConcernArgs.initialize(BSON("find"
                                        << "test" << repl::ReadConcernArgs::kReadConcernFieldName
                                        << BSON(repl::ReadConcernArgs::kLevelFieldName
                                                << "snapshot"))));
    repl::ReadConcernArgs::get(opCtx()) = readConcernArgs;

    auto txnParticipant = TransactionParticipant::get(opCtx());

    // Initialize SingleTransactionStats AdditiveMetrics objects.
    const int metricValue = 1;
    setupAdditiveMetrics(metricValue, opCtx());

    txnParticipant.unstashTransactionResources(opCtx(), "abortTransaction");

    const auto originalSlowMS = serverGlobalParams.slowMS.load();
    const auto originalSampleRate = serverGlobalParams.sampleRate.load();

    serverGlobalParams.slowMS.store(10);
    serverGlobalParams.sampleRate.store(1);

    // Reset the global parameters to their original values after this test exits.
    ON_BLOCK_EXIT([originalSlowMS, originalSampleRate] {
        serverGlobalParams.slowMS.store(originalSlowMS);
        serverGlobalParams.sampleRate.store(originalSampleRate);
    });

    tickSource->advance(Microseconds(11 * 1000));

    startCapturingLogMessages();
    txnParticipant.abortTransaction(opCtx());
    stopCapturingLogMessages();


    auto expectedTransactionInfo = buildTransactionInfoBSON(opCtx(),
                                                            txnParticipant,
                                                            "aborted",
                                                            *opCtx()->getLogicalSessionId(),
                                                            *opCtx()->getTxnNumber(),
                                                            metricValue,
                                                            false);

    ASSERT_EQUALS(1, countBSONFormatLogLinesIsSubset(expectedTransactionInfo));
}

TEST_F(TransactionsMetricsTest, LogPreparedTransactionInfoAfterSlowAbort) {
    auto tickSource = mockTickSource();

    auto sessionCheckout = checkOutSession();

    APIParameters apiParameters = APIParameters();
    apiParameters.setAPIVersion("2");
    apiParameters.setAPIStrict(true);
    apiParameters.setAPIDeprecationErrors(true);
    APIParameters::get(opCtx()) = apiParameters;

    repl::ReadConcernArgs readConcernArgs;
    ASSERT_OK(
        readConcernArgs.initialize(BSON("find"
                                        << "test" << repl::ReadConcernArgs::kReadConcernFieldName
                                        << BSON(repl::ReadConcernArgs::kLevelFieldName
                                                << "snapshot"))));
    repl::ReadConcernArgs::get(opCtx()) = readConcernArgs;

    auto txnParticipant = TransactionParticipant::get(opCtx());

    // Initialize SingleTransactionStats AdditiveMetrics objects.
    const int metricValue = 1;
    setupAdditiveMetrics(metricValue, opCtx());

    txnParticipant.unstashTransactionResources(opCtx(), "abortTransaction");
    txnParticipant.prepareTransaction(opCtx(), {});

    const auto originalSlowMS = serverGlobalParams.slowMS.load();
    const auto originalSampleRate = serverGlobalParams.sampleRate.load();

    serverGlobalParams.slowMS.store(10);
    serverGlobalParams.sampleRate.store(1);

    // Reset the global parameters to their original values after this test exits.
    ON_BLOCK_EXIT([originalSlowMS, originalSampleRate] {
        serverGlobalParams.slowMS.store(originalSlowMS);
        serverGlobalParams.sampleRate.store(originalSampleRate);
    });

    tickSource->advance(Microseconds(11 * 1000));

    auto prepareOpTime = txnParticipant.getPrepareOpTime();

    startCapturingLogMessages();
    txnParticipant.abortTransaction(opCtx());
    stopCapturingLogMessages();


    auto expectedTransactionInfo = buildTransactionInfoBSON(opCtx(),
                                                            txnParticipant,
                                                            "aborted",
                                                            *opCtx()->getLogicalSessionId(),
                                                            *opCtx()->getTxnNumber(),
                                                            metricValue,
                                                            true,
                                                            false,
                                                            prepareOpTime);
    ASSERT_EQUALS(1, countBSONFormatLogLinesIsSubset(expectedTransactionInfo));
}

TEST_F(TransactionsMetricsTest, LogTransactionInfoAfterExceptionInPrepare) {
    auto tickSource = mockTickSource();
    auto sessionCheckout = checkOutSession();

    APIParameters apiParameters = APIParameters();
    apiParameters.setAPIVersion("2");
    apiParameters.setAPIStrict(true);
    apiParameters.setAPIDeprecationErrors(true);
    APIParameters::get(opCtx()) = apiParameters;

    repl::ReadConcernArgs readConcernArgs;
    ASSERT_OK(
        readConcernArgs.initialize(BSON("find"
                                        << "test" << repl::ReadConcernArgs::kReadConcernFieldName
                                        << BSON(repl::ReadConcernArgs::kLevelFieldName
                                                << "snapshot"))));
    repl::ReadConcernArgs::get(opCtx()) = readConcernArgs;

    auto txnParticipant = TransactionParticipant::get(opCtx());

    // Initialize SingleTransactionStats AdditiveMetrics objects.
    const int metricValue = 1;
    setupAdditiveMetrics(metricValue, opCtx());

    txnParticipant.unstashTransactionResources(opCtx(), "prepareTransaction");

    const auto originalSlowMS = serverGlobalParams.slowMS.load();
    const auto originalSampleRate = serverGlobalParams.sampleRate.load();

    serverGlobalParams.slowMS.store(10);
    serverGlobalParams.sampleRate.store(1);

    // Reset the global parameters to their original values after this test exits.
    ON_BLOCK_EXIT([originalSlowMS, originalSampleRate] {
        serverGlobalParams.slowMS.store(originalSlowMS);
        serverGlobalParams.sampleRate.store(originalSampleRate);
    });

    tickSource->advance(Microseconds(11 * 1000));

    _opObserver->postTransactionPrepareThrowsException = true;

    startCapturingLogMessages();
    ASSERT_THROWS_CODE(txnParticipant.prepareTransaction(opCtx(), {}),
                       AssertionException,
                       ErrorCodes::OperationFailed);
    ASSERT_FALSE(_opObserver->transactionPrepared);
    ASSERT(txnParticipant.transactionIsAborted());
    stopCapturingLogMessages();

    auto expectedTransactionInfo = buildTransactionInfoBSON(opCtx(),
                                                            txnParticipant,
                                                            "aborted",
                                                            *opCtx()->getLogicalSessionId(),
                                                            *opCtx()->getTxnNumber(),
                                                            metricValue,
                                                            false);

    ASSERT_EQUALS(1, countBSONFormatLogLinesIsSubset(expectedTransactionInfo));
}

TEST_F(TransactionsMetricsTest, LogTransactionInfoAfterSlowStashedAbort) {
    auto tickSource = mockTickSource();

    auto sessionCheckout = checkOutSession();

    APIParameters apiParameters = APIParameters();
    apiParameters.setAPIVersion("2");
    apiParameters.setAPIStrict(true);
    apiParameters.setAPIDeprecationErrors(true);
    APIParameters::get(opCtx()) = apiParameters;

    repl::ReadConcernArgs readConcernArgs;
    ASSERT_OK(
        readConcernArgs.initialize(BSON("find"
                                        << "test" << repl::ReadConcernArgs::kReadConcernFieldName
                                        << BSON(repl::ReadConcernArgs::kLevelFieldName
                                                << "snapshot"))));
    repl::ReadConcernArgs::get(opCtx()) = readConcernArgs;

    auto txnParticipant = TransactionParticipant::get(opCtx());

    // Initialize SingleTransactionStats AdditiveMetrics objects.
    const int metricValue = 1;
    setupAdditiveMetrics(metricValue, opCtx());

    txnParticipant.unstashTransactionResources(opCtx(), "insert");

    { Lock::GlobalLock lk(opCtx(), MODE_IX, Date_t::now(), Lock::InterruptBehavior::kThrow); }

    txnParticipant.stashTransactionResources(opCtx());
    const auto txnResourceStashLocker = txnParticipant.getTxnResourceStashLockerForTest();
    ASSERT(txnResourceStashLocker);
    const auto lockerInfo = txnResourceStashLocker->getLockerInfo(boost::none);

    const auto originalSlowMS = serverGlobalParams.slowMS.load();
    const auto originalSampleRate = serverGlobalParams.sampleRate.load();

    serverGlobalParams.slowMS.store(10);
    serverGlobalParams.sampleRate.store(1);

    // Reset the global parameters to their original values after this test exits.
    ON_BLOCK_EXIT([originalSlowMS, originalSampleRate] {
        serverGlobalParams.slowMS.store(originalSlowMS);
        serverGlobalParams.sampleRate.store(originalSampleRate);
    });

    tickSource->advance(Microseconds(11 * 1000));

    startCapturingLogMessages();
    txnParticipant.abortTransaction(opCtx());
    stopCapturingLogMessages();

    ASSERT_EQUALS(1, countTextFormatLogLinesContaining("transaction"));
}

TEST_F(TransactionsMetricsTest, LogTransactionInfoZeroSampleRate) {
    auto tickSource = mockTickSource();

    auto sessionCheckout = checkOutSession();

    const auto originalSlowMS = serverGlobalParams.slowMS.load();
    const auto originalSampleRate = serverGlobalParams.sampleRate.load();

    serverGlobalParams.slowMS.store(10);
    // Set the sample rate to 0 to never log this transaction.
    serverGlobalParams.sampleRate.store(0);

    auto txnParticipant = TransactionParticipant::get(opCtx());
    txnParticipant.unstashTransactionResources(opCtx(), "commitTransaction");

    // Reset the global parameters to their original values after this test exits.
    ON_BLOCK_EXIT([originalSlowMS, originalSampleRate] {
        serverGlobalParams.slowMS.store(originalSlowMS);
        serverGlobalParams.sampleRate.store(originalSampleRate);
    });

    tickSource->advance(Microseconds(11 * 1000));

    startCapturingLogMessages();
    txnParticipant.commitUnpreparedTransaction(opCtx());
    stopCapturingLogMessages();

    // Test that the transaction is not logged.
    ASSERT_EQUALS(0, countTextFormatLogLinesContaining("transaction parameters"));
}

TEST_F(TransactionsMetricsTest, LogTransactionInfoVerbosityInfo) {
    auto sessionCheckout = checkOutSession();

    auto txnParticipant = TransactionParticipant::get(opCtx());

    const auto originalSlowMS = serverGlobalParams.slowMS.load();
    const auto originalSampleRate = serverGlobalParams.sampleRate.load();

    // Set a high slow operation threshold to avoid the transaction being logged as slow.
    serverGlobalParams.slowMS.store(10000);
    serverGlobalParams.sampleRate.store(1);

    // Reset the global parameters to their original values after this test exits.
    ON_BLOCK_EXIT([originalSlowMS, originalSampleRate] {
        serverGlobalParams.slowMS.store(originalSlowMS);
        serverGlobalParams.sampleRate.store(originalSampleRate);
    });

    // Set verbosity level of transaction components to info.
    auto severityGuard = unittest::MinimumLoggedSeverityGuard{logv2::LogComponent::kTransaction,
                                                              logv2::LogSeverity::Info()};

    txnParticipant.unstashTransactionResources(opCtx(), "commitTransaction");

    startCapturingLogMessages();
    txnParticipant.commitUnpreparedTransaction(opCtx());
    stopCapturingLogMessages();

    // Test that the transaction is not logged.
    ASSERT_EQUALS(0, countTextFormatLogLinesContaining("transaction parameters"));
}

TEST_F(TransactionsMetricsTest, LogTransactionInfoVerbosityDebug) {
    auto sessionCheckout = checkOutSession();

    auto txnParticipant = TransactionParticipant::get(opCtx());

    // Set verbosity level of transaction components to debug.
    auto severityGuard = unittest::MinimumLoggedSeverityGuard{logv2::LogComponent::kTransaction,
                                                              logv2::LogSeverity::Debug(1)};

    txnParticipant.unstashTransactionResources(opCtx(), "commitTransaction");

    const auto originalSlowMS = serverGlobalParams.slowMS.load();
    const auto originalSampleRate = serverGlobalParams.sampleRate.load();

    // Set a high slow operation threshold to avoid the transaction being logged as slow.
    serverGlobalParams.slowMS.store(10000);
    serverGlobalParams.sampleRate.store(1);

    // Reset the global parameters to their original values after this test exits.
    ON_BLOCK_EXIT([originalSlowMS, originalSampleRate] {
        serverGlobalParams.slowMS.store(originalSlowMS);
        serverGlobalParams.sampleRate.store(originalSampleRate);
    });

    startCapturingLogMessages();
    txnParticipant.commitUnpreparedTransaction(opCtx());
    stopCapturingLogMessages();

    // Test that the transaction is still logged.
    ASSERT_EQUALS(1, countTextFormatLogLinesContaining("transaction"));
}

TEST_F(TxnParticipantTest, RollbackResetsInMemoryStateOfPreparedTransaction) {
    auto sessionCheckout = checkOutSession();

    repl::ReadConcernArgs readConcernArgs;
    ASSERT_OK(
        readConcernArgs.initialize(BSON("find"
                                        << "test" << repl::ReadConcernArgs::kReadConcernFieldName
                                        << BSON(repl::ReadConcernArgs::kLevelFieldName
                                                << "snapshot"))));
    repl::ReadConcernArgs::get(opCtx()) = readConcernArgs;

    auto txnParticipant = TransactionParticipant::get(opCtx());

    // Perform an insert as a part of a transaction so that we have a transaction operation.
    txnParticipant.unstashTransactionResources(opCtx(), "insert");
    auto operation = repl::DurableOplogEntry::makeInsertOperation(
        kNss, _uuid, BSON("_id" << 0), BSON("_id" << 0));
    txnParticipant.addTransactionOperation(opCtx(), operation);
    ASSERT_BSONOBJ_EQ(operation.toBSON(),
                      txnParticipant.getTransactionOperationsForTest()[0].toBSON());

    auto [prepareTimestamp, namespaces] = txnParticipant.prepareTransaction(opCtx(), {});

    ASSERT_FALSE(txnParticipant.transactionIsAborted());

    // Make sure the state of txnParticipant is populated correctly after a prepared transaction.
    ASSERT(txnParticipant.transactionIsPrepared());
    ASSERT_EQ(txnParticipant.getTransactionOperationsForTest().size(), 1U);
    ASSERT_EQ(txnParticipant.getPrepareOpTime().getTimestamp(), prepareTimestamp);
    ASSERT_NE(txnParticipant.getActiveTxnNumberAndRetryCounter().getTxnNumber(),
              kUninitializedTxnNumber);
    ASSERT_EQ(txnParticipant.getActiveTxnNumberAndRetryCounter().getTxnRetryCounter(), 0);


    txnParticipant.abortTransaction(opCtx());
    txnParticipant.invalidate(opCtx());

    // After calling abortTransaction and invalidate, the state of txnParticipant should be
    // invalidated.
    ASSERT_FALSE(txnParticipant.transactionIsPrepared());
    ASSERT_EQ(txnParticipant.getTransactionOperationsForTest().size(), 0U);
    ASSERT_EQ(txnParticipant.getPrepareOpTime().getTimestamp(), Timestamp());
    ASSERT_EQ(txnParticipant.getActiveTxnNumberAndRetryCounter().getTxnNumber(),
              kUninitializedTxnNumber);
    ASSERT_EQ(txnParticipant.getActiveTxnNumberAndRetryCounter().getTxnRetryCounter(),
              kUninitializedTxnRetryCounter);
}

TEST_F(TxnParticipantTest, PrepareTransactionAsSecondarySetsThePrepareOpTime) {
    const auto prepareOpTime = repl::OpTime({3, 2}, 0);
    auto sessionCheckout = checkOutSession();
    auto txnParticipant = TransactionParticipant::get(opCtx());

    txnParticipant.unstashTransactionResources(opCtx(), "commitTransaction");
    const auto [prepareTimestamp, namespaces] =
        txnParticipant.prepareTransaction(opCtx(), prepareOpTime);
    ASSERT(txnParticipant.transactionIsPrepared());
    ASSERT_EQ(prepareTimestamp, prepareOpTime.getTimestamp());
    ASSERT_EQ(txnParticipant.getPrepareOpTime(), prepareOpTime);

    // If _prepareOptime was not set and was null, then commitPreparedTransaction would falsely
    // succeed everytime. We set the commitTimestamp to be less than the prepareTimestamp to make
    // sure this is not the case.
    const auto commitTimestamp =
        Timestamp(prepareTimestamp.getSecs(), prepareTimestamp.getInc() - 1);
    ASSERT_THROWS_CODE(txnParticipant.commitPreparedTransaction(opCtx(), commitTimestamp, {}),
                       AssertionException,
                       ErrorCodes::InvalidOptions);
}

TEST_F(TxnParticipantTest, CommitPreparedTransactionAsSecondarySetsTheFinishOpTime) {
    const auto prepareOpTime = repl::OpTime({3, 2}, 0);
    auto sessionCheckout = checkOutSession();
    auto txnParticipant = TransactionParticipant::get(opCtx());

    // Simulate committing a prepared transaction on a secondary.
    repl::UnreplicatedWritesBlock uwb(opCtx());
    ASSERT(!opCtx()->writesAreReplicated());

    txnParticipant.unstashTransactionResources(opCtx(), "commitTransaction");
    const auto [prepareTimestamp, namespaces] =
        txnParticipant.prepareTransaction(opCtx(), prepareOpTime);
    ASSERT(txnParticipant.transactionIsPrepared());
    ASSERT_EQ(prepareTimestamp, prepareOpTime.getTimestamp());
    ASSERT_EQ(txnParticipant.getPrepareOpTime(), prepareOpTime);

    const auto commitTimestamp =
        Timestamp(prepareTimestamp.getSecs(), prepareTimestamp.getInc() + 1);
    const auto commitOplogEntryOpTime = repl::OpTime({10, 10}, 0);
    txnParticipant.commitPreparedTransaction(opCtx(), commitTimestamp, commitOplogEntryOpTime);
    ASSERT_TRUE(txnParticipant.transactionIsCommitted());
}

DEATH_TEST_F(TxnParticipantTest,
             CommitPreparedTransactionAsSecondaryWithNullCommitOplogEntryOpTimeShouldFail,
             "invariant") {
    const auto prepareOpTime = repl::OpTime({3, 2}, 0);
    auto sessionCheckout = checkOutSession();
    auto txnParticipant = TransactionParticipant::get(opCtx());

    // Simulate committing a prepared transaction on a secondary.
    repl::UnreplicatedWritesBlock uwb(opCtx());
    ASSERT(!opCtx()->writesAreReplicated());

    txnParticipant.unstashTransactionResources(opCtx(), "commitTransaction");
    const auto [prepareTimestamp, namespaces] =
        txnParticipant.prepareTransaction(opCtx(), prepareOpTime);
    ASSERT(txnParticipant.transactionIsPrepared());
    ASSERT_EQ(prepareTimestamp, prepareOpTime.getTimestamp());
    ASSERT_EQ(txnParticipant.getPrepareOpTime(), prepareOpTime);

    const auto commitTimestamp =
        Timestamp(prepareTimestamp.getSecs(), prepareTimestamp.getInc() + 1);
    txnParticipant.commitPreparedTransaction(opCtx(), commitTimestamp, {});
}

DEATH_TEST_F(TxnParticipantTest,
             CommitPreparedTransactionAsPrimaryWithNonNullCommitOplogEntryOpTimeShouldFail,
             "invariant") {
    const auto prepareOpTime = repl::OpTime({3, 2}, 0);
    auto sessionCheckout = checkOutSession();
    auto txnParticipant = TransactionParticipant::get(opCtx());

    txnParticipant.unstashTransactionResources(opCtx(), "commitTransaction");
    const auto [prepareTimestamp, namespaces] =
        txnParticipant.prepareTransaction(opCtx(), prepareOpTime);
    ASSERT(txnParticipant.transactionIsPrepared());
    ASSERT_EQ(prepareTimestamp, prepareOpTime.getTimestamp());
    ASSERT_EQ(txnParticipant.getPrepareOpTime(), prepareOpTime);

    const auto commitTimestamp =
        Timestamp(prepareTimestamp.getSecs(), prepareTimestamp.getInc() + 1);
    const auto commitOplogEntryOpTime = repl::OpTime({10, 10}, 0);
    txnParticipant.commitPreparedTransaction(opCtx(), commitTimestamp, commitOplogEntryOpTime);
}

TEST_F(TxnParticipantTest, AbortTransactionOnSessionCheckoutWithoutRefresh) {
    // This test is intended to mimic the behavior in applyAbortTransaction from
    // transaction_oplog_application.cpp. This is to ensure that when secondaries see an abort oplog
    // entry for a non-existent transaction, the state of the transaction is set to aborted.

    // Simulate aborting a transaction on a secondary.
    repl::UnreplicatedWritesBlock uwb(opCtx());
    ASSERT(!opCtx()->writesAreReplicated());

    const auto txnNumber = *opCtx()->getTxnNumber();

    // MongoDOperationContextSessionWithoutRefresh will begin a new transaction with txnNumber
    // unconditionally since the participant's _activeTxnNumber is kUninitializedTxnNumber at time
    // of session checkout.
    auto mongoDSessionCatalog = MongoDSessionCatalog::get(opCtx());
    auto sessionCheckout = mongoDSessionCatalog->checkOutSessionWithoutRefresh(opCtx());

    auto txnParticipant = TransactionParticipant::get(opCtx());
    ASSERT(txnParticipant.transactionIsOpen());
    ASSERT_EQ(txnParticipant.getActiveTxnNumberAndRetryCounter().getTxnNumber(), txnNumber);

    txnParticipant.unstashTransactionResources(opCtx(), "abortTransaction");
    txnParticipant.abortTransaction(opCtx());
    ASSERT_TRUE(txnParticipant.transactionIsAborted());
}

TEST_F(TxnParticipantTest, ResponseMetadataHasHasReadOnlyFalseIfNothingInProgress) {
    auto mongoDSessionCatalog = MongoDSessionCatalog::get(opCtx());
    auto opCtxSession = mongoDSessionCatalog->checkOutSession(opCtx());
    auto txnParticipant = TransactionParticipant::get(opCtx());
    ASSERT_FALSE(txnParticipant.getResponseMetadata().getBoolField("readOnly"));
}

TEST_F(TxnParticipantTest, ResponseMetadataHasReadOnlyFalseIfInRetryableWrite) {
    auto mongoDSessionCatalog = MongoDSessionCatalog::get(opCtx());
    auto opCtxSession = mongoDSessionCatalog->checkOutSession(opCtx());
    auto txnParticipant = TransactionParticipant::get(opCtx());
    ASSERT_FALSE(txnParticipant.getResponseMetadata().getBoolField("readOnly"));

    // Start a retryable write.
    txnParticipant.beginOrContinue(opCtx(),
                                   {*opCtx()->getTxnNumber()},
                                   boost::none /* autocommit */,
                                   TransactionParticipant::TransactionActions::kNone);
    ASSERT_FALSE(txnParticipant.getResponseMetadata().getBoolField("readOnly"));
}

TEST_F(TxnParticipantTest, ResponseMetadataHasReadOnlyTrueIfInProgressAndOperationsVectorEmpty) {
    auto mongoDSessionCatalog = MongoDSessionCatalog::get(opCtx());
    auto opCtxSession = mongoDSessionCatalog->checkOutSession(opCtx());
    auto txnParticipant = TransactionParticipant::get(opCtx());
    ASSERT_FALSE(txnParticipant.getResponseMetadata().getBoolField("readOnly"));

    // Start a transaction.
    txnParticipant.beginOrContinue(opCtx(),
                                   {*opCtx()->getTxnNumber()},
                                   false /* autocommit */,
                                   TransactionParticipant::TransactionActions::kStart);
    ASSERT_TRUE(txnParticipant.getResponseMetadata().getBoolField("readOnly"));

    txnParticipant.unstashTransactionResources(opCtx(), "find");
    ASSERT_TRUE(txnParticipant.getResponseMetadata().getBoolField("readOnly"));
}

TEST_F(TxnParticipantTest,
       ResponseMetadataHasReadOnlyFalseIfInProgressAndOperationsVectorNotEmpty) {
    auto mongoDSessionCatalog = MongoDSessionCatalog::get(opCtx());
    auto opCtxSession = mongoDSessionCatalog->checkOutSession(opCtx());
    auto txnParticipant = TransactionParticipant::get(opCtx());
    ASSERT_FALSE(txnParticipant.getResponseMetadata().getBoolField("readOnly"));

    // Start a transaction.
    txnParticipant.beginOrContinue(opCtx(),
                                   {*opCtx()->getTxnNumber()},
                                   false /* autocommit */,
                                   TransactionParticipant::TransactionActions::kStart);
    ASSERT_TRUE(txnParticipant.getResponseMetadata().getBoolField("readOnly"));

    txnParticipant.unstashTransactionResources(opCtx(), "insert");
    ASSERT_TRUE(txnParticipant.getResponseMetadata().getBoolField("readOnly"));

    // Simulate an insert.
    auto operation = repl::DurableOplogEntry::makeInsertOperation(
        kNss, _uuid, BSON("_id" << 0), BSON("_id" << 0));
    txnParticipant.addTransactionOperation(opCtx(), operation);
    ASSERT_FALSE(txnParticipant.getResponseMetadata().getBoolField("readOnly"));
}

TEST_F(TxnParticipantTest, ResponseMetadataHasReadOnlyFalseIfAborted) {
    auto mongoDSessionCatalog = MongoDSessionCatalog::get(opCtx());
    auto opCtxSession = mongoDSessionCatalog->checkOutSession(opCtx());
    auto txnParticipant = TransactionParticipant::get(opCtx());
    ASSERT_FALSE(txnParticipant.getResponseMetadata().getBoolField("readOnly"));

    // Start a transaction.
    txnParticipant.beginOrContinue(opCtx(),
                                   {*opCtx()->getTxnNumber()},
                                   false /* autocommit */,
                                   TransactionParticipant::TransactionActions::kStart);
    ASSERT_TRUE(txnParticipant.getResponseMetadata().getBoolField("readOnly"));

    txnParticipant.unstashTransactionResources(opCtx(), "find");
    ASSERT_TRUE(txnParticipant.getResponseMetadata().getBoolField("readOnly"));

    txnParticipant.abortTransaction(opCtx());
    ASSERT_FALSE(txnParticipant.getResponseMetadata().getBoolField("readOnly"));
}

TEST_F(TxnParticipantTest, OldestActiveTransactionTimestamp) {
    auto nss = NamespaceString::kSessionTransactionsTableNamespace;

    auto deleteTxnRecord = [&](unsigned i) {
        Timestamp ts(1, i);
        AutoGetDb autoDb(opCtx(), nss.dbName(), MODE_X);
        auto db = autoDb.ensureDbExists(opCtx());
        ASSERT(db);
        WriteUnitOfWork wuow(opCtx());
        auto coll = CollectionCatalog::get(opCtx())->lookupCollectionByNamespace(opCtx(), nss);
        ASSERT(coll);
        auto cursor = coll->getCursor(opCtx());
        while (auto record = cursor->next()) {
            auto bson = record.value().data.toBson();
            if (bson["state"].String() != "prepared"_sd) {
                continue;
            }

            if (bson["startOpTime"]["ts"].timestamp() == ts) {
                collection_internal::deleteDocument(
                    opCtx(), CollectionPtr(coll), kUninitializedStmtId, record->id, nullptr);
                wuow.commit();
                return;
            }
        }
        FAIL(str::stream() << "No prepared transaction with start timestamp (1, " << i << ")");
    };

    auto oldestActiveTransactionTS = [&]() {
        return TransactionParticipant::getOldestActiveTimestamp(Timestamp()).getValue();
    };

    auto assertOldestActiveTS = [&](boost::optional<unsigned> i) {
        if (i.has_value()) {
            ASSERT_EQ(Timestamp(1, i.value()), oldestActiveTransactionTS());
        } else {
            ASSERT_EQ(boost::none, oldestActiveTransactionTS());
        }
    };

    assertOldestActiveTS(boost::none);
    insertTxnRecord(opCtx(), 1, DurableTxnStateEnum::kPrepared);
    assertOldestActiveTS(1);
    insertTxnRecord(opCtx(), 2, DurableTxnStateEnum::kPrepared);
    assertOldestActiveTS(1);
    deleteTxnRecord(1);
    assertOldestActiveTS(2);
    deleteTxnRecord(2);
    assertOldestActiveTS(boost::none);

    // Add a newer transaction, then an older one, to test that order doesn't matter.
    insertTxnRecord(opCtx(), 4, DurableTxnStateEnum::kPrepared);
    insertTxnRecord(opCtx(), 3, DurableTxnStateEnum::kPrepared);
    assertOldestActiveTS(3);
    deleteTxnRecord(4);
    assertOldestActiveTS(3);
    deleteTxnRecord(3);
    assertOldestActiveTS(boost::none);
};

TEST_F(TxnParticipantTest, OldestActiveTransactionTimestampTimeout) {
    // Block getOldestActiveTimestamp() by locking the config database.
    auto nss = NamespaceString::kSessionTransactionsTableNamespace;
    AutoGetDb autoDb(opCtx(), nss.dbName(), MODE_X);
    auto db = autoDb.ensureDbExists(opCtx());
    ASSERT(db);
    auto statusWith = TransactionParticipant::getOldestActiveTimestamp(Timestamp());
    ASSERT_FALSE(statusWith.isOK());
    ASSERT_TRUE(ErrorCodes::isInterruption(statusWith.getStatus().code()));
};

TEST_F(TxnParticipantTest, ExitPreparePromiseIsFulfilledOnAbortAfterPrepare) {
    auto mongoDSessionCatalog = MongoDSessionCatalog::get(opCtx());
    auto opCtxSession = mongoDSessionCatalog->checkOutSession(opCtx());
    auto txnParticipant = TransactionParticipant::get(opCtx());

    txnParticipant.beginOrContinue(opCtx(),
                                   {*opCtx()->getTxnNumber()},
                                   false /* autocommit */,
                                   TransactionParticipant::TransactionActions::kStart);
    ASSERT_TRUE(txnParticipant.onExitPrepare().isReady());

    txnParticipant.unstashTransactionResources(opCtx(), "find");
    ASSERT_TRUE(txnParticipant.onExitPrepare().isReady());

    const auto prepareOpTime = repl::OpTime({3, 2}, 0);
    txnParticipant.prepareTransaction(opCtx(), prepareOpTime);
    const auto exitPrepareFuture = txnParticipant.onExitPrepare();
    ASSERT_FALSE(exitPrepareFuture.isReady());

    txnParticipant.abortTransaction(opCtx());
    ASSERT_TRUE(exitPrepareFuture.isReady());

    // Once the promise has been fulfilled, new callers of onExitPrepare should immediately be
    // ready.
    ASSERT_TRUE(txnParticipant.onExitPrepare().isReady());

    // abortTransaction is retryable, but does not cause the completion promise to be set again.
    txnParticipant.abortTransaction(opCtx());
}

TEST_F(TxnParticipantTest, ExitPreparePromiseIsFulfilledOnCommitAfterPrepare) {
    auto mongoDSessionCatalog = MongoDSessionCatalog::get(opCtx());
    auto opCtxSession = mongoDSessionCatalog->checkOutSession(opCtx());
    auto txnParticipant = TransactionParticipant::get(opCtx());

    txnParticipant.beginOrContinue(opCtx(),
                                   {*opCtx()->getTxnNumber()},
                                   false /* autocommit */,
                                   TransactionParticipant::TransactionActions::kStart);
    ASSERT_TRUE(txnParticipant.onExitPrepare().isReady());

    txnParticipant.unstashTransactionResources(opCtx(), "find");
    ASSERT_TRUE(txnParticipant.onExitPrepare().isReady());

    const auto prepareOpTime = repl::OpTime({3, 2}, 0);
    const auto [prepareTimestamp, namespaces] =
        txnParticipant.prepareTransaction(opCtx(), prepareOpTime);
    const auto exitPrepareFuture = txnParticipant.onExitPrepare();
    ASSERT_FALSE(exitPrepareFuture.isReady());

    const auto commitTimestamp =
        Timestamp(prepareTimestamp.getSecs(), prepareTimestamp.getInc() + 1);
    txnParticipant.commitPreparedTransaction(opCtx(), commitTimestamp, {});
    ASSERT_TRUE(exitPrepareFuture.isReady());

    // Once the promise has been fulfilled, new callers of onExitPrepare should immediately be
    // ready.
    ASSERT_TRUE(txnParticipant.onExitPrepare().isReady());
}

TEST_F(ShardTxnParticipantTest, CanSpecifyTxnRetryCounterOnShardSvr) {
    auto mongoDSessionCatalog = MongoDSessionCatalog::get(opCtx());
    auto opCtxSession = mongoDSessionCatalog->checkOutSession(opCtx());
    auto txnParticipant = TransactionParticipant::get(opCtx());
    txnParticipant.beginOrContinue(opCtx(),
                                   {*opCtx()->getTxnNumber(), 0},
                                   false /* autocommit */,
                                   TransactionParticipant::TransactionActions::kStart);
}

TEST_F(ConfigTxnParticipantTest, CanSpecifyTxnRetryCounterOnConfigSvr) {
    auto mongoDSessionCatalog = MongoDSessionCatalog::get(opCtx());
    auto opCtxSession = mongoDSessionCatalog->checkOutSession(opCtx());
    auto txnParticipant = TransactionParticipant::get(opCtx());
    txnParticipant.beginOrContinue(opCtx(),
                                   {*opCtx()->getTxnNumber(), 0},
                                   false /* autocommit */,
                                   TransactionParticipant::TransactionActions::kStart);
}

TEST_F(TxnParticipantTest, CanOnlySpecifyTxnRetryCounterInShardedClusters) {
    auto mongoDSessionCatalog = MongoDSessionCatalog::get(opCtx());
    auto opCtxSession = mongoDSessionCatalog->checkOutSession(opCtx());
    auto txnParticipant = TransactionParticipant::get(opCtx());
    ASSERT_THROWS_CODE(
        txnParticipant.beginOrContinue(opCtx(),
                                       {*opCtx()->getTxnNumber(), 0},
                                       false /* autocommit */,
                                       TransactionParticipant::TransactionActions::kStart),
        AssertionException,
        ErrorCodes::InvalidOptions);
}

DEATH_TEST_F(ShardTxnParticipantTest,
             CannotSpecifyNegativeTxnRetryCounter,
             "Cannot specify a negative txnRetryCounter") {
    auto mongoDSessionCatalog = MongoDSessionCatalog::get(opCtx());
    auto opCtxSession = mongoDSessionCatalog->checkOutSession(opCtx());
    auto txnParticipant = TransactionParticipant::get(opCtx());
    txnParticipant.beginOrContinue(opCtx(),
                                   {*opCtx()->getTxnNumber(), -1},
                                   false /* autocommit */,
                                   TransactionParticipant::TransactionActions::kStart);
}

DEATH_TEST_F(ShardTxnParticipantTest,
             CannotSpecifyTxnRetryCounterForRetryableWrite,
             "Cannot specify a txnRetryCounter for retryable write") {
    auto mongoDSessionCatalog = MongoDSessionCatalog::get(opCtx());
    auto opCtxSession = mongoDSessionCatalog->checkOutSession(opCtx());
    auto txnParticipant = TransactionParticipant::get(opCtx());
    ASSERT_THROWS_CODE(
        txnParticipant.beginOrContinue(opCtx(),
                                       {*opCtx()->getTxnNumber(), 0},
                                       boost::none /* autocommit */,
                                       TransactionParticipant::TransactionActions::kNone),
        AssertionException,
        ErrorCodes::InvalidOptions);
}

TEST_F(ShardTxnParticipantTest,
       CanRestartInProgressTransactionUsingTxnRetryCounterGreaterThanLastUsed) {
    auto sessionCheckout = checkOutSession();
    auto txnParticipant = TransactionParticipant::get(opCtx());
    ASSERT_TRUE(txnParticipant.transactionIsInProgress());
    ASSERT_EQ(txnParticipant.getActiveTxnNumberAndRetryCounter().getTxnRetryCounter(), 0);

    txnParticipant.beginOrContinue(opCtx(),
                                   {*opCtx()->getTxnNumber(), 1},
                                   false /* autocommit */,
                                   TransactionParticipant::TransactionActions::kStart);
    ASSERT_TRUE(txnParticipant.transactionIsInProgress());
}

TEST_F(ShardTxnParticipantTest,
       CanRestartAbortedUnpreparedTransactionUsingTxnRetryCounterGreaterThanLastUsed) {
    auto sessionCheckout = checkOutSession();
    auto txnParticipant = TransactionParticipant::get(opCtx());
    ASSERT(txnParticipant.transactionIsInProgress());
    ASSERT_EQ(txnParticipant.getActiveTxnNumberAndRetryCounter().getTxnRetryCounter(),
              0 /* txnRetryCounter */);

    txnParticipant.abortTransaction(opCtx());
    ASSERT(txnParticipant.transactionIsAborted());

    txnParticipant.beginOrContinue(opCtx(),
                                   {*opCtx()->getTxnNumber(), 1},
                                   false /* autocommit */,
                                   TransactionParticipant::TransactionActions::kStart);
    ASSERT(txnParticipant.transactionIsInProgress());
    ASSERT_EQ(txnParticipant.getActiveTxnNumberAndRetryCounter().getTxnRetryCounter(), 1);
}

TEST_F(ShardTxnParticipantTest,
       CanRestartAbortedPreparedTransactionUsingTxnRetryCounterGreaterThanLastUsed) {
    auto sessionCheckout = checkOutSession();
    auto txnParticipant = TransactionParticipant::get(opCtx());
    ASSERT(txnParticipant.transactionIsInProgress());
    txnParticipant.unstashTransactionResources(opCtx(), "commitTransaction");
    txnParticipant.prepareTransaction(opCtx(), {});
    txnParticipant.abortTransaction(opCtx());
    ASSERT(txnParticipant.transactionIsAborted());
    ASSERT_EQ(txnParticipant.getActiveTxnNumberAndRetryCounter().getTxnRetryCounter(), 0);

    txnParticipant.beginOrContinue(opCtx(),
                                   {*opCtx()->getTxnNumber(), 1},
                                   false /* autocommit */,
                                   TransactionParticipant::TransactionActions::kStart);
    ASSERT(txnParticipant.transactionIsInProgress());
    ASSERT_EQ(txnParticipant.getActiveTxnNumberAndRetryCounter().getTxnRetryCounter(), 1);
}

TEST_F(ShardTxnParticipantTest,
       CannotRestartCommittedUnpreparedTransactionUsingTxnRetryCounterGreaterThanLastUsed) {
    auto sessionCheckout = checkOutSession();
    auto txnParticipant = TransactionParticipant::get(opCtx());
    ASSERT(txnParticipant.transactionIsInProgress());
    txnParticipant.unstashTransactionResources(opCtx(), "commitTransaction");
    txnParticipant.commitUnpreparedTransaction(opCtx());
    ASSERT(txnParticipant.transactionIsCommitted());
    ASSERT_EQ(txnParticipant.getActiveTxnNumberAndRetryCounter().getTxnRetryCounter(), 0);

    ASSERT_THROWS_CODE(
        txnParticipant.beginOrContinue(opCtx(),
                                       {*opCtx()->getTxnNumber(), 1},
                                       false /* autocommit */,
                                       TransactionParticipant::TransactionActions::kStart),
        AssertionException,
        ErrorCodes::IllegalOperation);
    ASSERT(txnParticipant.transactionIsCommitted());
    ASSERT_EQ(txnParticipant.getActiveTxnNumberAndRetryCounter().getTxnRetryCounter(), 0);
}

TEST_F(ShardTxnParticipantTest,
       CannotRestartCommittedPreparedTransactionUsingTxnRetryCounterGreaterThanLastUsed) {
    auto sessionCheckout = checkOutSession();
    auto txnParticipant = TransactionParticipant::get(opCtx());
    ASSERT(txnParticipant.transactionIsInProgress());
    txnParticipant.unstashTransactionResources(opCtx(), "commitTransaction");
    const auto [prepareTimestamp, namespaces] = txnParticipant.prepareTransaction(opCtx(), {});
    const auto commitTS = Timestamp(prepareTimestamp.getSecs(), prepareTimestamp.getInc() + 1);
    txnParticipant.commitPreparedTransaction(opCtx(), commitTS, {});
    ASSERT_TRUE(txnParticipant.transactionIsCommitted());
    ASSERT_EQ(txnParticipant.getActiveTxnNumberAndRetryCounter().getTxnRetryCounter(), 0);

    ASSERT_THROWS_CODE(
        txnParticipant.beginOrContinue(opCtx(),
                                       {*opCtx()->getTxnNumber(), 1},
                                       false /* autocommit */,
                                       TransactionParticipant::TransactionActions::kStart),
        AssertionException,
        ErrorCodes::IllegalOperation);
    ASSERT(txnParticipant.transactionIsCommitted());
    ASSERT_EQ(txnParticipant.getActiveTxnNumberAndRetryCounter().getTxnRetryCounter(), 0);
}

TEST_F(ShardTxnParticipantTest,
       CannotRestartPreparedTransactionUsingTxnRetryCounterGreaterThanLastUsed) {
    auto sessionCheckout = checkOutSession();
    auto txnParticipant = TransactionParticipant::get(opCtx());
    ASSERT(txnParticipant.transactionIsInProgress());
    txnParticipant.unstashTransactionResources(opCtx(), "commitTransaction");
    txnParticipant.prepareTransaction(opCtx(), {});
    ASSERT_TRUE(txnParticipant.transactionIsPrepared());
    ASSERT_EQ(txnParticipant.getActiveTxnNumberAndRetryCounter().getTxnRetryCounter(), 0);

    ASSERT_THROWS_CODE(
        txnParticipant.beginOrContinue(opCtx(),
                                       {*opCtx()->getTxnNumber(), 1},
                                       false /* autocommit */,
                                       TransactionParticipant::TransactionActions::kStart),
        AssertionException,
        ErrorCodes::IllegalOperation);
    ASSERT(txnParticipant.transactionIsPrepared());
    ASSERT_EQ(txnParticipant.getActiveTxnNumberAndRetryCounter().getTxnRetryCounter(), 0);
}

TEST_F(ShardTxnParticipantTest, CannotRestartTransactionUsingTxnRetryCounterLessThanLastUsed) {
    auto sessionCheckout = checkOutSession();
    auto txnParticipant = TransactionParticipant::get(opCtx());
    ASSERT_TRUE(txnParticipant.transactionIsInProgress());

    txnParticipant.beginOrContinue(opCtx(),
                                   {*opCtx()->getTxnNumber(), 1},
                                   false /* autocommit */,
                                   TransactionParticipant::TransactionActions::kStart);
    ASSERT_EQ(txnParticipant.getActiveTxnNumberAndRetryCounter().getTxnRetryCounter(), 1);
    ASSERT_THROWS_CODE(
        txnParticipant.beginOrContinue(opCtx(),
                                       {*opCtx()->getTxnNumber(), 0},
                                       false /* autocommit */,
                                       TransactionParticipant::TransactionActions::kStart),
        AssertionException,
        ErrorCodes::TxnRetryCounterTooOld);
    try {
        txnParticipant.beginOrContinue(opCtx(),
                                       {*opCtx()->getTxnNumber(), 0},
                                       false /* autocommit */,
                                       TransactionParticipant::TransactionActions::kStart);
        ASSERT(false);
    } catch (const TxnRetryCounterTooOldException& ex) {
        auto info = ex.extraInfo<TxnRetryCounterTooOldInfo>();
        ASSERT_EQ(info->getTxnRetryCounter(), 1);
    }
    ASSERT_TRUE(txnParticipant.transactionIsInProgress());
    ASSERT_EQ(txnParticipant.getActiveTxnNumberAndRetryCounter().getTxnRetryCounter(), 1);
}

TEST_F(ShardTxnParticipantTest, CanContinueTransactionUsingTxnRetryCounterEqualToLastUsed) {
    auto sessionCheckout = checkOutSession();
    auto txnParticipant = TransactionParticipant::get(opCtx());
    ASSERT_TRUE(txnParticipant.transactionIsInProgress());
    ASSERT_EQ(txnParticipant.getActiveTxnNumberAndRetryCounter().getTxnRetryCounter(), 0);
    txnParticipant.unstashTransactionResources(opCtx(), "insert");
    txnParticipant.stashTransactionResources(opCtx());

    txnParticipant.beginOrContinue(opCtx(),
                                   {*opCtx()->getTxnNumber(), 0},
                                   false /* autocommit */,
                                   TransactionParticipant::TransactionActions::kContinue);
    ASSERT_TRUE(txnParticipant.transactionIsInProgress());
    ASSERT_EQ(txnParticipant.getActiveTxnNumberAndRetryCounter().getTxnRetryCounter(), 0);
}

TEST_F(ShardTxnParticipantTest, CannotContinueTransactionUsingTxnRetryCounterGreaterThanLastUsed) {
    auto sessionCheckout = checkOutSession();
    auto txnParticipant = TransactionParticipant::get(opCtx());
    ASSERT_TRUE(txnParticipant.transactionIsInProgress());
    ASSERT_EQ(txnParticipant.getActiveTxnNumberAndRetryCounter().getTxnRetryCounter(), 0);
    txnParticipant.unstashTransactionResources(opCtx(), "insert");
    txnParticipant.stashTransactionResources(opCtx());

    ASSERT_THROWS_CODE(
        txnParticipant.beginOrContinue(opCtx(),
                                       {*opCtx()->getTxnNumber(), 1},
                                       false /* autocommit */,
                                       TransactionParticipant::TransactionActions::kContinue),
        AssertionException,
        ErrorCodes::IllegalOperation);
    ASSERT_TRUE(txnParticipant.transactionIsInProgress());
    ASSERT_EQ(txnParticipant.getActiveTxnNumberAndRetryCounter().getTxnRetryCounter(), 0);
}

TEST_F(ShardTxnParticipantTest, CannotContinueTransactionUsingTxnRetryCounterLessThanLastUsed) {
    auto sessionCheckout = checkOutSession();
    auto txnParticipant = TransactionParticipant::get(opCtx());
    ASSERT_TRUE(txnParticipant.transactionIsInProgress());

    txnParticipant.beginOrContinue(opCtx(),
                                   {*opCtx()->getTxnNumber(), 1},
                                   false /* autocommit */,
                                   TransactionParticipant::TransactionActions::kStart);
    ASSERT_EQ(txnParticipant.getActiveTxnNumberAndRetryCounter().getTxnRetryCounter(), 1);

    txnParticipant.unstashTransactionResources(opCtx(), "insert");
    txnParticipant.stashTransactionResources(opCtx());
    ASSERT_THROWS_CODE(
        txnParticipant.beginOrContinue(opCtx(),
                                       {*opCtx()->getTxnNumber(), 0},
                                       false /* autocommit */,
                                       TransactionParticipant::TransactionActions::kContinue),
        AssertionException,
        ErrorCodes::TxnRetryCounterTooOld);
    try {
        txnParticipant.beginOrContinue(opCtx(),
                                       {*opCtx()->getTxnNumber(), 0},
                                       false /* autocommit */,
                                       TransactionParticipant::TransactionActions::kStart);
        ASSERT(false);
    } catch (const TxnRetryCounterTooOldException& ex) {
        auto info = ex.extraInfo<TxnRetryCounterTooOldInfo>();
        ASSERT_EQ(info->getTxnRetryCounter(), 1);
    }
    ASSERT_TRUE(txnParticipant.transactionIsInProgress());
    ASSERT_EQ(txnParticipant.getActiveTxnNumberAndRetryCounter().getTxnRetryCounter(), 1);
}

TEST_F(ShardTxnParticipantTest,
       CannotRetryInProgressTransactionForRetryableWrite_OriginalTransactionForRetryableWrite) {
    opCtx()->setLogicalSessionId(makeLogicalSessionIdWithTxnNumberAndUUIDForTest());
    auto sessionCheckout = checkOutSession();

    auto txnParticipant = TransactionParticipant::get(opCtx());
    ASSERT_TRUE(txnParticipant.transactionIsInProgress());

    ASSERT_THROWS_CODE(
        txnParticipant.beginOrContinue(opCtx(),
                                       {*opCtx()->getTxnNumber(), 0},
                                       false /* autocommit */,
                                       TransactionParticipant::TransactionActions::kStart),
        AssertionException,
        50911);
    ASSERT_TRUE(txnParticipant.transactionIsInProgress());
}

TEST_F(ShardTxnParticipantTest, CannotRetryInProgressRetryableTxn_ConflictingRetryableTxn) {
    const auto parentLsid = makeLogicalSessionIdForTest();
    const auto parentTxnNumber = *opCtx()->getTxnNumber();

    opCtx()->setLogicalSessionId(
        makeLogicalSessionIdWithTxnNumberAndUUIDForTest(parentLsid, parentTxnNumber));
    {
        auto sessionCheckout = checkOutSession();
        auto txnParticipant = TransactionParticipant::get(opCtx());
        ASSERT_TRUE(txnParticipant.transactionIsInProgress());
    }

    // The first conflicting transaction should abort the active one.
    const auto firstConflictingLsid =
        makeLogicalSessionIdWithTxnNumberAndUUIDForTest(parentLsid, parentTxnNumber);
    runFunctionFromDifferentOpCtx([firstConflictingLsid](OperationContext* newOpCtx) {
        newOpCtx->setLogicalSessionId(firstConflictingLsid);
        newOpCtx->setTxnNumber(0);
        newOpCtx->setInMultiDocumentTransaction();

        auto mongoDSessionCatalog = MongoDSessionCatalog::get(newOpCtx);
        auto ocs = mongoDSessionCatalog->checkOutSession(newOpCtx);
        auto txnParticipant = TransactionParticipant::get(newOpCtx);
        txnParticipant.beginOrContinue(newOpCtx,
                                       {0},
                                       false /* autocommit */,
                                       TransactionParticipant::TransactionActions::kStart);
        ASSERT_TRUE(txnParticipant.transactionIsInProgress());
        txnParticipant.unstashTransactionResources(newOpCtx, "insert");
        txnParticipant.stashTransactionResources(newOpCtx);
    });

    // Continuing the interrupted transaction should throw without aborting the new active
    // transaction.
    {
        ASSERT_THROWS_CODE(checkOutSession(TransactionParticipant::TransactionActions::kContinue),
                           AssertionException,
                           ErrorCodes::RetryableTransactionInProgress);
    }

    // A second conflicting transaction should throw and not abort the active one.
    runFunctionFromDifferentOpCtx([parentLsid, parentTxnNumber](OperationContext* newOpCtx) {
        newOpCtx->setLogicalSessionId(
            makeLogicalSessionIdWithTxnNumberAndUUIDForTest(parentLsid, parentTxnNumber));
        newOpCtx->setTxnNumber(0);
        newOpCtx->setInMultiDocumentTransaction();

        auto mongoDSessionCatalog = MongoDSessionCatalog::get(newOpCtx);
        auto ocs = mongoDSessionCatalog->checkOutSession(newOpCtx);
        auto txnParticipant = TransactionParticipant::get(newOpCtx);
        ASSERT_THROWS_CODE(
            txnParticipant.beginOrContinue(newOpCtx,
                                           {0},
                                           false /* autocommit */,
                                           TransactionParticipant::TransactionActions::kStart),
            AssertionException,
            ErrorCodes::RetryableTransactionInProgress);
    });

    // Verify the first conflicting txn is still open.
    runFunctionFromDifferentOpCtx([firstConflictingLsid](OperationContext* newOpCtx) {
        newOpCtx->setLogicalSessionId(firstConflictingLsid);
        newOpCtx->setTxnNumber(0);
        newOpCtx->setInMultiDocumentTransaction();

        auto mongoDSessionCatalog = MongoDSessionCatalog::get(newOpCtx);
        auto ocs = mongoDSessionCatalog->checkOutSession(newOpCtx);
        auto txnParticipant = TransactionParticipant::get(newOpCtx);
        txnParticipant.beginOrContinue(newOpCtx,
                                       {0},
                                       false /* autocommit */,
                                       TransactionParticipant::TransactionActions::kContinue);
        txnParticipant.unstashTransactionResources(newOpCtx, "insert");
        ASSERT(txnParticipant.transactionIsInProgress());
    });
}

TEST_F(ShardTxnParticipantTest, CannotRetryInProgressRetryableTxn_ConflictingRetryableWrite) {
    const auto parentLsid = makeLogicalSessionIdForTest();
    const auto parentTxnNumber = *opCtx()->getTxnNumber();

    opCtx()->setLogicalSessionId(
        makeLogicalSessionIdWithTxnNumberAndUUIDForTest(parentLsid, parentTxnNumber));
    {
        auto sessionCheckout = checkOutSession();
        auto txnParticipant = TransactionParticipant::get(opCtx());
        ASSERT_TRUE(txnParticipant.transactionIsInProgress());
    }

    //
    // The first conflicting retryable write should abort a conflicting retryable transaction.
    //
    runFunctionFromDifferentOpCtx([parentLsid, parentTxnNumber](OperationContext* newOpCtx) {
        newOpCtx->setLogicalSessionId(parentLsid);
        newOpCtx->setTxnNumber(parentTxnNumber);

        // Shouldn't throw.
        auto mongoDSessionCatalog = MongoDSessionCatalog::get(newOpCtx);
        auto ocs = mongoDSessionCatalog->checkOutSession(newOpCtx);
        auto txnParticipant = TransactionParticipant::get(newOpCtx);
        txnParticipant.beginOrContinue(newOpCtx,
                                       {parentTxnNumber},
                                       boost::none /* autocommit */,
                                       TransactionParticipant::TransactionActions::kNone);
    });

    // Continuing the interrupted transaction should throw because it was aborted. Note this does
    // not throw RetryableTransactionInProgress because the retryable write that aborted the
    // transaction completed.
    {
        auto sessionCheckout =
            checkOutSession(TransactionParticipant::TransactionActions::kContinue);
        auto txnParticipant = TransactionParticipant::get(opCtx());
        ASSERT_THROWS_CODE(txnParticipant.unstashTransactionResources(opCtx(), "insert"),
                           AssertionException,
                           ErrorCodes::NoSuchTransaction);
    }

    //
    // The second conflicting retryable write should throw and not abort a conflicting retryable
    // transaction.
    //
    {
        auto sessionCheckout = checkOutSession();
        auto txnParticipant = TransactionParticipant::get(opCtx());
        ASSERT_TRUE(txnParticipant.transactionIsInProgress());
        txnParticipant.unstashTransactionResources(opCtx(), "insert");
        txnParticipant.stashTransactionResources(opCtx());
    }

    runFunctionFromDifferentOpCtx([parentLsid, parentTxnNumber](OperationContext* newOpCtx) {
        newOpCtx->setLogicalSessionId(parentLsid);
        newOpCtx->setTxnNumber(parentTxnNumber);

        auto mongoDSessionCatalog = MongoDSessionCatalog::get(newOpCtx);
        auto ocs = mongoDSessionCatalog->checkOutSession(newOpCtx);
        auto txnParticipant = TransactionParticipant::get(newOpCtx);
        ASSERT_THROWS_CODE(
            txnParticipant.beginOrContinue(newOpCtx,
                                           {parentTxnNumber},
                                           boost::none /* autocommit */,
                                           TransactionParticipant::TransactionActions::kNone),
            AssertionException,
            ErrorCodes::RetryableTransactionInProgress);
    });

    {
        auto sessionCheckout =
            checkOutSession(TransactionParticipant::TransactionActions::kContinue);
        auto txnParticipant = TransactionParticipant::get(opCtx());
        txnParticipant.beginOrContinue(opCtx(),
                                       {parentTxnNumber},
                                       false /* autocommit */,
                                       TransactionParticipant::TransactionActions::kContinue);
        txnParticipant.unstashTransactionResources(opCtx(), "insert");
        ASSERT_TRUE(txnParticipant.transactionIsInProgress());
    }
}

TEST_F(ShardTxnParticipantTest, RetryableTransactionInProgressCounterResetsUponNewTxnNumber) {
    const auto parentLsid = makeLogicalSessionIdForTest();
    auto parentTxnNumber = *opCtx()->getTxnNumber();

    opCtx()->setLogicalSessionId(
        makeLogicalSessionIdWithTxnNumberAndUUIDForTest(parentLsid, parentTxnNumber));
    {
        auto sessionCheckout = checkOutSession();
        auto txnParticipant = TransactionParticipant::get(opCtx());
        ASSERT_TRUE(txnParticipant.transactionIsInProgress());
    }

    // The first conflicting transaction should abort the active one.
    const auto firstConflictingLsid =
        makeLogicalSessionIdWithTxnNumberAndUUIDForTest(parentLsid, parentTxnNumber);
    runFunctionFromDifferentOpCtx([firstConflictingLsid](OperationContext* newOpCtx) {
        newOpCtx->setLogicalSessionId(firstConflictingLsid);
        newOpCtx->setTxnNumber(0);
        newOpCtx->setInMultiDocumentTransaction();

        auto mongoDSessionCatalog = MongoDSessionCatalog::get(newOpCtx);
        auto ocs = mongoDSessionCatalog->checkOutSession(newOpCtx);
        auto txnParticipant = TransactionParticipant::get(newOpCtx);
        txnParticipant.beginOrContinue(newOpCtx,
                                       {0},
                                       false /* autocommit */,
                                       TransactionParticipant::TransactionActions::kStart);
        ASSERT(txnParticipant.transactionIsInProgress());
        txnParticipant.unstashTransactionResources(newOpCtx, "insert");
        txnParticipant.stashTransactionResources(newOpCtx);
    });

    // A second conflicting transaction should throw and not abort the active one.
    runFunctionFromDifferentOpCtx([parentLsid, parentTxnNumber](OperationContext* newOpCtx) {
        newOpCtx->setLogicalSessionId(
            makeLogicalSessionIdWithTxnNumberAndUUIDForTest(parentLsid, parentTxnNumber));
        newOpCtx->setTxnNumber(0);
        newOpCtx->setInMultiDocumentTransaction();

        auto mongoDSessionCatalog = MongoDSessionCatalog::get(newOpCtx);
        auto ocs = mongoDSessionCatalog->checkOutSession(newOpCtx);
        auto txnParticipant = TransactionParticipant::get(newOpCtx);
        ASSERT_THROWS_CODE(
            txnParticipant.beginOrContinue(newOpCtx,
                                           {0},
                                           false /* autocommit */,
                                           TransactionParticipant::TransactionActions::kStart),
            AssertionException,
            ErrorCodes::RetryableTransactionInProgress);
    });

    // Advance the txnNumber and verify the first new conflicting transaction does not throw
    // RetryableTransactionInProgress.

    parentTxnNumber += 1;
    const auto higherChildLsid =
        makeLogicalSessionIdWithTxnNumberAndUUIDForTest(parentLsid, parentTxnNumber);
    runFunctionFromDifferentOpCtx([higherChildLsid](OperationContext* newOpCtx) {
        newOpCtx->setLogicalSessionId(higherChildLsid);
        newOpCtx->setTxnNumber(0);
        newOpCtx->setInMultiDocumentTransaction();

        auto mongoDSessionCatalog = MongoDSessionCatalog::get(newOpCtx);
        auto ocs = mongoDSessionCatalog->checkOutSession(newOpCtx);
        auto txnParticipant = TransactionParticipant::get(newOpCtx);
        txnParticipant.beginOrContinue(newOpCtx,
                                       {0},
                                       false /* autocommit */,
                                       TransactionParticipant::TransactionActions::kStart);
        ASSERT(txnParticipant.transactionIsInProgress());
    });

    const auto higherFirstConflictingLsid =
        makeLogicalSessionIdWithTxnNumberAndUUIDForTest(parentLsid, parentTxnNumber);
    runFunctionFromDifferentOpCtx([higherFirstConflictingLsid](OperationContext* newOpCtx) {
        newOpCtx->setLogicalSessionId(higherFirstConflictingLsid);
        newOpCtx->setTxnNumber(0);
        newOpCtx->setInMultiDocumentTransaction();

        auto mongoDSessionCatalog = MongoDSessionCatalog::get(newOpCtx);
        auto ocs = mongoDSessionCatalog->checkOutSession(newOpCtx);
        auto txnParticipant = TransactionParticipant::get(newOpCtx);
        txnParticipant.beginOrContinue(newOpCtx,
                                       {0},
                                       false /* autocommit */,
                                       TransactionParticipant::TransactionActions::kStart);
        ASSERT(txnParticipant.transactionIsInProgress());
    });

    // A second conflicting transaction should still throw and not abort the active one.
    const auto higherSecondConflictingLsid =
        makeLogicalSessionIdWithTxnNumberAndUUIDForTest(parentLsid, parentTxnNumber);
    runFunctionFromDifferentOpCtx([higherSecondConflictingLsid](OperationContext* newOpCtx) {
        newOpCtx->setLogicalSessionId(higherSecondConflictingLsid);
        newOpCtx->setTxnNumber(0);
        newOpCtx->setInMultiDocumentTransaction();

        auto mongoDSessionCatalog = MongoDSessionCatalog::get(newOpCtx);
        auto ocs = mongoDSessionCatalog->checkOutSession(newOpCtx);
        auto txnParticipant = TransactionParticipant::get(newOpCtx);
        ASSERT_THROWS_CODE(
            txnParticipant.beginOrContinue(newOpCtx,
                                           {0},
                                           false /* autocommit */,
                                           TransactionParticipant::TransactionActions::kStart),
            AssertionException,
            ErrorCodes::RetryableTransactionInProgress);
    });
}

TEST_F(ShardTxnParticipantTest, HigherTxnNumberAbortsLowerChildTransactions_RetryableTxn) {
    const auto parentLsid = makeLogicalSessionIdForTest();
    auto parentTxnNumber = *opCtx()->getTxnNumber();

    opCtx()->setLogicalSessionId(
        makeLogicalSessionIdWithTxnNumberAndUUIDForTest(parentLsid, parentTxnNumber));
    {
        auto sessionCheckout = checkOutSession();
        auto txnParticipant = TransactionParticipant::get(opCtx());
        ASSERT_TRUE(txnParticipant.transactionIsInProgress());
    }

    // Advance the txnNumber and verify the first new conflicting transaction does not throw
    // RetryableTransactionInProgress.

    parentTxnNumber += 1;

    const auto higherChildLsid =
        makeLogicalSessionIdWithTxnNumberAndUUIDForTest(parentLsid, parentTxnNumber);
    runFunctionFromDifferentOpCtx([higherChildLsid](OperationContext* newOpCtx) {
        newOpCtx->setLogicalSessionId(higherChildLsid);
        newOpCtx->setTxnNumber(0);
        newOpCtx->setInMultiDocumentTransaction();

        auto mongoDSessionCatalog = MongoDSessionCatalog::get(newOpCtx);
        auto ocs = mongoDSessionCatalog->checkOutSession(newOpCtx);
        auto txnParticipant = TransactionParticipant::get(newOpCtx);
        txnParticipant.beginOrContinue(newOpCtx,
                                       {0},
                                       false /* autocommit */,
                                       TransactionParticipant::TransactionActions::kStart);
        ASSERT(txnParticipant.transactionIsInProgress());
    });
}

TEST_F(ShardTxnParticipantTest, HigherTxnNumberAbortsLowerChildTransactions_RetryableWrite) {
    const auto parentLsid = makeLogicalSessionIdForTest();
    auto parentTxnNumber = *opCtx()->getTxnNumber();

    opCtx()->setLogicalSessionId(
        makeLogicalSessionIdWithTxnNumberAndUUIDForTest(parentLsid, parentTxnNumber));
    {
        auto sessionCheckout = checkOutSession();
        auto txnParticipant = TransactionParticipant::get(opCtx());
        ASSERT_TRUE(txnParticipant.transactionIsInProgress());
    }

    // Advance the txnNumber and verify the first new conflicting transaction does not throw
    // RetryableTransactionInProgress.

    parentTxnNumber += 1;

    runFunctionFromDifferentOpCtx([parentLsid, parentTxnNumber](OperationContext* newOpCtx) {
        newOpCtx->setLogicalSessionId(parentLsid);
        newOpCtx->setTxnNumber(parentTxnNumber);

        auto mongoDSessionCatalog = MongoDSessionCatalog::get(newOpCtx);
        auto ocs = mongoDSessionCatalog->checkOutSession(newOpCtx);
        auto txnParticipant = TransactionParticipant::get(newOpCtx);
        txnParticipant.beginOrContinue(newOpCtx,
                                       {parentTxnNumber},
                                       boost::none /* autocommit */,
                                       TransactionParticipant::TransactionActions::kNone);
    });
}

TEST_F(ShardTxnParticipantTest, HigherTxnNumberAbortsLowerChildTransactions_Transaction) {
    const auto parentLsid = makeLogicalSessionIdForTest();
    auto parentTxnNumber = *opCtx()->getTxnNumber();

    opCtx()->setLogicalSessionId(
        makeLogicalSessionIdWithTxnNumberAndUUIDForTest(parentLsid, parentTxnNumber));
    {
        auto sessionCheckout = checkOutSession();
        auto txnParticipant = TransactionParticipant::get(opCtx());
        ASSERT_TRUE(txnParticipant.transactionIsInProgress());
    }

    // Advance the txnNumber and verify the first new conflicting transaction does not throw
    // RetryableTransactionInProgress.

    parentTxnNumber += 1;

    runFunctionFromDifferentOpCtx([parentLsid, parentTxnNumber](OperationContext* newOpCtx) {
        newOpCtx->setLogicalSessionId(parentLsid);
        newOpCtx->setTxnNumber(parentTxnNumber);
        newOpCtx->setInMultiDocumentTransaction();

        auto mongoDSessionCatalog = MongoDSessionCatalog::get(newOpCtx);
        auto ocs = mongoDSessionCatalog->checkOutSession(newOpCtx);
        auto txnParticipant = TransactionParticipant::get(newOpCtx);
        txnParticipant.beginOrContinue(newOpCtx,
                                       *newOpCtx->getTxnNumber(),
                                       false /* autocommit */,
                                       TransactionParticipant::TransactionActions::kStart);
        ASSERT(txnParticipant.transactionIsInProgress());
    });
}

TEST_F(ShardTxnParticipantTest, HigherTxnNumberDoesNotAbortPreparedLowerChildTransaction) {
    const auto parentLsid = makeLogicalSessionIdForTest();
    const auto parentTxnNumber = *opCtx()->getTxnNumber();

    // Start a prepared child transaction.
    opCtx()->setLogicalSessionId(
        makeLogicalSessionIdWithTxnNumberAndUUIDForTest(parentLsid, parentTxnNumber));
    {
        auto sessionCheckout = checkOutSession();
        auto txnParticipant = TransactionParticipant::get(opCtx());
        txnParticipant.unstashTransactionResources(opCtx(), "prepareTransaction");
        txnParticipant.prepareTransaction(opCtx(), {});
        ASSERT(txnParticipant.transactionIsPrepared());
        txnParticipant.stashTransactionResources(opCtx());
    }

    // Advance the txnNumber and verify the first new conflicting transaction and retryable write
    // throws RetryableTransactionInProgress.

    const auto higherParentTxnNumber = parentTxnNumber + 1;

    const auto higherChildLsid =
        makeLogicalSessionIdWithTxnNumberAndUUIDForTest(parentLsid, higherParentTxnNumber);
    runFunctionFromDifferentOpCtx([higherChildLsid](OperationContext* newOpCtx) {
        newOpCtx->setLogicalSessionId(higherChildLsid);
        newOpCtx->setTxnNumber(0);
        newOpCtx->setInMultiDocumentTransaction();

        auto mongoDSessionCatalog = MongoDSessionCatalog::get(newOpCtx);
        auto ocs = mongoDSessionCatalog->checkOutSession(newOpCtx);
        auto txnParticipant = TransactionParticipant::get(newOpCtx);
        ASSERT_THROWS_CODE(
            txnParticipant.beginOrContinue(newOpCtx,
                                           {0},
                                           false /* autocommit */,
                                           TransactionParticipant::TransactionActions::kStart),
            AssertionException,
            ErrorCodes::RetryableTransactionInProgress);
    });

    runFunctionFromDifferentOpCtx([parentLsid, higherParentTxnNumber](OperationContext* newOpCtx) {
        newOpCtx->setLogicalSessionId(parentLsid);
        newOpCtx->setTxnNumber(higherParentTxnNumber);

        auto mongoDSessionCatalog = MongoDSessionCatalog::get(newOpCtx);
        auto ocs = mongoDSessionCatalog->checkOutSession(newOpCtx);
        auto txnParticipant = TransactionParticipant::get(newOpCtx);
        ASSERT_THROWS_CODE(
            txnParticipant.beginOrContinue(newOpCtx,
                                           {higherParentTxnNumber},
                                           boost::none /* autocommit */,
                                           TransactionParticipant::TransactionActions::kNone),
            AssertionException,
            ErrorCodes::RetryableTransactionInProgress);
    });

    // After the transaction leaves prepare a conflicting internal transaction can still abort an
    // active transaction.

    {
        auto sessionCheckout =
            checkOutSession(TransactionParticipant::TransactionActions::kContinue);
        auto txnParticipant = TransactionParticipant::get(opCtx());
        txnParticipant.beginOrContinue(opCtx(),
                                       {parentTxnNumber},
                                       false /* autocommit */,
                                       TransactionParticipant::TransactionActions::kContinue);
        txnParticipant.unstashTransactionResources(opCtx(), "abortTransaction");
        txnParticipant.abortTransaction(opCtx());
    }

    runFunctionFromDifferentOpCtx([higherChildLsid](OperationContext* newOpCtx) {
        newOpCtx->setLogicalSessionId(higherChildLsid);
        newOpCtx->setTxnNumber(0);
        newOpCtx->setInMultiDocumentTransaction();

        auto mongoDSessionCatalog = MongoDSessionCatalog::get(newOpCtx);
        auto ocs = mongoDSessionCatalog->checkOutSession(newOpCtx);
        auto txnParticipant = TransactionParticipant::get(newOpCtx);
        txnParticipant.beginOrContinue(newOpCtx,
                                       {0},
                                       false /* autocommit */,
                                       TransactionParticipant::TransactionActions::kStart);
        ASSERT(txnParticipant.transactionIsInProgress());
    });

    const auto higherConflictingChildLsid =
        makeLogicalSessionIdWithTxnNumberAndUUIDForTest(parentLsid, higherParentTxnNumber);
    runFunctionFromDifferentOpCtx([higherConflictingChildLsid](OperationContext* newOpCtx) {
        newOpCtx->setLogicalSessionId(higherConflictingChildLsid);
        newOpCtx->setTxnNumber(0);
        newOpCtx->setInMultiDocumentTransaction();

        auto mongoDSessionCatalog = MongoDSessionCatalog::get(newOpCtx);
        auto ocs = mongoDSessionCatalog->checkOutSession(newOpCtx);
        auto txnParticipant = TransactionParticipant::get(newOpCtx);
        txnParticipant.beginOrContinue(newOpCtx,
                                       {0},
                                       false /* autocommit */,
                                       TransactionParticipant::TransactionActions::kStart);
        ASSERT(txnParticipant.transactionIsInProgress());
    });
}

TEST_F(ShardTxnParticipantTest,
       CannotRetryPreparedTransactionForRetryableWrite_OriginalTransactionForRetryableWrite) {
    opCtx()->setLogicalSessionId(makeLogicalSessionIdWithTxnNumberAndUUIDForTest());
    auto sessionCheckout = checkOutSession();
    auto txnParticipant = TransactionParticipant::get(opCtx());
    ASSERT(txnParticipant.transactionIsInProgress());
    txnParticipant.unstashTransactionResources(opCtx(), "prepareTransaction");
    txnParticipant.prepareTransaction(opCtx(), {});
    ASSERT(txnParticipant.transactionIsPrepared());
    txnParticipant.stashTransactionResources(opCtx());

    ASSERT_THROWS_CODE(
        txnParticipant.beginOrContinue(opCtx(),
                                       {*opCtx()->getTxnNumber(), 0},
                                       false /* autocommit */,
                                       TransactionParticipant::TransactionActions::kStart),
        AssertionException,
        50911);
    ASSERT(txnParticipant.transactionIsPrepared());
}

TEST_F(ShardTxnParticipantTest,
       CannotRetryPreparedTransactionForRetryableWrite_ConflictingTransactionForRetryableWrite) {
    const auto parentLsid = makeLogicalSessionIdForTest();
    const auto parentTxnNumber = *opCtx()->getTxnNumber();

    opCtx()->setLogicalSessionId(
        makeLogicalSessionIdWithTxnNumberAndUUIDForTest(parentLsid, parentTxnNumber));
    auto sessionCheckout = checkOutSession();
    auto txnParticipant = TransactionParticipant::get(opCtx());
    ASSERT(txnParticipant.transactionIsInProgress());
    txnParticipant.unstashTransactionResources(opCtx(), "prepareTransaction");
    txnParticipant.prepareTransaction(opCtx(), {});
    ASSERT(txnParticipant.transactionIsPrepared());
    txnParticipant.stashTransactionResources(opCtx());
    OperationContextSession::checkIn(opCtx(), OperationContextSession::CheckInReason::kDone);

    runFunctionFromDifferentOpCtx([parentLsid, parentTxnNumber](OperationContext* newOpCtx) {
        newOpCtx->setLogicalSessionId(
            makeLogicalSessionIdWithTxnNumberAndUUIDForTest(parentLsid, parentTxnNumber));
        newOpCtx->setTxnNumber(0);
        newOpCtx->setInMultiDocumentTransaction();

        auto mongoDSessionCatalog = MongoDSessionCatalog::get(newOpCtx);
        auto ocs = mongoDSessionCatalog->checkOutSession(newOpCtx);
        auto txnParticipant = TransactionParticipant::get(newOpCtx);
        ASSERT_THROWS_CODE(
            txnParticipant.beginOrContinue(newOpCtx,
                                           {0},
                                           false /* autocommit */,
                                           TransactionParticipant::TransactionActions::kStart),
            AssertionException,
            ErrorCodes::RetryableTransactionInProgress);
    });

    ASSERT(txnParticipant.transactionIsPrepared());
}

TEST_F(ShardTxnParticipantTest,
       CannotRetryPreparedTransactionForRetryableWrite_ConflictingRetryableWrite) {
    const auto parentLsid = makeLogicalSessionIdForTest();
    const auto parentTxnNumber = *opCtx()->getTxnNumber();

    opCtx()->setLogicalSessionId(
        makeLogicalSessionIdWithTxnNumberAndUUIDForTest(parentLsid, parentTxnNumber));
    auto sessionCheckout = checkOutSession();
    auto txnParticipant = TransactionParticipant::get(opCtx());
    ASSERT(txnParticipant.transactionIsInProgress());
    txnParticipant.unstashTransactionResources(opCtx(), "prepareTransaction");
    txnParticipant.prepareTransaction(opCtx(), {});
    ASSERT(txnParticipant.transactionIsPrepared());
    txnParticipant.stashTransactionResources(opCtx());
    OperationContextSession::checkIn(opCtx(), OperationContextSession::CheckInReason::kDone);

    runFunctionFromDifferentOpCtx([parentLsid, parentTxnNumber](OperationContext* newOpCtx) {
        newOpCtx->setLogicalSessionId(parentLsid);
        newOpCtx->setTxnNumber(parentTxnNumber);

        auto mongoDSessionCatalog = MongoDSessionCatalog::get(newOpCtx);
        auto ocs = mongoDSessionCatalog->checkOutSession(newOpCtx);
        auto txnParticipant = TransactionParticipant::get(newOpCtx);
        ASSERT_THROWS_CODE(
            txnParticipant.beginOrContinue(newOpCtx,
                                           {parentTxnNumber},
                                           boost::none /* autocommit */,
                                           TransactionParticipant::TransactionActions::kNone),
            AssertionException,
            ErrorCodes::RetryableTransactionInProgress);
    });

    ASSERT(txnParticipant.transactionIsPrepared());
}

TEST_F(ShardTxnParticipantTest, CanRetryCommittedUnpreparedTransactionForRetryableWrite) {
    opCtx()->setLogicalSessionId(makeLogicalSessionIdWithTxnNumberAndUUIDForTest());
    auto sessionCheckout = checkOutSession();
    auto txnParticipant = TransactionParticipant::get(opCtx());
    ASSERT(txnParticipant.transactionIsInProgress());
    txnParticipant.unstashTransactionResources(opCtx(), "commitTransaction");
    txnParticipant.commitUnpreparedTransaction(opCtx());
    ASSERT(txnParticipant.transactionIsCommitted());

    txnParticipant.beginOrContinue(opCtx(),
                                   {*opCtx()->getTxnNumber(), 0},
                                   false /* autocommit */,
                                   TransactionParticipant::TransactionActions::kStart);
    ASSERT(txnParticipant.transactionIsCommitted());
}

TEST_F(ShardTxnParticipantTest, CanRetryCommittedPreparedTransactionForRetryableWrite) {
    opCtx()->setLogicalSessionId(makeLogicalSessionIdWithTxnNumberAndUUIDForTest());
    auto sessionCheckout = checkOutSession();
    auto txnParticipant = TransactionParticipant::get(opCtx());
    ASSERT(txnParticipant.transactionIsInProgress());
    txnParticipant.unstashTransactionResources(opCtx(), "commitTransaction");
    const auto [prepareTimestamp, namespaces] = txnParticipant.prepareTransaction(opCtx(), {});
    const auto commitTS = Timestamp(prepareTimestamp.getSecs(), prepareTimestamp.getInc() + 1);
    txnParticipant.commitPreparedTransaction(opCtx(), commitTS, {});
    ASSERT_TRUE(txnParticipant.transactionIsCommitted());

    txnParticipant.beginOrContinue(opCtx(),
                                   {*opCtx()->getTxnNumber(), 0},
                                   false /* autocommit */,
                                   TransactionParticipant::TransactionActions::kStart);
    ASSERT(txnParticipant.transactionIsCommitted());
}

TEST_F(ShardTxnParticipantTest, AbortingCommittedUnpreparedTransactionForRetryableWriteIsNoop) {
    opCtx()->setLogicalSessionId(makeLogicalSessionIdWithTxnNumberAndUUIDForTest());
    auto sessionCheckout = checkOutSession();
    auto txnParticipant = TransactionParticipant::get(opCtx());
    ASSERT(txnParticipant.transactionIsInProgress());
    txnParticipant.unstashTransactionResources(opCtx(), "commitTransaction");
    txnParticipant.commitUnpreparedTransaction(opCtx());
    ASSERT(txnParticipant.transactionIsCommitted());

    txnParticipant.abortTransaction(opCtx());
    ASSERT(txnParticipant.transactionIsCommitted());
}

TEST_F(ShardTxnParticipantTest, AbortingCommittedPreparedTransactionForRetryableWriteIsNoop) {
    opCtx()->setLogicalSessionId(makeLogicalSessionIdWithTxnNumberAndUUIDForTest());
    auto sessionCheckout = checkOutSession();
    auto txnParticipant = TransactionParticipant::get(opCtx());
    ASSERT(txnParticipant.transactionIsInProgress());
    txnParticipant.unstashTransactionResources(opCtx(), "commitTransaction");
    const auto [prepareTimestamp, namespaces] = txnParticipant.prepareTransaction(opCtx(), {});
    const auto commitTS = Timestamp(prepareTimestamp.getSecs(), prepareTimestamp.getInc() + 1);
    txnParticipant.commitPreparedTransaction(opCtx(), commitTS, {});
    ASSERT_TRUE(txnParticipant.transactionIsCommitted());

    txnParticipant.abortTransaction(opCtx());
    ASSERT(txnParticipant.transactionIsCommitted());
}

TEST_F(ShardTxnParticipantTest,
       CannotAddOperationToCommittedUnpreparedTransactionForRetryableWrite) {
    opCtx()->setLogicalSessionId(makeLogicalSessionIdWithTxnNumberAndUUIDForTest());
    auto sessionCheckout = checkOutSession();
    auto txnParticipant = TransactionParticipant::get(opCtx());
    ASSERT(txnParticipant.transactionIsInProgress());
    txnParticipant.unstashTransactionResources(opCtx(), "commitTransaction");
    txnParticipant.commitUnpreparedTransaction(opCtx());
    ASSERT(txnParticipant.transactionIsCommitted());

    txnParticipant.unstashTransactionResources(opCtx(), "insert");
    auto operation = repl::DurableOplogEntry::makeInsertOperation(
        kNss, _uuid, BSON("_id" << 0), BSON("_id" << 0));
    ASSERT_THROWS_CODE(
        txnParticipant.addTransactionOperation(opCtx(), operation), AssertionException, 5875606);
    ASSERT(txnParticipant.transactionIsCommitted());
    ASSERT(txnParticipant.getTransactionOperationsForTest().empty());
}

TEST_F(ShardTxnParticipantTest, CannotAddOperationToCommittedPreparedTransactionForRetryableWrite) {
    opCtx()->setLogicalSessionId(makeLogicalSessionIdWithTxnNumberAndUUIDForTest());
    auto sessionCheckout = checkOutSession();
    auto txnParticipant = TransactionParticipant::get(opCtx());
    ASSERT(txnParticipant.transactionIsInProgress());
    txnParticipant.unstashTransactionResources(opCtx(), "commitTransaction");
    const auto [prepareTimestamp, namespaces] = txnParticipant.prepareTransaction(opCtx(), {});
    const auto commitTS = Timestamp(prepareTimestamp.getSecs(), prepareTimestamp.getInc() + 1);
    txnParticipant.commitPreparedTransaction(opCtx(), commitTS, {});
    ASSERT_TRUE(txnParticipant.transactionIsCommitted());

    txnParticipant.unstashTransactionResources(opCtx(), "insert");
    auto operation = repl::DurableOplogEntry::makeInsertOperation(
        kNss, _uuid, BSON("_id" << 0), BSON("_id" << 0));
    ASSERT_THROWS_CODE(
        txnParticipant.addTransactionOperation(opCtx(), operation), AssertionException, 5875606);
    ASSERT(txnParticipant.transactionIsCommitted());
    ASSERT(txnParticipant.getTransactionOperationsForTest().empty());
}

TEST_F(ShardTxnParticipantTest, CannotAddOperationToAbortedUnpreparedTransactionForRetryableWrite) {
    opCtx()->setLogicalSessionId(makeLogicalSessionIdWithTxnNumberAndUUIDForTest());
    auto sessionCheckout = checkOutSession();
    auto txnParticipant = TransactionParticipant::get(opCtx());
    txnParticipant.unstashTransactionResources(opCtx(), "abortTransaction");
    txnParticipant.abortTransaction(opCtx());
    ASSERT_TRUE(txnParticipant.transactionIsAborted());
    ASSERT(txnParticipant.getTransactionOperationsForTest().empty());

    auto operation = repl::DurableOplogEntry::makeInsertOperation(
        kNss, _uuid, BSON("_id" << 0), BSON("_id" << 0));
    ASSERT_THROWS_CODE(
        txnParticipant.addTransactionOperation(opCtx(), operation), AssertionException, 5875606);
    ASSERT(txnParticipant.getTransactionOperationsForTest().empty());
}

TEST_F(ShardTxnParticipantTest, CannotAddOperationToAbortedPreparedTransactionForRetryableWrite) {
    opCtx()->setLogicalSessionId(makeLogicalSessionIdWithTxnNumberAndUUIDForTest());
    auto sessionCheckout = checkOutSession();
    auto txnParticipant = TransactionParticipant::get(opCtx());
    txnParticipant.unstashTransactionResources(opCtx(), "abortTransaction");
    txnParticipant.prepareTransaction(opCtx(), {});
    txnParticipant.abortTransaction(opCtx());
    ASSERT_TRUE(txnParticipant.transactionIsAborted());

    auto operation = repl::DurableOplogEntry::makeInsertOperation(
        kNss, _uuid, BSON("_id" << 0), BSON("_id" << 0));
    ASSERT_THROWS_CODE(
        txnParticipant.addTransactionOperation(opCtx(), operation), AssertionException, 5875606);
    ASSERT(txnParticipant.getTransactionOperationsForTest().empty());
}

TEST_F(ShardTxnParticipantTest, CannotAddOperationToPreparedTransactionForRetryableWrite) {
    opCtx()->setLogicalSessionId(makeLogicalSessionIdWithTxnNumberAndUUIDForTest());
    auto sessionCheckout = checkOutSession();
    auto txnParticipant = TransactionParticipant::get(opCtx());
    txnParticipant.unstashTransactionResources(opCtx(), "prepareTransaction");
    txnParticipant.prepareTransaction(opCtx(), {});
    ASSERT_TRUE(txnParticipant.transactionIsPrepared());

    auto operation = repl::DurableOplogEntry::makeInsertOperation(
        kNss, _uuid, BSON("_id" << 0), BSON("_id" << 0));
    ASSERT_THROWS_CODE(
        txnParticipant.addTransactionOperation(opCtx(), operation), AssertionException, 5875606);
    ASSERT(txnParticipant.getTransactionOperationsForTest().empty());
}

TEST_F(ShardTxnParticipantTest, CannotModifyParentLsidOfChildSession) {
    const auto lsid = makeLogicalSessionIdWithTxnNumberAndUUIDForTest();
    opCtx()->setLogicalSessionId(lsid);
    auto sessionCheckout = checkOutSession();
    auto txnParticipant = TransactionParticipant::get(opCtx());
    ASSERT_TRUE(txnParticipant.transactionIsOpen());

    Timestamp ts(1, 1);
    SessionTxnRecord sessionTxnRecord;
    sessionTxnRecord.setSessionId(lsid);
    sessionTxnRecord.setTxnNum(*opCtx()->getTxnNumber());
    sessionTxnRecord.setParentSessionId(*getParentSessionId(lsid));
    sessionTxnRecord.setLastWriteOpTime(repl::OpTime(ts, 0));
    sessionTxnRecord.setLastWriteDate(Date_t::now());

    // Insert.
    {
        WriteUnitOfWork wuow(opCtx());
        txnParticipant.onWriteOpCompletedOnPrimary(opCtx(), {}, sessionTxnRecord);
        wuow.commit();
    }

    // Update that does not modify "parentLsid".
    {
        WriteUnitOfWork wuow(opCtx());
        txnParticipant.onWriteOpCompletedOnPrimary(opCtx(), {}, sessionTxnRecord);
        wuow.commit();
    }

    // Updates that try to modify "parentLsid".
    {
        WriteUnitOfWork wuow(opCtx());
        sessionTxnRecord.setParentSessionId(makeLogicalSessionIdForTest());
        ASSERT_THROWS_CODE(
            txnParticipant.onWriteOpCompletedOnPrimary(opCtx(), {}, sessionTxnRecord),
            AssertionException,
            5875700);
    }
    {
        WriteUnitOfWork wuow(opCtx());
        sessionTxnRecord.setParentSessionId(lsid);
        ASSERT_THROWS_CODE(
            txnParticipant.onWriteOpCompletedOnPrimary(opCtx(), {}, sessionTxnRecord),
            AssertionException,
            5875700);
    }
    {
        WriteUnitOfWork wuow(opCtx());
        sessionTxnRecord.setParentSessionId(boost::none);
        ASSERT_THROWS_CODE(
            txnParticipant.onWriteOpCompletedOnPrimary(opCtx(), {}, sessionTxnRecord),
            AssertionException,
            5875700);
    }
}

TEST_F(ShardTxnParticipantTest, CannotModifyParentLsidOfNonChildSession) {
    const auto lsid = makeLogicalSessionIdForTest();
    opCtx()->setLogicalSessionId(lsid);
    auto sessionCheckout = checkOutSession();
    auto txnParticipant = TransactionParticipant::get(opCtx());
    ASSERT_TRUE(txnParticipant.transactionIsOpen());

    Timestamp ts(1, 1);
    SessionTxnRecord sessionTxnRecord;
    sessionTxnRecord.setSessionId(lsid);
    sessionTxnRecord.setTxnNum(*opCtx()->getTxnNumber());
    sessionTxnRecord.setLastWriteOpTime(repl::OpTime(ts, 0));
    sessionTxnRecord.setLastWriteDate(Date_t::now());

    // Insert.
    {
        WriteUnitOfWork wuow(opCtx());
        txnParticipant.onWriteOpCompletedOnPrimary(opCtx(), {}, sessionTxnRecord);
        wuow.commit();
    }

    // Update that does not set/modify "parentLsid".
    {
        WriteUnitOfWork wuow(opCtx());
        txnParticipant.onWriteOpCompletedOnPrimary(opCtx(), {}, sessionTxnRecord);
        wuow.commit();
    }

    // Update that tries to set "parentLsid".
    {
        WriteUnitOfWork wuow(opCtx());
        sessionTxnRecord.setParentSessionId(makeLogicalSessionIdForTest());
        ASSERT_THROWS_CODE(
            txnParticipant.onWriteOpCompletedOnPrimary(opCtx(), {}, sessionTxnRecord),
            AssertionException,
            5875700);
    }
}

TEST_F(ShardTxnParticipantTest,
       TxnRetryCounterShouldNotThrowIfWeContinueATransactionAfterDisablingFeatureFlag) {
    // We swap in a new opCtx in order to set a new active txnRetryCounter for this test.
    auto newClientOwned = getServiceContext()->getService()->makeClient("newClient");
    AlternativeClientRegion acr(newClientOwned);
    auto newOpCtx = cc().makeOperationContext();

    const auto newSessionId = makeLogicalSessionIdForTest();

    newOpCtx.get()->setLogicalSessionId(newSessionId);
    newOpCtx.get()->setTxnNumber(20);
    newOpCtx.get()->setTxnRetryCounter(1);
    newOpCtx.get()->setInMultiDocumentTransaction();
    auto mongoDSessionCatalog = MongoDSessionCatalog::get(newOpCtx.get());
    auto newOpCtxSession = mongoDSessionCatalog->checkOutSession(newOpCtx.get());
    auto txnParticipant = TransactionParticipant::get(newOpCtx.get());

    txnParticipant.beginOrContinue(newOpCtx.get(),
                                   {*newOpCtx.get()->getTxnNumber(), 1},
                                   false /* autocommit */,
                                   TransactionParticipant::TransactionActions::kStart);

    // We need to unstash and stash transaction resources so that we can continue the transaction in
    // the following statements.
    txnParticipant.unstashTransactionResources(newOpCtx.get(), "insert");
    txnParticipant.stashTransactionResources(newOpCtx.get());

    txnParticipant.beginOrContinue(newOpCtx.get(),
                                   {*newOpCtx.get()->getTxnNumber(), 1},
                                   false /* autocommit */,
                                   TransactionParticipant::TransactionActions::kContinue);
}

TEST_F(TxnParticipantTest, UnstashTransactionAfterActiveTxnNumberHasChanged) {
    auto clientOwned = getServiceContext()->getService()->makeClient("client");
    AlternativeClientRegion acr(clientOwned);
    auto opCtxHolder = cc().makeOperationContext();
    auto opCtx = opCtxHolder.get();
    opCtx->setLogicalSessionId(_sessionId);
    opCtx->setTxnNumber(_txnNumber);
    opCtx->setInMultiDocumentTransaction();

    auto mongoDSessionCatalog = MongoDSessionCatalog::get(opCtx);
    {
        auto opCtxSession = mongoDSessionCatalog->checkOutSession(opCtx);
        auto txnParticipant = TransactionParticipant::get(opCtx);
        txnParticipant.beginOrContinue(opCtx,
                                       {*opCtx->getTxnNumber()},
                                       false /* autocommit */,
                                       TransactionParticipant::TransactionActions::kStart);
    }

    {
        auto sideClientOwned = getServiceContext()->getService()->makeClient("sideClient");
        AlternativeClientRegion acr(sideClientOwned);
        auto sideOpCtx = cc().makeOperationContext();

        sideOpCtx.get()->setLogicalSessionId(_sessionId);
        sideOpCtx.get()->setTxnNumber(_txnNumber + 1);
        sideOpCtx.get()->setInMultiDocumentTransaction();
        auto sideOpCtxSession = mongoDSessionCatalog->checkOutSession(sideOpCtx.get());
        auto txnParticipant = TransactionParticipant::get(sideOpCtx.get());

        txnParticipant.beginOrContinue(sideOpCtx.get(),
                                       {*sideOpCtx.get()->getTxnNumber()},
                                       false /* autocommit */,
                                       TransactionParticipant::TransactionActions::kStart);
    }

    {
        auto opCtxSession = mongoDSessionCatalog->checkOutSession(opCtx);
        auto txnParticipant = TransactionParticipant::get(opCtx);
        ASSERT_THROWS_CODE(txnParticipant.unstashTransactionResources(opCtx, "insert"),
                           AssertionException,
                           ErrorCodes::NoSuchTransaction);
    }
}

TEST_F(TxnParticipantTest, UnstashRetryableWriteAfterActiveTxnNumberHasChanged) {
    auto clientOwned = getServiceContext()->getService()->makeClient("client");
    AlternativeClientRegion acr(clientOwned);
    auto opCtxHolder = cc().makeOperationContext();
    auto opCtx = opCtxHolder.get();
    opCtx->setLogicalSessionId(_sessionId);
    opCtx->setTxnNumber(_txnNumber);

    auto mongoDSessionCatalog = MongoDSessionCatalog::get(opCtx);
    {
        auto opCtxSession = mongoDSessionCatalog->checkOutSession(opCtx);
        auto txnParticipant = TransactionParticipant::get(opCtx);
        txnParticipant.beginOrContinue(opCtx,
                                       {*opCtx->getTxnNumber()},
                                       boost::none /* autocommit */,
                                       TransactionParticipant::TransactionActions::kNone);
    }

    {
        auto sideClientOwned = getServiceContext()->getService()->makeClient("sideClient");
        AlternativeClientRegion acr(sideClientOwned);
        auto sideOpCtx = cc().makeOperationContext();

        sideOpCtx.get()->setLogicalSessionId(_sessionId);
        sideOpCtx.get()->setTxnNumber(_txnNumber + 1);
        sideOpCtx.get()->setInMultiDocumentTransaction();
        auto sideOpCtxSession = mongoDSessionCatalog->checkOutSession(sideOpCtx.get());
        auto txnParticipant = TransactionParticipant::get(sideOpCtx.get());

        txnParticipant.beginOrContinue(sideOpCtx.get(),
                                       {*sideOpCtx.get()->getTxnNumber()},
                                       boost::none /* autocommit */,
                                       TransactionParticipant::TransactionActions::kNone);
    }

    {
        auto opCtxSession = mongoDSessionCatalog->checkOutSession(opCtx);
        auto txnParticipant = TransactionParticipant::get(opCtx);
        ASSERT_THROWS_CODE(txnParticipant.unstashTransactionResources(opCtx, "insert"),
                           AssertionException,
                           6564100);
    }
}

TEST_F(TxnParticipantTest, UnstashTransactionAfterActiveTxnNumberNoLongerCorrespondsToTransaction) {
    auto clientOwned = getServiceContext()->getService()->makeClient("client");
    AlternativeClientRegion acr(clientOwned);
    auto opCtxHolder = cc().makeOperationContext();
    auto opCtx = opCtxHolder.get();
    opCtx->setLogicalSessionId(_sessionId);
    opCtx->setTxnNumber(_txnNumber);
    opCtx->setInMultiDocumentTransaction();

    auto mongoDSessionCatalog = MongoDSessionCatalog::get(opCtx);
    {
        auto opCtxSession = mongoDSessionCatalog->checkOutSession(opCtx);
        auto txnParticipant = TransactionParticipant::get(opCtx);
        txnParticipant.beginOrContinue(opCtx,
                                       {*opCtx->getTxnNumber()},
                                       false /* autocommit */,
                                       TransactionParticipant::TransactionActions::kStart);
        // Invalidate the TransactionParticipant allow the txnNumber to be reused.
        txnParticipant.invalidate(opCtx);
    }

    {
        auto sideClientOwned = getServiceContext()->getService()->makeClient("sideClient");
        AlternativeClientRegion acr(sideClientOwned);
        auto sideOpCtx = cc().makeOperationContext();

        sideOpCtx.get()->setLogicalSessionId(_sessionId);
        sideOpCtx.get()->setTxnNumber(_txnNumber);
        sideOpCtx.get()->setInMultiDocumentTransaction();
        auto sideOpCtxSession = mongoDSessionCatalog->checkOutSession(sideOpCtx.get());
        auto txnParticipant = TransactionParticipant::get(sideOpCtx.get());

        txnParticipant.beginOrContinue(sideOpCtx.get(),
                                       {*sideOpCtx.get()->getTxnNumber()},
                                       boost::none /* autocommit */,
                                       TransactionParticipant::TransactionActions::kNone);
    }

    {
        auto opCtxSession = mongoDSessionCatalog->checkOutSession(opCtx);
        auto txnParticipant = TransactionParticipant::get(opCtx);
        ASSERT_THROWS_CODE(txnParticipant.unstashTransactionResources(opCtx, "insert"),
                           AssertionException,
                           6611000);
    }
}

TEST_F(TxnParticipantTest,
       UnstashRetryableWriteAfterActiveTxnNumberNoLongerCorrespondsToRetryableWrite) {
    auto clientOwned = getServiceContext()->getService()->makeClient("client");
    AlternativeClientRegion acr(clientOwned);
    auto opCtxHolder = cc().makeOperationContext();
    auto opCtx = opCtxHolder.get();
    opCtx->setLogicalSessionId(_sessionId);
    opCtx->setTxnNumber(_txnNumber);

    auto mongoDSessionCatalog = MongoDSessionCatalog::get(opCtx);
    {
        auto opCtxSession = mongoDSessionCatalog->checkOutSession(opCtx);
        auto txnParticipant = TransactionParticipant::get(opCtx);
        txnParticipant.beginOrContinue(opCtx,
                                       {*opCtx->getTxnNumber()},
                                       boost::none /* autocommit */,
                                       TransactionParticipant::TransactionActions::kNone);
        // Invalidate the TransactionParticipant allow the txnNumber to be reused.
        txnParticipant.invalidate(opCtx);
    }

    {
        auto sideClientOwned = getServiceContext()->getService()->makeClient("sideClient");
        AlternativeClientRegion acr(sideClientOwned);
        auto sideOpCtx = cc().makeOperationContext();

        sideOpCtx.get()->setLogicalSessionId(_sessionId);
        sideOpCtx.get()->setTxnNumber(_txnNumber);
        sideOpCtx.get()->setInMultiDocumentTransaction();
        auto sideOpCtxSession = mongoDSessionCatalog->checkOutSession(sideOpCtx.get());
        auto txnParticipant = TransactionParticipant::get(sideOpCtx.get());

        txnParticipant.beginOrContinue(sideOpCtx.get(),
                                       {*sideOpCtx.get()->getTxnNumber()},
                                       false /* autocommit */,
                                       TransactionParticipant::TransactionActions::kStart);
    }

    {
        auto opCtxSession = mongoDSessionCatalog->checkOutSession(opCtx);
        auto txnParticipant = TransactionParticipant::get(opCtx);
        ASSERT_THROWS_CODE(txnParticipant.unstashTransactionResources(opCtx, "insert"),
                           AssertionException,
                           6611001);
    }
}

/**
 * RAII type for operating at a timestamp. Will remove any timestamping when the object destructs.
 */
class OneOffRead {
public:
    OneOffRead(OperationContext* opCtx, const Timestamp& ts) : _opCtx(opCtx) {
        if (shard_role_details::getRecoveryUnit(_opCtx)->isActive()) {
            shard_role_details::getRecoveryUnit(_opCtx)->abandonSnapshot();
        }
        if (ts.isNull()) {
            shard_role_details::getRecoveryUnit(_opCtx)->setTimestampReadSource(
                RecoveryUnit::ReadSource::kNoTimestamp);
        } else {
            shard_role_details::getRecoveryUnit(_opCtx)->setTimestampReadSource(
                RecoveryUnit::ReadSource::kProvided, ts);
        }
    }

    ~OneOffRead() {
        shard_role_details::getRecoveryUnit(_opCtx)->abandonSnapshot();
        shard_role_details::getRecoveryUnit(_opCtx)->setTimestampReadSource(
            RecoveryUnit::ReadSource::kNoTimestamp);
    }

private:
    OperationContext* _opCtx;
};

TEST_F(TxnParticipantTest, AbortSplitPreparedTransaction) {
    // This test simulates:
    // 1) Preparing a transaction with multiple logical sessions as a secondary.
    // 2) Aborting the prepared transaction as a primary.
    //
    // This test asserts that:
    // A) The prepares done by both sessions are done with the same timestamp.
    // B) Aborting the transaction results in the split transaction participants being in the
    //    aborted state.
    // C) The split session entries in the `config.transactions` table are not left in the prepared
    //    state. It is legal for the documents to not exist and it's legal for them to be aborted.
    //
    // First we set up infrastructure such that we can simulate oplog application.
    OperationContext* opCtx = this->opCtx();
    DurableHistoryRegistry::set(opCtx->getServiceContext(),
                                std::make_unique<DurableHistoryRegistry>());
    opCtx->getServiceContext()->setOpObserver(
        std::make_unique<OpObserverImpl>(std::make_unique<OperationLoggerImpl>()));

    OpDebug* const nullOpDbg = nullptr;

    dynamic_cast<repl::ReplicationCoordinatorMock*>(repl::ReplicationCoordinator::get(opCtx))
        ->setUpdateCommittedSnapshot(false);
    // Initiate the term from 0 to 1 for familiarity.
    ASSERT_OK(repl::ReplicationCoordinator::get(opCtx)->updateTerm(opCtx, 1));

    // Bump the logical clock for easier visual cues.
    const Timestamp startTs(100, 1);
    auto oplogInfo = LocalOplogInfo::get(opCtx);
    oplogInfo->setNewTimestamp(opCtx->getServiceContext(), startTs);

    // Assign the variables that represent the "real", client-facing logical session.
    std::unique_ptr<MongoDSessionCatalog::Session> userSession = checkOutSession();
    TransactionParticipant::Participant userTxnParticipant = TransactionParticipant::get(opCtx);

    // TxnResources start in the "stashed" state.
    userTxnParticipant.unstashTransactionResources(opCtx, "crud ops");

    // Hold the collection lock/datastructure such that it can be released prior to rollback.
    boost::optional<AutoGetCollection> userColl;
    userColl.emplace(opCtx, kNss, LockMode::MODE_IX);

    // We split our user session into 2 split sessions.
    const std::vector<uint32_t> requesterIds{1, 3};
    auto* splitPrepareManager =
        repl::ReplicationCoordinator::get(opCtx)->getSplitPrepareSessionManager();
    const std::vector<repl::SplitSessionInfo>& splitSessions = splitPrepareManager->splitSession(
        opCtx->getLogicalSessionId().get(), opCtx->getTxnNumber().get(), requesterIds);
    // Insert an `_id: 1` document.
    callUnderSplitSession(splitSessions[0].session, [nullOpDbg](OperationContext* opCtx) {
        AutoGetCollection userColl(opCtx, kNss, LockMode::MODE_IX);
        ASSERT_OK(
            collection_internal::insertDocument(opCtx,
                                                userColl.getCollection(),
                                                InsertStatement(BSON("_id" << 1 << "value" << 1)),
                                                nullOpDbg));
    });

    // Insert an `_id: 2` document.
    callUnderSplitSession(splitSessions[1].session, [nullOpDbg](OperationContext* opCtx) {
        AutoGetCollection userColl(opCtx, kNss, LockMode::MODE_IX);
        ASSERT_OK(
            collection_internal::insertDocument(opCtx,
                                                userColl.getCollection(),
                                                InsertStatement(BSON("_id" << 2 << "value" << 1)),
                                                nullOpDbg));
    });

    // Mimic the methods to call for a secondary performing a split prepare. Those are called inside
    // `UnreplicatedWritesBlock` and explicitly pass in the prepare OpTime.
    const Timestamp prepTs = startTs;
    const repl::OpTime prepOpTime(prepTs, 1);
    callUnderSplitSession(splitSessions[0].session, [prepOpTime](OperationContext* opCtx) {
        auto txnParticipant = TransactionParticipant::get(opCtx);
        txnParticipant.prepareTransaction(opCtx, prepOpTime);
    });
    callUnderSplitSession(splitSessions[1].session, [prepOpTime](OperationContext* opCtx) {
        auto txnParticipant = TransactionParticipant::get(opCtx);
        txnParticipant.prepareTransaction(opCtx, prepOpTime);
    });

    // Normally this would also be called on a secondary under a `UnreplicatedWritesBlock`. However
    // we must change the `config.transactions` state for this logical session. In production, that
    // transaction table write would come via a synthetic oplog entry.
    userTxnParticipant.prepareTransaction(opCtx, prepOpTime);

    // Assert for each split session that they are:
    // 1) Prepared at the expected prepare timestamp.
    // 2) Have an active recovery unit.
    // 3) The recovery unit has the expected prepare timestamp.
    for (const auto& splitSession : splitSessions) {
        callUnderSplitSessionNoUnstash(
            splitSession.session,
            [&](OperationContext* opCtx, TransactionParticipant::Participant& txnParticipant) {
                ASSERT_EQ(prepOpTime, txnParticipant.getPrepareOpTime());
                ASSERT(txnParticipant.getTxnResourceStashRecoveryUnitForTest()->isActive());
                ASSERT_EQ(
                    prepOpTime.getTimestamp(),
                    txnParticipant.getTxnResourceStashRecoveryUnitForTest()->getPrepareTimestamp());
            });
    }

    // Aborting a transaction destroys the split session objects. Store the LSIDs for a post-abort
    // transaction table lookup.
    std::vector<LogicalSessionId> splitLSIDs{splitSessions[0].session.getSessionId(),
                                             splitSessions[1].session.getSessionId()};
    userTxnParticipant.abortTransaction(opCtx);

    // The `findOne` helpers invariant by default if no result is found.
    const bool invariantOnError = true;

    // To claim we rolled back all of the split prepared recovery units, we check that:
    // 1) A read cannot see the document. If it can, the split transaction was committed.
    // 2) A write does not generate a write conflict. If it does, the split transaction was left
    //    open.
    {
        OneOffRead oor(opCtx, Timestamp());
        BSONObj userWrite = Helpers::findOneForTesting(
            opCtx, userColl->getCollection(), BSON("_id" << 1), !invariantOnError);
        ASSERT(userWrite.isEmpty());
        userWrite = Helpers::findOneForTesting(
            opCtx, userColl->getCollection(), BSON("_id" << 2), !invariantOnError);
        ASSERT(userWrite.isEmpty());
    }

    {
        WriteUnitOfWork wuow(opCtx);
        ASSERT_DOES_NOT_THROW(auto _ = collection_internal::insertDocument(
                                  opCtx,
                                  userColl->getCollection(),
                                  InsertStatement(BSON("_id" << 1 << "value" << 1)),
                                  nullOpDbg));
    }

    // Assert that the TxnParticipant for the split sessions are in the "aborted prepared
    // transaction" state.
    for (const auto& splitSession : splitLSIDs) {
        const TxnNumber unneeded(0);
        callUnderSplitSessionNoUnstash(
            InternalSessionPool::Session(splitSession, unneeded),
            [&](OperationContext* opCtx, TransactionParticipant::Participant& txnParticipant) {
                ASSERT(txnParticipant.transactionIsAborted());
                ASSERT(!txnParticipant.transactionIsAbortedWithoutPrepare());
            });
    }

    AutoGetCollection configTransactions(
        opCtx, NamespaceString::kSessionTransactionsTableNamespace, LockMode::MODE_IS);
    {
        OneOffRead oor(opCtx, Timestamp());
        BSONObj userTxnObj =
            Helpers::findOneForTesting(opCtx,
                                       configTransactions.getCollection(),
                                       BSON("_id.id" << opCtx->getLogicalSessionId()->getId()),
                                       !invariantOnError);
        ASSERT(!userTxnObj.isEmpty());
        // Assert that the user config.transaction document is in the aborted state.
        assertSessionState(userTxnObj, DurableTxnStateEnum::kAborted);
    }

    // Rather than testing the implementation, we'll assert on the weakest necessary state. A split
    // `config.transactions` document may or may not exist. If it exists, it must not* be in the
    // "prepared" state.
    for (const LogicalSessionId& splitLSID : splitLSIDs) {
        OneOffRead oor(opCtx, Timestamp());
        BSONObj splitTxnObj = Helpers::findOneForTesting(opCtx,
                                                         configTransactions.getCollection(),
                                                         BSON("_id.id" << splitLSID.getId()),
                                                         !invariantOnError);
        if (!splitTxnObj.isEmpty()) {
            assertNotInSessionState(splitTxnObj, DurableTxnStateEnum::kPrepared);
        }
    }
}

TEST_F(TxnParticipantTest, CommitSplitPreparedTransaction) {
    // This test simulates:
    // 1) Preparing a transaction with multiple logical sessions as a secondary.
    // 2) Committing the transaction as a primary.
    // 3) Rolling back the commit.
    //
    // This test asserts that:
    // A) The writes done by both sessions are committed with the same "visible" timestamp.
    // B) The writes done by both sessions rollback, due to having a "durable" timestamp that's
    //    ahead of the stable timestamp.
    // C) The `config.transactions` table is not aware that the split sessions were part of a
    //    prepared transaction. One example of why this invariant is important is because
    //    reconstructing prepared transactions finds all `config.transactions` entries in the
    //    prepared state. We must ensure any prepared transaction is only reconstructed once.
    //
    // First we set up infrastructure such that we can simulate oplog application, primary behaviors
    // and `rollbackToStable`.
    OperationContext* opCtx = this->opCtx();
    DurableHistoryRegistry::set(opCtx->getServiceContext(),
                                std::make_unique<DurableHistoryRegistry>());
    opCtx->getServiceContext()->setOpObserver(
        std::make_unique<OpObserverImpl>(std::make_unique<OperationLoggerImpl>()));

    OpDebug* const nullOpDbg = nullptr;

    dynamic_cast<repl::ReplicationCoordinatorMock*>(repl::ReplicationCoordinator::get(opCtx))
        ->setUpdateCommittedSnapshot(false);
    // Initiate the term from 0 to 1 for familiarity.
    ASSERT_OK(repl::ReplicationCoordinator::get(opCtx)->updateTerm(opCtx, 1));

    // Bump the logical clock for easier visual cues.
    const Timestamp startTs(100, 1);
    auto oplogInfo = LocalOplogInfo::get(opCtx);
    oplogInfo->setNewTimestamp(opCtx->getServiceContext(), startTs);
    opCtx->getServiceContext()->getStorageEngine()->setInitialDataTimestamp(startTs);

    // Assign the variables that represent the "real", client-facing logical session.
    std::unique_ptr<MongoDSessionCatalog::Session> userSession = checkOutSession();
    TransactionParticipant::Participant userTxnParticipant = TransactionParticipant::get(opCtx);

    // TxnResources start in the "stashed" state.
    userTxnParticipant.unstashTransactionResources(opCtx, "crud ops");

    // Hold the collection lock/data structure such that it can be released prior to rollback.
    boost::optional<AutoGetCollection> userColl;
    userColl.emplace(opCtx, kNss, LockMode::MODE_IX);

    // We split our user session into 2 split sessions.
    const std::vector<uint32_t> requesterIds{1, 3};
    auto* splitPrepareManager =
        repl::ReplicationCoordinator::get(opCtx)->getSplitPrepareSessionManager();
    const auto& splitSessions = splitPrepareManager->splitSession(
        opCtx->getLogicalSessionId().get(), opCtx->getTxnNumber().get(), requesterIds);
    // Insert an `_id: 1` document.
    callUnderSplitSession(splitSessions[0].session, [nullOpDbg](OperationContext* opCtx) {
        AutoGetCollection userColl(opCtx, kNss, LockMode::MODE_IX);
        ASSERT_OK(
            collection_internal::insertDocument(opCtx,
                                                userColl.getCollection(),
                                                InsertStatement(BSON("_id" << 1 << "value" << 1)),
                                                nullOpDbg));
    });

    // Insert an `_id: 2` document.
    callUnderSplitSession(splitSessions[1].session, [nullOpDbg](OperationContext* opCtx) {
        AutoGetCollection userColl(opCtx, kNss, LockMode::MODE_IX);
        ASSERT_OK(
            collection_internal::insertDocument(opCtx,
                                                userColl.getCollection(),
                                                InsertStatement(BSON("_id" << 2 << "value" << 1)),
                                                nullOpDbg));
    });

    // Update `2` to increment its `value` to 2. This must be done in the same split session as the
    // insert.
    callUnderSplitSession(splitSessions[1].session, [nullOpDbg](OperationContext* opCtx) {
        auto userColl = acquireCollection(
            opCtx,
            CollectionAcquisitionRequest::fromOpCtx(opCtx, kNss, AcquisitionPrerequisites::kWrite),
            MODE_IX);
        Helpers::update(opCtx, userColl, BSON("_id" << 2), BSON("$inc" << BSON("value" << 1)));
    });

    // Mimic the methods to call for a secondary performing a split prepare. Those are called inside
    // `UnreplicatedWritesBlock` and explicitly pass in the prepare OpTime.
    const Timestamp prepTs = startTs;
    const repl::OpTime prepOpTime(prepTs, 1);
    callUnderSplitSession(splitSessions[0].session, [prepOpTime](OperationContext* opCtx) {
        auto txnParticipant = TransactionParticipant::get(opCtx);
        txnParticipant.prepareTransaction(opCtx, prepOpTime);
    });
    callUnderSplitSession(splitSessions[1].session, [prepOpTime](OperationContext* opCtx) {
        auto txnParticipant = TransactionParticipant::get(opCtx);
        txnParticipant.prepareTransaction(opCtx, prepOpTime);
    });

    // Normally this would also be called on a secondary under a `UnreplicatedWritesBlock`. However
    // we must change the `config.transactions` state for this logical session. In production, that
    // transaction table write would come via a synthetic oplog entry.
    userTxnParticipant.prepareTransaction(opCtx, prepOpTime);

    // Prepared transactions are dictated a commit/visible from the transaction coordinator. However
    // the commit function will assign a "durable" timestamp derived from the system clock. This is
    // the code path of a primary.
    const Timestamp visibleTs = prepTs + 100;

    // Committing a transaction as a primary does not allow direct control on the oplog/durable
    // timestamp being allocated. We want to observe that we correctly set a durable timestamp when
    // a primary commits a split prepare. Ultimately we'll observe that by seeing the transaction
    // rollback on a call to `rollbackToStable`. We can coerce the durable timestamp by bumping the
    // logical clock. Worth noting, the test is reading from the system clock which is expected to
    // choose a value for the transaction's durable timestamp that is much much larger than
    // `chosenStableTimestamp`. We only bump the clock here in the very unexpected case that the
    // system clock is within few hundred seconds of the epoch.
    const Timestamp chosenStableTimestamp = visibleTs + 10;
    oplogInfo->setNewTimestamp(opCtx->getServiceContext(), chosenStableTimestamp + 1);

    // Committing a transaction destroys the split session objects. Store the LSIDs for a post-abort
    // transaction table lookup.
    std::vector<LogicalSessionId> splitLSIDs{splitSessions[0].session.getSessionId(),
                                             splitSessions[1].session.getSessionId()};
    userTxnParticipant.commitPreparedTransaction(opCtx, visibleTs, boost::none);
    ASSERT_LT(chosenStableTimestamp, userTxnParticipant.getLastWriteOpTime().getTimestamp());

    // The `findOne` helpers will invariant by default if no result is found.
    const bool invariantOnError = true;
    // Print out reads at the interesting times prior to asserting for diagnostics.
    for (const auto& ts : std::vector<Timestamp>{prepTs, visibleTs}) {
        OneOffRead oor(opCtx, ts);
        LOGV2(7274401,
              "Pre-rollback values",
              "readTimestamp"_attr = ts,
              "Doc(_id: 1)"_attr = Helpers::findOneForTesting(
                  opCtx, userColl->getCollection(), BSON("_id" << 1), !invariantOnError),
              "Doc(_id: 2)"_attr = Helpers::findOneForTesting(
                  opCtx, userColl->getCollection(), BSON("_id" << 2), !invariantOnError));
    }

    // Reading at the prepare timestamp should not see anything.
    {
        OneOffRead oor(opCtx, prepTs);
        shard_role_details::getRecoveryUnit(opCtx)->setPrepareConflictBehavior(
            PrepareConflictBehavior::kIgnoreConflicts);
        ASSERT_BSONOBJ_EQ(
            BSONObj::kEmptyObject,
            Helpers::findOneForTesting(
                opCtx, userColl->getCollection(), BSON("_id" << 1), !invariantOnError));
        ASSERT_BSONOBJ_EQ(
            BSONObj::kEmptyObject,
            Helpers::findOneForTesting(
                opCtx, userColl->getCollection(), BSON("_id" << 2), !invariantOnError));
    }

    // Reading at the visible/commit timestamp should see the inserted and updated documents.
    {
        OneOffRead oor(opCtx, visibleTs);
        ASSERT_BSONOBJ_EQ(
            BSON("_id" << 1 << "value" << 1),
            Helpers::findOneForTesting(
                opCtx, userColl->getCollection(), BSON("_id" << 1), !invariantOnError));
        ASSERT_BSONOBJ_EQ(
            BSON("_id" << 2 << "value" << 2),
            Helpers::findOneForTesting(
                opCtx, userColl->getCollection(), BSON("_id" << 2), !invariantOnError));
    }

    {
        // We also assert that the user transaction record is in the committed state.
        AutoGetCollection configTransactions(
            opCtx, NamespaceString::kSessionTransactionsTableNamespace, LockMode::MODE_IS);
        // The user config.transactions document must exist and must be in the "committed" state.
        BSONObj userTxnObj =
            Helpers::findOneForTesting(opCtx,
                                       configTransactions.getCollection(),
                                       BSON("_id.id" << opCtx->getLogicalSessionId()->getId()),
                                       !invariantOnError);
        ASSERT(!userTxnObj.isEmpty());
        assertSessionState(userTxnObj, DurableTxnStateEnum::kCommitted);

        // Rather than testing the implementation, we'll assert on the weakest necessary state. A
        // split `config.transactions` document may or may not exist. If it exists, it must be
        // in the "committed" state.
        for (std::size_t idx = 0; idx < splitLSIDs.size(); ++idx) {
            BSONObj splitTxnObj =
                Helpers::findOneForTesting(opCtx,
                                           configTransactions.getCollection(),
                                           BSON("_id.id" << splitLSIDs[idx].getId()),
                                           !invariantOnError);
            if (!splitTxnObj.isEmpty()) {
                assertSessionState(splitTxnObj, DurableTxnStateEnum::kCommitted);
            }
        }
    }

    // Unlock the collection and check in the session to release locks.
    userColl = boost::none;
    userSession.reset();

    // Rollback such that the commit oplog entry and the effects of the transaction are rolled
    // back. Unset some of the multi-doc transaction state because it's illegal to request the
    // global lock in strong mode when the operation context has been part of a multi-doc
    // transaction.
    const auto lsid = *opCtx->getLogicalSessionId();
    const auto txnNum = *opCtx->getTxnNumber();
    opCtx->resetMultiDocumentTransactionState();
    opCtx->getServiceContext()->getStorageEngine()->setStableTimestamp(chosenStableTimestamp);
    {
        Lock::GlobalLock globalLock(opCtx, LockMode::MODE_X);
        ASSERT_OK(opCtx->getServiceContext()->getStorageEngine()->recoverToStableTimestamp(opCtx));
    }
    opCtx->setLogicalSessionId(lsid);
    opCtx->setTxnNumber(txnNum);

    // Again, display read values for diagnostics.
    userColl.emplace(opCtx, kNss, LockMode::MODE_IX);
    for (const auto& ts : std::vector<Timestamp>{prepTs, visibleTs}) {
        OneOffRead oor(opCtx, ts);
        LOGV2(7274402,
              "Post-rollback values",
              "readTimestamp"_attr = ts,
              "Doc(_id: 1)"_attr = Helpers::findOneForTesting(
                  opCtx, userColl->getCollection(), BSON("_id" << 1), !invariantOnError),
              "Doc(_id: 2)"_attr = Helpers::findOneForTesting(
                  opCtx, userColl->getCollection(), BSON("_id" << 2), !invariantOnError));
    }

    // Now when we read at the commit/visible timestamp, the documents must not exist.
    {
        OneOffRead oor(opCtx, visibleTs);
        ASSERT_BSONOBJ_EQ(
            BSONObj::kEmptyObject,
            Helpers::findOneForTesting(
                opCtx, userColl->getCollection(), BSON("_id" << 1), !invariantOnError));
        ASSERT_BSONOBJ_EQ(
            BSONObj::kEmptyObject,
            Helpers::findOneForTesting(
                opCtx, userColl->getCollection(), BSON("_id" << 2), !invariantOnError));
    }

    // We also assert that the user transaction record is in the prepared state. The split sessions
    // are not.
    AutoGetCollection configTransactions(
        opCtx, NamespaceString::kSessionTransactionsTableNamespace, LockMode::MODE_IS);
    // The user `config.transactions` document must be in the "prepared" state.
    BSONObj userTxnObj =
        Helpers::findOneForTesting(opCtx,
                                   configTransactions.getCollection(),
                                   BSON("_id.id" << opCtx->getLogicalSessionId()->getId()),
                                   !invariantOnError);
    ASSERT(!userTxnObj.isEmpty());
    assertSessionState(userTxnObj, DurableTxnStateEnum::kPrepared);

    // Rather than testing the implementation, we'll assert on the weakest necessary state. A split
    // `config.transactions` document may or may not exist. If it exists, it must not* be in the
    // "prepared" state.
    for (std::size_t idx = 0; idx < splitLSIDs.size(); ++idx) {
        BSONObj splitTxnObj = Helpers::findOneForTesting(opCtx,
                                                         configTransactions.getCollection(),
                                                         BSON("_id.id" << splitLSIDs[idx].getId()),
                                                         !invariantOnError);
        if (!splitTxnObj.isEmpty()) {
            assertNotInSessionState(splitTxnObj, DurableTxnStateEnum::kPrepared);
        }
    }
}

bool doesExistInCatalog(const LogicalSessionId& lsid, SessionCatalog* sessionCatalog) {
    bool existsInCatalog{false};
    sessionCatalog->scanSession(lsid,
                                [&](const ObservableSession& session) { existsInCatalog = true; });
    return existsInCatalog;
}

TEST_F(ShardTxnParticipantTest, EagerlyReapRetryableSessionsUponNewClientTransaction) {
    auto sessionCatalog = SessionCatalog::get(getServiceContext());
    ASSERT_EQ(sessionCatalog->size(), 0);

    // Add a parent session with one retryable child.

    auto parentLsid = makeLogicalSessionIdForTest();
    auto parentTxnNumber = 0;
    runRetryableWrite(parentLsid, parentTxnNumber);

    auto retryableChildLsid =
        makeLogicalSessionIdWithTxnNumberAndUUIDForTest(parentLsid, parentTxnNumber);
    runAndCommitTransaction(retryableChildLsid, 0);

    ASSERT_EQ(sessionCatalog->size(), 1);
    ASSERT(doesExistInCatalog(parentLsid, sessionCatalog));
    ASSERT(doesExistInCatalog(retryableChildLsid, sessionCatalog));

    // Start a higher txnNumber client transaction and verify the child was erased.

    parentTxnNumber++;
    runAndCommitTransaction(parentLsid, parentTxnNumber);

    ASSERT_EQ(sessionCatalog->size(), 1);
    ASSERT(doesExistInCatalog(parentLsid, sessionCatalog));
    ASSERT_FALSE(doesExistInCatalog(retryableChildLsid, sessionCatalog));
}

TEST_F(ShardTxnParticipantTest, EagerlyReapRetryableSessionsUponNewClientRetryableWrite) {
    auto sessionCatalog = SessionCatalog::get(getServiceContext());
    ASSERT_EQ(sessionCatalog->size(), 0);

    // Add a parent session with one retryable child.

    auto parentLsid = makeLogicalSessionIdForTest();
    auto parentTxnNumber = 0;
    runRetryableWrite(parentLsid, parentTxnNumber);

    auto retryableChildLsid =
        makeLogicalSessionIdWithTxnNumberAndUUIDForTest(parentLsid, parentTxnNumber);
    runAndCommitTransaction(retryableChildLsid, 0);

    ASSERT_EQ(sessionCatalog->size(), 1);
    ASSERT(doesExistInCatalog(parentLsid, sessionCatalog));
    ASSERT(doesExistInCatalog(retryableChildLsid, sessionCatalog));

    // Start a higher txnNumber retryable write and verify the child was erased.

    parentTxnNumber++;
    runRetryableWrite(parentLsid, parentTxnNumber);

    ASSERT_EQ(sessionCatalog->size(), 1);
    ASSERT(doesExistInCatalog(parentLsid, sessionCatalog));
    ASSERT_FALSE(doesExistInCatalog(retryableChildLsid, sessionCatalog));
}

TEST_F(ShardTxnParticipantTest, EagerlyReapRetryableSessionsUponNewRetryableTransaction) {
    auto sessionCatalog = SessionCatalog::get(getServiceContext());
    ASSERT_EQ(sessionCatalog->size(), 0);

    // Add a parent session with one retryable child.

    auto parentLsid = makeLogicalSessionIdForTest();
    auto parentTxnNumber = 0;
    runRetryableWrite(parentLsid, parentTxnNumber);

    auto retryableChildLsid =
        makeLogicalSessionIdWithTxnNumberAndUUIDForTest(parentLsid, parentTxnNumber);
    runAndCommitTransaction(retryableChildLsid, 0);

    ASSERT_EQ(sessionCatalog->size(), 1);
    ASSERT(doesExistInCatalog(parentLsid, sessionCatalog));
    ASSERT(doesExistInCatalog(retryableChildLsid, sessionCatalog));

    // Start a higher txnNumber retryable transaction and verify the child was erased.

    parentTxnNumber++;
    auto higherRetryableChildLsid =
        makeLogicalSessionIdWithTxnNumberAndUUIDForTest(parentLsid, parentTxnNumber);
    runAndCommitTransaction(higherRetryableChildLsid, 0);

    ASSERT_EQ(sessionCatalog->size(), 1);
    ASSERT(doesExistInCatalog(parentLsid, sessionCatalog));
    ASSERT_FALSE(doesExistInCatalog(retryableChildLsid, sessionCatalog));
    ASSERT(doesExistInCatalog(higherRetryableChildLsid, sessionCatalog));
}

TEST_F(
    ShardTxnParticipantTest,
    EagerlyReapRetryableSessionsOnlyUponNewTransactionBegunAndIgnoresNonRetryableAndUnrelatedSessions) {
    auto sessionCatalog = SessionCatalog::get(getServiceContext());
    ASSERT_EQ(sessionCatalog->size(), 0);

    // Add a parent session with two retryable children and one non-retryable child.

    auto parentLsid = makeLogicalSessionIdForTest();
    auto parentTxnNumber = 0;
    runRetryableWrite(parentLsid, parentTxnNumber);

    auto retryableChildLsid1 =
        makeLogicalSessionIdWithTxnNumberAndUUIDForTest(parentLsid, parentTxnNumber);
    runAndCommitTransaction(retryableChildLsid1, 0);

    auto retryableChildLsid2 =
        makeLogicalSessionIdWithTxnNumberAndUUIDForTest(parentLsid, parentTxnNumber);
    runAndCommitTransaction(retryableChildLsid2, 0);

    auto nonRetryableChildLsid = makeLogicalSessionIdWithTxnUUIDForTest(parentLsid);
    runAndCommitTransaction(nonRetryableChildLsid, 0);

    // Add entries for unrelated sessions to verify they aren't affected.

    auto parentLsidOther = makeLogicalSessionIdForTest();
    runRetryableWrite(parentLsidOther, 0);

    auto retryableChildLsidOther =
        makeLogicalSessionIdWithTxnNumberAndUUIDForTest(parentLsidOther, parentTxnNumber);
    runAndCommitTransaction(retryableChildLsidOther, 0);

    auto nonRetryableChildLsidOther =
        makeLogicalSessionIdWithTxnNumberAndUUIDForTest(parentLsidOther, parentTxnNumber);
    runAndCommitTransaction(nonRetryableChildLsidOther, 0);

    ASSERT_EQ(sessionCatalog->size(), 2);
    ASSERT(doesExistInCatalog(parentLsid, sessionCatalog));
    ASSERT(doesExistInCatalog(nonRetryableChildLsid, sessionCatalog));
    ASSERT(doesExistInCatalog(retryableChildLsid1, sessionCatalog));
    ASSERT(doesExistInCatalog(retryableChildLsid2, sessionCatalog));
    ASSERT(doesExistInCatalog(parentLsidOther, sessionCatalog));
    ASSERT(doesExistInCatalog(retryableChildLsidOther, sessionCatalog));
    ASSERT(doesExistInCatalog(nonRetryableChildLsidOther, sessionCatalog));

    // Check out with a higher txnNumber and verify we don't reap until a transaction has begun on
    // it.

    parentTxnNumber++;

    auto mongoDSessionCatalog = MongoDSessionCatalog::get(getServiceContext());

    // Does not call beginOrContinue.
    runFunctionFromDifferentOpCtx([&](OperationContext* opCtx) {
        opCtx->setLogicalSessionId(parentLsid);
        opCtx->setTxnNumber(parentTxnNumber);
        auto opCtxSession = mongoDSessionCatalog->checkOutSession(opCtx);
    });
    // Does not call beginOrContinue.
    auto higherRetryableChildLsid =
        makeLogicalSessionIdWithTxnNumberAndUUIDForTest(parentLsid, parentTxnNumber);
    runFunctionFromDifferentOpCtx([&](OperationContext* opCtx) {
        opCtx->setLogicalSessionId(higherRetryableChildLsid);
        opCtx->setTxnNumber(0);
        opCtx->setInMultiDocumentTransaction();
        auto opCtxSession = mongoDSessionCatalog->checkOutSession(opCtx);
    });
    // beginOrContinue fails because no startTransaction=true.
    runFunctionFromDifferentOpCtx([&](OperationContext* opCtx) {
        opCtx->setLogicalSessionId(parentLsid);
        opCtx->setTxnNumber(parentTxnNumber);
        opCtx->setInMultiDocumentTransaction();
        auto opCtxSession = mongoDSessionCatalog->checkOutSession(opCtx);

        auto txnParticipant = TransactionParticipant::get(opCtx);
        ASSERT_THROWS_CODE(
            txnParticipant.beginOrContinue(opCtx,
                                           {*opCtx->getTxnNumber()},
                                           false,
                                           TransactionParticipant::TransactionActions::kContinue),
            AssertionException,
            ErrorCodes::NoSuchTransaction);
    });
    // Non-retryable child sessions shouldn't affect retryable sessions.
    auto newNonRetryableChildLsid = makeLogicalSessionIdWithTxnUUIDForTest(parentLsid);
    runAndCommitTransaction(newNonRetryableChildLsid, 0);

    // No sessions should have been reaped.
    ASSERT_EQ(sessionCatalog->size(), 2);
    ASSERT(doesExistInCatalog(parentLsid, sessionCatalog));
    ASSERT(doesExistInCatalog(nonRetryableChildLsid, sessionCatalog));
    ASSERT(doesExistInCatalog(retryableChildLsid1, sessionCatalog));
    ASSERT(doesExistInCatalog(retryableChildLsid2, sessionCatalog));
    ASSERT(doesExistInCatalog(parentLsidOther, sessionCatalog));
    ASSERT(doesExistInCatalog(retryableChildLsidOther, sessionCatalog));
    ASSERT(doesExistInCatalog(nonRetryableChildLsidOther, sessionCatalog));

    // Call beginOrContinue for a higher txnNumber and verify we do erase old sessions for the
    // active session.

    runRetryableWrite(parentLsid, parentTxnNumber);

    // The two retryable children for parentLsid should have been reaped.
    ASSERT_EQ(sessionCatalog->size(), 2);
    ASSERT(doesExistInCatalog(parentLsid, sessionCatalog));
    ASSERT(doesExistInCatalog(nonRetryableChildLsid, sessionCatalog));
    ASSERT_FALSE(doesExistInCatalog(retryableChildLsid1, sessionCatalog));
    ASSERT_FALSE(doesExistInCatalog(retryableChildLsid2, sessionCatalog));
    ASSERT(doesExistInCatalog(parentLsidOther, sessionCatalog));
    ASSERT(doesExistInCatalog(retryableChildLsidOther, sessionCatalog));
    ASSERT(doesExistInCatalog(nonRetryableChildLsidOther, sessionCatalog));
}

TEST_F(ShardTxnParticipantTest, EagerlyReapLowerTxnNumbers) {
    auto sessionCatalog = SessionCatalog::get(getServiceContext());
    ASSERT_EQ(sessionCatalog->size(), 0);

    // Add a parent session with two retryable children.

    auto parentLsid = makeLogicalSessionIdForTest();
    auto parentTxnNumber = 1;
    runRetryableWrite(parentLsid, parentTxnNumber);

    auto retryableChildLsid1 =
        makeLogicalSessionIdWithTxnNumberAndUUIDForTest(parentLsid, parentTxnNumber);
    runAndCommitTransaction(retryableChildLsid1, 0);

    auto retryableChildLsid2 =
        makeLogicalSessionIdWithTxnNumberAndUUIDForTest(parentLsid, parentTxnNumber);
    runAndCommitTransaction(retryableChildLsid2, 0);

    auto lowerRetryableChildLsid =
        makeLogicalSessionIdWithTxnNumberAndUUIDForTest(parentLsid, parentTxnNumber - 1);
    runFunctionFromDifferentOpCtx([&](OperationContext* opCtx) {
        opCtx->setLogicalSessionId(lowerRetryableChildLsid);
        opCtx->setTxnNumber(0);
        opCtx->setInMultiDocumentTransaction();
        auto mongoDSessionCatalog = MongoDSessionCatalog::get(opCtx);
        auto opCtxSession = mongoDSessionCatalog->checkOutSession(opCtx);
    });

    ASSERT_EQ(sessionCatalog->size(), 1);
    ASSERT(doesExistInCatalog(parentLsid, sessionCatalog));
    ASSERT(doesExistInCatalog(retryableChildLsid1, sessionCatalog));
    ASSERT(doesExistInCatalog(retryableChildLsid2, sessionCatalog));
    ASSERT(doesExistInCatalog(lowerRetryableChildLsid, sessionCatalog));

    // Start a higher txnNumber retryable transaction and verify the child was erased.

    parentTxnNumber++;
    runRetryableWrite(parentLsid, parentTxnNumber);

    ASSERT_EQ(sessionCatalog->size(), 1);
    ASSERT(doesExistInCatalog(parentLsid, sessionCatalog));
    ASSERT_FALSE(doesExistInCatalog(retryableChildLsid1, sessionCatalog));
    ASSERT_FALSE(doesExistInCatalog(retryableChildLsid2, sessionCatalog));
    ASSERT_FALSE(doesExistInCatalog(lowerRetryableChildLsid, sessionCatalog));
}

TEST_F(ShardTxnParticipantTest, EagerlyReapSkipsHigherUnusedTxnNumbers) {
    auto sessionCatalog = SessionCatalog::get(getServiceContext());
    ASSERT_EQ(sessionCatalog->size(), 0);

    // Add a parent session with one retryable child.

    auto parentLsid = makeLogicalSessionIdForTest();
    auto parentTxnNumber = 1;
    runRetryableWrite(parentLsid, parentTxnNumber);

    auto retryableChildLsid =
        makeLogicalSessionIdWithTxnNumberAndUUIDForTest(parentLsid, parentTxnNumber);
    runAndCommitTransaction(retryableChildLsid, 0);

    ASSERT_EQ(sessionCatalog->size(), 1);
    ASSERT(doesExistInCatalog(parentLsid, sessionCatalog));
    ASSERT(doesExistInCatalog(retryableChildLsid, sessionCatalog));

    // Check out a higher txnNumber retryable transaction but do not start it and verify it does not
    // reap and is not reaped.

    auto higherUnusedRetryableChildLsid =
        makeLogicalSessionIdWithTxnNumberAndUUIDForTest(parentLsid, parentTxnNumber + 10);
    runFunctionFromDifferentOpCtx([&](OperationContext* opCtx) {
        opCtx->setLogicalSessionId(higherUnusedRetryableChildLsid);
        opCtx->setTxnNumber(0);
        opCtx->setInMultiDocumentTransaction();
        auto mongoDSessionCatalog = MongoDSessionCatalog::get(opCtx);
        auto opCtxSession = mongoDSessionCatalog->checkOutSession(opCtx);
    });

    ASSERT_EQ(sessionCatalog->size(), 1);
    ASSERT(doesExistInCatalog(parentLsid, sessionCatalog));
    ASSERT(doesExistInCatalog(retryableChildLsid, sessionCatalog));
    ASSERT(doesExistInCatalog(higherUnusedRetryableChildLsid, sessionCatalog));

    parentTxnNumber++;  // Still less than in higherUnusedRetryableChildLsid.
    runRetryableWrite(parentLsid, parentTxnNumber);

    ASSERT_EQ(sessionCatalog->size(), 1);
    ASSERT(doesExistInCatalog(parentLsid, sessionCatalog));
    ASSERT_FALSE(doesExistInCatalog(retryableChildLsid, sessionCatalog));
    ASSERT(doesExistInCatalog(higherUnusedRetryableChildLsid, sessionCatalog));
}

TEST_F(ShardTxnParticipantTest, EagerlyReapSkipsKilledSessions) {
    auto sessionCatalog = SessionCatalog::get(getServiceContext());
    ASSERT_EQ(sessionCatalog->size(), 0);

    // Add a parent session with two retryable children.

    auto parentLsid = makeLogicalSessionIdForTest();
    auto parentTxnNumber = 1;
    runRetryableWrite(parentLsid, parentTxnNumber);

    auto retryableChildLsid1 =
        makeLogicalSessionIdWithTxnNumberAndUUIDForTest(parentLsid, parentTxnNumber);
    runAndCommitTransaction(retryableChildLsid1, 0);

    auto retryableChildLsid2 =
        makeLogicalSessionIdWithTxnNumberAndUUIDForTest(parentLsid, parentTxnNumber);
    runAndCommitTransaction(retryableChildLsid2, 0);

    ASSERT_EQ(sessionCatalog->size(), 1);
    ASSERT(doesExistInCatalog(parentLsid, sessionCatalog));
    ASSERT(doesExistInCatalog(retryableChildLsid1, sessionCatalog));
    ASSERT(doesExistInCatalog(retryableChildLsid2, sessionCatalog));

    // Kill one retryable child and verify no sessions in its SRI can be reaped until it has been
    // checked out by its killer.

    parentTxnNumber++;
    boost::optional<SessionCatalog::KillToken> killToken;
    runFunctionFromDifferentOpCtx([&](OperationContext* opCtx) {
        opCtx->setLogicalSessionId(parentLsid);
        opCtx->setTxnNumber(parentTxnNumber);
        auto mongoDSessionCatalog = MongoDSessionCatalog::get(opCtx);
        auto opCtxSession = mongoDSessionCatalog->checkOutSession(opCtx);

        TransactionParticipant::get(opCtx).beginOrContinue(
            opCtx,
            {*opCtx->getTxnNumber()},
            boost::none,
            TransactionParticipant::TransactionActions::kNone);

        // Kill after checking out the session because we can't check out the session again
        // after a kill without checking out with the killToken first.
        killToken = sessionCatalog->killSession(retryableChildLsid1);
    });

    ASSERT_EQ(sessionCatalog->size(), 1);
    ASSERT(doesExistInCatalog(parentLsid, sessionCatalog));
    ASSERT(doesExistInCatalog(retryableChildLsid1, sessionCatalog));
    ASSERT(doesExistInCatalog(retryableChildLsid2, sessionCatalog));

    // Check out for kill the killed retryable session and now both retryable sessions can be
    // reaped.
    runFunctionFromDifferentOpCtx([&](OperationContext* opCtx) {
        sessionCatalog->checkOutSessionForKill(opCtx, std::move(*killToken));
    });

    // A new client txnNumber must be seen to trigger the reaping, so the sessions shouldn't have
    // been reaped upon releasing the killed session.
    ASSERT_EQ(sessionCatalog->size(), 1);
    ASSERT(doesExistInCatalog(parentLsid, sessionCatalog));
    ASSERT(doesExistInCatalog(retryableChildLsid1, sessionCatalog));
    ASSERT(doesExistInCatalog(retryableChildLsid2, sessionCatalog));

    parentTxnNumber++;
    runRetryableWrite(parentLsid, parentTxnNumber);

    ASSERT_EQ(sessionCatalog->size(), 1);
    ASSERT(doesExistInCatalog(parentLsid, sessionCatalog));
    ASSERT_FALSE(doesExistInCatalog(retryableChildLsid1, sessionCatalog));
    ASSERT_FALSE(doesExistInCatalog(retryableChildLsid2, sessionCatalog));
}

TEST_F(ShardTxnParticipantTest, CheckingOutEagerlyReapedSessionDoesNotCrash) {
    auto sessionCatalog = SessionCatalog::get(getServiceContext());
    ASSERT_EQ(sessionCatalog->size(), 0);

    // Add a parent session with one retryable child.

    auto parentLsid = makeLogicalSessionIdForTest();
    auto parentTxnNumber = 0;
    runRetryableWrite(parentLsid, parentTxnNumber);

    auto retryableChildLsid =
        makeLogicalSessionIdWithTxnNumberAndUUIDForTest(parentLsid, parentTxnNumber);
    runAndCommitTransaction(retryableChildLsid, 0);

    ASSERT_EQ(sessionCatalog->size(), 1);
    ASSERT(doesExistInCatalog(parentLsid, sessionCatalog));
    ASSERT(doesExistInCatalog(retryableChildLsid, sessionCatalog));

    // Start a higher txnNumber client transaction and verify the child was erased.

    parentTxnNumber++;
    runAndCommitTransaction(parentLsid, parentTxnNumber);

    ASSERT_EQ(sessionCatalog->size(), 1);
    ASSERT(doesExistInCatalog(parentLsid, sessionCatalog));
    ASSERT_FALSE(doesExistInCatalog(retryableChildLsid, sessionCatalog));

    // Check out the child again to verify this doesn't crash.
    ASSERT_THROWS_CODE(runAndCommitTransaction(retryableChildLsid, 1),
                       AssertionException,
                       ErrorCodes::TransactionTooOld);
}

class TxnParticipantAndTxnRouterTest : public TxnParticipantTest {
protected:
    bool doesExistInCatalog(const LogicalSessionId& lsid, SessionCatalog* sessionCatalog) {
        bool existsInCatalog{false};
        sessionCatalog->scanSession(
            lsid, [&](const ObservableSession& session) { existsInCatalog = true; });
        return existsInCatalog;
    }

    void runRouterTransactionLeaveOpen(LogicalSessionId lsid, TxnNumber txnNumber) {
        runFunctionFromDifferentOpCtx([&](OperationContext* opCtx) {
            opCtx->setLogicalSessionId(lsid);
            opCtx->setTxnNumber(txnNumber);
            opCtx->setInMultiDocumentTransaction();
            auto opCtxSession = std::make_unique<RouterOperationContextSession>(opCtx);

            auto txnRouter = TransactionRouter::get(opCtx);
            txnRouter.beginOrContinueTxn(
                opCtx, *opCtx->getTxnNumber(), TransactionRouter::TransactionActions::kStart);
        });
    }

    void runParticipantTransactionLeaveOpen(LogicalSessionId lsid, TxnNumber txnNumber) {
        runFunctionFromDifferentOpCtx([&](OperationContext* opCtx) {
            opCtx->setLogicalSessionId(lsid);
            opCtx->setTxnNumber(txnNumber);
            opCtx->setInMultiDocumentTransaction();
            auto opCtxSession = MongoDSessionCatalog::get(opCtx)->checkOutSession(opCtx);

            auto txnParticipant = TransactionParticipant::get(opCtx);
            txnParticipant.beginOrContinue(opCtx,
                                           {*opCtx->getTxnNumber()},
                                           false /* autocommit */,
                                           TransactionParticipant::TransactionActions::kStart);
        });
    }
};

TEST_F(TxnParticipantAndTxnRouterTest, SkipEagerReapingSessionUsedByParticipantFromRouter) {
    auto sessionCatalog = SessionCatalog::get(getServiceContext());
    ASSERT_EQ(sessionCatalog->size(), 0);

    // Add a parent session with two retryable children.

    auto parentLsid = makeLogicalSessionIdForTest();
    auto parentTxnNumber = 0;
    runRouterTransactionLeaveOpen(parentLsid, parentTxnNumber);

    auto retryableChildLsid =
        makeLogicalSessionIdWithTxnNumberAndUUIDForTest(parentLsid, parentTxnNumber);
    runRouterTransactionLeaveOpen(retryableChildLsid, 0);

    auto retryableChildLsidReapable =
        makeLogicalSessionIdWithTxnNumberAndUUIDForTest(parentLsid, parentTxnNumber);
    runRouterTransactionLeaveOpen(retryableChildLsidReapable, 0);

    ASSERT_EQ(sessionCatalog->size(), 1);
    ASSERT(doesExistInCatalog(parentLsid, sessionCatalog));
    ASSERT(doesExistInCatalog(retryableChildLsid, sessionCatalog));
    ASSERT(doesExistInCatalog(retryableChildLsidReapable, sessionCatalog));

    // Use one retryable session with a TransactionParticipant and verify this blocks the router
    // role from reaping it.

    runParticipantTransactionLeaveOpen(retryableChildLsid, 0);

    // Start a higher txnNumber client transaction in the router role and verify the child used with
    // TransactionParticipant was not erased but the other one was.

    parentTxnNumber++;
    runRouterTransactionLeaveOpen(parentLsid, parentTxnNumber);

    ASSERT_EQ(sessionCatalog->size(), 1);
    ASSERT(doesExistInCatalog(parentLsid, sessionCatalog));
    ASSERT(doesExistInCatalog(retryableChildLsid, sessionCatalog));
    ASSERT_FALSE(doesExistInCatalog(retryableChildLsidReapable, sessionCatalog));

    // Verify the participant role can reap the child.

    auto higherRetryableChildLsid =
        makeLogicalSessionIdWithTxnNumberAndUUIDForTest(parentLsid, parentTxnNumber);
    runParticipantTransactionLeaveOpen(higherRetryableChildLsid, 5);

    ASSERT_EQ(sessionCatalog->size(), 1);
    ASSERT(doesExistInCatalog(parentLsid, sessionCatalog));
    ASSERT(doesExistInCatalog(higherRetryableChildLsid, sessionCatalog));
    ASSERT_FALSE(doesExistInCatalog(retryableChildLsid, sessionCatalog));
    ASSERT_FALSE(doesExistInCatalog(retryableChildLsidReapable, sessionCatalog));

    // Sanity check that higher txnNumbers are reaped correctly and eager reaping only applies to
    // parent and children sessions in the same "family."

    auto parentLsid2 = makeLogicalSessionIdForTest();
    auto parentTxnNumber2 = parentTxnNumber + 11;
    runParticipantTransactionLeaveOpen(parentLsid2, parentTxnNumber2);

    auto retryableChildLsid2 =
        makeLogicalSessionIdWithTxnNumberAndUUIDForTest(parentLsid2, parentTxnNumber2);
    runRouterTransactionLeaveOpen(retryableChildLsid2, 12131);

    ASSERT_EQ(sessionCatalog->size(), 2);
    ASSERT(doesExistInCatalog(parentLsid, sessionCatalog));
    ASSERT(doesExistInCatalog(higherRetryableChildLsid, sessionCatalog));
    ASSERT(doesExistInCatalog(parentLsid2, sessionCatalog));
    ASSERT(doesExistInCatalog(retryableChildLsid2, sessionCatalog));

    parentTxnNumber2++;
    runParticipantTransactionLeaveOpen(parentLsid2, parentTxnNumber2);

    // The unrelated sessions still exist and the superseded child was reaped.
    ASSERT_EQ(sessionCatalog->size(), 2);
    ASSERT(doesExistInCatalog(parentLsid, sessionCatalog));
    ASSERT(doesExistInCatalog(higherRetryableChildLsid, sessionCatalog));
    ASSERT(doesExistInCatalog(parentLsid2, sessionCatalog));
    ASSERT_FALSE(doesExistInCatalog(retryableChildLsid2, sessionCatalog));
    ASSERT_FALSE(doesExistInCatalog(retryableChildLsid2, sessionCatalog));
}

}  // namespace
}  // namespace mongo
