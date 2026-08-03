// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/global_catalog/ddl/sharding_recovery_service.h"

#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/oid.h"
#include "mongo/bson/timestamp.h"
#include "mongo/client/dbclient_cursor.h"
#include "mongo/db/client.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/global_catalog/chunk_manager.h"
#include "mongo/db/global_catalog/ddl/sharding_migration_critical_section.h"
#include "mongo/db/global_catalog/sharding_catalog_client.h"
#include "mongo/db/global_catalog/type_chunk.h"
#include "mongo/db/global_catalog/type_chunk_range.h"
#include "mongo/db/global_catalog/type_collection.h"
#include "mongo/db/global_catalog/type_database_gen.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/op_observer/op_observer.h"
#include "mongo/db/op_observer/op_observer_util.h"
#include "mongo/db/query/find_command.h"
#include "mongo/db/repl/member_state.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/session/kill_sessions_local.h"
#include "mongo/db/session/logical_session_id.h"
#include "mongo/db/session/session_catalog.h"
#include "mongo/db/session/session_catalog_mongod.h"
#include "mongo/db/shard_role/ddl/create_gen.h"
#include "mongo/db/shard_role/lock_manager/lock_manager_defs.h"
#include "mongo/db/shard_role/lock_manager/locker.h"
#include "mongo/db/shard_role/shard_catalog/catalog_raii.h"
#include "mongo/db/shard_role/shard_catalog/collection.h"
#include "mongo/db/shard_role/shard_catalog/collection_critical_section_document_gen.h"
#include "mongo/db/shard_role/shard_catalog/collection_metadata.h"
#include "mongo/db/shard_role/shard_catalog/collection_sharding_runtime.h"
#include "mongo/db/shard_role/shard_catalog/collection_sharding_state.h"
#include "mongo/db/shard_role/shard_catalog/create_collection.h"
#include "mongo/db/shard_role/shard_catalog/database_sharding_runtime.h"
#include "mongo/db/shard_role/shard_catalog/operation_sharding_state.h"
#include "mongo/db/shard_role/transaction_resources.h"
#include "mongo/db/sharding_environment/shard_id.h"
#include "mongo/db/sharding_environment/shard_server_op_observer.h"
#include "mongo/db/sharding_environment/shard_server_test_fixture.h"
#include "mongo/db/storage/write_unit_of_work.h"
#include "mongo/db/topology/sharding_state.h"
#include "mongo/db/transaction/transaction_participant.h"
#include "mongo/db/versioning_protocol/chunk_version.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/stdx/condition_variable.h"
#include "mongo/stdx/thread.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/server_parameter_guard.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/string_map.h"
#include "mongo/util/tick_source_mock.h"

#include <algorithm>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {
namespace {

const std::string kShardKey = "_id";
const BSONObj kShardKeyPattern = BSON(kShardKey << 1);

// Builds tracked (sharded) filtering metadata for the given namespace.
CollectionMetadata makeShardedMetadata(const NamespaceString& nss,
                                       UUID uuid = UUID::gen(),
                                       ShardId chunkShardId = ShardId("other"),
                                       ShardId collectionShardId = ShardId("0")) {
    const OID epoch = OID::gen();
    const Timestamp timestamp(Date_t::now());
    auto range = ChunkRange(BSON(kShardKey << MINKEY), BSON(kShardKey << MAXKEY));
    auto chunk =
        ChunkType(uuid, std::move(range), ChunkVersion({epoch, timestamp}, {1, 0}), chunkShardId);
    CurrentChunkManager cm(ShardServerTestFixture::makeStandaloneRoutingTableHistory(
        RoutingTableHistory::makeNew(nss,
                                     uuid,
                                     kShardKeyPattern,
                                     false /* unsplittable */,
                                     nullptr,
                                     false,
                                     epoch,
                                     timestamp,
                                     boost::none /* timeseriesFields */,
                                     boost::none /* reshardingFields */,
                                     true,
                                     {std::move(chunk)})));
    return CollectionMetadata(std::move(cm), collectionShardId);
}

class ShardingRecoveryServiceTest : public ShardServerTestFixture {
public:
    inline static const NamespaceString collNss =
        NamespaceString::createNamespaceString_forTest("TestDB", "TestCollection");
    inline static const BSONObj collOpReason =
        BSON("Dummy operation on collection" << collNss.ns_forTest());

    inline static const NamespaceString dbName =
        NamespaceString::createNamespaceString_forTest("TestDB");
    inline static const BSONObj dbOpReason =
        BSON("Dummy operation on database" << dbName.ns_forTest());

    inline static const BSONObj differentOpReason = BSON("Yet another dummy operation" << true);

    OpObserver& opObserver() {
        return _opObserver;
    }

    boost::optional<CollectionCriticalSectionDocument> readCriticalSectionDocument(
        const NamespaceString& nss, const BSONObj& reason) {
        FindCommandRequest findOp(NamespaceString::kCollectionCriticalSectionsNamespace);
        findOp.setFilter(
            BSON(CollectionCriticalSectionDocument::kNssFieldName << nss.toString_forTest()));

        DBDirectClient dbClient(operationContext());
        auto cursor = dbClient.find(std::move(findOp));

        if (!cursor->more()) {
            return boost::none;
        }

        auto bsonObj = cursor->next();
        auto doc = CollectionCriticalSectionDocument::parse(
            bsonObj, IDLParserContext("AcquireRecoverableCSBW"));

        // The document exists, so the reason must match.
        ASSERT(!doc.getReason().woCompare(reason));

        return doc;
    }

    void writeReadCriticalSectionDocument(const NamespaceString& nss,
                                          const BSONObj& reason,
                                          bool blockReads) {
        auto doc = readCriticalSectionDocument(nss, reason);
        if (!doc) {
            doc = CollectionCriticalSectionDocument(nss, reason, blockReads);

            DBDirectClient dbClient(operationContext());
            dbClient.insert(NamespaceString::kCollectionCriticalSectionsNamespace, doc->toBSON());

            return;
        }

        // The document exists, so the blockReads should change.
        ASSERT_NE(doc->getBlockReads(), blockReads);

        DBDirectClient dbClient(operationContext());
        dbClient.update(
            NamespaceString::kCollectionCriticalSectionsNamespace,
            BSON(CollectionCriticalSectionDocument::kNssFieldName << nss.toString_forTest()),
            BSON("$set" << BSON(CollectionCriticalSectionDocument::kBlockReadsFieldName
                                << blockReads)));
    }

    void deleteReadCriticalSectionDocument(const NamespaceString& nss, const BSONObj& reason) {
        // The document must exist.
        ASSERT(readCriticalSectionDocument(nss, reason));

        DBDirectClient dbClient(operationContext());
        dbClient.remove(
            NamespaceString::kCollectionCriticalSectionsNamespace,
            BSON(CollectionCriticalSectionDocument::kNssFieldName << nss.toString_forTest()),
            false /* removeMany */);
    }

    void assertCriticalSectionCatchUpEnteredInMemory(const NamespaceString& nss) {
        if (nss.isDbOnly()) {
            const auto scopedDsr =
                DatabaseShardingRuntime::acquireShared(operationContext(), nss.dbName());
            ASSERT(scopedDsr->getCriticalSectionSignal(ShardingMigrationCriticalSection::kWrite));
            ASSERT(!scopedDsr->getCriticalSectionSignal(ShardingMigrationCriticalSection::kRead));
        } else {
            const auto csr = CollectionShardingRuntime::acquireShared(operationContext(), nss);
            ASSERT(csr->getCriticalSectionSignal(ShardingMigrationCriticalSection::kWrite));
            ASSERT(!csr->getCriticalSectionSignal(ShardingMigrationCriticalSection::kRead));
        }
    }

    void assertCriticalSectionCommitEnteredInMemory(const NamespaceString& nss) {
        if (nss.isDbOnly()) {
            const auto scopedDsr =
                DatabaseShardingRuntime::acquireShared(operationContext(), nss.dbName());
            ASSERT(scopedDsr->getCriticalSectionSignal(ShardingMigrationCriticalSection::kWrite));
            ASSERT(scopedDsr->getCriticalSectionSignal(ShardingMigrationCriticalSection::kRead));
        } else {
            const auto csr = CollectionShardingRuntime::acquireShared(operationContext(), nss);
            ASSERT(csr->getCriticalSectionSignal(ShardingMigrationCriticalSection::kWrite));
            ASSERT(csr->getCriticalSectionSignal(ShardingMigrationCriticalSection::kRead));
        }
    }

    void assertCriticalSectionLeftInMemory(const NamespaceString& nss) {
        if (nss.isDbOnly()) {
            const auto scopedDsr =
                DatabaseShardingRuntime::acquireShared(operationContext(), nss.dbName());
            ASSERT(!scopedDsr->getCriticalSectionSignal(ShardingMigrationCriticalSection::kWrite));
            ASSERT(!scopedDsr->getCriticalSectionSignal(ShardingMigrationCriticalSection::kRead));
        } else {
            const auto csr = CollectionShardingRuntime::acquireShared(operationContext(), nss);
            ASSERT(!csr->getCriticalSectionSignal(ShardingMigrationCriticalSection::kWrite));
            ASSERT(!csr->getCriticalSectionSignal(ShardingMigrationCriticalSection::kRead));
        }
    }

    void assertCriticalSectionCatchUpEnteredOnDisk(const NamespaceString& nss,
                                                   const BSONObj& reason) {
        auto doc = readCriticalSectionDocument(nss, reason);
        ASSERT(doc);
        ASSERT(!doc->getBlockReads());
    }

    void assertCriticalSectionCommitEnteredOnDisk(const NamespaceString& nss,
                                                  const BSONObj& reason) {
        auto doc = readCriticalSectionDocument(nss, reason);
        ASSERT(doc);
        ASSERT(doc->getBlockReads());
    }

    void assertCriticalSectionLeftOnDisk(const NamespaceString& nss, const BSONObj& reason) {
        ASSERT(!readCriticalSectionDocument(nss, reason));
    }

private:
    ShardServerOpObserver _opObserver;
};

class ShardingRecoveryServiceTestOnPrimary : public ShardingRecoveryServiceTest {
public:
    void setUp() override {
        ShardingRecoveryServiceTest::setUp();

        auto replCoord = repl::ReplicationCoordinator::get(operationContext());
        ASSERT_OK(replCoord->setFollowerMode(repl::MemberState::RS_PRIMARY));

        // Enable session catalog for transaction-related tests.
        MongoDSessionCatalog::get(operationContext())->onStepUp(operationContext());
    }

protected:
    class TransactionHandle {
    public:
        TransactionHandle(ServiceContext* svcCtx, const NamespaceString& nss)
            : _client(svcCtx->getService()->makeClient("txnHolder")), _nss(nss) {
            _opCtx = _client->makeOperationContext();
            _opCtx->setLogicalSessionId(makeLogicalSessionIdForTest());
        }

        ~TransactionHandle() {
            // Abort the transaction first (which destroys TxnResources), then release
            // the stashed locks so the Locker destructor invariants are satisfied.
            OperationContextSession checkout(_opCtx.get());
            auto txnParticipant = TransactionParticipant::get(_opCtx.get());
            if (txnParticipant && txnParticipant.transactionIsOpen()) {
                txnParticipant.abortTransaction(_opCtx.get());
            }
            if (_stashedLockerPtr) {
                releaseStashedLocks();
            }
        }

        /**
         * Creates a transaction that holds MODE_IX locks on the namespace, then stashes
         * the resources.
         *
         * If 'prepared' is true, the transaction is transitioned to the prepared state.
         * If 'registerNss' is true (default), the namespace is added as affected.
         */
        void createTransaction(bool prepared = false, bool registerNss = true) {
            OperationContextSession checkout(_opCtx.get());
            auto txnParticipant = TransactionParticipant::get(_opCtx.get());

            txnParticipant.transitionToInProgressForTest();

            // Acquire MODE_IX locks on the opCtx's own locker.
            auto* lkr = shard_role_details::getLocker(_opCtx.get());
            lkr->lockGlobal(_opCtx.get(), MODE_IX);
            lkr->lock(_opCtx.get(), ResourceId(RESOURCE_DATABASE, _nss.dbName()), MODE_IX);
            lkr->lock(_opCtx.get(), ResourceId(RESOURCE_COLLECTION, _nss), MODE_IX);

            if (registerNss) {
                txnParticipant.addToAffectedNamespaces(_opCtx.get(), _nss);
            }
            if (prepared) {
                txnParticipant.transitionToPreparedforTest(_opCtx.get(), repl::OpTime({1, 1}, 1));
            }
            advanceTransactionMetricsTimer(txnParticipant);

            // Remember the locker pointer before stashing swaps it out.
            _stashedLockerPtr = lkr;

            // Stash: moves the opCtx's locker (with its locks) into TxnResources and
            // installs a fresh locker on the opCtx.
            txnParticipant.stashActiveTransactionForTest(_opCtx.get());
        }

        /**
         * Releases the locks held by the stashed Locker. Must be called before the
         * stash is destroyed (e.g. inside the CS callback, before invoking the
         * session-kill helper). In production these locks are released as part of the
         * WUOW abort triggered by the transaction abort, but this test setup acquires
         * them outside a WUOW so we drive the release manually to keep the Locker
         * destructor invariants happy.
         */
        void releaseStashedLocks() {
            invariant(_stashedLockerPtr);
            _stashedLockerPtr->unlock(ResourceId(RESOURCE_COLLECTION, _nss));
            _stashedLockerPtr->unlock(ResourceId(RESOURCE_DATABASE, _nss.dbName()));
            _stashedLockerPtr->unlockGlobal();
            _stashedLockerPtr = nullptr;
        }

        OperationContext* opCtx() {
            return _opCtx.get();
        }

        bool isInProgress() {
            OperationContextSession checkout(_opCtx.get());
            return TransactionParticipant::get(_opCtx.get()).transactionIsInProgress();
        }

        bool isPrepared() {
            OperationContextSession checkout(_opCtx.get());
            return TransactionParticipant::get(_opCtx.get()).transactionIsPrepared();
        }

    private:
        void advanceTransactionMetricsTimer(TransactionParticipant::Participant txnParticipant) {
            auto tickSourceMock = std::make_unique<TickSourceMock<Microseconds>>();
            tickSourceMock->advance(Milliseconds{100});
            txnParticipant.startMetricsTimer(
                _opCtx.get(), tickSourceMock.get(), Date_t::now(), Date_t::now());
        }

        ServiceContext::UniqueClient _client;
        ServiceContext::UniqueOperationContext _opCtx;
        NamespaceString _nss;
        Locker* _stashedLockerPtr = nullptr;
    };
};

class ShardingRecoveryServiceTestOnSecondary : public ShardingRecoveryServiceTest {
public:
    void setUp() override {
        ShardingRecoveryServiceTest::setUp();

        // Create and lock the `config.collection_critical_sections` collection to allow
        // notifications on the operation observer.
        ASSERT_OK(
            createCollection(operationContext(),
                             CreateCommand(NamespaceString::kCollectionCriticalSectionsNamespace)));
        _criticalSectionColl.emplace(
            operationContext(), NamespaceString::kCollectionCriticalSectionsNamespace, MODE_IX);

        auto replCoord = repl::ReplicationCoordinator::get(operationContext());
        ASSERT_OK(replCoord->setFollowerMode(repl::MemberState::RS_SECONDARY));
    }

    void tearDown() override {
        _criticalSectionColl = boost::none;

        ShardingRecoveryServiceTest::tearDown();
    }

    const CollectionPtr& criticalSectionColl() {
        return **_criticalSectionColl;
    }

private:
    boost::optional<AutoGetCollection> _criticalSectionColl;
};

class ShardingRecoveryServiceTestonInitialData : public ShardingRecoveryServiceTest {
public:
    void setUp() override {
        ShardingRecoveryServiceTest::setUp();

        // Create the `config.collection_critical_sections` collection to allow notifications on the
        // operation observer.
        ASSERT_OK(
            createCollection(operationContext(),
                             CreateCommand(NamespaceString::kCollectionCriticalSectionsNamespace)));
    }
};

using ShardingRecoveryServiceTestAfterRollback = ShardingRecoveryServiceTestonInitialData;

////////////////////////////////////////////////////////////////////////////////////////////////////

TEST_F(ShardingRecoveryServiceTestOnPrimary, BlockAndUnblockOperationsOnDatabase) {
    ///////////////////////////////////////////
    // Block write operations (catch-up phase)
    ///////////////////////////////////////////

    ShardingRecoveryService::get(operationContext())
        ->acquireRecoverableCriticalSectionBlockWrites(
            operationContext(),
            dbName,
            dbOpReason,
            ShardingCatalogClient::writeConcernLocalHavingUpstreamWaiter(),
            true /* clearShardCatalogCache */);

    // Check that the in-memory status has been appropriately updated.
    assertCriticalSectionCatchUpEnteredInMemory(dbName);

    // Check that the document has been appropriately saved.
    assertCriticalSectionCatchUpEnteredOnDisk(dbName, dbOpReason);

    //////////////////////////////////////////////
    // Block read/write operations (commit phase)
    //////////////////////////////////////////////

    ShardingRecoveryService::get(operationContext())
        ->promoteRecoverableCriticalSectionToBlockAlsoReads(
            operationContext(),
            dbName,
            dbOpReason,
            ShardingCatalogClient::writeConcernLocalHavingUpstreamWaiter());

    // Check that the in-memory status has been appropriately updated.
    assertCriticalSectionCommitEnteredInMemory(dbName);

    // Check that the document has been appropriately updated.
    assertCriticalSectionCommitEnteredOnDisk(dbName, dbOpReason);

    /////////////////////////////////
    // Unblock read/write operations
    /////////////////////////////////

    ShardingRecoveryService::get(operationContext())
        ->releaseRecoverableCriticalSection(
            operationContext(),
            dbName,
            dbOpReason,
            ShardingCatalogClient::writeConcernLocalHavingUpstreamWaiter(),
            ShardingRecoveryService::NoCustomAction());

    // Check that the in-memory status has been appropriately updated.
    assertCriticalSectionLeftInMemory(dbName);

    // Check that the document has been deleted.
    assertCriticalSectionLeftOnDisk(dbName, dbOpReason);
}

TEST_F(ShardingRecoveryServiceTestOnPrimary, BlockAndUnblockOperationsTwiceOnDatabase) {
    ///////////////////////////////////////////
    // Block write operations (catch-up phase)
    ///////////////////////////////////////////

    ShardingRecoveryService::get(operationContext())
        ->acquireRecoverableCriticalSectionBlockWrites(
            operationContext(),
            dbName,
            dbOpReason,
            ShardingCatalogClient::writeConcernLocalHavingUpstreamWaiter(),
            true /* clearShardCatalogCache */);

    ShardingRecoveryService::get(operationContext())
        ->acquireRecoverableCriticalSectionBlockWrites(
            operationContext(),
            dbName,
            dbOpReason,
            ShardingCatalogClient::writeConcernLocalHavingUpstreamWaiter(),
            true /* clearShardCatalogCache */);

    // Check that the in-memory status has been appropriately updated.
    assertCriticalSectionCatchUpEnteredInMemory(dbName);

    // Check that the document has been appropriately saved.
    assertCriticalSectionCatchUpEnteredOnDisk(dbName, dbOpReason);

    //////////////////////////////////////////////
    // Block read/write operations (commit phase)
    //////////////////////////////////////////////

    ShardingRecoveryService::get(operationContext())
        ->promoteRecoverableCriticalSectionToBlockAlsoReads(
            operationContext(),
            dbName,
            dbOpReason,
            ShardingCatalogClient::writeConcernLocalHavingUpstreamWaiter());

    ShardingRecoveryService::get(operationContext())
        ->promoteRecoverableCriticalSectionToBlockAlsoReads(
            operationContext(),
            dbName,
            dbOpReason,
            ShardingCatalogClient::writeConcernLocalHavingUpstreamWaiter());

    // Check that the in-memory status has been appropriately updated.
    assertCriticalSectionCommitEnteredInMemory(dbName);

    // Check that the document has been appropriately updated.
    assertCriticalSectionCommitEnteredOnDisk(dbName, dbOpReason);

    /////////////////////////////////
    // Unblock read/write operations
    /////////////////////////////////

    ShardingRecoveryService::get(operationContext())
        ->releaseRecoverableCriticalSection(
            operationContext(),
            dbName,
            dbOpReason,
            ShardingCatalogClient::writeConcernLocalHavingUpstreamWaiter(),
            ShardingRecoveryService::NoCustomAction());

    ShardingRecoveryService::get(operationContext())
        ->releaseRecoverableCriticalSection(
            operationContext(),
            dbName,
            dbOpReason,
            ShardingCatalogClient::writeConcernLocalHavingUpstreamWaiter(),
            ShardingRecoveryService::NoCustomAction());

    // Check that the in-memory status has been appropriately updated.
    assertCriticalSectionLeftInMemory(dbName);

    // Check that the document has been deleted.
    assertCriticalSectionLeftOnDisk(dbName, dbOpReason);
}

using ShardingRecoveryServiceTestOnPrimaryDeathTest = ShardingRecoveryServiceTestOnPrimary;
DEATH_TEST_F(ShardingRecoveryServiceTestOnPrimaryDeathTest,
             FailBlockingWritesTwiceOnDatabaseWithDifferentReasons,
             "Location7032368") {
    ///////////////////////////////////////////
    // Block write operations (catch-up phase)
    ///////////////////////////////////////////

    ShardingRecoveryService::get(operationContext())
        ->acquireRecoverableCriticalSectionBlockWrites(
            operationContext(),
            dbName,
            dbOpReason,
            ShardingCatalogClient::writeConcernLocalHavingUpstreamWaiter(),
            true /* clearShardCatalogCache */);

    ShardingRecoveryService::get(operationContext())
        ->acquireRecoverableCriticalSectionBlockWrites(
            operationContext(),
            dbName,
            differentOpReason,
            ShardingCatalogClient::writeConcernLocalHavingUpstreamWaiter(),
            true /* clearShardCatalogCache */);
}

DEATH_TEST_F(ShardingRecoveryServiceTestOnPrimaryDeathTest,
             FailBlockingReadsOnDatabaseWithDifferentReasons,
             "Location7032362") {
    ///////////////////////////////////////////
    // Block write operations (catch-up phase)
    ///////////////////////////////////////////

    ShardingRecoveryService::get(operationContext())
        ->acquireRecoverableCriticalSectionBlockWrites(
            operationContext(),
            dbName,
            dbOpReason,
            ShardingCatalogClient::writeConcernLocalHavingUpstreamWaiter(),
            true /* clearShardCatalogCache */);

    //////////////////////////////////////////////
    // Block read/write operations (commit phase)
    //////////////////////////////////////////////

    ShardingRecoveryService::get(operationContext())
        ->promoteRecoverableCriticalSectionToBlockAlsoReads(
            operationContext(),
            dbName,
            differentOpReason,
            ShardingCatalogClient::writeConcernLocalHavingUpstreamWaiter());
}

DEATH_TEST_F(ShardingRecoveryServiceTestOnPrimaryDeathTest,
             FailUnblockingOperationsOnDatabaseWithDifferentReasons,
             "Location7032366") {
    ///////////////////////////////////////////
    // Block write operations (catch-up phase)
    ///////////////////////////////////////////

    ShardingRecoveryService::get(operationContext())
        ->acquireRecoverableCriticalSectionBlockWrites(
            operationContext(),
            dbName,
            dbOpReason,
            ShardingCatalogClient::writeConcernLocalHavingUpstreamWaiter(),
            true /* clearShardCatalogCache */);

    //////////////////////////////////////////////
    // Block read/write operations (commit phase)
    //////////////////////////////////////////////

    ShardingRecoveryService::get(operationContext())
        ->promoteRecoverableCriticalSectionToBlockAlsoReads(
            operationContext(),
            dbName,
            dbOpReason,
            ShardingCatalogClient::writeConcernLocalHavingUpstreamWaiter());

    /////////////////////////////////
    // Unblock read/write operations
    /////////////////////////////////

    ShardingRecoveryService::get(operationContext())
        ->releaseRecoverableCriticalSection(
            operationContext(),
            dbName,
            differentOpReason,
            ShardingCatalogClient::writeConcernLocalHavingUpstreamWaiter(),
            ShardingRecoveryService::NoCustomAction());
}

TEST_F(ShardingRecoveryServiceTestOnPrimary, BlockAndUnblockOperationsOnCollection) {
    ///////////////////////////////////////////
    // Block write operations (catch-up phase)
    ///////////////////////////////////////////

    ShardingRecoveryService::get(operationContext())
        ->acquireRecoverableCriticalSectionBlockWrites(
            operationContext(),
            collNss,
            collOpReason,
            ShardingCatalogClient::writeConcernLocalHavingUpstreamWaiter(),
            true /* clearShardCatalogCache */);

    // Check that the in-memory status has been appropriately updated.
    assertCriticalSectionCatchUpEnteredInMemory(collNss);

    // Check that the document has been appropriately saved.
    assertCriticalSectionCatchUpEnteredOnDisk(collNss, collOpReason);

    //////////////////////////////////////////////
    // Block read/write operations (commit phase)
    //////////////////////////////////////////////

    ShardingRecoveryService::get(operationContext())
        ->promoteRecoverableCriticalSectionToBlockAlsoReads(
            operationContext(),
            collNss,
            collOpReason,
            ShardingCatalogClient::writeConcernLocalHavingUpstreamWaiter());

    // Check that the in-memory status has been appropriately updated.
    assertCriticalSectionCommitEnteredInMemory(collNss);

    // Check that the document has been appropriately updated.
    assertCriticalSectionCommitEnteredOnDisk(collNss, collOpReason);

    /////////////////////////////////
    // Unblock read/write operations
    /////////////////////////////////

    ShardingRecoveryService::get(operationContext())
        ->releaseRecoverableCriticalSection(
            operationContext(),
            collNss,
            collOpReason,
            ShardingCatalogClient::writeConcernLocalHavingUpstreamWaiter(),
            ShardingRecoveryService::NoCustomAction());

    // Check that the in-memory status has been appropriately updated.
    assertCriticalSectionLeftInMemory(collNss);

    // Check that the document has been deleted.
    assertCriticalSectionLeftOnDisk(collNss, collOpReason);
}

TEST_F(ShardingRecoveryServiceTestOnPrimary, BlockAndUnblockOperationsTwiceOnCollection) {
    ///////////////////////////////////////////
    // Block write operations (catch-up phase)
    ///////////////////////////////////////////

    ShardingRecoveryService::get(operationContext())
        ->acquireRecoverableCriticalSectionBlockWrites(
            operationContext(),
            collNss,
            collOpReason,
            ShardingCatalogClient::writeConcernLocalHavingUpstreamWaiter(),
            true /* clearShardCatalogCache */);

    ShardingRecoveryService::get(operationContext())
        ->acquireRecoverableCriticalSectionBlockWrites(
            operationContext(),
            collNss,
            collOpReason,
            ShardingCatalogClient::writeConcernLocalHavingUpstreamWaiter(),
            true /* clearShardCatalogCache */);

    // Check that the in-memory status has been appropriately updated.
    assertCriticalSectionCatchUpEnteredInMemory(collNss);

    // Check that the document has been appropriately saved.
    assertCriticalSectionCatchUpEnteredOnDisk(collNss, collOpReason);

    //////////////////////////////////////////////
    // Block read/write operations (commit phase)
    //////////////////////////////////////////////

    ShardingRecoveryService::get(operationContext())
        ->promoteRecoverableCriticalSectionToBlockAlsoReads(
            operationContext(),
            collNss,
            collOpReason,
            ShardingCatalogClient::writeConcernLocalHavingUpstreamWaiter());

    ShardingRecoveryService::get(operationContext())
        ->promoteRecoverableCriticalSectionToBlockAlsoReads(
            operationContext(),
            collNss,
            collOpReason,
            ShardingCatalogClient::writeConcernLocalHavingUpstreamWaiter());

    // Check that the in-memory status has been appropriately updated.
    assertCriticalSectionCommitEnteredInMemory(collNss);

    // Check that the document has been appropriately updated.
    assertCriticalSectionCommitEnteredOnDisk(collNss, collOpReason);

    /////////////////////////////////
    // Unblock read/write operations
    /////////////////////////////////

    ShardingRecoveryService::get(operationContext())
        ->releaseRecoverableCriticalSection(
            operationContext(),
            collNss,
            collOpReason,
            ShardingCatalogClient::writeConcernLocalHavingUpstreamWaiter(),
            ShardingRecoveryService::NoCustomAction());

    ShardingRecoveryService::get(operationContext())
        ->releaseRecoverableCriticalSection(
            operationContext(),
            collNss,
            collOpReason,
            ShardingCatalogClient::writeConcernLocalHavingUpstreamWaiter(),
            ShardingRecoveryService::NoCustomAction());

    // Check that the in-memory status has been appropriately updated.
    assertCriticalSectionLeftInMemory(collNss);

    // Check that the document has been deleted.
    assertCriticalSectionLeftOnDisk(collNss, collOpReason);
}

DEATH_TEST_F(ShardingRecoveryServiceTestOnPrimaryDeathTest,
             FailBlockingWritesTwiceOnCollectionWithDifferentReasons,
             "Location7032368") {
    ///////////////////////////////////////////
    // Block write operations (catch-up phase)
    ///////////////////////////////////////////

    ShardingRecoveryService::get(operationContext())
        ->acquireRecoverableCriticalSectionBlockWrites(
            operationContext(),
            collNss,
            collOpReason,
            ShardingCatalogClient::writeConcernLocalHavingUpstreamWaiter(),
            true /* clearShardCatalogCache */);

    ShardingRecoveryService::get(operationContext())
        ->acquireRecoverableCriticalSectionBlockWrites(
            operationContext(),
            collNss,
            differentOpReason,
            ShardingCatalogClient::writeConcernLocalHavingUpstreamWaiter(),
            true /* clearShardCatalogCache */);
}

DEATH_TEST_F(ShardingRecoveryServiceTestOnPrimaryDeathTest,
             FailBlockingReadsOnCollectionWithDifferentReasons,
             "Location7032362") {
    ///////////////////////////////////////////
    // Block write operations (catch-up phase)
    ///////////////////////////////////////////

    ShardingRecoveryService::get(operationContext())
        ->acquireRecoverableCriticalSectionBlockWrites(
            operationContext(),
            collNss,
            collOpReason,
            ShardingCatalogClient::writeConcernLocalHavingUpstreamWaiter(),
            true /* clearShardCatalogCache */);

    //////////////////////////////////////////////
    // Block read/write operations (commit phase)
    //////////////////////////////////////////////

    ShardingRecoveryService::get(operationContext())
        ->promoteRecoverableCriticalSectionToBlockAlsoReads(
            operationContext(),
            collNss,
            differentOpReason,
            ShardingCatalogClient::writeConcernLocalHavingUpstreamWaiter());
}

DEATH_TEST_F(ShardingRecoveryServiceTestOnPrimaryDeathTest,
             FailUnblockingOperationsOnCollectionWithDifferentReasons,
             "Location7032366") {
    ///////////////////////////////////////////
    // Block write operations (catch-up phase)
    ///////////////////////////////////////////

    ShardingRecoveryService::get(operationContext())
        ->acquireRecoverableCriticalSectionBlockWrites(
            operationContext(),
            collNss,
            collOpReason,
            ShardingCatalogClient::writeConcernLocalHavingUpstreamWaiter(),
            true /* clearShardCatalogCache */);

    //////////////////////////////////////////////
    // Block read/write operations (commit phase)
    //////////////////////////////////////////////

    ShardingRecoveryService::get(operationContext())
        ->promoteRecoverableCriticalSectionToBlockAlsoReads(
            operationContext(),
            collNss,
            collOpReason,
            ShardingCatalogClient::writeConcernLocalHavingUpstreamWaiter());

    /////////////////////////////////
    // Unblock read/write operations
    /////////////////////////////////

    ShardingRecoveryService::get(operationContext())
        ->releaseRecoverableCriticalSection(
            operationContext(),
            collNss,
            differentOpReason,
            ShardingCatalogClient::writeConcernLocalHavingUpstreamWaiter(),
            ShardingRecoveryService::NoCustomAction());
}

////////////////////////////////////////////////////////////////////////////////////////////////////

TEST_F(ShardingRecoveryServiceTestOnSecondary, BlockAndUnblockOperationsOnDatabase) {
    ///////////////////////////////////////////
    // Block write operations (catch-up phase)
    ///////////////////////////////////////////

    // Simulate an insert notification on the `config.collection_critical_sections` collection, that
    // is what a secondary node would receive when the primary node enters the catch-up phase of the
    // critical section.
    auto doc = CollectionCriticalSectionDocument(dbName, dbOpReason, false);
    const auto& documentKey = getDocumentKey(criticalSectionColl(), doc.toBSON());
    {
        std::vector<InsertStatement> inserts;
        inserts.emplace_back(doc.toBSON());

        WriteUnitOfWork wuow(operationContext());
        AutoGetDb db(operationContext(), dbName.dbName(), MODE_IX);
        opObserver().onInserts(operationContext(),
                               criticalSectionColl(),
                               inserts.begin(),
                               inserts.end(),
                               /*recordIds*/ {},
                               /*fromMigrate=*/std::vector<bool>(inserts.size(), false),
                               /*defaultFromMigrate=*/false);
        wuow.commit();
    }

    // Check that the in-memory status has been appropriately updated.
    assertCriticalSectionCatchUpEnteredInMemory(dbName);

    //////////////////////////////////////////////
    // Block read/write operations (commit phase)
    //////////////////////////////////////////////

    // Simulate an update notification on the `config.collection_critical_sections` collection, that
    // is what a secondary node would receive when the primary node enters the commit phase of the
    // critical section.
    auto preImageDoc = doc.toBSON();
    doc.setBlockReads(true);
    {
        CollectionUpdateArgs updateArgs{preImageDoc};
        updateArgs.updatedDoc = doc.toBSON();
        OplogUpdateEntryArgs update(&updateArgs, criticalSectionColl());

        WriteUnitOfWork wuow(operationContext());
        AutoGetDb db(operationContext(), dbName.dbName(), MODE_IX);
        opObserver().onUpdate(operationContext(), update);
        wuow.commit();
    }

    // Check that the in-memory status has been appropriately updated.
    assertCriticalSectionCommitEnteredInMemory(dbName);

    /////////////////////////////////
    // Unblock read/write operations
    /////////////////////////////////

    // Simulate an delete notification on the `config.collection_critical_sections` collection, that
    // is what a secondary node would receive when the primary node leaves the critical section.
    {
        WriteUnitOfWork wuow(operationContext());
        AutoGetDb db(operationContext(), dbName.dbName(), MODE_IX);
        OplogDeleteEntryArgs args;
        opObserver().onDelete(operationContext(),
                              criticalSectionColl(),
                              kUninitializedStmtId,
                              doc.toBSON(),
                              documentKey,
                              args);
        wuow.commit();
    }

    // Check that the in-memory status has been appropriately updated.
    assertCriticalSectionLeftInMemory(dbName);
}

TEST_F(ShardingRecoveryServiceTestOnSecondary, BlockAndUnblockOperationsOnCollection) {
    ///////////////////////////////////////////
    // Block write operations (catch-up phase)
    ///////////////////////////////////////////

    // Simulate an insert notification on the `config.collection_critical_sections` collection, that
    // is what a secondary node would receive when the primary node enters the catch-up phase of the
    // critical section.
    auto doc = CollectionCriticalSectionDocument(collNss, collOpReason, false);
    const auto& documentKey = getDocumentKey(criticalSectionColl(), doc.toBSON());
    auto preImageDoc = doc.toBSON();
    {
        std::vector<InsertStatement> inserts;
        inserts.emplace_back(doc.toBSON());

        WriteUnitOfWork wuow(operationContext());
        AutoGetCollection coll(operationContext(), collNss, MODE_IX);
        opObserver().onInserts(operationContext(),
                               criticalSectionColl(),
                               inserts.begin(),
                               inserts.end(),
                               /*recordIds*/ {},
                               /*fromMigrate=*/std::vector<bool>(inserts.size(), false),
                               /*defaultFromMigrate=*/false);
        wuow.commit();
    }

    // Check that the in-memory status has been appropriately updated.
    assertCriticalSectionCatchUpEnteredInMemory(collNss);

    //////////////////////////////////////////////
    // Block read/write operations (commit phase)
    //////////////////////////////////////////////

    // Simulate an update notification on the `config.collection_critical_sections` collection, that
    // is what a secondary node would receive when the primary node enters the commit phase of the
    // critical section.
    doc.setBlockReads(true);
    {
        CollectionUpdateArgs updateArgs{preImageDoc};
        updateArgs.updatedDoc = doc.toBSON();
        OplogUpdateEntryArgs update(&updateArgs, criticalSectionColl());

        WriteUnitOfWork wuow(operationContext());
        AutoGetCollection coll(operationContext(), collNss, MODE_IX);
        opObserver().onUpdate(operationContext(), update);
        wuow.commit();
    }

    // Check that the in-memory status has been appropriately updated.
    assertCriticalSectionCommitEnteredInMemory(collNss);

    /////////////////////////////////
    // Unblock read/write operations
    /////////////////////////////////

    // Simulate an delete notification on the `config.collection_critical_sections` collection, that
    // is what a secondary node would receive when the primary node leaves the critical section.
    {
        WriteUnitOfWork wuow(operationContext());
        AutoGetCollection coll(operationContext(), collNss, MODE_IX);
        OplogDeleteEntryArgs args;
        opObserver().onDelete(operationContext(),
                              criticalSectionColl(),
                              kUninitializedStmtId,
                              doc.toBSON(),
                              documentKey,
                              args);
        wuow.commit();
    }

    // Check that the in-memory status has been appropriately updated.
    assertCriticalSectionLeftInMemory(collNss);
}

////////////////////////////////////////////////////////////////////////////////////////////////////

TEST_F(ShardingRecoveryServiceTestonInitialData, BlockAndUnblockOperationsOnDatabase) {
    ///////////////////////////////////////////
    // Block write operations (catch-up phase)
    ///////////////////////////////////////////

    // Insert a document in the `config.collection_critical_sections` collection to simulate
    // entering the catch-up phase of the critical section for a database, then react to an
    // hypothetical availability of initial data.
    {
        AutoGetDb db(operationContext(), dbName.dbName(), MODE_IX);
        writeReadCriticalSectionDocument(dbName, dbOpReason, false /* blockReads */);
    }
    ShardingRecoveryService::get(operationContext())
        ->onConsistentDataAvailable(operationContext(), false, false);

    // Check that the in-memory status has been appropriately updated.
    assertCriticalSectionCatchUpEnteredInMemory(dbName);

    //////////////////////////////////////////////
    // Block read/write operations (commit phase)
    //////////////////////////////////////////////

    // Update the document in the `config.collection_critical_sections` collection to simulate
    // entering the commit phase of the critical section for the database, then react to an
    // hypothetical availability of initial data.
    {
        AutoGetDb db(operationContext(), dbName.dbName(), MODE_IX);
        writeReadCriticalSectionDocument(dbName, dbOpReason, true /* blockReads */);
    }
    ShardingRecoveryService::get(operationContext())
        ->onConsistentDataAvailable(operationContext(), false, false);

    // Check that the in-memory status has been appropriately updated.
    assertCriticalSectionCommitEnteredInMemory(dbName);

    /////////////////////////////////
    // Unblock read/write operations
    /////////////////////////////////

    // Delete the document in the `config.collection_critical_sections` collection to simulate
    // leaving the critical section for the database, then react to an hypothetical availability of
    // initial data.
    {
        AutoGetDb db(operationContext(), dbName.dbName(), MODE_IX);
        deleteReadCriticalSectionDocument(dbName, dbOpReason);
    }
    ShardingRecoveryService::get(operationContext())
        ->onConsistentDataAvailable(operationContext(), false, false);

    // Check that the in-memory status has been appropriately updated.
    assertCriticalSectionLeftInMemory(dbName);
}

TEST_F(ShardingRecoveryServiceTestonInitialData, BlockAndUnblockOperationsOnCollection) {
    ///////////////////////////////////////////
    // Block write operations (catch-up phase)
    ///////////////////////////////////////////

    // Insert a document in the `config.collection_critical_sections` collection to simulate
    // entering the catch-up phase of the critical section for a collection, then react to an
    // hypothetical  availability of initial data.
    {
        AutoGetCollection coll(operationContext(), collNss, MODE_IX);
        writeReadCriticalSectionDocument(collNss, collOpReason, false /* blockReads */);
    }
    ShardingRecoveryService::get(operationContext())
        ->onConsistentDataAvailable(operationContext(), false, false);

    // Check that the in-memory status has been appropriately updated.
    assertCriticalSectionCatchUpEnteredInMemory(collNss);

    //////////////////////////////////////////////
    // Block read/write operations (commit phase)
    //////////////////////////////////////////////

    // Update the document in the `config.collection_critical_sections` collection to simulate
    // entering the commit phase of the critical section for the collection, then react to an
    // hypothetical  availability of initial data.
    {
        AutoGetCollection coll(operationContext(), collNss, MODE_IX);
        writeReadCriticalSectionDocument(collNss, collOpReason, true /* blockReads */);
    }
    ShardingRecoveryService::get(operationContext())
        ->onConsistentDataAvailable(operationContext(), false, false);

    // Check that the in-memory status has been appropriately updated.
    assertCriticalSectionCommitEnteredInMemory(collNss);

    /////////////////////////////////
    // Unblock read/write operations
    /////////////////////////////////

    // Delete the document in the `config.collection_critical_sections` collection to simulate
    // leaving the critical section for the collection, then react to an hypothetical  availability
    // of initial data.
    {
        AutoGetCollection coll(operationContext(), collNss, MODE_IX);
        deleteReadCriticalSectionDocument(collNss, collOpReason);
    }
    ShardingRecoveryService::get(operationContext())
        ->onConsistentDataAvailable(operationContext(), false, false);

    // Check that the in-memory status has been appropriately updated.
    assertCriticalSectionLeftInMemory(collNss);
}

////////////////////////////////////////////////////////////////////////////////////////////////////

TEST_F(ShardingRecoveryServiceTestAfterRollback, BlockAndUnblockOperationsOnDatabase) {
    ///////////////////////////////////////////
    // Block write operations (catch-up phase)
    ///////////////////////////////////////////

    // Insert a document in the `config.collection_critical_sections` collection to simulate
    // entering the catch-up phase of the critical section for a database, then react to an
    // hypothetical replication rollback.
    {
        AutoGetDb db(operationContext(), dbName.dbName(), MODE_IX);
        writeReadCriticalSectionDocument(dbName, dbOpReason, false /* blockReads */);
    }
    ShardingRecoveryService::get(operationContext())
        ->onReplicationRollback(
            operationContext(), {NamespaceString::kCollectionCriticalSectionsNamespace}, {});

    // Check that the in-memory status has been appropriately updated.
    assertCriticalSectionCatchUpEnteredInMemory(dbName);

    //////////////////////////////////////////////
    // Block read/write operations (commit phase)
    //////////////////////////////////////////////

    // Update the document in the `config.collection_critical_sections` collection to simulate
    // entering the commit phase of the critical section for the database, then react to an
    // hypothetical replication rollback.
    {
        AutoGetDb db(operationContext(), dbName.dbName(), MODE_IX);
        writeReadCriticalSectionDocument(dbName, dbOpReason, true /* blockReads */);
    }
    ShardingRecoveryService::get(operationContext())
        ->onReplicationRollback(
            operationContext(), {NamespaceString::kCollectionCriticalSectionsNamespace}, {});

    // Check that the in-memory status has been appropriately updated.
    assertCriticalSectionCommitEnteredInMemory(dbName);

    /////////////////////////////////
    // Unblock read/write operations
    /////////////////////////////////

    // Delete the document in the `config.collection_critical_sections` collection to simulate
    // leaving the critical section for the database, then react to an hypothetical replication
    // rollback.
    {
        AutoGetDb db(operationContext(), dbName.dbName(), MODE_IX);
        deleteReadCriticalSectionDocument(dbName, dbOpReason);
    }
    ShardingRecoveryService::get(operationContext())
        ->onReplicationRollback(
            operationContext(), {NamespaceString::kCollectionCriticalSectionsNamespace}, {});

    // Check that the in-memory status has been appropriately updated.
    assertCriticalSectionLeftInMemory(dbName);
}

TEST_F(ShardingRecoveryServiceTestAfterRollback, BlockAndUnblockOperationsOnCollection) {
    ///////////////////////////////////////////
    // Block write operations (catch-up phase)
    ///////////////////////////////////////////

    // Insert a document in the `config.collection_critical_sections` collection to simulate
    // entering the catch-up phase of the critical section for a collection, then react to an
    // hypothetical replication rollback.
    {
        AutoGetCollection coll(operationContext(), collNss, MODE_IX);
        writeReadCriticalSectionDocument(collNss, collOpReason, false /* blockReads */);
    }
    ShardingRecoveryService::get(operationContext())
        ->onReplicationRollback(
            operationContext(), {NamespaceString::kCollectionCriticalSectionsNamespace}, {});

    // Check that the in-memory status has been appropriately updated.
    assertCriticalSectionCatchUpEnteredInMemory(collNss);

    //////////////////////////////////////////////
    // Block read/write operations (commit phase)
    //////////////////////////////////////////////

    // Update the document in the `config.collection_critical_sections` collection to simulate
    // entering the commit phase of the critical section for the collection, then react to an
    // hypothetical replication rollback.
    {
        AutoGetCollection coll(operationContext(), collNss, MODE_IX);
        writeReadCriticalSectionDocument(collNss, collOpReason, true /* blockReads */);
    }
    ShardingRecoveryService::get(operationContext())
        ->onReplicationRollback(
            operationContext(), {NamespaceString::kCollectionCriticalSectionsNamespace}, {});

    // Check that the in-memory status has been appropriately updated.
    assertCriticalSectionCommitEnteredInMemory(collNss);

    /////////////////////////////////
    // Unblock read/write operations
    /////////////////////////////////

    // Delete the document in the `config.collection_critical_sections` collection to simulate
    // leaving the critical section for the collection, then react to an hypothetical replication
    // rollback.
    {
        AutoGetCollection coll(operationContext(), collNss, MODE_IX);
        deleteReadCriticalSectionDocument(collNss, collOpReason);
    }
    ShardingRecoveryService::get(operationContext())
        ->onReplicationRollback(
            operationContext(), {NamespaceString::kCollectionCriticalSectionsNamespace}, {});

    // Check that the in-memory status has been appropriately updated.
    assertCriticalSectionLeftInMemory(collNss);
}

// Installs filtering metadata and allowChunkOperations flag on a couple of collections. Both CSR
// and DSR are backed by durable shard-catalog collections. Then simulates a replication rollback
// carrying the given 'rollbackNamespaces' and 'rollbackCommandCounts' and asserts the resulting
// in-memory state.
//
// The DDL protocol first writes to durable shard-catalog, then updates the in-memory state
// through oplog c entries. We simulate the rollback between the shard-catalog write and the c
// entry. The rollback reverses the oplog c entry.
// Hence, in-memory DSR version and allowChunkOperations flag are seeded to differ from the disk
// (with a stale version). Post-rollback this helps distinguish a genuine rebuild from an untouched
// state.
//
// The post-rollback state is governed by two independent booleans:
//   - 'resetCsr': whether the CSR filtering metadata was cleared. Recovery resets every CSR or
//   none.
//   - 'recoveryTriggered': whether recovery actually ran, which governs whether the DSR and the
//     allowChunkOperations flag were rebuilt from disk.
//
// 'resetCsr' can only be true when 'recoveryTriggered' is true; the converse need not hold. (e.g.
// when the CSR reset is disabled by a server parameter).
void clearFilteringMetadataOnRecoveryTestHelper(OperationContext* opCtx,
                                                const std::set<NamespaceString>& rollbackNamespaces,
                                                const StringMap<long long>& rollbackCommandCounts,
                                                bool resetCsr,
                                                bool recoveryTriggered) {
    // Two collections in a single database, each fully backed by durable shard-catalog collections.
    // The installed collection namespace are arbitrary, but must be different to assert that every
    // CSR is reset or none.
    const auto nssX = NamespaceString::createNamespaceString_forTest("RecoverDB", "CollX");
    const auto nssY = NamespaceString::createNamespaceString_forTest("RecoverDB", "CollY");
    const auto dbName = nssX.dbName();
    const auto shardId = ShardingState::get(opCtx)->shardId();

    // Stale in-memory version vs newer on-disk version.
    const DatabaseVersion staleVersion{UUID::gen(), Timestamp(10, 1)};
    const DatabaseVersion diskVersion{UUID::gen(), Timestamp(20, 1)};

    // Back both collections with durable shard-catalog.
    {
        DBDirectClient client(opCtx);

        // config.shard.catalog.databases holds the newer version.
        client.insert(NamespaceString::kConfigShardCatalogDatabasesNamespace,
                      DatabaseType{dbName, shardId, diskVersion}.toBSON());

        for (const auto& nss : {nssX, nssY}) {
            const auto collUuid = UUID::gen();
            const OID epoch = OID::gen();
            const Timestamp collTimestamp(Date_t::now());

            // config.shard.catalog.collections: allowChunkOperations=false.
            CollectionType diskColl{
                nss, epoch, collTimestamp, Date_t::now(), collUuid, kShardKeyPattern};
            diskColl.setAllowChunkOperations(false);
            client.insert(NamespaceString::kConfigShardCatalogCollectionsNamespace,
                          diskColl.toBSON());
        }
    }

    // Install in-memory filtering metadata and allowChunkOperations=true on both collections so a
    // rebuild from the disk value (false) is observable.
    for (const auto& nss : {nssX, nssY}) {
        auto scopedCsr = CollectionShardingRuntime::acquireExclusive(opCtx, nss);
        scopedCsr->setCollectionMetadata(opCtx, makeShardedMetadata(nss));
        scopedCsr->setAllowChunkOperations(true);
    }

    // Install a stale in-memory DSR version for the database.
    {
        BypassDatabaseMetadataAccess bypass(
            opCtx, BypassDatabaseMetadataAccess::Type::kWriteOnly);  // NOLINT
        auto scopedDsr = DatabaseShardingRuntime::acquireExclusive(opCtx, dbName);
        scopedDsr->setDbMetadata(opCtx, DatabaseType{dbName, shardId, staleVersion});
    }

    // Simulate a replication rollback carrying the given namespaces and command counts.
    ShardingRecoveryService::get(opCtx)->onReplicationRollback(
        opCtx, rollbackNamespaces, rollbackCommandCounts);

    // Recovery resets every CSR or none; assert the same filtering-metadata outcome for both.
    for (const auto& nss : {nssX, nssY}) {
        auto csr = CollectionShardingRuntime::acquireShared(opCtx, nss);
        const auto metadata = csr->getCurrentMetadataIfKnown();
        ASSERT((resetCsr && !metadata) || (!resetCsr && metadata && metadata->isSharded()));
    }

    // The DSR is rebuilt from disk iff recovery ran: its version matches the newer on-disk version
    // when recovery ran, and stays at the stale in-memory version otherwise.
    {
        BypassDatabaseMetadataAccess bypass(
            opCtx, BypassDatabaseMetadataAccess::Type::kReadOnly);  // NOLINT
        auto scopedDsr = DatabaseShardingRuntime::acquireShared(opCtx, dbName);
        const auto version = scopedDsr->getDbVersion(opCtx);
        ASSERT_TRUE(version);
        ASSERT_EQ(*version, recoveryTriggered ? diskVersion : staleVersion);
        ASSERT_EQ(scopedDsr->getDbPrimaryShard(opCtx), boost::optional<ShardId>(shardId));
    }

    // The allowChunkOperations flag is rebuilt from disk (false) iff recovery ran; otherwise it
    // keeps its in-memory value (true). Asserted for both collections.
    for (const auto& nss : {nssX, nssY}) {
        auto scopedCsr = CollectionShardingRuntime::acquireShared(opCtx, nss);
        ASSERT_EQ(scopedCsr->allowChunkOperations(), !recoveryTriggered);
    }
}

TEST_F(ShardingRecoveryServiceTestAfterRollback,
       RecoveryRollbackClearsShardingStateForInvalidateCollectionMetadata) {
    // A rolled-back invalidateCollectionMetadata 'c' entry triggers recovery and resets all CSRs.
    clearFilteringMetadataOnRecoveryTestHelper(operationContext(),
                                               {} /* rollbackNamespaces */,
                                               {{"invalidateCollectionMetadata", 1}},
                                               true /* resetCsr */,
                                               true /* recoveryTriggered */);
}

TEST_F(ShardingRecoveryServiceTestAfterRollback,
       RecoveryRollbackClearsShardingStateForUpdateCollectionMetadata) {
    // A rolled-back updateCollectionMetadata 'c' entry triggers recovery and resets all CSRs.
    clearFilteringMetadataOnRecoveryTestHelper(operationContext(),
                                               {} /* rollbackNamespaces */,
                                               {{"updateCollectionMetadata", 1}},
                                               true /* resetCsr */,
                                               true /* recoveryTriggered */);
}

TEST_F(ShardingRecoveryServiceTestAfterRollback,
       RecoveryRollbackClearsShardingStateForCreateDatabaseMetadata) {
    // A rolled-back createDatabaseMetadata 'c' entry triggers recovery and resets all CSRs.
    clearFilteringMetadataOnRecoveryTestHelper(operationContext(),
                                               {} /* rollbackNamespaces */,
                                               {{"createDatabaseMetadata", 1}},
                                               true /* resetCsr */,
                                               true /* recoveryTriggered */);
}

TEST_F(ShardingRecoveryServiceTestAfterRollback,
       RecoveryRollbackClearsShardingStateForDropDatabaseMetadata) {
    // A rolled-back dropDatabaseMetadata 'c' entry triggers recovery and resets all CSRs.
    clearFilteringMetadataOnRecoveryTestHelper(operationContext(),
                                               {} /* rollbackNamespaces */,
                                               {{"dropDatabaseMetadata", 1}},
                                               true /* resetCsr */,
                                               true /* recoveryTriggered */);
}

TEST_F(ShardingRecoveryServiceTestAfterRollback,
       RecoveryRollbackClearsShardingStateForSetAllowChunkOperations) {
    // A rolled-back setAllowChunkOperations 'c' entry triggers recovery and resets all CSRs.
    clearFilteringMetadataOnRecoveryTestHelper(operationContext(),
                                               {} /* rollbackNamespaces */,
                                               {{"setAllowChunkOperations", 1}},
                                               true /* resetCsr */,
                                               true /* recoveryTriggered */);
}

TEST_F(ShardingRecoveryServiceTestAfterRollback,
       RecoveryDoesNotResetCollectionShardingRuntimesWhenParameterDisabled) {
    // Disable the CSR reset during recovery.
    unittest::ServerParameterGuard guard("clearCollectionShardingRuntimesOnRecovery", false);

    // Even though invalidateCollectionMetadata was rolled back, the server parameter overrides the
    // behavior and the CSRs are not reset. Recovery still runs, so the DSR and allowChunkOperations
    // flag are rebuilt from disk.
    clearFilteringMetadataOnRecoveryTestHelper(operationContext(),
                                               {} /* rollbackNamespaces */,
                                               {{"invalidateCollectionMetadata", 1}},
                                               false /* resetCsr */,
                                               true /* recoveryTriggered */);
}

TEST_F(ShardingRecoveryServiceTestAfterRollback,
       RecoveryDoesNotResetShardingStateWhenCommandCountIsZero) {
    // A command count of zero must not trigger recovery.
    clearFilteringMetadataOnRecoveryTestHelper(operationContext(),
                                               {} /* rollbackNamespaces */,
                                               {{"invalidateCollectionMetadata", 0}},
                                               false /* resetCsr */,
                                               false /* recoveryTriggered */);
}

TEST_F(ShardingRecoveryServiceTestAfterRollback,
       RecoveryDoesNotResetShardingStateForUnrelatedCommands) {
    // Synthetic CRUD keys are not CSR-maintenance commands and must not trigger recovery.
    clearFilteringMetadataOnRecoveryTestHelper(operationContext(),
                                               {} /* rollbackNamespaces */,
                                               {{"insert", 3}, {"update", 2}},
                                               false /* resetCsr */,
                                               false /* recoveryTriggered */);
}

TEST_F(ShardingRecoveryServiceTestAfterRollback,
       RecoveryDoesNotResetShardingStateForUserCollectionNamespace) {
    // A rolled-back user-collection namespace is not a trigger namespace, so on its own it resets
    // nothing.
    const auto nssA = NamespaceString::createNamespaceString_forTest("TestDB", "CollA");
    clearFilteringMetadataOnRecoveryTestHelper(operationContext(),
                                               {nssA},
                                               {} /* rollbackCommandCounts */,
                                               false /* resetCsr */,
                                               false /* recoveryTriggered */);
}

TEST_F(ShardingRecoveryServiceTestAfterRollback,
       RecoveryResetsShardingStateForCommandDespiteUserCollectionNamespace) {
    // The user-collection namespace is inert; the rolled-back invalidateCollectionMetadata 'c'
    // entry is what triggers the (global) reset of all CSRs.
    const auto nssA = NamespaceString::createNamespaceString_forTest("TestDB", "CollA");
    clearFilteringMetadataOnRecoveryTestHelper(operationContext(),
                                               {nssA},
                                               {{"invalidateCollectionMetadata", 1}},
                                               true /* resetCsr */,
                                               true /* recoveryTriggered */);
}

TEST_F(ShardingRecoveryServiceTestAfterRollback, RecoveryResetsShardingStateForTriggerNamespace) {
    // A rolled-back trigger namespace resets all CSRs even without any command counts.
    clearFilteringMetadataOnRecoveryTestHelper(
        operationContext(),
        {NamespaceString::kConfigShardCatalogCollectionsNamespace},
        {} /* rollbackCommandCounts */,
        true /* resetCsr */,
        true /* recoveryTriggered */);
}

TEST_F(ShardingRecoveryServiceTestOnPrimary, CallbackInvokedWhenCollLockContended) {
    // Create a separate client+opCtx to hold a conflicting MODE_IX collection lock.
    auto lockClient = getServiceContext()->getService()->makeClient("lockHolder");
    auto lockOpCtx = lockClient->makeOperationContext();

    auto* locker = shard_role_details::getLocker(lockOpCtx.get());
    const auto dbResId = ResourceId(RESOURCE_DATABASE, collNss.dbName());
    const auto collResId = ResourceId(RESOURCE_COLLECTION, collNss);

    locker->lockGlobal(lockOpCtx.get(), MODE_IX);
    locker->lock(lockOpCtx.get(), dbResId, MODE_IX);
    locker->lock(lockOpCtx.get(), collResId, MODE_IX);

    // Synchronization: the callback will signal the background thread to release the lock.
    std::mutex mu;
    stdx::condition_variable cv;
    bool callbackInvoked = false;
    bool releaseLock = false;

    stdx::thread lockReleaser([&] {
        std::unique_lock<std::mutex> lk(mu);
        cv.wait(lk, [&] { return releaseLock; });
        locker->unlock(collResId);
        locker->unlock(dbResId);
        locker->unlockGlobal();
    });

    // The callback runs after lockBegin (lock is enqueued) but before lockComplete.
    // It signals the background thread to release the conflicting lock so that lockComplete
    // can succeed.
    ShardingRecoveryService::get(operationContext())
        ->acquireRecoverableCriticalSectionBlockWrites(
            operationContext(),
            collNss,
            collOpReason,
            ShardingCatalogClient::writeConcernLocalHavingUpstreamWaiter(),
            true /* clearShardCatalogCache */,
            boost::none /* lockAcquisitionTimeout */,
            [&](OperationContext*) {
                std::lock_guard<std::mutex> lk(mu);
                callbackInvoked = true;
                releaseLock = true;
                cv.notify_one();
            });

    lockReleaser.join();

    // Verify the callback was invoked because the lock was contended.
    ASSERT(callbackInvoked);

    // Verify the critical section was acquired successfully.
    assertCriticalSectionCatchUpEnteredInMemory(collNss);
    assertCriticalSectionCatchUpEnteredOnDisk(collNss, collOpReason);

    // Clean up by releasing the critical section.
    ShardingRecoveryService::get(operationContext())
        ->releaseRecoverableCriticalSection(
            operationContext(),
            collNss,
            collOpReason,
            ShardingCatalogClient::writeConcernLocalHavingUpstreamWaiter(),
            ShardingRecoveryService::NoCustomAction());

    assertCriticalSectionLeftInMemory(collNss);
    assertCriticalSectionLeftOnDisk(collNss, collOpReason);
}

TEST_F(ShardingRecoveryServiceTestOnPrimary, CallbackNotInvokedWhenCollLockNotContended) {
    // When there is no conflicting lock, the callback should NOT be invoked.
    bool callbackInvoked = false;

    ShardingRecoveryService::get(operationContext())
        ->acquireRecoverableCriticalSectionBlockWrites(
            operationContext(),
            collNss,
            collOpReason,
            ShardingCatalogClient::writeConcernLocalHavingUpstreamWaiter(),
            true /* clearShardCatalogCache */,
            boost::none /* lockAcquisitionTimeout */,
            [&](OperationContext*) { callbackInvoked = true; });

    // The callback is only invoked on the LOCK_WAITING path.
    ASSERT(!callbackInvoked);

    // Verify the critical section was acquired successfully.
    assertCriticalSectionCatchUpEnteredInMemory(collNss);
    assertCriticalSectionCatchUpEnteredOnDisk(collNss, collOpReason);

    // Clean up.
    ShardingRecoveryService::get(operationContext())
        ->releaseRecoverableCriticalSection(
            operationContext(),
            collNss,
            collOpReason,
            ShardingCatalogClient::writeConcernLocalHavingUpstreamWaiter(),
            ShardingRecoveryService::NoCustomAction());

    assertCriticalSectionLeftInMemory(collNss);
    assertCriticalSectionLeftOnDisk(collNss, collOpReason);
}

TEST_F(ShardingRecoveryServiceTestOnPrimary,
       CriticalSectionSucceedsWhenCallbackKillsUnpreparedTransactions) {
    const auto collResId = ResourceId(RESOURCE_COLLECTION, collNss);

    // Transaction 1: registers the namespace as affected.
    TransactionHandle txnHandle1(getServiceContext(), collNss);
    txnHandle1.createTransaction();

    // Transaction 2: does NOT register the namespace, but still holds MODE_IX on collNss.
    TransactionHandle txnHandle2(getServiceContext(), collNss);
    txnHandle2.createTransaction(/*prepared=*/false, /*registerNss=*/false);

    // Acquire critical section with a callback that kills unprepared transactions by finding
    // conflicting lock holders.
    ShardingRecoveryService::get(operationContext())
        ->acquireRecoverableCriticalSectionBlockWrites(
            operationContext(),
            collNss,
            collOpReason,
            ShardingCatalogClient::writeConcernLocalHavingUpstreamWaiter(),
            true /* clearShardCatalogCache */,
            boost::none /* lockAcquisitionTimeout */,
            [&](OperationContext* opCtx) {
                auto* cbLocker = shard_role_details::getLocker(opCtx);
                auto conflictingLockerIds = cbLocker->getConflictingLockerIds(collResId, MODE_S);
                // Release stashed locks before killing so the Locker destructor
                // (triggered by abortTransaction destroying TxnResources) succeeds.
                txnHandle1.releaseStashedLocks();
                txnHandle2.releaseStashedLocks();
                killSessionsAbortUnpreparedTransactionsForLockerIds(
                    opCtx, conflictingLockerIds, ErrorCodes::Interrupted);
            });

    // Verify the critical section was acquired successfully.
    assertCriticalSectionCatchUpEnteredInMemory(collNss);
    assertCriticalSectionCatchUpEnteredOnDisk(collNss, collOpReason);

    // Verify both unprepared transactions were aborted.
    ASSERT_FALSE(txnHandle1.isInProgress());
    ASSERT_FALSE(txnHandle2.isInProgress());

    // Clean up by releasing the critical section.
    ShardingRecoveryService::get(operationContext())
        ->releaseRecoverableCriticalSection(
            operationContext(),
            collNss,
            collOpReason,
            ShardingCatalogClient::writeConcernLocalHavingUpstreamWaiter(),
            ShardingRecoveryService::NoCustomAction());

    assertCriticalSectionLeftInMemory(collNss);
    assertCriticalSectionLeftOnDisk(collNss, collOpReason);
}

TEST_F(ShardingRecoveryServiceTestOnPrimary,
       CriticalSectionTimesOutWhenPreparedTransactionHoldsLock) {
    const auto collResId = ResourceId(RESOURCE_COLLECTION, collNss);

    // Session 1: unprepared transaction that holds MODE_IX on collNss.
    TransactionHandle txnHandle1(getServiceContext(), collNss);
    txnHandle1.createTransaction();

    // Session 2: prepared transaction that holds MODE_IX on collNss.
    TransactionHandle txnHandle2(getServiceContext(), collNss);
    txnHandle2.createTransaction(/*prepared=*/true);

    // Attempt to acquire the critical section with the kill callback and a short timeout.
    // The callback kills the unprepared transaction (session 1) but cannot kill the prepared
    // transaction (session 2). Since session 2's lock is never released, lockComplete times out.
    ASSERT_THROWS_CODE(ShardingRecoveryService::get(operationContext())
                           ->acquireRecoverableCriticalSectionBlockWrites(
                               operationContext(),
                               collNss,
                               collOpReason,
                               ShardingCatalogClient::writeConcernLocalHavingUpstreamWaiter(),
                               true /* clearShardCatalogCache */,
                               Milliseconds(500) /* lockAcquisitionTimeout */,
                               [&](OperationContext* opCtx) {
                                   auto* cbLocker = shard_role_details::getLocker(opCtx);
                                   auto conflictingLockerIds =
                                       cbLocker->getConflictingLockerIds(collResId, MODE_S);
                                   // Release session 1's stashed locks before killing so the
                                   // Locker destructor succeeds when abortTransaction
                                   // destroys TxnResources.
                                   txnHandle1.releaseStashedLocks();
                                   killSessionsAbortUnpreparedTransactionsForLockerIds(
                                       opCtx, conflictingLockerIds, ErrorCodes::Interrupted);
                               }),
                       DBException,
                       ErrorCodes::LockTimeout);

    // Verify session 1 (unprepared) was aborted by the callback.
    ASSERT_FALSE(txnHandle1.isInProgress());

    // Verify session 2 (prepared) is still prepared — it was not killed.
    // Release its locks first so the session can be checked out.
    txnHandle2.releaseStashedLocks();
    ASSERT_TRUE(txnHandle2.isPrepared());
}

TEST_F(ShardingRecoveryServiceTestOnPrimary,
       CriticalSectionBlocksNewTransactionsAndUnblocksAfterRelease) {
    const auto dbResId = ResourceId(RESOURCE_DATABASE, collNss.dbName());
    const auto collResId = ResourceId(RESOURCE_COLLECTION, collNss);

    // Create an unprepared transaction holding MODE_IX on collNss.
    TransactionHandle lockTxnHandle(getServiceContext(), collNss);
    lockTxnHandle.createTransaction();

    // Enable failpoint to pause inside the callback after lockBegin (MODE_S enqueued but waiting).
    auto* fp = globalFailPointRegistry().find("pauseShardingRecoveryServiceAfterLockBegin");
    auto initialTimesEntered = fp->setMode(FailPoint::alwaysOn);

    // csThread: acquires the critical section with a kill callback.
    stdx::thread csThread([&] {
        auto csClient = getServiceContext()->getService()->makeClient("csThread");
        auto csOpCtx = csClient->makeOperationContext();
        AlternativeClientRegion acr(csClient);
        ShardingRecoveryService::get(csOpCtx.get())
            ->acquireRecoverableCriticalSectionBlockWrites(
                csOpCtx.get(),
                collNss,
                collOpReason,
                ShardingCatalogClient::writeConcernLocalHavingUpstreamWaiter(),
                true /* clearShardCatalogCache */,
                boost::none /* lockAcquisitionTimeout */,
                [&](OperationContext* opCtx) {
                    // Attempt to kill sessions for conflicting lockers.
                    auto* cbLocker = shard_role_details::getLocker(opCtx);
                    auto conflictingLockerIds =
                        cbLocker->getConflictingLockerIds(collResId, MODE_S);
                    // Release stashed locks before killing so the Locker destructor
                    // succeeds, and so lockComplete can grant MODE_S.
                    lockTxnHandle.releaseStashedLocks();
                    killSessionsAbortUnpreparedTransactionsForLockerIds(
                        opCtx, conflictingLockerIds, ErrorCodes::Interrupted);
                });
    });

    // Wait for the failpoint to be hit — confirms MODE_S is enqueued in the conflict queue.
    fp->waitForTimesEntered(initialTimesEntered + 1);

    // Start a new transaction thread that tries to acquire MODE_IX on collNss.
    // Since Critical section's MODE_S is in the conflict queue, new MODE_IX requests will wait.
    Atomic<bool> newTxnLockAcquired{false};
    std::mutex newTxnMu;
    stdx::condition_variable newTxnCv;
    bool newTxnEnqueued = false;

    auto newTxnClient = getServiceContext()->getService()->makeClient("newTxnThread");
    auto newTxnOpCtx = newTxnClient->makeOperationContext();
    auto* newTxnLocker = shard_role_details::getLocker(newTxnOpCtx.get());

    stdx::thread newTxnThread([&] {
        newTxnLocker->lockGlobal(newTxnOpCtx.get(), MODE_IX);
        newTxnLocker->lock(newTxnOpCtx.get(), dbResId, MODE_IX);
        // lockBeginForTest returns immediately with LOCK_WAITING since MODE_IX conflicts
        // with MODE_S in the conflict queue.
        auto result = newTxnLocker->lockBeginForTest(newTxnOpCtx.get(), collResId, MODE_IX);
        ASSERT_EQ(result, LOCK_WAITING);

        // Signal that the new transaction's lock request is enqueued.
        {
            std::lock_guard<std::mutex> lk(newTxnMu);
            newTxnEnqueued = true;
            newTxnCv.notify_one();
        }

        // Block here until the critical section's MODE_S is granted and then released
        // (the MODE_S lock scope ends when acquireRecoverableCriticalSectionBlockWrites
        // returns, which unblocks this MODE_IX request).
        newTxnLocker->lockCompleteForTest(newTxnOpCtx.get(), collResId, MODE_IX, Date_t::max());
        newTxnLockAcquired.store(true);
    });

    // Wait until the new transaction's lock request is enqueued behind the CS's MODE_S.
    {
        std::unique_lock<std::mutex> lk(newTxnMu);
        newTxnCv.wait(lk, [&] { return newTxnEnqueued; });
    }

    // The new transaction is blocked — queued behind MODE_S in the conflict queue.
    ASSERT_FALSE(newTxnLockAcquired.load());

    // Disable failpoint: callback resumes, releases the conflicting MODE_IX lock,
    // lockComplete grants MODE_S → CS function completes → MODE_S released (RAII) →
    // new transaction's MODE_IX is granted.
    fp->setMode(FailPoint::off);
    csThread.join();

    // Verify critical section was acquired.
    assertCriticalSectionCatchUpEnteredInMemory(collNss);
    assertCriticalSectionCatchUpEnteredOnDisk(collNss, collOpReason);

    // The new transaction's MODE_IX should now be granted — the CS's MODE_S lock was released
    // when acquireRecoverableCriticalSectionBlockWrites returned (RAII scope).
    newTxnThread.join();
    ASSERT_TRUE(newTxnLockAcquired.load());

    // Clean up: release the new transaction's locks and the critical section.
    newTxnLocker->unlock(collResId);
    newTxnLocker->unlock(dbResId);
    newTxnLocker->unlockGlobal();

    ShardingRecoveryService::get(operationContext())
        ->releaseRecoverableCriticalSection(
            operationContext(),
            collNss,
            collOpReason,
            ShardingCatalogClient::writeConcernLocalHavingUpstreamWaiter(),
            ShardingRecoveryService::NoCustomAction());

    assertCriticalSectionLeftInMemory(collNss);
    assertCriticalSectionLeftOnDisk(collNss, collOpReason);
}

}  // namespace
}  // namespace mongo
